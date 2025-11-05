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
  lv_table_set_col_cnt(ui_table, 7);   // +1: corrected flag
  lv_table_set_row_cnt(ui_table, 1);
  lv_table_set_cell_value(ui_table, 0, 0, "Node");
  lv_table_set_cell_value(ui_table, 0, 1, "Location");
  lv_table_set_cell_value(ui_table, 0, 2, "Addr");
  lv_table_set_cell_value(ui_table, 0, 3, "Label");
  lv_table_set_cell_value(ui_table, 0, 4, "Temp \xC2\xB0""C"); // °C
  lv_table_set_cell_value(ui_table, 0, 5, "Age(s)");
  lv_table_set_cell_value(ui_table, 0, 6, "C?");

  guiDirty = true;  // first paint
}

static void GUI_UpdateNetwork(size_t peers) {
  if (!ui_lbl_peers) return;
  static char buf[32];
  snprintf(buf, sizeof(buf), "Peers: %u", (unsigned)peers);
  lv_label_set_text(ui_lbl_peers, buf);
}

static void GUI_UpdateNodeSummary(const char* /*nodeIdStr*/, int /*busGpio*/, uint32_t /*lastMs*/) {
  guiDirty = true;
}

static void GUI_UpdateSensorRow(const char* /*nodeIdStr*/, const char* /*addr16*/, float /*tempC*/, const char* /*labelOrNull*/, uint32_t /*lastMs*/) {
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
      bool corr = sv.value()["corr"] | false;
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

      lv_table_set_cell_value(ui_table, row, 6, corr ? "Y" : "N");

      row++;
    }
  }
}

static void GUI_Pump() {
  lv_timer_handler();
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

  String nodeIdStr = String(nodeId);
  GUI_UpdateNodeSummary(nodeIdStr.c_str(), gpio, (uint32_t)millis());

  JsonObject sn = node["sensors"].to<JsonObject>();
  JsonArray  arr = d["sensors"].as<JsonArray>();
  DLOG("  nodeId=%u bus_gpio=%d sensors=%d\n", nodeId, gpio, arr.size());

  for (JsonObject s : arr) {
    const char *a = s["addr"] | "";
    if (!a || strlen(a) != 16) continue;
    JsonObject r = sn[a].to<JsonObject>();

    if (s["tC"].is<float>()) r["tC"] = s["tC"].as<float>();
    r["corr"] = s["corr"] | false;
    r["last"] = (uint32_t)millis();

    float t = r["tC"] | NAN;
    const char *label = labels["sensors"][a] | "";
    DLOG("    [%s]%s = %s (corr=%d)\n",
         a, (label && strlen(label)) ? (String(" \"")+label+"\"").c_str() : "",
         isnan(t) ? "NaN" : String(t,2).c_str(),
         (int)(r["corr"] | false));

    GUI_UpdateSensorRow(nodeIdStr.c_str(), a, t,
                        (label && strlen(label)) ? label : nullptr,
                        (uint32_t)millis());
  }

  GUI_RequestRender();
}

void onChangedConnections() { logConns(); }

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
          bool   c = sv.value()["corr"] | false;
          String nm= labels["sensors"][a] | "";
          Serial.printf("  %s%s : %s (corr=%c)\n", a.c_str(), nm.length()? (" \""+nm+"\"").c_str():"", isnan(v)?"NaN":String(v,2).c_str(), c?'Y':'N');
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
  GUI_Pump();     // LVGL timer + deferred redraw
}

#else
// ===================== LEAF (never promotes) =====================
#include <OneWire.h>
#include <DallasTemperature.h>
#include <array>
#include <vector>
#include <Preferences.h>
#include <math.h>

using Address = std::array<uint8_t, 8>;
std::vector<Address> devs;

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds(&oneWire);

static uint32_t g_rootId = 0;
static uint32_t g_rootLastSeenMs = 0;

Task taskSend;

// ---------- Utilities ----------
inline String addrToHex(const uint8_t *addr) {
  static const char *H = "0123456789ABCDEF";
  char s[17] = {0};
  for (int i = 0; i < 8; ++i) { s[i*2] = H[addr[i]>>4]; s[i*2+1] = H[addr[i]&0xF]; }
  return String(s);
}
static bool addrFromHex(const String& hex, uint8_t out[8]) {
  if (hex.length() != 16) return false;
  auto hexv = [](char c)->int{
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
  };
  for (int i=0;i<8;++i) {
    int hi = hexv(hex[i*2]), lo = hexv(hex[i*2+1]);
    if (hi<0||lo<0) return false;
    out[i] = (uint8_t)((hi<<4)|lo);
  }
  return true;
}
static bool findDeviceByAddr(const String& addr16, Address& out) {
  for (auto &a : devs) {
    if (addr16.equalsIgnoreCase(addrToHex(a.data()))) { out = a; return true; }
  }
  return false;
}

