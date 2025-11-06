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

using esp_panel::board::Board;
using esp_panel::drivers::BusRGB;
using esp_panel::drivers::LCD;

Preferences g_root_preferences;

// Last-seen data and labels (persisted).
JsonDocument g_last_seen;
JsonDocument g_labels;

Task g_task_announce;

bool g_display_fahrenheit = true;  // true = °F, false = °C

// LVGL GUI state (root).
lv_obj_t* g_ui_label_title = nullptr;
lv_obj_t* g_ui_label_peers = nullptr;
lv_obj_t* g_ui_table = nullptr;
volatile bool g_gui_dirty = false;

Board* g_board = nullptr;

namespace {

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

void LoadLabels() {
  EnsureDocuments();

  g_root_preferences.begin("meshroot", /*readOnly=*/true);
  const String json = g_root_preferences.getString("labels", "");
  g_root_preferences.end();

  if (json.length() > 0) {
    if (deserializeJson(g_labels, json) != DeserializationError::Ok) {
      g_labels.clear();
      EnsureDocuments();
    }
  }
}

void SaveLabels() {
  EnsureDocuments();

  String json;
  serializeJson(g_labels, json);

  g_root_preferences.begin("meshroot", /*readOnly=*/false);
  g_root_preferences.putString("labels", json);
  g_root_preferences.end();
}

void EraseLabels() {
  g_labels.clear();
  EnsureDocuments();
  SaveLabels();
}

void GuiRebuildTable();  // Forward declaration.

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

  g_ui_label_title = lv_label_create(screen);
  lv_label_set_text(g_ui_label_title, "MeshTemps — ROOT");
  lv_obj_align(g_ui_label_title, LV_ALIGN_TOP_LEFT, 6, 4);

  g_ui_label_peers = lv_label_create(screen);
  lv_label_set_text(g_ui_label_peers, "Peers: 0");
  lv_obj_align(g_ui_label_peers, LV_ALIGN_TOP_RIGHT, -6, 4);

  g_ui_table = lv_table_create(screen);
  lv_obj_set_size(g_ui_table, lv_pct(100), lv_pct(100));
  lv_obj_align(g_ui_table, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_table_set_col_cnt(g_ui_table, 6);
  lv_table_set_row_cnt(g_ui_table, 1);

  lv_table_set_cell_value(g_ui_table, 0, 0, "Node");
  lv_table_set_cell_value(g_ui_table, 0, 1, "Location");
  lv_table_set_cell_value(g_ui_table, 0, 2, "Addr");
  lv_table_set_cell_value(g_ui_table, 0, 3, "Label");

  if (g_display_fahrenheit) {
    lv_table_set_cell_value(g_ui_table, 0, 4, "Temp \xC2\xB0""F");
  } else {
    lv_table_set_cell_value(g_ui_table, 0, 4, "Temp \xC2\xB0""C");
  }

  lv_table_set_cell_value(g_ui_table, 0, 5, "Age(min)");

  g_gui_dirty = true;

  lvgl_port_unlock();
}

void GuiUpdateNodeSummary(const char* node_id,
                          int bus_gpio,
                          uint32_t last_ms) {
  (void)node_id;
  (void)bus_gpio;
  (void)last_ms;
  g_gui_dirty = true;
}

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

}  // namespace

void GuiUpdateNetwork(size_t peers) {
  if (!lvgl_port_lock(-1)) {
    return;
  }

  if (g_ui_label_peers != nullptr) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "Peers: %u",
             static_cast<unsigned>(peers));
    lv_label_set_text(g_ui_label_peers, buffer);
  }

  lvgl_port_unlock();
}

void GuiRequestRender() {
  g_gui_dirty = true;
}

