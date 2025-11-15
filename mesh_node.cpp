#include "mesh_node.h"

#include <algorithm>
#include <map>

MeshNode::MeshNode(uint32_t node_id)
    : node_id_(node_id),
      node_id_str_(String(node_id)),
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
                               const JsonArray& sensor_array,
                               uint32_t now_ms) {
  SetBusGpioAndLastUpdate(bus_gpio, now_ms);

  // Update / insert each sensor reported in this payload.
  for (JsonObject sensor_in : sensor_array) {
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

// ---------------------------------------------------------------------------
// Global node store
// ---------------------------------------------------------------------------

namespace {

std::map<uint32_t, MeshNode> g_mesh_nodes;

}  // namespace

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

void ClearAllMeshNodes() {
  g_mesh_nodes.clear();
}
