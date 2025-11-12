// MeshTemps.ino
//
// ROOT build:
//   - Always root node
//   - LVGL UI with Waveshare ESP32-S3 display
//
// LEAF build:
//   - DS18B20 temperature nodes with linear calibration
//
// Requires Config.h to define:
//   MESH_IS_ROOT, MESH_PREFIX, MESH_PASSWORD, MESH_PORT,
//   ONEWIRE_PIN, SEND_PERIOD_MS, ROOT_ANNOUNCE_MS,
//   addrToHex(const uint8_t*).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>
#include <painlessMesh.h>

#include "serial_console.h"


#include <array>
#include <vector>

using Address = std::array<uint8_t, 8>; // DS18B20 64-bit ROM code

static SerialConsole g_console;

// Linear: T_corr = a1 * T_raw + a0
struct Coeff {
  float a1;
  float a0;
};

enum CalStage {
  kCalIdle = 0,
  kCalIce,
  kCalBoil,
};

#include "Config.h" // Mesh / IO configuration, addrToHex()

// -----------------------------------------------------------------------------
// Shared mesh and logging
// -----------------------------------------------------------------------------

Scheduler user_scheduler;
painlessMesh mesh;

bool g_debug_enabled = false;

#define DLOG(fmt, ...)                                                         \
  do {                                                                         \
    if (g_debug_enabled) {                                                     \
      Serial.printf((fmt), ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

// Forward declarations used on ROOT builds from LogConnections().
#if MESH_IS_ROOT
void GuiUpdateNetwork(size_t peers);
void GuiRequestRender();
#endif // MESH_IS_ROOT

namespace {

void LogConnections() {
  const size_t peer_count = mesh.getNodeList().size();
  DLOG("Peers: %u\n", static_cast<unsigned int>(peer_count));

#if MESH_IS_ROOT
  GuiUpdateNetwork(peer_count);
  GuiRequestRender();
#endif // MESH_IS_ROOT
}

// Join argv[start..argc-1] with spaces.
static String JoinTokens(int argc, const String argv[], int start) {
  String out;
  for (int i = start; i < argc; ++i) {
    if (i > start) out += ' ';
    out += argv[i];
  }
  return out;
}

} // namespace

// -----------------------------------------------------------------------------
// ROOT BUILD
// -----------------------------------------------------------------------------
#if MESH_IS_ROOT

#include "esp_panel_board_custom_conf.h"
#include "lvgl_v8_port.h"
#include <Preferences.h>
#include <algorithm> // for std::sort
#include <esp_display_panel.hpp>
#include <limits>
#include <lvgl.h>
#include <map>

using esp_panel::board::Board;
using esp_panel::drivers::BusRGB;
using esp_panel::drivers::LCD;

constexpr int kStorageVersionCurrent = 6;

// Buzzer (root)
constexpr int kBuzzerPin = 6;
constexpr uint32_t kDefaultBeepLenMs = 150;   // ms buzzer ON
constexpr uint32_t kDefaultGapWarnMs = 10000; // ms between warning beeps
constexpr uint32_t kDefaultGapAlertMs = 5000; // ms between alert beeps

uint32_t g_beep_len_ms = kDefaultBeepLenMs;
uint32_t g_beep_gap_warn_ms = kDefaultGapWarnMs;
uint32_t g_beep_gap_alert_ms = kDefaultGapAlertMs;

// Flashing (root)
constexpr uint32_t kDefaultFlashIntervalMs =
    5000; // ms between flash toggles; 0 = off
uint32_t g_flash_interval_ms = kDefaultFlashIntervalMs;
bool g_flash_phase = false; // toggled in DisplayLoop()

// Persistent root prefs.
Preferences g_root_preferences;

// Last-seen data and labels (persisted labels, volatile last_seen).
JsonDocument g_last_seen;
JsonDocument g_labels;

// Mesh tasks.
Task g_task_announce;

// Units / highlight config.
bool g_display_fahrenheit = true; // true = °F, false = °C
bool g_highlight_missing_nodes = true;
uint32_t g_stale_minutes_threshold = 5; // minutes

// Dummy data mode (non-persistent).
bool g_use_dummy_data = false;

// Temperature limits (in °C, NaN = disabled).
float g_warn_low_c = 5.0;
float g_warn_high_c = 26.0;
float g_alert_low_c = 3.0;
float g_alert_high_c = 30.0;

// Aggregate state for buzzer.
bool g_any_warning = false;
bool g_any_alert = false;

// LVGL GUI state (root).
lv_obj_t *g_ui_label_title = nullptr;
lv_obj_t *g_ui_label_peers = nullptr;
lv_obj_t *g_ui_label_uptime = nullptr;
lv_obj_t *g_ui_tile_container = nullptr;

volatile bool g_gui_dirty = false;
volatile size_t g_ui_peers = 0;

Board *g_board = nullptr;

// Per-node persistent tile widgets.
struct TileWidgets {
  lv_obj_t *tile = nullptr;
  lv_obj_t *label_loc = nullptr;
  lv_obj_t *label_age = nullptr;
  lv_obj_t *sensor_label[2] = {nullptr, nullptr};

  // Base (non-flash) colors.
  lv_color_t bg_normal;
  lv_color_t fg_normal; // header + age

  // Flash-phase colors (for warn/alert tiles).
  lv_color_t bg_flash;
  lv_color_t fg_flash;

  lv_color_t sensor_color[2];
  int sensor_count = 0;

  bool node_has_alert = false;
  bool node_has_warning = false;
  bool is_missing = false;
  bool is_stale = false;

  bool last_flash_active = false;
};

std::map<String, TileWidgets> g_tiles;

volatile bool g_flash_dirty = false;

namespace {

void GuiRebuildTiles(); // forward

// ---------------------------------------------------------------------------
// Storage helpers
// ---------------------------------------------------------------------------

int LoadStorageVersion() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const int version = g_root_preferences.getInt("version", 0);
  g_root_preferences.end();
  return version;
}

void SaveStorageVersion(int version) {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putInt("version", version);
  g_root_preferences.end();
}

void LoadLabels() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const String json = g_root_preferences.getString("labels", "");
  g_root_preferences.end();

  g_labels.clear();
  if (!json.isEmpty()) {
    if (deserializeJson(g_labels, json) != DeserializationError::Ok) {
      g_labels.clear();
    }
  }
}

void SaveLabels() {
  String json;
  serializeJson(g_labels, json);

  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putString("labels", json);
  g_root_preferences.end();
}

void EraseLabels() {
  g_labels.clear();
  g_last_seen["nodes"].to<JsonObject>();
  g_labels["nodes"].to<JsonObject>();
  g_labels["sensors"].to<JsonObject>();
  SaveLabels();
}

void SaveLimits() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putFloat("warn_low_c", g_warn_low_c);
  g_root_preferences.putFloat("warn_high_c", g_warn_high_c);
  g_root_preferences.putFloat("alert_low_c", g_alert_low_c);
  g_root_preferences.putFloat("alert_high_c", g_alert_high_c);
  g_root_preferences.end();
}

void LoadLimits() {
  // Start from whatever defaults are in the globals; only override with valid
  // ranges.
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const float wl = g_root_preferences.getFloat("warn_low_c", g_warn_low_c);
  const float wh = g_root_preferences.getFloat("warn_high_c", g_warn_high_c);
  const float al = g_root_preferences.getFloat("alert_low_c", g_alert_low_c);
  const float ah = g_root_preferences.getFloat("alert_high_c", g_alert_high_c);
  g_root_preferences.end();

  if (wh > wl) {
    g_warn_low_c = wl;
    g_warn_high_c = wh;
  }
  if (ah > al) {
    g_alert_low_c = al;
    g_alert_high_c = ah;
  }
}

void LoadDisplayUnits() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const int stored = g_root_preferences.getInt("units", 1); // default °F
  g_root_preferences.end();
  g_display_fahrenheit = (stored != 0);
}

void SaveDisplayUnits() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putInt("units", g_display_fahrenheit ? 1 : 0);
  g_root_preferences.end();
}

void LoadHighlightSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const int missing = g_root_preferences.getInt("hl_missing", 1);
  const int stale = g_root_preferences.getInt("hl_stale_min", 10);
  g_root_preferences.end();

  g_highlight_missing_nodes = (missing != 0);
  g_stale_minutes_threshold = stale > 0 ? static_cast<uint32_t>(stale) : 10U;
}

void SaveHighlightSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putInt("hl_missing", g_highlight_missing_nodes ? 1 : 0);
  g_root_preferences.putInt("hl_stale_min",
                            static_cast<int>(g_stale_minutes_threshold));
  g_root_preferences.end();
}

void SaveBuzzerSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putULong("beep_len_ms", g_beep_len_ms);
  g_root_preferences.putULong("beep_gap_warn_ms", g_beep_gap_warn_ms);
  g_root_preferences.putULong("beep_gap_alert_ms", g_beep_gap_alert_ms);
  g_root_preferences.end();
}

void LoadBuzzerSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const uint32_t len =
      g_root_preferences.getULong("beep_len_ms", kDefaultBeepLenMs);
  const uint32_t gw =
      g_root_preferences.getULong("beep_gap_warn_ms", kDefaultGapWarnMs);
  const uint32_t ga =
      g_root_preferences.getULong("beep_gap_alert_ms", kDefaultGapAlertMs);
  g_root_preferences.end();

  g_beep_len_ms = (len > 0) ? len : kDefaultBeepLenMs;
  g_beep_gap_warn_ms = gw;
  g_beep_gap_alert_ms = ga;
}

void SaveFlashSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putULong("flash_interval_ms", g_flash_interval_ms);
  g_root_preferences.end();
}

void LoadFlashSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const uint32_t fi =
      g_root_preferences.getULong("flash_interval_ms", kDefaultFlashIntervalMs);
  g_root_preferences.end();
  g_flash_interval_ms = fi;
}

void EnsureDocuments() {
  if (!g_last_seen["nodes"].is<JsonObject>())
    g_last_seen["nodes"].to<JsonObject>();
  if (!g_labels["nodes"].is<JsonObject>())
    g_labels["nodes"].to<JsonObject>();
  if (!g_labels["sensors"].is<JsonObject>())
    g_labels["sensors"].to<JsonObject>();
  if (!g_labels["order"].is<JsonObject>())
    g_labels["order"].to<JsonObject>(); // global sensor order (existing)
  if (!g_labels["tile_order"].is<JsonObject>())
    g_labels["tile_order"].to<JsonObject>(); // node/tile order (existing)

  // NEW: per-node sensor order map: sorder[nodeId][addr16] = rank
  if (!g_labels["sorder"].is<JsonObject>())
    g_labels["sorder"].to<JsonObject>();
}

// Default high rank means "unspecified / after ranked ones"
static int GetNodeRank(const String &node_id) {
  JsonVariant v = g_labels["tile_order"][node_id];
  if (v.is<int>())
    return v.as<int>();
  return 1000000;
}

