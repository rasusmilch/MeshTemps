// MeshTemps-BridgeNode.ino
//
// Headless bridge for the MeshTemps network. Runs the root mesh + WiFi duties
// from the original MeshTemps-RootNode sketch, but skips all LVGL rendering and
// forwards leaf payloads to the dedicated GUI node over UART.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <painlessMesh.h>
#include <algorithm>
#include <map>
#include <stdarg.h>

#include "Config.h"

// Bridge <-> GUI uses a dedicated UART on GPIO17/18 so we leave UART0 on the
// CH434 for flashing/console access.
#ifndef BRIDGE_GUI_TX_PIN
#define BRIDGE_GUI_TX_PIN 17
#endif
#ifndef BRIDGE_GUI_RX_PIN
#define BRIDGE_GUI_RX_PIN 18
#endif

#include "serial_console.h"
#include "../serial_protocol.h"

// -----------------------------------------------------------------------------
// Mesh + WiFi state (copied from MeshTemps-RootNode)
// -----------------------------------------------------------------------------

Scheduler user_scheduler;
painlessMesh mesh;
Task g_task_announce;

int32_t g_mesh_channel = MESH_CHANNEL;

// WiFi credentials persisted by the mesh root.
struct NetworkConfig {
  String ssid;
  String password;
  // Timezone offset in minutes from UTC, e.g. -360 = UTC-6 (CST).
  int32_t timezone_minutes = 0;
  bool dst_enabled = false;  // when true, add +60 minutes
};

NetworkConfig g_network_config;
static String g_last_applied_ssid;
static String g_last_applied_password;

// NTP timing.
constexpr uint32_t kNtpSyncTimeoutMs = 15000;     // 15 s to wait for time
constexpr uint32_t kNtpResyncPeriodMs = 24UL * 60UL * 60UL * 1000UL;  // 24 hours
constexpr uint32_t kNtpRetryInitialMs = 5UL * 60UL * 1000UL;          // 5 min
constexpr uint32_t kNtpRetryMaxMs = 12UL * 60UL * 60UL * 1000UL;      // 12 h

static bool g_ntp_time_valid = false;
static uint32_t g_last_ntp_attempt_ms = 0;
static uint32_t g_last_ntp_ok_ms = 0;
static uint32_t g_ntp_retry_period_ms = kNtpRetryInitialMs;
static bool g_ntp_sync_in_progress = false;
static bool g_ntp_cb_pending = false;
static time_t g_ntp_last_sync_epoch = 0;
static bool g_wifi_connected = false;

HardwareSerial &gui_serial = Serial1;
static SerialConsole g_console;
static bool g_gui_passthrough = false;
static bool g_gui_passthrough_escape = false;
static String g_gui_passthrough_tx_line;
static String g_gui_passthrough_rx_line;
static bool g_debug_verbose = false;
static std::map<uint32_t, uint32_t> g_node_last_seen_ms;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void PrintCommandHeader(Print &out, int argc, const String argv[]) {
  out.print(F("> "));
  for (int i = 0; i < argc; ++i) {
    if (i > 0) out.print(' ');
    out.print(argv[i]);
  }
  out.println();
}

static void DebugPrintln(const __FlashStringHelper *msg) {
  if (g_debug_verbose && msg != nullptr) {
    Serial.println(msg);
  }
}

static void DebugPrintf(const char *fmt, ...) {
  if (!g_debug_verbose || fmt == nullptr) return;
  va_list args;
  va_start(args, fmt);
  Serial.vprintf(fmt, args);
  va_end(args);
}

static void SendJsonLineWithEcho(HardwareSerial &port, const JsonDocument &doc) {
  String line;
  serializeJson(doc, line);
  port.println(line);

  if (g_gui_passthrough || g_debug_verbose) {
    Serial.print(F("[BRIDGE->GUI] "));
    Serial.println(line);
  }
}

