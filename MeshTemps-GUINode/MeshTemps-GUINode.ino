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

#include "mesh_node.h" // make NodeMetaRecord & MeshNode visible before auto-prototypes
#include "serial_console.h"

// The GUI board talks to the bridge over the default UART0 header (U0TXD/U0RXD)
// while keeping Serial (USB CDC) for the PC console. Do not remap to the bridge
// pins (17/18); those are reserved for the bridge firmware's dedicated UART.
#ifndef BRIDGE_GUI_TX_PIN
#define BRIDGE_GUI_TX_PIN 43  // U0TXD on ESP32-S3
#endif
#ifndef BRIDGE_GUI_RX_PIN
#define BRIDGE_GUI_RX_PIN 44  // U0RXD on ESP32-S3
#endif
#include "../serial_protocol.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <algorithm>
#include <array>
#include <cstring> // for memcmp in NVS verification
#include <math.h>
#include <painlessMesh.h>
#include <vector>

#include "Config.h" // Mesh / IO configuration, addrToHex()
#include "esp_panel_board_custom_conf.h"
#include "lvgl_v8_port.h"
#include <WiFi.h>
#include <algorithm> // for std::sort
#include <esp_display_panel.hpp>
#include <esp_sntp.h>
#include <esp_wifi.h> // for wifi_auth_mode_t and auth enums
#include <limits>
#include <lvgl.h>
#include <map>
#include <sys/time.h> // for settimeofday()
#include <time.h>

using esp_panel::board::Board;
using esp_panel::drivers::BusRGB;
using esp_panel::drivers::LCD;

#include "mesh_tile.h"

using Address = std::array<uint8_t, 8>; // DS18B20 64-bit ROM code

static SerialConsole g_console;

// ---------------------------------------------------------------------------
// Map view definitions (root)
// ---------------------------------------------------------------------------

// High-level UI mode: traditional node tiles vs house map.
enum UiViewMode {
  kUiViewModeTiles = 0,
  kUiViewModeMap = 1,
};

UiViewMode g_ui_view_mode = kUiViewModeTiles;

// When true, the house map uses "Underbelly" sensors instead of "Room".
bool g_map_use_underbelly = false;

// When true, the screen shows map, not tiles
bool g_screen_use_map = false;

// Normalized rectangle inside a 0..1000 x 0..1000 logical space.
struct RoomRectNorm {
  int16_t x1;
  int16_t y1;
  int16_t x2;
  int16_t y2;
};

// Static description of one logical room on the map.
struct RoomDef {
  const char *id;           // canonical id for matching node labels
  const char *display_name; // label printed in the UI
  bool is_outside;          // true for outdoor zone
  uint8_t rect_count;       // 1 or 2
  RoomRectNorm rects[2];    // up to two rectangles (L-shapes)
};

// Coordinates are in a 0..1000 x 0..1000 "house" space. These are
// deliberately approximate; you can tweak them later without touching logic.
static const RoomDef kRoomDefs[] = {
    // id,             display_name,     is_outside, rect_count, {{x1,y1,x2,y2},
    // ...}
    // Done
    {"frontroom",
     "Front Room",
     false,
     1,
     {{418, 498, 764, 1000}, {0, 0, 0, 0}}},

    // Done
    {"livingroom",
     "Living Room",
     false,
     2,
     {{156, 0, 418, 498}, {156, 0, 233, 671}}},

    // Done
    {"masterbedroom",
     "Master Bedroom",
     false,
     1,
     {{764, 498, 1000, 1000}, {0, 0, 0, 0}}},

    // Done
    {"masterbath",
     "Master Bath",
     false,
     2,
     {{764, 288, 1000, 498}, {880, 0, 1000, 498}}},

    // Done
    {"bath", "Bath", false, 1, {{156, 670, 233, 1000}, {0, 0, 0, 0}}},

    // Done
    {"corner", "Corner", false, 1, {{0, 498, 156, 1000}, {0, 0, 0, 0}}},

    // Done
    {"kidsroom", "Kids Room", false, 1, {{233, 498, 418, 1000}, {0, 0, 0, 0}}},

    // Done
    {"nathans", "Nathan's", false, 1, {{0, 0, 156, 498}, {0, 0, 0, 0}}},

    // Kitchen as an L-shape: main body + leg.
    {"kitchen",
     "Kitchen",
     false,
     2,
     {{418, 0, 764, 498}, {418, 0, 880, 288}}}, // leg

    // Outdoor zone: drawn outside the house, in map-container space.
    {"outside", "Outside", true, 1, {{0, 0, 250, 250}, {0, 0, 0, 0}}},
};

constexpr size_t kRoomCount = sizeof(kRoomDefs) / sizeof(kRoomDefs[0]);

// ---------------------------------------------------------------------------
// House / outside layout configuration
// ---------------------------------------------------------------------------

// House placement relative to the map container.
// Negative values mean "auto": size = 80% of container, centered.
constexpr lv_coord_t kHousePosXPx = -1;   // e.g. 40 to pin X, or -1 for auto
constexpr lv_coord_t kHousePosYPx = -1;   // e.g. 20 to pin Y, or -1 for auto
constexpr lv_coord_t kHouseWidthPx = -1;  // e.g. 280 to fix width, or -1
constexpr lv_coord_t kHouseHeightPx = -1; // e.g. 200 to fix height, or -1

// "Outside" label position in absolute pixels in the map container.
constexpr lv_coord_t kOutsideLabelPosXPx = 10;
constexpr lv_coord_t kOutsideLabelPosYPx = 4;

// When position/size are auto (<0), place the house inside the "outside"
// area with these margins (in the map container's inner coordinate space).
constexpr lv_coord_t kHouseAutoMarginLeftPx = 8;
constexpr lv_coord_t kHouseAutoMarginRightPx = 8;
constexpr lv_coord_t kHouseAutoMarginBottomPx = 8;

// Vertical gap between the outside label and the top of the house rectangle.
constexpr lv_coord_t kHouseAutoGapBelowOutsideLabelPx = 20;

// Fallback top margin if there is no outside label.
constexpr lv_coord_t kHouseAutoMarginTopPx = 40;

// Track the last house size we configured so we don't depend on
// lv_obj_get_width()
static lv_coord_t g_house_w = 0;
static lv_coord_t g_house_h = 0;

struct RoomWidget {
  const RoomDef *def = nullptr;
  lv_obj_t *rect_objs[2] = {nullptr, nullptr};
  lv_obj_t *label_obj = nullptr;

  float temp_c = NAN;
  bool has_value = false;
  bool is_alert = false;
  bool is_warning = false;
  bool is_missing = true;
  bool sequence_stuck = false;
  bool node_present = false;

  lv_color_t base_color;      // normal background color (temp-mapped)
  lv_color_t base_text_color; // normal text color (typically white)
  bool flash_enabled = false;
};

std::vector<RoomWidget> g_room_widgets;

// Map containers.
lv_obj_t *g_ui_map_container = nullptr;
lv_obj_t *g_ui_map_house = nullptr;

// Map helpers (implemented later).
static String CanonicalRoomId(const String &input);
static MeshNode *FindNodeForRoom(const RoomDef &def);
static lv_color_t ColorForTemperature(float temp_c, bool has_value);
static void RoomMapBuildWidgets();
static void RoomMapRebuild(uint32_t now_ms);
static void RoomMapRefresh(uint32_t now_ms);
static void RoomMapLoop(uint32_t now_ms);
static void RoomMapSetViewMode(UiViewMode mode);
static void BellyButtonEvent(lv_event_t *e);
static void ViewButtonEvent(lv_event_t *e);
static void SilenceButtonEvent(lv_event_t *e);
static void UpdateSilenceButtonAppearance();
static void BuzzerStartTestInternal(bool use_alert_profile);
static void BuzzerStopTestInternal();

bool g_topology_persist_enabled = true;

// Buzzer (root)
constexpr int kBuzzerPin = 6;
constexpr uint32_t kDefaultBeepLenMs = 150;   // ms buzzer ON
constexpr uint32_t kDefaultGapWarnMs = 10000; // ms between warning beeps
constexpr uint32_t kDefaultGapAlertMs = 5000; // ms between alert beeps

// NVS keys (must be <= 15 chars due to NVS key limits).
constexpr const char kKeyBeepLen[] = "beep_len_ms";
constexpr const char kKeyBeepWarnGap[] = "beep_warn_gap";
constexpr const char kKeyBeepAlertGap[] = "beep_alert_gap";
constexpr const char kKeyBeepCooldown[] = "beep_cooldown";
// Legacy keys from the monolithic root sketch (too long for NVS but kept for
// read-back in case they were ever persisted on compatible builds).
constexpr const char kKeyBeepWarnLegacy[] = "beep_gap_warn_ms";
constexpr const char kKeyBeepAlertLegacy[] = "beep_gap_alert_ms";
constexpr const char kKeyBeepCooldownLegacy[] = "beep_cooldown_ms";

uint32_t g_beep_len_ms = kDefaultBeepLenMs;
uint32_t g_beep_gap_warn_ms = kDefaultGapWarnMs;
uint32_t g_beep_gap_alert_ms = kDefaultGapAlertMs;

// Global silence / cooldown for the buzzer.
constexpr uint32_t kDefaultBuzzerCooldownMs = 5UL * 60UL * 1000UL; // 5 min
uint32_t g_buzzer_cooldown_ms = kDefaultBuzzerCooldownMs;

enum BuzzerSilenceState : uint8_t {
  kBuzzerSilenceIdle = 0,      // normal operation, buzzer allowed
  kBuzzerSilenceActive = 1,    // user pressed "Silence"; current alerts suppressed
  kBuzzerSilenceCooldown = 2,  // alerts have cleared; ignore retriggers until cooldown ends
};

BuzzerSilenceState g_buzzer_silence_state = kBuzzerSilenceIdle;
uint32_t g_buzzer_silence_rearm_at_ms = 0;  // millis when we can re-arm (0 = not scheduled)

// Runtime buzzer state (moved out of BuzzerLoop so the UI handler can touch it).
bool g_buzzer_on = false;
uint32_t g_buzzer_beep_start_ms = 0;
uint32_t g_buzzer_last_beep_ms = 0;

// Buzzer test mode (console-controlled).
bool g_buzzer_test_active = false;
// true  => use alert pattern (beep_gap_alert_ms)
// false => use warning pattern (beep_gap_warn_ms)
bool g_buzzer_test_use_alert_pattern = true;

// Flashing (root)
constexpr uint32_t kDefaultFlashIntervalMs =
    1000; // ms between flash toggles; 0 = off
constexpr const char kKeyFlashInterval[] = "flash_int_ms";
constexpr const char kKeyFlashIntervalLegacy[] = "flash_interval_ms";
uint32_t g_flash_interval_ms = kDefaultFlashIntervalMs;

// Persistent root prefs.
Preferences g_root_preferences;

// Mesh tasks.
Task g_task_announce;

// Units / highlight config.
bool g_display_fahrenheit = true; // true = °F, false = °C
bool g_highlight_missing_nodes = true;
uint32_t g_stale_minutes_threshold = 5; // minutes

// NEW: sequence-health threshold.
// Consider a node "sequence-stuck" if its seq has not advanced for at least
// this many milliseconds while we continue to receive duplicate seq values.
// Using 10x SEND_PERIOD_MS keeps this lenient even with some packet loss.
constexpr uint32_t kSeqStuckMsThreshold = SEND_PERIOD_MS * 10UL;

// Dummy data mode (non-persistent).
bool g_use_dummy_data = false;

// Tile display toggles (root).
bool g_show_sensor_labels = true; // show sensor names in tiles
bool g_show_age_label = true;     // show "Age: N min" in tiles

// Temperature limits (in °C, NaN = disabled).
constexpr float kDefaultLimitHysteresisC = 0.5f * (5.0f / 9.0f); // 0.5°F
float g_warn_low_c = 5.0;
float g_warn_high_c = 26.0;
float g_alert_low_c = 3.0;
float g_alert_high_c = 30.0;
float g_limit_hysteresis_c = kDefaultLimitHysteresisC;

// Aggregate state for buzzer.
bool g_any_warning = false;
bool g_any_alert = false;

// ntfy notifications
struct NtfyConfig {
  bool enabled = true;
  bool summary_enabled = true;
  uint32_t summary_period_min = 15; // minutes
};

enum class AlertState { kNormal = 0, kWarning = 1, kAlert = 2, kMissing = 3 };

NtfyConfig g_ntfy_config;
std::map<String, AlertState> g_ntfy_states;
uint32_t g_ntfy_last_summary_ms = 0;
static uint32_t g_ntfy_next_sequence = 1;
static constexpr uint32_t kNtfyResendIntervalMs = 5000; // rate limit GUI->bridge reissues

struct QueuedNtfy {
  uint32_t sequence = 0;
  String message;
  String title;
  bool is_summary = false;
  bool cache_when_offline = false;
  uint32_t last_send_ms = 0;
  bool needs_resend = true;
};

static std::vector<QueuedNtfy> g_ntfy_pending_bridge;
static uint32_t g_ntfy_next_send_ms = 0;

// Top bar height in pixels; must match GuiInit().
constexpr lv_coord_t kTopBarHeightPx = 36;

// LVGL GUI state (root).
lv_obj_t *g_ui_label_title = nullptr;
lv_obj_t *g_ui_label_peers = nullptr;
lv_obj_t *g_ui_label_clock = nullptr;
lv_obj_t *g_ui_tile_container = nullptr;

lv_obj_t *g_ui_button_belly = nullptr;
lv_obj_t *g_ui_button_belly_label = nullptr;

lv_obj_t *g_ui_button_map = nullptr;
lv_obj_t *g_ui_button_map_label = nullptr;

lv_obj_t *g_ui_button_silence = nullptr;
lv_obj_t *g_ui_button_silence_label = nullptr;

lv_obj_t *g_ui_ntp_icon = nullptr;
lv_obj_t *g_ui_ntp_popup = nullptr;

volatile bool g_layout_dirty = false; // expensive: create/reflow tiles
volatile bool g_values_dirty = false; // cheap: rewrite texts/colors only

volatile size_t g_ui_peers = 0;

Board *g_board = nullptr;

void GuiRebuildTiles(); // forward
static void SendNtfyRequestToBridge(const String &message,
                                    bool cache_when_offline, bool is_summary,
                                    const char *title = nullptr);
// BuildTileContentForNode is defined later, after mesh_tile.h is included.
// No extra prototype needed here.

// ---------------------------------------------------------------------------
// WiFi + NTP configuration (root)
// ---------------------------------------------------------------------------

// Track what credentials we have actually pushed into the WiFi driver.
static String g_last_applied_ssid;
static String g_last_applied_password;

struct NetworkConfig {
  String ssid;
  String password;
  // Timezone offset in minutes from UTC, e.g. -360 = UTC-6 (CST).
  int32_t timezone_minutes = 0;
  bool dst_enabled = false; // when true, add +60 minutes
};

static NetworkConfig g_network_config;

// NTP timing.
constexpr uint32_t kWifiConnectTimeoutMs = 20000; // 20 s max wait
constexpr uint32_t kNtpSyncTimeoutMs = 15000;     // 15 s to wait for time
constexpr uint32_t kNtpResyncPeriodMs = 24UL * 60UL * 60UL * 1000UL; // 24 hours

// Retry faster until we have at least one successful NTP sync.
constexpr uint32_t kNtpRetryPeriodMs = 300000UL; // 5 minute

constexpr uint32_t kNtpRetryInitialMs = 5UL * 60UL * 1000UL;     // 5 min
constexpr uint32_t kNtpRetryMaxMs = 12UL * 60UL * 60UL * 1000UL; // 12 h

static bool g_ntp_time_valid = false;
static uint32_t g_last_ntp_attempt_ms = 0;
static uint32_t g_last_ntp_ok_ms = 0;
static uint32_t g_ntp_retry_period_ms = kNtpRetryInitialMs;
static bool g_ntp_sync_in_progress = false;
static uint32_t g_last_time_request_ms = 0;

// NTP servers.
static const char *kNtpServer1 = "pool.ntp.org";
static const char *kNtpServer2 = "time.nist.gov";

// Track completion from the SNTP callback without blocking.
static volatile bool g_ntp_cb_pending = false;
// Epoch at last SNTP sync (UTC seconds since 1970).
static volatile time_t g_ntp_last_sync_epoch = 0;

// History / time-mapping: track whether we've already back-filled
// MeshNode history using the first valid NTP fix.
static bool g_history_time_backfilled = false;

// Forward declaration; implemented below.
static void OnFirstNtpTimeSync(time_t epoch_now, uint32_t now_ms);


// Per-node alert/warning mute under g_labels["mute"][nodeId]:
// bit0 = LOW side muted; bit1 = HIGH side muted.
enum NodeMuteMask : uint8_t {
  kMuteNone = 0,
  kMuteLow = 1 << 0,
  kMuteHigh = 1 << 1,
  kMuteBoth = kMuteLow | kMuteHigh
};

// ============================================================================
// History configuration: NVS persistence
// ============================================================================

constexpr const char *kHistoryPrefsNamespace = "mesh_hist"; // adjust if needed
constexpr const char *kHistoryKeyIntervalMs = "histIntMs";
constexpr const char *kHistoryKeyRetentionDays = "histRetDays";

// Load history logging configuration (interval + retention) from NVS and
// apply it to MeshNode static configuration.
void LoadHistoryConfigFromNvs() {
  Preferences preferences;

  if (!preferences.begin(kHistoryPrefsNamespace, /*readOnly=*/true)) {
    // If this fails we just keep the compiled-in defaults.
    Serial.println(F("History NVS: begin() failed, using defaults."));
    return;
  }

  const uint32_t default_interval_ms = MeshNode::history_interval_ms();
  const uint32_t default_retention_days = MeshNode::history_retention_days();

  const uint32_t interval_ms =
      preferences.getULong(kHistoryKeyIntervalMs, default_interval_ms);
  const uint32_t retention_days =
      preferences.getUInt(kHistoryKeyRetentionDays, default_retention_days);

  preferences.end();

  MeshNode::SetHistoryConfig(interval_ms, retention_days);

  Serial.print(F("History NVS: interval_ms="));
  Serial.print(interval_ms);
  Serial.print(F(" ms, retention_days="));
  Serial.println(retention_days);
}

// Save current MeshNode history configuration back to NVS.
void SaveHistoryConfigToNvs() {
  Preferences preferences;

  if (!preferences.begin(kHistoryPrefsNamespace, /*readOnly=*/false)) {
    Serial.println(F("History NVS: begin() failed, NOT saving."));
    return;
  }

  const uint32_t interval_ms = MeshNode::history_interval_ms();
  const uint32_t retention_days = MeshNode::history_retention_days();

  preferences.putULong(kHistoryKeyIntervalMs, interval_ms);
  preferences.putUInt(kHistoryKeyRetentionDays, retention_days);

  preferences.end();

  Serial.print(F("History NVS: saved interval_ms="));
  Serial.print(interval_ms);
  Serial.print(F(" ms, retention_days="));
  Serial.println(retention_days);
}

// -----------------------------------------------------------------------------
// Shared mesh and logging
// -----------------------------------------------------------------------------

Scheduler user_scheduler;
painlessMesh mesh;

// Mesh channel: default from Config.h but can be overridden at runtime
int32_t g_mesh_channel = MESH_CHANNEL;

bool g_debug_enabled = false;

