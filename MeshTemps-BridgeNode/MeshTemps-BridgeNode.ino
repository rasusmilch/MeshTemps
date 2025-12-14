// MeshTemps-BridgeNode.ino
//
// Headless bridge for the MeshTemps network. Runs the root mesh + WiFi duties
// from the original MeshTemps-RootNode sketch, but skips all LVGL rendering and
// forwards leaf payloads to the dedicated GUI node over UART.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sntp.h>
#include <painlessMesh.h>
#include <algorithm>
#include <map>
#include <stdarg.h>
#include <vector>

#include "Config.h"

// Bridge <-> GUI uses a dedicated UART on GPIO16/17 so we leave UART0 on the
// CH434 for flashing/console access.
#ifndef BRIDGE_GUI_TX_PIN
#define BRIDGE_GUI_TX_PIN 17
#endif
#ifndef BRIDGE_GUI_RX_PIN
#define BRIDGE_GUI_RX_PIN 16
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
static String g_gui_rx_line;
static bool g_debug_verbose = false;
static std::map<uint32_t, uint32_t> g_node_last_seen_ms;

// ntfy (Wi-Fi bridge) ---------------------------------------------------------
struct NtfyConfig {
  bool enabled = true;
  String server_url = "https://ntfy.sh";
  String alert_topic = "meshtemps-alerts";
  String summary_topic = "meshtemps-summary";
};

struct NtfyRequest {
  String message;
  String title;
  bool is_summary = false;
  bool cache_when_offline = false;
};

static NtfyConfig g_ntfy_config;
struct PendingNtfy {
  NtfyRequest req;
  uint32_t sequence = 0; // optional sequence number from GUI (0 = none)
  uint32_t backoff_ms = 0;
  uint32_t next_attempt_ms = 0;
};

static std::vector<PendingNtfy> g_ntfy_queue;
static uint32_t g_ntfy_last_attempt_ms = 0;
constexpr uint32_t kNtfyBackoffInitialMs = 15000;     // 15 seconds
constexpr uint32_t kNtfyBackoffMaxMs = 10 * 60 * 1000; // 10 minutes
constexpr uint32_t kNtfyMinAttemptSpacingMs = 2000;    // 2 seconds

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

// -----------------------------------------------------------------------------
// ntfy helpers
// -----------------------------------------------------------------------------

static bool SendNtfyHttp(const NtfyRequest &req) {
  if (!g_ntfy_config.enabled) {
    DebugPrintln(F("[NTFY] disabled; dropping request"));
    return false;
  }

  const String &topic =
      req.is_summary ? g_ntfy_config.summary_topic : g_ntfy_config.alert_topic;
  const String &server = g_ntfy_config.server_url;

  if (topic.isEmpty() || server.isEmpty()) {
    Serial.println(F("[NTFY] Missing topic/server; cannot send"));
    return false;
  }

  String base = server;
  if (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  String url = base + "/" + topic;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.printf("[NTFY] http.begin failed for %s\n", url.c_str());
    return false;
  }

  if (!req.title.isEmpty()) {
    http.addHeader("Title", req.title);
  }
  http.addHeader("Content-Type", "text/plain");

  const int status = http.POST(req.message);
  http.end();

  if (status >= 200 && status < 300) {
    DebugPrintf("[NTFY] sent topic=%s status=%d\n", topic.c_str(), status);
    return true;
  }

  Serial.printf("[NTFY] post failed status=%d\n", status);
  return false;
}

static void SendNtfyAck(uint32_t sequence) {
  if (sequence == 0U) return; // no sequence to ack

  JsonDocument doc;
  doc["type"] = "ntfy_ack";
  doc["seq"] = sequence;
  SendJsonLineWithEcho(doc);
}

static void QueueNtfyRequest(const NtfyRequest &req, uint32_t sequence = 0U,
                              uint32_t backoff_ms = kNtfyBackoffInitialMs) {
  constexpr size_t kMaxQueue = 20;
  if (g_ntfy_queue.size() >= kMaxQueue) {
    Serial.println(F("[NTFY] queue full; dropping"));
    return;
  }
  PendingNtfy pending;
  pending.req = req;
  pending.sequence = sequence;
  pending.backoff_ms = backoff_ms;
  pending.next_attempt_ms = millis();

  g_ntfy_queue.push_back(pending);
  Serial.printf("[NTFY] cached request (size=%u)\n",
                static_cast<unsigned>(g_ntfy_queue.size()));
}