// Persist only the topology (node IDs and sensor addr16s), not temperatures.
// Key: "known"
void SaveKnownTopology() {
  EnsureDocuments();
  JsonObject nodes_cur = g_last_seen["nodes"].as<JsonObject>();

  // Load prior snapshot (if any) so we can merge.
  JsonDocument kd;
  {
    g_root_preferences.begin("meshroot", /*readOnly=*/true);
    const String prev = g_root_preferences.getString("known", "");
    g_root_preferences.end();
    if (!prev.isEmpty()) {
      (void)deserializeJson(kd, prev);  // best-effort
    }
  }

  JsonObject nodes_out = kd["nodes"].to<JsonObject>();

  // Merge: ensure every current node/sensor is present in the saved set.
  for (JsonPair n : nodes_cur) {
    const char* node_id = n.key().c_str();
    JsonArray arr = nodes_out[node_id].to<JsonArray>();

    // Build a small set to avoid duplicates.
    std::vector<String> have;
    have.reserve(arr.size());
    for (JsonVariant v : arr) {
      if (v.is<const char*>()) have.emplace_back(v.as<const char*>());
    }

    auto has = [&](const String& addr) {
      for (const auto& x : have) if (x == addr) return true;
      return false;
    };

    JsonObject sensors = n.value()["sensors"].as<JsonObject>();
    for (JsonPair s : sensors) {
      const char* addr = s.key().c_str();
      if (addr && strlen(addr) == 16 && !has(addr)) {
        arr.add(addr);
      }
    }
  }

  String json;
  serializeJson(kd, json);
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putString("known", json);
  g_root_preferences.end();
}


// Load topology and pre-populate g_last_seen so tiles render after reboot.
// We seed "last" with now to start age at 0 and allow normal staleness aging.
void LoadKnownTopology() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const String json = g_root_preferences.getString("known", "");
  g_root_preferences.end();

  if (json.isEmpty())
    return;

  JsonDocument kd;
  if (deserializeJson(kd, json) != DeserializationError::Ok)
    return;

  EnsureDocuments();
  JsonObject nodes_out = g_last_seen["nodes"].to<JsonObject>();
  JsonObject nodes_in = kd["nodes"].as<JsonObject>();
  const uint32_t now_ms = millis();

  for (JsonPair np : nodes_in) {
    const String node_id = np.key().c_str();
    JsonObject node_obj = nodes_out[node_id].to<JsonObject>();
    if (!node_obj["sensors"].is<JsonObject>()) {
      node_obj["sensors"].to<JsonObject>();
    }
    node_obj["busGpio"] = node_obj["busGpio"] | -1;
    node_obj["last"] = now_ms;

    JsonObject sensors = node_obj["sensors"].to<JsonObject>();
    JsonArray addrs = np.value().as<JsonArray>();
    for (JsonVariant v : addrs) {
      const char *addr = v.as<const char *>();
      if (addr && strlen(addr) == 16) {
        JsonObject s = sensors[addr].to<JsonObject>();
        s["last"] = now_ms; // seed fresh; values will fill when data arrives
        // leave tC/corr absent until an update comes in
      }
    }
  }
}

// Remove persisted topology and clear in-memory nodes.
void EraseKnownTopology() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.remove("known");
  g_root_preferences.end();

  // Clear in-memory nodes (labels untouched).
  g_last_seen["nodes"].to<JsonObject>().clear();
}

void MigrateStorageIfNeeded() {
  const int version = LoadStorageVersion();

  if (version == 0 || version == 1) {
    // Old: labels only.
    LoadLabels();

    EnsureDocuments();

    // After LoadFlashSettings();
    LoadFlashSettings();

    // NEW: pull any previously saved topology to pre-populate tiles
    LoadKnownTopology();

    g_display_fahrenheit = true;
    SaveDisplayUnits();

    g_highlight_missing_nodes = true;
    g_stale_minutes_threshold = 10;
    SaveHighlightSettings();

    // Initialize default limits and persist.
    g_warn_low_c = 5.0f;
    g_warn_high_c = 26.0f;
    g_alert_low_c = 3.0f;
    g_alert_high_c = 30.0f;
    SaveLimits();

    // Buzzer + flash defaults.
    g_beep_len_ms = kDefaultBeepLenMs;
    g_beep_gap_warn_ms = kDefaultGapWarnMs;
    g_beep_gap_alert_ms = kDefaultGapAlertMs;
    SaveBuzzerSettings();

    g_flash_interval_ms = kDefaultFlashIntervalMs;
    SaveFlashSettings();

    SaveStorageVersion(kStorageVersionCurrent);
    return;
  }

  if (version == 2) {
    // v2: units + highlight; add limits + buzzer + flash.
    LoadLabels();
    EnsureDocuments();
    LoadDisplayUnits();
    LoadHighlightSettings();

    // After LoadFlashSettings();
    LoadFlashSettings();

    // NEW: pull any previously saved topology to pre-populate tiles
    LoadKnownTopology();

    g_warn_low_c = 5.0f;
    g_warn_high_c = 26.0f;
    g_alert_low_c = 3.0f;
    g_alert_high_c = 30.0f;
    SaveLimits();

    g_beep_len_ms = kDefaultBeepLenMs;
    g_beep_gap_warn_ms = kDefaultGapWarnMs;
    g_beep_gap_alert_ms = kDefaultGapAlertMs;
    SaveBuzzerSettings();

    g_flash_interval_ms = kDefaultFlashIntervalMs;
    SaveFlashSettings();

    SaveStorageVersion(kStorageVersionCurrent);
    return;
  }

  if (version == 3) {
    // v3: had limits; add buzzer + flash.
    LoadLabels();
    EnsureDocuments();
    LoadDisplayUnits();
    LoadHighlightSettings();
    LoadLimits();

    // After LoadFlashSettings();
    LoadFlashSettings();

    // NEW: pull any previously saved topology to pre-populate tiles
    LoadKnownTopology();

    g_beep_len_ms = kDefaultBeepLenMs;
    g_beep_gap_warn_ms = kDefaultGapWarnMs;
    g_beep_gap_alert_ms = kDefaultGapAlertMs;
    SaveBuzzerSettings();

    g_flash_interval_ms = kDefaultFlashIntervalMs;
    SaveFlashSettings();

    SaveStorageVersion(kStorageVersionCurrent);
    return;
  }

  if (version == 4) {
    // v4: had buzzer; add flash.
    LoadLabels();
    EnsureDocuments();
    LoadDisplayUnits();
    LoadHighlightSettings();
    LoadLimits();
    LoadBuzzerSettings();

    // After LoadFlashSettings();
    LoadFlashSettings();

    // NEW: pull any previously saved topology to pre-populate tiles
    LoadKnownTopology();

    g_flash_interval_ms = kDefaultFlashIntervalMs;
    SaveFlashSettings();

    SaveStorageVersion(kStorageVersionCurrent);
    return;
  }

  // v5+ normal load.
  LoadLabels();
  EnsureDocuments();
  LoadDisplayUnits();
  LoadHighlightSettings();
  LoadLimits();
  LoadBuzzerSettings();
  LoadFlashSettings();

  // NEW: pull any previously saved topology to pre-populate tiles
  LoadKnownTopology();
}

// Upper-case canonical form for 16-hex DS18B20 IDs.
// Keeps only the first 16 chars; caller should validate length elsewhere.
static String CanonAddr16(const String &addr) {
  String out;
  out.reserve(16);
  for (size_t i = 0; i < addr.length() && out.length() < 16; ++i) {
    char c = addr[i];
    if (c >= 'a' && c <= 'f')
      c = static_cast<char>(c - 'a' + 'A');
    out += c;
  }
  return out;
}

// Global (node-agnostic) rank.
static int GetSensorRankGlobal(const String &addr16) {
  const String key = CanonAddr16(addr16);
  JsonVariant v = g_labels["order"][key];
  if (v.is<int>())
    return v.as<int>();
  return 1000000;
}

// Node-specific rank, falling back to global if unset.
static int GetSensorRankForNode(const String &node_id, const String &addr16) {
  const String key = CanonAddr16(addr16);
  JsonVariant v = g_labels["sorder"][node_id][key];
  if (v.is<int>())
    return v.as<int>();
  return GetSensorRankGlobal(key);
}

// Find the [start,end) span (0-based token index) of a whitespace-delimited
// token.
static bool FindTokenSpan(const String &line, int token_index, int *start_out,
                          int *end_out) {
  if (!start_out || !end_out)
    return false;
  int n = static_cast<int>(line.length());
  int idx = -1;
  int i = 0;
  while (i < n) {
    // Skip leading whitespace
    while (i < n && isspace(static_cast<unsigned char>(line[i])))
      ++i;
    if (i >= n)
      break;
    int s = i;
    while (i < n && !isspace(static_cast<unsigned char>(line[i])))
      ++i;
    int e = i; // exclusive
    ++idx;
    if (idx == token_index) {
      *start_out = s;
      *end_out = e;
      return true;
    }
  }
  return false;
}

// Keep printable bytes only (>=0x20); drop control characters like CR/LF/TAB.
static String SanitizePrintable(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c >= 0x20)
      out += static_cast<char>(c);
  }
  return out;
}

// Extract label text after the 2nd token (command + id/addr), handling optional
// quotes. Examples accepted:
//   node 1234 Main Floor South
//   node 1234 "Main Floor \"South\""
//   name A1B2C3D4E5F6A7B8 Rack\ 1\ Inlet   (backslashes are preserved except
//   for \" and \\)
static bool ExtractLabelAfterSecondToken(const String &line,
                                         String *out_label) {
  if (!out_label)
    return false;

  int s0, e0, s1, e1;
  if (!FindTokenSpan(line, 0, &s0, &e0))
    return false; // command
  if (!FindTokenSpan(line, 1, &s1, &e1))
    return false; // id/addr

  // Start scanning just after token #1, skipping whitespace.
  int i = e1;
  const int n = static_cast<int>(line.length());
  while (i < n && isspace(static_cast<unsigned char>(line[i])))
    ++i;
  if (i >= n)
    return false; // no label provided

  String raw = line.substring(i);
  raw.trim();

  // If quoted, strip outer quotes and unescape \" and \\.
  if (raw.length() >= 2 && raw[0] == '"' && raw[raw.length() - 1] == '"') {
    String inner = raw.substring(1, raw.length() - 1);
    String unesc;
    unesc.reserve(inner.length());
    for (size_t k = 0; k < inner.length(); ++k) {
      char c = inner[k];
      if (c == '\\' && k + 1 < inner.length()) {
        char nchar = inner[k + 1];
        if (nchar == '"' || nchar == '\\') {
          unesc += nchar;
          ++k;
          continue;
        }
      }
      unesc += c;
    }
    raw = unesc;
  }

  *out_label = SanitizePrintable(raw);
  return out_label->length() > 0;
}

// ---------------------------------------------------------------------------
// Display + GUI setup
// ---------------------------------------------------------------------------

void DisplayInit() {
  Serial.println(F("[Display] Init board"));

  g_board = new Board();
  if (g_board == nullptr) {
    Serial.println(F("[Display] Board alloc failed"));
    while (true) {
      delay(1000);
    }
  }

  if (!g_board->init()) {
    Serial.println(F("[Display] board->init() failed"));
    while (true) {
      delay(1000);
    }
  }

#if LVGL_PORT_AVOID_TEARING_MODE
  {
    LCD *lcd = g_board->getLCD();
    if (lcd != nullptr) {
      lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);

#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
      auto *bus = lcd->getBus();
      if (bus != nullptr &&
          bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(bus)->configRGB_BounceBufferSize(
            lcd->getFrameWidth() * 10);
      }
#endif
    }
  }
#endif // LVGL_PORT_AVOID_TEARING_MODE

  if (!g_board->begin()) {
    Serial.println(F("[Display] board->begin() failed"));
    while (true) {
      delay(1000);
    }
  }

  Serial.println(F("[Display] Init LVGL port"));
  if (!lvgl_port_init(g_board->getLCD(), g_board->getTouch())) {
    Serial.println(F("[Display] lvgl_port_init() failed"));
    while (true) {
      delay(1000);
    }
  }

  Serial.println(F("[Display] Ready"));
}

