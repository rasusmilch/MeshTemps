#include "history_hour_stager.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace {

uint8_t BitMask(uint8_t slot_id) {
  return static_cast<uint8_t>(1U << (slot_id % 8U));
}

bool SlotInRange(uint8_t slot_id) {
  return slot_id < kHistorySlotCapacity;
}

bool MinuteInRange(uint8_t minute_index) {
  return minute_index < kHistoryMinutesPerHour;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

char UpperHex(char c) {
  return (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
}

}  // namespace

HistoryCentiCResult HistoryTempCToCentiC(float temp_c) {
  HistoryCentiCResult result;
  if (!std::isfinite(temp_c)) {
    return result;
  }

  const float scaled = temp_c * 100.0f;
  if (!std::isfinite(scaled) ||
      scaled < static_cast<float>(std::numeric_limits<int16_t>::min() + 1) ||
      scaled > static_cast<float>(std::numeric_limits<int16_t>::max())) {
    return result;
  }

  const long rounded = std::lround(scaled);
  if (rounded < static_cast<long>(std::numeric_limits<int16_t>::min() + 1) ||
      rounded > static_cast<long>(std::numeric_limits<int16_t>::max())) {
    return result;
  }

  result.valid = true;
  result.value = static_cast<int16_t>(rounded);
  return result;
}

bool HistoryParseAddr16ToRom64(const char* addr16, uint64_t* out_rom64) {
  if (addr16 == nullptr || out_rom64 == nullptr) {
    return false;
  }

  uint64_t value = 0;
  for (std::size_t i = 0; i < 16U; ++i) {
    const int nibble = HexValue(addr16[i]);
    if (nibble < 0) {
      return false;
    }
    value = (value << 4U) | static_cast<uint64_t>(nibble);
  }
  if (addr16[16] != '\0') {
    return false;
  }

  *out_rom64 = value;
  return true;
}

bool HistoryNormalizeAddr16(const char* addr16, char out_addr16[17]) {
  if (addr16 == nullptr || out_addr16 == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < 16U; ++i) {
    if (HexValue(addr16[i]) < 0) {
      return false;
    }
    out_addr16[i] = UpperHex(addr16[i]);
  }
  if (addr16[16] != '\0') {
    return false;
  }
  out_addr16[16] = '\0';
  return true;
}

void HistoryMinuteFrame::Clear() {
  presence.fill(0);
  corrected.fill(0);
  temp_c_x100.fill(kHistoryInvalidTempCentiC);
}

bool HistoryMinuteFrame::IsPresent(uint8_t slot_id) const {
  if (!SlotInRange(slot_id)) {
    return false;
  }
  return (presence[slot_id / 8U] & BitMask(slot_id)) != 0;
}

bool HistoryMinuteFrame::IsCorrected(uint8_t slot_id) const {
  if (!SlotInRange(slot_id) || !IsPresent(slot_id)) {
    return false;
  }
  return (corrected[slot_id / 8U] & BitMask(slot_id)) != 0;
}

int16_t HistoryMinuteFrame::TemperatureCentiC(uint8_t slot_id) const {
  if (!SlotInRange(slot_id) || !IsPresent(slot_id)) {
    return kHistoryInvalidTempCentiC;
  }
  return temp_c_x100[slot_id];
}

bool HistoryMinuteFrame::SetSample(uint8_t slot_id,
                                   int16_t temp_c_x100_value,
                                   bool corrected_value) {
  if (!SlotInRange(slot_id) || temp_c_x100_value == kHistoryInvalidTempCentiC) {
    return false;
  }

  presence[slot_id / 8U] |= BitMask(slot_id);
  if (corrected_value) {
    corrected[slot_id / 8U] |= BitMask(slot_id);
  } else {
    corrected[slot_id / 8U] &= static_cast<uint8_t>(~BitMask(slot_id));
  }
  temp_c_x100[slot_id] = temp_c_x100_value;
  return true;
}

void HistoryMinuteFrame::ClearSample(uint8_t slot_id) {
  if (!SlotInRange(slot_id)) {
    return;
  }
  presence[slot_id / 8U] &= static_cast<uint8_t>(~BitMask(slot_id));
  corrected[slot_id / 8U] &= static_cast<uint8_t>(~BitMask(slot_id));
  temp_c_x100[slot_id] = kHistoryInvalidTempCentiC;
}

RamHourStager::RamHourStager() {
  Clear();
}

void RamHourStager::Clear() {
  status_ = HistoryStagerStatus{};
  for (auto& slot : slots_) {
    slot = HistorySlotDescriptor{};
  }
  for (auto& frame : frames_) {
    frame.Clear();
  }
}

bool RamHourStager::ResetHour(uint32_t hour_start_epoch_minute) {
  Clear();
  status_.hour_start_epoch_minute = hour_start_epoch_minute;
  status_.hour_active = true;
  return true;
}

bool RamHourStager::ValidateMinute_(uint8_t minute_index) {
  if (!status_.hour_active || !MinuteInRange(minute_index)) {
    ++status_.invalid_argument_count;
    return false;
  }
  return true;
}

int RamHourStager::FindSlotByRom_(uint64_t rom64) const {
  for (uint8_t i = 0; i < status_.active_slot_count; ++i) {
    if (slots_[i].active && slots_[i].rom64 == rom64) {
      return i;
    }
  }
  return -1;
}

void RamHourStager::MarkSlotSeen_(HistorySlotDescriptor* slot,
                                  uint32_t node_id,
                                  uint8_t minute_index) {
  if (slot == nullptr) {
    return;
  }
  slot->last_known_node_id = node_id;
  if (minute_index < slot->first_seen_minute) {
    slot->first_seen_minute = minute_index;
  }
  slot->last_seen_minute = minute_index;
}

bool RamHourStager::FindOrCreateSlot(const char* addr16,
                                     uint32_t node_id,
                                     uint8_t minute_index,
                                     uint8_t* out_slot_id) {
  if (out_slot_id != nullptr) {
    *out_slot_id = 0xFFU;
  }
  if (!ValidateMinute_(minute_index)) {
    return false;
  }

  uint64_t rom64 = 0;
  char normalized[17] = {};
  if (!HistoryParseAddr16ToRom64(addr16, &rom64) ||
      !HistoryNormalizeAddr16(addr16, normalized)) {
    ++status_.invalid_argument_count;
    return false;
  }

  const int existing = FindSlotByRom_(rom64);
  if (existing >= 0) {
    HistorySlotDescriptor& slot = slots_[static_cast<std::size_t>(existing)];
    MarkSlotSeen_(&slot, node_id, minute_index);
    if (out_slot_id != nullptr) {
      *out_slot_id = slot.slot_id;
    }
    return true;
  }

  if (status_.active_slot_count >= kHistorySlotCapacity) {
    ++status_.overflow_count;
    status_.overflowed = true;
    return false;
  }

  const uint8_t slot_id = status_.active_slot_count;
  HistorySlotDescriptor& slot = slots_[slot_id];
  slot = HistorySlotDescriptor{};
  slot.slot_id = slot_id;
  slot.active = true;
  std::memcpy(slot.addr16, normalized, sizeof(slot.addr16));
  slot.rom64 = rom64;
  slot.last_known_node_id = node_id;
  slot.first_seen_minute = minute_index;
  slot.last_seen_minute = minute_index;
  ++status_.active_slot_count;

  if (out_slot_id != nullptr) {
    *out_slot_id = slot_id;
  }
  return true;
}

bool RamHourStager::RecordSample(const char* addr16,
                                 uint32_t node_id,
                                 uint8_t minute_index,
                                 float temp_c,
                                 bool corrected) {
  uint8_t slot_id = 0xFFU;
  if (!FindOrCreateSlot(addr16, node_id, minute_index, &slot_id)) {
    return false;
  }

  const HistoryCentiCResult centi = HistoryTempCToCentiC(temp_c);
  if (!centi.valid) {
    ++status_.invalid_temperature_count;
    frames_[minute_index].ClearSample(slot_id);
    ++slots_[slot_id].missing_or_invalid_count;
    ++status_.missing_samples_recorded;
    return false;
  }

  return RecordSampleCentiC(addr16, node_id, minute_index, centi.value, corrected);
}

bool RamHourStager::RecordSampleCentiC(const char* addr16,
                                       uint32_t node_id,
                                       uint8_t minute_index,
                                       int16_t temp_c_x100,
                                       bool corrected) {
  uint8_t slot_id = 0xFFU;
  if (!FindOrCreateSlot(addr16, node_id, minute_index, &slot_id)) {
    return false;
  }

  if (temp_c_x100 == kHistoryInvalidTempCentiC) {
    ++status_.invalid_temperature_count;
    frames_[minute_index].ClearSample(slot_id);
    ++slots_[slot_id].missing_or_invalid_count;
    ++status_.missing_samples_recorded;
    return false;
  }

  if (!frames_[minute_index].SetSample(slot_id, temp_c_x100, corrected)) {
    ++status_.invalid_argument_count;
    return false;
  }

  ++slots_[slot_id].sample_count;
  ++status_.samples_recorded;
  return true;
}

bool RamHourStager::RecordMissing(const char* addr16,
                                  uint32_t node_id,
                                  uint8_t minute_index) {
  uint8_t slot_id = 0xFFU;
  if (!FindOrCreateSlot(addr16, node_id, minute_index, &slot_id)) {
    return false;
  }

  frames_[minute_index].ClearSample(slot_id);
  ++slots_[slot_id].missing_or_invalid_count;
  ++status_.missing_samples_recorded;
  return true;
}

bool RamHourStager::ExportSnapshot(HistoryHourSnapshot* out_snapshot) const {
  if (out_snapshot == nullptr || !status_.hour_active) {
    return false;
  }

  out_snapshot->format_version = kHistoryHourSnapshotFormatVersion;
  out_snapshot->hour_start_epoch_minute = status_.hour_start_epoch_minute;
  out_snapshot->active_slot_count = status_.active_slot_count;
  out_snapshot->slots = slots_;
  out_snapshot->frames = frames_;
  ++status_.exports_completed;
  out_snapshot->status = status_;
  return true;
}

const HistorySlotDescriptor* RamHourStager::slot(uint8_t slot_id) const {
  if (slot_id >= status_.active_slot_count || !slots_[slot_id].active) {
    return nullptr;
  }
  return &slots_[slot_id];
}

const HistoryMinuteFrame* RamHourStager::frame(uint8_t minute_index) const {
  if (!MinuteInRange(minute_index)) {
    return nullptr;
  }
  return &frames_[minute_index];
}
