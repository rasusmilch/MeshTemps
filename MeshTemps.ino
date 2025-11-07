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

#include <array>
#include <vector>

using Address = std::array<uint8_t, 8>;  // DS18B20 64-bit ROM code

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

#include "Config.h"  // Mesh / IO configuration, addrToHex()

// -----------------------------------------------------------------------------
// Shared mesh and logging
// -----------------------------------------------------------------------------

Scheduler user_scheduler;
painlessMesh mesh;

bool g_debug_enabled = true;

#define DLOG(fmt, ...)                                       \
  do {                                                       \
    if (g_debug_enabled) {                                   \
      Serial.printf((fmt), ##__VA_ARGS__);                   \
    }                                                        \
  } while (0)

// Forward declarations used on ROOT builds from LogConnections().
#if MESH_IS_ROOT
void GuiUpdateNetwork(size_t peers);
void GuiRequestRender();
#endif  // MESH_IS_ROOT

namespace {

void LogConnections() {
  const size_t peer_count = mesh.getNodeList().size();
  DLOG("Peers: %u\n", static_cast<unsigned int>(peer_count));

#if MESH_IS_ROOT
  GuiUpdateNetwork(peer_count);
  GuiRequestRender();
#endif  // MESH_IS_ROOT
}

}  // namespace

// -----------------------------------------------------------------------------
// ROOT BUILD
// -----------------------------------------------------------------------------
#if MESH_IS_ROOT

#include <Preferences.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "esp_panel_board_custom_conf.h"
#include <map>
#include <limits>

using esp_panel::board::Board;
using esp_panel::drivers::BusRGB;
using esp_panel::drivers::LCD;

constexpr int kStorageVersionCurrent = 5;

// Buzzer (root)
constexpr int kBuzzerPin = 6;
constexpr uint32_t kDefaultBeepLenMs      = 150;    // ms buzzer ON
constexpr uint32_t kDefaultGapWarnMs      = 10000;  // ms between warning beeps
constexpr uint32_t kDefaultGapAlertMs     = 5000;   // ms between alert beeps

uint32_t g_beep_len_ms       = kDefaultBeepLenMs;
uint32_t g_beep_gap_warn_ms  = kDefaultGapWarnMs;
uint32_t g_beep_gap_alert_ms = kDefaultGapAlertMs;

// Flashing (root)
constexpr uint32_t kDefaultFlashIntervalMs = 5000;  // ms between flash toggles; 0 = off
uint32_t g_flash_interval_ms = kDefaultFlashIntervalMs;
bool g_flash_phase = false;  // toggled in DisplayLoop()

// Persistent root prefs.
Preferences g_root_preferences;

// Last-seen data and labels (persisted labels, volatile last_seen).
JsonDocument g_last_seen;
JsonDocument g_labels;

// Mesh tasks.
Task g_task_announce;

// Units / highlight config.
bool g_display_fahrenheit = true;          // true = °F, false = °C
bool g_highlight_missing_nodes = true;
uint32_t g_stale_minutes_threshold = 5;   // minutes

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
lv_obj_t* g_ui_label_title = nullptr;
lv_obj_t* g_ui_label_peers = nullptr;
lv_obj_t* g_ui_label_uptime = nullptr;
lv_obj_t* g_ui_tile_container = nullptr;

volatile bool g_gui_dirty = false;
volatile size_t g_ui_peers = 0;

Board* g_board = nullptr;

// Per-node persistent tile widgets.
struct TileWidgets {
  lv_obj_t* tile = nullptr;
  lv_obj_t* label_loc = nullptr;
  lv_obj_t* label_age = nullptr;
  lv_obj_t* sensor_label[2] = {nullptr, nullptr};

  // Base (non-flash) colors.
  lv_color_t bg_normal;
  lv_color_t fg_normal;   // header + age

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

void GuiRebuildTiles();  // forward

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
  // Start from whatever defaults are in the globals; only override with valid ranges.
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
  const int stored = g_root_preferences.getInt("units", 1);  // default °F
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
  g_stale_minutes_threshold =
      stale > 0 ? static_cast<uint32_t>(stale) : 10U;
}

void SaveHighlightSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putInt("hl_missing",
                            g_highlight_missing_nodes ? 1 : 0);
  g_root_preferences.putInt("hl_stale_min",
                            static_cast<int>(g_stale_minutes_threshold));
  g_root_preferences.end();
}

void SaveBuzzerSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putULong("beep_len_ms",       g_beep_len_ms);
  g_root_preferences.putULong("beep_gap_warn_ms",  g_beep_gap_warn_ms);
  g_root_preferences.putULong("beep_gap_alert_ms", g_beep_gap_alert_ms);
  g_root_preferences.end();
}

void LoadBuzzerSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const uint32_t len  = g_root_preferences.getULong("beep_len_ms",       kDefaultBeepLenMs);
  const uint32_t gw   = g_root_preferences.getULong("beep_gap_warn_ms",  kDefaultGapWarnMs);
  const uint32_t ga   = g_root_preferences.getULong("beep_gap_alert_ms", kDefaultGapAlertMs);
  g_root_preferences.end();

  g_beep_len_ms       = (len > 0) ? len : kDefaultBeepLenMs;
  g_beep_gap_warn_ms  = gw;
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
  if (!g_last_seen["nodes"].is<JsonObject>()) {
    g_last_seen["nodes"].to<JsonObject>();
  }
  if (!g_labels["nodes"].is<JsonObject>()) {
    g_labels["nodes"].to<JsonObject>();
  }
  if (!g_labels["sensors"].is<JsonObject>()) {
    g_labels["sensors"].to<JsonObject>();
  }
}