void GuiInit() {
  if (!lvgl_port_lock(-1)) {
    return;
  }

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_pad_all(screen, 0, 0);

  // Disable all scrolling on the root screen
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  // Top bar.
  lv_obj_t *bar = lv_obj_create(screen);
  lv_obj_set_size(bar, lv_pct(100), 36);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(bar, 4, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_make(0x20, 0x20, 0x20), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  // White text on dark bar
  lv_obj_set_style_text_color(bar, lv_color_white(), 0);

  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

  g_ui_label_peers = lv_label_create(bar);
  lv_label_set_text(g_ui_label_peers, "Peers: 0");
  lv_obj_align(g_ui_label_peers, LV_ALIGN_LEFT_MID, 4, 0);

  g_ui_label_title = lv_label_create(bar);
  lv_label_set_text(g_ui_label_title, "Room Temps");
  lv_obj_align(g_ui_label_title, LV_ALIGN_CENTER, 0, 0);

  g_ui_label_uptime = lv_label_create(bar);
  lv_label_set_text(g_ui_label_uptime, "0:00");
  lv_obj_align(g_ui_label_uptime, LV_ALIGN_RIGHT_MID, -4, 0);

  // Tile container below the bar.
  g_ui_tile_container = lv_obj_create(screen);

  // Work out available height so we don't overlap the bar.
  lv_coord_t scr_h = lv_obj_get_height(screen);
  if (scr_h == 0) {
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
      scr_h = lv_disp_get_ver_res(disp);
    }
  }

  const lv_coord_t bar_h = lv_obj_get_height(bar);
  lv_coord_t cont_h = (scr_h > bar_h) ? (scr_h - bar_h) : scr_h;

  lv_obj_set_size(g_ui_tile_container, lv_pct(100), cont_h);
  lv_obj_align_to(g_ui_tile_container, bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

  lv_obj_set_style_pad_all(g_ui_tile_container, 6, 0);
  lv_obj_set_style_pad_row(g_ui_tile_container, 6, 0);
  lv_obj_set_style_pad_column(g_ui_tile_container, 6, 0);
  lv_obj_set_style_bg_opa(g_ui_tile_container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_ui_tile_container, 0, 0);

  lv_obj_set_flex_flow(g_ui_tile_container, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(g_ui_tile_container, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  // No scrolling for the tile container.
  lv_obj_clear_flag(g_ui_tile_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(g_ui_tile_container, LV_SCROLLBAR_MODE_OFF);

  // Ensure the bar is always drawn on top of tiles.
  lv_obj_move_foreground(bar);

  g_gui_dirty = true;

  lvgl_port_unlock();
}

// Called when node metadata changes.
void GuiUpdateNodeSummary(const char *node_id, int bus_gpio, uint32_t last_ms) {
  (void)node_id;
  (void)bus_gpio;
  (void)last_ms;
  g_gui_dirty = true;
}

// Called when sensor data changes.
void GuiUpdateSensorRow(const char *node_id, const char *addr, float temp_f,
                        const char *label, uint32_t last_ms) {
  (void)node_id;
  (void)addr;
  (void)temp_f;
  (void)label;
  (void)last_ms;
  g_gui_dirty = true;
}

void UpdateUptimeLabel() {
  if (g_ui_label_uptime == nullptr) {
    return;
  }
  const uint32_t seconds = millis() / 1000U;
  const uint32_t minutes = seconds / 60U;
  const uint32_t hours = minutes / 60U;

  char buffer[16];
  // Always show HH:MM (hours can go above 99 fine).
  snprintf(buffer, sizeof(buffer), "%02lu:%02lu",
           static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes % 60U));

  lv_label_set_text(g_ui_label_uptime, buffer);
}

// Dummy data injection (non-persistent).
void BuildDummyData() {
  g_last_seen.clear();
  EnsureDocuments();

  JsonObject nodes = g_last_seen["nodes"].to<JsonObject>();
  const uint32_t now_ms = millis();

  for (int i = 1; i <= 10; ++i) {
    const String node_key = String(1000 + i);
    JsonObject node_obj = nodes[node_key].to<JsonObject>();
    node_obj["busGpio"] = -1;
    node_obj["last"] = now_ms;

    const String loc = String("Dummy ") + String(i);
    g_labels["nodes"][node_key] = loc;

    JsonObject sensors = node_obj["sensors"].to<JsonObject>();

    char addr1[17];
    char addr2[17];
    snprintf(addr1, sizeof(addr1), "D%02d000000000001", i);
    snprintf(addr2, sizeof(addr2), "D%02d000000000002", i);

    JsonObject s1 = sensors[addr1].to<JsonObject>();
    JsonObject s2 = sensors[addr2].to<JsonObject>();

    g_labels["sensors"][addr1] = "Sensor 1";
    g_labels["sensors"][addr2] = "Sensor 2";

    const float base = 20.0f + static_cast<float>(i);

    s1["tC"] = base;
    s1["corr"] = false;
    s1["last"] = now_ms;

    s2["tC"] = base + 5.0f;
    s2["corr"] = false;
    s2["last"] = now_ms;
  }
}

// Placeholder buzzer.
void BuzzerBeepOnce() {
  // TODO: wire to actual buzzer pin.
  DLOG("[BUZZER] Beep\n");
}

TileWidgets &GetOrCreateTile(const String &node_id_str, lv_coord_t tile_w,
                             lv_coord_t tile_h) {
  auto it = g_tiles.find(node_id_str);
  if (it != g_tiles.end()) {
    // Ensure size in case geometry changed (e.g. rotation/res change).
    lv_obj_set_size(it->second.tile, tile_w, tile_h);
    return it->second;
  }

  TileWidgets tw;

  tw.tile = lv_obj_create(g_ui_tile_container);
  lv_obj_set_size(tw.tile, tile_w, tile_h);
  lv_obj_set_style_radius(tw.tile, 12, 0);
  lv_obj_set_style_pad_all(tw.tile, 6, 0);
  lv_obj_set_style_border_width(tw.tile, 0, 0);
  lv_obj_clear_flag(tw.tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(tw.tile, LV_SCROLLBAR_MODE_OFF);

  // Location label.
  tw.label_loc = lv_label_create(tw.tile);
  lv_obj_set_width(tw.label_loc, lv_pct(100));
  lv_obj_set_style_text_align(tw.label_loc, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(tw.label_loc, &lv_font_montserrat_16, 0);

  // Age label.
  tw.label_age = lv_label_create(tw.tile);
  lv_obj_set_width(tw.label_age, lv_pct(100));
  lv_obj_set_style_text_align(tw.label_age, LV_TEXT_ALIGN_CENTER, 0);

  // Sensor labels (up to 2).
  for (int i = 0; i < 2; ++i) {
    tw.sensor_label[i] = lv_label_create(tw.tile);
  }

  auto [ins_it, ok] = g_tiles.emplace(node_id_str, tw);
  return ins_it->second;
}

// ---------------------------------------------------------------------------
// Tile rebuild
// ---------------------------------------------------------------------------
void GuiApplyFlashPhase() {
  for (auto &entry : g_tiles) {
    TileWidgets &tw = entry.second;

    const bool flashable = (tw.node_has_alert || tw.node_has_warning);
    const bool flash_active =
        flashable && (g_flash_interval_ms > 0) && g_flash_phase;

    // If nothing changed for this tile, do nothing.
    if (flash_active == tw.last_flash_active) {
      continue;
    }
    tw.last_flash_active = flash_active;

    if (flash_active) {
      // FLASH: light gray bg, black text for entire tile.
      lv_obj_set_style_bg_color(tw.tile, tw.bg_flash, 0);
      lv_obj_set_style_bg_opa(tw.tile, LV_OPA_COVER, 0);

      lv_obj_set_style_text_color(tw.label_loc, tw.fg_flash, 0);
      lv_obj_set_style_text_color(tw.label_age, tw.fg_flash, 0);
      for (int i = 0; i < tw.sensor_count; ++i) {
        if (tw.sensor_label[i]) {
          lv_obj_set_style_text_color(tw.sensor_label[i], tw.fg_flash, 0);
        }
      }
    } else {
      // NORMAL: restore per-tile and per-sensor colors.
      lv_obj_set_style_bg_color(tw.tile, tw.bg_normal, 0);
      lv_obj_set_style_bg_opa(tw.tile, LV_OPA_COVER, 0);

      lv_obj_set_style_text_color(tw.label_loc, tw.fg_normal, 0);
      lv_obj_set_style_text_color(tw.label_age, tw.fg_normal, 0);
      for (int i = 0; i < tw.sensor_count; ++i) {
        if (tw.sensor_label[i]) {
          lv_obj_set_style_text_color(tw.sensor_label[i], tw.sensor_color[i],
                                      0);
        }
      }
    }
  }
}

// Update just the "Age: N min" labels and stale/missing state.
// No layout work, no object (re)creation.
static void GuiAgeTick(uint32_t now_ms) {
  EnsureDocuments();
  JsonObject nodes = g_last_seen["nodes"].as<JsonObject>();

  for (auto &kv : g_tiles) {
    const String &node_id = kv.first;
    TileWidgets &tw = kv.second;

    JsonObject node_obj = nodes[node_id];
    if (node_obj.isNull()) continue;

    uint32_t latest_ms = node_obj["last"] | 0U;
    JsonObject sensors = node_obj["sensors"].as<JsonObject>();
    for (JsonPair s_entry : sensors) {
      const uint32_t s_last = s_entry.value()["last"] | 0U;
      if (s_last > latest_ms) latest_ms = s_last;
    }

    const uint32_t age_min = (latest_ms <= now_ms) ? (now_ms - latest_ms) / 60000U : 0U;

    // Update Age label text only (no relayout).
    if (tw.label_age) {
      char age_buf[24];
      snprintf(age_buf, sizeof(age_buf), "Age: %lu min", (unsigned long)age_min);
      lv_label_set_text(tw.label_age, age_buf);
    }

    // Re-evaluate stale/missing and only flip colors if state changed.
    const bool was_missing = tw.is_missing;
    const bool was_stale   = tw.is_stale;

    const bool node_connected = false;  // tiles here are "known topology", connectivity is handled in full rebuilds
    const bool is_missing = g_highlight_missing_nodes && !node_connected &&
                            (g_stale_minutes_threshold > 0) &&
                            (age_min >= g_stale_minutes_threshold);
    const bool is_stale = node_connected && (g_stale_minutes_threshold > 0) &&
                          (age_min >= g_stale_minutes_threshold);

    if (is_missing != was_missing || is_stale != was_stale) {
      tw.is_missing = is_missing;
      tw.is_stale   = is_stale;

      // Minimal recolor only; do not touch layout.
      lv_color_t bg = lv_color_make(0x16, 0x3A, 0x24);
      lv_color_t fg = lv_color_white();

      if (tw.node_has_alert) {
        bg = lv_color_make(0xB7, 0x1C, 0x1C);
      } else if (is_stale || tw.node_has_warning) {
        bg = lv_color_make(0x8A, 0x5A, 0x00);
      } else if (is_missing) {
        bg = lv_color_make(0x20, 0x40, 0x60);
      }

      tw.bg_normal = bg;
      tw.fg_normal = fg;

      // Apply new base colors immediately (non-flash phase).
      lv_obj_set_style_bg_color(tw.tile, tw.bg_normal, 0);
      lv_obj_set_style_text_color(tw.label_loc, tw.fg_normal, 0);
      lv_obj_set_style_text_color(tw.label_age, tw.fg_normal, 0);
      for (int i = 0; i < tw.sensor_count && tw.sensor_label[i]; ++i) {
        lv_obj_set_style_text_color(tw.sensor_label[i], tw.sensor_color[i], 0);
      }
    }
  }
}

void GuiRebuildTiles() {
  if (g_ui_tile_container == nullptr)
    return;

  EnsureDocuments();
  JsonObject nodes = g_last_seen["nodes"].as<JsonObject>();

  // Snapshot current connections.
  auto node_list = mesh.getNodeList();
  std::vector<uint32_t> connected_ids(node_list.begin(), node_list.end());
  auto is_connected = [&](uint32_t node_id) -> bool {
    for (uint32_t id : connected_ids)
      if (id == node_id)
        return true;
    return false;
  };

  const uint32_t now_ms = millis();

  // Layout parameters.
  const lv_coord_t screen_w = lv_obj_get_width(lv_scr_act());
  const int columns = (screen_w >= 640) ? 3 : 2;
  const lv_coord_t gap = 6;
  const lv_coord_t tile_w = (screen_w - (columns + 1) * gap) / columns;
  const lv_coord_t tile_h = 100;

  g_any_warning = false;
  g_any_alert = false;

  // Sort nodes by explicit tile_order, then label, then id.
  std::vector<String> node_ids;
  node_ids.reserve(nodes.size());
  for (JsonPair e : nodes)
    node_ids.emplace_back(e.key().c_str());

  std::sort(node_ids.begin(), node_ids.end(),
            [&](const String &a, const String &b) {
              const int ra = GetNodeRank(a);
              const int rb = GetNodeRank(b);
              if (ra != rb)
                return ra < rb;

              const String la = (g_labels["nodes"][a] | "");
              const String lb = (g_labels["nodes"][b] | "");
              if (la.length() && lb.length()) {
                if (la != lb)
                  return la < lb;
              } else if (la.length() || lb.length()) {
                return la.length() > lb.length(); // labeled first
              }
              return a < b;
            });

  for (const String &node_id_str : node_ids) {
    JsonObject node_obj = nodes[node_id_str];

    TileWidgets &tw = GetOrCreateTile(node_id_str, tile_w, tile_h);

    // Location (label or node id).
    String loc = g_labels["nodes"][node_id_str] | "";
    if (loc.isEmpty())
      loc = node_id_str;

    // Connection + age.
    JsonObject sensors = node_obj["sensors"].as<JsonObject>();

    char *endptr = nullptr;
    const uint32_t node_id_u32 =
        static_cast<uint32_t>(strtoul(node_id_str.c_str(), &endptr, 10));
    const bool node_connected =
        (endptr != node_id_str.c_str()) && is_connected(node_id_u32);

    uint32_t latest_ms = node_obj["last"] | 0U;

    // Collect and sort sensor addresses for THIS node only.
    std::vector<String> addrs;
    addrs.reserve(8);
    for (JsonPair s_entry : sensors) {
      addrs.emplace_back(s_entry.key().c_str());
      const uint32_t s_last = s_entry.value()["last"] | 0U;
      if (s_last > latest_ms)
        latest_ms = s_last;
    }

    std::sort(addrs.begin(), addrs.end(),
              [&](const String &a, const String &b) {
                const int ra = GetSensorRankForNode(node_id_str, a);
                const int rb = GetSensorRankForNode(node_id_str, b);
                if (ra != rb)
                  return ra < rb;
                return a < b; // stable, readable fallback
              });

    const uint32_t age_min =
        (latest_ms <= now_ms) ? (now_ms - latest_ms) / 60000U : 0U;

    const bool is_missing = g_highlight_missing_nodes && !node_connected &&
                            (g_stale_minutes_threshold > 0) &&
                            (age_min >= g_stale_minutes_threshold);

    const bool is_stale = node_connected && (g_stale_minutes_threshold > 0) &&
                          (age_min >= g_stale_minutes_threshold);

    // Up to 2 sensor rows.
    struct SensorView {
      String label;
      float temp_c = NAN;
      bool has_value = false;
      bool is_alert = false;
      bool is_warning = false;
    } sv[2];

    int sv_count = 0;
    bool node_has_alert = false;
    bool node_has_warning = false;

    for (const String &addr : addrs) {
      if (sv_count >= 2)
        break;

      JsonObject s = sensors[addr].as<JsonObject>();

      SensorView &v = sv[sv_count++];
      v.label = (g_labels["sensors"][addr] | "");
      if (v.label.isEmpty())
        v.label = CanonAddr16(addr);

      if (s["tC"].is<float>()) {
        v.temp_c = s["tC"].as<float>();
        v.has_value = !isnan(v.temp_c);
      }

      if (v.has_value) {
        const float t = v.temp_c;
        if (!isnan(g_alert_low_c) && t < g_alert_low_c)
          v.is_alert = true;
        if (!isnan(g_alert_high_c) && t > g_alert_high_c)
          v.is_alert = true;
        if (!v.is_alert) {
          if (!isnan(g_warn_low_c) && t < g_warn_low_c)
            v.is_warning = true;
          if (!isnan(g_warn_high_c) && t > g_warn_high_c)
            v.is_warning = true;
        }
      }

      if (v.is_alert)
        node_has_alert = true;
      else if (v.is_warning)
        node_has_warning = true;
    }

    // Colors.
    lv_color_t bg = lv_color_make(0x16, 0x3A, 0x24); // dark green
    lv_color_t fg = lv_color_white();                // header/age
    if (node_has_alert) {
      bg = lv_color_make(0xB7, 0x1C, 0x1C);
      fg = lv_color_white();
      g_any_alert = true;
    } else if (is_stale || node_has_warning) {
      bg = lv_color_make(0x8A, 0x5A, 0x00);
      fg = lv_color_white();
      g_any_warning = true;
    } else if (is_missing) {
      bg = lv_color_make(0x20, 0x40, 0x60);
      fg = lv_color_white();
    }

    tw.node_has_alert = node_has_alert;
    tw.node_has_warning = node_has_warning;
    tw.is_missing = is_missing;
    tw.is_stale = is_stale;

    tw.bg_normal = bg;
    tw.fg_normal = fg;

    if (node_has_alert || node_has_warning) {
      tw.bg_flash = lv_color_make(0xCC, 0xCC, 0xCC);
      tw.fg_flash = lv_color_black();
    } else {
      tw.bg_flash = bg;
      tw.fg_flash = fg;
    }

    // Header + age.
    lv_label_set_text(tw.label_loc, loc.c_str());
    lv_obj_align(tw.label_loc, LV_ALIGN_TOP_MID, 0, 0);

    char age_buf[24];
    snprintf(age_buf, sizeof(age_buf), "Age: %lu min",
             static_cast<unsigned long>(age_min));
    lv_label_set_text(tw.label_age, age_buf);
    lv_obj_align(tw.label_age, LV_ALIGN_TOP_MID, 0, 20);

    // Sensor rows.
    tw.sensor_count = sv_count;
    for (int i = 0; i < 2; ++i) {
      lv_obj_t *lbl = tw.sensor_label[i];
      if (!lbl)
        continue;

      if (i >= sv_count) {
        // If the tile truly has no sensors, show a faint placeholder.
        if (sv_count == 0) {
          lv_label_set_text(lbl, "(no sensors)");
          lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
          tw.sensor_color[i] = lv_color_make(0xAA, 0xAA, 0xAA);
          lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, -8);
        } else {
          lv_label_set_text(lbl, "");
          lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
        continue;
      }

      lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);

      const SensorView &v = sv[i];
      char buf[64];
      if (!v.has_value) {
        snprintf(buf, sizeof(buf), "%s: --", v.label.c_str());
      } else {
        float temp_disp = v.temp_c;
        if (g_display_fahrenheit)
          temp_disp = temp_disp * 1.8f + 32.0f;
        const char unit_ch = g_display_fahrenheit ? 'F' : 'C';
        snprintf(buf, sizeof(buf), "%s: %.1f%c", v.label.c_str(),
                 static_cast<double>(temp_disp), unit_ch);
      }
      lv_label_set_text(lbl, buf);

      lv_color_t sensor_color;
      if (!v.has_value)
        sensor_color = lv_color_make(0xCC, 0xCC, 0xCC);
      else if (v.is_alert)
        sensor_color = lv_color_make(0xFF, 0xFF, 0x80);
      else if (v.is_warning)
        sensor_color = lv_color_make(0xFF, 0xD7, 0x00);
      else if (node_has_alert)
        sensor_color = lv_color_make(0xFF, 0xE0, 0xE0);
      else if (is_stale || node_has_warning)
        sensor_color = lv_color_make(0xFF, 0xF7, 0xE0);
      else if (is_missing)
        sensor_color = lv_color_make(0xB0, 0xBE, 0xC5);
      else
        sensor_color = lv_color_make(0xE0, 0xFF, 0xE0);

      tw.sensor_color[i] = sensor_color;

      const int y_offset = (sv_count == 1) ? -8 : (i == 0 ? -26 : -8);
      lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, y_offset);
    }

    // Apply NORMAL (non-flash).
    lv_obj_set_style_bg_color(tw.tile, tw.bg_normal, 0);
    lv_obj_set_style_bg_opa(tw.tile, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tw.label_loc, tw.fg_normal, 0);
    lv_obj_set_style_text_color(tw.label_age, tw.fg_normal, 0);
    for (int i = 0; i < tw.sensor_count && i < 2; ++i) {
      if (tw.sensor_label[i]) {
        lv_obj_set_style_text_color(tw.sensor_label[i], tw.sensor_color[i], 0);
      }
    }
    tw.last_flash_active = false;
  }
}

} // namespace

// -----------------------------------------------------------------------------
// GUI helpers callable from elsewhere
// -----------------------------------------------------------------------------

void GuiUpdateNetwork(size_t peers) {
  g_ui_peers = peers;
  g_gui_dirty = true;
}

void GuiRequestRender() { g_gui_dirty = true; }

// -----------------------------------------------------------------------------
// Display loop: rebuild tiles + buzzer + periodic refresh
// -----------------------------------------------------------------------------

void DisplayLoop() {
  static uint32_t last_build_ms = 0;
  static uint32_t last_force_ms = 0;
  static uint32_t last_flash_ms = 0;

  const uint32_t now_ms = millis();

  // Flashing: only mark flash-dirty, don't trigger full rebuild.
  if (g_flash_interval_ms > 0 && (g_any_alert || g_any_warning)) {
    if (now_ms - last_flash_ms >= g_flash_interval_ms) {
      g_flash_phase = !g_flash_phase;
      g_flash_dirty = true; // <--- instead of g_gui_dirty
      last_flash_ms = now_ms;
    }
  } else {
    if (g_flash_phase) {
      g_flash_phase = false;
      g_flash_dirty = true;
    }
    last_flash_ms = now_ms;
  }

  // Periodic light update for age/status text only (no full rebuild).
  if (now_ms - last_force_ms >= 60000U) {
    if (lvgl_port_lock(-1)) {
      GuiAgeTick(now_ms);
      UpdateUptimeLabel();
      lvgl_port_unlock();
    }
    last_force_ms = now_ms;
  }


  if ((g_gui_dirty || g_flash_dirty) && (now_ms - last_build_ms >= 50U)) {
    if (lvgl_port_lock(-1)) {

      if (g_gui_dirty) {
        g_gui_dirty = false;

        if (g_ui_label_peers != nullptr) {
          char buffer[32];
          snprintf(buffer, sizeof(buffer), "Peers: %u",
                   static_cast<unsigned>(g_ui_peers));
          lv_label_set_text(g_ui_label_peers, buffer);
        }

        GuiRebuildTiles(); // data/layout only
      }

      if (g_flash_dirty) {
        g_flash_dirty = false;
        GuiApplyFlashPhase(); // lightweight color toggle
      }

      UpdateUptimeLabel();
      lvgl_port_unlock();
      last_build_ms = now_ms;
    }
  }
}

void BuzzerLoop() {
  // Static state so we can be called every loop() tick.
  static bool buzzer_on = false;
  static uint32_t beep_start_ms = 0;
  static uint32_t last_beep_ms = 0;

  const uint32_t now_ms = millis();

  // Priority: ALERT > WARNING (warning includes stale/etc via g_any_warning).
  const bool have_alert = g_any_alert;
  const bool have_warning = (!have_alert && g_any_warning);

  uint32_t gap_ms = 0;
  if (have_alert) {
    gap_ms = g_beep_gap_alert_ms;
  } else if (have_warning) {
    gap_ms = g_beep_gap_warn_ms;
  }

  // If no conditions or disabled, ensure buzzer is off and reset schedule.
  if ((!have_alert && !have_warning) || gap_ms == 0 || g_beep_len_ms == 0) {
    if (buzzer_on) {
      digitalWrite(kBuzzerPin, LOW);
      buzzer_on = false;
    }
    last_beep_ms = 0;
    return;
  }

  if (buzzer_on) {
    // Turn off after configured beep length.
    if (now_ms - beep_start_ms >= g_beep_len_ms) {
      digitalWrite(kBuzzerPin, LOW);
      buzzer_on = false;
      last_beep_ms = now_ms;
    }
  } else {
    // Start a beep if it's the first one or the gap has elapsed.
    if (last_beep_ms == 0 || now_ms - last_beep_ms >= gap_ms) {
      digitalWrite(kBuzzerPin, HIGH);
      buzzer_on = true;
      beep_start_ms = now_ms;
    }
  }
}

// -----------------------------------------------------------------------------
// Console command handlers (ROOT)
// Signature required by SerialConsole:
//   void Handler(void* ctx, int argc, const String argv[], Print& out)
// -----------------------------------------------------------------------------

static void CmdHelp(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx; (void)argc; (void)argv;
  g_console.PrintHelp(out);
}

static void CmdLs(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx; (void)argc; (void)argv;
  EnsureDocuments();
  JsonObject nodes = g_last_seen["nodes"].as<JsonObject>();
  for (JsonPair node_entry : nodes) {
    const String node_id = node_entry.key().c_str();
    JsonObject node_obj = node_entry.value();
    const String node_label = g_labels["nodes"][node_id] | "";
    if (node_label.length() > 0) out.printf("node %s \"%s\":\n", node_id.c_str(), node_label.c_str());
    else                         out.printf("node %s:\n", node_id.c_str());
    JsonObject sensors = node_obj["sensors"].as<JsonObject>();
    bool any_sensor = false;
    for (JsonPair sensor_entry : sensors) {
      any_sensor = true;
      const String addr = sensor_entry.key().c_str();
      JsonObject sensor_obj = sensor_entry.value();
      const float temp_c = sensor_obj["tC"] | std::numeric_limits<float>::quiet_NaN();
      const bool corrected = sensor_obj["corr"] | false;
      const String sensor_label = g_labels["sensors"][addr] | "";
      const String addr_canon = CanonAddr16(addr);
      String temp_s = isnan(temp_c) ? String("NaN") : String(temp_c, 2);
      if (sensor_label.length() > 0) {
        out.printf("  %s \"%s\" : %s%s\n", addr_canon.c_str(), sensor_label.c_str(),
                   temp_s.c_str(), corrected ? " (corr)" : "");
      } else {
        out.printf("  %s : %s%s\n", addr_canon.c_str(), temp_s.c_str(),
                   corrected ? " (corr)" : "");
      }
    }
    if (!any_sensor) out.println(F("  (no sensors)"));
  }
  GuiRequestRender();
}

static void CmdNode(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc >= 3 && argv[1] == "rm") {
    const String& id = argv[2];

    // Read-only lookup (won't create a member if missing)
    JsonObjectConst nodes_ro = g_last_seen["nodes"].as<JsonObjectConst>();
    if (!nodes_ro[id].is<JsonObjectConst>()) {
      out.println(F("not found"));
      return;
    }

    // Now mutate safely
    JsonObject nodes = g_last_seen["nodes"].to<JsonObject>();
    nodes.remove(id);
    SaveKnownTopology();
    out.println(F("ok"));
    GuiRequestRender();
    return;
  }

  if (argc < 3) {
    out.println(F("ERR node (usage: node <id> <label...> | node rm <id>)"));
    return;
  }

  // node <id> <label...>  (keep your parser behavior)
  String line;
  for (int i = 0; i < argc; ++i) { if (i) line += ' '; line += argv[i]; }
  String label;
  if (!ExtractLabelAfterSecondToken(line, &label)) {
    out.println(F("ERR node (usage: node <id> <label...>)"));
    return;
  }

  JsonObject nodes = g_last_seen["nodes"].to<JsonObject>();
  g_labels["nodes"][argv[1]] = label;
  SaveLabels();
  out.println(F("ok"));
  GuiRequestRender();
}


static void CmdNodes(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc >= 2 && argv[1] == "clear") {
    EraseKnownTopology();
    SaveKnownTopology();
    out.println(F("ok"));
    GuiRequestRender();
  } else {
    out.println(F("ERR nodes (use: nodes clear)"));
  }
}

