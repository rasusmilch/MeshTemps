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
}

#if MESH_IS_ROOT
// ===================== ROOT (always) =====================
#include <Preferences.h>
Preferences prefs;

// Last-seen data and labels (persisted).
JsonDocument lastSeen;
JsonDocument labels;

Task taskAnnounce;

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

  JsonObject sn = node["sensors"].to<JsonObject>();
  JsonArray  arr = d["sensors"].as<JsonArray>();
  DLOG("  nodeId=%u bus_gpio=%d sensors=%d\n", nodeId, gpio, arr.size());

  for (JsonObject s : arr) {
    const char *a = s["addr"] | "";
    if (!a || strlen(a) != 16) continue;
    JsonObject r = sn[a].to<JsonObject>();
    if (s.containsKey("tC")) r["tC"] = s["tC"].as<float>();
    r["last"] = (uint32_t)millis();

    float t = r["tC"] | NAN;
    const char *label = labels["sensors"][a] | "";
    DLOG("    [%s]%s = %s\n",
         a, (label && strlen(label)) ? (String(" \"")+label+"\"").c_str() : "",
         isnan(t) ? "NaN" : String(t,2).c_str());
  }
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

    // split by space
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
    }
    else if (t[0]=="node" && t.size()>=3) {
      labels["nodes"][t[1]] = t[2]; saveLabels(); Serial.println(F("ok"));
    }
    else if (t[0]=="name" && t.size()>=3) {
      labels["sensors"][t[1]] = t[2]; saveLabels(); Serial.println(F("ok"));
    }
    else if (t[0]=="save") { saveLabels(); Serial.println(F("saved")); }
    else if (t[0]=="load") { loadLabels(); Serial.println(F("loaded")); }
    else if (t[0]=="erase"){ eraseLabels(); Serial.println(F("erased")); }
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