void MigrateStorageIfNeeded() {
  const int version = LoadStorageVersion();

  if (version == 0 || version == 1) {
    // Old: labels only.
    LoadLabels();
    EnsureDocuments();

    g_display_fahrenheit = true;
    SaveDisplayUnits();

    g_highlight_missing_nodes = true;
    g_stale_minutes_threshold = 10;
    SaveHighlightSettings();

    // Initialize default limits and persist.
    g_warn_low_c   = 5.0f;
    g_warn_high_c  = 26.0f;
    g_alert_low_c  = 3.0f;
    g_alert_high_c = 30.0f;
    SaveLimits();

    // Buzzer + flash defaults.
    g_beep_len_ms       = kDefaultBeepLenMs;
    g_beep_gap_warn_ms  = kDefaultGapWarnMs;
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

    g_warn_low_c   = 5.0f;
    g_warn_high_c  = 26.0f;
    g_alert_low_c  = 3.0f;
    g_alert_high_c = 30.0f;
    SaveLimits();

    g_beep_len_ms       = kDefaultBeepLenMs;
    g_beep_gap_warn_ms  = kDefaultGapWarnMs;
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

    g_beep_len_ms       = kDefaultBeepLenMs;
    g_beep_gap_warn_ms  = kDefaultGapWarnMs;
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
    LCD* lcd = g_board->getLCD();
    if (lcd != nullptr) {
      lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);

  #if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
      auto* bus = lcd->getBus();
      if (bus != nullptr &&
          bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB*>(bus)->configRGB_BounceBufferSize(
            lcd->getFrameWidth() * 10);
      }
  #endif
    }
  }
#endif  // LVGL_PORT_AVOID_TEARING_MODE

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

  lv_obj_t* screen = lv_scr_act();
  lv_obj_set_style_pad_all(screen, 0, 0);

  // Disable all scrolling on the root screen
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  // Top bar.
  lv_obj_t* bar = lv_obj_create(screen);
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
    lv_disp_t* disp = lv_disp_get_default();
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
  lv_obj_set_flex_align(g_ui_tile_container,
                        LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  // No scrolling for the tile container.
  lv_obj_clear_flag(g_ui_tile_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(g_ui_tile_container, LV_SCROLLBAR_MODE_OFF);

  // Ensure the bar is always drawn on top of tiles.
  lv_obj_move_foreground(bar);

  g_gui_dirty = true;

  lvgl_port_unlock();
}

// Called when node metadata changes.
void GuiUpdateNodeSummary(const char* node_id,
                          int bus_gpio,
                          uint32_t last_ms) {
  (void)node_id;
  (void)bus_gpio;
  (void)last_ms;
  g_gui_dirty = true;
}

// Called when sensor data changes.
void GuiUpdateSensorRow(const char* node_id,
                        const char* addr,
                        float temp_f,
                        const char* label,
                        uint32_t last_ms) {
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
  const uint32_t hours   = minutes / 60U;

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

TileWidgets& GetOrCreateTile(const String& node_id_str,
                             lv_coord_t tile_w,
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

    const bool flashable =
        (tw.node_has_alert || tw.node_has_warning);
    const bool flash_active =
        flashable &&
        (g_flash_interval_ms > 0) &&
        g_flash_phase;

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
          lv_obj_set_style_text_color(tw.sensor_label[i],
                                      tw.fg_flash, 0);
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
          lv_obj_set_style_text_color(tw.sensor_label[i],
                                      tw.sensor_color[i], 0);
        }
      }
    }
  }
}