static void CmdName(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 3) { out.println(F("ERR name (usage: name <addr16> <label...>)")); return; }
  String line; for (int i = 0; i < argc; ++i) { if (i) line += ' '; line += argv[i]; }
  String label;
  if (!ExtractLabelAfterSecondToken(line, &label)) {
    out.println(F("ERR name (usage: name <addr16> <label...>)"));
    return;
  }
  g_labels["sensors"][argv[1]] = label;
  SaveLabels();
  out.println(F("ok"));
  GuiRequestRender();
}

static void CmdSave(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx; (void)argc; (void)argv; SaveLabels(); out.println(F("saved"));
}
static void CmdLoad(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx; (void)argc; (void)argv; LoadLabels(); out.println(F("loaded")); GuiRequestRender();
}
static void CmdErase(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx; (void)argc; (void)argv; EraseLabels(); out.println(F("erased")); GuiRequestRender();
}

static void CmdDebug(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 2) { out.printf("debug=%s\n", g_debug_enabled ? "on" : "off"); return; }
  g_debug_enabled = (argv[1] == "on");
  out.printf("debug=%s\n", g_debug_enabled ? "on" : "off");
}

static void CmdUnits(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 2) { out.println(F("ERR units (use 'units c' or 'units f')")); return; }
  if (argv[1].equalsIgnoreCase("c")) {
    g_display_fahrenheit = false; SaveDisplayUnits(); out.println(F("units=C")); GuiRequestRender();
  } else if (argv[1].equalsIgnoreCase("f")) {
    g_display_fahrenheit = true;  SaveDisplayUnits(); out.println(F("units=F")); GuiRequestRender();
  } else {
    out.println(F("ERR units (use 'units c' or 'units f')"));
  }
}