#define DLOG(fmt, ...)                                                         \
  do {                                                                         \
    if (g_debug_enabled) {                                                     \
      Serial.printf((fmt), ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

// Forward declarations used on ROOT builds from LogConnections().
void GuiUpdateNetwork(size_t peers);
void GuiRequestRender();

static void DumpStringHex(const char *label, const String &value) {
  Serial.printf("%s (len=%d):", label, static_cast<int>(value.length()));
  for (int i = 0; i < value.length(); ++i) {
    Serial.printf(" %02X", static_cast<uint8_t>(value[i]));
  }
  Serial.println();
}

void LogConnections() {
  const size_t peer_count = mesh.getNodeList().size();
  DLOG("Peers: %u\n", static_cast<unsigned int>(peer_count));

  GuiUpdateNetwork(peer_count);
  GuiRequestRender();
}

// Join argv[start..argc-1] with spaces.
static String JoinTokens(int argc, const String argv[], int start) {
  String out;
  for (int i = start; i < argc; ++i) {
    if (i > start)
      out += ' ';
    out += argv[i];
  }
  return out;
}

// Echo the full command line (argv[0..argc-1]) before executing a handler.
static void PrintCommandHeader(Print &out, int argc, const String argv[]) {
  // Always print a line so you can see that the handler actually fired,
  // even if argc is 0 for some reason.
  const String line = JoinTokens(argc, argv, 0);
  out.print(F("> "));
  out.println(line);
}

// Prints document usage in both ArduinoJson v6 and v7.
static void PrintJsonStats(Print &out, const char *name,
                           const JsonDocument &doc) {
#if defined(ARDUINOJSON_VERSION_MAJOR) && (ARDUINOJSON_VERSION_MAJOR >= 7)
  const unsigned usage = 0u; // v7: memoryUsage() deprecated, returns 0
  const bool over = doc.overflowed();
  out.printf("%s: usage=%uB%s\n", name, usage, over ? " (OVERFLOW)" : "");
#else
  const unsigned usage = static_cast<unsigned>(doc.memoryUsage());
  const bool over = doc.overflowed();
  const unsigned cap = static_cast<unsigned>(doc.capacity());
  out.printf("%s: usage=%u/%uB%s\n", name, usage, cap,
             over ? " (OVERFLOW)" : "");
#endif
}

// -----------------------------------------------------------------------------
// Shared NVS write-then-read verification helpers (root + leaf)
// -----------------------------------------------------------------------------
// These helpers assume Preferences.begin() has already been called on the
// correct namespace and that Preferences.end() will be called by the caller.

static void NvsLogVerifyFailure(const char *key, const char *reason) {
  Serial.printf("[NVS VERIFY] key=\"%s\" FAILED: %s\n", key, reason);
}

static bool NvsPutBytesVerified(Preferences &preferences, const char *key,
                                const void *data, size_t length_bytes) {
  if (data == nullptr || length_bytes == 0) {
    NvsLogVerifyFailure(key, "null pointer or zero length");
    return false;
  }

  const size_t written =
      preferences.putBytes(key, data, static_cast<size_t>(length_bytes));
  if (written != length_bytes) {
    NvsLogVerifyFailure(key, "putBytes wrote wrong length");
    return false;
  }

  if (!preferences.isKey(key)) {
    NvsLogVerifyFailure(key, "key missing after putBytes");
    return false;
  }

  const size_t stored_length = preferences.getBytesLength(key);
  if (stored_length != length_bytes) {
    NvsLogVerifyFailure(key, "getBytesLength mismatch");
    return false;
  }

  std::vector<uint8_t> verify_buffer(length_bytes);
  const size_t read_back = preferences.getBytes(
      key, verify_buffer.data(), static_cast<size_t>(length_bytes));
  if (read_back != length_bytes) {
    NvsLogVerifyFailure(key, "getBytes read wrong length");
    return false;
  }

  if (memcmp(data, verify_buffer.data(), length_bytes) != 0) {
    NvsLogVerifyFailure(key, "content mismatch after read-back");
    return false;
  }
  return true;
}

static bool NvsPutIntVerified(Preferences &preferences, const char *key,
                              int32_t value) {
  const size_t written = preferences.putInt(key, value);
  if (written != sizeof(int32_t)) {
    NvsLogVerifyFailure(key, "putInt wrote wrong length");
    return false;
  }

  if (!preferences.isKey(key)) {
    NvsLogVerifyFailure(key, "key missing after putInt");
    return false;
  }

  const int32_t read_back = preferences.getInt(key, 0);
  if (read_back != value) {
    NvsLogVerifyFailure(key, "value mismatch after read-back");
    return false;
  }
  return true;
}

static bool NvsPutULongVerified(Preferences &preferences, const char *key,
                                uint32_t value) {
  (void)preferences.putULong(key, value);

  if (!preferences.isKey(key)) {
    NvsLogVerifyFailure(key, "key missing after putULong");
    return false;
  }

  const uint32_t read_back = preferences.getULong(key, 0);
  if (read_back != value) {
    NvsLogVerifyFailure(key, "value mismatch after read-back");
    return false;
  }
  return true;
}

static uint32_t NvsGetULongWithFallback(Preferences &preferences,
                                        const char *primary_key,
                                        const char *legacy_key,
                                        uint32_t default_value) {
  if (preferences.isKey(primary_key)) {
    return preferences.getULong(primary_key, default_value);
  }
  if (legacy_key != nullptr && preferences.isKey(legacy_key)) {
    return preferences.getULong(legacy_key, default_value);
  }
  return default_value;
}

static bool NvsPutFloatVerified(Preferences &preferences, const char *key,
                                float value) {
  const size_t written = preferences.putFloat(key, value);
  if (written != sizeof(float)) {
    NvsLogVerifyFailure(key, "putFloat wrote wrong length");
    return false;
  }

  if (!preferences.isKey(key)) {
    NvsLogVerifyFailure(key, "key missing after putFloat");
    return false;
  }

  const float read_back = preferences.getFloat(key, NAN);
  if (!isfinite(read_back) || read_back != value) {
    NvsLogVerifyFailure(key, "value mismatch after read-back");
    return false;
  }
  return true;
}

static bool NvsPutStringVerified(Preferences &preferences, const char *key,
                                 const String &value) {
  // putString may return length including terminator, so just rely on
  // read-back comparison instead of byte-count.
  (void)preferences.putString(key, value);

  if (!preferences.isKey(key)) {
    NvsLogVerifyFailure(key, "key missing after putString");
    return false;
  }

  const String read_back = preferences.getString(key, "");
  if (read_back != value) {
    NvsLogVerifyFailure(key, "string mismatch after read-back");
    return false;
  }
  return true;
}

static bool NvsRemoveKeyVerified(Preferences &preferences, const char *key) {
  preferences.remove(key);
  if (preferences.isKey(key)) {
    NvsLogVerifyFailure(key, "remove() did not clear key");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// WiFi + timezone persistence (meshroot namespace)
// ---------------------------------------------------------------------------

static void LoadNetworkConfigFromNVS() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);

  g_network_config.ssid = g_root_preferences.getString("wifi_ssid", String());
  g_network_config.password =
      g_root_preferences.getString("wifi_pwd", String());

  const int32_t tz_min =
      g_root_preferences.getInt("tz_min", g_network_config.timezone_minutes);
  const int32_t dst_flag =
      g_root_preferences.getInt("tz_dst", g_network_config.dst_enabled ? 1 : 0);

  g_root_preferences.end();

  g_network_config.timezone_minutes = tz_min;
  g_network_config.dst_enabled = (dst_flag != 0);

  // DumpStringHex("[NVS] SSID", g_network_config.ssid);
  // DumpStringHex("[NVS] PASSWORD", g_network_config.password);
}

static void SaveNetworkConfigToNVS() {
  Serial.println(F("Saving WiFi / timezone settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);

  bool is_successful = true;
  is_successful = NvsPutStringVerified(g_root_preferences, "wifi_ssid",
                                       g_network_config.ssid) &&
                  is_successful;
  is_successful = NvsPutStringVerified(g_root_preferences, "wifi_pwd",
                                       g_network_config.password) &&
                  is_successful;
  is_successful = NvsPutIntVerified(g_root_preferences, "tz_min",
                                    static_cast<int32_t>(
                                        g_network_config.timezone_minutes)) &&
                  is_successful;
  is_successful = NvsPutIntVerified(g_root_preferences, "tz_dst",
                                    g_network_config.dst_enabled ? 1 : 0) &&
                  is_successful;

  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveNetworkConfigToNVS failed"));
  }
}

// Try to find the configured SSID (from NVS) and use its channel for the mesh.
// Returns >0 on success, -1 on failure (no SSID, not found, etc.).
static int32_t ScanChannelForConfiguredSsid(Print &out) {
  if (g_network_config.ssid.isEmpty()) {
    out.println(F("[WiFi] No SSID configured; cannot auto-pick mesh channel"));
    return -1;
  }

  out.printf("[WiFi] Scanning for SSID \"%s\" to choose mesh channel...\n",
             g_network_config.ssid.c_str());

  // STA mode only for scanning.
  WiFi.mode(WIFI_STA);
  // Disconnect but keep radio on.
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/true);
  delay(100);

  int32_t found_channel = -1;
  int best_rssi = -200;

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

// Root-only helper to set g_mesh_channel before mesh.init().
static void RootPickMeshChannel() {
  // Make sure we have SSID/password in g_network_config.
  LoadNetworkConfigFromNVS();

  int32_t chan = ScanChannelForConfiguredSsid(Serial);
  if (chan > 0) {
    g_mesh_channel = chan;
  } else {
    g_mesh_channel = MESH_CHANNEL; // fallback
  }
}

// ---------------------------------------------------------------------------
// WiFi + NTP core helpers
// ---------------------------------------------------------------------------

static bool EnsureMeshStationConfigApplied(Print &out,
                                           bool force_reapply = false) {
  if (g_network_config.ssid.isEmpty()) {
    out.println(F("[WiFi] SSID not configured; use 'wifi ssid ...' first"));
    return false;
  }

  const bool creds_changed =
      (g_network_config.ssid != g_last_applied_ssid) ||
      (g_network_config.password != g_last_applied_password);

  if (force_reapply || creds_changed) {
    out.println(F("[WiFi] Applying STA credentials via painlessMesh"));

    // Root behaviour for painlessMesh
    mesh.setRoot(true);
    mesh.setContainsRoot(true);
    mesh.stationManual(g_network_config.ssid.c_str(),
                       g_network_config.password.c_str());

    g_last_applied_ssid = g_network_config.ssid;
    g_last_applied_password = g_network_config.password;
  }

  return true;
}

static bool EnsureWifiConnected(Print &out) {
  // Ensure the mesh has the current STA credentials.
  if (!EnsureMeshStationConfigApplied(out)) {
    return false;
  }

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    out.printf("[WiFi] Connected; IP=%s RSSI=%d dBm\n",
               ip.toString().c_str(), WiFi.RSSI());
    return true;
  }

  // Do not block here – STA connection is managed by painlessMesh.
  out.printf(
      "[WiFi] STA credentials applied; current status=%d. "
      "Root will connect via painlessMesh in the background.\n",
      static_cast<int>(status));
  return false;
}


// Set local clock to a fixed default of 2025-01-01 00:00.
static void SetDefaultDateTime() {
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = 2025 - 1900;
  t.tm_mon = 0;  // January
  t.tm_mday = 1; // 1st
  t.tm_hour = 0;
  t.tm_min = 0;
  t.tm_sec = 0;

  time_t epoch = mktime(&t);
  struct timeval now;
  now.tv_sec = epoch;
  now.tv_usec = 0;
  settimeofday(&now, nullptr);

  Serial.println(F("[TIME] Default date/time set to 2025-01-01 00:00"));
}

// Print current local time if available.
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

// SNTP time-sync notification callback (called from the SNTP task).
static void NtpTimeSyncNotification(struct timeval *tv) {
  // Just record that a sync happened; do the heavier work in NtpLoop().
  g_ntp_cb_pending = true;
  if (tv != nullptr) {
    g_ntp_last_sync_epoch = tv->tv_sec;
  } else {
    g_ntp_last_sync_epoch = time(nullptr);
  }
}

// NTP sync using current timezone / DST settings.
// NTP sync using current timezone / DST settings (non-blocking).
static bool SyncTimeFromNtp(Print &out) {
  if (WiFi.status() != WL_CONNECTED) {
    out.println(F("[NTP] WiFi not connected; cannot sync time"));
    return false;
  }

  // If a sync is already in progress, do not start another.
  if (g_ntp_sync_in_progress) {
    out.println(F("[NTP] Sync already in progress"));
    return true;  // Request is effectively already pending.
  }

  const long gmt_offset_sec =
      static_cast<long>(g_network_config.timezone_minutes) * 60L;
  const long dst_offset_sec = g_network_config.dst_enabled ? 3600L : 0L;

  out.printf("[NTP] configTime offset=%ld sec dst=%ld sec\n",
             gmt_offset_sec, dst_offset_sec);

  // Install (or re-install) the SNTP callback; this is idempotent.
  sntp_set_time_sync_notification_cb(NtpTimeSyncNotification);

  // Start / reconfigure the SNTP client. This returns immediately; SNTP will
  // update the system time later in the background and then invoke our
  // NtpTimeSyncNotification() callback.
  configTime(gmt_offset_sec, dst_offset_sec, kNtpServer1, kNtpServer2);

  g_ntp_sync_in_progress = true;
  g_last_ntp_attempt_ms = millis();

  out.println(F("[NTP] Sync requested (non-blocking)"));
  return true;
}


static void RootInitNetwork() {
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

  // Apply STA configuration to painlessMesh. This does NOT block;
  // the mesh will bring the STA link up when it can.
  EnsureMeshStationConfigApplied(Serial);
  Serial.println(F(
      "[WiFi] STA credentials applied; root mesh node will connect to WiFi "
      "in the background when possible."));

  // Start from a sane default local time; NTP will replace this once the
  // first successful sync occurs.
  SetDefaultDateTime();
  g_ntp_time_valid = false;

  // Configure initial retry period for NTP. The actual sync attempts
  // are handled in NtpLoop().
  g_ntp_retry_period_ms = kNtpRetryInitialMs;
  g_last_ntp_attempt_ms = millis();
}

// Called exactly once on the first successful NTP sync.
// epoch_now:   Unix epoch (seconds since 1970) at "now_ms".
// now_ms:      root millis() when we process the sync.
static void OnFirstNtpTimeSync(time_t epoch_now, uint32_t now_ms) {
  if (g_history_time_backfilled) {
    return;
  }

  if (epoch_now <= 0) {
    Serial.println(F("[HIST] OnFirstNtpTimeSync: invalid epoch, skipping"));
    return;
  }

  if (now_ms == 0U) {
    // Extremely early in boot; just wait for the next sync instead.
    Serial.println(F("[HIST] OnFirstNtpTimeSync: now_ms=0, skipping"));
    return;
  }

  Serial.printf(
      "[HIST] First NTP sync: epoch_now=%ld now_ms=%lu; backdating history.\n",
      static_cast<long>(epoch_now),
      static_cast<unsigned long>(now_ms));

  // Hand off to MeshNode:
  //  - store the mapping epoch_now <-> now_ms, and
  //  - back-fill any existing HistorySample entries that only have
  //    ms-since-boot timestamps.
  MeshNode::OnFirstTimeSync(epoch_now, now_ms);

  g_history_time_backfilled = true;
}


// Periodic NTP resync – call from loop(). Non-blocking.
static void NtpLoop() {
  const uint32_t now_ms = millis();

  // 1) Handle completion from the SNTP callback (if any).
  if (g_ntp_cb_pending) {
    g_ntp_cb_pending = false;

    g_ntp_sync_in_progress = false;
    g_ntp_time_valid = true;
    g_last_ntp_ok_ms = now_ms;
    g_ntp_retry_period_ms = kNtpResyncPeriodMs;  // switch to ~24 h

    // Log the time we just synced to, so it matches what "time now" would show.
    struct tm timeinfo;
    time_t t = g_ntp_last_sync_epoch;
    bool have_time = false;

    if (t != 0 && localtime_r(&t, &timeinfo) != nullptr) {
      have_time = true;
    } else if (getLocalTime(&timeinfo, 0)) {
      // Fallback: read back current local time without blocking.
      have_time = true;
    }

    if (have_time) {
      char buf[64];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      Serial.printf("[NTP] Time synced: %s\n", buf);
    } else {
      Serial.println(F("[NTP] Time sync callback, but getLocalTime() failed"));
    }

    // Once we have a valid epoch and a millis() reference, back-date
    // all existing history samples that only have ms-since-boot timestamps.
    if (t > 0 && !g_history_time_backfilled) {
      OnFirstNtpTimeSync(t, now_ms);
    }
  }

  // If no SSID configured, nothing to do.
  if (g_network_config.ssid.isEmpty()) {
    return;
  }

  // 2) Manage "in progress" state with a timeout, but do not block.
  if (g_ntp_sync_in_progress) {
    // If this attempt has run too long, treat it as a failure and backoff.
    if (now_ms - g_last_ntp_attempt_ms >= kNtpSyncTimeoutMs) {
      g_ntp_sync_in_progress = false;
      Serial.println(F("[NTP] Sync attempt timed out; will retry later"));

      if (!g_ntp_time_valid) {
        if (g_ntp_retry_period_ms < kNtpRetryMaxMs) {
          g_ntp_retry_period_ms =
              std::min(g_ntp_retry_period_ms * 2U, kNtpRetryMaxMs);
        }
      }

      // Record that we just finished an attempt (unsuccessfully).
      g_last_ntp_attempt_ms = now_ms;
    }
    // Either way, do not start a new attempt while one is/was in progress.
    return;
  }

  // 3) Check if it is time to start a new attempt.
  if ((g_last_ntp_attempt_ms != 0U) &&
      ((now_ms - g_last_ntp_attempt_ms) < g_ntp_retry_period_ms)) {
    return;  // not time yet
  }

  // 4) Ensure WiFi is connected before starting a new sync attempt.
  if (!EnsureWifiConnected(Serial)) {
    Serial.println(F("[NTP] Skipping resync; WiFi not connected"));

    // Before first valid fix, back off exponentially up to 12 h.
    if (!g_ntp_time_valid) {
      if (g_ntp_retry_period_ms < kNtpRetryMaxMs) {
        g_ntp_retry_period_ms =
            std::min(g_ntp_retry_period_ms * 2U, kNtpRetryMaxMs);
      }
    }
    g_last_ntp_attempt_ms = now_ms;
    return;
  }

  // 5) Start a new non-blocking sync attempt.
  (void)SyncTimeFromNtp(Serial);
}


// ---------------------------------------------------------------------------
// Storage helpers
// ---------------------------------------------------------------------------

static uint8_t GetNodeMuteMask(const String &node_id) {
  uint32_t id_u32 = 0;
  if (!ParseNodeIdToU32(node_id, &id_u32)) {
    return kMuteNone;
  }
  MeshNode *node = FindMeshNode(id_u32);
  if (node == nullptr) {
    return kMuteNone;
  }
  return static_cast<uint8_t>(node->mute_mask() & 0x3u);
}

static const char *NodeMuteMaskToString(uint8_t m) {
  switch (m & 0x3) {
  case kMuteNone:
    return "off";
  case kMuteLow:
    return "low";
  case kMuteHigh:
    return "high";
  default:
    return "both";
  }
}

static void LoadNodeMetaFromNVS() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const size_t nbytes = g_root_preferences.getBytesLength("node_meta");
  if (nbytes == 0 || (nbytes % sizeof(NodeMetaRecord)) != 0) {
    g_root_preferences.end();
    return;
  }
  const size_t count = nbytes / sizeof(NodeMetaRecord);
  if (count == 0) {
    g_root_preferences.end();
    return;
  }

  std::vector<NodeMetaRecord> records;
  records.resize(count);
  g_root_preferences.getBytes("node_meta", records.data(),
                              count * sizeof(NodeMetaRecord));
  g_root_preferences.end();

  for (size_t i = 0; i < records.size(); ++i) {
    const NodeMetaRecord &rec = records[i];

    // Only apply metadata to nodes that already exist (from known topology
    // or live traffic). Do NOT create nodes here.
    MeshNode *node = FindMeshNode(rec.node_id);
    if (node == nullptr) {
      continue;
    }

    node->set_tile_rank(rec.tile_rank);
    node->set_mute_mask(rec.mute_mask);
  }
}

// Build a sorted vector of current node metadata.
static void BuildCurrentNodeMeta(std::vector<NodeMetaRecord> *out) {
  out->clear();
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  out->reserve(ids.size());
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }
    NodeMetaRecord rec;
    rec.node_id = id;
    rec.tile_rank = node->tile_rank();
    rec.mute_mask = node->mute_mask();
    rec.reserved[0] = rec.reserved[1] = rec.reserved[2] = 0;
    out->push_back(rec);
  }
  std::sort(out->begin(), out->end(),
            [](const NodeMetaRecord &a, const NodeMetaRecord &b) {
              return a.node_id < b.node_id;
            });
}

static void SaveNodeMetaToNVSIfChanged() {

  if (g_use_dummy_data) {
    // Never persist dummy tiles.
    return;
  }

  std::vector<NodeMetaRecord> cur;
  BuildCurrentNodeMeta(&cur);

  // Load previous snapshot.
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const size_t nbytes = g_root_preferences.getBytesLength("node_meta");
  std::vector<NodeMetaRecord> prev;
  if (nbytes != 0 && (nbytes % sizeof(NodeMetaRecord)) == 0) {
    const size_t count = nbytes / sizeof(NodeMetaRecord);
    prev.resize(count);
    if (count > 0) {
      g_root_preferences.getBytes("node_meta", prev.data(),
                                  count * sizeof(NodeMetaRecord));
    }
  }
  g_root_preferences.end();

  bool same = (cur.size() == prev.size());
  if (same) {
    for (size_t i = 0; i < cur.size(); ++i) {
      if (cur[i].node_id != prev[i].node_id ||
          cur[i].tile_rank != prev[i].tile_rank ||
          cur[i].mute_mask != prev[i].mute_mask) {
        same = false;
        break;
      }
    }
  }

  if (same) {
    // Nothing changed → skip NVS write.
    return;
  }

  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  bool is_successful = true;
  if (!cur.empty()) {
    Serial.println(F("Saving node meta data..."));
    is_successful =
        NvsPutBytesVerified(g_root_preferences, "node_meta", cur.data(),
                            cur.size() * sizeof(NodeMetaRecord));
  } else {
    is_successful = NvsRemoveKeyVerified(g_root_preferences, "node_meta");
  }
  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveNodeMetaToNVSIfChanged failed"));
  }
}

static void SaveTopoPersistFlag() {
  Serial.println(F("Saving Topology Persist Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  const bool is_successful = NvsPutIntVerified(
      g_root_preferences, "topo_persist", g_topology_persist_enabled ? 1 : 0);
  g_root_preferences.end();
  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveTopoPersistFlag failed"));
  }
}

static void LoadTopoPersistFlag() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  g_topology_persist_enabled =
      (g_root_preferences.getInt("topo_persist", 1) != 0);
  g_root_preferences.end();
}

void LoadLabels() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const String json = g_root_preferences.getString("labels", "");
  g_root_preferences.end();

  if (json.isEmpty()) {
    return;
  }

#if defined(ARDUINOJSON_VERSION_MAJOR) && (ARDUINOJSON_VERSION_MAJOR >= 7)
  JsonDocument labels;
#else
  StaticJsonDocument<8192> labels;
#endif

  if (deserializeJson(labels, json) != DeserializationError::Ok) {
    Serial.println(F("LoadLabels: JSON parse error"));
    return;
  }

  // Node labels.
  JsonObject nodes_obj = labels["nodes"].as<JsonObject>();
  for (JsonPair p : nodes_obj) {
    const String node_key_hex = p.key().c_str();
    uint32_t id_u32 = 0;
    if (!ParseNodeIdToU32(node_key_hex, &id_u32)) {
      continue;
    }
    MeshNode *node = FindMeshNode(id_u32);
    if (node == nullptr) {
      continue;
    }

    String label;
    if (p.value().is<const char *>()) {
      label = p.value().as<const char *>();
    } else if (p.value().is<JsonObject>()) {
      label = p.value()["node"] | "";
    }
    if (label.length() > 0) {
      node->set_label(label);
    }
  }

  // Sensor global labels.
  JsonObject sensors_obj = labels["sensors"].as<JsonObject>();
  for (JsonPair p : sensors_obj) {
    const String addr16 = p.key().c_str();
    const char *label_c = p.value() | "";
    if (label_c == nullptr || label_c[0] == '\0') {
      continue;
    }
    const String label(label_c);

    // Apply to any existing sensor with this address.
    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }
      (void)node->SetSensorLabel(addr16, label);
    }
  }

  // Global sensor ordering.
  JsonObject order_obj = labels["order"].as<JsonObject>();
  for (JsonPair p : order_obj) {
    const String addr16 = p.key().c_str();
    const int32_t rank = p.value().as<int32_t>();

    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }
      (void)node->SetSensorGlobalRank(addr16, rank);
    }
  }

  // Per-node sensor ordering.
  JsonObject sorder_root = labels["sorder"].as<JsonObject>();
  for (JsonPair node_pair : sorder_root) {
    const String node_key_hex = node_pair.key().c_str();
    uint32_t id_u32 = 0;
    if (!ParseNodeIdToU32(node_key_hex, &id_u32)) {
      continue;
    }
    MeshNode *node = FindMeshNode(id_u32);
    if (node == nullptr) {
      continue;
    }
    JsonObject node_map = node_pair.value().as<JsonObject>();
    for (JsonPair p : node_map) {
      const String addr16 = p.key().c_str();
      const int32_t rank = p.value().as<int32_t>();
      (void)node->SetSensorNodeRank(addr16, rank);
    }
  }
}

void SaveLabels() {
  if (g_use_dummy_data) {
    Serial.println(F("Saving Labels skipped (dummy data mode)."));
    return;
  }

  // Build a compact JSON just for persistence; no global doc.
#if defined(ARDUINOJSON_VERSION_MAJOR) && (ARDUINOJSON_VERSION_MAJOR >= 7)
  JsonDocument labels;
#else
  StaticJsonDocument<8192> labels;
#endif

  JsonObject nodes_obj = labels["nodes"].to<JsonObject>();
  JsonObject sensors_obj = labels["sensors"].to<JsonObject>();
  JsonObject order_obj = labels["order"].to<JsonObject>();
  JsonObject sorder_root = labels["sorder"].to<JsonObject>();

  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    char node_key[9];
    FormatNodeKey(id, node_key, sizeof(node_key));
    const String node_key_hex(node_key);

    if (node->label().length() > 0) {
      nodes_obj[node_key_hex] = node->label();
    }

    for (const auto &sensor : node->sensors()) {
      const String &addr16 = sensor.address;

      if (sensor.label.length() > 0) {
        sensors_obj[addr16] = sensor.label;
      }

      if (sensor.global_rank != std::numeric_limits<int32_t>::max()) {
        order_obj[addr16] = sensor.global_rank;
      }

      if (sensor.node_rank != std::numeric_limits<int32_t>::max()) {
        JsonObject node_sorder = sorder_root[node_key_hex].to<JsonObject>();
        node_sorder[addr16] = sensor.node_rank;
      }
    }
  }

  String json;
  serializeJson(labels, json);

  Serial.println(F("Saving Labels..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  const bool is_successful =
      NvsPutStringVerified(g_root_preferences, "labels", json);
  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveLabels failed"));
  }
}

void EraseLabels() {
  // Clear runtime labels and ordering from all nodes.
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }
    node->set_label(String());
    std::vector<MeshNode::Sensor> &sensors = node->sensors();
    for (auto &sensor : sensors) {
      sensor.label.clear();
      sensor.global_rank = std::numeric_limits<int32_t>::max();
      sensor.node_rank = std::numeric_limits<int32_t>::max();
    }
  }

  // Wipe persisted labels.
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  const bool is_successful = NvsRemoveKeyVerified(g_root_preferences, "labels");
  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] EraseLabels failed"));
  }
}

void SaveLimits() {
  Serial.println(F("Saving Limit Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);

  bool is_successful = true;
  is_successful =
      NvsPutFloatVerified(g_root_preferences, "warn_low_c", g_warn_low_c) &&
      is_successful;
  is_successful =
      NvsPutFloatVerified(g_root_preferences, "warn_high_c", g_warn_high_c) &&
      is_successful;
  is_successful =
      NvsPutFloatVerified(g_root_preferences, "alert_low_c", g_alert_low_c) &&
      is_successful;
  is_successful =
      NvsPutFloatVerified(g_root_preferences, "alert_high_c", g_alert_high_c) &&
      is_successful;
  is_successful = NvsPutFloatVerified(g_root_preferences, "limit_hyst_c",
                                      g_limit_hysteresis_c) &&
                  is_successful;

  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveLimits failed"));
  }
}

void LoadLimits() {
  // Start from whatever defaults are in the globals; only override with valid
  // ranges.
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const float wl = g_root_preferences.getFloat("warn_low_c", g_warn_low_c);
  const float wh = g_root_preferences.getFloat("warn_high_c", g_warn_high_c);
  const float al = g_root_preferences.getFloat("alert_low_c", g_alert_low_c);
  const float ah = g_root_preferences.getFloat("alert_high_c", g_alert_high_c);
  const float hyst =
      g_root_preferences.getFloat("limit_hyst_c", g_limit_hysteresis_c);
  g_root_preferences.end();

  if (wh > wl) {
    g_warn_low_c = wl;
    g_warn_high_c = wh;
  }
  if (ah > al) {
    g_alert_low_c = al;
    g_alert_high_c = ah;
  }
  g_limit_hysteresis_c = (hyst >= 0.0f) ? hyst : kDefaultLimitHysteresisC;
}

void LoadDisplayUnits() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const int stored = g_root_preferences.getInt("units", 1); // default °F
  g_root_preferences.end();
  g_display_fahrenheit = (stored != 0);
}

void SaveDisplayUnits() {
  Serial.println(F("Saving Unit Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  const bool is_successful = NvsPutIntVerified(g_root_preferences, "units",
                                               g_display_fahrenheit ? 1 : 0);
  g_root_preferences.end();
  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveDisplayUnits failed"));
  }
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
  Serial.println(F("Saving Highlight Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);

  bool is_successful = true;
  is_successful = NvsPutIntVerified(g_root_preferences, "hl_missing",
                                    g_highlight_missing_nodes ? 1 : 0) &&
                  is_successful;
  is_successful =
      NvsPutIntVerified(g_root_preferences, "hl_stale_min",
                        static_cast<int32_t>(g_stale_minutes_threshold)) &&
      is_successful;

  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveHighlightSettings failed"));
  }
}

void LoadNtfySettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  g_ntfy_config.enabled = (g_root_preferences.getInt("ntfy_en", 1) != 0);
  g_ntfy_config.summary_enabled = (g_root_preferences.getInt("ntfy_sum_en", 1) != 0);
  g_ntfy_config.summary_period_min =
      static_cast<uint32_t>(g_root_preferences.getUInt("ntfy_sum_min",
                                                       g_ntfy_config.summary_period_min));
  g_root_preferences.end();
}

void SaveNtfySettings() {
  Serial.println(F("Saving ntfy Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);

  bool is_successful = true;
  is_successful =
      NvsPutIntVerified(g_root_preferences, "ntfy_en", g_ntfy_config.enabled ? 1 : 0) &&
      is_successful;
  is_successful = NvsPutIntVerified(g_root_preferences, "ntfy_sum_en",
                                    g_ntfy_config.summary_enabled ? 1 : 0) &&
                  is_successful;
  is_successful = NvsPutULongVerified(g_root_preferences, "ntfy_sum_min",
                                      g_ntfy_config.summary_period_min) &&
                  is_successful;

  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveNtfySettings failed"));
  }
}

void LoadTileDisplaySettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const int show_labels = g_root_preferences.getInt("show_labels", 1);
  const int show_age = g_root_preferences.getInt("show_age", 1);
  g_root_preferences.end();

  g_show_sensor_labels = (show_labels != 0);
  g_show_age_label = (show_age != 0);
}

void SaveTileDisplaySettings() {
  Serial.println(F("Saving Tile Display Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);

  bool is_successful = true;
  is_successful = NvsPutIntVerified(g_root_preferences, "show_labels",
                                    g_show_sensor_labels ? 1 : 0) &&
                  is_successful;
  is_successful = NvsPutIntVerified(g_root_preferences, "show_age",
                                    g_show_age_label ? 1 : 0) &&
                  is_successful;

  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveTileDisplaySettings failed"));
  }
}

void SaveBuzzerSettings() {
  Serial.println(F("Saving Buzzer Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);

  bool is_successful = true;
  is_successful = NvsPutULongVerified(g_root_preferences, kKeyBeepLen,
                                      g_beep_len_ms) &&
                  is_successful;
  is_successful = NvsPutULongVerified(g_root_preferences, kKeyBeepWarnGap,
                                      g_beep_gap_warn_ms) &&
                  is_successful;
  is_successful = NvsPutULongVerified(g_root_preferences, kKeyBeepAlertGap,
                                      g_beep_gap_alert_ms) &&
                  is_successful;
  // NEW: cooldown between alert episodes
  is_successful = NvsPutULongVerified(g_root_preferences, kKeyBeepCooldown,
                                      g_buzzer_cooldown_ms) &&
                  is_successful;

  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveBuzzerSettings failed"));
  }
}


void LoadBuzzerSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const uint32_t len =
      NvsGetULongWithFallback(g_root_preferences, kKeyBeepLen, nullptr,
                              kDefaultBeepLenMs);
  const uint32_t gw = NvsGetULongWithFallback(g_root_preferences,
                                              kKeyBeepWarnGap,
                                              kKeyBeepWarnLegacy,
                                              kDefaultGapWarnMs);
  const uint32_t ga = NvsGetULongWithFallback(g_root_preferences,
                                              kKeyBeepAlertGap,
                                              kKeyBeepAlertLegacy,
                                              kDefaultGapAlertMs);
  const uint32_t cd = NvsGetULongWithFallback(
      g_root_preferences, kKeyBeepCooldown, kKeyBeepCooldownLegacy,
      kDefaultBuzzerCooldownMs);
  g_root_preferences.end();

  g_beep_len_ms = (len > 0) ? len : kDefaultBeepLenMs;
  g_beep_gap_warn_ms = gw;
  g_beep_gap_alert_ms = ga;
  g_buzzer_cooldown_ms = cd;  // allow 0 to mean "no extra cooldown"
}


void SaveFlashSettings() {
  Serial.println(F("Saving Flash Interval Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  const bool is_successful = NvsPutULongVerified(
      g_root_preferences, kKeyFlashInterval, g_flash_interval_ms);
  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] SaveFlashSettings failed"));
  }
}

void LoadFlashSettings() {
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const uint32_t fi = NvsGetULongWithFallback(
      g_root_preferences, kKeyFlashInterval, kKeyFlashIntervalLegacy,
      kDefaultFlashIntervalMs);
  g_root_preferences.end();
  g_flash_interval_ms = fi;
}

// Default high rank means "unspecified / after ranked ones"
static int GetNodeRank(const String &node_id) {
  uint32_t id_u32 = 0;
  if (!ParseNodeIdToU32(node_id, &id_u32)) {
    return 1000000;
  }
  MeshNode *node = FindMeshNode(id_u32);
  if (node == nullptr) {
    return 1000000;
  }
  return static_cast<int>(node->tile_rank());
}

// --- Topology persistence: fixed-size binary array of node IDs -------------
constexpr size_t kMaxKnownNodes = 20;

// Read sorted known IDs from NVS ("known_bin") into a vector.
static bool ReadKnownFromNVS(std::vector<uint32_t> *out_ids) {
  out_ids->clear();
  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const size_t nbytes = g_root_preferences.getBytesLength("known_bin");
  if (nbytes == 0 || (nbytes % sizeof(uint32_t)) != 0) {
    g_root_preferences.end();
    return false;
  }
  const size_t count = std::min(nbytes / sizeof(uint32_t), kMaxKnownNodes);
  out_ids->resize(count);
  if (count > 0) {
    (void)g_root_preferences.getBytes("known_bin", out_ids->data(),
                                      count * sizeof(uint32_t));
  }
  g_root_preferences.end();
  if (!out_ids->empty()) {
    std::sort(out_ids->begin(), out_ids->end());
    out_ids->erase(std::unique(out_ids->begin(), out_ids->end()),
                   out_ids->end());
  }
  return !out_ids->empty();
}

// Write sorted IDs to NVS ("known_bin").
static void WriteKnownToNVS(const std::vector<uint32_t> &ids_sorted_unique) {
  const size_t count = std::min(ids_sorted_unique.size(), kMaxKnownNodes);

  Serial.println(F("[NVS] Saving Known IDs to meshroot/known_bin"));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);

  bool is_successful = true;

  if (count > 0) {
    is_successful =
        NvsPutBytesVerified(g_root_preferences, "known_bin",
                            ids_sorted_unique.data(), count * sizeof(uint32_t));
  } else {
    is_successful = NvsRemoveKeyVerified(g_root_preferences, "known_bin");
  }

  // Legacy key should be gone.
  is_successful =
      NvsRemoveKeyVerified(g_root_preferences, "known") && is_successful;

  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] WriteKnownToNVS failed"));
  }
}