static void TrySendOrQueueNtfy(const NtfyRequest &req, uint32_t sequence = 0U,
                              uint32_t backoff_ms = kNtfyBackoffInitialMs) {
  if (!g_ntfy_config.enabled) {
    Serial.println(F("[NTFY] disabled; ignoring request"));
    return;
  }

  const bool wifi_up = (WiFi.status() == WL_CONNECTED);
  if (!wifi_up && !req.cache_when_offline) {
    Serial.println(F("[NTFY] Wi-Fi down; dropping uncached request"));
    return;
  }

  QueueNtfyRequest(req, sequence, backoff_ms);
}

static size_t PickNextNtfyIndex(uint32_t now_ms) {
  size_t chosen_idx = SIZE_MAX;
  bool chosen_is_alert = false;

  for (size_t i = 0; i < g_ntfy_queue.size(); ++i) {
    const PendingNtfy &pending = g_ntfy_queue[i];
    if (pending.next_attempt_ms > now_ms) {
      continue;
    }

    const bool is_alert = !pending.req.is_summary;
    if (chosen_idx == SIZE_MAX) {
      chosen_idx = i;
      chosen_is_alert = is_alert;
      continue;
    }

    // Alerts take priority over summaries for all transmissions.
    if (is_alert && !chosen_is_alert) {
      chosen_idx = i;
      chosen_is_alert = true;
      continue;
    }

    // Otherwise, pick the earliest due item.
    if (pending.next_attempt_ms < g_ntfy_queue[chosen_idx].next_attempt_ms) {
      chosen_idx = i;
      chosen_is_alert = is_alert;
    }
  }

  return chosen_idx;
}

static void MaybeSendQueuedNtfy() {
  if (!g_ntfy_config.enabled) return;
  if (g_ntfy_queue.empty()) return;

  const uint32_t now_ms = millis();
  if (g_ntfy_last_attempt_ms != 0U &&
      (now_ms - g_ntfy_last_attempt_ms) < kNtfyMinAttemptSpacingMs) {
    return;
  }

  const size_t idx = PickNextNtfyIndex(now_ms);
  if (idx == SIZE_MAX) {
    return; // nothing ready to send yet
  }

  PendingNtfy &pending = g_ntfy_queue[idx];
  if (WiFi.status() != WL_CONNECTED) {
    pending.next_attempt_ms = now_ms + pending.backoff_ms;
    pending.backoff_ms = std::min(pending.backoff_ms * 2, kNtfyBackoffMaxMs);
    return;
  }

  const bool ok = SendNtfyHttp(pending.req);
  g_ntfy_last_attempt_ms = now_ms;
  if (ok) {
    SendNtfyAck(pending.sequence);
    g_ntfy_queue.erase(g_ntfy_queue.begin() + idx);
  } else {
    pending.next_attempt_ms = now_ms + pending.backoff_ms;
    pending.backoff_ms = std::min(pending.backoff_ms * 2, kNtfyBackoffMaxMs);
  }
}

static void SendLineToGui(const String &line) {
  if (g_gui_passthrough || g_debug_verbose) {
    Serial.print(F("[BRIDGE->GUI SENT] "));
    Serial.println(line);
  }

  gui_serial.println(line);
}