static void CmdHighlight(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 3) {
    out.println(F("ERR highlight (use 'highlight missing on|off' or 'highlight stale <min>')"));
    return;
  }
  if (argv[1] == "missing") {
    if (argv[2] == "on")  { g_highlight_missing_nodes = true;  SaveHighlightSettings(); out.println(F("highlight missing=on"));  GuiRequestRender(); }
    else if (argv[2] == "off") { g_highlight_missing_nodes = false; SaveHighlightSettings(); out.println(F("highlight missing=off")); GuiRequestRender(); }
    else out.println(F("ERR highlight missing (use on|off)"));
  } else if (argv[1] == "stale") {
    const long m = argv[2].toInt();
    if (m > 0) { g_stale_minutes_threshold = static_cast<uint32_t>(m); SaveHighlightSettings(); out.printf("highlight stale=%ld min\n", m); GuiRequestRender(); }
    else out.println(F("ERR highlight stale (use positive minutes)"));
  } else {
    out.println(F("ERR highlight (use 'highlight missing on|off' or 'highlight stale <min>')"));
  }
}

static void CmdDummy(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 2) { out.println(F("ERR dummy (use 'dummy on' or 'dummy off')")); return; }
  if (argv[1] == "on")  { g_use_dummy_data = true;  BuildDummyData(); out.println(F("dummy=on"));  GuiRequestRender(); }
  else if (argv[1] == "off") { g_use_dummy_data = false; g_last_seen.clear(); EnsureDocuments(); out.println(F("dummy=off")); GuiRequestRender(); }
  else out.println(F("ERR dummy (use 'dummy on' or 'dummy off')"));
}

static void CmdLimits(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  auto ToC   = [&](float v){ return g_display_fahrenheit ? (v - 32.0f)/1.8f : v; };
  auto FromC = [&](float v){ return g_display_fahrenheit ? (v*1.8f + 32.0f) : v; };
  const char* unit = g_display_fahrenheit ? "F" : "C";
  if (argc >= 2 && argv[1] == "show") {
    out.printf("limits warn=%.2f..%.2f %s\n", FromC(g_warn_low_c),  FromC(g_warn_high_c),  unit);
    out.printf("limits alert=%.2f..%.2f %s\n", FromC(g_alert_low_c), FromC(g_alert_high_c), unit);
    return;
  }
  if (argc >= 4 && (argv[1] == "warn" || argv[1] == "alert")) {
    const float low_in  = argv[2].toFloat();
    const float high_in = argv[3].toFloat();
    if (high_in <= low_in) { out.println(F("ERR limits (high must be > low)")); return; }
    if (argv[1] == "warn")  { g_warn_low_c  = ToC(low_in);  g_warn_high_c  = ToC(high_in);  SaveLimits(); out.printf("limits warn=%.2f..%.2f %s\n",  FromC(g_warn_low_c),  FromC(g_warn_high_c),  unit); }
    else                    { g_alert_low_c = ToC(low_in);  g_alert_high_c = ToC(high_in); SaveLimits(); out.printf("limits alert=%.2f..%.2f %s\n", FromC(g_alert_low_c), FromC(g_alert_high_c), unit); }
    return;
  }
  out.println(F("ERR limits (use 'limits show' | 'limits warn <lo> <hi>' | 'limits alert <lo> <hi>')"));
}

static void CmdBuzzer(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 4) { out.println(F("ERR buzzer (use: buzzer <len_ms> <warn_gap_ms> <alert_gap_ms>, gaps>=0)")); return; }
  long len_ms = argv[1].toInt();
  long gap_warn_ms = argv[2].toInt();
  long gap_alert_ms = argv[3].toInt();
  if (len_ms <= 0 || gap_warn_ms < 0 || gap_alert_ms < 0) {
    out.println(F("ERR buzzer (use: buzzer <len_ms> <warn_gap_ms> <alert_gap_ms>, gaps>=0)"));
    return;
  }
  g_beep_len_ms = (uint32_t)len_ms;
  g_beep_gap_warn_ms = (uint32_t)gap_warn_ms;
  g_beep_gap_alert_ms = (uint32_t)gap_alert_ms;
  SaveBuzzerSettings();
  out.printf("buzzer len=%lu ms warn_gap=%lu ms alert_gap=%lu ms\n",
             (unsigned long)g_beep_len_ms,
             (unsigned long)g_beep_gap_warn_ms,
             (unsigned long)g_beep_gap_alert_ms);
}