// ---------- Calibration store (Preferences) ----------
// Linear model: Tcorr = a1 * Traw + a0
struct Coeff { float a1, a0; }; 
struct CalEntry { String addr; Coeff c; };
static std::vector<CalEntry> g_cal;
Preferences g_prefs;

static int calFind(const String& addr) {
  for (size_t i=0;i<g_cal.size();++i) if (g_cal[i].addr.equalsIgnoreCase(addr)) return (int)i;
  return -1;
}
static void calSet(const String& addr, const Coeff& c) {
  int i = calFind(addr);
  if (i>=0) g_cal[i].c = c;
  else g_cal.push_back({addr, c});
}
static bool isIdentity(const Coeff& c) {
  return (fabsf(c.a1 - 1.0f) < 1e-6f) && (fabsf(c.a0) < 1e-6f);
}
static float applyCorr(float tRaw, const String& addr16, bool* outCorr) {
  int i = calFind(addr16);
  if (i<0) { if (outCorr) *outCorr=false; return tRaw; }
  const Coeff &c = g_cal[i].c;
  float tC = c.a1 * tRaw + c.a0;
  if (outCorr) *outCorr = !isIdentity(c);
  return tC;
}
static void calSaveAll() {
  // Persist index (JSON array) + each coeff as CSV "a1,a0" in key "c_<addr>"
  g_prefs.begin("leafcal", false);
  JsonDocument idx;
  JsonArray a = idx.to<JsonArray>();
  for (auto& e : g_cal) a.add(e.addr);
  String s; serializeJson(idx, s);
  g_prefs.putString("index", s);
  for (auto& e : g_cal) {
    char key[32]; snprintf(key, sizeof(key), "c_%s", e.addr.c_str());
    char val[64];
    snprintf(val, sizeof(val), "%.8f,%.8f", (double)e.c.a1, (double)e.c.a0);
    g_prefs.putString(key, val);
  }
  g_prefs.end();
}
static void calLoadAll() {
  g_cal.clear();
  g_prefs.begin("leafcal", true);
  String s = g_prefs.getString("index", "");
  if (!s.isEmpty()) {
    JsonDocument idx;
    if (!deserializeJson(idx, s)) {
      for (JsonVariant v : idx.as<JsonArray>()) {
        String addr = v.as<const char*>();
        char key[32]; snprintf(key, sizeof(key), "c_%s", addr.c_str());
        String val = g_prefs.getString(key, "");
        if (!val.isEmpty()) {
          // Back-compat: accept "a1,a0" or legacy "a2,a1,a0"
          Coeff c{1.0f, 0.0f};
          float a1=1, a0=0, a2_legacy=0;
          int n = sscanf(val.c_str(), "%f,%f", &a1, &a0);
          if (n != 2) {
            // try legacy 3-field
            n = sscanf(val.c_str(), "%f,%f,%f", &a2_legacy, &a1, &a0);
          }
          if (n >= 2) { c.a1 = a1; c.a0 = a0; calSet(addr, c); }
        }
      }
    }
  }
  g_prefs.end();
}
static void calClear(const String& addr) {
  int i = calFind(addr);
  if (i>=0) g_cal.erase(g_cal.begin()+i);
  calSaveAll();
}

// ---------- Live calibration session ----------
enum CalStage { CAL_IDLE=0, CAL_ICE, CAL_BOIL };
static struct {
  CalStage stage = CAL_IDLE;
  String addr;
  float lastRaw = NAN;
  bool  haveIce=false, haveBoil=false;
  float rawIce=0, actIce=0, rawBoil=0, actBoil=0;
} g_calSess;

Task taskCal;

static void calTaskFn() {
  if (g_calSess.stage==CAL_IDLE || g_calSess.addr.length()!=16) {
    taskCal.disable(); return;
  }
  Address a;
  if (!findDeviceByAddr(g_calSess.addr, a)) {
    Serial.println(F("CAL ERROR: sensor not found; aborting"));
    g_calSess.stage = CAL_IDLE;
    taskCal.disable();
    return;
  }
  ds.requestTemperatures();
  float t = ds.getTempC((uint8_t*)a.data());
  g_calSess.lastRaw = t;
  const char* st = (g_calSess.stage==CAL_ICE) ? "ICE" : "BOIL";
  if (!isnan(t)) Serial.printf("CAL %s %s : raw=%.3f C\n", st, g_calSess.addr.c_str(), t);
  taskCal.delay(1000);
}