void GuiRebuildTiles() {
  if (g_ui_tile_container == nullptr) {
    return;
  }

  EnsureDocuments();
  JsonObject nodes = g_last_seen["nodes"].as<JsonObject>();

  // Snapshot current connections.
  auto node_list = mesh.getNodeList();
  std::vector<uint32_t> connected_ids(node_list.begin(), node_list.end());

  auto is_connected = [&](uint32_t node_id) -> bool {
    for (uint32_t id : connected_ids) {
      if (id == node_id) return true;
    }
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

  // Iterate all known nodes and update/create tiles.
  for (JsonPair node_entry : nodes) {
    const String node_id_str = node_entry.key().c_str();
    JsonObject node_obj = node_entry.value();

    TileWidgets& tw = GetOrCreateTile(node_id_str, tile_w, tile_h);

    // Location: label or node id.
    String loc = g_labels["nodes"][node_id_str] | "";
    if (loc.isEmpty()) {
      loc = node_id_str;
    }

    // Connection + age.
    JsonObject sensors = node_obj["sensors"].as<JsonObject>();

    char* endptr = nullptr;
    const uint32_t node_id_u32 =
        static_cast<uint32_t>(strtoul(node_id_str.c_str(), &endptr, 10));
    const bool node_connected =
        (endptr != node_id_str.c_str()) && is_connected(node_id_u32);

    uint32_t latest_ms = node_obj["last"] | 0U;
    for (JsonPair sensor_entry : sensors) {
      JsonObject s = sensor_entry.value();
      const uint32_t s_last = s["last"] | 0U;
      if (s_last > latest_ms) {
        latest_ms = s_last;
      }
    }

    const uint32_t age_min =
        (latest_ms <= now_ms)
            ? (now_ms - latest_ms) / 60000U
            : 0U;

    const bool is_missing =
        g_highlight_missing_nodes &&
        !node_connected &&
        (g_stale_minutes_threshold > 0) &&
        (age_min >= g_stale_minutes_threshold);

    const bool is_stale =
        node_connected &&
        (g_stale_minutes_threshold > 0) &&
        (age_min >= g_stale_minutes_threshold);

    // Build up to 2 sensor views.
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

    for (JsonPair sensor_entry : sensors) {
      if (sv_count >= 2) break;

      const String addr = sensor_entry.key().c_str();
      JsonObject s = sensor_entry.value();

      SensorView& v = sv[sv_count++];
      v.label = (g_labels["sensors"][addr] | "");
      if (v.label.isEmpty()) v.label = addr;

      if (s["tC"].is<float>()) {
        v.temp_c = s["tC"].as<float>();
        v.has_value = !isnan(v.temp_c);
      }

      if (v.has_value) {
        const float t = v.temp_c;

        if (!isnan(g_alert_low_c) && t < g_alert_low_c) {
          v.is_alert = true;
        }
        if (!isnan(g_alert_high_c) && t > g_alert_high_c) {
          v.is_alert = true;
        }

        if (!v.is_alert) {
          if (!isnan(g_warn_low_c) && t < g_warn_low_c) {
            v.is_warning = true;
          }
          if (!isnan(g_warn_high_c) && t > g_warn_high_c) {
            v.is_warning = true;
          }
        }
      }

      if (v.is_alert) {
        node_has_alert = true;
      } else if (v.is_warning) {
        node_has_warning = true;
      }
    }

    // Decide base colors for this tile (normal phase).
    lv_color_t bg = lv_color_make(0x16, 0x3A, 0x24);  // dark green
    lv_color_t fg = lv_color_white();                 // header/age

    if (node_has_alert) {
      bg = lv_color_make(0xB7, 0x1C, 0x1C);          // deep red
      fg = lv_color_white();
      g_any_alert = true;
    } else if (is_stale || node_has_warning) {
      bg = lv_color_make(0x8A, 0x5A, 0x00);          // dark amber
      fg = lv_color_white();
      g_any_warning = true;
    } else if (is_missing) {
      bg = lv_color_make(0x20, 0x40, 0x60);          // muted blue-gray
      fg = lv_color_white();
    }

    // Store state in tile.
    tw.node_has_alert   = node_has_alert;
    tw.node_has_warning = node_has_warning;
    tw.is_missing       = is_missing;
    tw.is_stale         = is_stale;

    tw.bg_normal = bg;
    tw.fg_normal = fg;

    // Flash colors (used only by GuiApplyFlashPhase()).
    if (node_has_alert || node_has_warning) {
      // Flash = light grey background with black text.
      tw.bg_flash = lv_color_make(0xCC, 0xCC, 0xCC);
      tw.fg_flash = lv_color_black();
    } else {
      // Non-flashing nodes: flash == normal (no visible change).
      tw.bg_flash = bg;
      tw.fg_flash = fg;
    }

    // Update location label.
    lv_label_set_text(tw.label_loc, loc.c_str());
    lv_obj_align(tw.label_loc, LV_ALIGN_TOP_MID, 0, 0);

    // Update age label.
    char age_buf[24];
    snprintf(age_buf, sizeof(age_buf), "Age: %lu min",
             static_cast<unsigned long>(age_min));
    lv_label_set_text(tw.label_age, age_buf);
    lv_obj_align(tw.label_age, LV_ALIGN_TOP_MID, 0, 20);

    // Update sensor labels & store their "normal" colors.
    tw.sensor_count = sv_count;
    for (int i = 0; i < 2; ++i) {
      lv_obj_t* lbl = tw.sensor_label[i];
      if (!lbl) continue;

      if (i >= sv_count) {
        lv_label_set_text(lbl, "");
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        continue;
      }

      lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);

      const SensorView& v = sv[i];

      char buf[64];
      if (!v.has_value) {
        snprintf(buf, sizeof(buf), "%s: --", v.label.c_str());
      } else {
        float temp_disp = v.temp_c;
        if (g_display_fahrenheit) {
          temp_disp = temp_disp * 1.8f + 32.0f;
        }
        const char unit_ch = g_display_fahrenheit ? 'F' : 'C';
        snprintf(buf, sizeof(buf), "%s: %.1f%c",
                 v.label.c_str(),
                 static_cast<double>(temp_disp),
                 unit_ch);
      }
      lv_label_set_text(lbl, buf);

      lv_color_t sensor_color;
      if (!v.has_value) {
        sensor_color = lv_color_make(0xCC, 0xCC, 0xCC); // light gray
      } else if (v.is_alert) {
        sensor_color = lv_color_make(0xFF, 0xFF, 0x80); // bright yellow
      } else if (v.is_warning) {
        sensor_color = lv_color_make(0xFF, 0xD7, 0x00); // gold
      } else if (node_has_alert) {
        sensor_color = lv_color_make(0xFF, 0xE0, 0xE0); // soft light
      } else if (is_stale || node_has_warning) {
        sensor_color = lv_color_make(0xFF, 0xF7, 0xE0); // warm cream
      } else if (is_missing) {
        sensor_color = lv_color_make(0xB0, 0xBE, 0xC5); // blue-gray
      } else {
        sensor_color = lv_color_make(0xE0, 0xFF, 0xE0); // light green
      }

      tw.sensor_color[i] = sensor_color;

      // Position sensors near bottom.
      const int y_offset = (sv_count == 1)
                               ? -8
                               : (i == 0 ? -26 : -8);
      lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, y_offset);
    }

    // Apply NORMAL (non-flash) appearance only.
    lv_obj_set_style_bg_color(tw.tile, tw.bg_normal, 0);
    lv_obj_set_style_bg_opa(tw.tile, LV_OPA_COVER, 0);

    lv_obj_set_style_text_color(tw.label_loc, tw.fg_normal, 0);
    lv_obj_set_style_text_color(tw.label_age, tw.fg_normal, 0);

    for (int i = 0; i < tw.sensor_count; ++i) {
      if (tw.sensor_label[i]) {
        lv_obj_set_style_text_color(tw.sensor_label[i],
                                    tw.sensor_color[i], 0);
      }
    }

    // Let GuiApplyFlashPhase() decide flashing; start from "not flashed".
    tw.last_flash_active = false;
  }
}

}  // namespace

// -----------------------------------------------------------------------------
// GUI helpers callable from elsewhere
// -----------------------------------------------------------------------------

void GuiUpdateNetwork(size_t peers) {
  g_ui_peers = peers;
  g_gui_dirty = true;
}


void GuiRequestRender() {
  g_gui_dirty = true;
}

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
      g_flash_dirty = true;      // <--- instead of g_gui_dirty
      last_flash_ms = now_ms;
    }
  } else {
    if (g_flash_phase) {
      g_flash_phase = false;
      g_flash_dirty = true;
    }
    last_flash_ms = now_ms;
  }

  // Periodic full GUI rebuild for age/status.
  if (now_ms - last_force_ms >= 60000U) {
    g_gui_dirty = true;
    last_force_ms = now_ms;
  }

  if ((g_gui_dirty || g_flash_dirty) &&
      (now_ms - last_build_ms >= 50U)) {
    if (lvgl_port_lock(-1)) {

      if (g_gui_dirty) {
        g_gui_dirty = false;

        if (g_ui_label_peers != nullptr) {
          char buffer[32];
          snprintf(buffer, sizeof(buffer), "Peers: %u",
                   static_cast<unsigned>(g_ui_peers));
          lv_label_set_text(g_ui_label_peers, buffer);
        }

        GuiRebuildTiles();  // data/layout only
      }

      if (g_flash_dirty) {
        g_flash_dirty = false;
        GuiApplyFlashPhase();  // lightweight color toggle
      }

      UpdateUptimeLabel();
      lvgl_port_unlock();
      last_build_ms = now_ms;
    }
  }
}



