#include <Arduino.h>
#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <vector>
#include "Config.h"

Scheduler     userScheduler;
painlessMesh  mesh;

static bool g_debug = true;  // runtime-toggle via serial
#define DLOG(fmt, ...) do { if (g_debug) Serial.printf((fmt), ##__VA_ARGS__); } while (0)

static void logConns() {
  DLOG("Peers: %u\n", mesh.getNodeList().size());
#if MESH_IS_ROOT
  GUI_UpdateNetwork(mesh.getNodeList().size());
  GUI_RequestRender();
#endif
}

#if MESH_IS_ROOT
// ===================== ROOT (always) =====================
#include <Preferences.h>
#include <lvgl.h>

Preferences prefs;

// Last-seen data and labels (persisted).
JsonDocument lastSeen;
JsonDocument labels;

Task taskAnnounce;

// ---------------- LVGL GUI (root) ----------------
static lv_obj_t* ui_lbl_title = nullptr;
static lv_obj_t* ui_lbl_peers = nullptr;
static lv_obj_t* ui_table     = nullptr;
static volatile bool guiDirty = false;

static void GUI_RebuildTable();  // forward

static void ensureDocs() {
  if (!lastSeen["nodes"].is<JsonObject>()) lastSeen["nodes"].to<JsonObject>();
  if (!labels["nodes"].is<JsonObject>())   labels["nodes"].to<JsonObject>();
  if (!labels["sensors"].is<JsonObject>()) labels["sensors"].to<JsonObject>();
}

static void loadLabels() {
  ensureDocs();
  prefs.begin("meshroot", true);
  String s = prefs.getString("labels", "");
  prefs.end();
  if (s.length()) {
    if (deserializeJson(labels, s)) { labels.clear(); ensureDocs(); }
  }
}

static void saveLabels() {
  ensureDocs();
  String s; serializeJson(labels, s);
  prefs.begin("meshroot", false);
  prefs.putString("labels", s);
  prefs.end();
}

static void eraseLabels() {
  labels.clear(); ensureDocs(); saveLabels();
}