// Extract everything after the 2nd token (command + subcommand) as-is,
// only stripping trailing CR/LF. This is used for WiFi SSID/password so
// all valid characters (spaces, quotes, etc.) are preserved.
static bool ExtractRawTailAfterSecondToken(const String &line, String *out_value) {
  if (out_value == nullptr) return false;

  int parts = 0;
  int pos = 0;
  while (parts < 2) {
    while (pos < static_cast<int>(line.length()) &&
           isspace(static_cast<unsigned char>(line[pos]))) {
      ++pos;
    }
    while (pos < static_cast<int>(line.length()) &&
           !isspace(static_cast<unsigned char>(line[pos]))) {
      ++pos;
    }
    ++parts;
  }
  while (pos < static_cast<int>(line.length()) &&
         isspace(static_cast<unsigned char>(line[pos]))) {
    ++pos;
  }
  if (pos >= static_cast<int>(line.length())) return false;
  *out_value = line.substring(pos);
  out_value->trim();
  return !out_value->isEmpty();
}

static void LoadNetworkConfigFromNVS() {
  Preferences prefs;
  if (!prefs.begin("wifi", true)) {
    return;
  }
  g_network_config.ssid = prefs.getString("ssid", String());
  g_network_config.password = prefs.getString("pwd", String());
  g_network_config.timezone_minutes = prefs.getInt("tz_min", 0);
  g_network_config.dst_enabled = prefs.getInt("tz_dst", 0) != 0;
  prefs.end();
}

static void SaveNetworkConfigToNVS() {
  Preferences prefs;
  if (!prefs.begin("wifi", false)) {
    return;
  }
  prefs.putString("ssid", g_network_config.ssid);
  prefs.putString("pwd", g_network_config.password);
  prefs.putInt("tz_min", g_network_config.timezone_minutes);
  prefs.putInt("tz_dst", g_network_config.dst_enabled ? 1 : 0);
  prefs.end();
}

static void RootPickMeshChannel() {
  // For now, just keep the configured channel. Hook for future STA-driven pick.
  g_mesh_channel = MESH_CHANNEL;
}

static void RootAnnounce() {
  JsonDocument doc;
  const uint32_t now_ms = millis();
  doc["type"] = "root";
  doc["rootId"] = mesh.getNodeId();
  doc["uptimeMs"] = static_cast<uint32_t>(now_ms);

  String message;
  serializeJson(doc, message);
  mesh.sendBroadcast(message);
  DebugPrintln(F("[MESH] broadcast root announce"));
}

static void SendBridgeHello() {
  JsonDocument doc;
  doc["type"] = "bridge_hello";
  doc["rootId"] = mesh.getNodeId();
  doc["uptimeMs"] = static_cast<uint32_t>(millis());
  doc["note"] = "mesh->gui headless bridge";
  SendJsonLineWithEcho(gui_serial, doc);
  DebugPrintln(F("[BRIDGE] sent bridge_hello"));
}

static void SendBridgeStatus(const char *reason = nullptr) {
  JsonDocument doc;
  doc["type"] = "bridge_status";
  doc["rootId"] = mesh.getNodeId();
  const uint32_t now_ms = millis();
  doc["uptimeMs"] = static_cast<uint32_t>(now_ms);
  auto node_list = mesh.getNodeList();
  doc["connections"] = node_list.size();
  JsonArray nodes = doc.createNestedArray("nodes");
  for (const auto &id : node_list) {
    JsonObject node = nodes.createNestedObject();
    node["id"] = id;
    auto it = g_node_last_seen_ms.find(id);
    if (it != g_node_last_seen_ms.end()) {
      node["lastSeenMs"] = static_cast<uint32_t>(now_ms - it->second);
    }
  }
  if (reason != nullptr) {
    doc["reason"] = reason;
  }
  SendJsonLineWithEcho(gui_serial, doc);
  DebugPrintf("[BRIDGE] status connections=%u\n",
              static_cast<unsigned>(node_list.size()));
}