void BuzzerLoop() {
  // Static state so we can be called every loop() tick.
  static bool     buzzer_on     = false;
  static uint32_t beep_start_ms = 0;
  static uint32_t last_beep_ms  = 0;

  const uint32_t now_ms = millis();

  // Priority: ALERT > WARNING (warning includes stale/etc via g_any_warning).
  const bool have_alert   = g_any_alert;
  const bool have_warning = (!have_alert && g_any_warning);

  uint32_t gap_ms = 0;
  if (have_alert) {
    gap_ms = g_beep_gap_alert_ms;
  } else if (have_warning) {
    gap_ms = g_beep_gap_warn_ms;
  }

  // If no conditions or disabled, ensure buzzer is off and reset schedule.
  if ((!have_alert && !have_warning) ||
      gap_ms == 0 ||
      g_beep_len_ms == 0) {
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

void ProcessConsoleRoot() {
  static String input_line;
  constexpr size_t kMaxConsoleLineLength = 256;

  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch != '\n') {
      input_line += ch;
      if (input_line.length() > kMaxConsoleLineLength) {
        const int excess =
            static_cast<int>(input_line.length()) -
            static_cast<int>(kMaxConsoleLineLength);
        input_line.remove(0, excess);
      }
      continue;
    }

    input_line.trim();
    if (input_line.length() == 0) {
      Serial.println(F("ok"));
      input_line = "";
      continue;
    }

    std::vector<String> tokens;
    tokens.reserve(8);

    const int line_length = static_cast<int>(input_line.length());
    int field_start = 0;
    for (int i = 0; i < line_length; ++i) {
      if (isspace(static_cast<unsigned char>(input_line[i]))) {
        if (i > field_start) {
          tokens.push_back(input_line.substring(field_start, i));
        }
        field_start = i + 1;
      }
    }
    if (field_start < line_length) {
      tokens.push_back(input_line.substring(field_start));
    }

    if (tokens.empty()) {
      Serial.println(F("ok"));
      input_line = "";
      continue;
    }

    const String& command = tokens[0];

    auto print_help = []() {
      Serial.println(F("Commands (root):"));
      Serial.println(F("  ls"));
      Serial.println(F("  node <id> <location>"));
      Serial.println(F("  name <addr16> <label>"));
      Serial.println(F("  save | load | erase"));
      Serial.println(F("  debug on|off"));
      Serial.println(F("  units c|f"));
      Serial.println(F("  highlight missing on|off"));
      Serial.println(F("  highlight stale <minutes>"));
      Serial.println(F("  dummy on|off"));
      Serial.println(F("  limits warn <low> <high>"));
      Serial.println(F("  limits alert <low> <high>"));
      Serial.println(F("  limits show"));
      Serial.println(F("  buzzer <len_ms> <warn_gap_ms> <alert_gap_ms>"));
      Serial.println(F("  flash <interval_ms|off>"));
    };

    if (command == "help" || command == "?") {
      print_help();

    } else if (command == "ls") {
      EnsureDocuments();
      JsonObject nodes = g_last_seen["nodes"].as<JsonObject>();

      for (JsonPair node_entry : nodes) {
        const String node_id = node_entry.key().c_str();
        JsonObject node_obj = node_entry.value();

        const String node_label = g_labels["nodes"][node_id] | "";

        if (node_label.length() > 0) {
          Serial.printf("node %s \"%s\":\n",
                        node_id.c_str(), node_label.c_str());
        } else {
          Serial.printf("node %s:\n", node_id.c_str());
        }

        JsonObject sensors = node_obj["sensors"].as<JsonObject>();
        for (JsonPair sensor_entry : sensors) {
          const String addr = sensor_entry.key().c_str();
          JsonObject sensor_obj = sensor_entry.value();

          const float temp_c =
              sensor_obj["tC"] |
              std::numeric_limits<float>::quiet_NaN();
          const bool corrected = sensor_obj["corr"] | false;
          const String sensor_label =
              g_labels["sensors"][addr] | "";

          const char* temp_str =
              isnan(temp_c) ? "NaN" : String(temp_c, 2).c_str();

          if (sensor_label.length() > 0) {
            Serial.printf("  %s \"%s\" : %s%s\n",
                          addr.c_str(),
                          sensor_label.c_str(),
                          temp_str,
                          corrected ? " (corr)" : "");
          } else {
            Serial.printf("  %s : %s%s\n",
                          addr.c_str(),
                          temp_str,
                          corrected ? " (corr)" : "");
          }
        }
      }

      GuiRequestRender();

    } else if (command == "node" && tokens.size() >= 3) {
      g_labels["nodes"][tokens[1]] = tokens[2];
      SaveLabels();
      Serial.println(F("ok"));
      GuiRequestRender();

    } else if (command == "name" && tokens.size() >= 3) {
      g_labels["sensors"][tokens[1]] = tokens[2];
      SaveLabels();
      Serial.println(F("ok"));
      GuiRequestRender();

    } else if (command == "save") {
      SaveLabels();
      Serial.println(F("saved"));

    } else if (command == "load") {
      LoadLabels();
      Serial.println(F("loaded"));
      GuiRequestRender();

    } else if (command == "erase") {
      EraseLabels();
      Serial.println(F("erased"));
      GuiRequestRender();

    } else if (command == "debug" && tokens.size() >= 2) {
      g_debug_enabled = (tokens[1] == "on");
      Serial.printf("debug=%s\n", g_debug_enabled ? "on" : "off");

    } else if (command == "units" && tokens.size() >= 2) {
      const String& mode = tokens[1];
      if (mode.equalsIgnoreCase("c")) {
        g_display_fahrenheit = false;
        SaveDisplayUnits();
        Serial.println(F("units=C"));
        GuiRequestRender();
      } else if (mode.equalsIgnoreCase("f")) {
        g_display_fahrenheit = true;
        SaveDisplayUnits();
        Serial.println(F("units=F"));
        GuiRequestRender();
      } else {
        Serial.println(F("ERR units (use 'units c' or 'units f')"));
      }

    } else if (command == "highlight" && tokens.size() >= 3) {
      const String& target = tokens[1];
      const String& value = tokens[2];

      if (target == "missing") {
        if (value == "on") {
          g_highlight_missing_nodes = true;
          SaveHighlightSettings();
          Serial.println(F("highlight missing=on"));
          GuiRequestRender();
        } else if (value == "off") {
          g_highlight_missing_nodes = false;
          SaveHighlightSettings();
          Serial.println(F("highlight missing=off"));
          GuiRequestRender();
        } else {
          Serial.println(F("ERR highlight missing (use on|off)"));
        }

      } else if (target == "stale") {
        const long minutes = value.toInt();
        if (minutes > 0) {
          g_stale_minutes_threshold =
              static_cast<uint32_t>(minutes);
          SaveHighlightSettings();
          Serial.printf("highlight stale=%ld min\n", minutes);
          GuiRequestRender();
        } else {
          Serial.println(
              F("ERR highlight stale (use positive minutes)"));
        }
      } else {
        Serial.println(
            F("ERR highlight (use 'highlight missing on|off' or "
              "'highlight stale <min>')"));
      }

    } else if (command == "flash" && tokens.size() >= 2) {
      if (tokens[1].equalsIgnoreCase("off")) {
        g_flash_interval_ms = 0;
        SaveFlashSettings();
        g_flash_phase = false;
        g_gui_dirty = true;
        Serial.println(F("flash off"));
      } else {
        const long interval = tokens[1].toInt();
        if (interval < 0) {
          Serial.println(
              F("ERR flash (use 'flash <interval_ms>' with >=0, or 'flash off')"));
        } else {
          g_flash_interval_ms = static_cast<uint32_t>(interval);
          SaveFlashSettings();
          g_flash_phase = false;
          g_gui_dirty = true;
          Serial.printf("flash interval=%lu ms (%s)\n",
                        static_cast<unsigned long>(g_flash_interval_ms),
                        g_flash_interval_ms ? "enabled" : "off");
        }
      }

    } else if (command == "dummy" && tokens.size() >= 2) {
      const String& mode = tokens[1];
      if (mode == "on") {
        g_use_dummy_data = true;
        BuildDummyData();
        Serial.println(F("dummy=on"));
        GuiRequestRender();
      } else if (mode == "off") {
        g_use_dummy_data = false;
        g_last_seen.clear();
        EnsureDocuments();
        Serial.println(F("dummy=off"));
        GuiRequestRender();
      } else {
        Serial.println(F("ERR dummy (use 'dummy on' or 'dummy off')"));
      }

    } else if (command == "limits" && tokens.size() >= 2 && tokens[1] == "show") {
      auto FromC = [&](float c) -> float {
        return g_display_fahrenheit ? (c * 1.8f + 32.0f) : c;
      };
      const char* unit = g_display_fahrenheit ? "F" : "C";

      Serial.printf("limits warn=%.2f..%.2f %s\n",
                    FromC(g_warn_low_c),
                    FromC(g_warn_high_c),
                    unit);
      Serial.printf("limits alert=%.2f..%.2f %s\n",
                    FromC(g_alert_low_c),
                    FromC(g_alert_high_c),
                    unit);

    } else if (command == "limits" && tokens.size() >= 4) {
      const String& kind = tokens[1];
      const float low_in  = tokens[2].toFloat();
      const float high_in = tokens[3].toFloat();

      if (high_in <= low_in) {
        Serial.println(F("ERR limits (high must be > low)"));
      } else {
        auto ToC = [&](float v) -> float {
          return g_display_fahrenheit ? (v - 32.0f) / 1.8f : v;
        };
        auto FromC = [&](float v) -> float {
          return g_display_fahrenheit ? (v * 1.8f + 32.0f) : v;
        };
        const char* unit = g_display_fahrenheit ? "F" : "C";

        if (kind == "warn") {
          g_warn_low_c  = ToC(low_in);
          g_warn_high_c = ToC(high_in);
          SaveLimits();
          Serial.printf("limits warn=%.2f..%.2f %s\n",
                        FromC(g_warn_low_c),
                        FromC(g_warn_high_c),
                        unit);
        } else if (kind == "alert") {
          g_alert_low_c  = ToC(low_in);
          g_alert_high_c = ToC(high_in);
          SaveLimits();
          Serial.printf("limits alert=%.2f..%.2f %s\n",
                        FromC(g_alert_low_c),
                        FromC(g_alert_high_c),
                        unit);
        } else {
          Serial.println(
              F("ERR limits (use 'limits warn <low> <high>' or "
                "'limits alert <low> <high>' or 'limits show')"));
        }
      }
    } else if (command == "buzzer" && tokens.size() >= 4) {
      const long len_ms      = tokens[1].toInt();
      const long gap_warn_ms = tokens[2].toInt();
      const long gap_alert_ms= tokens[3].toInt();

      if (len_ms <= 0 || gap_warn_ms < 0 || gap_alert_ms < 0) {
        Serial.println(
            F("ERR buzzer (use: buzzer <len_ms> <warn_gap_ms> <alert_gap_ms>, gaps>=0)"));
      } else {
        g_beep_len_ms       = static_cast<uint32_t>(len_ms);
        g_beep_gap_warn_ms  = static_cast<uint32_t>(gap_warn_ms);
        g_beep_gap_alert_ms = static_cast<uint32_t>(gap_alert_ms);
        SaveBuzzerSettings();
        Serial.printf("buzzer len=%lu ms warn_gap=%lu ms alert_gap=%lu ms\n",
                      static_cast<unsigned long>(g_beep_len_ms),
                      static_cast<unsigned long>(g_beep_gap_warn_ms),
                      static_cast<unsigned long>(g_beep_gap_alert_ms));
      }
    } else {
      Serial.println(F("ERR (help)"));
    }

    input_line = "";
  }
}

