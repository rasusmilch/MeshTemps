#include "mesh_node.h"

#include <algorithm>
#include <map>

MeshNode::MeshNode(uint32_t node_id)
    : node_id_(node_id),
      node_id_str_(FormatNodeKeyHex(node_id)),   // hex for any user-facing use
      node_key_hex_(FormatNodeKeyHex(node_id)) {}

String MeshNode::FormatNodeKeyHex(uint32_t node_id) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(node_id));
  return String(buf);  // e.g. "10000001"
}

MeshNode::Sensor* MeshNode::FindSensor(const String& address) {
  for (auto& sensor : sensors_) {
    if (sensor.address == address) {
      return &sensor;
    }
  }
  return nullptr;
}

const MeshNode::Sensor* MeshNode::FindSensor(const String& address) const {
  for (const auto& sensor : sensors_) {
    if (sensor.address == address) {
      return &sensor;
    }
  }
  return nullptr;
}

// Find sensors by human label (case-insensitive), e.g. "Room" / "Underbelly".
MeshNode::Sensor* MeshNode::FindSensorByLabel(const String& label) {
  for (auto& sensor : sensors_) {
    if (sensor.label.equalsIgnoreCase(label)) {
      return &sensor;
    }
  }
  return nullptr;
}

const MeshNode::Sensor* MeshNode::FindSensorByLabel(
    const String& label) const {
  for (const auto& sensor : sensors_) {
    if (sensor.label.equalsIgnoreCase(label)) {
      return &sensor;
    }
  }
  return nullptr;
}

bool MeshNode::SetSensorLabel(const String& address, const String& label) {
  Sensor* sensor = FindSensor(address);
  if (sensor == nullptr) {
    return false;
  }
  sensor->label = label;
  return true;
}

String MeshNode::GetSensorLabel(const String& address) const {
  const Sensor* sensor = FindSensor(address);
  if (sensor == nullptr) {
    return String();
  }
  return sensor->label;
}

bool MeshNode::SetSensorGlobalRank(const String& address, int32_t rank) {
  Sensor* sensor = FindSensor(address);
  if (sensor == nullptr) {
    return false;
  }
  sensor->global_rank = rank;
  return true;
}

bool MeshNode::SetSensorNodeRank(const String& address, int32_t rank) {
  Sensor* sensor = FindSensor(address);
  if (sensor == nullptr) {
    return false;
  }
  sensor->node_rank = rank;
  return true;
}

int32_t MeshNode::GetSensorEffectiveRank(const String& address) const {
  const Sensor* sensor = FindSensor(address);
  if (sensor == nullptr) {
    return std::numeric_limits<int32_t>::max();
  }
  if (sensor->node_rank != std::numeric_limits<int32_t>::max()) {
    return sensor->node_rank;
  }
  return sensor->global_rank;
}

void MeshNode::SetBusGpioAndLastUpdate(int bus_gpio, uint32_t now_ms) {
  bus_gpio_ = bus_gpio;
  last_update_ms_ = now_ms;
}

MeshNode::Sensor* MeshNode::GetOrCreateSensor(const String& address) {
  Sensor* existing = FindSensor(address);
  if (existing != nullptr) {
    return existing;
  }

  sensors_.push_back(Sensor());
  Sensor* created = &sensors_.back();
  created->address = address;
  return created;
}

void MeshNode::UpdateFromTemps(int bus_gpio,
                               const JsonArrayConst& sensor_array,
                               uint32_t now_ms) {
  SetBusGpioAndLastUpdate(bus_gpio, now_ms);

  // Update / insert each sensor reported in this payload.
  for (JsonObjectConst sensor_in : sensor_array) {
    const char* addr = sensor_in["addr"] | "";
    if (!addr || strlen(addr) != 16) {
      // Ignore malformed addresses (must be 16 hex chars).
      continue;
    }

    String addr_str(addr);

    Sensor* sensor = GetOrCreateSensor(addr_str);
    if (sensor == nullptr) {
      continue;
    }

    if (sensor_in["tC"].is<float>()) {
      sensor->temp_c = sensor_in["tC"].as<float>();
      sensor->has_value = !isnan(sensor->temp_c);
    } else {
      sensor->temp_c = NAN;
      sensor->has_value = false;
    }

    sensor->corrected = sensor_in["corr"] | false;
    sensor->last_ms = now_ms;
  }
}


uint32_t MeshNode::ComputeAgeMinutes(uint32_t now_ms) const {
  uint32_t latest_ms = last_update_ms_;

  for (const auto& sensor : sensors_) {
    if (sensor.last_ms > latest_ms) {
      latest_ms = sensor.last_ms;
    }
  }

  if (latest_ms == 0 || latest_ms > now_ms) {
    return 0;
  }

  const uint32_t delta_ms = now_ms - latest_ms;
  return delta_ms / 60000U;
}