// Add id to the persisted set (sorted/unique, clamped to kMaxKnownNodes).
// Returns true if NVS was actually updated (i.e., set changed).
static bool AddKnownAndPersist(uint32_t id) {
  std::vector<uint32_t> cur;
  (void)ReadKnownFromNVS(&cur); // ok if empty

  cur.push_back(id);
  std::sort(cur.begin(), cur.end());
  cur.erase(std::unique(cur.begin(), cur.end()), cur.end());
  if (cur.size() > kMaxKnownNodes) {
    cur.resize(kMaxKnownNodes);
  }

  std::vector<uint32_t> prev;
  (void)ReadKnownFromNVS(&prev); // ok if empty

  const bool same = (prev.size() == cur.size()) &&
                    std::equal(prev.begin(), prev.end(), cur.begin());
  if (same) {
    return false; // no NVS activity
  }

  Serial.printf("[NVS] known topology updated: %u node(s); latest=0x%08lX\n",
                static_cast<unsigned>(cur.size()),
                static_cast<unsigned long>(id));
  WriteKnownToNVS(cur);
  return true;
}

// Save known topology from MeshNode store, not from a JSON doc.
void SaveKnownTopology() {
  if (!g_topology_persist_enabled || g_use_dummy_data) {
    return;
  }

  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  if (ids.empty()) {
    return;
  }

  std::vector<uint32_t> cur = ids;
  std::sort(cur.begin(), cur.end());
  cur.erase(std::unique(cur.begin(), cur.end()), cur.end());
  if (cur.empty()) {
    return;
  }

  std::vector<uint32_t> prev;
  (void)ReadKnownFromNVS(&prev); // ok if empty

  const bool same = (prev.size() == cur.size()) &&
                    std::equal(prev.begin(), prev.end(), cur.begin());
  if (same) {
    return;
  }

  Serial.printf("Updating known topology: %u node(s)\n",
                static_cast<unsigned>(cur.size()));
  WriteKnownToNVS(cur);
}

// Load persisted known IDs and pre-seed MeshNode store so tiles can appear
// on boot without any live traffic yet.
void LoadKnownTopology() {
  if (!g_topology_persist_enabled) {
    return;
  }

  std::vector<uint32_t> ids;
  if (!ReadKnownFromNVS(&ids) || ids.empty()) {
    return;
  }

  const uint32_t now_ms = millis();
  for (uint32_t id : ids) {
    MeshNode *node_model = GetOrCreateMeshNode(id);
    if (node_model != nullptr && node_model->last_update_ms() == 0U) {
      node_model->SetBusGpioAndLastUpdate(-1, now_ms);
    }
  }
}

// Remove persisted topology and clear in-memory nodes/tiles.
void EraseKnownTopology() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  bool is_successful = true;
  is_successful =
      NvsRemoveKeyVerified(g_root_preferences, "known_bin") && is_successful;
  is_successful =
      NvsRemoveKeyVerified(g_root_preferences, "known") && is_successful;
  is_successful =
      NvsRemoveKeyVerified(g_root_preferences, "node_meta") && is_successful;
  g_root_preferences.end();

  if (!is_successful) {
    Serial.println(F("[NVS VERIFY] EraseKnownTopology failed"));
  }

  ClearAllMeshNodes();
  MeshTile::DestroyAll();
}

// Minimal root storage initialization:
//   - runtime settings (units, highlight, limits, buzzer, flash, tile
//   display)
//   - topology persist flag + known topology
//   - per-node metadata (rank, mute, node labels)
//   - sensor/node labels + ranks loaded directly into MeshNode instances
static void RootInitStorage() {
  // Runtime settings (each uses its own defaults if key missing).
  LoadDisplayUnits();
  LoadHighlightSettings();
  LoadLimits();
  LoadBuzzerSettings();
  LoadFlashSettings();
  LoadTileDisplaySettings();

  LoadHistoryConfigFromNvs();

  // Topology persistence.
  LoadTopoPersistFlag();
  LoadKnownTopology(); // seeds MeshNode objects for persisted IDs

  // Per-node metadata (rank, mute, etc.).
  LoadNodeMetaFromNVS();

  // Labels + per-sensor ordering from NVS -> MeshNode model.
  LoadLabels();
}

// Node ID canonicalization: accept only hex, with optional "0x"/"0X" prefix.
// Canonical form for JSON keys: 8-hex, uppercase, no "0x".
static bool ParseNodeIdToU32(const String &s, uint32_t *out) {
  if (out == nullptr) {
    return false;
  }

  const char *c = s.c_str();

  // Skip optional 0x/0X prefix.
  if (s.length() >= 2 && c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) {
    c += 2;
  }

  char *end = nullptr;
  unsigned long value = strtoul(c, &end, 16);
  if (end == c) {
    // No hex digits consumed.
    return false;
  }

  *out = static_cast<uint32_t>(value);
  return true;
}

// Make a stable JSON key for a node id (8-hex, uppercase, no "0x").
static String CanonNodeHex8(const String &in) {
  uint32_t id = 0;
  if (!ParseNodeIdToU32(in, &id)) {
    return in; // should not happen for internal keys
  }
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(id));
  return String(buf);
}

// Make a stable JSON key for a node id (8-hex, uppercase, no "0x").
static void FormatNodeKey(uint32_t node_id, char *out, size_t out_len) {
  snprintf(out, out_len, "%08lX", static_cast<unsigned long>(node_id));
}

// Helper: try reading a value by hex key, then legacy decimal key.
// NOTE: avoid templates here; Arduino's auto-prototype pass doesn't handle
// them well.
static JsonVariant GetByNodeKeyWithLegacyFallback(JsonObject obj,
                                                  const String &node_key_hex8) {
  // New behaviour: hex only, no decimal fallbacks.
  return obj[node_key_hex8];
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

static float ToDisplayUnits(float temp_c) {
  if (isnan(temp_c)) return temp_c;
  return g_display_fahrenheit ? (temp_c * 9.0f / 5.0f + 32.0f) : temp_c;
}

static const char *DisplayUnitsLabel() { return g_display_fahrenheit ? "°F" : "°C"; }

static String FormatTempForMessage(float temp_c) {
  if (isnan(temp_c)) {
    return String("(no value)");
  }
  char buf[24];
  const float t = ToDisplayUnits(temp_c);
  snprintf(buf, sizeof(buf), "%.1f%s", static_cast<double>(t), DisplayUnitsLabel());
  return String(buf);
}

static String FormatLimitRange(float low_c, float high_c) {
  const bool has_low = !isnan(low_c);
  const bool has_high = !isnan(high_c);
  if (!has_low && !has_high) {
    return String();
  }

  char buf[48];
  if (has_low && has_high) {
    snprintf(buf, sizeof(buf), "%.1f to %.1f%s", static_cast<double>(ToDisplayUnits(low_c)),
             static_cast<double>(ToDisplayUnits(high_c)), DisplayUnitsLabel());
  } else if (has_low) {
    snprintf(buf, sizeof(buf), "below %.1f%s", static_cast<double>(ToDisplayUnits(low_c)),
             DisplayUnitsLabel());
  } else {
    snprintf(buf, sizeof(buf), "above %.1f%s", static_cast<double>(ToDisplayUnits(high_c)),
             DisplayUnitsLabel());
  }
  return String(buf);
}

static String FormatLimitSummary() {
  String parts;
  String warn = FormatLimitRange(g_warn_low_c, g_warn_high_c);
  String alert = FormatLimitRange(g_alert_low_c, g_alert_high_c);
  if (g_limit_hysteresis_c > 0.0f) {
    const float hyst_disp =
        g_display_fahrenheit ? (g_limit_hysteresis_c * 1.8f) : g_limit_hysteresis_c;
    char buf[32];
    snprintf(buf, sizeof(buf), "hyst=%.2f%s", static_cast<double>(hyst_disp),
             DisplayUnitsLabel());
    parts += buf;
  }
  if (warn.length() > 0) {
    if (parts.length() > 0) parts += "; ";
    parts += "warn: ";
    parts += warn;
  }
  if (alert.length() > 0) {
    if (parts.length() > 0) parts += "; ";
    parts += "alert: ";
    parts += alert;
  }
  return parts;
}

static const char *AlertStateName(AlertState state) {
  switch (state) {
  case AlertState::kWarning:
    return "warning";
  case AlertState::kAlert:
    return "alert";
  case AlertState::kMissing:
    return "missing";
  case AlertState::kNormal:
  default:
    return "normal";
  }
}

static String NodeDisplayName(const MeshNode *node) {
  if (node == nullptr) return String("(unknown node)");
  if (node->label().length() > 0) {
    return node->label();
  }
  char buf[11];
  snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(node->node_id()));
  return String(buf);
}

static String SensorDisplayName(const MeshNode::Sensor &sensor) {
  if (sensor.label.length() > 0) {
    return sensor.label;
  }
  return CanonAddr16(sensor.address);
}

// Build one tile's Content from MeshNode state + thresholds.
// If a node exists in MeshNode store, we render it; otherwise we skip.
static bool BuildTileContentForNode(const String &node_key_hex, uint32_t now_ms,
                                    bool node_connected, void *out_ptr) {
  if (out_ptr == nullptr) {
    return false;
  }
  MeshTile::Content *out = static_cast<MeshTile::Content *>(out_ptr);

  if (out == nullptr) {
    return false;
  }

  const String node_hex = CanonNodeHex8(node_key_hex);
  uint32_t node_id_u32 = 0;
  if (!ParseNodeIdToU32(node_hex, &node_id_u32)) {
    return false;
  }

  const MeshNode *node_model = FindMeshNode(node_id_u32);
  if (node_model == nullptr) {
    // No model – nothing to show.
    return false;
  }

  MeshTile::Content c;
  c.display_fahrenheit = g_display_fahrenheit;
  c.show_sensor_labels = g_show_sensor_labels;
  c.show_age = g_show_age_label;

  // Title: node label if present, otherwise formatted node id.
  if (node_model->label().length() > 0) {
    c.title = node_model->label();
  } else {
    char hex_id[11]; // "0x" + 8-hex + NUL
    snprintf(hex_id, sizeof(hex_id), "0x%08lX",
             static_cast<unsigned long>(node_id_u32));
    c.title = hex_id;
  }

  // Age and stale/missing flags from MeshNode timestamps only.
  const uint32_t age_min = node_model->ComputeAgeMinutes(now_ms);
  c.age_minutes = age_min;

  c.is_missing = g_highlight_missing_nodes && !node_connected &&
                 (g_stale_minutes_threshold > 0U) &&
                 (age_min >= g_stale_minutes_threshold);
  c.is_stale = node_connected && (g_stale_minutes_threshold > 0U) &&
               (age_min >= g_stale_minutes_threshold);

  // Sequence health.
  bool sequence_stuck = node_model->SequenceStuck(now_ms, kSeqStuckMsThreshold);
  c.seq_stuck = sequence_stuck;

  const uint8_t node_mute = node_model->mute_mask() & 0x3u;

  bool node_has_alert = false;
  bool node_has_warning = false;
  int sv_count = 0;

  const auto &sensors = node_model->sensors();
  if (!sensors.empty()) {
    // Sort sensor indices by effective rank, then address.
    std::vector<size_t> indices;
    indices.reserve(sensors.size());
    for (size_t i = 0; i < sensors.size(); ++i) {
      indices.push_back(i);
    }

    auto effective_rank = [](const MeshNode::Sensor &s) -> int32_t {
      if (s.node_rank != std::numeric_limits<int32_t>::max()) {
        return s.node_rank;
      }
      return s.global_rank;
    };

    std::sort(indices.begin(), indices.end(), [&](size_t ia, size_t ib) {
      const MeshNode::Sensor &sa = sensors[ia];
      const MeshNode::Sensor &sb = sensors[ib];

      const int32_t ra = effective_rank(sa);
      const int32_t rb = effective_rank(sb);
      if (ra != rb) {
        return ra < rb;
      }
      return sa.address < sb.address;
    });

    for (size_t idx : indices) {
      if (sv_count >= 2) {
        break; // only two sensor rows per tile
      }

      const MeshNode::Sensor &sensor = sensors[idx];
      MeshTile::SensorView &sv = c.sensors[sv_count++];

      // Label: use sensor.label if set, otherwise the canonical address.
      if (sensor.label.length() > 0) {
        sv.label = sensor.label;
      } else {
        sv.label = CanonAddr16(sensor.address);
      }

      if (sensor.has_value && !isnan(sensor.temp_c)) {
        sv.temp_c = sensor.temp_c;
        sv.has_value = true;

        const float t = sv.temp_c;

        // Alerts (respect node mute).
        const float alert_low_thresh =
            isnan(g_alert_low_c) ? NAN : (g_alert_low_c - g_limit_hysteresis_c);
        const float alert_high_thresh =
            isnan(g_alert_high_c) ? NAN : (g_alert_high_c + g_limit_hysteresis_c);
        bool low_alert = (!isnan(alert_low_thresh) && t < alert_low_thresh);
        bool high_alert = (!isnan(alert_high_thresh) && t > alert_high_thresh);
        if (low_alert && (node_mute & kMuteLow)) {
          low_alert = false;
        }
        if (high_alert && (node_mute & kMuteHigh)) {
          high_alert = false;
        }
        sv.is_alert = (low_alert || high_alert);

        // Warnings only if not already alert (respect mute).
        if (!sv.is_alert) {
          const float warn_low_thresh =
              isnan(g_warn_low_c) ? NAN : (g_warn_low_c - g_limit_hysteresis_c);
          const float warn_high_thresh =
              isnan(g_warn_high_c) ? NAN : (g_warn_high_c + g_limit_hysteresis_c);
          bool low_warn = (!isnan(warn_low_thresh) && t < warn_low_thresh);
          bool high_warn = (!isnan(warn_high_thresh) && t > warn_high_thresh);
          if (low_warn && (node_mute & kMuteLow)) {
            low_warn = false;
          }
          if (high_warn && (node_mute & kMuteHigh)) {
            high_warn = false;
          }
          sv.is_warning = (low_warn || high_warn);
        } else {
          sv.is_warning = false;
        }
      } else {
        sv.temp_c = NAN;
        sv.has_value = false;
        sv.is_alert = false;
        sv.is_warning = false;
      }

      if (sv.is_alert) {
        node_has_alert = true;
      } else if (sv.is_warning) {
        node_has_warning = true;
      }
    }
  }

  // Treat a sequence-stuck node as warning-level unless it already has
  // a temperature alert.
  if (sequence_stuck && !node_has_alert) {
    node_has_warning = true;
  }

  c.sensor_count = sv_count;
  c.node_has_alert = node_has_alert;
  c.node_has_warning = node_has_warning;

  *out = c;
  return true;
}

static void EmitStateNotification(const String &key, AlertState new_state,
                                  const String &subject, const String &detail,
                                  const String &normal_detail, bool cache,
                                  bool include_normal_detail_on_resolve = true) {
  AlertState prev = AlertState::kNormal;
  auto it = g_ntfy_states.find(key);
  if (it != g_ntfy_states.end()) {
    prev = it->second;
  }

  if (new_state == prev) {
    return;
  }

  if (new_state == AlertState::kNormal) {
    if (prev != AlertState::kNormal) {
      String msg = "Resolved (";
      msg += AlertStateName(prev);
      msg += "): ";
      msg += subject;
      const bool append_normal_detail =
          include_normal_detail_on_resolve || (prev == AlertState::kMissing);
      if (append_normal_detail && normal_detail.length() > 0) {
        msg += " - ";
        msg += normal_detail;
      }
      if (g_debug_enabled) {
        Serial.printf("[NTFY] %s -> normal (%s)\n", key.c_str(),
                      normal_detail.c_str());
      }
      SendNtfyRequestToBridge(msg, cache, false, "MeshTemps");
    }
    g_ntfy_states.erase(key);
    return;
  }

  String msg = "[";
  msg += AlertStateName(new_state);
  msg += "] ";
  msg += subject;
  if (detail.length() > 0) {
    msg += " - ";
    msg += detail;
  }

  if (g_debug_enabled) {
    Serial.printf("[NTFY] %s: %s -> %s\n", key.c_str(),
                  AlertStateName(prev), AlertStateName(new_state));
  }
  SendNtfyRequestToBridge(msg, cache, false, "MeshTemps");
  g_ntfy_states[key] = new_state;
}