// ---------- LVGL helpers ----------
static void GUI_Init() {
  // Build a simple layout: title at top-left, peers at top-right, table fills the rest.
  lv_obj_t* scr = lv_scr_act();

  ui_lbl_title = lv_label_create(scr);
  lv_label_set_text(ui_lbl_title, "MeshTemps — ROOT");
  lv_obj_align(ui_lbl_title, LV_ALIGN_TOP_LEFT, 6, 4);

  ui_lbl_peers = lv_label_create(scr);
  lv_label_set_text(ui_lbl_peers, "Peers: 0");
  lv_obj_align(ui_lbl_peers, LV_ALIGN_TOP_RIGHT, -6, 4);

  ui_table = lv_table_create(scr);
  lv_obj_set_size(ui_table, lv_pct(100), lv_pct(100));
  lv_obj_align(ui_table, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_table_set_col_cnt(ui_table, 6);
  lv_table_set_row_cnt(ui_table, 1);
  lv_table_set_cell_value(ui_table, 0, 0, "Node");
  lv_table_set_cell_value(ui_table, 0, 1, "Location");
  lv_table_set_cell_value(ui_table, 0, 2, "Addr");
  lv_table_set_cell_value(ui_table, 0, 3, "Label");
  lv_table_set_cell_value(ui_table, 0, 4, "Temp \xC2\xB0""C"); // °C
  lv_table_set_cell_value(ui_table, 0, 5, "Age(s)");

  guiDirty = true;  // first paint
}

static void GUI_UpdateNetwork(size_t peers) {
  if (!ui_lbl_peers) return;
  static char buf[32];
  snprintf(buf, sizeof(buf), "Peers: %u", (unsigned)peers);
  lv_label_set_text(ui_lbl_peers, buf);
}

static void GUI_UpdateNodeSummary(const char* /*nodeIdStr*/, int /*busGpio*/, uint32_t /*lastMs*/) {
  // No per-node widgets; we rebuild a single table. Mark dirty only.
  guiDirty = true;
}

static void GUI_UpdateSensorRow(const char* /*nodeIdStr*/, const char* /*addr16*/, float /*tempC*/, const char* /*labelOrNull*/, uint32_t /*lastMs*/) {
  // Table is rebuilt from the model. Mark dirty only.
  guiDirty = true;
}

static void GUI_RequestRender() {
  guiDirty = true;
}

static void GUI_RebuildTable() {
  if (!ui_table) return;

  // Row 0 = header
  uint16_t row = 1;
  lv_table_set_row_cnt(ui_table, 1);

  const uint32_t now = millis();
  ensureDocs();
  JsonObject nodes = lastSeen["nodes"].as<JsonObject>();
  for (JsonPair kv : nodes) {
    String nodeIdStr = kv.key().c_str();
    JsonObject n = kv.value();
    String loc = labels["nodes"][nodeIdStr] | "";
    JsonObject sn = n["sensors"].as<JsonObject>();

    for (JsonPair sv : sn) {
      String addr = sv.key().c_str();
      float  t    = sv.value()["tC"] | NAN;
      uint32_t last = sv.value()["last"] | 0;
      String lab = labels["sensors"][addr] | "";

      uint32_t age_s = (last <= now) ? ((now - last)/1000U) : 0;

      lv_table_set_row_cnt(ui_table, row + 1);
      lv_table_set_cell_value(ui_table, row, 0, nodeIdStr.c_str());
      lv_table_set_cell_value(ui_table, row, 1, loc.c_str());
      lv_table_set_cell_value(ui_table, row, 2, addr.c_str());
      lv_table_set_cell_value(ui_table, row, 3, lab.c_str());

      char tmp[16];
      if (isnan(t)) strncpy(tmp, "--", sizeof(tmp));
      else { snprintf(tmp, sizeof(tmp), "%.2f", t); }
      lv_table_set_cell_value(ui_table, row, 4, tmp);

      char age[12]; snprintf(age, sizeof(age), "%lu", (unsigned long)age_s);
      lv_table_set_cell_value(ui_table, row, 5, age);

      row++;
    }
  }
}

static void GUI_Pump() {
  // Always let LVGL process its timers
  lv_timer_handler();

  // Rebuild table when marked dirty (rate-limited)
  static uint32_t lastBuild = 0;
  if (guiDirty && (millis() - lastBuild >= 50)) {
    lastBuild = millis();
    guiDirty = false;
    GUI_RebuildTable();
  }
}
// -------------------------------------------------

void onReceive(uint32_t from, String &msg) {
  DLOG("[ROOT RX] from=%u len=%u: %s\n", from, msg.length(), msg.c_str());

  JsonDocument d;
  if (deserializeJson(d, msg)) {
    DLOG("  ! JSON parse error\n");
    return;
  }

  const char *type = d["type"] | "temps";
  if (strcmp(type, "temps") != 0) {
    DLOG("  ! ignoring type '%s'\n", type);
    return;
  }

  ensureDocs();

  uint32_t nodeId = d["nodeId"] | from;
  int      gpio   = d["busGpio"] | -1;

  JsonObject nodes = lastSeen["nodes"].to<JsonObject>();
  JsonObject node  = nodes[String(nodeId)].to<JsonObject>();
  node["busGpio"]  = gpio;
  node["last"]     = (uint32_t)millis();

  // LVGL model-hook (per-node)
  String nodeIdStr = String(nodeId);
  GUI_UpdateNodeSummary(nodeIdStr.c_str(), gpio, (uint32_t)millis());

  JsonObject sn = node["sensors"].to<JsonObject>();
  JsonArray  arr = d["sensors"].as<JsonArray>();
  DLOG("  nodeId=%u bus_gpio=%d sensors=%d\n", nodeId, gpio, arr.size());

  for (JsonObject s : arr) {
    const char *a = s["addr"] | "";
    if (!a || strlen(a) != 16) continue;
    JsonObject r = sn[a].to<JsonObject>();

    // ArduinoJson v7-safe: check via is<T>()
    if (s["tC"].is<float>()) r["tC"] = s["tC"].as<float>();
    r["last"] = (uint32_t)millis();

    float t = r["tC"] | NAN;
    const char *label = labels["sensors"][a] | "";
    DLOG("    [%s]%s = %s\n",
         a, (label && strlen(label)) ? (String(" \"")+label+"\"").c_str() : "",
         isnan(t) ? "NaN" : String(t,2).c_str());

    // LVGL model-hook (per-sensor)
    GUI_UpdateSensorRow(nodeIdStr.c_str(), a, t,
                        (label && strlen(label)) ? label : nullptr,
                        (uint32_t)millis());
  }

  GUI_RequestRender();
}

void onChangedConnections() { logConns(); }

// Root announces itself periodically so leaves address it directly
static void announceRoot() {
  JsonDocument d;
  d["type"]   = "root_announce";
  d["rootId"] = mesh.getNodeId();
  String m; serializeJson(d, m);
  DLOG("[ROOT TX announce] %s\n", m.c_str());
  mesh.sendBroadcast(m);
}

static void processConsole() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { line += c; if (line.length() > 256) line.remove(0, line.length()-256); continue; }
    line.trim();
    if (!line.length()) { Serial.println("ok"); line = ""; continue; }

    std::vector<String> t; int s = 0;
    for (int i=0;i<(int)line.length();++i) if (isspace((unsigned char)line[i])) { if (i>s) t.push_back(line.substring(s,i)); s=i+1; }
    if (s < (int)line.length()) t.push_back(line.substring(s));

    auto help = [](){
      Serial.println(F("Commands (root):"));
      Serial.println(F("  ls"));
      Serial.println(F("  node <id> <location>"));
      Serial.println(F("  name <addr16> <label>"));
      Serial.println(F("  save | load | erase"));
      Serial.println(F("  debug on|off"));
    };

    if (t[0]=="help"||t[0]=="?") help();
    else if (t[0]=="ls") {
      ensureDocs();
      JsonObject nodes = lastSeen["nodes"].as<JsonObject>();
      for (JsonPair kv : nodes) {
        String id = kv.key().c_str();
        JsonObject n = kv.value();
        String loc = labels["nodes"][id] | "";
        Serial.printf("node %s%s:\n", id.c_str(), loc.length()? (" \""+loc+"\"").c_str() : "");
        JsonObject sn = n["sensors"].as<JsonObject>();
        for (JsonPair sv : sn) {
          String a = sv.key().c_str();
          float  v = sv.value()["tC"] | NAN;
          String nm= labels["sensors"][a] | "";
          Serial.printf("  %s%s : %s\n", a.c_str(), nm.length()? (" \""+nm+"\"").c_str():"", isnan(v)?"NaN":String(v,2).c_str());
        }
      }
      GUI_RequestRender();
    }
    else if (t[0]=="node" && t.size()>=3) { labels["nodes"][t[1]] = t[2]; saveLabels(); Serial.println(F("ok")); GUI_RequestRender(); }
    else if (t[0]=="name" && t.size()>=3) { labels["sensors"][t[1]] = t[2]; saveLabels(); Serial.println(F("ok")); GUI_RequestRender(); }
    else if (t[0]=="save") { saveLabels(); Serial.println(F("saved")); }
    else if (t[0]=="load") { loadLabels(); Serial.println(F("loaded")); GUI_RequestRender(); }
    else if (t[0]=="erase"){ eraseLabels(); Serial.println(F("erased")); GUI_RequestRender(); }
    else if (t[0]=="debug" && t.size()>=2) {
      g_debug = (t[1]=="on");
      Serial.printf("debug=%s\n", g_debug?"on":"off");
    }
    else { Serial.println(F("ERR (help)")); }

    line = "";
  }
}