void MeshNode::UpdateSequence(uint32_t seq, uint32_t now_ms) {
  // Backwards compatibility: if the payload has no seq field,
  // callers pass seq==0 and we ignore it.
  if (seq == 0U) {
    return;
  }

  if (!has_sequence_) {
    has_sequence_ = true;
    last_seq_ = seq;
    last_seq_advance_ms_ = now_ms;
    last_seq_rx_ms_ = now_ms;
    duplicate_sequence_rx_count_ = 0;
    return;
  }

  // We have seen a sequence before; record this receive time.
  last_seq_rx_ms_ = now_ms;

  if (seq > last_seq_) {
    // Normal forward progress (we do not require +1, only monotonic).
    last_seq_ = seq;
    last_seq_advance_ms_ = now_ms;
    duplicate_sequence_rx_count_ = 0;
  } else if (seq == last_seq_) {
    // Duplicate; be lenient, but track how many such repeats we have seen.
    if (duplicate_sequence_rx_count_ != 0xFFFFFFFFu) {
      ++duplicate_sequence_rx_count_;
    }
  } else {
    // Sequence wrapped or node rebooted. Treat this as a fresh starting
    // point rather than an error.
    last_seq_ = seq;
    last_seq_advance_ms_ = now_ms;
    duplicate_sequence_rx_count_ = 0;
  }
}

bool MeshNode::SequenceStuck(uint32_t now_ms,
                             uint32_t stuck_ms_threshold) const {
  if (!has_sequence_) {
    return false;
  }
  if (last_seq_advance_ms_ == 0U || now_ms <= last_seq_advance_ms_) {
    return false;
  }

  const uint32_t since_advance_ms = now_ms - last_seq_advance_ms_;

  // Only call it "stuck" if:
  //  - the sequence has not advanced for at least stuck_ms_threshold, AND
  //  - we have actually received one or more duplicate seq values
  //    after the last advance.
  //
  // Nodes that simply go silent are handled by the existing age/stale/missing
  // logic instead.
  if (since_advance_ms >= stuck_ms_threshold &&
      last_seq_rx_ms_ > last_seq_advance_ms_ &&
      duplicate_sequence_rx_count_ > 0U) {
    return true;
  }

  return false;
}

MeshNode* UpdateMeshNodeFromTempsJson(const JsonDocument& doc,
                                      uint32_t default_node_id,
                                      uint32_t now_ms) {
  const uint32_t node_id = doc["nodeId"] | default_node_id;
  const int bus_gpio = doc["busGpio"] | -1;

  // With a const JsonDocument, doc["sensors"] is a JsonVariantConst,
  // so we must convert to JsonArrayConst in ArduinoJson v7.
  JsonArrayConst sensor_array = doc["sensors"].as<JsonArrayConst>();

  // NEW: optional sequence number (0 if absent / unsupported).
  const uint32_t seq = doc["seq"] | 0U;

  MeshNode* node = GetOrCreateMeshNode(node_id);
  if (node == nullptr) {
    return nullptr;
  }

  // NEW: track sequence health independently of sensor contents.
  node->UpdateSequence(seq, now_ms);

  node->UpdateFromTemps(bus_gpio, sensor_array, now_ms);
  return node;
}



// ---------------------------------------------------------------------------
// Global node store
// ---------------------------------------------------------------------------

namespace {

std::map<uint32_t, MeshNode> g_mesh_nodes;

}  // namespace

std::vector<uint32_t> GetAllMeshNodeIds() {
  std::vector<uint32_t> ids;
  ids.reserve(g_mesh_nodes.size());
  for (const auto& kv : g_mesh_nodes) {
    ids.push_back(kv.first);
  }
  return ids;
}

MeshNode* GetOrCreateMeshNode(uint32_t node_id) {
  auto it = g_mesh_nodes.find(node_id);
  if (it != g_mesh_nodes.end()) {
    return &it->second;
  }

  auto result = g_mesh_nodes.emplace(node_id, MeshNode(node_id));
  return &result.first->second;
}

MeshNode* FindMeshNode(uint32_t node_id) {
  auto it = g_mesh_nodes.find(node_id);
  if (it == g_mesh_nodes.end()) {
    return nullptr;
  }
  return &it->second;
}

bool RemoveMeshNode(uint32_t node_id) {
  auto it = g_mesh_nodes.find(node_id);
  if (it == g_mesh_nodes.end()) {
    return false;
  }
  g_mesh_nodes.erase(it);
  return true;
}

void ClearAllMeshNodes() {
  g_mesh_nodes.clear();
}