static void SendTimeSyncToGui(const char *source) {
  JsonDocument doc;
  doc["type"] = "time_sync";
  doc["rootId"] = mesh.getNodeId();
  doc["epoch"] = static_cast<long>(g_ntp_last_sync_epoch);
  doc["tzMinutes"] = g_network_config.timezone_minutes;
  doc["dst"] = g_network_config.dst_enabled;
  doc["source"] = source;
  SendJsonLineWithEcho(gui_serial, doc);
  DebugPrintf("[TIME] forwarded time_sync source=%s epoch=%ld tzMin=%ld dst=%s\n",
              source, static_cast<long>(g_ntp_last_sync_epoch),
              static_cast<long>(g_network_config.timezone_minutes),
              g_network_config.dst_enabled ? "on" : "off");
}

static void ForwardMeshPayload(uint32_t from, const JsonDocument &payload) {
  JsonDocument envelope;
  envelope["type"] = "mesh_frame";
  envelope["from"] = from;
  envelope["rxMs"] = static_cast<uint32_t>(millis());
  envelope["payload"] = payload.as<JsonVariantConst>();
  SendJsonLineWithEcho(gui_serial, envelope);
  DebugPrintf("[MESH] fwd temps from=0x%08lX\n", static_cast<unsigned long>(from));
}

// Respond to a leaf's root_probe with a unicast root_ack.
static void HandleRootProbe(uint32_t from, const JsonDocument &probe_doc) {
  const uint32_t now_ms = millis();
  const uint32_t leaf_node_id = probe_doc["nodeId"] | from;

  JsonDocument reply;
  reply["type"] = "root_ack";
  reply["rootId"] = mesh.getNodeId();
  reply["uptimeMs"] = static_cast<uint32_t>(now_ms);
  reply["toNodeId"] = leaf_node_id;

  String out;
  serializeJson(reply, out);
  mesh.sendSingle(from, out);
  DebugPrintf("[MESH] root_probe ack to=0x%08lX\n",
              static_cast<unsigned long>(from));
}

static void ProcessGuiPassthrough() {
  if (!g_gui_passthrough) return;

  // Host PC -> GUI UART.
  while (Serial.available() > 0) {
    const char ch = Serial.read();

    if (g_gui_passthrough_escape) {
      g_gui_passthrough_escape = false;
      if (ch == '.') {
        g_gui_passthrough = false;
        g_gui_passthrough_tx_line = "";
        g_gui_passthrough_rx_line = "";
        Serial.println(F("[BRIDGE<->GUI] passthrough disabled"));
        return;
      }
      gui_serial.write('~');
      if (ch != '\r' && ch != '\n') {
        g_gui_passthrough_tx_line += '~';
      }
    }

    if (ch == '~') {
      g_gui_passthrough_escape = true;
      continue;
    }

    gui_serial.write(ch);

    if (ch == '\n') {
      if (!g_gui_passthrough_tx_line.isEmpty()) {
        Serial.print(F("[BRIDGE-PC->GUI] "));
        Serial.println(g_gui_passthrough_tx_line);
      }
      g_gui_passthrough_tx_line = "";
    } else if (ch != '\r') {
      g_gui_passthrough_tx_line += ch;
    }
  }

  // GUI UART -> Host PC.
  while (gui_serial.available() > 0) {
    const char ch = gui_serial.read();
    if (ch == '\n') {
      if (!g_gui_passthrough_rx_line.isEmpty()) {
        Serial.print(F("[GUI->BRIDGE-PC] "));
        Serial.println(g_gui_passthrough_rx_line);
      }
      g_gui_passthrough_rx_line = "";
    } else if (ch != '\r') {
      g_gui_passthrough_rx_line += ch;
    }
  }
}