void setup() {
  Serial.begin(115200); delay(200);

  lastSeen.clear(); labels.clear(); ensureDocs(); loadLabels();

  // Build LVGL widgets (assumes lv_init() + display driver already done)
  GUI_Init();
  GUI_UpdateNetwork(0);
  GUI_RequestRender();

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.setRoot(true);             // <- always root
  mesh.setContainsRoot(true);
  mesh.onReceive(&onReceive);
  mesh.onChangedConnections(&onChangedConnections);

  taskAnnounce.set(TASK_IMMEDIATE, TASK_FOREVER, [](){
    announceRoot();
    taskAnnounce.delay(ROOT_ANNOUNCE_MS);
  });
  userScheduler.addTask(taskAnnounce);
  taskAnnounce.enable();

  Serial.println(F("ROOT ready. Type 'help'."));
}

void loop() {
  mesh.update();
  processConsole();
  GUI_Pump();     // <-- LVGL timer + deferred redraw
}

#else
// ===================== LEAF (never promotes) =====================
#include <OneWire.h>
#include <DallasTemperature.h>
#include <array>
#include <vector>

using Address = std::array<uint8_t, 8>;
std::vector<Address> devs;

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds(&oneWire);

static uint32_t g_rootId = 0;
static uint32_t g_rootLastSeenMs = 0;

Task taskSend;

inline String addrToHex(const uint8_t *addr) {
  static const char *H = "0123456789ABCDEF";
  char s[17] = {0};
  for (int i = 0; i < 8; ++i) { s[i*2] = H[addr[i]>>4]; s[i*2+1] = H[addr[i]&0xF]; }
  return String(s);
}

