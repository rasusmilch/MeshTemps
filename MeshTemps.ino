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
#include "mesh_node.h"  // make NodeMetaRecord & MeshNode visible before auto-prototypes

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
    if (i > start)
      out += ' ';
    out += argv[i];
  }
  return out;
}

// Echo the full command line (argv[0..argc-1]) before executing a handler.
static void PrintCommandHeader(Print& out, int argc, const String argv[]) {
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

#include "mesh_tile.h"

bool g_topology_persist_enabled = true;

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
    1000; // ms between flash toggles; 0 = off
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

volatile bool g_layout_dirty = false; // expensive: create/reflow tiles
volatile bool g_values_dirty = false; // cheap: rewrite texts/colors only

volatile size_t g_ui_peers = 0;

Board *g_board = nullptr;

void GuiRebuildTiles();  // forward
// BuildTileContentForNode is defined later, after mesh_tile.h is included.
// No extra prototype needed here.


// ---------------------------------------------------------------------------
// Storage helpers
// ---------------------------------------------------------------------------

// Per-node alert/warning mute under g_labels["mute"][nodeId]:
// bit0 = LOW side muted; bit1 = HIGH side muted.
enum NodeMuteMask : uint8_t {
  kMuteNone = 0,
  kMuteLow = 1 << 0,
  kMuteHigh = 1 << 1,
  kMuteBoth = kMuteLow | kMuteHigh
};

static uint8_t GetNodeMuteMask(const String &node_id) {
  uint32_t id_u32 = 0;
  if (!ParseNodeIdToU32(node_id, &id_u32)) {
    return kMuteNone;
  }
  MeshNode* node = FindMeshNode(id_u32);
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
    const NodeMetaRecord& rec = records[i];

    // Only apply metadata to nodes that already exist (from known topology
    // or live traffic). Do NOT create nodes here.
    MeshNode* node = FindMeshNode(rec.node_id);
    if (node == nullptr) {
      continue;
    }

    node->set_tile_rank(rec.tile_rank);
    node->set_mute_mask(rec.mute_mask);
  }

}