void OnReceiveRoot(uint32_t from, String &msg) {
  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) {
    return;
  }

  const char *type = doc["type"] | "temps";
  DebugPrintf("[MESH RX] from=0x%08lX type=%s\n",
              static_cast<unsigned long>(from), type);
  g_node_last_seen_ms[from] = millis();
  if (strcmp(type, "root_probe") == 0) {
    HandleRootProbe(from, doc);
    return;
  }

  if (strcmp(type, "temps") == 0) {
    ForwardMeshPayload(from, doc);
  }
}

void OnConnectionsChangedRoot() { SendBridgeStatus("mesh_event"); }

// -----------------------------------------------------------------------------
// WiFi / NTP helpers
// -----------------------------------------------------------------------------

static void SetDefaultDateTime() {
  struct tm t = {};
  t.tm_year = 2025 - 1900;
  t.tm_mon = 0;
  t.tm_mday = 1;
  t.tm_hour = 0;
  t.tm_min = 0;
  t.tm_sec = 0;
  time_t epoch = mktime(&t);
  struct timeval now = {epoch, 0};
  settimeofday(&now, nullptr);
}

static void PrintCurrentLocalTime(Print &out) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) {
    out.println(F("time: (no valid RTC time)"));
    return;
  }
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  out.printf("time: %s\n", buf);
}

static bool EnsureMeshStationConfigApplied(Print &out, bool force_reapply = false) {
  if (g_network_config.ssid.isEmpty()) {
    out.println(F("[WiFi] SSID not configured; use 'wifi ssid ...' first"));
    return false;
  }
  const bool creds_changed = (g_network_config.ssid != g_last_applied_ssid) ||
                             (g_network_config.password != g_last_applied_password);
  if (force_reapply || creds_changed) {
    out.println(F("[WiFi] Applying STA credentials via painlessMesh"));
    mesh.stationManual(g_network_config.ssid.c_str(),
                       g_network_config.password.c_str());
    g_last_applied_ssid = g_network_config.ssid;
    g_last_applied_password = g_network_config.password;
  }
  return true;
}

static bool EnsureWifiConnected(Print &out) {
  if (!EnsureMeshStationConfigApplied(out)) {
    return false;
  }
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    out.printf("[WiFi] Connected; IP=%s RSSI=%d dBm\n", ip.toString().c_str(),
               WiFi.RSSI());
    return true;
  }
  out.printf(
      "[WiFi] STA credentials applied; current status=%d. Root will connect in the background.\n",
      static_cast<int>(status));
  return false;
}

static void NtpTimeSyncNotification(struct timeval *tv) {
  g_ntp_cb_pending = true;
  g_ntp_last_sync_epoch = (tv != nullptr) ? tv->tv_sec : time(nullptr);
}

static bool SyncTimeFromNtp(Print &out) {
  if (WiFi.status() != WL_CONNECTED) {
    out.println(F("[NTP] WiFi not connected; cannot sync time"));
    return false;
  }
  if (g_ntp_sync_in_progress) {
    out.println(F("[NTP] Sync already in progress"));
    return true;
  }
  const long gmt_offset_sec = static_cast<long>(g_network_config.timezone_minutes) * 60L;
  const long dst_offset_sec = g_network_config.dst_enabled ? 3600L : 0L;
  out.printf("[NTP] configTime offset=%ld sec dst=%ld sec\n", gmt_offset_sec,
             dst_offset_sec);
  sntp_set_time_sync_notification_cb(&NtpTimeSyncNotification);
  configTime(gmt_offset_sec, dst_offset_sec, "pool.ntp.org", "time.nist.gov");
  g_ntp_sync_in_progress = true;
  g_last_ntp_attempt_ms = millis();
  return true;
}