static void SendJsonLineWithEcho(const JsonDocument &doc) {
  String line;
  serializeJson(doc, line);
  SendLineToGui(line);
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

static String JoinArgs(int argc, const String argv[], int start_idx) {
  String out;
  for (int i = start_idx; i < argc; ++i) {
    if (i > start_idx) {
      out += ' ';
    }
    out += argv[i];
  }
  return out;
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

static void LoadNtfyConfigFromNvs() {
  Preferences prefs;
  if (!prefs.begin("ntfy", true)) {
    return;
  }

  g_ntfy_config.enabled = prefs.getInt("enabled", 1) != 0;
  g_ntfy_config.server_url = prefs.getString("server", g_ntfy_config.server_url);
  g_ntfy_config.alert_topic =
      prefs.getString("alert_topic", g_ntfy_config.alert_topic);
  g_ntfy_config.summary_topic =
      prefs.getString("summary_topic", g_ntfy_config.summary_topic);

  prefs.end();
}

static void SaveNtfyConfigToNvs() {
  Preferences prefs;
  if (!prefs.begin("ntfy", false)) {
    return;
  }

  prefs.putInt("enabled", g_ntfy_config.enabled ? 1 : 0);
  prefs.putString("server", g_ntfy_config.server_url);
  prefs.putString("alert_topic", g_ntfy_config.alert_topic);
  prefs.putString("summary_topic", g_ntfy_config.summary_topic);

  prefs.end();
}

static bool BuildBridgeNvsBackup(String *out_json, Print &out) {
  if (out_json == nullptr) return false;

#if defined(ARDUINOJSON_VERSION_MAJOR) && (ARDUINOJSON_VERSION_MAJOR >= 7)
  JsonDocument doc;
#else
  StaticJsonDocument<2048> doc;
#endif

  doc.clear();
  doc["kind"] = "meshtemps-nvs";
  doc["node"] = "bridge";
  doc["version"] = 1;

  Preferences wifi;
  if (wifi.begin("wifi", true)) {
    JsonObject wifi_obj = doc["wifi"].to<JsonObject>();
    wifi_obj["tz_min"] = wifi.getInt("tz_min", 0);
    wifi_obj["tz_dst"] = wifi.getInt("tz_dst", 0) != 0;
    wifi.end();
  }

  Preferences ntfy;
  if (ntfy.begin("ntfy", true)) {
    JsonObject ntfy_obj = doc["ntfy"].to<JsonObject>();
    ntfy_obj["enabled"] = ntfy.getInt("enabled", 1) != 0;
    ntfy_obj["server"] = ntfy.getString("server", g_ntfy_config.server_url);
    ntfy_obj["alert_topic"] =
        ntfy.getString("alert_topic", g_ntfy_config.alert_topic);
    ntfy_obj["summary_topic"] =
        ntfy.getString("summary_topic", g_ntfy_config.summary_topic);
    ntfy.end();
  }

  out_json->clear();
  serializeJson(doc, *out_json);
  return true;
}

static bool RestoreBridgeNvsFromJson(const String &json, Print &out) {
#if defined(ARDUINOJSON_VERSION_MAJOR) && (ARDUINOJSON_VERSION_MAJOR >= 7)
  JsonDocument doc;
#else
  StaticJsonDocument<2048> doc;
#endif

  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    out.printf("nvs restore: invalid JSON (%s)\n", err.c_str());
    return false;
  }

  int applied = 0;

  if (doc.containsKey("wifi")) {
    JsonObject wifi_obj = doc["wifi"];
    bool changed = false;

    // Start from existing values so omitted fields are preserved.
    LoadNetworkConfigFromNVS();

    if (wifi_obj.containsKey("ssid")) {
      g_network_config.ssid = wifi_obj["ssid"].as<String>();
      changed = true;
    }
    if (wifi_obj.containsKey("pwd")) {
      g_network_config.password = wifi_obj["pwd"].as<String>();
      changed = true;
    }
    if (wifi_obj.containsKey("tz_min")) {
      g_network_config.timezone_minutes = wifi_obj["tz_min"].as<int32_t>();
      changed = true;
    }
    if (wifi_obj.containsKey("tz_dst")) {
      g_network_config.dst_enabled = wifi_obj["tz_dst"].as<bool>();
      changed = true;
    }

    if (changed) {
      SaveNetworkConfigToNVS();
      ++applied;
    }
  }

  if (doc.containsKey("ntfy")) {
    JsonObject ntfy_obj = doc["ntfy"];
    g_ntfy_config.enabled = ntfy_obj["enabled"].as<bool>();
    g_ntfy_config.server_url = ntfy_obj["server"].as<String>();
    g_ntfy_config.alert_topic = ntfy_obj["alert_topic"].as<String>();
    g_ntfy_config.summary_topic = ntfy_obj["summary_topic"].as<String>();
    SaveNtfyConfigToNvs();
    ++applied;
  }

  out.printf("nvs restore: applied %d section(s)\n", applied);
  return applied > 0;
}

static const char *WifiStatusToString(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN_DONE";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

// Scan nearby APs for our configured SSID and reuse its channel so the STA
// interface doesn't force the mesh AP to hop (which would drop leaf links).
static int32_t ScanChannelForConfiguredSsid(Print &out) {
  LoadNetworkConfigFromNVS();

  if (g_network_config.ssid.isEmpty()) {
    out.println(F("[WiFi] SSID not configured; using default mesh channel"));
    return -1;
  }

  out.println(F("[WiFi] Scanning for configured SSID to pick mesh channel..."));
  int32_t best_rssi = -1000;
  int32_t found_channel = -1;

  const int network_count =
      WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);

  if (network_count <= 0) {
    out.println(F("[WiFi] WiFi.scanNetworks() found no APs"));
  } else {
    for (int i = 0; i < network_count; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid == g_network_config.ssid) {
        const int chan = WiFi.channel(i);
        const int rssi = WiFi.RSSI(i);
        if (chan > 0 && rssi > best_rssi) {
          best_rssi = rssi;
          found_channel = chan;
        }
      }
    }
  }

  WiFi.scanDelete();

  if (found_channel > 0) {
    out.printf("[WiFi] Chose channel %d for mesh (AP RSSI=%d dBm)\n",
               static_cast<int>(found_channel), best_rssi);
  } else {
    out.println(
        F("[WiFi] Configured SSID not found; keeping default mesh channel"));
  }
  return found_channel;
}