static AlertState EvaluateSensorState(const MeshNode::Sensor &sensor,
                                      uint8_t node_mute, bool *low_alert,
                                      bool *high_alert, bool *low_warn,
                                      bool *high_warn) {
  if (low_alert) *low_alert = false;
  if (high_alert) *high_alert = false;
  if (low_warn) *low_warn = false;
  if (high_warn) *high_warn = false;

  if (!sensor.has_value || isnan(sensor.temp_c)) {
    return AlertState::kNormal;
  }

  const float alert_low_thresh =
      isnan(g_alert_low_c) ? NAN : (g_alert_low_c - g_limit_hysteresis_c);
  const float alert_high_thresh =
      isnan(g_alert_high_c) ? NAN : (g_alert_high_c + g_limit_hysteresis_c);
  bool la = (!isnan(alert_low_thresh) && sensor.temp_c < alert_low_thresh);
  bool ha = (!isnan(alert_high_thresh) && sensor.temp_c > alert_high_thresh);
  if (la && (node_mute & kMuteLow)) la = false;
  if (ha && (node_mute & kMuteHigh)) ha = false;

  if (low_alert) *low_alert = la;
  if (high_alert) *high_alert = ha;

  if (la || ha) {
    return AlertState::kAlert;
  }

  const float warn_low_thresh =
      isnan(g_warn_low_c) ? NAN : (g_warn_low_c - g_limit_hysteresis_c);
  const float warn_high_thresh =
      isnan(g_warn_high_c) ? NAN : (g_warn_high_c + g_limit_hysteresis_c);
  bool lw = (!isnan(warn_low_thresh) && sensor.temp_c < warn_low_thresh);
  bool hw = (!isnan(warn_high_thresh) && sensor.temp_c > warn_high_thresh);
  if (lw && (node_mute & kMuteLow)) lw = false;
  if (hw && (node_mute & kMuteHigh)) hw = false;

  if (low_warn) *low_warn = lw;
  if (high_warn) *high_warn = hw;

  if (lw || hw) {
    return AlertState::kWarning;
  }
  return AlertState::kNormal;
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

// Extract label text after the 2nd token (command + id/addr), handling
// optional quotes. Examples accepted:
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

// Extract everything after the 2nd token (command + subcommand) as-is,
// only stripping trailing CR/LF. This is used for WiFi SSID/password so
// all valid characters (spaces, quotes, etc.) are preserved.
static bool ExtractRawTailAfterSecondToken(const String &line,
                                           String *out_value) {
  if (out_value == nullptr) {
    return false;
  }

  int s0 = 0;
  int e0 = 0;
  int s1 = 0;
  int e1 = 0;
  if (!FindTokenSpan(line, 0, &s0, &e0)) {
    return false;
  }
  if (!FindTokenSpan(line, 1, &s1, &e1)) {
    return false;
  }

  const int n = static_cast<int>(line.length());
  int pos = e1;
  while (pos < n &&
         isspace(static_cast<unsigned char>(line[pos]))) { // skip delimiter
    ++pos;
  }
  if (pos >= n) {
    return false; // nothing after subcommand
  }

  String raw = line.substring(pos);

  // Strip trailing CR/LF only; keep all other characters, including spaces.
  while (raw.length() > 0) {
    char c = raw[raw.length() - 1];
    if (c == '\r' || c == '\n') {
      raw.remove(raw.length() - 1);
    } else {
      break;
    }
  }

  *out_value = raw;
  return (out_value->length() > 0);
}

static const char *WifiAuthModeToString(wifi_auth_mode_t auth_mode) {
  switch (auth_mode) {
  case WIFI_AUTH_OPEN:
    return "OPEN";
  case WIFI_AUTH_WEP:
    return "WEP";
  case WIFI_AUTH_WPA_PSK:
    return "WPA-PSK";
  case WIFI_AUTH_WPA2_PSK:
    return "WPA2-PSK";
  case WIFI_AUTH_WPA_WPA2_PSK:
    return "WPA/WPA2-PSK";
#ifdef WIFI_AUTH_WPA3_PSK
  case WIFI_AUTH_WPA3_PSK:
    return "WPA3-PSK";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return "WPA2/WPA3-PSK";
#endif
  case WIFI_AUTH_WAPI_PSK:
    return "WAPI-PSK";
  default:
    return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
// Display + GUI setup
// ---------------------------------------------------------------------------

// See if a double buffer and no bounce fix pixel ghosting
// void DisplayInit() {
//   g_board = new Board();
//   g_board->init();

//   LCD* lcd = g_board->getLCD();
//   if (lcd != nullptr) {
//     // Use 2 full frame buffers in PSRAM
//     lcd->configFrameBufferNumber(2);

// #if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
//     auto* bus = lcd->getBus();
//     if (bus != nullptr &&
//         bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
//       // Disable bounce buffer for now
//       // static_cast<BusRGB*>(bus)->configRGB_BounceBufferSize(0);
//     }
// #endif
//   }

//   assert(g_board->begin());
//   lvgl_port_init(g_board->getLCD(), g_board->getTouch());
// }

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

static void GuiUpdateNtpStatusIcon() {
  if (g_ui_ntp_icon == nullptr || g_ui_ntp_popup == nullptr) {
    return;
  }

  const uint32_t now_ms = millis();
  const bool recent_ok = g_ntp_time_valid && (now_ms - g_last_ntp_ok_ms <=
                                              24UL * 60UL * 60UL * 1000UL);

  const char *icon = nullptr;
  if (g_ntp_sync_in_progress) {
    icon = LV_SYMBOL_REFRESH; // spinning arrows
  } else if (recent_ok) {
    icon = LV_SYMBOL_WIFI; // checkmark
  } else {
    icon = LV_SYMBOL_CLOSE; // X
  }
  lv_label_set_text(g_ui_ntp_icon, icon);

  if (g_ntp_sync_in_progress) {
    lv_obj_clear_flag(g_ui_ntp_popup, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_ui_ntp_popup, LV_OBJ_FLAG_HIDDEN);
  }
}

static void UpdateSilenceButtonAppearance() {
  if (g_ui_button_silence == nullptr) {
    return;
  }

  const bool silenced =
      (g_buzzer_silence_state == kBuzzerSilenceActive ||
       g_buzzer_silence_state == kBuzzerSilenceCooldown);

  // Neutral dark when idle, red when silenced/cooldown.
  lv_color_t bg_color =
      silenced ? lv_color_make(0xC0, 0x20, 0x20)   // red
               : lv_color_make(0x40, 0x40, 0x40);  // dark gray

  lv_obj_set_style_bg_color(g_ui_button_silence, bg_color, 0);
  lv_obj_set_style_bg_opa(g_ui_button_silence, LV_OPA_COVER, 0);

  if (g_ui_button_silence_label != nullptr) {
    lv_obj_set_style_text_color(g_ui_button_silence_label,
                                lv_color_white(), 0);
  }
}

void GuiInit() {
  if (!lvgl_port_lock(-1)) {
    return;
  }

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_make(0x25, 0x25, 0x25), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);

  // Disable all scrolling on the root screen
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  // Top bar.
  lv_obj_t *bar = lv_obj_create(screen);
  lv_obj_set_size(bar, lv_pct(100), kTopBarHeightPx);

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

  // Peers label
  g_ui_label_peers = lv_label_create(bar);
  lv_label_set_text(g_ui_label_peers, "Peers: 0");
  lv_obj_align(g_ui_label_peers, LV_ALIGN_LEFT_MID, 4, 0);

  // Room Temps label
  g_ui_label_title = lv_label_create(bar);
  lv_label_set_text(g_ui_label_title, "Room Temps");
  lv_obj_align(g_ui_label_title, LV_ALIGN_CENTER, 0, 0);

  // Clock
  g_ui_label_clock = lv_label_create(bar);
  lv_label_set_text(g_ui_label_clock, "--:--"); // was "0:00"
  lv_obj_align(g_ui_label_clock, LV_ALIGN_RIGHT_MID, -4, 0);

  // NTP status icon near the clock.
  g_ui_ntp_icon = lv_label_create(bar);
  lv_label_set_text(g_ui_ntp_icon, LV_SYMBOL_CLOSE); // "no valid sync yet"
  lv_obj_align(g_ui_ntp_icon, LV_ALIGN_LEFT_MID, 80, 0);

  // Small button to silence alarms
  g_ui_button_silence = lv_btn_create(bar);
  lv_obj_set_size(g_ui_button_silence, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(g_ui_button_silence, LV_ALIGN_LEFT_MID, 110, 0);
  lv_obj_add_event_cb(g_ui_button_silence, SilenceButtonEvent, LV_EVENT_CLICKED,
                      nullptr);

  g_ui_button_silence_label = lv_label_create(g_ui_button_silence);
  lv_label_set_text(g_ui_button_silence_label, "Silence");
  lv_obj_center(g_ui_button_silence_label);

    // Ensure initial visual state matches "not silenced".
  UpdateSilenceButtonAppearance();

  // Small button to toggle Room / Underbelly view for the map.
  g_ui_button_belly = lv_btn_create(bar);
  lv_obj_set_size(g_ui_button_belly, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(g_ui_button_belly, LV_ALIGN_RIGHT_MID, -150, 0);
  lv_obj_add_event_cb(g_ui_button_belly, BellyButtonEvent, LV_EVENT_CLICKED,
                      nullptr);

  g_ui_button_belly_label = lv_label_create(g_ui_button_belly);
  lv_label_set_text(g_ui_button_belly_label, "Room");
  lv_obj_center(g_ui_button_belly_label);

  // Small button to toggle Tile / map view.
  g_ui_button_map = lv_btn_create(bar);
  lv_obj_set_size(g_ui_button_map, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(g_ui_button_map, LV_ALIGN_RIGHT_MID, -60, 0);
  lv_obj_add_event_cb(g_ui_button_map, ViewButtonEvent, LV_EVENT_CLICKED,
                      nullptr);

  g_ui_button_map_label = lv_label_create(g_ui_button_map);
  lv_label_set_text(g_ui_button_map_label, "Tiles");
  lv_obj_center(g_ui_button_map_label);

  // Prominent popup "card" for "syncing" state (hidden by default).
  g_ui_ntp_popup = lv_obj_create(screen);

  // Size: wide box across most of the screen width, auto height.
  lv_obj_set_width(g_ui_ntp_popup, lv_pct(80));
  lv_obj_set_height(g_ui_ntp_popup, lv_pct(50));

  // Subtle light blue background.
  lv_obj_set_style_bg_color(g_ui_ntp_popup,
                            lv_color_make(0x60, 0x90, 0xC0), 0);  // light, not bright
  lv_obj_set_style_bg_opa(g_ui_ntp_popup, LV_OPA_80, 0);

  // Rounded corners, border, and padding.
  lv_obj_set_style_radius(g_ui_ntp_popup, 8, 0);
  lv_obj_set_style_border_width(g_ui_ntp_popup, 2, 0);
  lv_obj_set_style_border_color(g_ui_ntp_popup,
                                lv_color_make(0x30, 0x50, 0x80), 0);
  lv_obj_set_style_pad_all(g_ui_ntp_popup, 12, 0);
  lv_obj_set_style_shadow_width(g_ui_ntp_popup, 8, 0);
  lv_obj_set_style_shadow_opa(g_ui_ntp_popup, LV_OPA_50, 0);

  // Optional: make sure it does not scroll.
  lv_obj_clear_flag(g_ui_ntp_popup, LV_OBJ_FLAG_SCROLLABLE);

  // Position: centered near the top, just below the top bar.
  lv_obj_align(g_ui_ntp_popup, LV_ALIGN_TOP_MID, 0, kTopBarHeightPx + 12);

  // Create the label inside the popup.
  lv_obj_t *ntp_label = lv_label_create(g_ui_ntp_popup);
  lv_label_set_text(ntp_label, "Syncing time...");
  lv_obj_set_style_text_color(ntp_label, lv_color_black(), 0);
  lv_obj_center(ntp_label);

  // Start hidden; shown/hidden by GuiUpdateNtpStatusIcon().
  lv_obj_add_flag(g_ui_ntp_popup, LV_OBJ_FLAG_HIDDEN);

  // Tile container below the bar.
  g_ui_tile_container = lv_obj_create(screen);

  // Use the display's physical vertical resolution plus the known bar height.
  // This avoids relying on lv_obj_get_height(bar) before layout.
  lv_disp_t *disp = lv_disp_get_default();
  lv_coord_t scr_h =
      disp ? lv_disp_get_ver_res(disp) : lv_obj_get_height(screen);
  if (scr_h <= 0) {
    scr_h = 480; // safe fallback; adjust if your panel is not 480px tall
  }

  lv_coord_t cont_h =
      (scr_h > kTopBarHeightPx) ? (scr_h - kTopBarHeightPx) : scr_h;

  // Serial.printf("GuiInit: scr_h=%d, cont_h=%d (bar=%d)\n", (int)scr_h,
  //               (int)cont_h, (int)kTopBarHeightPx);

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

  // Map container: same geometry, manual layout, hidden by default.
  g_ui_map_container = lv_obj_create(screen);
  lv_obj_set_size(g_ui_map_container, lv_pct(100), cont_h);
  lv_obj_align_to(g_ui_map_container, bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

  lv_obj_set_style_pad_all(g_ui_map_container, 6, 0);
  lv_obj_set_style_bg_opa(g_ui_map_container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_ui_map_container, 0, 0);
  lv_obj_clear_flag(g_ui_map_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(g_ui_map_container, LV_SCROLLBAR_MODE_OFF);
  g_ui_map_house = nullptr; // created lazily
  lv_obj_add_flag(g_ui_map_container, LV_OBJ_FLAG_HIDDEN);

  // Ensure NTP popup is drawn above tiles/map, but below the top bar.
  if (g_ui_ntp_popup != nullptr) {
    lv_obj_move_foreground(g_ui_ntp_popup);
  }

  // Now ensure the bar is the topmost element.
  lv_obj_move_foreground(bar);

  g_layout_dirty = true; // first time needs a full build

  lvgl_port_unlock();
}

// Called when node metadata changes.
void GuiUpdateNodeSummary(const char *, int, uint32_t) {
  g_values_dirty = true; // no layout
}

// Called when sensor data changes.
void GuiUpdateSensorRow(const char *, const char *, float, const char *,
                        uint32_t) {
  g_values_dirty = true; // no layout
}

void UpdateUptimeLabel() {
  if (g_ui_label_clock == nullptr) {
    return;
  }

  // Read current time in a non-blocking way.
  time_t now = time(nullptr);
  if (now == static_cast<time_t>(-1)) {
    // Time not initialised yet: only show "--:--" once, then keep whatever is
    // on screen to avoid flicker.
    static bool clock_initialised = false;
    if (!clock_initialised) {
      lv_label_set_text(g_ui_label_clock, "--:--");
      clock_initialised = true;
    }
    return;
  }

  struct tm local_time;
  if (localtime_r(&now, &local_time) == nullptr) {
    // On any error, keep the previous display; do not blank.
    return;
  }

  // Only update the label when the minute changes.
  static int last_hour = -1;
  static int last_minute = -1;

  if (local_time.tm_hour == last_hour &&
      local_time.tm_min == last_minute) {
    // No visible change required.
    return;
  }

  last_hour = local_time.tm_hour;
  last_minute = local_time.tm_min;

  char buffer[16];
  if (strftime(buffer, sizeof(buffer), "%H:%M", &local_time) == 0) {
    // Formatting failed; keep previous text.
    return;
  }

  lv_label_set_text(g_ui_label_clock, buffer);
}


// Dummy data: one node per RoomDef with "Room" and "Underbelly" sensors.
// This version deliberately creates at least one instance of each "issue":
//  - Normal in-range
//  - High warning
//  - High alert
//  - Low warning
//  - Low alert
//  - Missing sensor data
//  - Stale sensor
//  - "Missing" node (old last_update_ms)
static void BuildDummyData() {
  ClearAllMeshNodes();
  MeshTile::DestroyAll();

  const uint32_t now_ms = millis();

  // Snapshot current limits so dummy behavior tracks whatever the user
  // configured via "limits" commands.
  const float warn_low = g_warn_low_c;
  const float warn_high = g_warn_high_c;
  const float alert_low = g_alert_low_c;
  const float alert_high = g_alert_high_c;

  auto NormalTemp = [&]() -> float {
    if (!isnan(warn_low) && !isnan(warn_high) && warn_high > warn_low) {
      return (warn_low + warn_high) * 0.5f;
    }
    return 21.0f; // fallback
  };

  auto WarnHighTemp = [&]() -> float {
    if (!isnan(warn_high) && !isnan(alert_high) && alert_high > warn_high) {
      return (warn_high + alert_high) * 0.5f;
    }
    if (!isnan(warn_high)) {
      return warn_high + 1.0f;
    }
    return NormalTemp() + 5.0f;
  };

  auto AlertHighTemp = [&]() -> float {
    if (!isnan(alert_high)) {
      return alert_high + 1.0f;
    }
    return WarnHighTemp() + 3.0f;
  };

  auto WarnLowTemp = [&]() -> float {
    if (!isnan(warn_low) && !isnan(alert_low) && warn_low > alert_low) {
      return (warn_low + alert_low) * 0.5f;
    }
    if (!isnan(warn_low)) {
      return warn_low - 0.5f;
    }
    return NormalTemp() - 5.0f;
  };

  auto AlertLowTemp = [&]() -> float {
    if (!isnan(alert_low)) {
      return alert_low - 1.0f;
    }
    return WarnLowTemp() - 3.0f;
  };

  const float t_normal = NormalTemp();
  const float t_warn_hi = WarnHighTemp();
  const float t_alert_hi = AlertHighTemp();
  const float t_warn_lo = WarnLowTemp();
  const float t_alert_lo = AlertLowTemp();

  // Underbelly baseline: generally cooler than room.
  const float belly_normal = t_normal - 10.0f;

  // "Stale" age: just beyond the configured stale threshold.
  const uint32_t stale_minutes =
      (g_stale_minutes_threshold > 0U) ? (g_stale_minutes_threshold + 1U) : 6U;
  const uint32_t stale_age_ms = stale_minutes * 60000UL;

  unsigned long addr_counter = 1; // simple unique address generator

  auto AddSensor = [&](MeshNode *node, const char *label, bool has_value,
                       float temp_c, bool make_stale) {
    if (node == nullptr) {
      return;
    }
    char addr[17];
    snprintf(addr, sizeof(addr), "%016lu",
             static_cast<unsigned long>(addr_counter++));
    MeshNode::Sensor *sensor = node->GetOrCreateSensor(String(addr));
    if (sensor == nullptr) {
      return;
    }

    sensor->label = String(label);
    sensor->corrected = false;

    if (has_value) {
      sensor->temp_c = temp_c;
      sensor->has_value = true;
      sensor->last_ms = make_stale ? (now_ms - stale_age_ms) : now_ms;
    } else {
      sensor->temp_c = NAN;
      sensor->has_value = false;
      sensor->last_ms = 0U;
    }
  };

  for (size_t i = 0; i < kRoomCount; ++i) {
    const RoomDef &def = kRoomDefs[i];

    // Stable but obviously dummy node IDs.
    const uint32_t node_id = 0xAA000000u + static_cast<uint32_t>(i + 1u);
    MeshNode *node_model = GetOrCreateMeshNode(node_id);
    if (node_model == nullptr) {
      continue;
    }

    // Node label must match RoomDef::id via CanonicalRoomId(),
    // so we use display_name here ("Front Room", "Kitchen", "Outside", etc.).
    node_model->set_label(String(def.display_name));
    node_model->set_mute_mask(0);
    node_model->set_tile_rank(std::numeric_limits<int32_t>::max());

    // Default: node is "fresh". Specific scenarios can override this.
    uint32_t node_update_ms = now_ms;

    // Scenario assignment by RoomDef::id:
    if (strcmp(def.id, "frontroom") == 0) {
      // Completely normal.
      AddSensor(node_model, "Room", true, t_normal, false);
      AddSensor(node_model, "Underbelly", true, belly_normal, false);

    } else if (strcmp(def.id, "livingroom") == 0) {
      // High warning (above warn_high, below alert_high) on Room.
      AddSensor(node_model, "Room", true, t_warn_hi, false);
      AddSensor(node_model, "Underbelly", true, belly_normal, false);

    } else if (strcmp(def.id, "masterbedroom") == 0) {
      // High alert on Room.
      AddSensor(node_model, "Room", true, t_alert_hi, false);
      AddSensor(node_model, "Underbelly", true, belly_normal, false);

    } else if (strcmp(def.id, "masterbath") == 0) {
      // Low warning on Room, stale Underbelly sensor.
      AddSensor(node_model, "Room", true, t_warn_lo, false);
      AddSensor(node_model, "Underbelly", true, belly_normal, true);

    } else if (strcmp(def.id, "bath") == 0) {
      // Low alert on Room.
      AddSensor(node_model, "Room", true, t_alert_lo, false);
      AddSensor(node_model, "Underbelly", true, belly_normal, false);

    } else if (strcmp(def.id, "corner") == 0) {
      // Missing data: sensor exists but has no value.
      AddSensor(node_model, "Room", false, NAN, false);
      AddSensor(node_model, "Underbelly", true, belly_normal, false);

    } else if (strcmp(def.id, "kidsroom") == 0) {
      // Node that looks "missing": last_update_ms is far in the past.
      node_update_ms = now_ms - stale_age_ms;
      AddSensor(node_model, "Room", true, t_normal, true);
      AddSensor(node_model, "Underbelly", true, belly_normal, true);

    } else if (strcmp(def.id, "nathans") == 0) {
      // Underbelly high alert, Room normal.
      AddSensor(node_model, "Room", true, t_normal, false);
      AddSensor(node_model, "Underbelly", true, t_alert_hi, false);

    } else if (strcmp(def.id, "kitchen") == 0) {
      // Underbelly low warning, Room normal.
      AddSensor(node_model, "Room", true, t_normal, false);
      AddSensor(node_model, "Underbelly", true, t_warn_lo, false);

    } else if (strcmp(def.id, "outside") == 0) {
      // Outside: cooler but normal Room, missing Underbelly data.
      AddSensor(node_model, "Room", true, t_normal - 5.0f, false);
      AddSensor(node_model, "Underbelly", false, NAN, false);

    } else {
      // Fallback: treat as normal.
      AddSensor(node_model, "Room", true, t_normal, false);
      AddSensor(node_model, "Underbelly", true, belly_normal, false);
    }

    // Mark as "seen" at the chosen time so age/missing logic behaves.
    node_model->SetBusGpioAndLastUpdate(-1, node_update_ms);
  }

  // Force a full rebuild + value refresh on the next DisplayLoop().
  g_layout_dirty = true;
  g_values_dirty = true;
}

// Placeholder buzzer.
void BuzzerBeepOnce() {
  // TODO: wire to actual buzzer pin.
  DLOG("[BUZZER] Beep\n");
}

// ---------------------------------------------------------------------------
// Tile rebuild
// ---------------------------------------------------------------------------
// Update just the "Age: N min" labels and stale/missing state.
// No layout work, no object (re)creation.
static void GuiAgeTick(uint32_t now_ms) {
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    char node_key[9];
    FormatNodeKey(id, node_key, sizeof(node_key));
    MeshTile *tile = MeshTile::Find(String(node_key));
    if (tile == nullptr) {
      continue;
    }

    const uint32_t age_min = node->ComputeAgeMinutes(now_ms);

    // Age tick treats tiles here as "known topology" and only
    // marks missing; connection state is handled elsewhere.
    const bool node_connected = false; // we only care about age here
    const bool is_missing = g_highlight_missing_nodes && !node_connected &&
                            (g_stale_minutes_threshold > 0U) &&
                            (age_min >= g_stale_minutes_threshold);
    const bool is_stale = node_connected && (g_stale_minutes_threshold > 0U) &&
                          (age_min >= g_stale_minutes_threshold);

    tile->SetAgeOnly(age_min, is_missing, is_stale);
  }
}

// Cheap refresh used on most updates (no object creation or layout).
static void GuiRefreshValues(uint32_t now_ms) {
  // Snapshot current connections.
  auto mesh_node_list = mesh.getNodeList();
  std::vector<uint32_t> connected_ids(mesh_node_list.begin(),
                                      mesh_node_list.end());

  auto is_connected = [&](uint32_t node_id) -> bool {
    for (uint32_t connected_id : connected_ids) {
      if (connected_id == node_id) {
        return true;
      }
    }
    return false;
  };

  // Peers label.
  if (g_ui_label_peers != nullptr) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "Peers: %u",
             static_cast<unsigned>(g_ui_peers));
    lv_label_set_text(g_ui_label_peers, buffer);
  }

  bool any_warning = false;
  bool any_alert = false;

  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    char node_key[9];
    FormatNodeKey(id, node_key, sizeof(node_key));
    const String node_key_hex(node_key);

    MeshTile *tile = MeshTile::Find(node_key_hex);
    if (tile == nullptr) {
      continue;
    }

    const bool node_connected = is_connected(id);

    MeshTile::Content content;
    if (!BuildTileContentForNode(node_key_hex, now_ms, node_connected,
                                 &content)) {
      continue;
    }

    tile->SetContent(content);
    tile->SetFlashIntervalMs(g_flash_interval_ms);

    if (content.node_has_alert) {
      any_alert = true;
    } else if (content.is_stale || content.node_has_warning) {
      any_warning = true;
    }
  }

  g_any_alert = any_alert;
  g_any_warning = any_warning;
}

void GuiRebuildTiles() {
  if (g_ui_tile_container == nullptr) {
    return;
  }

  // Snapshot current connections.
  auto mesh_node_list = mesh.getNodeList();
  std::vector<uint32_t> connected_ids(mesh_node_list.begin(),
                                      mesh_node_list.end());

  auto is_connected = [&](uint32_t node_id) -> bool {
    for (uint32_t connected_id : connected_ids) {
      if (connected_id == node_id) {
        return true;
      }
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

  // Update peers label.
  if (g_ui_label_peers != nullptr) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "Peers: %u",
             static_cast<unsigned>(g_ui_peers));
    lv_label_set_text(g_ui_label_peers, buffer);
  }

  // Collect node ids.
  std::vector<uint32_t> ids = GetAllMeshNodeIds();

  // Sort nodes by tile rank, then label, then id.
  std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
    MeshNode *na = FindMeshNode(a);
    MeshNode *nb = FindMeshNode(b);
    if (na == nullptr || nb == nullptr) {
      return a < b;
    }

    const int32_t ra = na->tile_rank();
    const int32_t rb = nb->tile_rank();
    if (ra != rb) {
      return ra < rb;
    }

    const String &la = na->label();
    const String &lb = nb->label();
    if (la.length() && lb.length()) {
      if (la != lb) {
        return la < lb;
      }
    } else if (la.length() || lb.length()) {
      // Labeled nodes first.
      return la.length() > lb.length();
    }
    return a < b;
  });

  bool any_warning = false;
  bool any_alert = false;

  // Apply sorted order, build content, and push tiles to correct indices.
  for (size_t index = 0; index < ids.size(); ++index) {
    const uint32_t node_id = ids[index];
    MeshNode *node = FindMeshNode(node_id);
    if (node == nullptr) {
      continue;
    }

    char node_key[9];
    FormatNodeKey(node_id, node_key, sizeof(node_key));
    const String node_key_hex(node_key);

    const bool node_connected = is_connected(node_id);

    MeshTile::Content content;
    if (!BuildTileContentForNode(node_key_hex, now_ms, node_connected,
                                 &content)) {
      continue;
    }

    MeshTile *tile = MeshTile::GetOrCreate(node_key_hex, g_ui_tile_container,
                                           tile_w, tile_h);
    if (tile == nullptr) {
      continue;
    }

    tile->SetContent(content);
    tile->SetFlashIntervalMs(g_flash_interval_ms);

    // Reorder LVGL children to match the sorted ranking.
    tile->EnsureChildIndex(static_cast<uint32_t>(index));

    if (content.node_has_alert) {
      any_alert = true;
    } else if (content.is_stale || content.node_has_warning) {
      any_warning = true;
    }
  }

  g_any_alert = any_alert;
  g_any_warning = any_warning;
}

// ---------------------------------------------------------------------------
// House map helpers
// ---------------------------------------------------------------------------

// Canonicalize a user-facing room name into an id string that matches
// RoomDef::id, e.g. "Master Bedroom" -> "masterbedroom".
static String CanonicalRoomId(const String &input) {
  String out;
  out.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    // Keep only letters and digits; drop spaces, apostrophes, punctuation,
    // etc.
    const bool is_alpha = (c >= 'a' && c <= 'z');
    const bool is_digit = (c >= '0' && c <= '9');
    if (!(is_alpha || is_digit)) {
      continue;
    }
    out += c;
  }
  return out;
}

// Find the MeshNode whose label maps to this room, if any.
static MeshNode *FindNodeForRoom(const RoomDef &def) {
  const String target_id(def.id);
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }
    const String &label = node->label();
    if (label.length() == 0) {
      continue;
    }
    const String canon = CanonicalRoomId(label);
    if (canon.equalsIgnoreCase(target_id)) {
      return node;
    }
  }
  return nullptr;
}

// Simple blue->red color map for a temperature range. Range derived from
// warn/alert limits if sensible; otherwise constructed around the value.
static lv_color_t ColorForTemperature(float temp_c, bool has_value) {
  if (!has_value || isnan(temp_c)) {
    return lv_color_make(0x40, 0x40, 0x40);
  }

  float cold_c = g_warn_low_c;
  float hot_c = g_warn_high_c;

  if (!isnan(g_alert_low_c) && (g_alert_low_c < cold_c)) {
    cold_c = g_alert_low_c;
  }
  if (!isnan(g_alert_high_c) && (g_alert_high_c > hot_c)) {
    hot_c = g_alert_high_c;
  }

  if (!(hot_c > cold_c)) {
    cold_c = temp_c - 5.0f;
    hot_c = temp_c + 5.0f;
  }

  float norm = (temp_c - cold_c) / (hot_c - cold_c);
  if (norm < 0.0f) {
    norm = 0.0f;
  } else if (norm > 1.0f) {
    norm = 1.0f;
  }

  const uint8_t red = static_cast<uint8_t>(255.0f * norm);
  const uint8_t blue = static_cast<uint8_t>(255.0f * (1.0f - norm));
  const uint8_t green = 32u;

  return lv_color_make(red, green, blue);
}

// Create or recreate LVGL objects for the house map and all rooms.
// Create or recreate LVGL objects for the house map and all rooms.
static void RoomMapBuildWidgets() {
  if (g_ui_map_container == nullptr) {
    return;
  }

  // Remove any previous map objects, but keep style (padding, bg, etc.).
  lv_obj_clean(g_ui_map_container);
  g_ui_map_house = nullptr;
  g_room_widgets.clear();

  // Logical size of the map area (we treat the container itself as the
  // coordinate system; LVGL already applies padding for us).
  lv_coord_t cont_w = lv_obj_get_content_width(g_ui_map_container);
  lv_coord_t cont_h = lv_obj_get_content_height(g_ui_map_container);
  if (cont_w <= 0 || cont_h <= 0) {
    lv_disp_t *disp = lv_disp_get_default();
    if (disp != nullptr) {
      if (cont_w <= 0) {
        cont_w = lv_disp_get_hor_res(disp);
      }
      if (cont_h <= 0) {
        cont_h = lv_disp_get_ver_res(disp);
      }
    }
    if (cont_w <= 0) {
      cont_w = 320;
    }
    if (cont_h <= 0) {
      cont_h = 240;
    }
  }

  const lv_coord_t inner_w = cont_w;
  const lv_coord_t inner_h = cont_h;

  g_room_widgets.resize(kRoomCount);

  // Find the "outside" room definition (if any).
  int outside_index = -1;
  for (size_t i = 0; i < kRoomCount; ++i) {
    if (kRoomDefs[i].is_outside) {
      outside_index = static_cast<int>(i);
      break;
    }
  }

  lv_obj_t *outside_label_obj = nullptr;
  lv_coord_t outside_label_height = 0;

  // -------------------------------------------------------------------------
  // OUTSIDE: full background rectangle that fills the map container.
  // -------------------------------------------------------------------------
  if (outside_index >= 0) {
    const RoomDef &outside_def = kRoomDefs[outside_index];
    RoomWidget &outside_widget = g_room_widgets[outside_index];
    outside_widget.def = &outside_def;

    lv_obj_t *rect = lv_obj_create(g_ui_map_container);
    outside_widget.rect_objs[0] = rect;

    // Fill the entire map container; padding is already handled by LVGL.
    lv_obj_set_pos(rect, 0, 0);
    lv_obj_set_size(rect, inner_w, inner_h);

    lv_obj_set_style_radius(rect, 0, 0);
    lv_obj_set_style_border_width(rect, 0, 0);
    lv_obj_set_style_outline_width(rect, 0, 0);
    lv_obj_set_style_shadow_width(rect, 0, 0);
    lv_obj_set_style_pad_all(rect, 0, 0);
    lv_obj_set_style_bg_color(rect, lv_color_make(0x20, 0x20, 0x20), 0);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);

    // "Outside" label anchored near the top-left of the map.
    outside_label_obj = lv_label_create(g_ui_map_container);
    outside_widget.label_obj = outside_label_obj;
    lv_label_set_text(outside_label_obj, outside_def.display_name);
    lv_obj_set_style_text_color(outside_label_obj, lv_color_white(), 0);
    lv_obj_set_pos(outside_label_obj, kOutsideLabelPosXPx, kOutsideLabelPosYPx);

    outside_label_height = lv_obj_get_height(outside_label_obj);

    outside_widget.temp_c = NAN;
    outside_widget.has_value = false;
    outside_widget.is_alert = false;
    outside_widget.is_warning = false;
    outside_widget.is_missing = true;
    outside_widget.sequence_stuck = false;
    outside_widget.node_present = false;
    outside_widget.flash_enabled = false;
    outside_widget.base_color = lv_color_make(0x40, 0x40, 0x40);
    outside_widget.base_text_color = lv_color_white();
  }

  // -------------------------------------------------------------------------
  // HOUSE: rectangle inside the map area, just below the outside label.
  // -------------------------------------------------------------------------

  // Compute the usable inner size for the map content.

  if (cont_w <= 0) {
    cont_w = lv_obj_get_width(g_ui_map_container);
  }
  if (cont_h <= 0) {
    cont_h = lv_obj_get_height(g_ui_map_container);
  }

  // Compute margins for the house inside the map.
  lv_coord_t margin_left = kHouseAutoMarginLeftPx;
  lv_coord_t margin_right = kHouseAutoMarginRightPx;
  lv_coord_t margin_top = kHouseAutoMarginTopPx;
  lv_coord_t margin_bottom = kHouseAutoMarginBottomPx;

  // If we have an outside label, push the top margin down to clear it.
  if (outside_label_obj != nullptr && outside_label_height > 0) {
    const lv_coord_t label_bottom = kOutsideLabelPosYPx + outside_label_height;
    const lv_coord_t min_top = label_bottom + kHouseAutoGapBelowOutsideLabelPx;
    if (margin_top < min_top) {
      margin_top = min_top;
    }
  }

  // Make sure margins are sane compared to the available height.
  // Enforce at least 16 px of house height.
  if (margin_top + margin_bottom + 16 > inner_h) {
    margin_top = 8;
    margin_bottom = 8;
    if (margin_top + margin_bottom + 16 > inner_h) {
      margin_top = 4;
      margin_bottom = 4;
    }
  }

  // Width: either fixed or full inner width minus horizontal margins.
  lv_coord_t house_w = kHouseWidthPx;
  if (house_w <= 0 || house_w > inner_w) {
    house_w = inner_w - (margin_left + margin_right);
    if (house_w < 16) {
      house_w = inner_w; // last resort: just fill width
      margin_left = 0;
      margin_right = 0;
    }
  }

  // Height: either fixed or full inner height minus vertical margins.
  lv_coord_t house_h = kHouseHeightPx;
  if (house_h <= 0 || house_h > inner_h) {
    house_h = inner_h - (margin_top + margin_bottom);
    if (house_h < 16) {
      house_h = 16;
      // if this happens, margins were huge; but bottom will still not
      // overflow
    }
  }

  // X position: either fixed or centered between left/right margins.
  lv_coord_t house_x = kHousePosXPx;
  if (house_x < 0 || house_x + house_w > inner_w) {
    const lv_coord_t usable_w = inner_w - (margin_left + margin_right);
    const lv_coord_t extra_space =
        (usable_w > house_w) ? (usable_w - house_w) : 0;
    house_x = margin_left + extra_space / 2;
  }

  // Y position: either fixed or just margin_top.
  lv_coord_t house_y = kHousePosYPx;
  if (house_y < 0 || house_y + house_h > inner_h) {
    house_y = margin_top;
  }

  // Final safety clamp: never let the house run off the bottom.
  if (house_y + house_h > inner_h) {
    house_h = inner_h - house_y;
    if (house_h < 16) {
      house_h = 16;
    }
  }

  g_ui_map_house = lv_obj_create(g_ui_map_container);
  lv_obj_set_size(g_ui_map_house, house_w, house_h);
  lv_obj_set_pos(g_ui_map_house, house_x, house_y);
  lv_obj_set_style_bg_color(g_ui_map_house, lv_color_make(0x30, 0x30, 0x30), 0);
  lv_obj_set_style_bg_opa(g_ui_map_house, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_ui_map_house, 6, 0);
  lv_obj_set_style_border_width(g_ui_map_house, 0, 0);
  lv_obj_set_style_pad_all(g_ui_map_house, 0, 0);
  lv_obj_clear_flag(g_ui_map_house, LV_OBJ_FLAG_SCROLLABLE);

  g_house_w = house_w;
  g_house_h = house_h;

  // Draw house and rooms above the outside background.
  lv_obj_move_foreground(g_ui_map_house);

  // Serial.printf("map: inner_h=%d, house_y=%d, house_h=%d, bottom=%d\n",
  //               (int)inner_h, (int)house_y, (int)house_h,
  //               (int)(house_y + house_h));

  // -------------------------------------------------------------------------
  // INTERIOR ROOMS: rooms inside the house rectangle.
  // -------------------------------------------------------------------------
  for (size_t i = 0; i < kRoomCount; ++i) {
    const RoomDef &def = kRoomDefs[i];
    if (def.is_outside) {
      continue; // already handled
    }

    RoomWidget &widget = g_room_widgets[i];
    widget.def = &def;

    lv_obj_t *parent = g_ui_map_house;

    lv_coord_t parent_w =
        (g_house_w > 0) ? g_house_w : lv_obj_get_content_width(g_ui_map_house);
    lv_coord_t parent_h =
        (g_house_h > 0) ? g_house_h : lv_obj_get_content_height(g_ui_map_house);

    if (parent_w <= 0) {
      parent_w = house_w;
    }
    if (parent_h <= 0) {
      parent_h = house_h;
    }

    // Create each rectangle (supports L-shapes with rect_count == 2).
    for (uint8_t r = 0; r < def.rect_count && r < 2; ++r) {
      const RoomRectNorm &rr = def.rects[r];

      lv_obj_t *rect = lv_obj_create(parent);
      widget.rect_objs[r] = rect;

      const lv_coord_t x = static_cast<lv_coord_t>(
          (static_cast<int32_t>(parent_w) * rr.x1) / 1000);
      const lv_coord_t y = static_cast<lv_coord_t>(
          (static_cast<int32_t>(parent_h) * rr.y1) / 1000);
      lv_coord_t w_px = static_cast<lv_coord_t>(
          (static_cast<int32_t>(parent_w) * (rr.x2 - rr.x1)) / 1000);
      lv_coord_t h_px = static_cast<lv_coord_t>(
          (static_cast<int32_t>(parent_h) * (rr.y2 - rr.y1)) / 1000);

      const lv_coord_t wall_px = 3;
      if (w_px > 2 * wall_px) {
        w_px -= 2 * wall_px;
      }
      if (h_px > 2 * wall_px) {
        h_px -= 2 * wall_px;
      }
      if (w_px < 4) {
        w_px = 4;
      }
      if (h_px < 4) {
        h_px = 4;
      }

      lv_obj_set_pos(rect, x + wall_px, y + wall_px);
      lv_obj_set_size(rect, w_px, h_px);

      lv_obj_set_style_radius(rect, 0, 0);
      lv_obj_set_style_border_width(rect, 0, 0);
      lv_obj_set_style_outline_width(rect, 0, 0);
      lv_obj_set_style_shadow_width(rect, 0, 0);
      lv_obj_set_style_pad_all(rect, 2, 0);
      lv_obj_set_style_bg_color(rect, lv_color_make(0x40, 0x40, 0x40), 0);
      lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
      lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Label: sibling of rectangles, anchored to the first rect.
    if (widget.rect_objs[0] != nullptr) {
      widget.label_obj = lv_label_create(g_ui_map_house);
      lv_label_set_text(widget.label_obj, def.display_name);
      lv_obj_set_style_text_color(widget.label_obj, lv_color_white(), 0);
      lv_obj_align_to(widget.label_obj, widget.rect_objs[0], LV_ALIGN_TOP_MID,
                      0, 0);
    } else {
      widget.label_obj = lv_label_create(g_ui_map_house);
      lv_label_set_text(widget.label_obj, def.display_name);
      lv_obj_set_style_text_color(widget.label_obj, lv_color_white(), 0);
      lv_obj_align(widget.label_obj, LV_ALIGN_TOP_MID, 0, 0);
    }

    widget.temp_c = NAN;
    widget.has_value = false;
    widget.is_alert = false;
    widget.is_warning = false;
    widget.is_missing = true;
    widget.sequence_stuck = false;
    widget.node_present = false;
    widget.flash_enabled = false;
    widget.base_color = lv_color_make(0x40, 0x40, 0x40);
    widget.base_text_color = lv_color_white();
  }

  // Ensure the outside label (if any) is drawn above the house.
  if (outside_label_obj != nullptr) {
    lv_obj_move_foreground(outside_label_obj);
  }
}