// -----------------------------------------------------------------------------
// Mesh callbacks (root)
// -----------------------------------------------------------------------------

void OnReceiveRoot(uint32_t from, String& msg) {
  DLOG("[ROOT RX] from=%u len=%u: %s\n",
       from, static_cast<unsigned>(msg.length()), msg.c_str());

  if (g_use_dummy_data) {
    // In dummy mode, ignore real traffic to keep UI deterministic.
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) {
    DLOG("  ! JSON parse error\n");
    return;
  }

  const char* type = doc["type"] | "temps";
  if (strcmp(type, "temps") != 0) {
    DLOG("  ! ignoring type '%s'\n", type);
    return;
  }

  EnsureDocuments();

  const uint32_t node_id = doc["nodeId"] | from;
  const int bus_gpio = doc["busGpio"] | -1;

  JsonObject nodes = g_last_seen["nodes"].to<JsonObject>();
  JsonObject node_obj = nodes[String(node_id)].to<JsonObject>();
  node_obj["busGpio"] = bus_gpio;
  node_obj["last"] = static_cast<uint32_t>(millis());

  const String node_id_str = String(node_id);
  GuiUpdateNodeSummary(node_id_str.c_str(),
                       bus_gpio,
                       static_cast<uint32_t>(millis()));

  JsonObject sensors = node_obj["sensors"].to<JsonObject>();
  JsonArray sensor_array = doc["sensors"].as<JsonArray>();

  DLOG("  nodeId=%u bus_gpio=%d sensors=%d\n",
       node_id, bus_gpio, sensor_array.size());

  for (JsonObject sensor_in : sensor_array) {
    const char* addr = sensor_in["addr"] | "";
    if (addr == nullptr || strlen(addr) != 16) {
      continue;
    }

    JsonObject sensor_out = sensors[addr].to<JsonObject>();

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
    const char* label = g_labels["sensors"][addr] | "";

    DLOG("    [%s]%s = %s%s\n",
         addr,
         (label != nullptr && strlen(label) > 0)
             ? (String(" \"") + label + "\"").c_str()
             : "",
         isnan(temp_f) ? "NaN" : String(temp_f, 2).c_str(),
         corrected ? " (corr)" : "");

    GuiUpdateSensorRow(node_id_str.c_str(),
                       addr,
                       temp_f,
                       (label != nullptr && strlen(label) > 0)
                           ? label
                           : nullptr,
                       static_cast<uint32_t>(millis()));
  }

  GuiRequestRender();
}