static void RootPickMeshChannel() {
  int32_t chan = ScanChannelForConfiguredSsid(Serial);
  if (chan > 0) {
    g_mesh_channel = chan;
  } else {
    g_mesh_channel = MESH_CHANNEL; // fallback
  }
}

static void RootAnnounce() {
  JsonDocument doc;
  const uint32_t now_ms = millis();
  doc["type"] = "root_announce";
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
  SendJsonLineWithEcho(doc);
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
  JsonArray nodes = doc["nodes"].to<JsonArray>();
  for (const auto &id : node_list) {
    JsonObject node = nodes.add<JsonObject>();
    node["id"] = id;
    auto it = g_node_last_seen_ms.find(id);
    if (it != g_node_last_seen_ms.end()) {
      node["lastSeenMs"] = static_cast<uint32_t>(now_ms - it->second);
    }
  }
  if (reason != nullptr) {
    doc["reason"] = reason;
  }
  SendJsonLineWithEcho(doc);
  DebugPrintf("[BRIDGE] status connections=%u\n",
              static_cast<unsigned>(node_list.size()));
}

static bool SendTimeSyncToGui(const char *source, bool allow_stale_time = false) {
  if (!g_ntp_time_valid && !allow_stale_time) {
    DebugPrintf("[TIME] skip time_sync source=%s (time not valid)\n", source);
    return false;
  }

  // Prefer the current RTC value so we propagate an up-to-date epoch instead
  // of reusing the last NTP sync moment. Fall back to the last sync time if
  // the RTC has not been seeded yet.
  time_t epoch = time(nullptr);
  if (epoch <= 0) {
    epoch = g_ntp_last_sync_epoch;
  }
  if (epoch <= 0) {
    DebugPrintf("[TIME] skip time_sync source=%s (no epoch available)\n", source);
    return false;
  }

  g_ntp_last_sync_epoch = epoch;

  JsonDocument doc;
  doc["type"] = "time_sync";
  doc["rootId"] = mesh.getNodeId();
  doc["epoch"] = static_cast<long>(epoch);
  doc["tzMinutes"] = g_network_config.timezone_minutes;
  doc["dst"] = g_network_config.dst_enabled;
  doc["source"] = source;
  SendJsonLineWithEcho(doc);
  DebugPrintf("[TIME] forwarded time_sync source=%s epoch=%ld tzMin=%ld dst=%s\n",
              source, static_cast<long>(g_ntp_last_sync_epoch),
              static_cast<long>(g_network_config.timezone_minutes),
              g_network_config.dst_enabled ? "on" : "off");
  return true;
}