// Rebuild map layout and immediately refresh visuals.
static void RoomMapRebuild(uint32_t now_ms) {
  if (g_ui_map_container == nullptr) {
    return;
  }
  RoomMapBuildWidgets();
  RoomMapRefresh(now_ms);
}

// Update colors, labels and flash flags for each room based on current
// MeshNode state and the Room/Underbelly selector.
//
// NEW: we now evaluate BOTH "Room" and "Underbelly" sensors for limits and
// stale/missing, regardless of which one is currently displayed. Flashing
// is triggered if EITHER sensor is in alert/warning or has stale/missing
// data. The numeric label still shows only the currently selected source.
static void RoomMapRefresh(uint32_t now_ms) {
  if (g_ui_map_container == nullptr) {
    return;
  }
  if (g_room_widgets.empty()) {
    RoomMapBuildWidgets();
  }

  // Snapshot mesh connectivity.
  auto mesh_node_list = mesh.getNodeList();
  std::vector<uint32_t> connected_ids(mesh_node_list.begin(),
                                      mesh_node_list.end());
  auto IsConnected = [&](uint32_t node_id) -> bool {
    for (uint32_t connected_id : connected_ids) {
      if (connected_id == node_id) {
        return true;
      }
    }
    return false;
  };

  const char *kRoomLabel = "Room";
  const char *kUnderbellyLabel = "Underbelly";

  // Per-sensor summary for this refresh.
  struct SensorState {
    bool present = false;      // sensor with that label exists
    bool has_value = false;    // has a valid numeric reading
    bool missing_data = false; // label exists but no valid value
    bool stale = false;        // value too old by g_stale_minutes_threshold
    bool alert = false;        // over/under alert limits (respecting mute)
    bool warn = false;         // over/under warn limits (respecting mute)
    float temp_c = NAN;        // last value (if has_value)
  };

  for (auto &widget : g_room_widgets) {
    widget.temp_c = NAN;
    widget.has_value = false;
    widget.is_alert = false;
    widget.is_warning = false;
    widget.is_missing = true;
    widget.sequence_stuck = false;
    widget.node_present = false;
    widget.flash_enabled = false;

    const RoomDef *def = widget.def;
    if (def == nullptr) {
      continue;
    }

    MeshNode *node = FindNodeForRoom(*def);
    if (node == nullptr) {
      if (widget.label_obj != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n--", def->display_name);
        lv_label_set_text(widget.label_obj, buf);
      }
      continue;
    }

    widget.node_present = true;

    const uint32_t node_id = node->node_id();
    const bool node_connected = IsConnected(node_id);
    const uint32_t age_min = node->ComputeAgeMinutes(now_ms);

    const bool node_missing = g_highlight_missing_nodes && !node_connected &&
                              (g_stale_minutes_threshold > 0U) &&
                              (age_min >= g_stale_minutes_threshold);

    const bool node_stale = node_connected &&
                            (g_stale_minutes_threshold > 0U) &&
                            (age_min >= g_stale_minutes_threshold);

    const bool seq_stuck = node->SequenceStuck(now_ms, kSeqStuckMsThreshold);
    widget.sequence_stuck = seq_stuck;

    const uint8_t node_mute = node->mute_mask() & 0x3u;

    // Helper to evaluate one sensor ("Room" or "Underbelly").
    auto EvalSensor = [&](const MeshNode::Sensor *sensor, SensorState &state) {
      if (sensor == nullptr) {
        // No such sensor label on this node: treat as neutral.
        state.present = false;
        state.has_value = false;
        state.missing_data = false;
        state.stale = false;
        state.alert = false;
        state.warn = false;
        state.temp_c = NAN;
        return;
      }

      state.present = true;
      state.temp_c = sensor->temp_c;

      const bool has_val = sensor->has_value && !isnan(sensor->temp_c);
      state.has_value = has_val;
      state.missing_data = !has_val;

      // Per-sensor staleness (uses sensor->last_ms, not just node age).
      state.stale = false;
      if (has_val && sensor->last_ms != 0U && now_ms >= sensor->last_ms &&
          g_stale_minutes_threshold > 0U) {
        const uint32_t age_sensor_min = (now_ms - sensor->last_ms) / 60000U;
        if (age_sensor_min >= g_stale_minutes_threshold) {
          state.stale = true;
        }
      }

      state.alert = false;
      state.warn = false;

      if (!has_val) {
        // No numeric value → no limit comparison, just missing_data/stale.
        return;
      }

      const float t = sensor->temp_c;

      // Alert limits.
      bool low_alert = (!isnan(g_alert_low_c) && t < g_alert_low_c);
      bool high_alert = (!isnan(g_alert_high_c) && t > g_alert_high_c);
      if (low_alert && (node_mute & kMuteLow)) {
        low_alert = false;
      }
      if (high_alert && (node_mute & kMuteHigh)) {
        high_alert = false;
      }
      state.alert = (low_alert || high_alert);

      // Warn limits (only if not already alert).
      if (!state.alert) {
        bool low_warn = (!isnan(g_warn_low_c) && t < g_warn_low_c);
        bool high_warn = (!isnan(g_warn_high_c) && t > g_warn_high_c);
        if (low_warn && (node_mute & kMuteLow)) {
          low_warn = false;
        }
        if (high_warn && (node_mute & kMuteHigh)) {
          high_warn = false;
        }
        state.warn = (low_warn || high_warn);
      }
    };

    // Evaluate both sensors, regardless of which one we are displaying.
    SensorState room_state;
    SensorState belly_state;

    const MeshNode::Sensor *room_sensor =
        node->FindSensorByLabel(String(kRoomLabel));
    const MeshNode::Sensor *belly_sensor =
        node->FindSensorByLabel(String(kUnderbellyLabel));

    EvalSensor(room_sensor, room_state);
    EvalSensor(belly_sensor, belly_state);

    // Aggregate severity across both sensors + node health.
    const bool any_thresh_alert = room_state.alert || belly_state.alert;
    const bool any_thresh_warn = room_state.warn || belly_state.warn;
    const bool any_sensor_missing =
        room_state.missing_data || belly_state.missing_data;
    const bool any_sensor_stale = room_state.stale || belly_state.stale;

    widget.is_missing = node_missing || any_sensor_missing;

    widget.is_alert = false;
    widget.is_warning = false;

    if (node_missing) {
      // Node disappeared entirely → treat as hard alert.
      widget.is_alert = true;
    } else if (any_thresh_alert) {
      // Either Room or Underbelly outside alert limits.
      widget.is_alert = true;
    } else if (any_sensor_missing) {
      // We *should* have data (sensor label exists) but don't.
      widget.is_alert = true;
    } else if (any_thresh_warn || node_stale || seq_stuck || any_sensor_stale) {
      // Anything in warning/stale territory → warning flash.
      widget.is_warning = true;
    }

    // Decide which sensor to *display* (but flashing is already based on
    // BOTH).
    const bool use_belly = g_map_use_underbelly;
    const SensorState &display_state = use_belly ? belly_state : room_state;

    if (!node_missing && display_state.present && display_state.has_value) {
      widget.has_value = true;
      widget.temp_c = display_state.temp_c;
    } else {
      widget.has_value = false;
      widget.temp_c = NAN;
    }

    widget.flash_enabled = (widget.is_alert || widget.is_warning);

    const lv_color_t fill_color =
        ColorForTemperature(widget.temp_c, widget.has_value);
    widget.base_color = fill_color;

    lv_color_t border_color = lv_color_make(0x10, 0x10, 0x10);
    if (widget.is_alert) {
      border_color = lv_color_make(0xFF, 0x40, 0x40);
    } else if (widget.is_warning) {
      border_color = lv_color_make(0xFF, 0xD0, 0x40);
    } else if (widget.sequence_stuck) {
      border_color = lv_color_make(0x80, 0x40, 0xFF);
    }

    for (lv_obj_t *rect : widget.rect_objs) {
      if (rect == nullptr) {
        continue;
      }
      lv_obj_set_style_bg_color(rect, fill_color, 0);
      lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(rect, border_color, 0);
    }

    if (widget.label_obj != nullptr) {
      if (widget.has_value) {
        float display_temp = widget.temp_c;
        const char *unit = "C";
        if (g_display_fahrenheit) {
          display_temp = widget.temp_c * 1.8f + 32.0f;
          unit = "F";
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n%.1f %s", def->display_name,
                 static_cast<double>(display_temp), unit);
        lv_label_set_text(widget.label_obj, buf);
      } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n--", def->display_name);
        lv_label_set_text(widget.label_obj, buf);
      }
    }
  }
}

// Per-frame flashing for map rooms, sharing the global flash interval.
static void RoomMapLoop(uint32_t now_ms) {
  if (g_ui_map_container == nullptr || g_room_widgets.empty()) {
    return;
  }

  // If flashing is disabled, force everything back to the base colors.
  if (g_flash_interval_ms == 0) {
    for (auto &widget : g_room_widgets) {
      for (lv_obj_t *rect : widget.rect_objs) {
        if (rect != nullptr) {
          lv_obj_set_style_bg_color(rect, widget.base_color, 0);
          lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
        }
      }
      if (widget.label_obj != nullptr) {
        lv_obj_set_style_text_color(widget.label_obj, widget.base_text_color,
                                    0);
      }
    }
    return;
  }

  bool any_flashing = false;
  for (const auto &widget : g_room_widgets) {
    if (widget.flash_enabled) {
      any_flashing = true;
      break;
    }
  }

  // If nothing has flash enabled, also force the base appearance.
  if (!any_flashing) {
    for (auto &widget : g_room_widgets) {
      for (lv_obj_t *rect : widget.rect_objs) {
        if (rect != nullptr) {
          lv_obj_set_style_bg_color(rect, widget.base_color, 0);
          lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
        }
      }
      if (widget.label_obj != nullptr) {
        lv_obj_set_style_text_color(widget.label_obj, widget.base_text_color,
                                    0);
      }
    }
    return;
  }

  static uint32_t last_toggle_ms = 0;
  static bool flash_white_phase = false;

  if (now_ms - last_toggle_ms >= g_flash_interval_ms) {
    flash_white_phase = !flash_white_phase;
    last_toggle_ms = now_ms;
  }

  for (auto &widget : g_room_widgets) {
    // Rooms that are not in a warning/alert state always show base colors.
    bool use_white_flash = (widget.flash_enabled && flash_white_phase);

    lv_color_t rect_color = widget.base_color;
    lv_color_t text_color = widget.base_text_color;

    if (use_white_flash) {
      rect_color = lv_color_white();
      text_color = lv_color_black();
    }

    for (lv_obj_t *rect : widget.rect_objs) {
      if (rect == nullptr) {
        continue;
      }
      lv_obj_set_style_bg_color(rect, rect_color, 0);
      lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
    }

    if (widget.label_obj != nullptr) {
      lv_obj_set_style_text_color(widget.label_obj, text_color, 0);
    }
  }
}

static void RoomMapSetViewMode(UiViewMode mode) {
  if (g_ui_view_mode == mode) {
    return;
  }
  g_ui_view_mode = mode;

  if (!lvgl_port_lock(-1)) {
    return;
  }

  if (g_ui_tile_container != nullptr) {
    if (mode == kUiViewModeTiles) {
      lv_obj_clear_flag(g_ui_tile_container, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(g_ui_tile_container, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (g_ui_map_container != nullptr) {
    if (mode == kUiViewModeMap) {
      lv_obj_clear_flag(g_ui_map_container, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(g_ui_map_container, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (g_ui_label_title != nullptr) {
    if (mode == kUiViewModeMap) {
      lv_label_set_text(g_ui_label_title, "House Map");
    } else {
      lv_label_set_text(g_ui_label_title, "Room Temps");
    }
  }

  // NEW: keep the map/tile button label in sync with current mode
  if (g_ui_button_map_label != nullptr) {
    lv_label_set_text(g_ui_button_map_label,
                      (g_ui_view_mode == kUiViewModeMap) ? "Map" : "Tiles");
  }

  lvgl_port_unlock();

  g_layout_dirty = true;
  g_values_dirty = true;
}

static void SilenceButtonEvent(lv_event_t *e) {
  (void)e;

  // If we're in console-driven test mode, treat "Silence" as
  // "stop the test now", even if there are no real alerts/warnings.
  if (g_buzzer_test_active) {
    BuzzerStopTestInternal();

    // No need to touch the silence state machine here; this was only a test.
    // Just ensure the buzzer is off and the button appearance is up to date.
    digitalWrite(kBuzzerPin, LOW);
    g_buzzer_on = false;
    g_buzzer_beep_start_ms = 0;
    g_buzzer_last_beep_ms = 0;

    UpdateSilenceButtonAppearance();
    return;
  }

  // --- Existing behaviour for real alerts/warnings -------------------------

  // Do nothing if already in a silenced / cooldown state.
  if (g_buzzer_silence_state != kBuzzerSilenceIdle) {
    return;
  }

  // Only makes sense to silence if something is actually alarming or warning.
  if (!g_any_alert && !g_any_warning) {
    return;
  }

  // Enter "active silence" for the current alert episode.
  g_buzzer_silence_state = kBuzzerSilenceActive;
  g_buzzer_silence_rearm_at_ms = 0;

  // Immediately force the buzzer off and reset its timing.
  digitalWrite(kBuzzerPin, LOW);
  g_buzzer_on = false;
  g_buzzer_beep_start_ms = 0;
  g_buzzer_last_beep_ms = 0;

  // Turn the button red.
  UpdateSilenceButtonAppearance();
}



static void BellyButtonEvent(lv_event_t *e) {
  (void)e;
  g_map_use_underbelly = !g_map_use_underbelly;

  if (g_ui_button_belly_label != nullptr) {
    lv_label_set_text(g_ui_button_belly_label,
                      g_map_use_underbelly ? "Underbelly" : "Room");
  }

  g_layout_dirty = true;
  g_values_dirty = true;
}

static void ViewButtonEvent(lv_event_t *e) {
  (void)e;

  switch (g_ui_view_mode) {
  case kUiViewModeMap:
    RoomMapSetViewMode(kUiViewModeTiles);
    break;
  case kUiViewModeTiles:
    RoomMapSetViewMode(kUiViewModeMap);
    g_map_use_underbelly = false;
    break;
  }
}

static void ProcessNtfyAlerts(uint32_t now_ms) {
  if (!g_ntfy_config.enabled || g_use_dummy_data) {
    return;
  }

  auto mesh_node_list = mesh.getNodeList();
  std::vector<uint32_t> connected_ids(mesh_node_list.begin(), mesh_node_list.end());
  auto is_connected = [&](uint32_t node_id) -> bool {
    for (uint32_t connected_id : connected_ids) {
      if (connected_id == node_id) {
        return true;
      }
    }
    return false;
  };

  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    const bool node_connected = is_connected(id);
    const uint32_t age_min = node->ComputeAgeMinutes(now_ms);
    const bool is_missing = !node_connected && (g_stale_minutes_threshold > 0U) &&
                           (age_min >= g_stale_minutes_threshold);
    const bool is_stale = node_connected && (g_stale_minutes_threshold > 0U) &&
                          (age_min >= g_stale_minutes_threshold);
    const bool sequence_stuck =
        node->SequenceStuck(now_ms, kSeqStuckMsThreshold);
    const uint8_t node_mute = node->mute_mask() & 0x3u;

    bool node_has_alert = false;
    bool node_has_warning = false;
    bool sensor_has_warning = false;
    String node_normal_detail;

    for (const auto &sensor : node->sensors()) {
      bool low_alert = false, high_alert = false, low_warn = false, high_warn = false;
      const AlertState state = EvaluateSensorState(sensor, node_mute, &low_alert,
                                                  &high_alert, &low_warn, &high_warn);
      if (state == AlertState::kAlert) {
        node_has_alert = true;
      } else if (state == AlertState::kWarning) {
        sensor_has_warning = true;
      }

      if (node_normal_detail.isEmpty() && sensor.has_value && !isnan(sensor.temp_c)) {
        node_normal_detail = FormatTempForMessage(sensor.temp_c);
      }

      const String sensor_key = String("S:") + node->node_id_str() + ":" +
                                CanonAddr16(sensor.address);
      const String subject = NodeDisplayName(node) + " / " + SensorDisplayName(sensor);

      String detail;
      String resolved_detail;
      if (sensor.has_value && !isnan(sensor.temp_c)) {
        resolved_detail = FormatTempForMessage(sensor.temp_c);
      }

      if (state != AlertState::kNormal && sensor.has_value && !isnan(sensor.temp_c)) {
        detail = FormatTempForMessage(sensor.temp_c);
        if (low_alert) detail += " < alert low";
        if (high_alert) detail += " > alert high";
        if (low_warn && !low_alert) detail += " < warn low";
        if (high_warn && !high_alert) detail += " > warn high";
        const String limits = FormatLimitSummary();
        if (limits.length() > 0) {
          detail += " (";
          detail += limits;
          detail += ")";
        }
      }

      EmitStateNotification(sensor_key, state, subject, detail, resolved_detail,
                            /*cache=*/true);
    }

    if (sequence_stuck && !node_has_alert) {
      node_has_warning = true;
    }
    if (is_stale && !node_has_alert) {
      node_has_warning = true;
    }

    AlertState node_state = AlertState::kNormal;
    String node_detail;
    if (is_missing) {
      node_state = AlertState::kMissing;
      node_detail = "last update " + String(age_min) + " min ago";
    } else if (node_has_alert) {
      node_state = AlertState::kAlert;
      node_detail = "sensor alert active";
    } else if (node_has_warning) {
      node_state = AlertState::kWarning;
      if (sequence_stuck) {
        node_detail = "sequence stuck";
      } else if (is_stale) {
        node_detail = "data stale (" + String(age_min) + " min)";
      }
    } else if (sensor_has_warning) {
      // Sensor warnings are reported per-sensor; avoid node-level noise.
      node_state = AlertState::kNormal;
    }

    const String node_key = String("N:") + node->node_id_str();
    EmitStateNotification(node_key, node_state, NodeDisplayName(node), node_detail,
                          node_normal_detail, /*cache=*/true,
                          /*include_normal_detail_on_resolve=*/false);
  }
}

static void MaybeSendNtfySummary(uint32_t now_ms) {
  if (!g_ntfy_config.enabled || !g_ntfy_config.summary_enabled || g_use_dummy_data) {
    return;
  }
  if (g_ntfy_config.summary_period_min == 0U) {
    return;
  }

  const uint32_t period_ms = g_ntfy_config.summary_period_min * 60000UL;
  if (g_ntfy_last_summary_ms == 0U) {
    g_ntfy_last_summary_ms = now_ms;
    return;
  }
  if (now_ms - g_ntfy_last_summary_ms < period_ms) {
    return;
  }
  g_ntfy_last_summary_ms = now_ms;

  String message;
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (size_t i = 0; i < ids.size(); ++i) {
    MeshNode *node = FindMeshNode(ids[i]);
    if (node == nullptr) {
      continue;
    }

    message += NodeDisplayName(node);
    message += ":\n";

    size_t sensor_lines = 0;
    for (const auto &sensor : node->sensors()) {
      if (!sensor.has_value || isnan(sensor.temp_c)) {
        continue;
      }
      message += "  - ";
      message += SensorDisplayName(sensor);
      message += ": ";
      message += FormatTempForMessage(sensor.temp_c);
      message += "\n";
      ++sensor_lines;
    }

    if (sensor_lines == 0) {
      message += "  - (no readings)\n";
    }

    if (i + 1 < ids.size()) {
      message += "\n";
    }
  }

  if (message.length() > 0) {
    SendNtfyRequestToBridge(message, /*cache_when_offline=*/false,
                            /*is_summary=*/true, "MeshTemps summary");
  }
}

static void NtfyLoop() {
  static uint32_t last_check_ms = 0;
  const uint32_t now_ms = millis();
  if (last_check_ms != 0U && (now_ms - last_check_ms) < 2000U) {
    return; // throttle to avoid heavy work every loop()
  }
  last_check_ms = now_ms;

  ProcessNtfyAlerts(now_ms);
  MaybeSendNtfySummary(now_ms);
  MaybeSendQueuedNtfyToBridge();
}

// -----------------------------------------------------------------------------
// GUI helpers callable from elsewhere
// -----------------------------------------------------------------------------

void GuiUpdateNetwork(size_t peers) {
  const size_t node_count = GetAllMeshNodeIds().size();
  const size_t effective_peers = std::max(peers, node_count);

  g_ui_peers = effective_peers;
  g_values_dirty = true; // label text only
}

// CHANGED
void GuiRequestRender() { g_layout_dirty = true; }

// -----------------------------------------------------------------------------
// Display loop: rebuild tiles + buzzer + periodic refresh
// -----------------------------------------------------------------------------
// Tiles are now self-contained: each tile tracks its own flash timer and
// decides when to repaint itself based on its Content and per-tile
// flash interval.
void DisplayLoop() {
  static uint32_t last_build_ms = 0;
  static uint32_t last_minute_ms = 0;

  // NEW: debug – detect long gaps between DisplayLoop calls.
  static uint32_t last_loop_ms = 0;
  const uint32_t now_ms = millis();

  if (g_debug_enabled && last_loop_ms != 0) {
    const uint32_t delta = now_ms - last_loop_ms;
    if (delta > 300U) {
      Serial.printf("[DisplayLoop] gap=%lu ms\n",
                    static_cast<unsigned long>(delta));
    }
  }
  last_loop_ms = now_ms;

  if (!lvgl_port_lock(-1)) {
    return;
  }

  // Minute tick: age text + uptime (no layout work).
  if (now_ms - last_minute_ms >= 60000U) {
    GuiAgeTick(now_ms);
    last_minute_ms = now_ms;
  }

  // Layout / content refresh throttled to avoid hammering LVGL.
  if ((g_layout_dirty || g_values_dirty) && (now_ms - last_build_ms >= 50U)) {
    if (g_layout_dirty) {
      g_layout_dirty = false;
      GuiRebuildTiles();      // heavy path, rare
      RoomMapRebuild(now_ms); // mirror into map view
      g_values_dirty = false; // rebuilding already refreshed content
    } else if (g_values_dirty) {
      g_values_dirty = false;
      GuiRefreshValues(now_ms); // cheap path, frequent
      RoomMapRefresh(now_ms);
    }
    last_build_ms = now_ms;
  }

  MeshTile::LoopAll(now_ms);
  RoomMapLoop(now_ms);

  UpdateUptimeLabel();
  GuiUpdateNtpStatusIcon();

  UpdateSilenceButtonAppearance();
  
  lvgl_port_unlock();
}

// Start console-driven buzzer test mode.
static void BuzzerStartTestInternal(bool use_alert_profile) {
  g_buzzer_test_use_alert_pattern = use_alert_profile;
  g_buzzer_test_active = true;

  // Test should not be blocked by any prior silence/cooldown state.
  g_buzzer_silence_state = kBuzzerSilenceIdle;
  g_buzzer_silence_rearm_at_ms = 0;

  // Reset scheduler so the first test beep comes quickly.
  digitalWrite(kBuzzerPin, LOW);
  g_buzzer_on = false;
  g_buzzer_beep_start_ms = 0;
  g_buzzer_last_beep_ms = 0;

  DLOG("[BUZZER] Test mode ON (%s pattern)\n",
       use_alert_profile ? "alert" : "warn");
}

static void BuzzerStopTestInternal() {
  if (!g_buzzer_test_active) {
    return;
  }

  g_buzzer_test_active = false;
  digitalWrite(kBuzzerPin, LOW);
  g_buzzer_on = false;
  g_buzzer_beep_start_ms = 0;
  g_buzzer_last_beep_ms = 0;

  DLOG("[BUZZER] Test mode OFF\n");
}

void BuzzerLoop() {
  const uint32_t now_ms = millis();

  // Real alert/warning state from the tile/map logic.
  const bool any_real_active = (g_any_alert || g_any_warning);

  // --- Silence / cooldown state machine (real alerts only) ------------------
  switch (g_buzzer_silence_state) {
  case kBuzzerSilenceIdle:
    // Normal operation; nothing special.
    break;

  case kBuzzerSilenceActive:
    // User has pressed "Silence" for the current episode.
    // Stay here while there are active alerts/warnings.
    if (!any_real_active) {
      // Alerts cleared → start cooldown timer.
      g_buzzer_silence_state = kBuzzerSilenceCooldown;
      g_buzzer_silence_rearm_at_ms = now_ms + g_buzzer_cooldown_ms;
    }
    break;

  case kBuzzerSilenceCooldown:
    // Ignore any new alerts until both:
    //   - cooldown has expired, AND
    //   - there are no active alerts/warnings.
    if (!any_real_active && g_buzzer_silence_rearm_at_ms != 0U &&
        now_ms >= g_buzzer_silence_rearm_at_ms) {
      g_buzzer_silence_state = kBuzzerSilenceIdle;
      g_buzzer_silence_rearm_at_ms = 0;
    }
    break;
  }

  const bool silence_in_effect =
      (g_buzzer_silence_state == kBuzzerSilenceActive ||
       g_buzzer_silence_state == kBuzzerSilenceCooldown);

  // If a real alert episode is active (and not silenced), it owns the buzzer
  // and cancels any console-driven test.
  if (any_real_active && !silence_in_effect && g_buzzer_test_active) {
    DLOG("[BUZZER] Real alert active; cancelling test mode\n");
    BuzzerStopTestInternal();
  }

  // Decide what *pattern* should drive the buzzer right now.
  bool beep_should_run = false;
  uint32_t gap_ms = 0;

  // 1) Real alert/warning pattern (highest priority).
  if (any_real_active && !silence_in_effect) {
    bool use_alert_profile = g_any_alert;
    bool use_warn_profile = (!use_alert_profile && g_any_warning);

    if (use_alert_profile) {
      beep_should_run = true;
      gap_ms = g_beep_gap_alert_ms;
    } else if (use_warn_profile) {
      beep_should_run = true;
      gap_ms = g_beep_gap_warn_ms;
    }
  }

  // 2) If no real alert is currently driving the buzzer, allow test mode.
  if (!beep_should_run && g_buzzer_test_active) {
    beep_should_run = true;
    gap_ms = g_buzzer_test_use_alert_pattern ? g_beep_gap_alert_ms
                                             : g_beep_gap_warn_ms;
  }

  const bool beep_disabled =
      (!beep_should_run || gap_ms == 0 || g_beep_len_ms == 0);

  // If there is nothing to beep about, or it's disabled, ensure the buzzer
  // is off and reset scheduling appropriately.
  if (beep_disabled) {
    if (g_buzzer_on) {
      digitalWrite(kBuzzerPin, LOW);
      g_buzzer_on = false;
    }

    // For real-alert episodes, reset schedule when they're inactive or silenced
    // so the next re-armed episode starts cleanly. Test mode handles its own
    // reset via BuzzerStartTestInternal().
    if (!any_real_active || silence_in_effect) {
      g_buzzer_last_beep_ms = 0;
    }
    return;
  }

  // --- Normal beep scheduling (shared for real + test) ----------------------

  if (g_buzzer_on) {
    // Turn off after configured beep length.
    if (now_ms - g_buzzer_beep_start_ms >= g_beep_len_ms) {
      digitalWrite(kBuzzerPin, LOW);
      g_buzzer_on = false;
      g_buzzer_last_beep_ms = now_ms;
    }
  } else {
    // Start a beep if it's the first one, or the gap has elapsed.
    if (g_buzzer_last_beep_ms == 0U ||
        now_ms - g_buzzer_last_beep_ms >= gap_ms) {
      digitalWrite(kBuzzerPin, HIGH);
      g_buzzer_on = true;
      g_buzzer_beep_start_ms = now_ms;
    }
  }
}

// -----------------------------------------------------------------------------
// Console command handlers (ROOT)
// Signature required by SerialConsole:
//   void Handler(void* ctx, int argc, const String argv[], Print& out)
// -----------------------------------------------------------------------------

static void MaybeRequestTimeFromBridge(const char *reason,
                                       bool force_ntp = false);

// Passthrough controls are needed by console handlers defined below; declare
// them here so they are available before the bridge serial section.
static bool g_bridge_passthrough = false;
static bool g_bridge_passthrough_escape = false;
static String g_bridge_passthrough_tx_line;

static void CmdHelp(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  g_console.PrintHelp(out);
}

static void CmdPassthru(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.printf("passthru %s\n", g_bridge_passthrough ? "on" : "off");
    out.println(F("Use 'passthru on' to mirror GUI<->bridge UART0 to USB."));
    out.println(F("Exit passthrough with '~.' when active."));
    return;
  }

  if (argv[1].equalsIgnoreCase("on")) {
    g_bridge_passthrough = true;
    g_bridge_passthrough_escape = false;
    g_bridge_passthrough_tx_line = "";
    out.println(F("[BRIDGE<->GUI] passthrough enabled; exit with '~.'"));
    return;
  }

  if (argv[1].equalsIgnoreCase("off")) {
    g_bridge_passthrough = false;
    g_bridge_passthrough_tx_line = "";
    out.println(F("[BRIDGE<->GUI] passthrough disabled"));
    return;
  }

  out.println(F("ERR passthru (use on|off)"));
}

static void CmdTime(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.println(F("usage: time now|request|sync"));
    return;
  }

  const String sub = argv[1];
  if (sub.equalsIgnoreCase("now") || sub.equalsIgnoreCase("show")) {
    PrintCurrentLocalTime(out);
    return;
  }

  if (sub.equalsIgnoreCase("request")) {
    MaybeRequestTimeFromBridge("console_request", /*force_ntp=*/false);
    out.println(F("[TIME] requested time_sync from bridge"));
    return;
  }

  if (sub.equalsIgnoreCase("sync") || sub.equalsIgnoreCase("force")) {
    MaybeRequestTimeFromBridge("console_force", /*force_ntp=*/true);
    out.println(F("[TIME] requested bridge NTP sync"));
    return;
  }

  out.println(F("ERR time (use now|request|sync)"));
}

static void CmdKnown(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc >= 2 && argv[1] == "erase") {
    g_root_preferences.begin("meshroot", false);
    bool is_successful = true;
    is_successful =
        NvsRemoveKeyVerified(g_root_preferences, "known_bin") && is_successful;
    is_successful =
        NvsRemoveKeyVerified(g_root_preferences, "known") && is_successful;
    g_root_preferences.end();

    if (!is_successful) {
      out.println(F("[NVS VERIFY] CmdKnown erase failed"));
    } else {
      out.println(F("known erased"));
    }
    return;
  }

  std::vector<uint32_t> ids;
  (void)ReadKnownFromNVS(&ids);
  if (ids.empty()) {
    out.println(F("(empty)"));
    return;
  }
  out.printf("known count=%u\n", static_cast<unsigned>(ids.size()));
  for (uint32_t id : ids) {
    out.printf("0x%08lX\n", static_cast<unsigned long>(id));
  }
}

static void CmdMute(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // mute list
  if (argc >= 2 && argv[1] == "list") {
    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    if (ids.empty()) {
      out.println(F("mute list: (no nodes)"));
      return;
    }

    out.printf("mute list: %u node(s)\n", static_cast<unsigned>(ids.size()));

    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }
      const uint8_t mask = node->mute_mask() & 0x3u;
      char key[9];
      FormatNodeKey(id, key, sizeof(key));
      out.printf("%s : %s\n", key, NodeMuteMaskToString(mask));
    }
    out.println(F("mute list: done"));
    return;
  }

  // mute get <nodeIdHex>
  if (argc >= 3 && argv[1] == "get") {
    uint32_t id_u32 = 0;
    if (!ParseNodeIdToU32(argv[2], &id_u32)) {
      out.println(F("ERR mute get (nodeId must be hex, e.g. 0x1234ABCD)"));
      return;
    }
    MeshNode *node = FindMeshNode(id_u32);
    const uint8_t mask =
        (node != nullptr) ? (node->mute_mask() & 0x3u) : kMuteNone;
    out.printf("mute get 0x%08lX : %s\n", static_cast<unsigned long>(id_u32),
               NodeMuteMaskToString(mask));
    return;
  }

  // mute <nodeIdHex> off|low|high|both
  if (argc < 3) {
    out.println(
        F("ERR mute (use: mute <nodeIdHex> off|low|high|both | mute get "
          "<nodeIdHex> | mute list)"));
    return;
  }

  uint32_t id_u32 = 0;
  if (!ParseNodeIdToU32(argv[1], &id_u32)) {
    out.println(F("ERR mute (nodeId must be hex, e.g. 0x1234ABCD)"));
    return;
  }

  const String mode = argv[2];
  uint8_t mask = kMuteNone;
  if (mode.equalsIgnoreCase("off")) {
    mask = kMuteNone;
  } else if (mode.equalsIgnoreCase("low")) {
    mask = kMuteLow;
  } else if (mode.equalsIgnoreCase("high")) {
    mask = kMuteHigh;
  } else if (mode.equalsIgnoreCase("both") || mode.equalsIgnoreCase("all") ||
             mode.equalsIgnoreCase("on")) {
    mask = kMuteBoth;
  } else {
    out.println(F("ERR mute (use: off|low|high|both)"));
    return;
  }

  MeshNode *node = GetOrCreateMeshNode(id_u32);
  if (node != nullptr) {
    node->set_mute_mask(mask);
    SaveNodeMetaToNVSIfChanged();
  }

  out.printf("mute 0x%08lX=%s\n", static_cast<unsigned long>(id_u32),
             NodeMuteMaskToString(mask));
  GuiRequestRender();
}

static void CmdLs(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  const uint32_t now_ms = millis();
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  if (ids.empty()) {
    out.println(F("(no nodes)"));
  }

  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    const String node_hex = node->node_key_hex();
    const String node_label = node->label();
    const uint32_t age_min = node->ComputeAgeMinutes(now_ms);
    const int bus_gpio = node->bus_gpio();

    if (node_label.length() > 0) {
      out.printf("node %s \"%s\" gpio=%d age=%lu min:\n", node_hex.c_str(),
                 node_label.c_str(), bus_gpio,
                 static_cast<unsigned long>(age_min));
    } else {
      out.printf("node %s gpio=%d age=%lu min:\n", node_hex.c_str(), bus_gpio,
                 static_cast<unsigned long>(age_min));
    }

    if (node->has_sequence()) {
      const uint32_t seq = node->last_sequence();
      const uint32_t last_adv_ms = node->last_sequence_advance_ms();
      const uint32_t last_rx_ms = node->last_sequence_rx_ms();

      uint32_t since_adv_s = 0U;
      uint32_t since_rx_s = 0U;

      if (last_adv_ms != 0U && now_ms >= last_adv_ms) {
        since_adv_s = (now_ms - last_adv_ms) / 1000U;
      }
      if (last_rx_ms != 0U && now_ms >= last_rx_ms) {
        since_rx_s = (now_ms - last_rx_ms) / 1000U;
      }

      const bool stuck = node->SequenceStuck(now_ms, kSeqStuckMsThreshold);

      out.printf(
          "  seq=%lu last_adv=%lus last_rx=%lus dup_rx=%lu stuck=%s\n",
          static_cast<unsigned long>(seq),
          static_cast<unsigned long>(since_adv_s),
          static_cast<unsigned long>(since_rx_s),
          static_cast<unsigned long>(node->duplicate_sequence_rx_count()),
          stuck ? "YES" : "no");
    }

    const auto &sensors = node->sensors();
    if (sensors.empty()) {
      out.println(F("  (no sensors)"));
      continue;
    }

    for (const auto &sensor : sensors) {
      const String &addr16 = sensor.address;
      const String sensor_label = sensor.label;

      String temp_s;
      if (!sensor.has_value || isnan(sensor.temp_c)) {
        temp_s = String("NaN");
      } else {
        temp_s = String(sensor.temp_c, 2);
      }

      uint32_t age_sensor_min = 0U;
      if (sensor.last_ms != 0U && now_ms >= sensor.last_ms) {
        age_sensor_min = (now_ms - sensor.last_ms) / 60000U;
      }

      if (sensor_label.length() > 0) {
        out.printf("  %s \"%s\" : %s%s (age=%lu min)\n", addr16.c_str(),
                   sensor_label.c_str(), temp_s.c_str(),
                   sensor.corrected ? " (corr)" : "",
                   static_cast<unsigned long>(age_sensor_min));
      } else {
        out.printf("  %s : %s%s (age=%lu min)\n", addr16.c_str(),
                   temp_s.c_str(), sensor.corrected ? " (corr)" : "",
                   static_cast<unsigned long>(age_sensor_min));
      }
    }
  }

  GuiRequestRender();
}

static void CmdNode(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // node rm <idHex>
  if (argc >= 3 && argv[1] == "rm") {
    uint32_t id_u32 = 0;
    if (!ParseNodeIdToU32(argv[2], &id_u32)) {
      out.println(F("ERR node rm <idHex>"));
      return;
    }

    if (!RemoveMeshNode(id_u32)) {
      out.printf("node rm 0x%08lX: not found\n",
                 static_cast<unsigned long>(id_u32));
      return;
    }

    // Persist updated topology (if enabled) and refresh UI.
    SaveKnownTopology();
    out.printf("node rm 0x%08lX: removed and topology saved\n",
               static_cast<unsigned long>(id_u32));
    GuiRequestRender();
    return;
  }

  // node <idHex> <label...>
  if (argc < 3) {
    out.println(
        F("ERR node (usage: node <idHex> <label...> | node rm <idHex>)"));
    return;
  }

  // Reconstruct the original line to reuse the common label extractor.
  String line;
  for (int i = 0; i < argc; ++i) {
    if (i)
      line += ' ';
    line += argv[i];
  }

  String label;
  if (!ExtractLabelAfterSecondToken(line, &label)) {
    out.println(F("ERR node (usage: node <idHex> <label...>)"));
    return;
  }

  uint32_t id_u32 = 0;
  if (!ParseNodeIdToU32(argv[1], &id_u32)) {
    out.println(F("ERR node (usage: node <idHex> <label...>)"));
    return;
  }

  MeshNode *node_model = GetOrCreateMeshNode(id_u32);
  if (node_model != nullptr) {
    node_model->set_label(label);
    SaveNodeMetaToNVSIfChanged();
    out.printf("node 0x%08lX label=\"%s\"\n",
               static_cast<unsigned long>(id_u32), label.c_str());
  } else {
    out.printf("node 0x%08lX: FAILED to set label \"%s\"\n",
               static_cast<unsigned long>(id_u32), label.c_str());
  }
  GuiRequestRender();
}

static void CmdNodes(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc >= 2 && argv[1] == "clear") {
    EraseKnownTopology();
    out.println(
        F("nodes clear: erased persisted topology and in-memory nodes"));
    GuiRequestRender();
  } else {
    out.println(F("ERR nodes (use: nodes clear)"));
  }
}