static void CmdFlash(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 2) { out.println(F("ERR flash (use 'flash <interval_ms>' or 'flash off')")); return; }
  if (argv[1].equalsIgnoreCase("off")) {
    g_flash_interval_ms = 0; SaveFlashSettings(); g_flash_phase = false; g_gui_dirty = true; out.println(F("flash off"));
  } else {
    long interval = argv[1].toInt();
    if (interval < 0) { out.println(F("ERR flash (interval must be >=0, or 'off')")); return; }
    g_flash_interval_ms = (uint32_t)interval; SaveFlashSettings(); g_flash_phase = false; g_gui_dirty = true;
    out.printf("flash interval=%lu ms (%s)\n", (unsigned long)g_flash_interval_ms, g_flash_interval_ms ? "enabled" : "off");
  }
}

static void CmdOrder(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  EnsureDocuments();
  JsonObject ord = g_labels["order"].to<JsonObject>();
  if (argc >= 2 && argv[1] == "list") {
    bool any = false;
    for (JsonPair p : ord) { any = true; out.printf("%s : %d\n", p.key().c_str(), p.value().as<int>()); }
    if (!any) out.println(F("(empty)"));
    out.println(F("ok"));
    return;
  }
  if (argc >= 3 && argv[1] == "clear") {
    const String key = CanonAddr16(argv[2]); ord.remove(key); SaveLabels(); out.println(F("ok")); GuiRequestRender(); return;
  }
  if (argc >= 3) {
    const String key = CanonAddr16(argv[1]); const int rank = argv[2].toInt();
    ord[key] = rank; SaveLabels(); out.println(F("ok")); GuiRequestRender(); return;
  }
  out.println(F("ERR order (use 'order <addr16> <rank>' | 'order clear <addr16>' | 'order list')"));
}

static void CmdNorder(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  EnsureDocuments();
  JsonObject ord = g_labels["tile_order"].to<JsonObject>();
  if (argc >= 2 && argv[1] == "list") {
    for (JsonPair p : ord) out.printf("%s : %d\n", p.key().c_str(), p.value().as<int>());
    out.println(F("ok"));
    return;
  }
  if (argc >= 3 && argv[1] == "clear") {
    ord.remove(argv[2]); SaveLabels(); out.println(F("ok")); GuiRequestRender(); return;
  }
  if (argc >= 3) {
    const String node_id = argv[1]; const int rank = argv[2].toInt();
    ord[node_id] = rank; SaveLabels(); out.println(F("ok")); GuiRequestRender(); return;
  }
  out.println(F("ERR norder (use 'norder <nodeId> <rank>' | 'norder clear <nodeId>' | 'norder list')"));
}

static void CmdSorder(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  EnsureDocuments();
  JsonObject sroot = g_labels["sorder"].to<JsonObject>();
  if (argc >= 2 && argv[1] == "list") {
    if (argc >= 3) {
      const String node_id = argv[2];
      JsonObject m = sroot[node_id].as<JsonObject>();
      if (m.isNull()) out.println(F("(empty)")); else {
        bool any = false;
        for (JsonPair p : m) { any = true; out.printf("%s : %d\n", p.key().c_str(), p.value().as<int>()); }
        if (!any) out.println(F("(empty)"));
      }
      out.println(F("ok"));
      return;
    }
    bool any_node = false;
    for (JsonPair node_pair : sroot) {
      any_node = true;
      out.printf("[%s]\n", node_pair.key().c_str());
      JsonObject m = node_pair.value();
      bool any = false;
      for (JsonPair p : m) { any = true; out.printf("  %s : %d\n", p.key().c_str(), p.value().as<int>()); }
      if (!any) out.println(F("  (empty)"));
    }
    if (!any_node) out.println(F("(empty)"));
    out.println(F("ok"));
    return;
  }
  if (argc >= 4 && argv[1] == "clear") {
    const String node_id = argv[2];
    const String key = CanonAddr16(argv[3]);
    JsonObject m = sroot[node_id].to<JsonObject>(); m.remove(key); SaveLabels(); out.println(F("ok")); GuiRequestRender(); return;
  }
  if (argc >= 4) {
    const String node_id = argv[1];
    const String key = CanonAddr16(argv[2]);
    const int rank = argv[3].toInt();
    JsonObject m = sroot[node_id].to<JsonObject>(); m[key] = rank; SaveLabels(); out.println(F("ok")); GuiRequestRender(); return;
  }
  out.println(F("ERR sorder (use 'sorder <nodeId> <addr16> <rank>' | 'sorder clear <nodeId> <addr16>' | 'sorder list [nodeId]')"));
}

// -----------------------------------------------------------------------------
// Root announce and console
// -----------------------------------------------------------------------------

void RootAnnounce() {
  JsonDocument doc;
  doc["type"] = "root_announce";
  doc["rootId"] = mesh.getNodeId();

  String message;
  serializeJson(doc, message);

  DLOG("[ROOT TX announce] %s\n", message.c_str());
  mesh.sendBroadcast(message);
}

// -----------------------------------------------------------------------------
// Mesh callbacks (root)
// -----------------------------------------------------------------------------

void OnReceiveRoot(uint32_t from, String &msg) {

  bool topology_changed = false;

  DLOG("[ROOT RX] from=%u len=%u: %s\n", from,
       static_cast<unsigned>(msg.length()), msg.c_str());

  if (g_use_dummy_data) {
    // In dummy mode, ignore real traffic to keep UI deterministic.
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) {
    DLOG("  ! JSON parse error\n");
    return;
  }

  const char *type = doc["type"] | "temps";
  if (strcmp(type, "temps") != 0) {
    DLOG("  ! ignoring type '%s'\n", type);
    return;
  }

  EnsureDocuments();

  const uint32_t node_id = doc["nodeId"] | from;
  const int bus_gpio = doc["busGpio"] | -1;

  JsonObject nodes = g_last_seen["nodes"].to<JsonObject>();
  bool node_was_new = nodes[String(node_id)].isNull();
  JsonObject node_obj = nodes[String(node_id)].to<JsonObject>();
  if (node_was_new)
    topology_changed = true;
  node_obj["busGpio"] = bus_gpio;
  node_obj["last"] = static_cast<uint32_t>(millis());

  const String node_id_str = String(node_id);
  GuiUpdateNodeSummary(node_id_str.c_str(), bus_gpio,
                       static_cast<uint32_t>(millis()));

  JsonObject sensors = node_obj["sensors"].to<JsonObject>();
  JsonArray sensor_array = doc["sensors"].as<JsonArray>();

  DLOG("  nodeId=%u bus_gpio=%d sensors=%d\n", node_id, bus_gpio,
       sensor_array.size());

  for (JsonObject sensor_in : sensor_array) {
    const char *addr = sensor_in["addr"] | "";
    if (addr == nullptr || strlen(addr) != 16) {
      continue;
    }

    JsonObject prev = sensors[addr];
    const bool sensor_was_new = prev.isNull();
    JsonObject sensor_out = sensors[addr].to<JsonObject>();
    if (sensor_was_new)
      topology_changed = true;

    if (sensor_in["tC"].is<float>()) {
      sensor_out["tC"] = sensor_in["tC"].as<float>();
    }
    if (sensor_in["corr"].is<bool>()) {
      sensor_out["corr"] = sensor_in["corr"].as<bool>();
    }
    sensor_out["last"] = static_cast<uint32_t>(millis());

    const float temp_c = sensor_out["tC"] | NAN;
    const float temp_f = isnan(temp_c) ? NAN : (1.8f * temp_c + 32.0f);
    const bool corrected = sensor_out["corr"] | false;
    const char *label = g_labels["sensors"][addr] | "";

    DLOG("    [%s]%s = %s%s\n", addr,
         (label != nullptr && strlen(label) > 0)
             ? (String(" \"") + label + "\"").c_str()
             : "",
         isnan(temp_f) ? "NaN" : String(temp_f, 2).c_str(),
         corrected ? " (corr)" : "");

    GuiUpdateSensorRow(node_id_str.c_str(), addr, temp_f,
                       (label != nullptr && strlen(label) > 0) ? label
                                                               : nullptr,
                       static_cast<uint32_t>(millis()));
  }

  if (topology_changed) {
    SaveKnownTopology();
  }

  GuiRequestRender();
}

void OnConnectionsChangedRoot() {
  LogConnections();

  if (g_use_dummy_data) {
    GuiRequestRender();
    return;
  }

  EnsureDocuments();
  JsonObject nodes = g_last_seen["nodes"].to<JsonObject>();
  const uint32_t now_ms = millis();

  bool topology_changed = false;

  // Ensure each connected node has an entry.
  for (const auto &node_id : mesh.getNodeList()) {
    const String key = String(node_id);

    JsonObject node_obj = nodes[key];
    if (node_obj.isNull()) {
      node_obj = nodes[key].to<JsonObject>();
      topology_changed = true;
    }
    if (!node_obj["last"].is<uint32_t>()) {
      node_obj["last"] = now_ms;
    }
    if (!node_obj["sensors"].is<JsonObject>()) {
      node_obj["sensors"].to<JsonObject>();
    }
  }

  if (topology_changed) {
    SaveKnownTopology();
  }

  // Do not delete nodes; they age out visually.
  GuiRequestRender();
}

// -----------------------------------------------------------------------------
// Arduino entry points (root)
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(DBG_BAUD);
  delay(200);

  g_last_seen.clear();
  g_labels.clear();

  MigrateStorageIfNeeded();
  EnsureDocuments();

  // NEW: pre-populate tiles from persisted topology
  LoadKnownTopology();

  DisplayInit();
  GuiInit();
  GuiUpdateNetwork(0);
  GuiRequestRender();

  pinMode(kBuzzerPin, OUTPUT);
  digitalWrite(kBuzzerPin, LOW);

  g_console.RegisterCommand("help",   &CmdHelp,      "show help");
  g_console.RegisterCommand("ls",     &CmdLs,        "list nodes/sensors");
  g_console.RegisterCommand("node",   &CmdNode,      "label or rm a node");
  g_console.RegisterCommand("nodes",  &CmdNodes,     "nodes clear");
  g_console.RegisterCommand("name",   &CmdName,      "label a sensor");
  g_console.RegisterCommand("save",   &CmdSave,      "save labels");
  g_console.RegisterCommand("load",   &CmdLoad,      "load labels");
  g_console.RegisterCommand("erase",  &CmdErase,     "erase labels");
  g_console.RegisterCommand("debug",  &CmdDebug,     "debug on|off");
  g_console.RegisterCommand("units",  &CmdUnits,     "units c|f");
  g_console.RegisterCommand("highlight", &CmdHighlight, "highlight missing on|off | highlight stale <min>");
  g_console.RegisterCommand("dummy",  &CmdDummy,     "dummy on|off");
  g_console.RegisterCommand("limits", &CmdLimits,    "limits show | warn <lo> <hi> | alert <lo> <hi>");
  g_console.RegisterCommand("buzzer", &CmdBuzzer,    "buzzer <len_ms> <warn_gap_ms> <alert_gap_ms>");
  g_console.RegisterCommand("flash",  &CmdFlash,     "flash <ms>|off");
  g_console.RegisterCommand("order",  &CmdOrder,     "sensor global order");
  g_console.RegisterCommand("norder", &CmdNorder,    "tile order");
  g_console.RegisterCommand("sorder", &CmdSorder,    "per-node sensor order");


  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &user_scheduler, MESH_PORT);
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

  Serial.println(F("ROOT ready. Type 'help'."));
}

