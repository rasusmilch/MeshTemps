#include "sd_finalized_hour_block.h"

#include <cstring>

#include "history_crc.h"

namespace {

constexpr uint32_t kHeaderCrcOffset = 36U;
constexpr uint32_t kPayloadOffset = kSdFinalizedHourHeaderBytes;

void AppendU8(std::vector<uint8_t>* out, uint8_t value) {
  out->push_back(value);
}

void AppendU16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
}

void AppendU32(std::vector<uint8_t>* out, uint32_t value) {
  for (uint8_t i = 0; i < 4U; ++i) {
    out->push_back(static_cast<uint8_t>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendU64(std::vector<uint8_t>* out, uint64_t value) {
  for (uint8_t i = 0; i < 8U; ++i) {
    out->push_back(static_cast<uint8_t>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendI16(std::vector<uint8_t>* out, int16_t value) {
  AppendU16(out, static_cast<uint16_t>(value));
}

uint16_t ReadU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t ReadU32(const uint8_t* data) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4U; ++i) {
    value |= static_cast<uint32_t>(data[i]) << (8U * i);
  }
  return value;
}

void WriteU32(std::vector<uint8_t>* record, size_t offset, uint32_t value) {
  for (uint8_t i = 0; i < 4U; ++i) {
    (*record)[offset + i] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
  }
}

bool SnapshotIsValid(const HistoryHourSnapshot& snapshot) {
  if (snapshot.format_version != kHistoryHourSnapshotFormatVersion) {
    return false;
  }
  if (snapshot.status.hour_active == false) {
    return false;
  }
  if (snapshot.hour_start_epoch_minute == 0U) {
    return false;
  }
  if (snapshot.active_slot_count > kHistorySlotCapacity) {
    return false;
  }
  if (snapshot.status.active_slot_count != snapshot.active_slot_count) {
    return false;
  }

  for (uint8_t slot_id = 0; slot_id < snapshot.active_slot_count; ++slot_id) {
    const HistorySlotDescriptor& slot = snapshot.slots[slot_id];
    if (!slot.active || slot.slot_id != slot_id || slot.rom64 == 0U) {
      return false;
    }
    if (slot.addr16[16] != '\0') {
      return false;
    }
    uint64_t parsed = 0;
    if (!HistoryParseAddr16ToRom64(slot.addr16, &parsed) || parsed != slot.rom64) {
      return false;
    }
  }
  return true;
}

void AppendHeaderPlaceholder(const HistoryHourSnapshot& snapshot,
                             uint32_t record_bytes,
                             uint32_t descriptor_bytes,
                             uint32_t payload_bytes,
                             uint32_t payload_crc32,
                             std::vector<uint8_t>* out) {
  AppendU32(out, kSdFinalizedHourMagic);
  AppendU16(out, kSdFinalizedHourVersion);
  AppendU16(out, kSdFinalizedHourHeaderBytes);
  AppendU32(out, record_bytes);
  AppendU32(out, snapshot.hour_start_epoch_minute);
  AppendU16(out, snapshot.active_slot_count);
  AppendU16(out, kSdFinalizedHourDescriptorBytes);
  AppendU16(out, kHistoryMinutesPerHour);
  AppendU16(out, kSdFinalizedHourFrameBytes);
  AppendU32(out, descriptor_bytes);
  AppendU32(out, payload_bytes);
  AppendU32(out, payload_crc32);
  AppendU32(out, 0U);  // header_crc32 patched after header is complete.
  AppendU16(out, snapshot.format_version);
  AppendU16(out, 0U);  // reserved0
  AppendU32(out, 0U);  // flags
}

void AppendDescriptors(const HistoryHourSnapshot& snapshot,
                       std::vector<uint8_t>* out) {
  for (uint8_t slot_id = 0; slot_id < snapshot.active_slot_count; ++slot_id) {
    const HistorySlotDescriptor& slot = snapshot.slots[slot_id];
    AppendU8(out, slot.slot_id);
    AppendU8(out, slot.first_seen_minute);
    AppendU8(out, slot.last_seen_minute);
    AppendU8(out, slot.active ? 1U : 0U);
    AppendU32(out, slot.last_known_node_id);
    AppendU64(out, slot.rom64);
    for (uint8_t i = 0; i < 16U; ++i) {
      AppendU8(out, static_cast<uint8_t>(slot.addr16[i]));
    }
  }
}

void AppendFrames(const HistoryHourSnapshot& snapshot, std::vector<uint8_t>* out) {
  for (const HistoryMinuteFrame& frame : snapshot.frames) {
    for (uint8_t value : frame.presence) {
      AppendU8(out, value);
    }
    for (uint8_t value : frame.corrected) {
      AppendU8(out, value);
    }
    for (int16_t value : frame.temp_c_x100) {
      AppendI16(out, value);
    }
  }
}

bool HeaderFieldsAreSane(const SdFinalizedHourBlockHeader& header) {
  if (header.magic != kSdFinalizedHourMagic) return false;
  if (header.version != kSdFinalizedHourVersion) return false;
  if (header.header_bytes != kSdFinalizedHourHeaderBytes) return false;
  if (header.snapshot_format_version != kHistoryHourSnapshotFormatVersion) return false;
  if (header.hour_start_epoch_minute == 0U) return false;
  if (header.active_slot_count > kHistorySlotCapacity) return false;
  if (header.descriptor_entry_bytes != kSdFinalizedHourDescriptorBytes) return false;
  if (header.frame_count != kHistoryMinutesPerHour) return false;
  if (header.frame_bytes != kSdFinalizedHourFrameBytes) return false;

  const uint32_t expected_descriptor_bytes =
      static_cast<uint32_t>(header.active_slot_count) * kSdFinalizedHourDescriptorBytes;
  const uint32_t expected_frame_payload_bytes =
      static_cast<uint32_t>(kHistoryMinutesPerHour) * kSdFinalizedHourFrameBytes;
  if (header.descriptor_bytes != expected_descriptor_bytes) return false;
  if (header.payload_bytes != expected_descriptor_bytes + expected_frame_payload_bytes) {
    return false;
  }
  if (header.record_bytes != header.header_bytes + header.payload_bytes) return false;
  return true;
}

uint32_t ComputeHeaderCrc(const uint8_t* record) {
  uint8_t header[kSdFinalizedHourHeaderBytes];
  std::memcpy(header, record, sizeof(header));
  for (uint8_t i = 0; i < sizeof(uint32_t); ++i) {
    header[kHeaderCrcOffset + i] = 0U;
  }
  return Crc32(header, sizeof(header));
}

}  // namespace

bool EncodeSdFinalizedHourBlock(const HistoryHourSnapshot& snapshot,
                                std::vector<uint8_t>* out_record) {
  if (out_record == nullptr) {
    return false;
  }
  out_record->clear();
  if (!SnapshotIsValid(snapshot)) {
    return false;
  }

  const uint32_t descriptor_bytes =
      static_cast<uint32_t>(snapshot.active_slot_count) * kSdFinalizedHourDescriptorBytes;
  const uint32_t frame_payload_bytes =
      static_cast<uint32_t>(kHistoryMinutesPerHour) * kSdFinalizedHourFrameBytes;
  const uint32_t payload_bytes = descriptor_bytes + frame_payload_bytes;
  const uint32_t record_bytes = kSdFinalizedHourHeaderBytes + payload_bytes;

  out_record->reserve(record_bytes);
  AppendHeaderPlaceholder(snapshot, record_bytes, descriptor_bytes, payload_bytes,
                          0U, out_record);
  AppendDescriptors(snapshot, out_record);
  AppendFrames(snapshot, out_record);

  if (out_record->size() != record_bytes) {
    out_record->clear();
    return false;
  }

  const uint32_t payload_crc32 = Crc32(out_record->data() + kPayloadOffset,
                                      out_record->size() - kPayloadOffset);
  WriteU32(out_record, 32U, payload_crc32);
  const uint32_t header_crc32 = ComputeHeaderCrc(out_record->data());
  WriteU32(out_record, kHeaderCrcOffset, header_crc32);
  return true;
}

bool DecodeSdFinalizedHourBlockHeader(const uint8_t* record,
                                      size_t record_length,
                                      SdFinalizedHourBlockHeader* out_header) {
  if (record == nullptr || out_header == nullptr ||
      record_length < kSdFinalizedHourHeaderBytes) {
    return false;
  }

  SdFinalizedHourBlockHeader header;
  header.magic = ReadU32(record + 0U);
  header.version = ReadU16(record + 4U);
  header.header_bytes = ReadU16(record + 6U);
  header.record_bytes = ReadU32(record + 8U);
  header.hour_start_epoch_minute = ReadU32(record + 12U);
  header.active_slot_count = ReadU16(record + 16U);
  header.descriptor_entry_bytes = ReadU16(record + 18U);
  header.frame_count = ReadU16(record + 20U);
  header.frame_bytes = ReadU16(record + 22U);
  header.descriptor_bytes = ReadU32(record + 24U);
  header.payload_bytes = ReadU32(record + 28U);
  header.payload_crc32 = ReadU32(record + 32U);
  header.header_crc32 = ReadU32(record + 36U);
  header.snapshot_format_version = ReadU16(record + 40U);
  header.reserved0 = ReadU16(record + 42U);
  header.flags = ReadU32(record + 44U);

  if (!HeaderFieldsAreSane(header)) {
    return false;
  }
  *out_header = header;
  return true;
}

bool VerifySdFinalizedHourBlock(const uint8_t* record,
                                size_t record_length,
                                SdFinalizedHourBlockHeader* out_header) {
  SdFinalizedHourBlockHeader header;
  if (!DecodeSdFinalizedHourBlockHeader(record, record_length, &header)) {
    return false;
  }
  if (record_length < header.record_bytes) {
    return false;
  }
  if (ComputeHeaderCrc(record) != header.header_crc32) {
    return false;
  }
  const uint32_t payload_crc = Crc32(record + kPayloadOffset, header.payload_bytes);
  if (payload_crc != header.payload_crc32) {
    return false;
  }
  if (out_header != nullptr) {
    *out_header = header;
  }
  return true;
}

bool VerifySdFinalizedHourBlock(const std::vector<uint8_t>& record,
                                SdFinalizedHourBlockHeader* out_header) {
  return VerifySdFinalizedHourBlock(record.data(), record.size(), out_header);
}