static void NtpLoop() {
  const uint32_t now_ms = millis();
  if (g_ntp_cb_pending) {
    g_ntp_cb_pending = false;
    g_ntp_sync_in_progress = false;
    g_ntp_time_valid = true;
    g_last_ntp_ok_ms = now_ms;
    g_ntp_retry_period_ms = kNtpResyncPeriodMs;
    SendTimeSyncToGui("ntp");
    PrintCurrentLocalTime(Serial);
  }

  if (g_network_config.ssid.isEmpty()) {
    return;
  }
  if (g_ntp_sync_in_progress) {
    if (now_ms - g_last_ntp_attempt_ms >= kNtpSyncTimeoutMs) {
      g_ntp_sync_in_progress = false;
      Serial.println(F("[NTP] Sync attempt timed out; will retry later"));
      if (!g_ntp_time_valid && g_ntp_retry_period_ms < kNtpRetryMaxMs) {
        g_ntp_retry_period_ms = std::min(g_ntp_retry_period_ms * 2U, kNtpRetryMaxMs);
      }
      g_last_ntp_attempt_ms = now_ms;
    }
    return;
  }

  if ((g_last_ntp_attempt_ms != 0U) &&
      ((now_ms - g_last_ntp_attempt_ms) < g_ntp_retry_period_ms)) {
    return;
  }

  if (!EnsureWifiConnected(Serial)) {
    if (!g_ntp_time_valid && g_ntp_retry_period_ms < kNtpRetryMaxMs) {
      g_ntp_retry_period_ms = std::min(g_ntp_retry_period_ms * 2U, kNtpRetryMaxMs);
    }
    g_last_ntp_attempt_ms = now_ms;
    return;
  }
  (void)SyncTimeFromNtp(Serial);
}

// Mirror the original root behaviour: let painlessMesh own STA attach.
// We only apply credentials via stationManual() and seed the RTC so SNTP can
// adjust it later. Explicit WiFi.begin()/mode juggling caused mesh links to
// bounce, so we avoid it here.
static void InitNetwork() {
  LoadNetworkConfigFromNVS();

  if (g_network_config.ssid.isEmpty()) {
    Serial.println(F("[WiFi] No SSID configured; skipping WiFi/NTP"));
    SetDefaultDateTime();
    g_ntp_time_valid = false;
    g_last_ntp_attempt_ms = millis();
    g_ntp_retry_period_ms = kNtpRetryInitialMs;
    return;
  }

  Serial.printf("[WiFi] Using SSID=\"%s\"\n", g_network_config.ssid.c_str());
  EnsureMeshStationConfigApplied(Serial, /*force_reapply=*/true);
  Serial.println(F(
      "[WiFi] STA credentials applied; root mesh node will connect to WiFi "
      "in the background when possible."));

  SetDefaultDateTime();
  g_ntp_time_valid = false;
  g_ntp_retry_period_ms = kNtpRetryInitialMs;
  g_last_ntp_attempt_ms = millis();
}

static void WifiLoop() {
  if (g_network_config.ssid.isEmpty()) {
    return;
  }
  const wl_status_t status = WiFi.status();
  const bool connected = (status == WL_CONNECTED);
  if (connected != g_wifi_connected) {
    g_wifi_connected = connected;
    JsonDocument doc;
    doc["type"] = connected ? "wifi_connected" : "wifi_disconnected";
    doc["ssid"] = g_network_config.ssid;
    doc["ip"] = connected ? WiFi.localIP().toString() : "";
    SendJsonLineWithEcho(gui_serial, doc);
    if (g_debug_verbose) {
      Serial.printf("[WiFi] %s ssid=\"%s\" ip=%s\n",
                    connected ? "connected" : "disconnected",
                    g_network_config.ssid.c_str(),
                    connected ? WiFi.localIP().toString().c_str() : "");
    }
  }
}

// -----------------------------------------------------------------------------
// Console command handlers
// -----------------------------------------------------------------------------

static void CmdHelp(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  g_console.PrintHelp(out);
}