static void calStart(const String& addr16, CalStage stg) {
  if (addr16.length()!=16) { Serial.println(F("ERR addr16")); return; }
  g_calSess.stage = stg;
  g_calSess.addr = addr16;
  g_calSess.lastRaw = NAN;
  if (stg==CAL_ICE) { g_calSess.haveIce=false; }
  if (stg==CAL_BOIL){ g_calSess.haveBoil=false; }
  Serial.printf("CAL %s started for %s\n", stg==CAL_ICE?"ICE":"BOIL", addr16.c_str());
  taskCal.enableIfNot();
}

// Solve linear fit from two points and save
static void calSolveAndSave() {
  if (!g_calSess.haveIce || !g_calSess.haveBoil) { Serial.println(F("CAL not complete")); return; }
  float x1=g_calSess.rawIce, y1=g_calSess.actIce;
  float x2=g_calSess.rawBoil, y2=g_calSess.actBoil;
  if (fabsf(x2-x1) < 1e-4f) { Serial.println(F("CAL ERROR: identical raw points")); return; }
  Coeff c;
  c.a1 = (y2 - y1)/(x2 - x1);
  c.a0 = y1 - c.a1*x1;

  calSet(g_calSess.addr, c);
  calSaveAll();

  Serial.printf("CAL SAVED %s : a1=%.6f a0=%.6f\n", g_calSess.addr.c_str(), (double)c.a1, (double)c.a0);
  Serial.printf("  check: ice  raw=%.3f -> %.3f (want %.3f)\n", (double)x1, (double)(c.a1*x1 + c.a0), (double)y1);
  Serial.printf("  check: boil raw=%.3f -> %.3f (want %.3f)\n", (double)x2, (double)(c.a1*x2 + c.a0), (double)y2);

  g_calSess = {};
  taskCal.disable();
}

// ---------- Mesh / sensors ----------
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

  JsonDocument doc;
  doc["type"]     = "temps";
  doc["nodeId"]   = mesh.getNodeId();
  doc["busGpio"]  = ONEWIRE_PIN;
  doc["uptimeMs"] = (uint32_t)millis();

  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (size_t i = 0; i < devs.size(); ++i) {
    const auto &a = devs[i];
    float tRaw = ds.getTempC((uint8_t*)a.data());
    String addr16 = addrToHex(a.data());
    bool corr = false;
    float tOut = (!isnan(tRaw) && tRaw > -100.0f) ? applyCorr(tRaw, addr16, &corr) : NAN;

    JsonObject s = arr.add<JsonObject>();
    s["addr"] = addr16;
    if (!isnan(tOut)) s["tC"] = tOut;
    s["corr"] = corr;

    if (!isnan(tRaw) && tRaw > -100.0f) {
      DLOG("[LEAF MEAS] %s raw=%.2f -> out=%.2f (corr=%d)\n", addr16.c_str(), tRaw, tOut, (int)corr);
    } else {
      DLOG("[LEAF MEAS] %s = (disconnected)\n", addr16.c_str());
    }
  }

  String msg; serializeJson(doc, msg);
  bool unicast = (g_rootId && (millis() - g_rootLastSeenMs) < 15000);

  DLOG("[LEAF TX %s] %s\n", unicast ? "unicast" : "bcast", msg.c_str());

  if (unicast) mesh.sendSingle(g_rootId, msg);
  else         mesh.sendBroadcast(msg);
}