static void CmdName(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  if (argc < 3) {
    out.println(F("ERR name (usage: name <addr16> <label...>)"));
    return;
  }

  String line;
  for (int i = 0; i < argc; ++i) {
    if (i) {
      line += ' ';
    }
    line += argv[i];
  }

  String label;
  if (!ExtractLabelAfterSecondToken(line, &label)) {
    out.println(F("ERR name (usage: name <addr16> <label...>)"));
    return;
  }

  const String addr16 = CanonAddr16(argv[1]);

  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  bool applied = false;
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }
    if (node->SetSensorLabel(addr16, label)) {
      applied = true;
    }
  }

  if (!applied) {
    out.println(F("WARN: no sensors with that address found"));
  }

  SaveLabels();
  out.printf("name %s=\"%s\" (labels saved)\n", addr16.c_str(), label.c_str());
  GuiRequestRender();
}

static void CmdSave(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  (void)argc;
  (void)argv;
  SaveLabels();
  out.println(F("saved"));
}

static void CmdLoad(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  (void)argc;
  (void)argv;

  // Re-apply per-node metadata (tile rank, mute mask) first.
  LoadNodeMetaFromNVS();

  // Then re-apply labels and per-sensor ordering.
  LoadLabels();

  out.println(F("loaded"));
  GuiRequestRender();
}

static void CmdErase(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  (void)argc;
  (void)argv;
  EraseLabels();
  out.println(F("erased"));
  GuiRequestRender();
}

static void CmdStats(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  (void)argc;
  (void)argv;

  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  size_t sensor_count = 0;
  for (uint32_t id : ids) {
    MeshNode *node = FindMeshNode(id);
    if (node != nullptr) {
      sensor_count += node->sensors().size();
    }
  }

  out.printf("nodes=%u sensors=%u\n", static_cast<unsigned>(ids.size()),
             static_cast<unsigned>(sensor_count));

  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const size_t known_bytes = g_root_preferences.getBytesLength("known_bin");
  const size_t labels_bytes =
      g_root_preferences.getString("labels", "").length();
  g_root_preferences.end();

  out.printf("known_bin NVS size=%u bytes\n",
             static_cast<unsigned>(known_bytes));
  out.printf("labels NVS size=%u bytes\n", static_cast<unsigned>(labels_bytes));
}

static void CmdView(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.printf("view mode=%s map_source=%s\n",
               (g_ui_view_mode == kUiViewModeMap) ? "map" : "tiles",
               g_map_use_underbelly ? "underbelly" : "room");
    out.println(F("usage: view tiles | view map [room|belly] | "
                  "view belly on|off"));
    return;
  }

  const String sub = argv[1];

  if (sub.equalsIgnoreCase("tiles")) {
    RoomMapSetViewMode(kUiViewModeTiles);
    out.println(F("view: tiles"));
    return;
  }

  if (sub.equalsIgnoreCase("map")) {
    RoomMapSetViewMode(kUiViewModeMap);
    if (argc >= 3) {
      const String which = argv[2];
      if (which.equalsIgnoreCase("room")) {
        g_map_use_underbelly = false;
      } else if (which.equalsIgnoreCase("belly") ||
                 which.equalsIgnoreCase("underbelly")) {
        g_map_use_underbelly = true;
      }
    }
    g_layout_dirty = true;
    g_values_dirty = true;
    out.printf("view: map (%s)\n",
               g_map_use_underbelly ? "underbelly" : "room");
    return;
  }

  if (sub.equalsIgnoreCase("belly") || sub.equalsIgnoreCase("underbelly")) {
    if (argc < 3) {
      out.printf("view belly=%s\n", g_map_use_underbelly ? "on" : "off");
      out.println(F("usage: view belly on|off"));
      return;
    }
    const String mode = argv[2];
    if (mode.equalsIgnoreCase("on")) {
      g_map_use_underbelly = true;
      out.println(F("view belly=on"));
    } else if (mode.equalsIgnoreCase("off")) {
      g_map_use_underbelly = false;
      out.println(F("view belly=off"));
    } else {
      out.println(F("ERR view belly (use on|off)"));
      return;
    }
    g_layout_dirty = true;
    g_values_dirty = true;
    return;
  }

  out.println(F("ERR view (use: view tiles | view map [room|belly] | "
                "view belly on|off)"));
}

static void CmdDebug(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // Extended debug: "debug nodes" prints per-node age and sequence health.
  if (argc >= 2 && argv[1] == "nodes") {
    const uint32_t now_ms = millis();
    const time_t now_epoch = time(nullptr);

    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    if (ids.empty()) {
      out.println(F("(no nodes)"));
      return;
    }

    out.printf("nodes=%u\n", static_cast<unsigned>(ids.size()));

    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }

      const uint32_t last_update_ms = node->last_update_ms();
      uint32_t since_update_ms = 0U;
      if (last_update_ms != 0U && now_ms >= last_update_ms) {
        since_update_ms = now_ms - last_update_ms;
      }

      const uint32_t age_min = node->ComputeAgeMinutes(now_ms);

      out.printf(
          "node 0x%08lX : age=%lu min (~%lu s since update) "
          "gpio=%d sensors=%u\n",
          static_cast<unsigned long>(id), static_cast<unsigned long>(age_min),
          static_cast<unsigned long>(since_update_ms / 1000U), node->bus_gpio(),
          static_cast<unsigned>(node->sensors().size()));

      // Sequence diagnostics.
      if (!node->has_sequence()) {
        out.println(F("  seq=(none)"));
        continue;
      }

      const uint32_t seq = node->last_sequence();
      const uint32_t last_adv_ms = node->last_sequence_advance_ms();
      const uint32_t last_rx_ms = node->last_sequence_rx_ms();

      uint32_t since_adv_ms = 0U;
      uint32_t since_rx_ms = 0U;

      if (last_adv_ms != 0U && now_ms >= last_adv_ms) {
        since_adv_ms = now_ms - last_adv_ms;
      }
      if (last_rx_ms != 0U && now_ms >= last_rx_ms) {
        since_rx_ms = now_ms - last_rx_ms;
      }

      const uint32_t dup_count = node->duplicate_sequence_rx_count();
      const bool stuck = node->SequenceStuck(now_ms, kSeqStuckMsThreshold);

      out.printf("  seq=%lu last_adv=%lus ago last_rx=%lus ago "
                 "dup_rx=%lu stuck=%s\n",
                 static_cast<unsigned long>(seq),
                 static_cast<unsigned long>(since_adv_ms / 1000U),
                 static_cast<unsigned long>(since_rx_ms / 1000U),
                 static_cast<unsigned long>(dup_count), stuck ? "YES" : "no");
    }
    return;
  }

  // Original behavior: show or toggle global debug flag.
  if (argc < 2) {
    out.printf("debug=%s\n", g_debug_enabled ? "on" : "off");
    return;
  }
  g_debug_enabled = (argv[1] == "on");
  out.printf("debug=%s\n", g_debug_enabled ? "on" : "off");
}

static void CmdUnits(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.println(F("ERR units (use 'units c' or 'units f')"));
    return;
  }
  if (argv[1].equalsIgnoreCase("c")) {
    g_display_fahrenheit = false;
    SaveDisplayUnits();
    out.println(F("units=C"));
    GuiRequestRender();
  } else if (argv[1].equalsIgnoreCase("f")) {
    g_display_fahrenheit = true;
    SaveDisplayUnits();
    out.println(F("units=F"));
    GuiRequestRender();
  } else {
    out.println(F("ERR units (use 'units c' or 'units f')"));
  }
}

static void CmdHighlight(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 3) {
    out.println(F("ERR highlight (use 'highlight missing on|off' or 'highlight "
                  "stale <min>')"));
    return;
  }
  if (argv[1] == "missing") {
    if (argv[2] == "on") {
      g_highlight_missing_nodes = true;
      SaveHighlightSettings();
      out.println(F("highlight missing=on"));
      GuiRequestRender();
    } else if (argv[2] == "off") {
      g_highlight_missing_nodes = false;
      SaveHighlightSettings();
      out.println(F("highlight missing=off"));
      GuiRequestRender();
    } else
      out.println(F("ERR highlight missing (use on|off)"));
  } else if (argv[1] == "stale") {
    const long m = argv[2].toInt();
    if (m > 0) {
      g_stale_minutes_threshold = static_cast<uint32_t>(m);
      SaveHighlightSettings();
      out.printf("highlight stale=%ld min\n", m);
      GuiRequestRender();
    } else
      out.println(F("ERR highlight stale (use positive minutes)"));
  } else {
    out.println(F("ERR highlight (use 'highlight missing on|off' or 'highlight "
                  "stale <min>')"));
  }
}

static void CmdLabels(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 3) {
    out.printf("labels sensor=%s age=%s\n", g_show_sensor_labels ? "on" : "off",
               g_show_age_label ? "on" : "off");
    out.println(F("usage: labels sensor on|off | labels age on|off"));
    return;
  }

  const String target = argv[1];
  const String mode = argv[2];

  bool enable = false;
  if (mode.equalsIgnoreCase("on")) {
    enable = true;
  } else if (mode.equalsIgnoreCase("off")) {
    enable = false;
  } else {
    out.println(F("ERR labels (use on|off)"));
    return;
  }

  if (target.equalsIgnoreCase("sensor") || target.equalsIgnoreCase("sensors")) {
    g_show_sensor_labels = enable;
  } else if (target.equalsIgnoreCase("age")) {
    g_show_age_label = enable;
  } else {
    out.println(
        F("ERR labels (use 'labels sensor on|off' or 'labels age on|off')"));
    return;
  }

  SaveTileDisplaySettings();
  out.printf("labels %s=%s\n", target.c_str(), enable ? "on" : "off");
  GuiRequestRender();
}

static void CmdDummy(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.println(F("ERR dummy (use 'dummy on' or 'dummy off')"));
    return;
  }
  if (argv[1] == "on") {
    g_use_dummy_data = true;
    // Drop any existing runtime nodes and tiles before building dummy set.
    ClearAllMeshNodes();
    MeshTile::DestroyAll();
    BuildDummyData();
    out.println(F("dummy=on"));
    GuiRequestRender();
  } else if (argv[1] == "off") {
    g_use_dummy_data = false;

    // Drop dummy nodes/tiles and restore from any persisted topology.
    ClearAllMeshNodes();
    MeshTile::DestroyAll();
    LoadKnownTopology(); // seeds MeshNode store from NVS if enabled

    out.println(F("dummy=off"));
    GuiRequestRender();

  } else {
    out.println(F("ERR dummy (use 'dummy on' or 'dummy off')"));
  }
}

static void CmdLimits(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  auto ToC = [&](float v) {
    return g_display_fahrenheit ? (v - 32.0f) / 1.8f : v;
  };
  auto FromC = [&](float v) {
    return g_display_fahrenheit ? (v * 1.8f + 32.0f) : v;
  };
  auto DeltaToC = [&](float v) { return g_display_fahrenheit ? (v / 1.8f) : v; };
  auto DeltaFromC = [&](float v) {
    return g_display_fahrenheit ? (v * 1.8f) : v;
  };
  const char *unit = g_display_fahrenheit ? "F" : "C";
  if (argc >= 2 && argv[1] == "show") {
    out.printf("limits warn=%.2f..%.2f %s\n", FromC(g_warn_low_c),
               FromC(g_warn_high_c), unit);
    out.printf("limits alert=%.2f..%.2f %s\n", FromC(g_alert_low_c),
               FromC(g_alert_high_c), unit);
    out.printf("limits hysteresis=%.2f %s\n", DeltaFromC(g_limit_hysteresis_c),
               unit);
    return;
  }
  if (argc >= 4 && (argv[1] == "warn" || argv[1] == "alert")) {
    const float low_in = argv[2].toFloat();
    const float high_in = argv[3].toFloat();
    if (high_in <= low_in) {
      out.println(F("ERR limits (high must be > low)"));
      return;
    }
    if (argv[1] == "warn") {
      g_warn_low_c = ToC(low_in);
      g_warn_high_c = ToC(high_in);
      SaveLimits();
      out.printf("limits warn=%.2f..%.2f %s\n", FromC(g_warn_low_c),
                 FromC(g_warn_high_c), unit);
    } else {
      g_alert_low_c = ToC(low_in);
      g_alert_high_c = ToC(high_in);
      SaveLimits();
      out.printf("limits alert=%.2f..%.2f %s\n", FromC(g_alert_low_c),
                 FromC(g_alert_high_c), unit);
    }
    return;
  }
  if (argc >= 3 && argv[1] == "hyst") {
    const float hyst_in = argv[2].toFloat();
    if (hyst_in < 0.0f) {
      out.println(F("ERR limits (hysteresis must be >= 0)"));
      return;
    }
    g_limit_hysteresis_c = DeltaToC(hyst_in);
    SaveLimits();
    out.printf("limits hysteresis=%.2f %s\n", DeltaFromC(g_limit_hysteresis_c),
               unit);
    return;
  }
  out.println(F("ERR limits (use 'limits show' | 'limits warn <lo> <hi>' | "
                "'limits alert <lo> <hi>' | 'limits hyst <delta>')"));
}

static void CmdTopo(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.println(F("topo cmds:\n"
                  "  topo show\n"
                  "  topo on | off\n"
                  "  topo save       (force persist current RAM set)\n"
                  "  topo load       (pre-seed from NVS into RAM)\n"
                  "  topo clear      (erase persisted + RAM topology)"));
    return;
  }
  const String sub = argv[1];

  if (sub == "show") {
    std::vector<uint32_t> ids;
    (void)ReadKnownFromNVS(&ids);
    out.printf("topo persist=%s, persisted_count=%u\n",
               g_topology_persist_enabled ? "on" : "off",
               static_cast<unsigned>(ids.size()));
    return;
  }

  if (sub == "on" || sub == "off") {
    g_topology_persist_enabled = (sub == "on");
    SaveTopoPersistFlag();
    out.printf("topo persist=%s\n", g_topology_persist_enabled ? "on" : "off");
    return;
  }

  if (sub == "save") {
    // Force one-time save even if persist is off.
    bool prev = g_topology_persist_enabled;
    g_topology_persist_enabled = true;
    SaveKnownTopology();
    g_topology_persist_enabled = prev;
    out.println(F("topo save: requested persist of current RAM topology"));
    return;
  }

  if (sub == "load") {
    // One-time pre-seed regardless of flag (common operational need).
    bool prev = g_topology_persist_enabled;
    g_topology_persist_enabled = true;
    LoadKnownTopology();
    g_topology_persist_enabled = prev;

    // After seeding nodes, apply per-node meta and labels.
    LoadNodeMetaFromNVS();
    LoadLabels();

    GuiRequestRender();
    out.println(
        F("topo load: pre-seeded MeshNode store + meta + labels from NVS"));
    return;
  }

  if (sub == "clear") {
    EraseKnownTopology();
    out.println(F("topo clear: erased persisted topology and in-memory nodes"));
    GuiRequestRender();
    return;
  }

  out.println(F("ERR topo (type just 'topo' for help)"));
}

static bool ParseMsArgument(const String &arg, uint32_t *out_ms, Print &out) {
  if (out_ms == nullptr) {
    return false;
  }

  const char *cstr = arg.c_str();
  char *end = nullptr;
  unsigned long value = strtoul(cstr, &end, 10);
  if (end == cstr) {
    out.printf("ERR: \"%s\" is not a valid integer\n", cstr);
    return false;
  }
  if (value > 3600000UL) { // clamp: 1 hour max
    out.println(F("ERR: value too large (max 3600000 ms)"));
    return false;
  }

  *out_ms = static_cast<uint32_t>(value);
  return true;
}

static void CmdNtfy(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2 || argv[1].equalsIgnoreCase("show")) {
    out.printf("ntfy: enabled=%s summary=%s every=%lu min\n",
               g_ntfy_config.enabled ? "yes" : "no",
               g_ntfy_config.summary_enabled ? "yes" : "no",
               static_cast<unsigned long>(g_ntfy_config.summary_period_min));
    out.println(F("  topics/server configured on bridge"));
    out.printf("  tracked_states=%u\n",
               static_cast<unsigned>(g_ntfy_states.size()));
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
    SaveNtfySettings();
    out.printf("ntfy: enabled=%s (saved)\n", g_ntfy_config.enabled ? "yes" : "no");
    return;
  }

  if (sub.equalsIgnoreCase("summary")) {
    if (argc < 3) {
      out.println(F("ERR ntfy summary (use on|off)"));
      return;
    }
    g_ntfy_config.summary_enabled = argv[2].equalsIgnoreCase("on") ||
                                    argv[2].equalsIgnoreCase("true") ||
                                    argv[2] == "1";
    SaveNtfySettings();
    out.printf("ntfy: summary=%s (saved)\n",
               g_ntfy_config.summary_enabled ? "on" : "off");
    return;
  }

  if (sub.equalsIgnoreCase("freq") || sub.equalsIgnoreCase("minutes")) {
    if (argc < 3) {
      out.println(F("ERR ntfy freq <minutes>"));
      return;
    }
    const long minutes = argv[2].toInt();
    if (minutes <= 0) {
      out.println(F("ERR ntfy freq (minutes must be >0)"));
      return;
    }
    g_ntfy_config.summary_period_min = static_cast<uint32_t>(minutes);
    g_ntfy_last_summary_ms = millis();
    SaveNtfySettings();
    out.printf("ntfy: summary every %ld min (saved)\n", minutes);
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
    SendNtfyRequestToBridge(payload, /*cache_when_offline=*/true,
                            /*is_summary=*/false, "MeshTemps test");
    SendNtfyRequestToBridge(payload, /*cache_when_offline=*/true,
                            /*is_summary=*/true, "MeshTemps test");
    out.println(F("ntfy: test message queued to alert & summary"));
    return;
  }

  out.println(F("ERR ntfy (use show|enable|summary|freq|test [msg])"));
}

