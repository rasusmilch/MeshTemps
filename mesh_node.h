#ifndef MESH_NODE_H_
#define MESH_NODE_H_

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

// Represents one sensor attached to a mesh node.
class MeshNode {
 public:
  struct Sensor {
    String address;      // 16-char DS18B20 ROM code (as sent by leaf)
    float temp_c = NAN;  // Raw temperature in °C
    bool has_value = false;
    bool corrected = false;
    uint32_t last_ms = 0;  // millis() timestamp of last update
  };

  explicit MeshNode(uint32_t node_id);

  uint32_t node_id() const { return node_id_; }
  const String& node_id_str() const { return node_id_str_; }        // decimal
  const String& node_key_hex() const { return node_key_hex_; }      // 8-hex
  int bus_gpio() const { return bus_gpio_; }
  uint32_t last_update_ms() const { return last_update_ms_; }

  const std::vector<Sensor>& sensors() const { return sensors_; }

  // Returns pointer to sensor with matching 16-char address, or nullptr.
  Sensor* FindSensor(const String& address);
  const Sensor* FindSensor(const String& address) const;

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
  //       ...
  //     ]
  //   }
  //
  // The caller should pass the already-extracted sensor array and bus_gpio to
  // avoid re-parsing.
  void UpdateFromTemps(int bus_gpio,
                       const JsonArray& sensor_array,
                       uint32_t now_ms);

  // Compute the node "age" in minutes based on the latest of node and sensor
  // timestamps. If the node has never been updated, returns 0.
  uint32_t ComputeAgeMinutes(uint32_t now_ms) const;

 private:
  // Helper used in constructor.
  static String FormatNodeKeyHex(uint32_t node_id);

  uint32_t node_id_ = 0;
  String node_id_str_;   // decimal string form, e.g. "268435457"
  String node_key_hex_;  // canonical 8-hex form, e.g. "10000001"

  int bus_gpio_ = -1;
  uint32_t last_update_ms_ = 0;

  std::vector<Sensor> sensors_;
};

// ---------------------------------------------------------------------------
// Simple global accessors for MeshNode instances.
// ---------------------------------------------------------------------------

// Get an existing node or create a new one for the given node_id.
MeshNode* GetOrCreateMeshNode(uint32_t node_id);

// Return a pointer to an existing node, or nullptr if not present.
MeshNode* FindMeshNode(uint32_t node_id);

// Clear all known nodes from the global store (used by dummy mode).
void ClearAllMeshNodes();

#endif  // MESH_NODE_H_
