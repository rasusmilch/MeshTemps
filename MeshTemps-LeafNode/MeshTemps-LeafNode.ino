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
// #include "mesh_node.h" // make NodeMetaRecord & MeshNode visible before
// auto-prototypes
#include "serial_console.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <array>
#include <cstring> // for memcmp in NVS verification
#include <math.h>
#include <painlessMesh.h>
#include <vector>

#include "Config.h" // Mesh / IO configuration, addrToHex()
#include <DallasTemperature.h>
#include <OneWire.h>
#include <map>

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

void LogConnections() {
  const size_t peer_count = mesh.getNodeList().size();
  DLOG("Peers: %u\n", static_cast<unsigned int>(peer_count));
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
  const size_t written = preferences.putULong(key, value);
  if (written != sizeof(uint32_t)) {
    NvsLogVerifyFailure(key, "putULong wrote wrong length");
    return false;
  }

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

// Save all calibration coefficients into a single JSON blob under key "cal".
// Now uses:
//   - NVS read-before-write compare (skip write if unchanged)
//   - NvsPutStringVerified() for write + read-back verification.
void SaveAllCalibration() {
  // Open NVS namespace for write.
  if (!g_leaf_prefs.begin("leafcal", /*readOnly=*/false)) {
    Serial.println(F("CAL NVS: begin(\"leafcal\", false) FAILED"));
    return;
  }

  // Build JSON array from current RAM entries.
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto &entry : g_cal_entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["addr"] = entry.addr;
    obj["a1"] = entry.coeff.a1;
    obj["a0"] = entry.coeff.a0;
  }

  String json;
  serializeJson(doc, json);

  const size_t entry_count = g_cal_entries.size();

  // If there are no entries, clear the key instead of writing "[]".
  if (entry_count == 0) {
    // Remove existing key (if any) and verify.
    if (!NvsRemoveKeyVerified(g_leaf_prefs, "cal")) {
      Serial.println(F("CAL NVS: remove(\"cal\") FAILED"));
    } else {
      Serial.println(F("CAL NVS: cleared 'cal' key (0 entries)"));
    }
    g_leaf_prefs.end();
    return;
  }

  // Read current stored value so we can skip redundant writes.
  const String existing = g_leaf_prefs.getString("cal", "");

  if (existing == json) {
    Serial.printf("CAL NVS: no change (%u entr%s, %u bytes) – skipping write\n",
                  static_cast<unsigned>(entry_count),
                  (entry_count == 1) ? "y" : "ies",
                  static_cast<unsigned>(json.length()));
    g_leaf_prefs.end();
    return;
  }

  // Write + verify using shared helper.
  if (!NvsPutStringVerified(g_leaf_prefs, "cal", json)) {
    Serial.printf("CAL NVS: NvsPutStringVerified(\"cal\") FAILED "
                  "(%u entr%s, %u bytes)\n",
                  static_cast<unsigned>(entry_count),
                  (entry_count == 1) ? "y" : "ies",
                  static_cast<unsigned>(json.length()));
    g_leaf_prefs.end();
    return;
  }

  Serial.printf("CAL NVS: saved %u entr%s (%u bytes) to NVS\n",
                static_cast<unsigned>(entry_count),
                (entry_count == 1) ? "y" : "ies",
                static_cast<unsigned>(json.length()));

  g_leaf_prefs.end();
}

// Load all calibration coefficients from the single "cal" JSON blob.
void LoadAllCalibration() {
  g_cal_entries.clear();

  if (!g_leaf_prefs.begin("leafcal", /*readOnly=*/true)) {
    Serial.println(F("CAL NVS: begin(\"leafcal\", true) FAILED"));
    return;
  }

  const String json = g_leaf_prefs.getString("cal", "");

  if (json.isEmpty()) {
    Serial.println(F("CAL NVS: no 'cal' blob found (key missing or empty); "
                     "node running uncalibrated until you solve+save"));
    g_leaf_prefs.end();
    return;
  }

  Serial.printf("CAL NVS: found 'cal' blob (%u bytes), parsing...\n",
                static_cast<unsigned>(json.length()));

  JsonDocument doc;
  const auto err = deserializeJson(doc, json);
  if (err != DeserializationError::Ok) {
    Serial.printf("CAL load: JSON parse error: %s\n", err.c_str());
    g_leaf_prefs.end();
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    const char *addr_cstr = obj["addr"] | nullptr;
    if (addr_cstr == nullptr || strlen(addr_cstr) != 16) {
      Serial.println(F("CAL load: skipping entry with invalid addr"));
      continue;
    }

    Coeff coeff;
    coeff.a1 = obj["a1"] | 1.0f;
    coeff.a0 = obj["a0"] | 0.0f;

    SetCal(String(addr_cstr), coeff);
  }

  const size_t count = g_cal_entries.size();
  Serial.printf("CAL load: loaded %u calibration entr%s from NVS\n",
                static_cast<unsigned>(count), (count == 1) ? "y" : "ies");

  g_leaf_prefs.end();
}