static void CmdBuzzer(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // No args or "status" -> show current settings/state.
  if (argc == 1 || (argc >= 2 && argv[1].equalsIgnoreCase("status"))) {
    out.printf("buzzer settings:\n");
    out.printf("  len       : %lu ms\n",
               static_cast<unsigned long>(g_beep_len_ms));
    out.printf("  warn_gap  : %lu ms\n",
               static_cast<unsigned long>(g_beep_gap_warn_ms));
    out.printf("  alert_gap : %lu ms\n",
               static_cast<unsigned long>(g_beep_gap_alert_ms));
    out.printf("  cooldown  : %lu ms\n",
               static_cast<unsigned long>(g_buzzer_cooldown_ms));

    const char *silence_str = "idle";
    switch (g_buzzer_silence_state) {
    case kBuzzerSilenceIdle:
      silence_str = "idle";
      break;
    case kBuzzerSilenceActive:
      silence_str = "active";
      break;
    case kBuzzerSilenceCooldown:
      silence_str = "cooldown";
      break;
    }

    out.printf("buzzer state:\n");
    out.printf("  alerts    : %s (warn=%s)\n",
               g_any_alert ? "YES" : "no",
               g_any_warning ? "YES" : "no");
    out.printf("  silence   : %s\n", silence_str);
    out.printf("  test_mode : %s (%s pattern)\n",
               g_buzzer_test_active ? "ON" : "off",
               g_buzzer_test_use_alert_pattern ? "alert" : "warn");
    return;
  }

  const String sub = argv[1];

  // -------------------------------------------------------------------------
  // Set beep length:  buzzer len <ms>
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("len") || sub.equalsIgnoreCase("length")) {
    if (argc < 3) {
      out.println(F("ERR: usage: buzzer len <ms>"));
      return;
    }
    uint32_t ms = 0;
    if (!ParseMsArgument(argv[2], &ms, out)) {
      return;
    }
    g_beep_len_ms = ms;
    SaveBuzzerSettings();
    out.printf("buzzer len set to %lu ms\n", static_cast<unsigned long>(ms));
    return;
  }

  // -------------------------------------------------------------------------
  // Set warning gap:  buzzer warn <ms>
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("warn") || sub.equalsIgnoreCase("warn_gap")) {
    if (argc < 3) {
      out.println(F("ERR: usage: buzzer warn <ms>"));
      return;
    }
    uint32_t ms = 0;
    if (!ParseMsArgument(argv[2], &ms, out)) {
      return;
    }
    g_beep_gap_warn_ms = ms;
    SaveBuzzerSettings();
    out.printf("buzzer warn_gap set to %lu ms\n",
               static_cast<unsigned long>(ms));
    return;
  }

  // -------------------------------------------------------------------------
  // Set alert gap:  buzzer alert <ms>
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("alert") || sub.equalsIgnoreCase("alert_gap")) {
    if (argc < 3) {
      out.println(F("ERR: usage: buzzer alert <ms>"));
      return;
    }
    uint32_t ms = 0;
    if (!ParseMsArgument(argv[2], &ms, out)) {
      return;
    }
    g_beep_gap_alert_ms = ms;
    SaveBuzzerSettings();
    out.printf("buzzer alert_gap set to %lu ms\n",
               static_cast<unsigned long>(ms));
    return;
  }

  // -------------------------------------------------------------------------
  // Set cooldown:  buzzer cooldown <ms>
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("cooldown") || sub.equalsIgnoreCase("cd")) {
    if (argc < 3) {
      out.println(F("ERR: usage: buzzer cooldown <ms>"));
      return;
    }
    uint32_t ms = 0;
    if (!ParseMsArgument(argv[2], &ms, out)) {
      return;
    }
    // Allow 0 to mean "no extra cooldown".
    g_buzzer_cooldown_ms = ms;
    SaveBuzzerSettings();
    out.printf("buzzer cooldown set to %lu ms\n",
               static_cast<unsigned long>(ms));
    return;
  }

  // -------------------------------------------------------------------------
  // Test mode:  buzzer test [alert|warn|off]
  //   - uses beep_len_ms and the configured gaps
  //   - runs until "buzzer test off" or "buzzer stop"
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("test")) {
    if (argc == 2) {
      // Default: alert pattern test.
      BuzzerStartTestInternal(/*use_alert_profile=*/true);
      out.println(F("buzzer test: ON (alert pattern)"));
      return;
    }

    const String mode = argv[2];
    if (mode.equalsIgnoreCase("off") || mode.equalsIgnoreCase("stop")) {
      BuzzerStopTestInternal();
      out.println(F("buzzer test: OFF"));
      return;
    }

    if (mode.equalsIgnoreCase("alert") || mode.equalsIgnoreCase("high")) {
      BuzzerStartTestInternal(/*use_alert_profile=*/true);
      out.println(F("buzzer test: ON (alert pattern)"));
      return;
    }

    if (mode.equalsIgnoreCase("warn") || mode.equalsIgnoreCase("low")) {
      BuzzerStartTestInternal(/*use_alert_profile=*/false);
      out.println(F("buzzer test: ON (warn pattern)"));
      return;
    }

    out.println(
        F("ERR: usage: buzzer test [alert|warn|off] (default is alert)"));
    return;
  }

  // -------------------------------------------------------------------------
  // Silence:  buzzer silence
  //   Same semantics as pressing the on-screen Silence button:
  //   - only works if a real alert/warning is active
  //   - starts a silence/cooldown episode
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("silence")) {
    if (g_buzzer_silence_state != kBuzzerSilenceIdle) {
      out.println(F("buzzer: already silenced / in cooldown"));
      return;
    }
    if (!g_any_alert && !g_any_warning) {
      out.println(F("buzzer: no active alerts/warnings to silence"));
      return;
    }

    g_buzzer_silence_state = kBuzzerSilenceActive;
    g_buzzer_silence_rearm_at_ms = 0;

    digitalWrite(kBuzzerPin, LOW);
    g_buzzer_on = false;
    g_buzzer_beep_start_ms = 0;
    g_buzzer_last_beep_ms = 0;

    out.println(
        F("buzzer: silence ACTIVE (will auto re-arm after alerts clear + "
          "cooldown)"));
    return;
  }

  // -------------------------------------------------------------------------
  // Hard stop:  buzzer stop
  //   - turn off test mode (if any)
  //   - force buzzer output low and clear timing
  //   (does NOT alter silence/cooldown state)
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("stop")) {
    BuzzerStopTestInternal();
    digitalWrite(kBuzzerPin, LOW);
    g_buzzer_on = false;
    g_buzzer_beep_start_ms = 0;
    g_buzzer_last_beep_ms = 0;

    out.println(F("buzzer: STOP (test off, output low)"));
    return;
  }

  // -------------------------------------------------------------------------
  // Fallback: print usage
  // -------------------------------------------------------------------------
  out.println(F("buzzer usage:"));
  out.println(F("  buzzer                     - show settings/state"));
  out.println(F("  buzzer len <ms>           - set beep length"));
  out.println(F("  buzzer warn <ms>          - set warning gap"));
  out.println(F("  buzzer alert <ms>         - set alert gap"));
  out.println(F("  buzzer cooldown <ms>      - set cooldown between episodes"));
  out.println(F("  buzzer test [alert|warn|off]"));
  out.println(F("                            - start/stop buzzer test pattern"));
  out.println(F("  buzzer silence            - same as GUI Silence button"));
  out.println(F("  buzzer stop               - stop test and force buzzer off"));
}

static void CmdFlash(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 2) {
    out.println(F("ERR flash (use 'flash <interval_ms>' or 'flash off')"));
    return;
  }

  if (argv[1].equalsIgnoreCase("off")) {
    g_flash_interval_ms = 0; // turn off
    SaveFlashSettings();
    g_values_dirty = true; // propagate to tiles
    out.println(F("flash off"));
    return;
  }

  long interval_ms = argv[1].toInt();
  if (interval_ms < 0) {
    out.println(F("ERR flash (interval must be >=0, or 'off')"));
    return;
  }

  g_flash_interval_ms =
      static_cast<uint32_t>(interval_ms); // enable with interval
  SaveFlashSettings();
  g_values_dirty =
      true; // next refresh will update all tiles' per-tile interval

  out.printf("flash interval=%lu ms (%s)\n",
             static_cast<unsigned long>(g_flash_interval_ms),
             g_flash_interval_ms ? "enabled" : "off");
}

static void CmdNorder(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // norder list
  if (argc >= 2 && argv[1] == "list") {
    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    if (ids.empty()) {
      out.println(F("norder list: (no nodes)"));
      return;
    }

    out.printf("norder list: %u node(s)\n", static_cast<unsigned>(ids.size()));

    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }
      char key[9];
      FormatNodeKey(id, key, sizeof(key));
      out.printf("%s : %ld\n", key, static_cast<long>(node->tile_rank()));
    }
    out.println(F("norder list: done"));
    return;
  }

  // norder clear <nodeIdHex>  (reset to "unspecified")
  if (argc >= 3 && argv[1] == "clear") {
    uint32_t id_u32 = 0;
    if (!ParseNodeIdToU32(argv[2], &id_u32)) {
      out.println(F("ERR norder clear <nodeIdHex>"));
      return;
    }
    MeshNode *node = FindMeshNode(id_u32);
    if (node != nullptr) {
      node->set_tile_rank(std::numeric_limits<int32_t>::max());
      SaveNodeMetaToNVSIfChanged();
    }
    out.printf("norder clear %s: rank reset to default\n", argv[2].c_str());
    GuiRequestRender();
    return;
  }

  // norder <nodeIdHex> <rank>
  if (argc >= 3) {
    uint32_t id_u32 = 0;
    if (!ParseNodeIdToU32(argv[1], &id_u32)) {
      out.println(F("ERR norder <nodeIdHex> <rank>"));
      return;
    }

    const int32_t rank = argv[2].toInt();
    MeshNode *node = GetOrCreateMeshNode(id_u32);
    if (node != nullptr) {
      node->set_tile_rank(rank);
      char key[9];
      FormatNodeKey(id_u32, key, sizeof(key));
      out.printf("norder %s -> %ld\n", key, static_cast<long>(rank));
      SaveNodeMetaToNVSIfChanged();
    } else {
      out.printf("norder %s: FAILED to set rank\n", argv[1].c_str());
    }
    GuiRequestRender();
    return;
  }

  out.println(F("ERR norder (use 'norder <nodeIdHex> <rank>' | "
                "'norder clear <nodeIdHex>' | 'norder list')"));
}

static void CmdOrder(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // order list
  if (argc >= 2 && argv[1] == "list") {
    // Build a map of addr16 -> global_rank from the current MeshNode store.
    std::map<String, int32_t> rank_by_addr;

    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }

      const std::vector<MeshNode::Sensor> &sensors = node->sensors();
      for (const auto &sensor : sensors) {
        if (sensor.global_rank == std::numeric_limits<int32_t>::max()) {
          continue; // "unspecified"
        }
        const String addr16 = CanonAddr16(sensor.address);

        auto it = rank_by_addr.find(addr16);
        if (it == rank_by_addr.end()) {
          rank_by_addr.emplace(addr16, sensor.global_rank);
        } else {
          // If multiple nodes disagree, keep the lowest rank.
          if (sensor.global_rank < it->second) {
            it->second = sensor.global_rank;
          }
        }
      }
    }

    if (rank_by_addr.empty()) {
      out.println(F("order list: (empty)"));
      return;
    }

    // Sort by rank then address for stable, predictable output.
    std::vector<std::pair<String, int32_t>> entries;
    entries.reserve(rank_by_addr.size());
    for (const auto &kv : rank_by_addr) {
      entries.push_back(kv);
    }

    std::sort(entries.begin(), entries.end(),
              [](const std::pair<String, int32_t> &a,
                 const std::pair<String, int32_t> &b) {
                if (a.second != b.second) {
                  return a.second < b.second;
                }
                return a.first < b.first;
              });

    out.printf("order list: %u unique address(es)\n",
               static_cast<unsigned>(entries.size()));
    for (const auto &e : entries) {
      out.printf("%s : %ld\n", e.first.c_str(), static_cast<long>(e.second));
    }
    out.println(F("order list: done"));
    return;
  }

  // order clear <addr16>
  if (argc >= 3 && argv[1] == "clear") {
    const String key = CanonAddr16(argv[2]);

    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }
      (void)node->SetSensorGlobalRank(key, std::numeric_limits<int32_t>::max());
    }

    SaveLabels();
    out.printf("order clear %s: global rank reset\n", key.c_str());
    GuiRequestRender();
    return;
  }

  // order <addr16> <rank>
  if (argc >= 3) {
    const String key = CanonAddr16(argv[1]);
    const int32_t rank = argv[2].toInt();

    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    bool applied = false;
    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }
      if (node->SetSensorGlobalRank(key, rank)) {
        applied = true;
      }
    }

    if (!applied) {
      out.println(F("WARN: no sensors with that address found"));
    }

    SaveLabels();
    out.printf("order %s -> %ld (labels saved)\n", key.c_str(),
               static_cast<long>(rank));
    GuiRequestRender();
    return;
  }

  out.println(F("ERR order (use 'order <addr16> <rank>' | "
                "'order clear <addr16>' | 'order list')"));
}

static void CmdSorder(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // sorder list [nodeIdHex]
  if (argc >= 2 && argv[1] == "list") {
    // sorder list <nodeIdHex>
    if (argc >= 3) {
      uint32_t id_u32 = 0;
      if (!ParseNodeIdToU32(argv[2], &id_u32)) {
        out.println(F("ERR sorder list <nodeIdHex>"));
        return;
      }

      MeshNode *node = FindMeshNode(id_u32);
      if (node == nullptr) {
        out.printf("sorder list %s: (node not found)\n", argv[2].c_str());
        return;
      }

      const std::vector<MeshNode::Sensor> &sensors = node->sensors();
      std::vector<std::pair<String, int32_t>> entries;
      entries.reserve(sensors.size());

      for (const auto &sensor : sensors) {
        if (sensor.node_rank == std::numeric_limits<int32_t>::max()) {
          continue; // "unspecified"
        }
        entries.emplace_back(CanonAddr16(sensor.address), sensor.node_rank);
      }

      if (entries.empty()) {
        out.printf("sorder list %s: (no per-node ranks)\n", argv[2].c_str());
        return;
      }

      std::sort(entries.begin(), entries.end(),
                [](const std::pair<String, int32_t> &a,
                   const std::pair<String, int32_t> &b) {
                  if (a.second != b.second) {
                    return a.second < b.second;
                  }
                  return a.first < b.first;
                });

      out.printf("sorder list %s:\n", argv[2].c_str());
      for (const auto &e : entries) {
        out.printf("%s : %ld\n", e.first.c_str(), static_cast<long>(e.second));
      }
      out.printf("sorder list %s: done\n", argv[2].c_str());
      return;
    }

    // sorder list (all nodes)
    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    if (ids.empty()) {
      out.println(F("sorder list: (no nodes)"));
      return;
    }

    for (uint32_t id : ids) {
      MeshNode *node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }

      char node_key[9];
      FormatNodeKey(id, node_key, sizeof(node_key));
      out.printf("[%s]\n", node_key);

      const std::vector<MeshNode::Sensor> &sensors = node->sensors();
      std::vector<std::pair<String, int32_t>> entries;
      entries.reserve(sensors.size());

      for (const auto &sensor : sensors) {
        if (sensor.node_rank == std::numeric_limits<int32_t>::max()) {
          continue;
        }
        entries.emplace_back(CanonAddr16(sensor.address), sensor.node_rank);
      }

      if (entries.empty()) {
        out.println(F("  (empty)"));
      } else {
        std::sort(entries.begin(), entries.end(),
                  [](const std::pair<String, int32_t> &a,
                     const std::pair<String, int32_t> &b) {
                    if (a.second != b.second) {
                      return a.second < b.second;
                    }
                    return a.first < b.first;
                  });

        for (const auto &e : entries) {
          out.printf("  %s : %ld\n", e.first.c_str(),
                     static_cast<long>(e.second));
        }
      }
    }

    out.println(F("sorder list: done"));
    return;
  }

  // sorder clear <nodeIdHex> <addr16>
  if (argc >= 4 && argv[1] == "clear") {
    uint32_t id_u32 = 0;
    if (!ParseNodeIdToU32(argv[2], &id_u32)) {
      out.println(F("ERR sorder clear <nodeIdHex> <addr16>"));
      return;
    }

    const String key = CanonAddr16(argv[3]);
    MeshNode *node = FindMeshNode(id_u32);
    if (node != nullptr) {
      (void)node->SetSensorNodeRank(key, std::numeric_limits<int32_t>::max());
    }

    SaveLabels();
    out.printf("sorder clear %s %s: rank reset\n", argv[2].c_str(),
               key.c_str());
    GuiRequestRender();
    return;
  }

  // sorder <nodeIdHex> <addr16> <rank>
  if (argc >= 4) {
    uint32_t id_u32 = 0;
    if (!ParseNodeIdToU32(argv[1], &id_u32)) {
      out.println(F("ERR sorder <nodeIdHex> <addr16> <rank>"));
      return;
    }

    const String key = CanonAddr16(argv[2]);
    const int32_t rank = argv[3].toInt();

    MeshNode *node = FindMeshNode(id_u32);
    if (node != nullptr) {
      (void)node->SetSensorNodeRank(key, rank);
    }

    SaveLabels();
    out.printf("sorder %s %s -> %ld\n", argv[1].c_str(), key.c_str(),
               static_cast<long>(rank));
    GuiRequestRender();
    return;
  }

  out.println(
      F("ERR sorder (use 'sorder <nodeIdHex> <addr16> <rank>' | "
        "'sorder clear <nodeIdHex> <addr16>' | 'sorder list [nodeIdHex]')"));
}

static void CmdWifi(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // Rebuild the original command line (tokens joined by single spaces).
  String line;
  for (int i = 0; i < argc; ++i) {
    if (i > 0) {
      line += ' ';
    }
    line += argv[i];
  }

  if (argc < 2) {
    out.println(
        F("usage: wifi status | wifi scan | wifi connect | wifi ssid <value...> | "
          "wifi password <value...> | wifi clear"));
    return;
  }

  const String sub = argv[1];

  if (sub.equalsIgnoreCase("status")) {
    out.printf("ssid=\"%s\"\n", g_network_config.ssid.c_str());
    out.printf("tz_minutes=%ld dst=%s\n",
               static_cast<long>(g_network_config.timezone_minutes),
               g_network_config.dst_enabled ? "on" : "off");

    const wl_status_t st = WiFi.status();
    const char *st_str = "UNKNOWN";
    switch (st) {
      case WL_IDLE_STATUS:
        st_str = "IDLE";
        break;
      case WL_NO_SSID_AVAIL:
        st_str = "NO_SSID";
        break;
      case WL_SCAN_COMPLETED:
        st_str = "SCAN_DONE";
        break;
      case WL_CONNECTED:
        st_str = "CONNECTED";
        break;
      case WL_CONNECT_FAILED:
        st_str = "CONNECT_FAILED";
        break;
      case WL_CONNECTION_LOST:
        st_str = "CONNECTION_LOST";
        break;
      case WL_DISCONNECTED:
        st_str = "DISCONNECTED";
        break;
      default:
        st_str = "UNKNOWN";
        break;
    }
    out.printf("wifi_status=%s (%d)\n", st_str, static_cast<int>(st));

    // Always print the physical channel the radio is on.
    const int32_t phy_channel = WiFi.channel();
    if (phy_channel > 0) {
      out.printf("radio_channel=%d\n", static_cast<int>(phy_channel));
    }

    // STA-specific info only if actually connected to an infrastructure AP.
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
    out.println(F("[WiFi] Scanning for networks..."));

    // synchronous scan, include hidden networks
    const int16_t network_count =
        WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);

    if (network_count < 0) {
      out.printf("WiFi scan failed (err=%d)\n",
                 static_cast<int>(network_count));
      return;
    }

    out.printf("Found %d network(s)\n", static_cast<int>(network_count));
    if (network_count == 0) {
      WiFi.scanDelete();
      return;
    }

    out.println(
        F("Idx  RSSI  Chan  Auth                 BSSID              SSID"));
    for (int i = 0; i < network_count; ++i) {
      const int32_t rssi = WiFi.RSSI(i);
      const int32_t channel = WiFi.channel(i);

      const wifi_auth_mode_t auth_mode =
          static_cast<wifi_auth_mode_t>(WiFi.encryptionType(i));
      const char *auth_str = WifiAuthModeToString(auth_mode);

      const String bssid = WiFi.BSSIDstr(i);
      const String ssid = WiFi.SSID(i);

      out.printf("%3d  %4d  %4d  %-20s  %17s  %s\n", i,
                 static_cast<int>(rssi),
                 static_cast<int>(channel),
                 auth_str,
                 bssid.c_str(),
                 ssid.c_str());
    }

    WiFi.scanDelete();  // free scan results
    return;
  }

  if (sub.equalsIgnoreCase("ssid")) {
    String new_ssid;
    if (!ExtractRawTailAfterSecondToken(line, &new_ssid)) {
      out.println(F("ERR wifi ssid (use: wifi ssid <value...>)"));
      return;
    }

    DumpStringHex("[CMD] SSID (before save)", g_network_config.ssid);
    g_network_config.ssid = new_ssid;
    DumpStringHex("[CMD] SSID (after  save)", g_network_config.ssid);

    SaveNetworkConfigToNVS();

    // Force re-apply so the next Wi-Fi connection uses the new SSID.
    (void)EnsureMeshStationConfigApplied(out, /*force_reapply=*/true);
    out.printf(
        "wifi ssid set to \"%s\"\n"
        "[WiFi] STA credentials updated; root mesh node will connect to the "
        "AP in the background when possible.\n",
        g_network_config.ssid.c_str());

    return;
  }

  if (sub.equalsIgnoreCase("password") || sub.equalsIgnoreCase("pwd")) {
    String new_password;
    if (!ExtractRawTailAfterSecondToken(line, &new_password)) {
      out.println(F("ERR wifi password (use: wifi password <value...>)"));
      return;
    }

    // Existing debug
    // DumpStringHex("[CMD] PASSWORD (before save)", g_network_config.password);

    g_network_config.password = new_password;

    // Optional verification of what we are about to store.
    // DumpStringHex("[CMD] PASSWORD (after  save)", g_network_config.password);

    SaveNetworkConfigToNVS();
    (void)EnsureMeshStationConfigApplied(out, /*force_reapply=*/true);

    // (Second SaveNetworkConfigToNVS() call was redundant; removed.)

    out.println(
        F("wifi password set (value not echoed)\n"
          "[WiFi] STA credentials updated; root mesh node will reconnect in "
          "the background when possible."));
    return;
  }

  if (sub.equalsIgnoreCase("connect")) {
    // With the new non-blocking EnsureWifiConnected(), this no longer waits;
    // it reports current status and ensures credentials are applied.
    const bool connected_now = EnsureWifiConnected(out);
    if (connected_now) {
      out.println(
          F("wifi connect: already connected; attempting immediate NTP sync."));
      (void)SyncTimeFromNtp(out);
    } else {
      out.println(
          F("wifi connect: STA credentials applied; painlessMesh will bring "
            "the STA link up in the background when possible. Use "
            "'wifi status' later to check the connection and local time."));
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

  out.println(
      F("ERR wifi (use: wifi status|scan|connect|ssid|password|clear)"));
}

// Helper: find the first sensor with a given 16-char address across all nodes.
static bool FindSensorByAddressAcrossNodes(const String &address16,
                                           MeshNode **out_node,
                                           MeshNode::Sensor **out_sensor) {
  if (out_node != nullptr) {
    *out_node = nullptr;
  }
  if (out_sensor != nullptr) {
    *out_sensor = nullptr;
  }

  auto node_ids = GetAllMeshNodeIds();
  for (size_t i = 0; i < node_ids.size(); ++i) {
    const uint32_t node_id = node_ids[i];
    MeshNode *node = FindMeshNode(node_id);
    if (node == nullptr) {
      continue;
    }

    MeshNode::Sensor *sensor = node->FindSensor(address16);
    if (sensor != nullptr) {
      if (out_node != nullptr) {
        *out_node = node;
      }
      if (out_sensor != nullptr) {
        *out_sensor = sensor;
      }
      return true;
    }
  }

  return false;
}

// ============================================================================
// History configuration + data: serial console command
// ============================================================================

static void CmdHist(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // -------------------------------------------------------------------------
  // Usage
  // -------------------------------------------------------------------------
  if (argc <= 1) {
    out.println(F("usage:"));
    out.println(F("  hist show|get"));
    out.println(F("    - show global history logging configuration"));
    out.println(F("  hist set <interval_s> <retention_days>"));
    out.println(F("    - set history interval (seconds) and retention (days)"));
    out.println(F("  hist clear"));
    out.println(F("    - clear history for all nodes / sensors"));
    out.println(F("  hist clear <addr16>"));
    out.println(
        F("    - clear history for a specific sensor (16-char DS18B20 addr)"));
    out.println(F("  hist view <addr16> [max_samples]"));
    out.println(
        F("    - view history for a specific sensor, oldest -> newest"));
    return;
  }

  const String sub = argv[1];

  // -------------------------------------------------------------------------
  // hist show / hist get -> show global history config
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("show") || sub.equalsIgnoreCase("get")) {
    PrintHistoryConfig(out);
    return;
  }

  // -------------------------------------------------------------------------
  // hist set <interval_s> <retention_days>
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("set")) {
    if (argc < 4) {
      out.println(
          F("ERR hist set (usage: hist set <interval_s> <retention_days>)"));
      return;
    }

    const long interval_seconds = argv[2].toInt();
    const long retention_days = argv[3].toInt();

    if (interval_seconds <= 0) {
      out.println(F("ERR hist set: interval_s must be > 0"));
      return;
    }
    if (retention_days <= 0) {
      out.println(F("ERR hist set: retention_days must be > 0"));
      return;
    }

    const uint32_t interval_ms =
        static_cast<uint32_t>(interval_seconds) * 1000UL;

    MeshNode::SetHistoryConfig(interval_ms,
                               static_cast<uint32_t>(retention_days));

    out.print(F("hist set: interval="));
    out.print(interval_ms);
    out.print(F(" ms, retention="));
    out.print(retention_days);
    out.println(F(" days"));

    PrintHistoryConfig(out);
    return;
  }

  // -------------------------------------------------------------------------
  // hist clear [addr16]
  //   - no addr: clear history for all sensors on all nodes
  //   - addr16 : clear history for matching sensor across all nodes
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("clear")) {
    // No additional argument: clear all sensors.
    if (argc == 2) {
      auto node_ids = GetAllMeshNodeIds();
      size_t cleared_sensors = 0;

      for (size_t i = 0; i < node_ids.size(); ++i) {
        MeshNode *node = FindMeshNode(node_ids[i]);
        if (node == nullptr) {
          continue;
        }

        auto &sensors = node->sensors();
        for (size_t s = 0; s < sensors.size(); ++s) {
          MeshNode::Sensor &sensor = sensors[s];
          sensor.history.clear();
          sensor.history_head_index = 0U;
          sensor.history_size = 0U;
          ++cleared_sensors;
        }
      }

      out.print(F("hist clear: cleared history for "));
      out.print(static_cast<unsigned long>(cleared_sensors));
      out.println(F(" sensors across all nodes"));
      return;
    }

    // hist clear <addr16>
    if (argc == 3) {
      const String addr16 = argv[2];
      if (addr16.length() != 16) {
        out.println(
            F("ERR hist clear: addr16 must be a 16-char hex DS18B20 address"));
        return;
      }

      MeshNode *node = nullptr;
      MeshNode::Sensor *sensor = nullptr;
      if (!FindSensorByAddressAcrossNodes(addr16, &node, &sensor) ||
          node == nullptr || sensor == nullptr) {
        out.print(F("ERR hist clear: sensor with addr="));
        out.print(addr16);
        out.println(F(" not found on any node"));
        return;
      }

      sensor->history.clear();
      sensor->history_head_index = 0U;
      sensor->history_size = 0U;

      out.print(F("hist clear: cleared history for node "));
      out.print(node->node_id_str());
      out.print(F(" sensor "));
      out.println(addr16);
      return;
    }

    out.println(F("ERR hist clear (usage: hist clear [addr16])"));
    return;
  }

  // -------------------------------------------------------------------------
  // hist view <addr16> [max_samples]
  //   - show up to max_samples (default 50) samples, oldest -> newest.
  // -------------------------------------------------------------------------
  if (sub.equalsIgnoreCase("view")) {
    if (argc < 3) {
      out.println(F("ERR hist view (usage: hist view <addr16> [max_samples])"));
      return;
    }

    const String addr16 = argv[2];
    if (addr16.length() != 16) {
      out.println(
          F("ERR hist view: addr16 must be a 16-char hex DS18B20 address"));
      return;
    }

    long max_samples_arg = 50;
    if (argc >= 4) {
      max_samples_arg = argv[3].toInt();
      if (max_samples_arg <= 0) {
        max_samples_arg = 50;
      }
    }
    const size_t max_samples = static_cast<size_t>(max_samples_arg);

    MeshNode *node = nullptr;
    MeshNode::Sensor *sensor = nullptr;
    if (!FindSensorByAddressAcrossNodes(addr16, &node, &sensor) ||
        node == nullptr || sensor == nullptr) {
      out.print(F("ERR hist view: sensor with addr="));
      out.print(addr16);
      out.println(F(" not found on any node"));
      return;
    }

    std::vector<MeshNode::SensorHistorySample> samples;
    sensor->CopyHistoryInChronologicalOrder(&samples);

    if (samples.empty()) {
      out.print(F("hist view: no history samples for node "));
      out.print(node->node_id_str());
      out.print(F(" sensor "));
      out.println(addr16);
      return;
    }

    const uint32_t now_ms = millis();
    const time_t now_epoch = time(nullptr);
    const size_t total = samples.size();
    const size_t start_index =
        (total > max_samples) ? (total - max_samples) : 0U;
    const size_t shown = total - start_index;

    out.print(F("History for node "));
    out.print(node->node_id_str());
    out.print(F(" sensor "));
    out.print(addr16);
    out.print(F(" ("));
    out.print(static_cast<unsigned long>(total));
    out.print(F(" samples total, showing last "));
    out.print(static_cast<unsigned long>(shown));
    out.println(F("):"));

    for (size_t i = start_index; i < total; ++i) {
      const MeshNode::SensorHistorySample &sample = samples[i];
      const uint32_t sample_ts = sample.timestamp_ms;
      uint32_t age_s = 0U;

      if (sample.has_epoch && now_epoch > 0) {
        const time_t sample_epoch = static_cast<time_t>(sample_ts);
        age_s = (now_epoch >= sample_epoch)
                    ? static_cast<uint32_t>(now_epoch - sample_epoch)
                    : 0U;
      } else if (!sample.has_epoch && now_ms >= sample_ts) {
        age_s = (now_ms - sample_ts) / 1000U;
      }

      out.print(F("  #"));
      out.print(static_cast<unsigned long>(i));
      if (sample.has_epoch) {
        out.print(F(": t_epoch="));
      } else {
        out.print(F(": t_ms="));
      }
      out.print(static_cast<unsigned long>(sample_ts));
      out.print(F(" (age="));
      out.print(static_cast<unsigned long>(age_s));
      out.print(F(" s)"));

      if (sample.has_epoch) {
        struct tm tm_buf;
        char time_buf[32];
        if (localtime_r(&sample.timestamp_epoch, &tm_buf) != nullptr &&
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                     &tm_buf) != 0) {
          out.print(F(" @ "));
          out.print(time_buf);
        } else {
          out.print(F(" @ epoch="));
          out.print(static_cast<long>(sample.timestamp_epoch));
        }
      }

      out.print(F(" temp="));
      out.print(sample.temp_c, 3);
      out.print(F(" C"));

      if (!sample.has_value) {
        out.print(F(" [invalid]"));
      }
      if (sample.corrected) {
        out.print(F(" [corrected]"));
      }
      out.println();
    }

    return;
  }

  // -------------------------------------------------------------------------
  // Unknown subcommand
  // -------------------------------------------------------------------------
  out.println(F(
      "ERR hist (use 'hist show', 'hist set', 'hist clear', or 'hist view')"));
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