static void ForwardMeshPayload(uint32_t from, const JsonDocument &payload) {
  JsonDocument envelope;
  envelope["type"] = "mesh_frame";
  envelope["from"] = from;
  envelope["rxMs"] = static_cast<uint32_t>(millis());
  envelope["payload"] = payload.as<JsonVariantConst>();
  SendJsonLineWithEcho(envelope);
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

static void HandleGuiJsonLine(const String &line) {
  if (line.isEmpty()) {
    return;
  }

  if (g_debug_verbose) {
    Serial.print(F("[GUI->BRIDGE] "));
    Serial.println(line);
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, line);
  if (err != DeserializationError::Ok) {
    Serial.printf("[GUI JSON] parse error=%s raw=%s\n", err.c_str(),
                  line.c_str());
    return;
  }

  const char *type = doc["type"] | "";
  if (strcmp(type, "time_request") == 0) {
    HandleGuiTimeRequest(doc);
  } else if (strcmp(type, "ntfy_request") == 0) {
    HandleGuiNtfyRequest(doc);
  } else {
    Serial.printf("[GUI JSON] unknown type=%s raw=%s\n", type, line.c_str());
  }
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
        Serial.print(F("[GUI->BRIDGE-PC RECEIVED] "));
        Serial.println(g_gui_passthrough_rx_line);
        HandleGuiJsonLine(g_gui_passthrough_rx_line);
      }
      g_gui_passthrough_rx_line = "";
    } else if (ch != '\r') {
      g_gui_passthrough_rx_line += ch;
    }
  }
}

static void HandleGuiTimeRequest(const JsonDocument &doc) {
  const bool force_ntp = doc["force"] | false;
  const char *reason = doc["reason"] | "gui_request";

  Serial.printf("[BRIDGE] gui time_request force=%s reason=%s\n",
                force_ntp ? "true" : "false", reason);

  const bool sent = SendTimeSyncToGui(reason);
  if (!sent) {
    Serial.printf("[TIME] time_sync to GUI skipped (reason=%s valid=%s)\n", reason,
                  g_ntp_time_valid ? "true" : "false");
  }
  // Always reply so the GUI can refresh its clock even if we only have a
  // coarse RTC value; a force_ntp below will refresh the authoritative time.
  SendTimeSyncToGui(reason, /*allow_stale_time=*/true);

  if (force_ntp || !g_ntp_time_valid) {
    // Try an immediate NTP sync regardless of the normal retry schedule.
    g_ntp_retry_period_ms = kNtpRetryInitialMs;
    g_last_ntp_attempt_ms = 0;
    if (EnsureWifiConnected(Serial)) {
      (void)SyncTimeFromNtp(Serial);
    }
  }
}

static void HandleGuiNtfyRequest(const JsonDocument &doc) {
  const char *msg = doc["message"] | "";
  if (msg[0] == '\0') {
    Serial.println(F("[NTFY] empty message from GUI; ignoring"));
    return;
  }

  NtfyRequest req;
  req.message = msg;
  req.title = doc["title"].as<String>();
  req.is_summary = doc["summary"].is<bool>() ? doc["summary"].as<bool>()
                                              : false;
  req.cache_when_offline = doc["cache"].is<bool>() ? doc["cache"].as<bool>()
                                                    : false;
  const uint32_t sequence = doc["seq"].is<uint32_t>() ? doc["seq"].as<uint32_t>()
                                                       : 0U;

  const bool wifi_up = (WiFi.status() == WL_CONNECTED);
  if (g_debug_verbose) {
    Serial.printf(
        "[NTFY] GUI req len=%u cache=%s summary=%s title=%s seq=%lu enabled=%s wifi=%s "
        "queue=%u raw=%s\n",
        static_cast<unsigned>(req.message.length()),
        req.cache_when_offline ? "yes" : "no", req.is_summary ? "yes" : "no",
        req.title.c_str(), static_cast<unsigned long>(sequence),
        g_ntfy_config.enabled ? "yes" : "no",
        wifi_up ? "up" : "down",
        static_cast<unsigned>(g_ntfy_queue.size()),
        doc.as<String>().c_str());
  }

  TrySendOrQueueNtfy(req, sequence);
}