// Clear one sensor's calibration and resave the blob.
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
                  "  cal save | cal load\n"
                  "  cal status"));
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
    if (g_cal_entries.empty()) {
      out.println(F("cal list: no calibration entries in RAM "
                    "(calibrate or run 'cal load')"));
      return;
    }

    for (const auto &e : g_cal_entries) {
      const Coeff &c = e.coeff;
      out.printf("%s : a1=%.6f a0=%.6f\n", e.addr.c_str(),
                 static_cast<double>(c.a1), static_cast<double>(c.a0));
    }
    return;
  }

  if (sub == "show") {
    if (g_cal_entries.empty()) {
      out.println(F("cal show: no calibration entries in RAM "
                    "(calibrate or run 'cal load')"));
      return;
    }

    if (argc >= 3) {
      const int idx = FindCalIndex(argv[2]);
      if (idx >= 0) {
        const Coeff &c = g_cal_entries[static_cast<size_t>(idx)].coeff;
        out.printf("%s : a1=%.6f a0=%.6f\n", argv[2].c_str(),
                   static_cast<double>(c.a1), static_cast<double>(c.a0));
      } else {
        out.printf("cal show %s: not found\n", argv[2].c_str());
      }
    } else {
      for (const auto &e : g_cal_entries) {
        const Coeff &c = e.coeff;
        out.printf("%s : a1=%.6f a0=%.6f\n", e.addr.c_str(),
                   static_cast<double>(c.a1), static_cast<double>(c.a0));
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
    const size_t count = g_cal_entries.size();
    out.printf("cal save: persisted %u calibration entr%s to NVS\n",
               static_cast<unsigned>(count), (count == 1) ? "y" : "ies");
    return;
  }

  if (sub == "load") {
    LoadAllCalibration();
    const size_t count = g_cal_entries.size();
    out.printf("cal load: loaded %u calibration entr%s from NVS\n",
               static_cast<unsigned>(count), (count == 1) ? "y" : "ies");
    if (count == 0) {
      out.println(
          F("  (no stored calibration found; node is running uncalibrated)"));
    }
    return;
  }

  if (sub == "status") {
    out.println(F("cal status:"));

    // 1) Attached sensors and their calibration state.
    if (g_devices.empty()) {
      out.println(F("  attached sensors: (none)"));
    } else {
      out.println(F("  attached sensors:"));
      for (const auto &addr : g_devices) {
        const String addr16 = addrToHex(addr.data());
        const int idx = FindCalIndex(addr16);
        if (idx >= 0) {
          const Coeff &c = g_cal_entries[static_cast<size_t>(idx)].coeff;
          const bool ident = IsIdentity(c);
          out.printf("    %s : CAL a1=%.6f a0=%.6f%s\n", addr16.c_str(),
                     static_cast<double>(c.a1), static_cast<double>(c.a0),
                     ident ? " (identity)" : "");
        } else {
          out.printf("    %s : NO CAL\n", addr16.c_str());
        }
      }
    }

    // 2) Calibration entries whose sensor is not currently attached.
    bool have_orphans = false;
    for (const auto &e : g_cal_entries) {
      bool attached = false;
      for (const auto &addr : g_devices) {
        const String addr16 = addrToHex(addr.data());
        if (e.addr.equalsIgnoreCase(addr16)) {
          attached = true;
          break;
        }
      }
      if (!attached) {
        if (!have_orphans) {
          out.println(F("  stored-only entries (no attached sensor):"));
          have_orphans = true;
        }
        const Coeff &c = e.coeff;
        out.printf("    %s : CAL a1=%.6f a0=%.6f\n", e.addr.c_str(),
                   static_cast<double>(c.a1), static_cast<double>(c.a0));
      }
    }

    if (!have_orphans && !g_cal_entries.empty()) {
      out.println(F("  (no stored-only entries; all calibration entries match "
                    "attached sensors)"));
    }

    if (g_devices.empty() && g_cal_entries.empty()) {
      out.println(
          F("  (no sensors and no calibration entries; node is uncalibrated)"));
    }

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
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &user_scheduler, MESH_PORT, WIFI_AP_STA,
            MESH_CHANNEL, MESH_HIDDEN);
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
