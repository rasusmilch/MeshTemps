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

// Forward declaration so we can use this in the watchdog task.
void MeshWatchdogTaskFn();
static void AdvanceDiscoveryChannel(Print &out);

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

uint32_t g_root_id = 0;
uint32_t g_root_last_seen_ms = 0;

bool g_mesh_initialized = false;

// -----------------------------------------------------------------------------
// Mesh discovery / root tracking (LEAF)
// -----------------------------------------------------------------------------

// Root timeout: if we don't see root for this long, we assume it's gone.
constexpr uint32_t kRootAnnounceMs =
    ROOT_ANNOUNCE_MS; // must match ROOT_ANNOUNCE_MS on root
constexpr uint32_t kRootUnicastWindowMs = 3 * kRootAnnounceMs; // 60 s
constexpr uint32_t kRootTimeoutMs =
    3 * kRootAnnounceMs; // 60 s (as you have now)

// How long we consider a root presence "fresh" (for success in discovery).
constexpr uint32_t kRootPresenceWindowMs = 30000; // 30 s

// Minimum time we stay on a candidate channel during discovery before we
// declare "no root" and tear the mesh down. This should be at least as
// long as kRootTimeoutMs so that a real root has time to send one or more
// root_announce() frames.
constexpr uint32_t kChannelMinDiscoveryMs = kRootTimeoutMs;

// Root probe attempts per channel.
constexpr uint8_t kMaxProbesPerChannel = 3;

// Base delay between probes on a given channel (will exponential-backoff).
constexpr uint32_t kProbeBaseDelayMs = 2000; // 2 s

// When there is *no* mesh with a root on any channel, wait 5–10 minutes
// before doing another discovery scan, randomized to avoid stampedes.
constexpr uint32_t kRescanMinDelayMs = 5UL * 60UL * 1000UL; // 5 minutes
constexpr uint32_t kRescanJitterMs = 5UL * 60UL * 1000UL;   // + up to 5 min

// At most 11 channels (1–11). We keep a compact list of channels where
// we saw MESH_PREFIX.
constexpr uint8_t kMaxMeshChannels = 11;

struct MeshDiscoveryState {
  bool ongoing = false;                    // true while discovery is active
  uint8_t channels[kMaxMeshChannels] = {}; // unique channel list
  uint8_t channel_count = 0;               // how many entries are valid
  uint8_t channel_index = 0;               // current index into channels[]

  uint8_t probes_sent_on_this_channel = 0;
  uint32_t last_probe_ms = 0;
};

MeshDiscoveryState g_discovery;

// Time we started the current discovery channel (for per-channel dwell).
uint32_t g_discovery_channel_start_ms = 0;

// Discovery actions that must be executed from loop(), not from
// TaskScheduler callbacks, to avoid calling mesh.stop() while
// Scheduler::execute() is iterating tasks.
volatile bool g_start_discovery_pending = false;
volatile bool g_advance_discovery_channel_pending = false;

// Next time we are allowed to start a new discovery (ms since boot).
uint32_t g_next_discovery_allowed_ms = 0;

// Number of consecutive discovery cycles that ended with "no root found".
uint8_t g_consecutive_discovery_failures = 0;

// Watchdog task (already declared in your code).
Task g_task_mesh_watchdog;

// Minimum spacing between mesh restarts (avoid thrashing).
constexpr uint32_t kMeshRestartMinIntervalMs = 30000; // 30 s

uint32_t g_last_mesh_restart_ms = 0;

std::vector<Address> g_devices;

OneWire g_one_wire(ONEWIRE_PIN);
DallasTemperature g_ds18(&g_one_wire);

// Per-node transmit sequence number (increments on every temps send).
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

// -----------------------------------------------------------------------------
// Calibration NVS binary blob format (header + entries + CRC32)
// -----------------------------------------------------------------------------

static constexpr uint32_t kCalMagic = 0x43414C31u; // "CAL1"
static constexpr uint16_t kCalBlobVersion = 1;

#pragma pack(push, 1)
struct CalBlobHeader {
  uint32_t magic;       // kCalMagic
  uint16_t version;     // kCalBlobVersion
  uint16_t entry_count; // number of CalBlobEntry records that follow
  uint32_t crc32;       // CRC32 over the entries array only
};