// Build a sorted vector of current node metadata.
static void BuildCurrentNodeMeta(std::vector<NodeMetaRecord>* out) {
  out->clear();
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  out->reserve(ids.size());
  for (uint32_t id : ids) {
    MeshNode* node = FindMeshNode(id);
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
            [](const NodeMetaRecord& a, const NodeMetaRecord& b) {
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
      if (cur[i].node_id   != prev[i].node_id ||
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
  if (!cur.empty()) {
    Serial.println(F("Saving node meta data..."));
    g_root_preferences.putBytes("node_meta", cur.data(),
                                cur.size() * sizeof(NodeMetaRecord));
  } else {
    g_root_preferences.remove("node_meta");
  }
  g_root_preferences.end();
}

// NEW (helpers, near other Save*/Load* helpers)
static void SaveTopoPersistFlag() {
  Serial.println(F("Saving Topology Persist Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putInt("topo_persist", g_topology_persist_enabled ? 1 : 0);
  g_root_preferences.end();
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
    MeshNode* node = FindMeshNode(id_u32);
    if (node == nullptr) {
      continue;
    }

    String label;
    if (p.value().is<const char*>()) {
      label = p.value().as<const char*>();
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
    const char* label_c = p.value() | "";
    if (label_c == nullptr || label_c[0] == '\0') {
      continue;
    }
    const String label(label_c);

    // Apply to any existing sensor with this address.
    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    for (uint32_t id : ids) {
      MeshNode* node = FindMeshNode(id);
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
      MeshNode* node = FindMeshNode(id);
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
    MeshNode* node = FindMeshNode(id_u32);
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

  JsonObject nodes_obj   = labels["nodes"].to<JsonObject>();
  JsonObject sensors_obj = labels["sensors"].to<JsonObject>();
  JsonObject order_obj   = labels["order"].to<JsonObject>();
  JsonObject sorder_root = labels["sorder"].to<JsonObject>();

  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (uint32_t id : ids) {
    MeshNode* node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    char node_key[9];
    FormatNodeKey(id, node_key, sizeof(node_key));
    const String node_key_hex(node_key);

    if (node->label().length() > 0) {
      nodes_obj[node_key_hex] = node->label();
    }

    for (const auto& sensor : node->sensors()) {
      const String& addr16 = sensor.address;

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
  g_root_preferences.putString("labels", json);
  g_root_preferences.end();
}


void EraseLabels() {
  // Clear runtime labels and ordering from all nodes.
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  for (uint32_t id : ids) {
    MeshNode* node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }
    node->set_label(String());
    std::vector<MeshNode::Sensor>& sensors = node->sensors();
    for (auto& sensor : sensors) {
      sensor.label.clear();
      sensor.global_rank = std::numeric_limits<int32_t>::max();
      sensor.node_rank   = std::numeric_limits<int32_t>::max();
    }
  }

  // Wipe persisted labels.
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.remove("labels");
  g_root_preferences.end();
}



void SaveLimits() {
  Serial.println(F("Saving Limit Settings..."));
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
  Serial.println(F("Saving Unit Settings..."));
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
  Serial.println(F("Saving Highlight Settings..."));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putInt("hl_missing", g_highlight_missing_nodes ? 1 : 0);
  g_root_preferences.putInt("hl_stale_min",
                            static_cast<int>(g_stale_minutes_threshold));
  g_root_preferences.end();
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
  g_root_preferences.putInt("show_labels", g_show_sensor_labels ? 1 : 0);
  g_root_preferences.putInt("show_age", g_show_age_label ? 1 : 0);
  g_root_preferences.end();
}

void SaveBuzzerSettings() {
  Serial.println(F("Saving Buzzer Settings..."));
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
  Serial.println(F("Saving Flash Interval Settings..."));
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

// Default high rank means "unspecified / after ranked ones"
static int GetNodeRank(const String &node_id) {
  uint32_t id_u32 = 0;
  if (!ParseNodeIdToU32(node_id, &id_u32)) {
    return 1000000;
  }
  MeshNode* node = FindMeshNode(id_u32);
  if (node == nullptr) {
    return 1000000;
  }
  return static_cast<int>(node->tile_rank());
}


// --- Topology persistence: fixed-size binary array of node IDs -------------
constexpr size_t kMaxKnownNodes = 20;

// Read sorted known IDs from NVS ("known_bin") into a vector.
static bool ReadKnownFromNVS(std::vector<uint32_t>* out_ids) {
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
static void WriteKnownToNVS(const std::vector<uint32_t>& ids_sorted_unique) {
  const size_t count = std::min(ids_sorted_unique.size(), kMaxKnownNodes);

  Serial.println(F("[NVS] Saving Known IDs to meshroot/known_bin"));
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  if (count > 0) {
    (void)g_root_preferences.putBytes("known_bin", ids_sorted_unique.data(),
                                      count * sizeof(uint32_t));
  } else {
    g_root_preferences.remove("known_bin");
  }
  g_root_preferences.remove("known");  // legacy
  g_root_preferences.end();
}

// Add id to the persisted set (sorted/unique, clamped to kMaxKnownNodes).
// Returns true if NVS was actually updated (i.e., set changed).
static bool AddKnownAndPersist(uint32_t id) {
  std::vector<uint32_t> cur;
  (void)ReadKnownFromNVS(&cur);  // ok if empty

  cur.push_back(id);
  std::sort(cur.begin(), cur.end());
  cur.erase(std::unique(cur.begin(), cur.end()), cur.end());
  if (cur.size() > kMaxKnownNodes) {
    cur.resize(kMaxKnownNodes);
  }

  std::vector<uint32_t> prev;
  (void)ReadKnownFromNVS(&prev);  // ok if empty

  const bool same = (prev.size() == cur.size()) &&
                    std::equal(prev.begin(), prev.end(), cur.begin());
  if (same) {
    return false;  // no NVS activity
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
  (void)ReadKnownFromNVS(&prev);  // ok if empty

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
    MeshNode* node_model = GetOrCreateMeshNode(id);
    if (node_model != nullptr && node_model->last_update_ms() == 0U) {
      node_model->SetBusGpioAndLastUpdate(-1, now_ms);
    }
  }
}

// Remove persisted topology and clear in-memory nodes/tiles.
void EraseKnownTopology() {
  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.remove("known_bin");
  g_root_preferences.remove("known");      // legacy
  g_root_preferences.remove("node_meta");  // also drop rank/mute snapshot
  g_root_preferences.end();

  ClearAllMeshNodes();
  MeshTile::DestroyAll();
}


// Minimal root storage initialization:
//   - runtime settings (units, highlight, limits, buzzer, flash, tile display)
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

  // Topology persistence.
  LoadTopoPersistFlag();
  LoadKnownTopology();       // seeds MeshNode objects for persisted IDs

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
// NOTE: avoid templates here; Arduino's auto-prototype pass doesn't handle them
// well.
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

// Build one tile's Content from MeshNode state + thresholds.
// If a node exists in MeshNode store, we render it; otherwise we skip.
static bool BuildTileContentForNode(const String& node_key_hex,
                                    uint32_t now_ms,
                                    bool node_connected,
                                    void* out_ptr) {
  if (out_ptr == nullptr) {
    return false;
  }
  MeshTile::Content* out = static_cast<MeshTile::Content*>(out_ptr);

  if (out == nullptr) {
    return false;
  }

  const String node_hex = CanonNodeHex8(node_key_hex);
  uint32_t node_id_u32 = 0;
  if (!ParseNodeIdToU32(node_hex, &node_id_u32)) {
    return false;
  }

  const MeshNode* node_model = FindMeshNode(node_id_u32);
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
    char hex_id[11];  // "0x" + 8-hex + NUL
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

  const auto& sensors = node_model->sensors();
  if (!sensors.empty()) {
    // Sort sensor indices by effective rank, then address.
    std::vector<size_t> indices;
    indices.reserve(sensors.size());
    for (size_t i = 0; i < sensors.size(); ++i) {
      indices.push_back(i);
    }

    auto effective_rank = [](const MeshNode::Sensor& s) -> int32_t {
      if (s.node_rank != std::numeric_limits<int32_t>::max()) {
        return s.node_rank;
      }
      return s.global_rank;
    };

    std::sort(indices.begin(), indices.end(),
              [&](size_t ia, size_t ib) {
                const MeshNode::Sensor& sa = sensors[ia];
                const MeshNode::Sensor& sb = sensors[ib];

                const int32_t ra = effective_rank(sa);
                const int32_t rb = effective_rank(sb);
                if (ra != rb) {
                  return ra < rb;
                }
                return sa.address < sb.address;
              });

    for (size_t idx : indices) {
      if (sv_count >= 2) {
        break;  // only two sensor rows per tile
      }

      const MeshNode::Sensor& sensor = sensors[idx];
      MeshTile::SensorView& sv = c.sensors[sv_count++];

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
        bool low_alert = (!isnan(g_alert_low_c) && t < g_alert_low_c);
        bool high_alert = (!isnan(g_alert_high_c) && t > g_alert_high_c);
        if (low_alert && (node_mute & kMuteLow)) {
          low_alert = false;
        }
        if (high_alert && (node_mute & kMuteHigh)) {
          high_alert = false;
        }
        sv.is_alert = (low_alert || high_alert);

        // Warnings only if not already alert (respect mute).
        if (!sv.is_alert) {
          bool low_warn = (!isnan(g_warn_low_c) && t < g_warn_low_c);
          bool high_warn = (!isnan(g_warn_high_c) && t > g_warn_high_c);
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

      // #if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
      //       auto *bus = lcd->getBus();
      //       if (bus != nullptr &&
      //           bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
      //         static_cast<BusRGB *>(bus)->configRGB_BounceBufferSize(
      //             lcd->getFrameWidth() * 20);
      //       }
      // #endif
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
  lv_obj_set_style_bg_color(screen, lv_color_make(0x25, 0x25, 0x25), 0);
lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
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
  // lv_obj_set_style_bg_color(g_ui_tile_container, lv_color_make(0x25, 0x25, 0x25), 0);
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

// Dummy data: 10 nodes, 2 sensors each, with simple ramping temps.
// This now populates only the MeshNode model + tile registry.
static void BuildDummyData() {
  ClearAllMeshNodes();
  MeshTile::DestroyAll();

  const uint32_t now_ms = millis();
  const float base_c = 21.0f;

  for (int i = 1; i <= 10; ++i) {
    const uint32_t node_id = 0xAA000000u + static_cast<uint32_t>(i);
    MeshNode* node_model = GetOrCreateMeshNode(node_id);
    if (node_model == nullptr) {
      continue;
    }
    node_model->SetBusGpioAndLastUpdate(-1, now_ms);

    for (int s = 1; s <= 2; ++s) {
      char addr[17];
      snprintf(addr, sizeof(addr), "D%02d000000000001", i * 2 + s - 2);

      MeshNode::Sensor* sensor =
          node_model->GetOrCreateSensor(String(addr));
      if (sensor == nullptr) {
        continue;
      }

      const float temp_c = base_c + static_cast<float>((i - 1) * 2 + s - 1);
      sensor->temp_c = temp_c;
      sensor->has_value = true;
      sensor->corrected = false;
      sensor->last_ms = now_ms;
    }

    // Simple node label: "Node 1", "Node 2", ...
    char name_buf[16];
    snprintf(name_buf, sizeof(name_buf), "Node %d", i);
    node_model->set_label(String(name_buf));
  }

  g_layout_dirty = true;
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
    MeshNode* node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    char node_key[9];
    FormatNodeKey(id, node_key, sizeof(node_key));
    MeshTile* tile = MeshTile::Find(String(node_key));
    if (tile == nullptr) {
      continue;
    }

    const uint32_t age_min = node->ComputeAgeMinutes(now_ms);

    // Age tick treats tiles here as "known topology" and only
    // marks missing; connection state is handled elsewhere.
    const bool node_connected = false;  // we only care about age here
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
    MeshNode* node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    char node_key[9];
    FormatNodeKey(id, node_key, sizeof(node_key));
    const String node_key_hex(node_key);

    MeshTile* tile = MeshTile::Find(node_key_hex);
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
  std::sort(ids.begin(), ids.end(),
            [&](uint32_t a, uint32_t b) {
              MeshNode* na = FindMeshNode(a);
              MeshNode* nb = FindMeshNode(b);
              if (na == nullptr || nb == nullptr) {
                return a < b;
              }

              const int32_t ra = na->tile_rank();
              const int32_t rb = nb->tile_rank();
              if (ra != rb) {
                return ra < rb;
              }

              const String& la = na->label();
              const String& lb = nb->label();
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
    MeshNode* node = FindMeshNode(node_id);
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

    MeshTile* tile =
        MeshTile::GetOrCreate(node_key_hex, g_ui_tile_container, tile_w,
                              tile_h);
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

// -----------------------------------------------------------------------------
// GUI helpers callable from elsewhere
// -----------------------------------------------------------------------------

void GuiUpdateNetwork(size_t peers) {
  g_ui_peers = peers;
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
  const uint32_t now_ms = millis();

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
      g_values_dirty = false; // rebuilding already refreshed content
    } else if (g_values_dirty) {
      g_values_dirty = false;
      GuiRefreshValues(now_ms); // cheap path, frequent
    }
    last_build_ms = now_ms;
  }

  // Drive per-tile time-based behaviour (flashing) using each tile's own
  // timers and internal state.
  MeshTile::LoopAll(now_ms);

  // Uptime label updated at least once per minute; harmless to refresh here.
  UpdateUptimeLabel();
  lvgl_port_unlock();
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

static void CmdHelp(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  g_console.PrintHelp(out);
}


static void CmdKnown(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc >= 2 && argv[1] == "erase") {
    g_root_preferences.begin("meshroot", false);
    g_root_preferences.remove("known_bin");
    g_root_preferences.remove("known"); // legacy
    g_root_preferences.end();
    out.println(F("known erased"));
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

    out.printf("mute list: %u node(s)\n",
               static_cast<unsigned>(ids.size()));

    for (uint32_t id : ids) {
      MeshNode* node = FindMeshNode(id);
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
    MeshNode* node = FindMeshNode(id_u32);
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
  } else if (mode.equalsIgnoreCase("both") ||
             mode.equalsIgnoreCase("all") ||
             mode.equalsIgnoreCase("on")) {
    mask = kMuteBoth;
  } else {
    out.println(F("ERR mute (use: off|low|high|both)"));
    return;
  }

  MeshNode* node = GetOrCreateMeshNode(id_u32);
  if (node != nullptr) {
    node->set_mute_mask(mask);
    SaveNodeMetaToNVSIfChanged();
  }

  out.printf("mute 0x%08lX=%s\n", static_cast<unsigned long>(id_u32),
             NodeMuteMaskToString(mask));
  GuiRequestRender();
}


static void CmdLs(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  const uint32_t now_ms = millis();
  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  if (ids.empty()) {
    out.println(F("(no nodes)"));
  }

  for (uint32_t id : ids) {
    MeshNode* node = FindMeshNode(id);
    if (node == nullptr) {
      continue;
    }

    const String node_hex = node->node_key_hex();
    const String node_label = node->label();
    const uint32_t age_min = node->ComputeAgeMinutes(now_ms);
    const int bus_gpio = node->bus_gpio();

    if (node_label.length() > 0) {
      out.printf("node %s \"%s\" gpio=%d age=%lu min:\n",
                 node_hex.c_str(), node_label.c_str(), bus_gpio,
                 static_cast<unsigned long>(age_min));
    } else {
      out.printf("node %s gpio=%d age=%lu min:\n",
                 node_hex.c_str(), bus_gpio,
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

    const auto& sensors = node->sensors();
    if (sensors.empty()) {
      out.println(F("  (no sensors)"));
      continue;
    }

    for (const auto& sensor : sensors) {
      const String& addr16 = sensor.address;
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
        out.printf("  %s \"%s\" : %s%s (age=%lu min)\n",
                   addr16.c_str(), sensor_label.c_str(),
                   temp_s.c_str(),
                   sensor.corrected ? " (corr)" : "",
                   static_cast<unsigned long>(age_sensor_min));
      } else {
        out.printf("  %s : %s%s (age=%lu min)\n",
                   addr16.c_str(),
                   temp_s.c_str(),
                   sensor.corrected ? " (corr)" : "",
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

  MeshNode* node_model = GetOrCreateMeshNode(id_u32);
  if (node_model != nullptr) {
    node_model->set_label(label);
    SaveNodeMetaToNVSIfChanged();
    out.printf("node 0x%08lX label=\"%s\"\n",
               static_cast<unsigned long>(id_u32),
               label.c_str());
  } else {
    out.printf("node 0x%08lX: FAILED to set label \"%s\"\n",
               static_cast<unsigned long>(id_u32),
               label.c_str());
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

static void CmdName(void* ctx, int argc, const String argv[], Print& out) {
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
    MeshNode* node = FindMeshNode(id);
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
  out.printf("name %s=\"%s\" (labels saved)\n",
             addr16.c_str(), label.c_str());
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

static void CmdStats(void* ctx, int argc, const String argv[], Print& out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  (void)argc;
  (void)argv;

  const std::vector<uint32_t> ids = GetAllMeshNodeIds();
  size_t sensor_count = 0;
  for (uint32_t id : ids) {
    MeshNode* node = FindMeshNode(id);
    if (node != nullptr) {
      sensor_count += node->sensors().size();
    }
  }

  out.printf("nodes=%u sensors=%u\n",
             static_cast<unsigned>(ids.size()),
             static_cast<unsigned>(sensor_count));

  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const size_t known_bytes = g_root_preferences.getBytesLength("known_bin");
  const size_t labels_bytes = g_root_preferences.getString("labels", "").length();
  g_root_preferences.end();

  out.printf("known_bin NVS size=%u bytes\n",
             static_cast<unsigned>(known_bytes));
  out.printf("labels NVS size=%u bytes\n",
             static_cast<unsigned>(labels_bytes));
}

static void CmdDebug(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  // Extended debug: "debug nodes" prints per-node age and sequence health.
  if (argc >= 2 && argv[1] == "nodes") {
    const uint32_t now_ms = millis();

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
    LoadKnownTopology();   // seeds MeshNode store from NVS if enabled

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
  const char *unit = g_display_fahrenheit ? "F" : "C";
  if (argc >= 2 && argv[1] == "show") {
    out.printf("limits warn=%.2f..%.2f %s\n", FromC(g_warn_low_c),
               FromC(g_warn_high_c), unit);
    out.printf("limits alert=%.2f..%.2f %s\n", FromC(g_alert_low_c),
               FromC(g_alert_high_c), unit);
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
  out.println(F("ERR limits (use 'limits show' | 'limits warn <lo> <hi>' | "
                "'limits alert <lo> <hi>')"));
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
    GuiRequestRender();
    out.println(F("topo load: pre-seeded MeshNode store from NVS"));
    return;
  }

  if (sub == "clear") {
    EraseKnownTopology();
    out.println(
        F("topo clear: erased persisted topology and in-memory nodes"));
    GuiRequestRender();
    return;
  }

  out.println(F("ERR topo (type just 'topo' for help)"));
}


static void CmdBuzzer(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  if (argc < 4) {
    out.println(F("ERR buzzer (use: buzzer <len_ms> <warn_gap_ms> "
                  "<alert_gap_ms>, gaps>=0)"));
    return;
  }
  long len_ms = argv[1].toInt();
  long gap_warn_ms = argv[2].toInt();
  long gap_alert_ms = argv[3].toInt();
  if (len_ms <= 0 || gap_warn_ms < 0 || gap_alert_ms < 0) {
    out.println(F("ERR buzzer (use: buzzer <len_ms> <warn_gap_ms> "
                  "<alert_gap_ms>, gaps>=0)"));
    return;
  }
  g_beep_len_ms = (uint32_t)len_ms;
  g_beep_gap_warn_ms = (uint32_t)gap_warn_ms;
  g_beep_gap_alert_ms = (uint32_t)gap_alert_ms;
  SaveBuzzerSettings();
  out.printf("buzzer len=%lu ms warn_gap=%lu ms alert_gap=%lu ms\n",
             (unsigned long)g_beep_len_ms, (unsigned long)g_beep_gap_warn_ms,
             (unsigned long)g_beep_gap_alert_ms);
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

    out.printf("norder list: %u node(s)\n",
               static_cast<unsigned>(ids.size()));

    for (uint32_t id : ids) {
      MeshNode* node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }
      char key[9];
      FormatNodeKey(id, key, sizeof(key));
      out.printf("%s : %ld\n", key,
                 static_cast<long>(node->tile_rank()));
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
    MeshNode* node = FindMeshNode(id_u32);
    if (node != nullptr) {
      node->set_tile_rank(std::numeric_limits<int32_t>::max());
      SaveNodeMetaToNVSIfChanged();
    }
    out.printf("norder clear %s: rank reset to default\n",
               argv[2].c_str());
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
    MeshNode* node = GetOrCreateMeshNode(id_u32);
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
      MeshNode* node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }

      const std::vector<MeshNode::Sensor>& sensors = node->sensors();
      for (const auto& sensor : sensors) {
        if (sensor.global_rank == std::numeric_limits<int32_t>::max()) {
          continue;  // "unspecified"
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
    for (const auto& kv : rank_by_addr) {
      entries.push_back(kv);
    }

    std::sort(entries.begin(), entries.end(),
              [](const std::pair<String, int32_t>& a,
                 const std::pair<String, int32_t>& b) {
                if (a.second != b.second) {
                  return a.second < b.second;
                }
                return a.first < b.first;
              });

    out.printf("order list: %u unique address(es)\n",
               static_cast<unsigned>(entries.size()));
    for (const auto& e : entries) {
      out.printf("%s : %ld\n", e.first.c_str(),
                 static_cast<long>(e.second));
    }
    out.println(F("order list: done"));
    return;
  }

  // order clear <addr16>
  if (argc >= 3 && argv[1] == "clear") {
    const String key = CanonAddr16(argv[2]);

    const std::vector<uint32_t> ids = GetAllMeshNodeIds();
    for (uint32_t id : ids) {
      MeshNode* node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }
      (void)node->SetSensorGlobalRank(
          key, std::numeric_limits<int32_t>::max());
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
      MeshNode* node = FindMeshNode(id);
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
    out.printf("order %s -> %ld (labels saved)\n",
               key.c_str(), static_cast<long>(rank));
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

      MeshNode* node = FindMeshNode(id_u32);
      if (node == nullptr) {
        out.printf("sorder list %s: (node not found)\n", argv[2].c_str());
        return;
      }

      const std::vector<MeshNode::Sensor>& sensors = node->sensors();
      std::vector<std::pair<String, int32_t>> entries;
      entries.reserve(sensors.size());

      for (const auto& sensor : sensors) {
        if (sensor.node_rank == std::numeric_limits<int32_t>::max()) {
          continue;  // "unspecified"
        }
        entries.emplace_back(CanonAddr16(sensor.address), sensor.node_rank);
      }

      if (entries.empty()) {
        out.printf("sorder list %s: (no per-node ranks)\n", argv[2].c_str());
        return;
      }

      std::sort(entries.begin(), entries.end(),
                [](const std::pair<String, int32_t>& a,
                   const std::pair<String, int32_t>& b) {
                  if (a.second != b.second) {
                    return a.second < b.second;
                  }
                  return a.first < b.first;
                });

      out.printf("sorder list %s:\n", argv[2].c_str());
      for (const auto& e : entries) {
        out.printf("%s : %ld\n", e.first.c_str(),
                   static_cast<long>(e.second));
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
      MeshNode* node = FindMeshNode(id);
      if (node == nullptr) {
        continue;
      }

      char node_key[9];
      FormatNodeKey(id, node_key, sizeof(node_key));
      out.printf("[%s]\n", node_key);

      const std::vector<MeshNode::Sensor>& sensors = node->sensors();
      std::vector<std::pair<String, int32_t>> entries;
      entries.reserve(sensors.size());

      for (const auto& sensor : sensors) {
        if (sensor.node_rank == std::numeric_limits<int32_t>::max()) {
          continue;
        }
        entries.emplace_back(CanonAddr16(sensor.address),
                             sensor.node_rank);
      }

      if (entries.empty()) {
        out.println(F("  (empty)"));
      } else {
        std::sort(entries.begin(), entries.end(),
                  [](const std::pair<String, int32_t>& a,
                     const std::pair<String, int32_t>& b) {
                    if (a.second != b.second) {
                      return a.second < b.second;
                    }
                    return a.first < b.first;
                  });

        for (const auto& e : entries) {
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
    MeshNode* node = FindMeshNode(id_u32);
    if (node != nullptr) {
      (void)node->SetSensorNodeRank(
          key, std::numeric_limits<int32_t>::max());
    }

    SaveLabels();
    out.printf("sorder clear %s %s: rank reset\n",
               argv[2].c_str(), key.c_str());
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

    MeshNode* node = FindMeshNode(id_u32);
    if (node != nullptr) {
      (void)node->SetSensorNodeRank(key, rank);
    }

    SaveLabels();
    out.printf("sorder %s %s -> %ld\n",
               argv[1].c_str(), key.c_str(), static_cast<long>(rank));
    GuiRequestRender();
    return;
  }

  out.println(
      F("ERR sorder (use 'sorder <nodeIdHex> <addr16> <rank>' | "
        "'sorder clear <nodeIdHex> <addr16>' | 'sorder list [nodeIdHex]')"));
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

void OnReceiveRoot(uint32_t from, String& msg) {
  bool structure_changed = false;

  DLOG("[ROOT RX] from=0x%08lX len=%u: %s\n",
       static_cast<unsigned long>(from),
       static_cast<unsigned>(msg.length()), msg.c_str());

  if (g_use_dummy_data) {
    // In dummy mode, ignore real traffic.
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

  const uint32_t now_ms = millis();

  MeshNode* node_model = UpdateMeshNodeFromTempsJson(doc, from, now_ms);
  if (node_model == nullptr) {
    DLOG("  ! UpdateMeshNodeFromTempsJson failed\n");
    return;
  }

  const uint32_t node_id = node_model->node_id();
  const int bus_gpio = node_model->bus_gpio();

  DLOG("  nodeId=0x%08lX busGpio=%d sensors=%u\n",
       static_cast<unsigned long>(node_id), bus_gpio,
       static_cast<unsigned>(node_model->sensors().size()));

  // Topology persistence: add/update known IDs.
  if (g_topology_persist_enabled) {
    structure_changed |= AddKnownAndPersist(node_id);
  }

  // For now, labels/ranks live in MeshNode only. If you want to
  // re-apply persisted labels on each packet, call LoadLabels()
  // occasionally or add a small cache here.

  GuiUpdateNodeSummary(node_model->node_id_str().c_str(), bus_gpio, now_ms);

  // Layout vs values.
  if (structure_changed) {
    g_layout_dirty = true;   // new node(s)
  } else {
    g_values_dirty = true;   // just values
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

  for (const auto& id : mesh.getNodeList()) {
    MeshNode* node_model = GetOrCreateMeshNode(id);
    if (node_model != nullptr && node_model->last_update_ms() == 0U) {
      node_model->SetBusGpioAndLastUpdate(-1, now_ms);
      structure_changed = true;
    }
  }

  if (structure_changed) {
    g_layout_dirty = true;
  } else {
    g_values_dirty = true;
  }

  GuiRequestRender();
}


// -----------------------------------------------------------------------------
// Arduino entry points (root)
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(DBG_BAUD);
  delay(200);

  RootInitStorage();

  // NEW: pre-populate tiles from persisted topology
  // LoadKnownTopology();

  DisplayInit();
  GuiInit();
  GuiUpdateNetwork(0);
  GuiRequestRender();

  pinMode(kBuzzerPin, OUTPUT);
  digitalWrite(kBuzzerPin, LOW);

  g_console.RegisterCommand("help", &CmdHelp, "show help");
  g_console.RegisterCommand("ls", &CmdLs, "list nodes/sensors");
  g_console.RegisterCommand("node", &CmdNode, "label or rm a node");
  g_console.RegisterCommand("nodes", &CmdNodes, "nodes clear");
  g_console.RegisterCommand("name", &CmdName, "label a sensor");
  g_console.RegisterCommand("save", &CmdSave, "save labels");
  g_console.RegisterCommand("load", &CmdLoad, "load labels");
  g_console.RegisterCommand("erase", &CmdErase, "erase labels");
  g_console.RegisterCommand("debug", &CmdDebug, "debug on|off|nodes");
  g_console.RegisterCommand("units", &CmdUnits, "units c|f");
  g_console.RegisterCommand("highlight", &CmdHighlight,
                            "highlight missing on|off | highlight stale <min>");
  g_console.RegisterCommand("dummy", &CmdDummy, "dummy on|off");
  g_console.RegisterCommand("limits", &CmdLimits,
                            "limits show | warn <lo> <hi> | alert <lo> <hi>");
  g_console.RegisterCommand("buzzer", &CmdBuzzer,
                            "buzzer <len_ms> <warn_gap_ms> <alert_gap_ms>");
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

// NEW: per-node transmit sequence number (increments on every temps send).
uint32_t g_send_seq = 0;

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

void SendTemperatures() {
  g_ds18.requestTemperatures();

  JsonDocument doc;
  doc["type"] = "temps";
  doc["nodeId"] = mesh.getNodeId();
  doc["busGpio"] = ONEWIRE_PIN;
  doc["uptimeMs"] = static_cast<uint32_t>(millis());

  // NEW: monotonically increasing sequence number per node.
  // Wraparound is tolerated; the root treats a lower value as a reset.
  ++g_send_seq;
  doc["seq"] = g_send_seq;

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

// Station pressure from sea-level pressure (altimeter setting) and elevation.
// ISA troposphere: P = P0 * (1 - 2.25577e-5 * h_m)^5.25588
static float stationPressureFromSLP_inHg(float slp_inHg, float elev_ft) {
  if (slp_inHg <= 0.0f)
    return NAN;
  const float h_m = (elev_ft <= 0.0f) ? 0.0f : elev_ft * 0.3048f;
  const float base = 1.0f - 2.25577e-5f * h_m;
  if (base <= 0.0f)
    return NAN; // out of model range
  return slp_inHg * powf(base, 5.25588f);
}

// Boiling point [°C] from STATION pressure in inHg using Antoine.
// log10(P_mmHg) = A - B/(C + T), with A=8.07131, B=1730.63, C=233.426.
static float boilingPointC_fromStationInHg(float station_inHg) {
  if (station_inHg <= 0.0f)
    return NAN;
  const float P_mmHg = station_inHg * 25.4f; // 1 inHg = 25.4 mmHg
  if (P_mmHg <= 0.0f)
    return NAN;

  const float A = 8.07131f;
  const float B = 1730.63f;
  const float C = 233.426f;

  const float logP = log10f(P_mmHg);
  const float denom = A - logP;
  if (denom == 0.0f)
    return NAN;

  return B / denom - C; // °C
}

// Convenience: treat inHg as ALTIMETER (sea-level) if elevation is provided.
// If elev_ft == 0, the value is effectively already station pressure.
static float boilingPointC_fromInHgElev(float inHg, float elev_ft) {
  const float station_inHg =
      (elev_ft != 0.0f) ? stationPressureFromSLP_inHg(inHg, elev_ft) : inHg;
  return boilingPointC_fromStationInHg(station_inHg);
}

// -----------------------------------------------------------------------------
// Console command handlers (LEAF)
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
    out.printf("debug=%s\n", g_debug_enabled ? "on" : "off");
    return;
  }
  g_debug_enabled = (argv[1] == "on");
  out.printf("debug=%s\n", g_debug_enabled ? "on" : "off");
}

static void CmdScan(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  (void)argc;
  (void)argv;
  ScanSensors();
  out.printf("scan: enumerated %u DS18B20 sensor(s)\n",
             static_cast<unsigned>(g_devices.size()));
}

static void CmdSendNow(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  (void)argc;
  (void)argv;
  SendTemperatures();
  out.println(F("sendnow: temperatures frame sent to mesh"));
}

static void CmdBoilPt(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  if (argc < 2) {
    out.println(F("ERR boilpt <inHg> [elev_ft]"));
    return;
  }

  const float inHg = argv[1].toFloat();

  if (argc >= 3) {
    // Treat argv[1] as ALTIMERTER (sea-level) and argv[2] as elevation in feet.
    const float elev_ft = argv[2].toFloat();
    const float station_inHg = stationPressureFromSLP_inHg(inHg, elev_ft);
    const float TbC = boilingPointC_fromStationInHg(station_inHg);
    if (isnan(TbC)) {
      out.println(F("ERR invalid inputs"));
      return;
    }
    const float TbF = TbC * 9.0f / 5.0f + 32.0f;
    out.printf("Boiling point at station %.3f inHg (AS=%.3f, elev=%.0f ft): "
               "%.3f C (%.3f F)\n",
               (double)station_inHg, (double)inHg, (double)elev_ft, (double)TbC,
               (double)TbF);
    return;
  }

  // Back-compat: single arg -> treat as STATION pressure directly.
  const float TbC = boilingPointC_fromStationInHg(inHg);
  if (isnan(TbC)) {
    out.println(F("ERR invalid pressure"));
    return;
  }
  const float TbF = TbC * 9.0f / 5.0f + 32.0f;
  out.printf("Boiling point at %.3f inHg (station): %.3f C (%.3f F)\n",
             (double)inHg, (double)TbC, (double)TbF);
}

static void CmdWhoAmI(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);

  const uint32_t node = mesh.getNodeId();
  const uint64_t mac = ESP.getEfuseMac(); // base MAC from eFuse
  out.printf("nodeId: 0x%08lX\n", static_cast<unsigned long>(node));
  out.printf("baseMAC: %04X%08lX\n",
             static_cast<unsigned>((mac >> 32) & 0xFFFFu),
             static_cast<unsigned long>(mac & 0xFFFFFFFFul));
}


static void CmdCal(void *ctx, int argc, const String argv[], Print &out) {
  (void)ctx;
  PrintCommandHeader(out, argc, argv);
  
  if (argc < 2) {
    out.println(F("cal cmds:\n"
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

  if (sub == "ice") {
    if (argc >= 3) {
      CalibrationStart(argv[2], kCalIce);
      out.printf("cal ice %s: session started\n", argv[2].c_str());
    } else {
      CalibrationStartAll(kCalIce);
      out.println(F("cal ice: session started for all attached sensors"));
    }
    return;
  }

  if (sub == "boil") {
    if (argc >= 3) {
      CalibrationStart(argv[2], kCalBoil);
      out.printf("cal boil %s: session started\n", argv[2].c_str());
    } else {
      CalibrationStartAll(kCalBoil);
      out.println(F("cal boil: session started for all attached sensors"));
    }
    return;
  }

  if (sub == "lock") {
    if (argc < 3) {
      out.println(F("ERR cal lock <actualC>"));
      return;
    }
    const float actual_c = argv[2].toFloat();
    CalibrationLockAll(actual_c);
    out.printf("cal lock %.3fC: lock requested for active sensors\n",
               static_cast<double>(actual_c));
    return;
  }

  if (sub == "solve") {
    CalibrationSolveAndSaveAll();
    out.println(F("cal solve: attempted solve + save for all locked sensors"));
    return;
  }

  if (sub == "list") {
    for (const auto &e : g_cal_entries) {
      const Coeff &c = e.coeff;
      out.printf("%s : a1=%.6f a0=%.6f\n", e.addr.c_str(), (double)c.a1,
                 (double)c.a0);
    }
    return;
  }

  if (sub == "show") {
    if (argc >= 3) {
      const int idx = FindCalIndex(argv[2]);
      if (idx >= 0) {
        const Coeff &c = g_cal_entries[(size_t)idx].coeff;
        out.printf("%s : a1=%.6f a0=%.6f\n", argv[2].c_str(), (double)c.a1,
                   (double)c.a0);
      } else {
        out.printf("cal show %s: not found\n", argv[2].c_str());
      }
    } else {
      for (const auto &e : g_cal_entries) {
        const Coeff &c = e.coeff;
        out.printf("%s : a1=%.6f a0=%.6f\n", e.addr.c_str(), (double)c.a1,
                   (double)c.a0);
      }
    }
    return;
  }

  if (sub == "clear") {
    if (argc < 3) {
      out.println(F("ERR cal clear <addr16>"));
      return;
    }
    ClearCalibration(argv[2]);
    out.printf("cal clear %s: coefficients removed\n", argv[2].c_str());
    return;
  }

  if (sub == "save") {
    SaveAllCalibration();
    out.println(F("cal save: all calibration coefficients persisted to NVS"));
    return;
  }

  if (sub == "load") {
    LoadAllCalibration();
    out.println(F("cal load: calibration coefficients loaded from NVS"));
    return;
  }

  out.println(F("ERR cal (type just 'cal' for help)"));
}

// painlessMesh callbacks (leaf).
void OnReceiveLeaf(uint32_t from, String &msg) {
  DLOG("[LEAF RX] from=0x%08lX len=%u: %s\n", static_cast<unsigned long>(from),
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
      DLOG("  root_announce: rootId=0x%08lX\n",
           static_cast<unsigned long>(g_root_id));
    }
  }
}

void OnConnectionsChangedLeaf() { LogConnections(); }

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

  g_console.RegisterCommand("help", &CmdHelp, "show help");
  g_console.RegisterCommand("debug", &CmdDebug, "debug on|off");
  g_console.RegisterCommand("scan", &CmdScan, "enumerate DS18B20");
  g_console.RegisterCommand("sendnow", &CmdSendNow, "send temperatures now");
  g_console.RegisterCommand("boilpt", &CmdBoilPt, "boilpt <inHg> [elev_ft]");
  g_console.RegisterCommand("cal", &CmdCal, "calibration commands");
  g_console.RegisterCommand("whoami", &CmdWhoAmI, "show nodeId and MAC");

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