static void CmdDebug(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.printf("debug %s\n", g_debug_verbose ? "on" : "off");
    out.println(F("Use 'debug on|off' to toggle verbose mesh/wifi/ntp logging."));
    return;
  }

  const String sub = argv[1];
  if (sub.equalsIgnoreCase("on")) {
    g_debug_verbose = true;
    out.println(F("[DEBUG] verbose logging enabled"));
    return;
  }
  if (sub.equalsIgnoreCase("off")) {
    g_debug_verbose = false;
    out.println(F("[DEBUG] verbose logging disabled"));
    return;
  }

  out.println(F("ERR debug (use on|off)"));
}

static void CmdPassthru(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.printf("passthru %s\n", g_gui_passthrough ? "on" : "off");
    out.println(F("Use 'passthru on' to mirror GUI UART (17/18) to USB."));
    out.println(F("Exit passthrough with '~.' when active."));
    return;
  }

  if (argv[1].equalsIgnoreCase("on")) {
    g_gui_passthrough = true;
    g_gui_passthrough_escape = false;
    g_gui_passthrough_tx_line = "";
    g_gui_passthrough_rx_line = "";
    out.println(F("[BRIDGE<->GUI] passthrough enabled; exit with '~.'"));
    return;
  }

  if (argv[1].equalsIgnoreCase("off")) {
    g_gui_passthrough = false;
    g_gui_passthrough_tx_line = "";
    g_gui_passthrough_rx_line = "";
    out.println(F("[BRIDGE<->GUI] passthrough disabled"));
    return;
  }

  out.println(F("ERR passthru (use on|off)"));
}

static void CmdWifi(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  String line;
  for (int i = 0; i < argc; ++i) {
    if (i > 0) line += ' ';
    line += argv[i];
  }

  if (argc < 2) {
    out.println(F("usage: wifi status | wifi scan | wifi connect | wifi ssid <value...> | wifi password <value...> | wifi clear"));
    return;
  }

  const String sub = argv[1];
  if (sub.equalsIgnoreCase("status")) {
    out.printf("ssid=\"%s\"\n", g_network_config.ssid.c_str());
    out.printf("tz_minutes=%ld dst=%s\n", static_cast<long>(g_network_config.timezone_minutes),
               g_network_config.dst_enabled ? "on" : "off");
    const wl_status_t st = WiFi.status();
    out.printf("wifi_status=%d\n", static_cast<int>(st));
    const int32_t phy_channel = WiFi.channel();
    if (phy_channel > 0) {
      out.printf("radio_channel=%d\n", static_cast<int>(phy_channel));
    }
    if (st == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      out.printf("ip=%s rssi=%d dBm\n", ip.toString().c_str(), WiFi.RSSI());
    } else {
      out.println(F("ip=(no STA connection)"));
    }
    PrintCurrentLocalTime(out);
    return;
  }

  if (sub.equalsIgnoreCase("scan")) {
    out.println(F("#    RSSI  CH   AUTH                 BSSID             SSID"));
    int count = WiFi.scanNetworks();
    for (int i = 0; i < count; ++i) {
      const int32_t rssi = WiFi.RSSI(i);
      const int32_t channel = WiFi.channel(i);
      const String auth_str = String(WiFi.encryptionType(i));
      const String bssid = WiFi.BSSIDstr(i);
      const String ssid = WiFi.SSID(i);
      out.printf("%3d  %4d  %4d  %-20s  %17s  %s\n", i, static_cast<int>(rssi),
                 static_cast<int>(channel), auth_str.c_str(), bssid.c_str(),
                 ssid.c_str());
    }
    WiFi.scanDelete();
    return;
  }

  if (sub.equalsIgnoreCase("ssid")) {
    String new_ssid;
    if (!ExtractRawTailAfterSecondToken(line, &new_ssid)) {
      out.println(F("ERR wifi ssid (use: wifi ssid <value...>)"));
      return;
    }
    g_network_config.ssid = new_ssid;
    SaveNetworkConfigToNVS();
    EnsureMeshStationConfigApplied(out, /*force_reapply=*/true);
    out.printf("wifi ssid set to \"%s\"\n", g_network_config.ssid.c_str());
    return;
  }

  if (sub.equalsIgnoreCase("password") || sub.equalsIgnoreCase("pwd")) {
    String new_password;
    if (!ExtractRawTailAfterSecondToken(line, &new_password)) {
      out.println(F("ERR wifi password (use: wifi password <value...>)"));
      return;
    }
    g_network_config.password = new_password;
    SaveNetworkConfigToNVS();
    EnsureMeshStationConfigApplied(out, /*force_reapply=*/true);
    out.println(F("wifi password set (value not echoed)"));
    return;
  }

  if (sub.equalsIgnoreCase("connect")) {
    const bool connected_now = EnsureWifiConnected(out);
    if (connected_now) {
      (void)SyncTimeFromNtp(out);
    }
    return;
  }

  if (sub.equalsIgnoreCase("clear")) {
    g_network_config.ssid.clear();
    g_network_config.password.clear();
    SaveNetworkConfigToNVS();
    out.println(F("wifi config cleared"));
    return;
  }

  out.println(F("ERR wifi (use: wifi status|scan|connect|ssid|password|clear)"));
}