// ============================================================================
// History configuration: serial console command
// ============================================================================

static void PrintHistoryConfig(Print &output) {
  const uint32_t interval_ms = MeshNode::history_interval_ms();
  const uint32_t retention_days = MeshNode::history_retention_days();
  const size_t capacity = MeshNode::history_capacity_per_sensor();

  const float interval_seconds = static_cast<float>(interval_ms) / 1000.0f;
  const float interval_minutes = interval_seconds / 60.0f;

  output.println(F("History logging configuration:"));
  output.print(F("  Interval: "));
  output.print(interval_ms);
  output.print(F(" ms ("));
  output.print(interval_seconds, 3);
  output.print(F(" s, "));
  output.print(interval_minutes, 3);
  output.println(F(" min)"));

  output.print(F("  Retention: "));
  output.print(retention_days);
  output.println(F(" days"));

  output.print(F("  Per-sensor capacity: "));
  output.print(static_cast<unsigned long>(capacity));
  output.println(F(" samples"));
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

// Respond to a leaf's root_probe with a unicast root_ack.
// This lets the leaf confirm "this channel actually has the real root".
static void HandleRootProbe(uint32_t from, const JsonDocument &probe_doc) {
  const uint32_t now_ms = millis();

  // Leaf may include its view of nodeId; default to 'from' if missing.
  const uint32_t leaf_node_id = probe_doc["nodeId"] | from;

  DLOG("[ROOT PROBE] from=0x%08lX leafNodeId=0x%08lX\n",
       static_cast<unsigned long>(from),
       static_cast<unsigned long>(leaf_node_id));

  JsonDocument reply;
  reply["type"] = "root_ack";
  reply["rootId"] = mesh.getNodeId();
  reply["uptimeMs"] = static_cast<uint32_t>(now_ms);
  reply["toNodeId"] = leaf_node_id;  // for debugging on the leaf side

  String out;
  serializeJson(reply, out);

  DLOG("[ROOT TX ack] to=0x%08lX: %s\n",
       static_cast<unsigned long>(from),
       out.c_str());

  // Unicast back to the probing leaf.
  mesh.sendSingle(from, out);
}

// -----------------------------------------------------------------------------
// Mesh callbacks (root)
// -----------------------------------------------------------------------------

void OnReceiveRoot(uint32_t from, String &msg) {
  bool structure_changed = false;

  DLOG("[ROOT RX] from=0x%08lX len=%u: %s\n",
       static_cast<unsigned long>(from),
       static_cast<unsigned>(msg.length()),
       msg.c_str());

  if (g_use_dummy_data) {
    // In dummy mode, ignore real traffic (including probes).
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok) {
    DLOG("  ! JSON parse error\n");
    return;
  }

  const char *type = doc["type"] | "temps";

  // --- NEW: handle root_probe first ----------------------------------------
  if (strcmp(type, "root_probe") == 0) {
    HandleRootProbe(from, doc);
    return;
  }
  // -------------------------------------------------------------------------

  if (strcmp(type, "temps") != 0) {
    DLOG("  ! ignoring type '%s'\n", type);
    return;
  }

  const uint32_t now_ms = millis();

  // Determine the nodeId the leaf claims *before* we update the model,
  // and record how many sensors we had so we can detect newly created ones.
  const uint32_t payload_node_id = doc["nodeId"] | from;
  size_t prev_sensor_count = 0;
  const bool node_existed = (FindMeshNode(payload_node_id) != nullptr);
  if (MeshNode *existing = FindMeshNode(payload_node_id)) {
    prev_sensor_count = existing->sensors().size();
  }

  MeshNode *node_model = UpdateMeshNodeFromTempsJson(doc, from, now_ms);
  if (node_model == nullptr) {
    DLOG("  ! UpdateMeshNodeFromTempsJson failed\n");
    return;
  }

  const uint32_t node_id = node_model->node_id();
  const int bus_gpio = node_model->bus_gpio();

  // If this temps frame caused new sensors to exist on this node
  // (first-ever temps, or additional sensors), re-apply persisted labels
  // so those sensors immediately pick up any saved names/order.
  const size_t new_sensor_count = node_model->sensors().size();
  if (new_sensor_count > prev_sensor_count) {
    LoadLabels();
  }

  DLOG("  nodeId=0x%08lX busGpio=%d sensors=%u\n",
       static_cast<unsigned long>(node_id),
       bus_gpio,
       static_cast<unsigned>(new_sensor_count));

  // Apply any persisted per-node metadata (rank, mute, etc.) as soon as a
  // brand-new node appears via the bridge so settings like mute survive GUI
  // restarts even though we don't run the mesh stack here.
  if (!node_existed) {
    LoadNodeMetaFromNVS();
  }

  // Topology persistence: add/update known IDs.
  if (g_topology_persist_enabled) {
    structure_changed |= AddKnownAndPersist(node_id);
  }

  GuiUpdateNodeSummary(node_model->node_id_str().c_str(), bus_gpio, now_ms);
  GuiUpdateNetwork(GetAllMeshNodeIds().size());

  // Layout vs values: new node means layout; otherwise just value refresh.
  if (structure_changed) {
    g_layout_dirty = true; // new node(s)
  } else {
    g_values_dirty = true; // existing tiles, changed values/labels
  }
}

void OnConnectionsChangedRoot() {
  LogConnections();

  if (g_use_dummy_data) {
    GuiRequestRender();
    return;
  }

  const uint32_t now_ms = millis();
  bool structure_changed = false;
  bool new_node_seen = false;

  for (const auto &id : mesh.getNodeList()) {
    MeshNode *node_model = GetOrCreateMeshNode(id);
    if (node_model == nullptr) {
      continue;
    }

    // last_update_ms()==0 means this node model was just created and has
    // never been updated (no temps, no persisted topology seed).
    if (node_model->last_update_ms() == 0U) {
      node_model->SetBusGpioAndLastUpdate(-1, now_ms);
      structure_changed = true;
      new_node_seen = true;
    }
  }

  // If any brand-new node appeared via a connection event, re-apply
  // per-node metadata (tile rank, mute, etc.) and labels so those nodes
  // immediately pick up any persisted settings.
  if (new_node_seen) {
    LoadNodeMetaFromNVS();
    LoadLabels();
  }

  if (structure_changed) {
    g_layout_dirty = true;
  } else {
    g_values_dirty = true;
  }

  GuiRequestRender();
}

// -----------------------------------------------------------------------------
// Serial bridge input (GUI node)
// -----------------------------------------------------------------------------

HardwareSerial bridge_serial(0);
static String g_bridge_rx_line;

static void SendLineToBridge(const String &line,
                             const __FlashStringHelper *tag = nullptr) {
  if (g_bridge_passthrough || g_debug_enabled) {
    Serial.print(F("[GUI->BRIDGE SENT] "));
    if (tag != nullptr) {
      Serial.print(tag);
      Serial.print(F(": "));
    }
    Serial.println(line);
  }

  bridge_serial.println(line);
}

static void HandleBridgeFrame(const JsonDocument &doc) {
  if (!doc["payload"].is<JsonVariantConst>()) {
    return;
  }

  // Reuse the root's JSON handling by passing the original temps payload and
  // source node id to OnReceiveRoot().
  String payload;
  serializeJson(doc["payload"], payload);

  String mutable_payload = payload; // OnReceiveRoot expects a mutable String
  const uint32_t from = doc["from"] | 0U;
  OnReceiveRoot(from, mutable_payload);
}

static void HandleBridgeHello(const JsonDocument &doc) {
  const uint32_t root_id = doc["rootId"] | 0U;
  const uint32_t uptime_ms = doc["uptimeMs"] | 0U;
  const char *note = doc["note"] | "";

  if (g_debug_enabled) {
    Serial.printf("[BRIDGE] hello rootId=0x%08lX uptimeMs=%lu note=%s\n",
                  static_cast<unsigned long>(root_id),
                  static_cast<unsigned long>(uptime_ms), note);
  }

  GuiUpdateNetwork(GetAllMeshNodeIds().size());
  MaybeRequestTimeFromBridge("bridge_hello", /*force_ntp=*/false);
  MarkAllNtfyForResend();
  MaybeSendQueuedNtfyToBridge();
  GuiRequestRender();
}

static void HandleBridgeWifiStatus(const JsonDocument &doc, bool connected) {
  const char *ssid = doc["ssid"] | "";
  const char *ip = doc["ip"] | "";

  if (g_debug_enabled) {
    Serial.printf("[BRIDGE] wifi_%s ssid=\"%s\" ip=%s\n",
                  connected ? "connected" : "disconnected", ssid, ip);
  }

  if (connected) {
    MaybeRequestTimeFromBridge("wifi_connected", /*force_ntp=*/false);
  }

  GuiRequestRender();
}

static void HandleBridgeStatus(const JsonDocument &doc) {
  const size_t peers = doc["connections"] | 0U;
  size_t node_count = 0U;
  if (doc["nodes"].is<JsonArrayConst>()) {
    node_count = doc["nodes"].size();
  }
  if (node_count == 0U) {
    node_count = GetAllMeshNodeIds().size();
  }

  GuiUpdateNetwork(std::max(peers, node_count));
  if (!g_ntp_time_valid) {
    MaybeRequestTimeFromBridge("bridge_status", /*force_ntp=*/false);
  }
  GuiRequestRender();
}

static uint32_t NextNtfySequence() { return g_ntfy_next_sequence++; }

static size_t PickNextQueuedNtfyIndex() {
  size_t candidate = SIZE_MAX;
  bool candidate_is_alert = false;

  for (size_t i = 0; i < g_ntfy_pending_bridge.size(); ++i) {
    if (!g_ntfy_pending_bridge[i].needs_resend) {
      continue;
    }
    const bool is_alert = !g_ntfy_pending_bridge[i].is_summary;
    if (candidate == SIZE_MAX) {
      candidate = i;
      candidate_is_alert = is_alert;
      continue;
    }

    if (is_alert && !candidate_is_alert) {
      candidate = i;
      candidate_is_alert = true;
      continue;
    }

    if (is_alert == candidate_is_alert &&
        g_ntfy_pending_bridge[i].sequence <
            g_ntfy_pending_bridge[candidate].sequence) {
      candidate = i;
      candidate_is_alert = is_alert;
    }
  }

  return candidate;
}

static void MaybeSendQueuedNtfyToBridge() {
  if (g_ntfy_pending_bridge.empty()) {
    return;
  }

  const uint32_t now_ms = millis();
  if (g_ntfy_next_send_ms != 0U && now_ms < g_ntfy_next_send_ms) {
    return;
  }

  const size_t idx = PickNextQueuedNtfyIndex();
  if (idx == SIZE_MAX) {
    return;
  }

  const QueuedNtfy pending = g_ntfy_pending_bridge[idx];

  JsonDocument doc;
  doc["type"] = "ntfy_request";
  doc["message"] = pending.message;
  doc["seq"] = pending.sequence;
  if (pending.is_summary) {
    doc["summary"] = true;
  }
  if (pending.cache_when_offline) {
    doc["cache"] = true;
  }
  if (!pending.title.isEmpty()) {
    doc["title"] = pending.title;
  }

  String line;
  serializeJson(doc, line);
  SendLineToBridge(line, /*tag=*/F("ntfy_request"));

  g_ntfy_pending_bridge[idx].last_send_ms = now_ms;
  g_ntfy_pending_bridge[idx].needs_resend = false;
  g_ntfy_next_send_ms = now_ms + kNtfyResendIntervalMs;

  Serial.printf(
      "[NTFY] queued for bridge len=%u cache=%s summary=%s title=%s seq=%lu\n",
      static_cast<unsigned>(pending.message.length()),
      pending.cache_when_offline ? "yes" : "no",
      pending.is_summary ? "yes" : "no",
      pending.title.c_str(), static_cast<unsigned long>(pending.sequence));

  if (!g_bridge_passthrough && g_debug_enabled) {
    Serial.printf(
        "[GUI->BRIDGE SENT] ntfy_request cache=%s summary=%s title=%s seq=%lu len=%u\n",
        pending.cache_when_offline ? "yes" : "no",
        pending.is_summary ? "yes" : "no",
        pending.title.c_str(), static_cast<unsigned long>(pending.sequence),
        static_cast<unsigned>(pending.message.length()));
  }
}

static void HandleNtfyAckFromBridge(const JsonDocument &doc) {
  const uint32_t seq = doc["seq"] | 0U;
  if (seq == 0U) {
    return;
  }

  auto it = std::find_if(g_ntfy_pending_bridge.begin(), g_ntfy_pending_bridge.end(),
                         [&](const QueuedNtfy &pending) {
                           return pending.sequence == seq;
                         });
  if (it == g_ntfy_pending_bridge.end()) {
    if (g_debug_enabled) {
      Serial.printf("[NTFY] ack for unknown seq=%lu\n",
                    static_cast<unsigned long>(seq));
    }
    return;
  }

  g_ntfy_pending_bridge.erase(it);
  if (g_ntfy_pending_bridge.empty()) {
    g_ntfy_next_send_ms = 0;
  } else if (g_ntfy_next_send_ms == 0U) {
    g_ntfy_next_send_ms = millis() + kNtfyResendIntervalMs;
  }

  if (g_debug_enabled) {
    Serial.printf("[NTFY] acked seq=%lu remaining=%u\n",
                  static_cast<unsigned long>(seq),
                  static_cast<unsigned>(g_ntfy_pending_bridge.size()));
  }
}

static void MarkAllNtfyForResend() {
  for (auto &pending : g_ntfy_pending_bridge) {
    pending.needs_resend = true;
  }
  g_ntfy_next_send_ms = millis();
}

static void HandleBridgeTimeSync(const JsonDocument &doc) {
  const time_t epoch = doc["epoch"] | 0;
  const int32_t tz_min = doc["tzMinutes"] | 0;
  const bool dst = doc["dst"] | false;

  if (epoch <= 0) {
    return;
  }

  const long gmt_offset_sec = static_cast<long>(tz_min) * 60L;
  const long dst_offset_sec = dst ? 3600L : 0L;

  // Apply timezone offsets (servers empty because GUI has no Wi-Fi/NTP).
  configTime(gmt_offset_sec, dst_offset_sec, "", "");

  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  g_ntp_sync_in_progress = false;
  g_ntp_time_valid = true;
  g_last_ntp_ok_ms = millis();
  g_ntp_retry_period_ms = kNtpResyncPeriodMs;
  g_ntp_last_sync_epoch = epoch;

  if (!g_history_time_backfilled) {
    OnFirstNtpTimeSync(epoch, g_last_ntp_ok_ms);
  }

  GuiUpdateNtpStatusIcon();
  GuiRequestRender();
}

static void SendTimeRequestToBridge(const char *reason, bool force_ntp) {
  JsonDocument doc;
  doc["type"] = "time_request";
  if (reason != nullptr && reason[0] != '\0') {
    doc["reason"] = reason;
  }
  if (force_ntp) {
    doc["force"] = true;
  }

  String line;
  serializeJson(doc, line);
  SendLineToBridge(line, /*tag=*/F("time_request"));

  if (!g_bridge_passthrough && g_debug_enabled) {
    const char *why = (reason != nullptr && reason[0] != '\0') ? reason : "";
    Serial.printf("[GUI->BRIDGE SENT] time_request force=%s reason=%s\n",
                  force_ntp ? "true" : "false", why);
  }
}

static void SendNtfyRequestToBridge(const String &message,
                                    bool cache_when_offline, bool is_summary,
                                    const char *title) {
  if (!g_ntfy_config.enabled || message.isEmpty()) {
    Serial.println(F("[NTFY] not sending to bridge (disabled or empty message)"));
    return;
  }

  QueuedNtfy pending;
  pending.sequence = NextNtfySequence();
  pending.message = message;
  pending.title = (title != nullptr) ? String(title) : "";
  pending.is_summary = is_summary;
  pending.cache_when_offline = cache_when_offline;

  g_ntfy_pending_bridge.push_back(std::move(pending));
  if (g_ntfy_next_send_ms == 0U) {
    g_ntfy_next_send_ms = millis();
  }

  MaybeSendQueuedNtfyToBridge();
}

static void MaybeRequestTimeFromBridge(const char *reason, bool force_ntp) {
  const uint32_t now_ms = millis();
  constexpr uint32_t kMinIntervalMs = 2000;
  if (g_last_time_request_ms != 0U &&
      (now_ms - g_last_time_request_ms) < kMinIntervalMs) {
    return;  // Avoid spamming the bridge.
  }

  SendTimeRequestToBridge(reason, force_ntp);
  g_last_time_request_ms = now_ms;
}

static void ProcessBridgeSerial() {
  while (bridge_serial.available() > 0) {
    const char ch = bridge_serial.read();
    if (ch == '\n') {
      if (g_bridge_rx_line.isEmpty()) {
        continue;
      }

      if (g_debug_enabled || g_bridge_passthrough) {
        Serial.print(F("[BRIDGE->GUI RECEIVED] "));
        Serial.println(g_bridge_rx_line);
      }

      JsonDocument doc;
      const auto err = deserializeJson(doc, g_bridge_rx_line);
      if (err != DeserializationError::Ok) {
        Serial.printf("[BRIDGE JSON] parse error=%s raw=%s\n", err.c_str(),
                      g_bridge_rx_line.c_str());
      } else {
        const char *type = doc["type"] | "";
        if (strcmp(type, "mesh_frame") == 0) {
          HandleBridgeFrame(doc);
        } else if (strcmp(type, "bridge_hello") == 0) {
          HandleBridgeHello(doc);
        } else if (strcmp(type, "bridge_status") == 0) {
          HandleBridgeStatus(doc);
        } else if (strcmp(type, "wifi_connected") == 0) {
          HandleBridgeWifiStatus(doc, /*connected=*/true);
        } else if (strcmp(type, "wifi_disconnected") == 0) {
          HandleBridgeWifiStatus(doc, /*connected=*/false);
        } else if (strcmp(type, "time_sync") == 0) {
          HandleBridgeTimeSync(doc);
        } else if (strcmp(type, "ntfy_ack") == 0) {
          HandleNtfyAckFromBridge(doc);
        } else {
          Serial.printf("[BRIDGE JSON] unknown type=%s raw=%s\n", type,
                        g_bridge_rx_line.c_str());
        }
      }

      g_bridge_rx_line.clear();
    } else if (ch != '\r') {
      g_bridge_rx_line += ch;
    }
  }
}

static void ProcessBridgePassthroughTx() {
  if (!g_bridge_passthrough) return;

  while (Serial.available() > 0) {
    const char ch = Serial.read();

    if (g_bridge_passthrough_escape) {
      g_bridge_passthrough_escape = false;
      if (ch == '.') {
        g_bridge_passthrough = false;
        g_bridge_passthrough_tx_line = "";
        Serial.println(F("[BRIDGE<->GUI] passthrough disabled"));
        return;
      }
      // If escape was started but not completed, forward both characters.
      bridge_serial.write('~');
      if (ch != '\r' && ch != '\n') {
        g_bridge_passthrough_tx_line += '~';
      }
    }

    if (ch == '~') {
      g_bridge_passthrough_escape = true;
      continue;
    }

    bridge_serial.write(ch);

    if (ch == '\n') {
      if (!g_bridge_passthrough_tx_line.isEmpty()) {
        Serial.print(F("[GUI-PC->BRIDGE] "));
        Serial.println(g_bridge_passthrough_tx_line);
      }
      g_bridge_passthrough_tx_line = "";
    } else if (ch != '\r') {
      g_bridge_passthrough_tx_line += ch;
    }
  }
}

// -----------------------------------------------------------------------------
// Arduino entry points (GUI)
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(DBG_BAUD);
  bridge_serial.begin(BRIDGE_GUI_BAUD, SERIAL_8N1, BRIDGE_GUI_RX_PIN,
                      BRIDGE_GUI_TX_PIN);
  delay(200);

  #ifdef ESP_ARDUINO_VERSION_STR
    Serial.printf("ESP32 Arduino core version: %s\n", ESP_ARDUINO_VERSION_STR);
  #else
    Serial.println("ESP32 Arduino core version macro not defined (likely core < 3.x).");
  #endif

  Serial.printf("ESP-IDF version        : %s\n", esp_get_idf_version());

  RootInitStorage();
  RootInitNetwork();

  DisplayInit();
  GuiInit();

  // Make the house map with ROOM sensors the default view on boot.
  g_map_use_underbelly = false;       // ensure we show Room sensors
  RoomMapSetViewMode(kUiViewModeMap); // switch LVGL to map view

  GuiUpdateNetwork(0);
  GuiRequestRender();

  pinMode(kBuzzerPin, OUTPUT);
  digitalWrite(kBuzzerPin, LOW);

  g_console.RegisterCommand("help", &CmdHelp, "show help");
  g_console.RegisterCommand("passthru", &CmdPassthru,
                            "mirror GUI<->bridge UART to USB");
  g_console.RegisterCommand("ls", &CmdLs, "list nodes/sensors");
  g_console.RegisterCommand("node", &CmdNode, "label or rm a node");
  g_console.RegisterCommand("nodes", &CmdNodes, "nodes clear");
  g_console.RegisterCommand("name", &CmdName, "label a sensor");
  g_console.RegisterCommand("save", &CmdSave, "save labels");
  g_console.RegisterCommand("load", &CmdLoad, "load labels");
  g_console.RegisterCommand("erase", &CmdErase, "erase labels");
  g_console.RegisterCommand("debug", &CmdDebug, "debug on|off|nodes");
  g_console.RegisterCommand("time", &CmdTime, "time now|request|sync");
  g_console.RegisterCommand("units", &CmdUnits, "units c|f");
  g_console.RegisterCommand("highlight", &CmdHighlight,
                            "highlight missing on|off | highlight stale <min>");
  g_console.RegisterCommand("dummy", &CmdDummy, "dummy on|off");
  g_console.RegisterCommand("limits", &CmdLimits,
                            "limits show | warn <lo> <hi> | alert <lo> <hi>");
  g_console.RegisterCommand("ntfy", &CmdNtfy,
                            "ntfy show|enable|summary|freq|test [msg]");
  g_console.RegisterCommand("buzzer", &CmdBuzzer,
                            "buzzer len | warn | alert | cooldown | test | silence | stop");
  g_console.RegisterCommand("flash", &CmdFlash, "flash <ms>|off");
  g_console.RegisterCommand("order", &CmdOrder, "sensor global order");
  g_console.RegisterCommand("norder", &CmdNorder, "tile order");
  g_console.RegisterCommand("sorder", &CmdSorder, "per-node sensor order");
  g_console.RegisterCommand(
      "mute", &CmdMute,
      "mute <nodeId> off|low|high|both | mute get <nodeId> | mute list");
  g_console.RegisterCommand("stats", &CmdStats, "json usage + known size");
  g_console.RegisterCommand("known", &CmdKnown, "known [erase|show]");
  g_console.RegisterCommand("topo", &CmdTopo,
                            "topo show|on|off|save|load|clear");
  g_console.RegisterCommand("labels", &CmdLabels,
                            "labels sensor on|off | labels age on|off");
  g_console.RegisterCommand("topo", &CmdTopo,
                            "topo show|on|off|save|load|clear");
  g_console.RegisterCommand("labels", &CmdLabels,
                            "labels sensor on|off | labels age on|off");
  g_console.RegisterCommand("view", &CmdView, "view tiles | map [room|belly]");
  g_console.RegisterCommand("hist", &CmdHist, "hist show|get|set|clear|view");

  // This will override the compile-time defaults if keys exist in NVS.
  LoadNtfySettings();
  LoadHistoryConfigFromNvs();

  // Ask the bridge for current time (and to force NTP if needed) after a GUI
  // reboot so the display regains a valid clock without waiting for the
  // bridge's periodic resync window.
  MaybeRequestTimeFromBridge("boot", /*force_ntp=*/true);

  Serial.println(F("GUI ready. Type 'help'."));
}

void loop() {
  ProcessBridgeSerial();
  NtfyLoop();
  DisplayLoop();
  BuzzerLoop();
  if (g_bridge_passthrough) {
    ProcessBridgePassthroughTx();
  } else {
    g_console.Poll(Serial, Serial);
  }
}