static void ProcessGuiSerial() {
  while (gui_serial.available() > 0) {
    const char ch = gui_serial.read();
    if (ch == '\n') {
      if (g_debug_verbose || g_gui_passthrough) {
        Serial.print(F("[GUI->BRIDGE RECEIVED] "));
        Serial.println(g_gui_rx_line);
      }

      HandleGuiJsonLine(g_gui_rx_line);
      g_gui_rx_line = "";
    } else if (ch != '\r') {
      g_gui_rx_line += ch;
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
    SendJsonLineWithEcho(doc);

    if (connected) {
      MaybeSendQueuedNtfy();
    }
    if (g_debug_verbose) {
      Serial.printf("[WiFi] %s ssid=\"%s\" ip=%s\n",
                    connected ? "connected" : "disconnected",
                    g_network_config.ssid.c_str(),
                    connected ? WiFi.localIP().toString().c_str() : "");
    }
  }

  if (connected && !g_ntfy_queue.empty()) {
    MaybeSendQueuedNtfy();
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

static void CmdNtfy(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2 || argv[1].equalsIgnoreCase("show")) {
    out.printf("ntfy: enabled=%s\n", g_ntfy_config.enabled ? "yes" : "no");
    out.printf("  server=%s\n", g_ntfy_config.server_url.c_str());
    out.printf("  alert_topic=%s\n", g_ntfy_config.alert_topic.c_str());
    out.printf("  summary_topic=%s\n", g_ntfy_config.summary_topic.c_str());
    out.printf("  queued=%u\n", static_cast<unsigned>(g_ntfy_queue.size()));
    return;
  }

  const String sub = argv[1];
  if (sub.equalsIgnoreCase("enable")) {
    if (argc < 3) {
      out.println(F("ERR ntfy enable (use on|off)"));
      return;
    }
    g_ntfy_config.enabled = argv[2].equalsIgnoreCase("on") ||
                            argv[2].equalsIgnoreCase("true") ||
                            argv[2] == "1";
    SaveNtfyConfigToNvs();
    out.printf("ntfy: enabled=%s (saved)\n", g_ntfy_config.enabled ? "yes" : "no");
    return;
  }

  if (sub.equalsIgnoreCase("server")) {
    if (argc < 3) {
      out.println(F("ERR ntfy server <url>"));
      return;
    }
    g_ntfy_config.server_url = argv[2];
    SaveNtfyConfigToNvs();
    out.printf("ntfy: server=%s (saved)\n", g_ntfy_config.server_url.c_str());
    return;
  }

  if (sub.equalsIgnoreCase("topic")) {
    if (argc < 4) {
      out.println(F("ERR ntfy topic alert|summary <name>"));
      return;
    }
    const String which = argv[2];
    const String value = argv[3];
    if (which.equalsIgnoreCase("alert")) {
      g_ntfy_config.alert_topic = value;
      SaveNtfyConfigToNvs();
      out.printf("ntfy: alert_topic=%s (saved)\n",
                 g_ntfy_config.alert_topic.c_str());
      return;
    }
    if (which.equalsIgnoreCase("summary")) {
      g_ntfy_config.summary_topic = value;
      SaveNtfyConfigToNvs();
      out.printf("ntfy: summary_topic=%s (saved)\n",
                 g_ntfy_config.summary_topic.c_str());
      return;
    }
    out.println(F("ERR ntfy topic (use alert|summary <name>)"));
    return;
  }

  if (sub.equalsIgnoreCase("clearqueue")) {
    g_ntfy_queue.clear();
    out.println(F("ntfy: cleared pending queue"));
    return;
  }

  if (sub.equalsIgnoreCase("test")) {
    String payload("MeshTemps ntfy test");
    if (argc > 2) {
      payload = argv[2];
      for (int i = 3; i < argc; ++i) {
        payload += ' ';
        payload += argv[i];
      }
    }

    NtfyRequest req;
    req.message = payload;
    req.title = "MeshTemps test";
    req.cache_when_offline = true;
    TrySendOrQueueNtfy(req);

    NtfyRequest summary_req = req;
    summary_req.is_summary = true;
    TrySendOrQueueNtfy(summary_req);

    out.println(F("ntfy: test message queued to alert & summary"));
    return;
  }

  out.println(F("ERR ntfy (use show|enable|server|topic|clearqueue|test [msg])"));
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
    const char *st_str = WifiStatusToString(st);
    out.printf("wifi_status=%s (%d)\n", st_str, static_cast<int>(st));
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

static void CmdNvs(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.println(F("usage: nvs dump | nvs restore <json>"));
    return;
  }

  const String sub = argv[1];
  if (sub.equalsIgnoreCase("dump")) {
    String json;
    if (BuildBridgeNvsBackup(&json, out)) {
      out.println(json);
      out.println(F("nvs dump: copy the JSON above for restore"));
    } else {
      out.println(F("ERR nvs dump"));
    }
    return;
  }

  if (sub.equalsIgnoreCase("restore")) {
    if (argc < 3) {
      out.println(F("ERR nvs restore (provide JSON)"));
      return;
    }

    const String payload = JoinArgs(argc, argv, 2);
    if (RestoreBridgeNvsFromJson(payload, out)) {
      out.println(F("nvs restore: ok"));
    } else {
      out.println(F("nvs restore: no changes"));
    }
    return;
  }

  out.println(F("ERR nvs (use dump|restore)"));
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
    if (!EnsureWifiConnected(out)) {
      out.println(F("[TIME] WiFi not connected; cannot perform NTP sync"));
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

static void CmdTz(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // tz           -> show
  // tz show
  if (argc < 2 || argv[1].equalsIgnoreCase("show")) {
    const long tz_min = g_network_config.timezone_minutes;
    const long abs_min = (tz_min >= 0) ? tz_min : -tz_min;
    const long h = abs_min / 60;
    const long m = abs_min % 60;
    const char sign = (tz_min >= 0) ? '+' : '-';

    out.printf("tz: utc_offset=%ld min (%c%02ld:%02ld) dst=%s\n", tz_min, sign,
               h, m, g_network_config.dst_enabled ? "on" : "off");
    return;
  }

  // tz set <minutes> [dst on|off|0|1]
  if (argv[1].equalsIgnoreCase("set")) {
    if (argc < 3) {
      out.println(F("ERR tz set (usage: tz set <minutes> [dst on|off|0|1])"));
      return;
    }
    const long minutes = argv[2].toInt();

    bool dst = g_network_config.dst_enabled;
    if (argc >= 4) {
      const String dst_arg = argv[3];
      if (dst_arg.equalsIgnoreCase("on") || dst_arg == "1") {
        dst = true;
      } else if (dst_arg.equalsIgnoreCase("off") || dst_arg == "0") {
        dst = false;
      } else {
        out.println(
            F("ERR tz set (dst must be on|off|0|1 if provided as arg 3)"));
        return;
      }
    }

    g_network_config.timezone_minutes = static_cast<int32_t>(minutes);
    g_network_config.dst_enabled = dst;
    SaveNetworkConfigToNVS();

    const long tz_min = g_network_config.timezone_minutes;
    const long abs_min = (tz_min >= 0) ? tz_min : -tz_min;
    const long h = abs_min / 60;
    const long m = abs_min % 60;
    const char sign = (tz_min >= 0) ? '+' : '-';

    out.printf(
        "tz set: utc_offset=%ld min (%c%02ld:%02ld) dst=%s (saved to NVS)\n",
        tz_min, sign, h, m, g_network_config.dst_enabled ? "on" : "off");

    // If WiFi is already connected, re-sync time with new offset.
    if (WiFi.status() == WL_CONNECTED) {
      (void)SyncTimeFromNtp(out);
    }
    return;
  }

  out.println(F("ERR tz (use 'tz show' or 'tz set <minutes> [dst on|off]')"));
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(DBG_BAUD);
  gui_serial.begin(BRIDGE_GUI_BAUD, SERIAL_8N1, BRIDGE_GUI_RX_PIN, BRIDGE_GUI_TX_PIN);

  RootPickMeshChannel();

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
  g_console.RegisterCommand("ntfy", &CmdNtfy,
                            "ntfy show|enable|server|topic|clearqueue|test [msg]");
  g_console.RegisterCommand("nvs", &CmdNvs, "nvs dump|restore <json>");
  g_console.RegisterCommand("wifi", &CmdWifi, "wifi status|scan|connect|ssid|password|clear");
  g_console.RegisterCommand("tz", &CmdTz, "tz show|set <minutes> [dst on|off]");
  g_console.RegisterCommand("time", &CmdTime, "time now|sync");

  LoadNtfyConfigFromNvs();
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
    ProcessGuiSerial();
    g_console.Poll(Serial, Serial);
  }
}

