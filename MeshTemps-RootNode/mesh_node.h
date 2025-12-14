#ifndef MESH_NODE_H_
#define MESH_NODE_H_

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include <vector>
#include <limits>

// Small POD used by the root to persist node metadata in NVS.
struct NodeMetaRecord {
  uint32_t node_id;
  int32_t tile_rank;
  uint8_t mute_mask;
  uint8_t reserved[3];  // future use / alignment
};

// Represents one sensor attached to a mesh node.
class MeshNode {
 public:
  // One time-series sample for a single sensor.
  struct SensorHistorySample {
    enum Flag : uint8_t {
      kHasEpoch = 1 << 0,
      kHasValue = 1 << 1,
      kCorrected = 1 << 2,
    };

    uint32_t timestamp_ms;   // monotonic millis() at time of sample
    time_t timestamp_epoch;  // optional wall-clock time at sample
    float temp_c;
    uint8_t flags;

    SensorHistorySample()
        : timestamp_ms(0), timestamp_epoch(0), temp_c(NAN), flags(0) {}

    bool has_epoch() const { return flags & kHasEpoch; }
    void set_has_epoch(bool value) { UpdateFlag(kHasEpoch, value); }

    bool has_value() const { return flags & kHasValue; }
    void set_has_value(bool value) { UpdateFlag(kHasValue, value); }

    bool corrected() const { return flags & kCorrected; }
    void set_corrected(bool value) { UpdateFlag(kCorrected, value); }

   private:
    void UpdateFlag(Flag flag, bool set) {
      if (set) {
        flags |= flag;
      } else {
        flags &= static_cast<uint8_t>(~flag);
      }
    }
  };

  struct Sensor {
    String address;  // 16-char DS18B20 ROM code (as sent by leaf)

    // Metadata owned by MeshNode.
    String label;  // Optional human-friendly label

    // Ordering metadata:
    //   - global_rank: from "order <addr> <rank>"
    //   - node_rank  : from "sorder <node> <addr> <rank>"
    int32_t global_rank = std::numeric_limits<int32_t>::max();
    int32_t node_rank = std::numeric_limits<int32_t>::max();

    // Live measurement data.
    float temp_c = NAN;   // Raw temperature in °C
    bool has_value = false;
    bool corrected = false;
    uint32_t last_ms = 0;  // millis() timestamp of last update

    // Time-series history for this sensor (ring buffer).
    //
    // We pre-size 'history' to MeshNode::history_capacity_per_sensor_ and
    // use (history_head_index, history_size) to track the valid window.
    std::vector<SensorHistorySample> history;
    size_t history_head_index = 0;  // where the next sample will be written
    size_t history_size = 0;        // number of valid entries in 'history'

    // Copy history into |out| in chronological order (oldest -> newest).
    void CopyHistoryInChronologicalOrder(
        std::vector<SensorHistorySample>* out) const {
      if (out == nullptr) {
        return;
      }
      out->clear();
      if (history.empty() || history_size == 0) {
        return;
      }
      out->reserve(history_size);

      const size_t capacity = history.size();
      const size_t start_index =
          (history_size == capacity) ? history_head_index : 0;

      for (size_t i = 0; i < history_size; ++i) {
        const size_t idx = (start_index + i) % capacity;
        out->push_back(history[idx]);
      }
    }
  };



  explicit MeshNode(uint32_t node_id);

  uint32_t node_id() const { return node_id_; }
  const String& node_id_str() const { return node_id_str_; }   // canonical 8-hex
  const String& node_key_hex() const { return node_key_hex_; } // 8-hex
  int bus_gpio() const { return bus_gpio_; }
  uint32_t last_update_ms() const { return last_update_ms_; }

  // Access to sensor list.
  std::vector<Sensor>& sensors() { return sensors_; }
  const std::vector<Sensor>& sensors() const { return sensors_; }