void OnConnectionsChangedRoot() {
  LogConnections();

  if (g_use_dummy_data) {
    // Leave dummy view untouched.
    GuiRequestRender();
    return;
  }

  EnsureDocuments();
  JsonObject nodes = g_last_seen["nodes"].to<JsonObject>();

  const uint32_t now_ms = millis();

  // Ensure each connected node has an entry.
  for (const auto& node_id : mesh.getNodeList()) {
    const String key = String(node_id);

    JsonObject node_obj = nodes[key];
    if (node_obj.isNull()) {
      node_obj = nodes[key].to<JsonObject>();
    }
    if (!node_obj["last"].is<uint32_t>()) {
      node_obj["last"] = now_ms;
    }
    if (!node_obj["sensors"].is<JsonObject>()) {
      node_obj["sensors"].to<JsonObject>();
    }
  }

  // Do not delete nodes; they age out visually.
  GuiRequestRender();
}

// -----------------------------------------------------------------------------
// Arduino entry points (root)
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

  g_last_seen.clear();
  g_labels.clear();

  MigrateStorageIfNeeded();
  EnsureDocuments();

  DisplayInit();
  GuiInit();
  GuiUpdateNetwork(0);
  GuiRequestRender();

  pinMode(kBuzzerPin, OUTPUT);
  digitalWrite(kBuzzerPin, LOW);

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &user_scheduler, MESH_PORT);
  mesh.setRoot(true);
  mesh.setContainsRoot(true);
  mesh.onReceive(&OnReceiveRoot);
  mesh.onChangedConnections(&OnConnectionsChangedRoot);

  g_task_announce.set(
      TASK_IMMEDIATE, TASK_FOREVER,
      []() {
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
  ProcessConsoleRoot();
}

// -----------------------------------------------------------------------------
// LEAF BUILD
// -----------------------------------------------------------------------------
#else  // !MESH_IS_ROOT

#include <DallasTemperature.h>
#include <OneWire.h>
#include <Preferences.h>

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

// Live calibration session.
struct CalSession {
  CalStage stage = kCalIdle;
  String addr;
  float last_raw = NAN;

  bool have_ice = false;
  bool have_boil = false;

  float raw_ice = 0.0f;
  float act_ice = 0.0f;
  float raw_boil = 0.0f;
  float act_boil = 0.0f;
};

CalSession g_cal_session;
Task g_task_cal;

namespace {

bool FindDeviceByAddr(const String& addr16, Address* out_addr) {
  if (addr16.length() != 16) {
    return false;
  }
  for (const auto& address : g_devices) {
    if (addr16.equalsIgnoreCase(addrToHex(address.data()))) {
      *out_addr = address;
      return true;
    }
  }
  return false;
}

int FindCalIndex(const String& addr) {
  for (size_t i = 0; i < g_cal_entries.size(); ++i) {
    if (g_cal_entries[i].addr.equalsIgnoreCase(addr)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void SetCal(const String& addr, const Coeff& coeff) {
  const int index = FindCalIndex(addr);
  if (index >= 0) {
    g_cal_entries[static_cast<size_t>(index)].coeff = coeff;
  } else {
    g_cal_entries.push_back({addr, coeff});
  }
}

bool IsIdentity(const Coeff& coeff) {
  return (fabsf(coeff.a1 - 1.0f) < 1e-6f) &&
         (fabsf(coeff.a0) < 1e-6f);
}

float ApplyCorrection(float temp_raw_c,
                      const String& addr16,
                      bool* out_corrected) {
  const int index = FindCalIndex(addr16);
  if (index < 0) {
    if (out_corrected != nullptr) {
      *out_corrected = false;
    }
    return temp_raw_c;
  }

  const Coeff& coeff = g_cal_entries[static_cast<size_t>(index)].coeff;
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
  for (const auto& entry : g_cal_entries) {
    index_array.add(entry.addr);
  }

  String index_json;
  serializeJson(index_doc, index_json);
  g_leaf_prefs.putString("index", index_json);

  for (const auto& entry : g_cal_entries) {
    char key[32];
    snprintf(key, sizeof(key), "c_%s", entry.addr.c_str());

    const Coeff& c = entry.coeff;
    char value[64];
    snprintf(value,
             sizeof(value),
             "%.8f,%.8f",
             static_cast<double>(c.a1),
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
        const String addr = value.as<const char*>();

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
            parsed = sscanf(cal_str.c_str(),
                            "%f,%f,%f", &legacy_a2, &a1, &a0);
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

void ClearCalibration(const String& addr) {
  const int index = FindCalIndex(addr);
  if (index >= 0) {
    g_cal_entries.erase(g_cal_entries.begin() +
                        static_cast<size_t>(index));
  }
  SaveAllCalibration();
}

// Live calibration task.
void CalibrationTaskFn() {
  if (g_cal_session.stage == kCalIdle ||
      g_cal_session.addr.length() != 16) {
    g_task_cal.disable();
    return;
  }

  Address addr;
  if (!FindDeviceByAddr(g_cal_session.addr, &addr)) {
    Serial.println(F("CAL ERROR: sensor not found; aborting"));
    g_cal_session.stage = kCalIdle;
    g_task_cal.disable();
    return;
  }

  g_ds18.requestTemperatures();
  const float temp_c =
      g_ds18.getTempC(reinterpret_cast<const uint8_t*>(addr.data()));
  g_cal_session.last_raw = temp_c;

  const char* stage_str =
      (g_cal_session.stage == kCalIce) ? "ICE" : "BOIL";

  if (!isnan(temp_c)) {
    Serial.printf("CAL %s %s : raw=%.3f C\n",
                  stage_str,
                  g_cal_session.addr.c_str(),
                  temp_c);
  }

  g_task_cal.delay(1000);
}

void CalibrationStart(const String& addr16, CalStage stage) {
  if (addr16.length() != 16) {
    Serial.println(F("ERR addr16"));
    return;
  }

  g_cal_session.stage = stage;
  g_cal_session.addr = addr16;
  g_cal_session.last_raw = NAN;

  if (stage == kCalIce) {
    g_cal_session.have_ice = false;
  }
  if (stage == kCalBoil) {
    g_cal_session.have_boil = false;
  }

  Serial.printf("CAL %s started for %s\n",
                (stage == kCalIce ? "ICE" : "BOIL"),
                addr16.c_str());

  g_task_cal.enableIfNot();
}

void CalibrationLock(float actual_c) {
  if (g_cal_session.stage == kCalIdle) {
    Serial.println(F("CAL idle"));
    return;
  }
  if (isnan(g_cal_session.last_raw)) {
    Serial.println(F("CAL no reading yet"));
    return;
  }

  if (g_cal_session.stage == kCalIce) {
    g_cal_session.raw_ice = g_cal_session.last_raw;
    g_cal_session.act_ice = actual_c;
    g_cal_session.have_ice = true;
    Serial.println(F("ICE locked."));
    g_cal_session.stage = kCalIdle;
  } else if (g_cal_session.stage == kCalBoil) {
    g_cal_session.raw_boil = g_cal_session.last_raw;
    g_cal_session.act_boil = actual_c;
    g_cal_session.have_boil = true;
    Serial.println(F("BOIL locked."));
    g_cal_session.stage = kCalIdle;
  }
}

void CalibrationSolveAndSave() {
  if (!g_cal_session.have_ice || !g_cal_session.have_boil) {
    Serial.println(F("CAL not complete"));
    return;
  }

  const float x1 = g_cal_session.raw_ice;
  const float y1 = g_cal_session.act_ice;
  const float x2 = g_cal_session.raw_boil;
  const float y2 = g_cal_session.act_boil;

  if (fabsf(x2 - x1) < 1e-4f) {
    Serial.println(F("CAL ERROR: identical raw points"));
    return;
  }

  Coeff coeff;
  coeff.a1 = (y2 - y1) / (x2 - x1);
  coeff.a0 = y1 - coeff.a1 * x1;

  SetCal(g_cal_session.addr, coeff);
  SaveAllCalibration();

  Serial.printf("CAL SAVED %s : a1=%.6f a0=%.6f\n",
                g_cal_session.addr.c_str(),
                static_cast<double>(coeff.a1),
                static_cast<double>(coeff.a0));
  Serial.printf("  check: ice  raw=%.3f -> %.3f (want %.3f)\n",
                static_cast<double>(x1),
                static_cast<double>(coeff.a1 * x1 + coeff.a0),
                static_cast<double>(y1));
  Serial.printf("  check: boil raw=%.3f -> %.3f (want %.3f)\n",
                static_cast<double>(x2),
                static_cast<double>(coeff.a1 * x2 + coeff.a0),
                static_cast<double>(y2));

  g_cal_session = CalSession{};
  g_task_cal.disable();
}

void ScanSensors() {
  g_devices.clear();
  g_ds18.begin();

  DeviceAddress raw;
  const int count = g_ds18.getDeviceCount();
  DLOG("[LEAF] scanning DS18B20 on GPIO %d: found=%d\n",
       ONEWIRE_PIN, count);

  for (int i = 0; i < count; ++i) {
    if (g_ds18.getAddress(raw, i)) {
      g_devices.emplace_back();
      memcpy(g_devices.back().data(), raw, 8);
      DLOG("  addr[%d]=%s\n", i,
           addrToHex(g_devices.back().data()).c_str());
    }
  }

  if (g_devices.empty()) {
    DLOG("  (no sensors)\n");
  }
}

void SendTemperatures() {
  g_ds18.requestTemperatures();

  JsonDocument doc;
  doc["type"] = "temps";
  doc["nodeId"] = mesh.getNodeId();
  doc["busGpio"] = ONEWIRE_PIN;
  doc["uptimeMs"] = static_cast<uint32_t>(millis());

  JsonArray sensors = doc["sensors"].to<JsonArray>();

  for (const auto& address : g_devices) {
    const String addr16 = addrToHex(address.data());
    const float temp_raw_c =
        g_ds18.getTempC(reinterpret_cast<const uint8_t*>(address.data()));

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
      DLOG("[LEAF MEAS] %s raw=%.2fC out=%.2fC%s\n",
           addr16.c_str(),
           temp_raw_c,
           temp_out_c,
           corrected ? " (corr)" : "");
    } else {
      DLOG("[LEAF MEAS] %s = (disconnected)\n", addr16.c_str());
    }
  }

  String message;
  serializeJson(doc, message);

  const bool use_unicast =
      (g_root_id != 0U) &&
      ((millis() - g_root_last_seen_ms) < 15000U);

  DLOG("[LEAF TX %s] %s\n",
       use_unicast ? "unicast" : "bcast",
       message.c_str());

  if (use_unicast) {
    mesh.sendSingle(g_root_id, message);
  } else {
    mesh.sendBroadcast(message);
  }
}

void ProcessConsoleLeaf() {
  static String input_line;
  constexpr size_t kMaxConsoleLineLength = 256;

  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());

    if (ch == '\r') {
      continue;
    }

    if (ch != '\n') {
      input_line += ch;

      if (input_line.length() > kMaxConsoleLineLength) {
        const int excess =
            static_cast<int>(input_line.length()) -
            static_cast<int>(kMaxConsoleLineLength);
        input_line.remove(0, excess);
      }
      continue;
    }

    input_line.trim();

    if (input_line.length() == 0) {
      Serial.println(F("ok"));
      input_line = "";
      continue;
    }

    std::vector<String> tokens;
    tokens.reserve(8);

    const int line_length = static_cast<int>(input_line.length());
    int field_start = 0;

    for (int i = 0; i < line_length; ++i) {
      if (isspace(static_cast<unsigned char>(input_line[i]))) {
        if (i > field_start) {
          tokens.push_back(input_line.substring(field_start, i));
        }
        field_start = i + 1;
      }
    }

    if (field_start < line_length) {
      tokens.push_back(input_line.substring(field_start));
    }

    if (tokens.empty()) {
      Serial.println(F("ok"));
      input_line = "";
      continue;
    }

    const String& command = tokens[0];

    if (command == "debug" && tokens.size() >= 2) {
      g_debug_enabled = (tokens[1] == "on");
      Serial.printf("debug=%s\n", g_debug_enabled ? "on" : "off");

    } else if (command == "scan") {
      ScanSensors();
      Serial.println(F("ok"));

    } else if (command == "sendnow") {
      SendTemperatures();
      Serial.println(F("ok"));

    } else if (command == "cal" && tokens.size() >= 2) {
      const String& sub = tokens[1];

      if (sub == "ice" && tokens.size() >= 3) {
        CalibrationStart(tokens[2], kCalIce);
        Serial.println(F("ok"));

      } else if (sub == "boil" && tokens.size() >= 3) {
        CalibrationStart(tokens[2], kCalBoil);
        Serial.println(F("ok"));

      } else if (sub == "lock" && tokens.size() >= 3) {
        CalibrationLock(tokens[2].toFloat());
        Serial.println(F("ok"));

      } else if (sub == "solve") {
        CalibrationSolveAndSave();
        Serial.println(F("ok"));

      } else if (sub == "list") {
        for (const auto& entry : g_cal_entries) {
          const Coeff& c = entry.coeff;
          Serial.printf("%s : a1=%.6f a0=%.6f\n",
                        entry.addr.c_str(),
                        static_cast<double>(c.a1),
                        static_cast<double>(c.a0));
        }

      } else if (sub == "show" && tokens.size() >= 3) {
        const int idx = FindCalIndex(tokens[2]);
        if (idx >= 0) {
          const Coeff& c = g_cal_entries[static_cast<size_t>(idx)].coeff;
          Serial.printf("%s : a1=%.6f a0=%.6f\n",
                        tokens[2].c_str(),
                        static_cast<double>(c.a1),
                        static_cast<double>(c.a0));
        } else {
          Serial.println(F("not found"));
        }

      } else if (sub == "clear" && tokens.size() >= 3) {
        ClearCalibration(tokens[2]);
        Serial.println(F("ok"));

      } else if (sub == "save") {
        SaveAllCalibration();
        Serial.println(F("saved"));

      } else if (sub == "load") {
        LoadAllCalibration();
        Serial.println(F("loaded"));

      } else {
        Serial.println(
            F("cal cmds: ice <addr16> | boil <addr16> | "
              "lock <actualC> | solve | list | show <addr16> | "
              "clear <addr16> | save | load"));
      }

    } else if (command == "help" || command == "?") {
      Serial.println(F("Commands (leaf):"));
      Serial.println(F("  debug on|off | scan | sendnow"));
      Serial.println(
          F("  cal ice <addr16> | cal boil <addr16> | "
            "cal lock <actualC> | cal solve"));
      Serial.println(
          F("  cal list | cal show <addr16> | cal clear <addr16> | "
            "cal save | cal load"));

    } else {
      Serial.println(F("ERR (help)"));
    }

    input_line = "";
  }
}

// painlessMesh callbacks (leaf).
void OnReceiveLeaf(uint32_t from, String& msg) {
  DLOG("[LEAF RX] from=%u len=%u: %s\n",
       from, static_cast<unsigned>(msg.length()), msg.c_str());

  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) {
    DLOG("  ! JSON parse error\n");
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "root_announce") == 0) {
    const uint32_t root_id = doc["rootId"] | 0U;
    if (root_id != 0U) {
      g_root_id = root_id;
      g_root_last_seen_ms = millis();
      DLOG("  root_announce: rootId=%u\n", g_root_id);
    }
  }
}

void OnConnectionsChangedLeaf() {
  LogConnections();
}

}  // namespace

// Arduino entry points (leaf)
void setup() {
  Serial.begin(115200);
  delay(1000);

  g_ds18.begin();
  LoadAllCalibration();
  ScanSensors();

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &user_scheduler, MESH_PORT);
  mesh.setContainsRoot(true);
  mesh.onReceive(&OnReceiveLeaf);
  mesh.onChangedConnections(&OnConnectionsChangedLeaf);

  g_task_send.set(
      TASK_IMMEDIATE,
      TASK_FOREVER,
      []() {
        SendTemperatures();
        g_task_send.delay(SEND_PERIOD_MS);
      });
  user_scheduler.addTask(g_task_send);
  g_task_send.enable();

  g_task_cal.set(TASK_IMMEDIATE, TASK_FOREVER, CalibrationTaskFn);
  user_scheduler.addTask(g_task_cal);
  g_task_cal.disable();  // Enabled by CalibrationStart().

  Serial.println(F("LEAF ready. Type 'help'."));
}

void loop() {
  mesh.update();
  ProcessConsoleLeaf();
}

#endif  // MESH_IS_ROOT