namespace {

void GuiRebuildTable() {
  if (g_ui_table == nullptr) {
    return;
  }

  if (!lvgl_port_lock(-1)) {
    return;
  }

  lv_table_set_row_cnt(g_ui_table, 1);

  const uint32_t now_ms = millis();
  EnsureDocuments();
  JsonObject nodes = g_last_seen["nodes"].as<JsonObject>();

  uint16_t row = 1;

  for (JsonPair node_entry : nodes) {
    const String node_id = node_entry.key().c_str();
    JsonObject node_obj = node_entry.value();

    const String node_label = g_labels["nodes"][node_id] | "";
    JsonObject sensors = node_obj["sensors"].as<JsonObject>();

    bool has_sensor_rows = false;

    for (JsonPair sensor_entry : sensors) {
      has_sensor_rows = true;

      const String addr = sensor_entry.key().c_str();
      JsonObject sensor_obj = sensor_entry.value();

      const float temp_c = sensor_obj["tC"] | NAN;
      const bool corrected = sensor_obj["corr"] | false;
      const uint32_t last_ms = sensor_obj["last"] | 0U;
      const String sensor_label = g_labels["sensors"][addr] | "";

      const uint32_t age_min =
          (last_ms <= now_ms)
              ? (now_ms - last_ms) / 60000U
              : 0U;

      float temp_display = temp_c;
      if (!isnan(temp_c) && g_display_fahrenheit) {
        temp_display = temp_c * 1.8f + 32.0f;
      }

      lv_table_set_row_cnt(g_ui_table, row + 1);

      lv_table_set_cell_value(g_ui_table, row, 0, node_id.c_str());
      lv_table_set_cell_value(g_ui_table, row, 1, node_label.c_str());
      lv_table_set_cell_value(g_ui_table, row, 2, addr.c_str());
      lv_table_set_cell_value(g_ui_table, row, 3, sensor_label.c_str());

      char temp_buffer[24];
      if (isnan(temp_display)) {
        strlcpy(temp_buffer, "--", sizeof(temp_buffer));
      } else {
        snprintf(temp_buffer,
                 sizeof(temp_buffer),
                 "%.2f%s",
                 temp_display,
                 corrected ? " *" : "");
      }
      lv_table_set_cell_value(g_ui_table, row, 4, temp_buffer);

      char age_buffer[12];
      snprintf(age_buffer,
               sizeof(age_buffer),
               "%lu",
               static_cast<unsigned long>(age_min));
      lv_table_set_cell_value(g_ui_table, row, 5, age_buffer);

      ++row;
    }

    // If no sensors yet: show a placeholder row for this node.
    if (!has_sensor_rows) {
      lv_table_set_row_cnt(g_ui_table, row + 1);

      lv_table_set_cell_value(g_ui_table, row, 0, node_id.c_str());
      lv_table_set_cell_value(g_ui_table, row, 1, node_label.c_str());
      lv_table_set_cell_value(g_ui_table, row, 2, "--");
      lv_table_set_cell_value(g_ui_table, row, 3, "");

      lv_table_set_cell_value(g_ui_table, row, 4, "--");

      const uint32_t last_ms = node_obj["last"] | 0U;
      const uint32_t age_min =
          (last_ms <= now_ms)
              ? (now_ms - last_ms) / 60000U
              : 0U;

      char age_buffer[12];
      snprintf(age_buffer,
               sizeof(age_buffer),
               "%lu",
               static_cast<unsigned long>(age_min));
      lv_table_set_cell_value(g_ui_table, row, 5, age_buffer);

      ++row;
    }
  }

  lvgl_port_unlock();
}


void DisplayLoop() {
  static uint32_t last_build_ms = 0;

  const uint32_t now_ms = millis();
  if (g_gui_dirty && (now_ms - last_build_ms >= 50U)) {
    if (lvgl_port_lock(10)) {
      g_gui_dirty = false;
      GuiRebuildTable();
      lvgl_port_unlock();
      last_build_ms = now_ms;
    }
  }
}

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
              sensor_obj["tC"] | std::numeric_limits<float>::quiet_NaN();
          const bool corrected = sensor_obj["corr"] | false;
          const String sensor_label = g_labels["sensors"][addr] | "";

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
    } else {
      Serial.println(F("ERR (help)"));
    }

    input_line = "";
  }
}

// painlessMesh callbacks (names are ours; signatures must match).
void OnReceiveRoot(uint32_t from, String& msg) {
  DLOG("[ROOT RX] from=%u len=%u: %s\n",
       from, static_cast<unsigned>(msg.length()), msg.c_str());

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
  GuiUpdateNodeSummary(node_id_str.c_str(), bus_gpio,
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
    const char* label =
        g_labels["sensors"][addr] | "";

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
                       (label != nullptr && strlen(label) > 0) ? label : nullptr,
                       static_cast<uint32_t>(millis()));
  }

  GuiRequestRender();
}

void OnConnectionsChangedRoot() {
  LogConnections();

  EnsureDocuments();
  JsonObject nodes = g_last_seen["nodes"].to<JsonObject>();

  const uint32_t now_ms = millis();

  // Ensure each connected node has an entry with a last-seen timestamp.
  for (const auto& node_id : mesh.getNodeList()) {
    const String key = String(node_id);

    JsonObject node_obj = nodes[key];
    if (node_obj.isNull()) {
      node_obj = nodes[key].to<JsonObject>();
    }
    if (!node_obj.containsKey("last")) {
      node_obj["last"] = now_ms;
    }
    if (!node_obj["sensors"].is<JsonObject>()) {
      node_obj["sensors"].to<JsonObject>();
    }
  }

  // Note: we do not delete stale nodes here; they simply age out visually.
  GuiRequestRender();
}


}  // namespace

// Arduino entry points (root)
void setup() {
  Serial.begin(115200);
  delay(200);

  g_last_seen.clear();
  g_labels.clear();
  EnsureDocuments();
  LoadLabels();
  LoadDisplayUnits();

  DisplayInit();
  GuiInit();
  GuiUpdateNetwork(0);
  GuiRequestRender();

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &user_scheduler, MESH_PORT);
  mesh.setRoot(true);
  mesh.setContainsRoot(true);
  mesh.onReceive(&OnReceiveRoot);
  mesh.onChangedConnections(&OnConnectionsChangedRoot);

  g_task_announce.set(
      TASK_IMMEDIATE,
      TASK_FOREVER,
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