  // Metadata for sensors (labels + ordering).
  bool SetSensorLabel(const String& address, const String& label);
  String GetSensorLabel(const String& address) const;

  bool SetSensorGlobalRank(const String& address, int32_t rank);
  bool SetSensorNodeRank(const String& address, int32_t rank);
  int32_t GetSensorEffectiveRank(const String& address) const;

  // Returns pointer to sensor with matching 16-char address, or nullptr.
  Sensor* FindSensor(const String& address);
  const Sensor* FindSensor(const String& address) const;

  // Returns pointer to first sensor whose label matches (case-insensitive),
  // or nullptr if not found. Used by the room map view for "Room"/"Underbelly".
  Sensor* FindSensorByLabel(const String& label);
  const Sensor* FindSensorByLabel(const String& label) const;

  // Returns the history for the first sensor whose label matches
  // (case-insensitive), in chronological order (oldest -> newest).
  //
  // Returns true if history was found and written into |out|.
  // On failure, |out| is cleared and false is returned.
  bool GetSensorHistoryByLabel(
      const String& label,
      std::vector<SensorHistorySample>* out) const;

  // Returns the history for the sensor with the given 16-char address
  // (ROM code) in chronological order (oldest -> newest).
  //
  // Returns true if history was found and written into |out|.
  // On failure, |out| is cleared and false is returned.
  bool GetSensorHistoryByAddress(
      const String& address,
      std::vector<SensorHistorySample>* out) const;

  // Helpers for callers that want to populate nodes without going through a
  // full JSON "temps" document (e.g. dummy data, tests).
  void SetBusGpioAndLastUpdate(int bus_gpio, uint32_t now_ms);

  // Ensure a sensor with the given address exists and return it.
  // If it does not exist yet, it is created with no value.
  Sensor* GetOrCreateSensor(const String& address);

  // Update this node from a "temps" JSON payload (already parsed).
  //
  // Expected JSON shape:
  //   {
  //     "type": "temps",
  //     "nodeId": <uint32>,
  //     "busGpio": <int>,
  //     "sensors": [
  //       { "addr": "0011223344556677", "tC": 23.5, "corr": true },
  //       etc
  //     ]
  //   }
  //
  // The caller should pass the already-extracted sensor array and bus_gpio to
  // avoid re-parsing.
  void UpdateFromTemps(int bus_gpio,
                       const JsonArrayConst& sensor_array,
                       uint32_t now_ms);

  // Compute the node "age" in minutes based on the latest of node and sensor
  // timestamps. If the node has never been updated, returns 0.
  uint32_t ComputeAgeMinutes(uint32_t now_ms) const;

  // Sequence tracking for "temps" messages.
  //
  // Call UpdateSequence() for every "temps" message (even if the payload
  // is otherwise unchanged). seq==0 is treated as "no sequence" for
  // backwards compatibility with older leaf firmware.
  void UpdateSequence(uint32_t seq, uint32_t now_ms);

  bool has_sequence() const { return has_sequence_; }
  uint32_t last_sequence() const { return last_seq_; }
  uint32_t last_sequence_advance_ms() const { return last_seq_advance_ms_; }
  uint32_t last_sequence_rx_ms() const { return last_seq_rx_ms_; }
  uint32_t duplicate_sequence_rx_count() const {
    return duplicate_sequence_rx_count_;
  }

  // Return true if the sequence appears "stuck": the sequence has not
  // advanced for at least stuck_ms_threshold while we continue to receive
  // messages with exactly the same sequence value.
  bool SequenceStuck(uint32_t now_ms,
                     uint32_t stuck_ms_threshold) const;

  // ---- History logging configuration ----
  //
  // interval_ms == 0 disables history logging.
  // retention_days is used to size the per-sensor ring buffer.
  static void SetHistoryConfig(uint32_t interval_ms, uint32_t retention_days);