// ---------- Leaf console ----------
static void printHelpLeaf() {
  Serial.println(F("Commands (leaf):"));
  Serial.println(F("  debug on|off"));
  Serial.println(F("  scan"));
  Serial.println(F("  sendnow"));
  Serial.println(F("  cal list"));
  Serial.println(F("  cal show <addr16>"));
  Serial.println(F("  cal clear <addr16>"));
  Serial.println(F("  cal ice <addr16>   (prints live raw until 'set <actualC>')"));
  Serial.println(F("  cal boil <addr16>  (prints live raw until 'set <actualC>')"));
  Serial.println(F("  set <actualC>      (while ICE/BOIL active)"));
}

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

    auto bad = [](){ Serial.println(F("ERR (help)")); };

    if (t[0]=="debug" && t.size()>=2) {
      g_debug=(t[1]=="on"); Serial.printf("debug=%s\n", g_debug?"on":"off");
    } else if (t[0]=="scan") {
      scanSensors(); Serial.println("ok");
    } else if (t[0]=="sendnow") {
      sendTemperatures(); Serial.println("ok");
    } else if (t[0]=="help"||t[0]=="?") {
      printHelpLeaf();
    } else if (t[0]=="cal" && t.size()>=2) {
      if (t[1]=="list") {
        for (auto &a : devs) {
          String addr16 = addrToHex(a.data());
          int i = calFind(addr16);
          if (i>=0) {
            auto c = g_cal[i].c;
            Serial.printf("%s : a1=%.6f a0=%.6f\n", addr16.c_str(), (double)c.a1, (double)c.a0);
          } else {
            Serial.printf("%s : (identity)\n", addr16.c_str());
          }
        }
      } else if (t[1]=="show" && t.size()>=3) {
        String addr16=t[2]; int i=calFind(addr16);
        if (i>=0) { 
          auto c=g_cal[i].c; 
          Serial.printf("%s : a1=%.6f a0=%.6f\n", addr16.c_str(), (double)c.a1, (double)c.a0); 
        }
        else  {
          Serial.println(F("(identity)"));
        }
      } else if (t[1]=="clear" && t.size()>=3) {
        calClear(t[2]); Serial.println(F("ok"));
      } else if (t[1]=="ice" && t.size()>=3) {
        String addr16=t[2];
        Address a;
        if (!findDeviceByAddr(addr16, a)) { 
          Serial.println(F("ERR addr not found")); 
        }
        else { 
          calStart(addr16, CAL_ICE); 
          Serial.println(F("Place in ice bath; watch values; enter 'set <actualC>' when settled.")); 
        }
      } else if (t[1]=="boil" && t.size()>=3) {
        String addr16=t[2];
        Address a;
        if (!findDeviceByAddr(addr16, a)) { 
          Serial.println(F("ERR addr not found")); 
        }
        else { 
          calStart(addr16, CAL_BOIL); 
          Serial.println(F("Place in boiling bath; watch values; enter 'set <actualC>' when settled.")); 
        }
      } else {
        bad();
      }
    } else if (t[0]=="set" && t.size()>=2) {
      if (g_calSess.stage==CAL_IDLE || g_calSess.addr.length()!=16) { 
        Serial.println(F("No active calibration.")); 
      }
      else {
        float actual = t[1].toFloat();
        if (isnan(g_calSess.lastRaw)) { Serial.println(F("No raw reading yet; wait a second.")); }
        else if (g_calSess.stage==CAL_ICE) {
          g_calSess.actIce = actual;
          g_calSess.rawIce = g_calSess.lastRaw;
          g_calSess.haveIce = true;
          Serial.printf("ICE locked: raw=%.3f -> actual=%.3f\n", (double)g_calSess.rawIce, (double)g_calSess.actIce);
          Serial.println(F("Now run: cal boil <addr16> ; then 'set <actualC>'"));
          // keep task running for the next stage
        } else if (g_calSess.stage==CAL_BOIL) {
          g_calSess.actBoil = actual;
          g_calSess.rawBoil = g_calSess.lastRaw;
          g_calSess.haveBoil = true;
          Serial.printf("BOIL locked: raw=%.3f -> actual=%.3f\n", (double)g_calSess.rawBoil, (double)g_calSess.actBoil);
          // Finish if both present:
          if (g_calSess.haveIce && g_calSess.haveBoil) calSolveAndSave();
        }
      }
    } else {
      bad();
    }

    line="";
  }
}

void setup() {
  Serial.begin(115200); delay(1000);

  scanSensors();
  calLoadAll();

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.setContainsRoot(true);
  mesh.onReceive(&onReceive);
  mesh.onChangedConnections(&onChangedConnections);

  // periodic sender
  taskSend.set(TASK_IMMEDIATE, TASK_FOREVER, [](){
    sendTemperatures();
    taskSend.delay(SEND_PERIOD_MS);
  });
  userScheduler.addTask(taskSend);
  taskSend.enable();

  // calibration printer task (stays disabled until used)
  taskCal.set(TASK_IMMEDIATE, TASK_FOREVER, calTaskFn);
  userScheduler.addTask(taskCal);

  Serial.println(F("LEAF ready. Type 'help'."));
}

void loop() {
  mesh.update();
  processConsoleLeaf();
}

#endif