static void scanSensors() {
  devs.clear();
  ds.begin();
  DeviceAddress raw;
  int n = ds.getDeviceCount();
  DLOG("[LEAF] scanning DS18B20 on GPIO %d: found=%d\n", ONEWIRE_PIN, n);
  for (int i = 0; i < n; ++i) {
    if (ds.getAddress(raw, i)) {
      devs.emplace_back();
      memcpy(devs.back().data(), raw, 8);
      DLOG("  addr[%d]=%s\n", i, addrToHex(devs.back().data()).c_str());
    }
  }
  if (devs.empty()) DLOG("  (no sensors)\n");
}

void onReceive(uint32_t from, String &msg) {
  DLOG("[LEAF RX] from=%u len=%u: %s\n", from, msg.length(), msg.c_str());
  JsonDocument d;
  if (deserializeJson(d, msg)) {
    DLOG("  ! JSON parse error\n");
    return;
  }
  const char *type = d["type"] | "";
  if (strcmp(type, "root_announce") == 0) {
    uint32_t rid = d["rootId"] | 0;
    if (rid) {
      g_rootId = rid; g_rootLastSeenMs = millis();
      DLOG("  root_announce: rootId=%u\n", g_rootId);
    }
  }
}

void onChangedConnections() { logConns(); }

static void sendTemperatures() {
  ds.requestTemperatures();

  // Build payload
  JsonDocument doc;
  doc["type"]     = "temps";
  doc["nodeId"]   = mesh.getNodeId();
  doc["busGpio"]  = ONEWIRE_PIN;
  doc["uptimeMs"] = (uint32_t)millis();

  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (size_t i = 0; i < devs.size(); ++i) {
    const auto &a = devs[i];
    float t = ds.getTempC((uint8_t*)a.data());
    JsonObject s = arr.add<JsonObject>();
    s["addr"] = addrToHex(a.data());
    if (!isnan(t) && t > -100.0f) s["tC"] = t; // guard against DEVICE_DISCONNECTED_C

    // Per-sensor debug line
    if (!isnan(t) && t > -100.0f)
      DLOG("[LEAF MEAS] %s = %.2f C\n", addrToHex(a.data()).c_str(), t);
    else
      DLOG("[LEAF MEAS] %s = (disconnected)\n", addrToHex(a.data()).c_str());
  }

  String msg; serializeJson(doc, msg);
  bool unicast = (g_rootId && (millis() - g_rootLastSeenMs) < 15000);

  DLOG("[LEAF TX %s] %s\n", unicast ? "unicast" : "bcast", msg.c_str());

  if (unicast) mesh.sendSingle(g_rootId, msg);
  else         mesh.sendBroadcast(msg);
}

// Minimal leaf console: debug toggle + rescan + immediate send
static void processConsoleLeaf() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c=='\r') continue;
    if (c!='\n') { line += c; if (line.length()>256) line.remove(0,line.length()-256); continue; }
    line.trim();
    if (!line.length()) { Serial.println("ok"); line=""; continue; }

    std::vector<String> t; int s=0;
    for (int i=0;i<(int)line.length();++i) if (isspace((unsigned char)line[i])) { if (i>s) t.push_back(line.substring(s,i)); s=i+1; }
    if (s<(int)line.length()) t.push_back(line.substring(s));

    if (t[0]=="debug" && t.size()>=2) {
      g_debug=(t[1]=="on"); Serial.printf("debug=%s\n", g_debug?"on":"off");
    } else if (t[0]=="scan") {
      scanSensors(); Serial.println("ok");
    } else if (t[0]=="sendnow") {
      sendTemperatures(); Serial.println("ok");
    } else if (t[0]=="help"||t[0]=="?") {
      Serial.println(F("Commands (leaf): debug on|off | scan | sendnow"));
    } else {
      Serial.println(F("ERR (help)"));
    }
    line="";
  }
}

void setup() {
  Serial.begin(115200); delay(1000);

  scanSensors();

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.setContainsRoot(true);
  mesh.onReceive(&onReceive);
  mesh.onChangedConnections(&onChangedConnections);

  taskSend.set(TASK_IMMEDIATE, TASK_FOREVER, [](){
    sendTemperatures();
    taskSend.delay(SEND_PERIOD_MS);
  });
  userScheduler.addTask(taskSend);
  taskSend.enable();

  Serial.println(F("LEAF ready. Type 'help'."));
}

void loop() {
  mesh.update();
  processConsoleLeaf();
}

#endif