  // Called once after the first successful time sync (e.g., SNTP).
  //
  // Stores the mapping between millis() and epoch, and back-fills any
  // existing SensorHistorySample entries that were logged before the sync
  // so they gain epoch timestamps.
  static void OnFirstTimeSync(time_t epoch_now, uint32_t now_ms);

  static uint32_t history_interval_ms() { return history_interval_ms_; }
  static uint32_t history_retention_days() { return history_retention_days_; }
  static size_t history_capacity_per_sensor() {
    return history_capacity_per_sensor_;
  }

  // ---- NEW: per-node UI metadata accessors ----
  int32_t tile_rank() const { return tile_rank_; }
  void set_tile_rank(int32_t rank) { tile_rank_ = rank; }

  const String& label() const { return label_; }
  void set_label(const String& label) { label_ = label; }

  uint8_t mute_mask() const { return mute_mask_; }
  void set_mute_mask(uint8_t mask) { mute_mask_ = mask; }

 private:
  // Helper used in constructor.
  static String FormatNodeKeyHex(uint32_t node_id);

  uint32_t node_id_ = 0;
  String node_id_str_;   // canonical 8-hex string form, e.g. "10000001"
  String node_key_hex_;  // canonical 8-hex form, e.g. "10000001"

  // Sequence tracking (root-only).
  bool has_sequence_ = false;
  uint32_t last_seq_ = 0;
  uint32_t last_seq_advance_ms_ = 0;
  uint32_t last_seq_rx_ms_ = 0;
  uint32_t duplicate_sequence_rx_count_ = 0;

  int bus_gpio_ = -1;
  uint32_t last_update_ms_ = 0;

  std::vector<Sensor> sensors_;

  // Per-node UI metadata.
  // Large default rank means "unspecified / after ranked ones".
  int32_t tile_rank_ = std::numeric_limits<int32_t>::max();
  String label_;
  uint8_t mute_mask_ = 0;  // NodeMuteMask bits

  // ---- History logging (per-node) ----
  //
  // Last time we logged a sample for this node (millis()).
  uint32_t last_history_log_ms_ = 0;

  // Global (static) history configuration shared by all nodes.
  static uint32_t history_interval_ms_;
  static uint32_t history_retention_days_;
  static size_t history_capacity_per_sensor_;

  // Internal helpers.
  static size_t ComputeHistoryCapacityPerSensor();
  static time_t ComputeEpochFromMillis(uint32_t timestamp_ms);
  void MaybeLogHistorySample(uint32_t now_ms);

  // Mapping from millis() -> epoch captured at first time sync.
  static bool has_time_sync_;
  static time_t first_sync_epoch_;
  static uint32_t first_sync_ms_;
};

// ---------------------------------------------------------------------------
// Simple global accessors for MeshNode instances.
// ---------------------------------------------------------------------------

// Get an existing node or create a new one for the given node_id.
MeshNode* GetOrCreateMeshNode(uint32_t node_id);

// Return a pointer to an existing node, or nullptr if not present.
MeshNode* FindMeshNode(uint32_t node_id);

// Parse a leaf "temps" JSON payload and update the corresponding MeshNode.
//
// |doc| must already be a successfully-deserialized ArduinoJson document
// matching the "temps" schema. |default_node_id| is used if the document does
// not contain an explicit "nodeId" field (backwards compatibility).
//
// Returns the updated MeshNode*, or nullptr on error.
MeshNode* UpdateMeshNodeFromTempsJson(const JsonDocument& doc,
                                      uint32_t default_node_id,
                                      uint32_t now_ms);

// Get a snapshot of all known node IDs in the global store.
std::vector<uint32_t> GetAllMeshNodeIds();

// Clear all known nodes from the global store (used by dummy mode).
void ClearAllMeshNodes();

// Remove a single node from the global store; returns true if removed.
bool RemoveMeshNode(uint32_t node_id);

#endif  // MESH_NODE_H_
