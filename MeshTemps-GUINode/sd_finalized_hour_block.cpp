#include "sd_finalized_hour_block.h"

#include <cstring>

#include "history_crc.h"

namespace {

constexpr uint32_t kHeaderCrcOffset = 36U;
constexpr uint32_t kPayloadOffset = kSdFinalizedHourHeaderBytes;

void PutU8(uint8_t* out, size_t* offset, uint8_t value) {
  out[(*offset)++] = value;
}

void PutU16(uint8_t* out, size_t* offset, uint16_t value) {
  out[(*offset)++] = static_cast<uint8_t>(value & 0xFFU);
  out[(*offset)++] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

void PutU32(uint8_t* out, size_t* offset, uint32_t value) {
  for (uint8_t i = 0; i < 4U; ++i) {
    out[(*offset)++] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
  }
}

void PutU64(uint8_t* out, size_t* offset, uint64_t value) {
  for (uint8_t i = 0; i < 8U; ++i) {
    out[(*offset)++] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
  }
}

void PutI16(uint8_t* out, size_t* offset, int16_t value) {
  PutU16(out, offset, static_cast<uint16_t>(value));
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

void WriteU32(uint8_t* record, size_t offset, uint32_t value) {
  for (uint8_t i = 0; i < 4U; ++i) {
    record[offset + i] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
  }
}

bool SourceIsValid(const ISdFinalizedHourSource& source) {
  if (source.format_version() != kHistoryHourSnapshotFormatVersion) {
    return false;
  }
  if (!source.hour_active()) {
    return false;
  }
  if (source.hour_start_epoch_minute() == 0U) {
    return false;
  }
  if (source.active_slot_count() > kHistorySlotCapacity) {
    return false;
  }

  const HistoryStagerStatus status = source.status();
  if (!status.hour_active) {
    return false;
  }
  if (status.hour_start_epoch_minute != source.hour_start_epoch_minute()) {
    return false;
  }
  if (status.active_slot_count != source.active_slot_count()) {
    return false;
  }

  for (uint16_t slot_index = 0; slot_index < source.active_slot_count(); ++slot_index) {
    const uint8_t slot_id = static_cast<uint8_t>(slot_index);
    const HistorySlotDescriptor* slot = source.slot(slot_id);
    if (slot == nullptr || !slot->active || slot->slot_id != slot_id ||
        slot->rom64 == 0U) {
      return false;
    }
    if (slot->addr16[16] != '\0') {
      return false;
    }
    uint64_t parsed = 0;
    if (!HistoryParseAddr16ToRom64(slot->addr16, &parsed) ||
        parsed != slot->rom64) {
      return false;
    }
  }

  for (uint8_t minute_index = 0; minute_index < kHistoryMinutesPerHour;
       ++minute_index) {
    if (source.frame(minute_index) == nullptr) {
      return false;
    }
  }
  return true;
}

uint32_t DescriptorBytesFor(const ISdFinalizedHourSource& source) {
  return static_cast<uint32_t>(source.active_slot_count()) *
         kSdFinalizedHourDescriptorBytes;
}

uint32_t FramePayloadBytes() {
  return static_cast<uint32_t>(kHistoryMinutesPerHour) *
         kSdFinalizedHourFrameBytes;
}

void FillHeader(const ISdFinalizedHourSource& source,
                uint32_t payload_crc32,
                uint8_t* out) {
  const uint32_t descriptor_bytes = DescriptorBytesFor(source);
  const uint32_t payload_bytes = descriptor_bytes + FramePayloadBytes();
  const uint32_t record_bytes = kSdFinalizedHourHeaderBytes + payload_bytes;

  size_t offset = 0;
  PutU32(out, &offset, kSdFinalizedHourMagic);
  PutU16(out, &offset, kSdFinalizedHourVersion);
  PutU16(out, &offset, kSdFinalizedHourHeaderBytes);
  PutU32(out, &offset, record_bytes);
  PutU32(out, &offset, source.hour_start_epoch_minute());
  PutU16(out, &offset, source.active_slot_count());
  PutU16(out, &offset, kSdFinalizedHourDescriptorBytes);
  PutU16(out, &offset, kHistoryMinutesPerHour);
  PutU16(out, &offset, kSdFinalizedHourFrameBytes);
  PutU32(out, &offset, descriptor_bytes);
  PutU32(out, &offset, payload_bytes);
  PutU32(out, &offset, payload_crc32);
  PutU32(out, &offset, 0U);  // header_crc32 patched after header is complete.
  PutU16(out, &offset, source.format_version());
  PutU16(out, &offset, 0U);  // reserved0
  PutU32(out, &offset, 0U);  // flags
}

void FillDescriptor(const HistorySlotDescriptor& slot, uint8_t* out) {
  size_t offset = 0;
  PutU8(out, &offset, slot.slot_id);
  PutU8(out, &offset, slot.first_seen_minute);
  PutU8(out, &offset, slot.last_seen_minute);
  PutU8(out, &offset, slot.active ? 1U : 0U);
  PutU32(out, &offset, slot.last_known_node_id);
  PutU64(out, &offset, slot.rom64);
  for (uint8_t i = 0; i < 16U; ++i) {
    PutU8(out, &offset, static_cast<uint8_t>(slot.addr16[i]));
  }
}

void FillFrame(const HistoryMinuteFrame& frame, uint8_t* out) {
  size_t offset = 0;
  for (uint8_t value : frame.presence) {
    PutU8(out, &offset, value);
  }
  for (uint8_t value : frame.corrected) {
    PutU8(out, &offset, value);
  }
  for (int16_t value : frame.temp_c_x100) {
    PutI16(out, &offset, value);
  }
}

bool EmitPayload(const ISdFinalizedHourSource& source,
                 SdFinalizedHourWriteFn write_fn,
                 void* ctx) {
  uint8_t descriptor[kSdFinalizedHourDescriptorBytes];
  for (uint16_t slot_index = 0; slot_index < source.active_slot_count();
       ++slot_index) {
    const HistorySlotDescriptor* slot = source.slot(static_cast<uint8_t>(slot_index));
    if (slot == nullptr) return false;
    FillDescriptor(*slot, descriptor);
    if (!write_fn(descriptor, sizeof(descriptor), ctx)) return false;
  }

  uint8_t frame[kSdFinalizedHourFrameBytes];
  for (uint8_t minute_index = 0; minute_index < kHistoryMinutesPerHour;
       ++minute_index) {
    const HistoryMinuteFrame* minute_frame = source.frame(minute_index);
    if (minute_frame == nullptr) return false;
    FillFrame(*minute_frame, frame);
    if (!write_fn(frame, sizeof(frame), ctx)) return false;
  }
  return true;
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

bool CrcSink(const uint8_t* data, size_t len, void* ctx) {
  uint32_t* crc = static_cast<uint32_t*>(ctx);
  *crc = Crc32Update(*crc, data, len);
  return true;
}

}  // namespace

bool WriteSdFinalizedHourBlock(const ISdFinalizedHourSource& source,
                               SdFinalizedHourWriteFn write_fn,
                               void* ctx,
                               SdFinalizedHourBlockHeader* out_header,
                               SdFinalizedHourWriteStatus* out_status) {
  if (write_fn == nullptr) return false;
  if (!SourceIsValid(source)) return false;

  uint32_t payload_crc32 = 0xFFFFFFFFu;
  if (!EmitPayload(source, CrcSink, &payload_crc32)) return false;

  uint8_t header[kSdFinalizedHourHeaderBytes];
  FillHeader(source, payload_crc32, header);
  const uint32_t header_crc32 = ComputeSdFinalizedHourHeaderCrc32(
      header, sizeof(header));
  WriteU32(header, kHeaderCrcOffset, header_crc32);

  SdFinalizedHourBlockHeader decoded_header;
  if (!DecodeSdFinalizedHourBlockHeader(header, sizeof(header), &decoded_header)) {
    return false;
  }
  if (!VerifySdFinalizedHourBlockHeaderCrc(header, sizeof(header))) {
    return false;
  }

  if (!write_fn(header, sizeof(header), ctx)) return false;
  if (!EmitPayload(source, write_fn, ctx)) return false;

  if (out_header != nullptr) {
    *out_header = decoded_header;
  }
  if (out_status != nullptr) {
    out_status->bytes_written = decoded_header.record_bytes;
    out_status->payload_bytes = decoded_header.payload_bytes;
    out_status->payload_crc32 = decoded_header.payload_crc32;
    out_status->header_crc32 = decoded_header.header_crc32;
  }
  return true;
}

bool WriteSdFinalizedHourBlock(const HistoryHourSnapshot& snapshot,
                               SdFinalizedHourWriteFn write_fn,
                               void* ctx,
                               SdFinalizedHourBlockHeader* out_header,
                               SdFinalizedHourWriteStatus* out_status) {
  const HistoryHourSnapshotFinalizedHourSource source(snapshot);
  return WriteSdFinalizedHourBlock(source, write_fn, ctx, out_header, out_status);
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

uint32_t ComputeSdFinalizedHourHeaderCrc32(const uint8_t* header_bytes,
                                           size_t header_length) {
  if (header_bytes == nullptr || header_length < kSdFinalizedHourHeaderBytes) {
    return 0U;
  }
  uint8_t header[kSdFinalizedHourHeaderBytes];
  std::memcpy(header, header_bytes, sizeof(header));
  for (uint8_t i = 0; i < sizeof(uint32_t); ++i) {
    header[kHeaderCrcOffset + i] = 0U;
  }
  return Crc32(header, sizeof(header));
}

bool VerifySdFinalizedHourBlockHeaderCrc(const uint8_t* header_bytes,
                                         size_t header_length) {
  SdFinalizedHourBlockHeader header;
  if (!DecodeSdFinalizedHourBlockHeader(header_bytes, header_length, &header)) {
    return false;
  }
  return ComputeSdFinalizedHourHeaderCrc32(header_bytes, header_length) ==
         header.header_crc32;
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
  if (!VerifySdFinalizedHourBlockHeaderCrc(record, kSdFinalizedHourHeaderBytes)) {
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