static void CmdTime(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  if (argc <= 1) {
    out.println(F("usage:\n  time now          - show current local time\n  time sync         - force NTP sync now"));
    return;
  }
  const String sub = argv[1];
  if (sub.equalsIgnoreCase("now") || sub.equalsIgnoreCase("show")) {
    PrintCurrentLocalTime(out);
    return;
  }
  if (sub.equalsIgnoreCase("sync")) {
    if (g_network_config.ssid.isEmpty()) {
      out.println(F("[TIME] SSID not configured; use 'wifi ssid ...' first"));
      return;
    }
    g_last_ntp_attempt_ms = millis();
    if (!SyncTimeFromNtp(out) && !g_ntp_time_valid) {
      SetDefaultDateTime();
    }
    return;
  }
  out.println(F("ERR time (use 'time now' or 'time sync')"));
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(DBG_BAUD);
  gui_serial.begin(BRIDGE_GUI_BAUD, SERIAL_8N1, BRIDGE_GUI_RX_PIN, BRIDGE_GUI_TX_PIN);

  RootPickMeshChannel();
  LoadNetworkConfigFromNVS();

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &user_scheduler, MESH_PORT, WIFI_AP_STA,
            g_mesh_channel, MESH_HIDDEN);
  mesh.setRoot(true);
  mesh.setContainsRoot(true);
  mesh.onReceive(&OnReceiveRoot);
  mesh.onChangedConnections(&OnConnectionsChangedRoot);

  g_task_announce.set(TASK_IMMEDIATE, TASK_FOREVER, []() {
    RootAnnounce();
    g_task_announce.delay(ROOT_ANNOUNCE_MS);
  });
  user_scheduler.addTask(g_task_announce);
  g_task_announce.enable();

  g_console.RegisterCommand("help", &CmdHelp, "show help");
  g_console.RegisterCommand("debug", &CmdDebug, "debug on|off");
  g_console.RegisterCommand("passthru", &CmdPassthru,
                            "mirror GUI UART (17/18) to USB");
  g_console.RegisterCommand("wifi", &CmdWifi, "wifi status|scan|connect|ssid|password|clear");
  g_console.RegisterCommand("time", &CmdTime, "time now|sync");

  InitNetwork();

  SendBridgeHello();
  SendBridgeStatus("boot");
}

void loop() {
  mesh.update();
  user_scheduler.execute();
  WifiLoop();
  NtpLoop();
  if (g_gui_passthrough) {
    ProcessGuiPassthrough();
  } else {
    g_console.Poll(Serial, Serial);
  }
}