void loop() {
  mesh.update();
  DisplayLoop();
  BuzzerLoop();
  g_console.Poll(Serial, Serial);

}

// -----------------------------------------------------------------------------
// LEAF BUILD
// -----------------------------------------------------------------------------
#else // !MESH_IS_ROOT

#include <DallasTemperature.h>
#include <OneWire.h>
#include <Preferences.h>
#include <map>

std::vector<Address> g_devices;

OneWire g_one_wire(ONEWIRE_PIN);
DallasTemperature g_ds18(&g_one_wire);

uint32_t g_root_id = 0;
uint32_t g_root_last_seen_ms = 0;

Task g_task_send;

// Calibration storage (NVS).
struct CalEntry {
  String addr;
  Coeff coeff;
};

std::vector<CalEntry> g_cal_entries;
Preferences g_leaf_prefs;

// Per-sensor work-in-progress (raw locks while calibrating).
struct CalPoint {
  float last_raw = NAN; // last live reading during an active stage
  bool ice_locked = false;
  float raw_ice = 0.0f;
  float act_ice = 0.0f;
  bool boil_locked = false;
  float raw_boil = 0.0f;
  float act_boil = 0.0f;

  // NEW: stability tracking (per active calibration session)
  uint32_t last_change_ms = 0; // when the reading last changed
  bool steady = false;         // true if unchanged for >= kSteadyMs
};

// All WIP points keyed by sensor address (hex16)
std::map<String, CalPoint> g_cal_work;

// Live calibration session: a stage plus a set of active sensors.
struct CalSession {
  CalStage stage = kCalIdle;        // kCalIdle, kCalIce, kCalBoil
  std::vector<String> active_addrs; // hex16 addresses currently being tracked
};

CalSession g_cal_session;
Task g_task_cal;

// Steady-state detection: unchanged reading for >=30s at 0.001 C tolerance.
constexpr uint32_t kSteadyMs = 30000; // ms
constexpr float kSteadyEpsC = 0.001f; // °C