struct CalBlobEntry {
  uint8_t addr[8]; // DS18B20 ROM code bytes
  float a1;
  float a0;
};
#pragma pack(pop)

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
  if (written == 0) {
    NvsLogVerifyFailure(key, "putULong wrote zero bytes");
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

// Simple bitwise CRC32 (polynomial 0xEDB88320, initial 0xFFFFFFFF, final ~crc).
static uint32_t Crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

// Parse a 16-char hex address string (e.g. "28175E57000000A9") into 8 bytes.
static int HexNibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return -1;
}

static bool ParseAddrHex16(const String &hex16, uint8_t out[8]) {
  if (hex16.length() != 16) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    const int hi = HexNibble(hex16[2 * i + 0]);
    const int lo = HexNibble(hex16[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

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

// Save all calibration coefficients into a single binary blob under key "cal".
// Layout: [CalBlobHeader][CalBlobEntry x N], CRC32 over the entries array.
// Uses NvsPutBytesVerified() for write + read-back verification.
void SaveAllCalibration() {
  // Open NVS namespace for write.
  if (!g_leaf_prefs.begin("leafcal", /*readOnly=*/false)) {
    Serial.println(F("CAL NVS: begin(\"leafcal\", false) FAILED"));
    return;
  }

  const size_t entry_count = g_cal_entries.size();

  if (entry_count == 0) {
    // No entries -> clear key.
    if (!NvsRemoveKeyVerified(g_leaf_prefs, "cal")) {
      Serial.println(F("CAL NVS: remove(\"cal\") FAILED"));
    } else {
      Serial.println(F("CAL NVS: cleared 'cal' key (0 entries)"));
    }
    g_leaf_prefs.end();
    return;
  }

  // Build binary blob in RAM.
  const size_t payload_size = entry_count * sizeof(CalBlobEntry);
  const size_t total_size = sizeof(CalBlobHeader) + payload_size;

  std::vector<uint8_t> buffer(total_size);
  auto *header = reinterpret_cast<CalBlobHeader *>(buffer.data());
  auto *entries =
      reinterpret_cast<CalBlobEntry *>(buffer.data() + sizeof(CalBlobHeader));

  header->magic = kCalMagic;
  header->version = kCalBlobVersion;
  header->entry_count = static_cast<uint16_t>(entry_count);
  header->crc32 = 0u; // filled after entries are populated

  // Populate entries from g_cal_entries (String hex16 -> 8-byte ROM).
  size_t filled = 0;
  for (const auto &e : g_cal_entries) {
    CalBlobEntry &be = entries[filled];

    if (!ParseAddrHex16(e.addr, be.addr)) {
      Serial.printf("CAL NVS: skipping entry with invalid addr=\"%s\"\n",
                    e.addr.c_str());
      continue;
    }

    be.a1 = e.coeff.a1;
    be.a0 = e.coeff.a0;
    ++filled;
  }

  if (filled == 0) {
    Serial.println(F("CAL NVS: no valid entries to save (all addrs invalid?)"));
    g_leaf_prefs.end();
    return;
  }

  // If we skipped some invalid entries, shrink the blob to only valid ones.
  if (filled != entry_count) {
    header->entry_count = static_cast<uint16_t>(filled);
  }

  const size_t final_payload_size =
      static_cast<size_t>(header->entry_count) * sizeof(CalBlobEntry);
  const size_t final_total_size = sizeof(CalBlobHeader) + final_payload_size;

  // Compute CRC over entries region only.
  header->crc32 =
      Crc32(reinterpret_cast<const uint8_t *>(entries), final_payload_size);

  // Write + verify to NVS under key "cal".
  if (!NvsPutBytesVerified(g_leaf_prefs, "cal", buffer.data(),
                           final_total_size)) {
    Serial.printf("CAL NVS: NvsPutBytesVerified(\"cal\") FAILED "
                  "(%u entr%s, %u bytes)\n",
                  static_cast<unsigned>(header->entry_count),
                  (header->entry_count == 1) ? "y" : "ies",
                  static_cast<unsigned>(final_total_size));
    g_leaf_prefs.end();
    return;
  }

  Serial.printf("CAL NVS: saved %u entr%s (%u bytes) to NVS\n",
                static_cast<unsigned>(header->entry_count),
                (header->entry_count == 1) ? "y" : "ies",
                static_cast<unsigned>(final_total_size));

  g_leaf_prefs.end();
}

// Load all calibration coefficients from the binary "cal" blob.
// Validates magic, version, size, and CRC32.
void LoadAllCalibration() {
  g_cal_entries.clear();

  if (!g_leaf_prefs.begin("leafcal", /*readOnly=*/true)) {
    Serial.println(F("CAL NVS: begin(\"leafcal\", true) FAILED"));
    return;
  }

  const size_t blob_length = g_leaf_prefs.getBytesLength("cal");
  if (blob_length == 0) {
    Serial.println(F("CAL NVS: no 'cal' blob found; node running uncalibrated "
                     "until you solve+save"));
    g_leaf_prefs.end();
    return;
  }

  if (blob_length < sizeof(CalBlobHeader)) {
    Serial.printf("CAL NVS: 'cal' blob too small (%u bytes); ignoring\n",
                  static_cast<unsigned>(blob_length));
    g_leaf_prefs.end();
    return;
  }

  std::vector<uint8_t> buffer(blob_length);
  const size_t read_back =
      g_leaf_prefs.getBytes("cal", buffer.data(), blob_length);
  if (read_back != blob_length) {
    Serial.printf("CAL NVS: getBytes(\"cal\") read %u/%u bytes; ignoring\n",
                  static_cast<unsigned>(read_back),
                  static_cast<unsigned>(blob_length));
    g_leaf_prefs.end();
    return;
  }

  const auto *header = reinterpret_cast<const CalBlobHeader *>(buffer.data());

  if (header->magic != kCalMagic) {
    Serial.println(F("CAL NVS: 'cal' blob has wrong magic (old format?); "
                     "ignoring"));
    g_leaf_prefs.end();
    return;
  }

  if (header->version != kCalBlobVersion) {
    Serial.printf("CAL NVS: unsupported cal blob version %u (expected %u); "
                  "ignoring\n",
                  static_cast<unsigned>(header->version),
                  static_cast<unsigned>(kCalBlobVersion));
    g_leaf_prefs.end();
    return;
  }

  const size_t expected_payload_size =
      static_cast<size_t>(header->entry_count) * sizeof(CalBlobEntry);
  const size_t actual_payload_size = blob_length - sizeof(CalBlobHeader);

  if (actual_payload_size != expected_payload_size) {
    Serial.printf("CAL NVS: size mismatch (entries=%u, expected_payload=%u, "
                  "actual_payload=%u); ignoring\n",
                  static_cast<unsigned>(header->entry_count),
                  static_cast<unsigned>(expected_payload_size),
                  static_cast<unsigned>(actual_payload_size));
    g_leaf_prefs.end();
    return;
  }

  const uint8_t *payload = buffer.data() + sizeof(CalBlobHeader);
  const uint32_t crc_calc = Crc32(payload, actual_payload_size);

  if (crc_calc != header->crc32) {
    Serial.printf("CAL NVS: CRC mismatch (stored=0x%08lX calc=0x%08lX); "
                  "ignoring\n",
                  static_cast<unsigned long>(header->crc32),
                  static_cast<unsigned long>(crc_calc));
    g_leaf_prefs.end();
    return;
  }

  const auto *entries = reinterpret_cast<const CalBlobEntry *>(payload);

  for (uint16_t i = 0; i < header->entry_count; ++i) {
    const CalBlobEntry &be = entries[i];

    Coeff coeff;
    coeff.a1 = be.a1;
    coeff.a0 = be.a0;

    // Convert back to the same 16-char hex form used everywhere else.
    const String addr16 = addrToHex(be.addr);
    SetCal(addr16, coeff);
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
      (g_root_id != 0U) &&
      ((millis() - g_root_last_seen_ms) < kRootUnicastWindowMs);

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

static void SendRootProbe() {
  JsonDocument doc;
  doc["type"] = "root_probe";
  doc["nodeId"] = mesh.getNodeId();
  doc["uptimeMs"] = static_cast<uint32_t>(millis());

  String message;
  serializeJson(doc, message);

  DLOG("[LEAF TX probe] %s\n", message.c_str());
  mesh.sendBroadcast(message);

  g_discovery.probes_sent_on_this_channel++;
  g_discovery.last_probe_ms = millis();
}

static void InitMeshOnCurrentDiscoveryChannel(Print &out) {
  if (g_discovery.channel_index >= g_discovery.channel_count) {
    out.println(
        "[mesh] discovery: InitMeshOnCurrentDiscoveryChannel out of range");
    return;
  }

  const uint8_t mesh_channel = g_discovery.channels[g_discovery.channel_index];

  out.printf("[mesh] discovery: starting mesh on channel %u\n",
             static_cast<unsigned>(mesh_channel));

  mesh.init(MESH_PREFIX, MESH_PASSWORD, &user_scheduler, MESH_PORT,
            WIFI_AP_STA, mesh_channel, MESH_HIDDEN);
  g_mesh_initialized = true; // mark as initialized

  mesh.setContainsRoot(true);
  mesh.onReceive(&OnReceiveLeaf);
  mesh.onChangedConnections(&OnConnectionsChangedLeaf);

  // Reset root tracking for this attempt.
  g_root_id = 0;
  g_root_last_seen_ms = 0;

  // Reset probe counters and mark the time we started this channel.
  g_discovery.probes_sent_on_this_channel = 0;
  g_discovery.last_probe_ms = 0;
  g_discovery_channel_start_ms = millis();
  // First root_probe() will be sent from OnConnectionsChangedLeaf()
  // once we actually have at least one peer on this channel.
}


static bool RootRecentlyPresent(uint32_t now_ms) {
  if (g_root_id == 0U) {
    return false;
  }
  return (now_ms - g_root_last_seen_ms) < kRootPresenceWindowMs;
}

// Fill g_discovery.channels[] with all channels where we see MESH_PREFIX.
// Returns number of channels found.
static uint8_t ScanMeshChannels(Print &out) {
  const char *mesh_ssid = MESH_PREFIX;

  g_discovery.channel_count = 0;
  g_discovery.channel_index = 0;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/true);
  delay(100);

  out.printf("[mesh] discovery: scanning for \"%s\" on all channels...\n",
             mesh_ssid);

  const int16_t network_count = WiFi.scanNetworks(
      /*async=*/false,
      /*show_hidden=*/true,
      /*passive=*/false,
      /*max_ms_per_channel=*/200,
      /*channel=*/0);

  if (network_count <= 0) {
    out.printf("[mesh] discovery: scan found no networks (err=%d)\n",
               static_cast<int>(network_count));
    WiFi.scanDelete();
    return 0;
  }

  for (int i = 0; i < network_count; ++i) {
    const String ssid = WiFi.SSID(i);
    const int32_t channel = WiFi.channel(i);

    if (ssid != mesh_ssid) {
      continue;
    }
    if (channel < 1 || channel > 11) {
      continue;
    }

    const uint8_t ch = static_cast<uint8_t>(channel);

    // Ensure uniqueness.
    bool already_present = false;
    for (uint8_t j = 0; j < g_discovery.channel_count; ++j) {
      if (g_discovery.channels[j] == ch) {
        already_present = true;
        break;
      }
    }
    if (already_present) {
      continue;
    }

    if (g_discovery.channel_count < kMaxMeshChannels) {
      g_discovery.channels[g_discovery.channel_count++] = ch;
      out.printf("[mesh] discovery: saw \"%s\" on channel %d (RSSI=%d dBm)\n",
                 mesh_ssid, static_cast<int>(ch),
                 static_cast<int>(WiFi.RSSI(i)));
    }
  }

  WiFi.scanDelete();

  if (g_discovery.channel_count == 0) {
    out.println("[mesh] discovery: no channels with mesh SSID found");
  } else {
    out.printf("[mesh] discovery: %u candidate channel(s)\n",
               static_cast<unsigned>(g_discovery.channel_count));
  }

  return g_discovery.channel_count;
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

    return;
  }

  if (strcmp(type, "root_ack") == 0) {
    // Future: root replies specifically to probes; treat it as a presence ping.
    const uint32_t root_id = doc["rootId"] | 0U;
    if (root_id != 0U) {
      g_root_id = root_id;
      g_root_last_seen_ms = millis();
      DLOG("  root_ack: rootId=0x%08lX\n",
           static_cast<unsigned long>(g_root_id));
    }
    return;
  }
}

void OnConnectionsChangedLeaf() {
  LogConnections();

  // During discovery, start probing for the root only after we actually
  // have at least one mesh peer on this channel. This avoids sending
  // probes into the void before the link is formed.
  if (g_discovery.ongoing && g_mesh_initialized &&
      g_discovery.probes_sent_on_this_channel == 0 &&
      !mesh.getNodeList().empty()) {
    Serial.println(
        F("[mesh] discovery: first peer on this channel, sending initial root probe"));
    SendRootProbe();
  }
}


static void StartDiscovery(Print &out) {
  const uint32_t now = millis();

  if (now < g_next_discovery_allowed_ms) {
    const uint32_t remaining_ms = g_next_discovery_allowed_ms - now;
    out.printf("[mesh] discovery: not yet allowed, retry in %u s\n",
               static_cast<unsigned>(remaining_ms / 1000U));
    return;
  }

  out.println("[mesh] discovery: starting discovery phase");

  // Only stop if the mesh has been initialized before.
  if (g_mesh_initialized) {
    mesh.stop();
    g_mesh_initialized = false;
  }

  g_root_id = 0;
  g_root_last_seen_ms = 0;

const uint8_t count = ScanMeshChannels(out);
if (count == 0) {
  // Nobody with our SSID is on the air; treat this as a failed discovery
  // cycle and use the adaptive backoff strategy.
  if (g_consecutive_discovery_failures < 255) {
    ++g_consecutive_discovery_failures;
  }

  const uint32_t backoff = ComputeRescanDelayMs();
  g_next_discovery_allowed_ms = now + backoff;
  out.printf("[mesh] discovery: no candidate channels; rescan in %u s "
             "(failures=%u)\n",
             static_cast<unsigned>(backoff / 1000U),
             static_cast<unsigned>(g_consecutive_discovery_failures));
  g_discovery.ongoing = false;
  return;
}


  g_discovery.ongoing = true;
  g_discovery.channel_index = 0;
  g_discovery.probes_sent_on_this_channel = 0;
  g_discovery.last_probe_ms = 0;

  InitMeshOnCurrentDiscoveryChannel(out);
}

static uint32_t ComputeRescanDelayMs() {
  // Base times tuned to ROOT_ANNOUNCE_MS = 20000 ms.
  uint32_t base_ms;

  if (g_consecutive_discovery_failures == 0) {
    // First failure: retry fairly quickly (about 30 s).
    base_ms = kRootAnnounceMs * 1.5;
  } else if (g_consecutive_discovery_failures == 1) {
    // Second failure: a bit longer (about 60 s).
    base_ms = 3 * kRootAnnounceMs;
  } else {
    // Third and subsequent failures: long nap.
    base_ms = kRescanMinDelayMs;  // 5 minutes
  }

  // Add ±50 % jitter so nodes do not all wake at once.
  const uint32_t jitter = base_ms / 2;
  const uint32_t offset =
      static_cast<uint32_t>(random(0L, static_cast<long>(2 * jitter + 1)));

  // Result is in [base_ms - jitter, base_ms + jitter].
  return base_ms - jitter + offset;
}

// Advance to the next discovery channel (or finish discovery and schedule
// a rescan backoff). This must be called from loop(), never from a
// TaskScheduler callback.
static void AdvanceDiscoveryChannel(Print &out) {
  if (!g_discovery.ongoing) {
    // Nothing to do.
    return;
  }

  const uint32_t now_ms = millis();

  Serial.println(F("[mesh] discovery: advancing discovery channel"));

  if (g_mesh_initialized) {
    mesh.stop();
    g_mesh_initialized = false;
  }

  g_root_id = 0;
  g_root_last_seen_ms = 0;

  g_discovery.channel_index++;
  g_discovery.probes_sent_on_this_channel = 0;
  g_discovery.last_probe_ms = 0;

  if (g_discovery.channel_index < g_discovery.channel_count) {
    // Try the next channel in the candidate list.
    InitMeshOnCurrentDiscoveryChannel(out);
    // We remain in discovery mode.
    return;
  }

  // No more channels left to try; schedule another full scan after a backoff.
  if (g_consecutive_discovery_failures < 255) {
    ++g_consecutive_discovery_failures;
  }
  const uint32_t backoff = ComputeRescanDelayMs();
  g_next_discovery_allowed_ms = now_ms + backoff;

  Serial.printf("[mesh] discovery: no root found on any channel; "
                "will rescan in %u s (failures=%u)\n",
                static_cast<unsigned>(backoff / 1000U),
                static_cast<unsigned>(g_consecutive_discovery_failures));

  g_discovery.ongoing = false;
}

// Mesh watchdog: if we clearly lost the root for a while, drive the
// discovery state machine. All heavy operations (mesh.stop(), changing
// channels, re-init) are delegated to loop() via pending flags.
void MeshWatchdogTaskFn() {
  const uint32_t now_ms = millis();

  // If we are in normal mode (no discovery ongoing) and root is present
  // within the timeout window, we do nothing except re-check later.
  if (!g_discovery.ongoing) {
    if (RootRecentlyPresent(now_ms)) {
      g_task_mesh_watchdog.delay(10000);  // 10 s
      return;
    }

    // Root missing or never seen. Time to request discovery if allowed.
    if (now_ms >= g_next_discovery_allowed_ms) {
      Serial.println(
          F("[mesh] watchdog: root missing; scheduling discovery phase"));
      g_start_discovery_pending = true;
    } else {
      const uint32_t remaining_ms = g_next_discovery_allowed_ms - now_ms;
      Serial.printf("[mesh] watchdog: root missing; discovery blocked for "
                    "%u more s\n",
                    static_cast<unsigned>(remaining_ms / 1000U));
    }

    g_task_mesh_watchdog.delay(5000);  // 5 s
    return;
  }

  // --------------------------------------------------------------------
  // Discovery is ongoing: we are trying some channel from g_discovery.channels[]
  // --------------------------------------------------------------------

  // Success path: if we see root on this channel, we stop discovery and stay.
  if (RootRecentlyPresent(now_ms)) {
    const uint8_t ch = g_discovery.channels[g_discovery.channel_index];
    Serial.printf("[mesh] discovery: root found on channel %u, "
                  "locking onto this mesh\n",
                  static_cast<unsigned>(ch));
    g_discovery.ongoing = false;
    // After success, we can allow immediate future discovery if root is lost.
    g_next_discovery_allowed_ms = now_ms;
    g_consecutive_discovery_failures = 0;  // reset failures
    g_task_mesh_watchdog.delay(10000);     // 10 s
    return;
  }

  // No root yet on this channel; decide whether to send another probe or move
  // on. (Sending probes is cheap and safe from a scheduler task.)
  if (g_discovery.probes_sent_on_this_channel < kMaxProbesPerChannel) {
    const uint32_t delay_ms =
        kProbeBaseDelayMs << g_discovery.probes_sent_on_this_channel;  // 2^n

    if ((now_ms - g_discovery.last_probe_ms) >= delay_ms) {
      Serial.println(F("[mesh] discovery: sending another root probe"));
      SendRootProbe();
    }

    g_task_mesh_watchdog.delay(1000);  // check again in ~1 s
    return;
  }

  // Before we declare this channel dead, stay attached long enough to
  // give any real root time to send at least one root_announce().
  const uint32_t elapsed_ms = now_ms - g_discovery_channel_start_ms;
  if (elapsed_ms < kChannelMinDiscoveryMs) {
    g_task_mesh_watchdog.delay(1000);
    return;
  }

  // We have exhausted probes on this channel with no root. Ask loop()
  // to tear the mesh down and advance to the next channel (or to backoff).
  Serial.println(
      F("[mesh] discovery: no root on this channel after retries; "
        "will advance to next channel (deferred)"));

  g_advance_discovery_channel_pending = true;
  g_task_mesh_watchdog.delay(1000);  // keep checking regularly
}

// Process any deferred mesh-discovery actions. This **must** only be called
// from loop(), never from a TaskScheduler callback, so that mesh.stop() /
// mesh.init() are not invoked while Scheduler::execute() is iterating tasks.
static void ProcessPendingDiscoveryActionsFromLoop() {
  if (g_start_discovery_pending) {
    g_start_discovery_pending = false;
    StartDiscovery(Serial);
  }

  if (g_advance_discovery_channel_pending) {
    g_advance_discovery_channel_pending = false;
    AdvanceDiscoveryChannel(Serial);
  }
}

// Arduino entry points (leaf)
void setup() {
  Serial.begin(DBG_BAUD);
  delay(1000);

  randomSeed(esp_random());

  g_ds18.begin();
  LoadAllCalibration();
  ScanSensors();

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  StartDiscovery(Serial);

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

  g_task_mesh_watchdog.set(TASK_IMMEDIATE, TASK_FOREVER, MeshWatchdogTaskFn);
  user_scheduler.addTask(g_task_mesh_watchdog);
  g_task_mesh_watchdog.enable();

  Serial.println(F("LEAF ready. Type 'help'."));
}

void loop() {
  // Run PainlessMesh and TaskScheduler tasks (including MeshWatchdogTaskFn).
  mesh.update();

  // Perform any deferred mesh stop / re-init / channel-advance actions
  // **after** mesh.update() has returned.
  ProcessPendingDiscoveryActionsFromLoop();

  // Process console input/output.
  g_console.Poll(Serial, Serial);
}