namespace {

bool FindDeviceByAddr(const String &addr16, Address *out_addr) {
  if (addr16.length() != 16) {
    return false;
  }
  for (const auto &address : g_devices) {
    if (addr16.equalsIgnoreCase(addrToHex(address.data()))) {
      *out_addr = address;
      return true;
    }
  }
  return false;
}

int FindCalIndex(const String &addr) {
  for (size_t i = 0; i < g_cal_entries.size(); ++i) {
    if (g_cal_entries[i].addr.equalsIgnoreCase(addr)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void SetCal(const String &addr, const Coeff &coeff) {
  const int index = FindCalIndex(addr);
  if (index >= 0) {
    g_cal_entries[static_cast<size_t>(index)].coeff = coeff;
  } else {
    g_cal_entries.push_back({addr, coeff});
  }
}

bool IsIdentity(const Coeff &coeff) {
  return (fabsf(coeff.a1 - 1.0f) < 1e-6f) && (fabsf(coeff.a0) < 1e-6f);
}

float ApplyCorrection(float temp_raw_c, const String &addr16,
                      bool *out_corrected) {
  const int index = FindCalIndex(addr16);
  if (index < 0) {
    if (out_corrected != nullptr) {
      *out_corrected = false;
    }
    return temp_raw_c;
  }

  const Coeff &coeff = g_cal_entries[static_cast<size_t>(index)].coeff;
  const float temp_corr = coeff.a1 * temp_raw_c + coeff.a0;

  if (out_corrected != nullptr) {
    *out_corrected = !IsIdentity(coeff);
  }
  return temp_corr;
}

// Persistence (leaf).
void SaveAllCalibration() {
  g_leaf_prefs.begin("leafcal", /*readOnly=*/false);

  JsonDocument index_doc;
  JsonArray index_array = index_doc.to<JsonArray>();
  for (const auto &entry : g_cal_entries) {
    index_array.add(entry.addr);
  }

  String index_json;
  serializeJson(index_doc, index_json);
  g_leaf_prefs.putString("index", index_json);

  for (const auto &entry : g_cal_entries) {
    char key[32];
    snprintf(key, sizeof(key), "c_%s", entry.addr.c_str());

    const Coeff &c = entry.coeff;
    char value[64];
    snprintf(value, sizeof(value), "%.8f,%.8f", static_cast<double>(c.a1),
             static_cast<double>(c.a0));
    g_leaf_prefs.putString(key, value);
  }

  g_leaf_prefs.end();
}

void LoadAllCalibration() {
  g_cal_entries.clear();

  g_leaf_prefs.begin("leafcal", /*readOnly=*/true);
  const String index_json = g_leaf_prefs.getString("index", "");

  if (!index_json.isEmpty()) {
    JsonDocument index_doc;
    if (deserializeJson(index_doc, index_json) == DeserializationError::Ok) {
      for (JsonVariant value : index_doc.as<JsonArray>()) {
        const String addr = value.as<const char *>();

        char key[32];
        snprintf(key, sizeof(key), "c_%s", addr.c_str());

        const String cal_str = g_leaf_prefs.getString(key, "");
        if (!cal_str.isEmpty()) {
          Coeff coeff{1.0f, 0.0f};
          float a1 = 1.0f;
          float a0 = 0.0f;
          float legacy_a2 = 0.0f;

          int parsed = sscanf(cal_str.c_str(), "%f,%f", &a1, &a0);
          if (parsed != 2) {
            parsed = sscanf(cal_str.c_str(), "%f,%f,%f", &legacy_a2, &a1, &a0);
          }
          if (parsed >= 2) {
            coeff.a1 = a1;
            coeff.a0 = a0;
            SetCal(addr, coeff);
          }
        }
      }
    }
  }

  g_leaf_prefs.end();
}

void ClearCalibration(const String &addr) {
  const int index = FindCalIndex(addr);
  if (index >= 0) {
    g_cal_entries.erase(g_cal_entries.begin() + static_cast<size_t>(index));
  }
  SaveAllCalibration();
}

// Collect a vector of all currently attached sensor addresses (hex16).
static std::vector<String> AllAttachedAddr16() {
  std::vector<String> out;
  out.reserve(g_devices.size());
  for (const auto &addr : g_devices) {
    out.push_back(addrToHex(addr.data()));
  }
  return out;
}

// Start a session for a provided list of addresses (must be non-empty).
static void CalibrationStartFor(const std::vector<String> &addrs,
                                CalStage stage) {
  if (addrs.empty()) {
    Serial.println(F("CAL ERROR: no sensors"));
    return;
  }

  g_cal_session.stage = stage;
  g_cal_session.active_addrs = addrs;

  // Ensure work slots exist.
  for (const auto &a : g_cal_session.active_addrs) {
    (void)g_cal_work[a]; // default-construct if missing
  }

  for (const auto &a : g_cal_session.active_addrs) {
    CalPoint &cp = g_cal_work[a]; // default-construct if missing
    cp.last_raw = NAN;            // force first sample to "change"
    cp.last_change_ms = millis();
    cp.steady = false;
  }

  const char *st = (stage == kCalIce) ? "ICE" : "BOIL";
  if (addrs.size() == 1) {
    Serial.printf("CAL %s started for %s\n", st, addrs[0].c_str());
  } else {
    Serial.printf("CAL %s started for %u sensors\n", st,
                  static_cast<unsigned>(addrs.size()));
  }

  g_task_cal.enableIfNot();
}

// Overload: start for a single address (validates it exists).
static void CalibrationStart(const String &addr16, CalStage stage) {
  if (addr16.length() != 16) {
    Serial.println(F("ERR addr16"));
    return;
  }
  Address dummy;
  if (!FindDeviceByAddr(addr16, &dummy)) {
    Serial.println(F("CAL ERROR: sensor not found"));
    return;
  }
  CalibrationStartFor(std::vector<String>{addr16}, stage);
}

// Start for all attached sensors.
static void CalibrationStartAll(CalStage stage) {
  auto all = AllAttachedAddr16();
  CalibrationStartFor(all, stage);
}

// Lock current stage for all active sensors with a single actual reference.
static void CalibrationLockAll(float actual_c) {
  if (g_cal_session.stage == kCalIdle) {
    Serial.println(F("CAL idle"));
    return;
  }

  const bool is_ice = (g_cal_session.stage == kCalIce);
  const char *tag = is_ice ? "ICE" : "BOIL";

  size_t locked = 0;
  for (const auto &addr : g_cal_session.active_addrs) {
    CalPoint &cp = g_cal_work[addr];
    if (isnan(cp.last_raw)) {
      Serial.printf("CAL %s %s : no reading yet\n", tag, addr.c_str());
      continue;
    }
    if (is_ice) {
      cp.raw_ice = cp.last_raw;
      cp.act_ice = actual_c;
      cp.ice_locked = true;
    } else {
      cp.raw_boil = cp.last_raw;
      cp.act_boil = actual_c;
      cp.boil_locked = true;
    }
    ++locked;
    Serial.printf("CAL %s %s : raw=%.3fC locked (act=%.3fC)\n", tag,
                  addr.c_str(), static_cast<double>(cp.last_raw),
                  static_cast<double>(actual_c));
  }

  Serial.printf("CAL %s locks captured: %u\n", tag,
                static_cast<unsigned>(locked));

  // Stop live sampling until next start.
  g_cal_session.stage = kCalIdle;
  g_cal_session.active_addrs.clear();
  g_task_cal.disable();
}

// Solve and save coefficients for every sensor with both points captured.
static void CalibrationSolveAndSaveAll() {
  size_t saved = 0;
  for (auto &kv : g_cal_work) {
    const String &addr = kv.first;
    CalPoint &cp = kv.second;

    if (!(cp.ice_locked && cp.boil_locked))
      continue;

    const float x1 = cp.raw_ice, y1 = cp.act_ice;
    const float x2 = cp.raw_boil, y2 = cp.act_boil;

    if (fabsf(x2 - x1) < 1e-4f) {
      Serial.printf("CAL ERROR %s: identical raw points\n", addr.c_str());
      continue;
    }

    Coeff coeff;
    coeff.a1 = (y2 - y1) / (x2 - x1);
    coeff.a0 = y1 - coeff.a1 * x1;

    SetCal(addr, coeff);
    ++saved;

    Serial.printf("CAL SAVED %s : a1=%.6f a0=%.6f\n", addr.c_str(),
                  static_cast<double>(coeff.a1), static_cast<double>(coeff.a0));
    Serial.printf(
        "  check: ice  raw=%.3f -> %.3f (want %.3f)\n", static_cast<double>(x1),
        static_cast<double>(coeff.a1 * x1 + coeff.a0), static_cast<double>(y1));
    Serial.printf(
        "  check: boil raw=%.3f -> %.3f (want %.3f)\n", static_cast<double>(x2),
        static_cast<double>(coeff.a1 * x2 + coeff.a0), static_cast<double>(y2));

    // Clear locks after saving so user can re-run later if desired.
    cp.ice_locked = false;
    cp.boil_locked = false;
  }

  if (saved > 0) {
    SaveAllCalibration();
    Serial.printf("CAL saved %u sensor(s)\n", static_cast<unsigned>(saved));
  } else {
    Serial.println(F("CAL nothing to save (need both ICE and BOIL)"));
  }
}

// Live sampling task: print each active sensor once per second during ICE/BOIL.
void CalibrationTaskFn() {
  if (g_cal_session.stage == kCalIdle || g_cal_session.active_addrs.empty()) {
    g_task_cal.disable();
    return;
  }

  // Trigger conversions once for all sensors.
  g_ds18.requestTemperatures();

  const bool is_ice = (g_cal_session.stage == kCalIce);
  const char *tag = is_ice ? "ICE" : "BOIL";
  const uint32_t now_ms = millis();

  // Build one concise status line for all active sensors.
  String line;
  line.reserve(128);
  line += "CAL ";
  line += tag;
  line += ": ";

  for (size_t i = 0; i < g_cal_session.active_addrs.size(); ++i) {
    const String &addr16 = g_cal_session.active_addrs[i];

    Address addr;
    if (!FindDeviceByAddr(addr16, &addr)) {
      // Print short id + missing marker.
      const String short_id =
          addr16.substring(max(0, (int)addr16.length() - 4));
      line += short_id;
      line += "=NaN";
      if (i + 1 < g_cal_session.active_addrs.size())
        line += "  ";
      continue;
    }

    const float t_c =
        g_ds18.getTempC(reinterpret_cast<const uint8_t *>(addr.data()));

    CalPoint &cp = g_cal_work[addr16];

    // Update stability state.
    bool changed = false;
    if (isnan(cp.last_raw) && !isnan(t_c)) {
      changed = true;
    } else if (!isnan(cp.last_raw) && !isnan(t_c)) {
      if (fabsf(t_c - cp.last_raw) > kSteadyEpsC) {
        changed = true;
      }
    } else if (isnan(t_c) != isnan(cp.last_raw)) {
      changed = true;
    }

    if (changed) {
      cp.last_raw = t_c;
      cp.last_change_ms = now_ms;
      cp.steady = false;
    } else {
      if (now_ms - cp.last_change_ms >= kSteadyMs) {
        cp.steady = true;
      }
    }

    // Append compact display: last 4 hex chars + value [+* if steady]
    const String short_id = addr16.substring(max(0, (int)addr16.length() - 4));
    line += short_id;
    line += '=';

    if (!isnan(t_c)) {
      // Match prior precision (3 decimals).
      line += String(t_c, 3);
      if (cp.steady) {
        line += '*';
      }
    } else {
      line += "NaN";
    }

    if (i + 1 < g_cal_session.active_addrs.size()) {
      line += "  ";
    }
  }

  Serial.println(line);

  // Run at ~1 Hz as before.
  g_task_cal.delay(1000);
}

void ScanSensors() {
  g_devices.clear();
  g_ds18.begin();

  DeviceAddress raw;
  const int count = g_ds18.getDeviceCount();
  DLOG("[LEAF] scanning DS18B20 on GPIO %d: found=%d\n", ONEWIRE_PIN, count);

  for (int i = 0; i < count; ++i) {
    if (g_ds18.getAddress(raw, i)) {
      g_devices.emplace_back();
      memcpy(g_devices.back().data(), raw, 8);
      DLOG("  addr[%d]=%s\n", i, addrToHex(g_devices.back().data()).c_str());
    }
  }

  if (g_devices.empty()) {
    DLOG("  (no sensors)\n");
  }
}

// -----------------------------------------------------------------------------
// Console command handlers (LEAF)
// -----------------------------------------------------------------------------

static void CmdHelp(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx; (void)argc; (void)argv; g_console.PrintHelp(out);
}

static void CmdDebug(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 2) { out.printf("debug=%s\n", g_debug_enabled ? "on" : "off"); return; }
  g_debug_enabled = (argv[1] == "on");
  out.printf("debug=%s\n", g_debug_enabled ? "on" : "off");
}

static void CmdScan(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx; (void)argc; (void)argv; ScanSensors(); out.println(F("ok"));
}

static void CmdSendNow(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx; (void)argc; (void)argv; SendTemperatures(); out.println(F("ok"));
}

static void CmdBoilPt(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 2) { out.println(F("ERR boilpt <inHg>")); return; }
  float inHg = argv[1].toFloat();
  float TbC = boilingPointC_fromInHg(inHg);
  if (isnan(TbC)) { out.println(F("ERR invalid pressure")); return; }
  float TbF = TbC * 9.0f / 5.0f + 32.0f;
  out.printf("Boiling point at %.3f inHg: %.3f C (%.3f F)\n",
             (double)inHg, (double)TbC, (double)TbF);
}

static void CmdCal(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  if (argc < 2) {
    out.println(
      F("cal cmds:\n"
        "  cal ice [addr16]\n"
        "  cal boil [addr16]\n"
        "  cal lock <actualC>\n"
        "  cal solve\n"
        "  cal list\n"
        "  cal show [addr16]\n"
        "  cal clear <addr16>\n"
        "  cal save | cal load"));
    return;
  }
  const String sub = argv[1];
  if (sub == "ice")  { if (argc >= 3) CalibrationStart(argv[2], kCalIce);  else CalibrationStartAll(kCalIce);  out.println(F("ok")); return; }
  if (sub == "boil") { if (argc >= 3) CalibrationStart(argv[2], kCalBoil); else CalibrationStartAll(kCalBoil); out.println(F("ok")); return; }
  if (sub == "lock") { if (argc < 3) { out.println(F("ERR cal lock <actualC>")); return; } CalibrationLockAll(argv[2].toFloat()); out.println(F("ok")); return; }
  if (sub == "solve"){ CalibrationSolveAndSaveAll(); out.println(F("ok")); return; }
  if (sub == "list") {
    for (const auto &e : g_cal_entries) { const Coeff &c = e.coeff; out.printf("%s : a1=%.6f a0=%.6f\n", e.addr.c_str(), (double)c.a1, (double)c.a0); }
    return;
  }
  if (sub == "show") {
    if (argc >= 3) {
      const int idx = FindCalIndex(argv[2]);
      if (idx >= 0) { const Coeff &c = g_cal_entries[(size_t)idx].coeff; out.printf("%s : a1=%.6f a0=%.6f\n", argv[2].c_str(), (double)c.a1, (double)c.a0); }
      else out.println(F("not found"));
    } else {
      for (const auto &e : g_cal_entries) { const Coeff &c = e.coeff; out.printf("%s : a1=%.6f a0=%.6f\n", e.addr.c_str(), (double)c.a1, (double)c.a0); }
    }
    return;
  }
  if (sub == "clear") { if (argc < 3) { out.println(F("ERR cal clear <addr16>")); return; } ClearCalibration(argv[2]); out.println(F("ok")); return; }
  if (sub == "save")  { SaveAllCalibration(); out.println(F("saved")); return; }
  if (sub == "load")  { LoadAllCalibration(); out.println(F("loaded")); return; }
  out.println(F("ERR cal (type just 'cal' for help)"));
}

void SendTemperatures() {
  g_ds18.requestTemperatures();

  JsonDocument doc;
  doc["type"] = "temps";
  doc["nodeId"] = mesh.getNodeId();
  doc["busGpio"] = ONEWIRE_PIN;
  doc["uptimeMs"] = static_cast<uint32_t>(millis());

  JsonArray sensors = doc["sensors"].to<JsonArray>();

  for (const auto &address : g_devices) {
    const String addr16 = addrToHex(address.data());
    const float temp_raw_c =
        g_ds18.getTempC(reinterpret_cast<const uint8_t *>(address.data()));

    bool corrected = false;
    const float temp_out_c =
        (!isnan(temp_raw_c) && temp_raw_c > -100.0f)
            ? ApplyCorrection(temp_raw_c, addr16, &corrected)
            : NAN;

    JsonObject sensor_obj = sensors.add<JsonObject>();
    sensor_obj["addr"] = addr16;
    if (!isnan(temp_out_c)) {
      sensor_obj["tC"] = temp_out_c;
      sensor_obj["corr"] = corrected;
    }

    if (!isnan(temp_raw_c)) {
      DLOG("[LEAF MEAS] %s raw=%.2fC out=%.2fC%s\n", addr16.c_str(), temp_raw_c,
           temp_out_c, corrected ? " (corr)" : "");
    } else {
      DLOG("[LEAF MEAS] %s = (disconnected)\n", addr16.c_str());
    }
  }

  String message;
  serializeJson(doc, message);

  const bool use_unicast =
      (g_root_id != 0U) && ((millis() - g_root_last_seen_ms) < 15000U);

  DLOG("[LEAF TX %s] %s\n", use_unicast ? "unicast" : "bcast", message.c_str());

  if (use_unicast) {
    mesh.sendSingle(g_root_id, message);
  } else {
    mesh.sendBroadcast(message);
  }
}

// Approximate boiling point of water [°C] from station pressure in inches of
// Hg. Uses Antoine equation (valid ~1–100°C): log10(P_mmHg) = A - B / (C + T)
// A=8.07131, B=1730.63, C=233.426  (water)
static float boilingPointC_fromInHg(float inHg) {
  if (inHg <= 0.0f)
    return NAN;

  const float P_mmHg = inHg * 25.4f; // 1 inHg = 25.4 mmHg
  if (P_mmHg <= 0.0f)
    return NAN;

  const float A = 8.07131f;
  const float B = 1730.63f;
  const float C = 233.426f;

  float logP = log10f(P_mmHg);
  float denom = A - logP;
  if (denom == 0.0f)
    return NAN;

  float T = B / denom - C; // °C
  return T;
}

// painlessMesh callbacks (leaf).
void OnReceiveLeaf(uint32_t from, String &msg) {
  DLOG("[LEAF RX] from=%u len=%u: %s\n", from,
       static_cast<unsigned>(msg.length()), msg.c_str());

  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) {
    DLOG("  ! JSON parse error\n");
    return;
  }

  const char *type = doc["type"] | "";
  if (strcmp(type, "root_announce") == 0) {
    const uint32_t root_id = doc["rootId"] | 0U;
    if (root_id != 0U) {
      g_root_id = root_id;
      g_root_last_seen_ms = millis();
      DLOG("  root_announce: rootId=%u\n", g_root_id);
    }
  }
}

void OnConnectionsChangedLeaf() { LogConnections(); }

} // namespace

// Arduino entry points (leaf)
void setup() {
  Serial.begin(DBG_BAUD);
  delay(1000);

  g_ds18.begin();
  LoadAllCalibration();
  ScanSensors();

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &user_scheduler, MESH_PORT);
  mesh.setContainsRoot(true);
  mesh.onReceive(&OnReceiveLeaf);
  mesh.onChangedConnections(&OnConnectionsChangedLeaf);

  g_console.RegisterCommand("help",   &CmdHelp,    "show help");
  g_console.RegisterCommand("debug",  &CmdDebug,   "debug on|off");
  g_console.RegisterCommand("scan",   &CmdScan,    "enumerate DS18B20");
  g_console.RegisterCommand("sendnow",&CmdSendNow, "send temperatures now");
  g_console.RegisterCommand("boilpt", &CmdBoilPt,  "boilpt <inHg>");
  g_console.RegisterCommand("cal",    &CmdCal,     "calibration commands");

  g_task_send.set(TASK_IMMEDIATE, TASK_FOREVER, []() {
    SendTemperatures();
    g_task_send.delay(SEND_PERIOD_MS);
  });
  user_scheduler.addTask(g_task_send);
  g_task_send.enable();

  g_task_cal.set(TASK_IMMEDIATE, TASK_FOREVER, CalibrationTaskFn);
  user_scheduler.addTask(g_task_cal);
  g_task_cal.disable(); // Enabled by CalibrationStart().

  Serial.println(F("LEAF ready. Type 'help'."));
}

void loop() {
  mesh.update();
  g_console.Poll(Serial, Serial);
}

#endif // MESH_IS_ROOT
