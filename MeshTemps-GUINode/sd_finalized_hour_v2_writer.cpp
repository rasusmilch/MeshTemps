#include "sd_finalized_hour_v2_writer.h"

#include <cstring>

#include "history_crc.h"

namespace {

struct SensorWorkEntry {
  uint8_t slot_index = 0;
  uint64_t rom64 = 0;
  uint16_t valid_sample_count = 0;
  uint16_t missing_or_invalid_count = 0;
  uint16_t corrected_sample_count = 0;
  uint16_t sanitized_corrected_count = 0;
};

uint8_t MinuteMask(uint8_t minute) {
  return static_cast<uint8_t>(1U << (minute % 8U));
}

void SetFailure(SdFinalizedHourV2WriteStatus* status,
                SdFinalizedHourV2WriteFailureReason reason) {
  if (status != nullptr) status->failure_reason = reason;
}

bool WriteAll(SdFinalizedHourV2WriteFn write_fn,
              void* ctx,
              const uint8_t* data,
              size_t len,
              SdFinalizedHourV2WriteStatus* status) {
  if (len == 0U) return true;
  if (write_fn == nullptr || data == nullptr) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSinkFailure);
    return false;
  }
  if (!write_fn(data, len, ctx)) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSinkFailure);
    return false;
  }
  if (status != nullptr) {
    status->bytes_written += static_cast<uint32_t>(len);
  }
  return true;
}

bool SlotCanBeEmitted(const HistorySlotDescriptor& slot) {
  return slot.active && slot.rom64 != 0U &&
         (slot.sample_count != 0U || slot.missing_or_invalid_count != 0U);
}

uint16_t CountSnapshotPresence(const HistoryHourSnapshot& snapshot,
                               uint8_t slot_index) {
  uint16_t count = 0;
  for (uint8_t minute = 0; minute < kHistoryMinutesPerHour; ++minute) {
    if (snapshot.frames[minute].IsPresent(slot_index)) ++count;
  }
  return count;
}

uint16_t CountSnapshotCorrected(const HistoryHourSnapshot& snapshot,
                                uint8_t slot_index,
                                uint16_t* sanitized_count) {
  uint16_t count = 0;
  uint16_t sanitized = 0;
  for (uint8_t minute = 0; minute < kHistoryMinutesPerHour; ++minute) {
    const HistoryMinuteFrame& frame = snapshot.frames[minute];
    const bool raw_corrected =
        (frame.corrected[slot_index / 8U] &
         static_cast<uint8_t>(1U << (slot_index % 8U))) != 0U;
    if (frame.IsPresent(slot_index)) {
      if (raw_corrected) ++count;
    } else if (raw_corrected) {
      ++sanitized;
    }
  }
  if (sanitized_count != nullptr) *sanitized_count = sanitized;
  return count;
}

bool PrepareSensorWork(const HistoryHourSnapshot& snapshot,
                       SensorWorkEntry* entries,
                       uint16_t* entry_count,
                       SdFinalizedHourV2WriteStatus* status) {
  if (entries == nullptr || entry_count == nullptr) return false;
  *entry_count = 0;
  if (snapshot.format_version != kHistoryHourSnapshotFormatVersion ||
      !snapshot.status.hour_active ||
      snapshot.active_slot_count > kHistorySlotCapacity) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kUnsupportedSnapshot);
    return false;
  }

  for (uint8_t slot_index = 0; slot_index < snapshot.active_slot_count; ++slot_index) {
    const HistorySlotDescriptor& slot = snapshot.slots[slot_index];
    if (!SlotCanBeEmitted(slot)) continue;
    for (uint16_t i = 0; i < *entry_count; ++i) {
      if (entries[i].rom64 == slot.rom64) {
        SetFailure(status, SdFinalizedHourV2WriteFailureReason::kDuplicateRom64);
        return false;
      }
    }
    SensorWorkEntry entry;
    entry.slot_index = slot_index;
    entry.rom64 = slot.rom64;
    entry.valid_sample_count = CountSnapshotPresence(snapshot, slot_index);
    uint16_t sanitized = 0;
    entry.corrected_sample_count =
        CountSnapshotCorrected(snapshot, slot_index, &sanitized);
    entry.sanitized_corrected_count = sanitized;
    const uint32_t missing = slot.missing_or_invalid_count;
    entry.missing_or_invalid_count =
        (missing > 0xFFFFU) ? 0xFFFFU : static_cast<uint16_t>(missing);
    entries[*entry_count] = entry;
    ++(*entry_count);
  }

  for (uint16_t i = 1; i < *entry_count; ++i) {
    const SensorWorkEntry key = entries[i];
    uint16_t j = i;
    while (j > 0U && entries[j - 1U].rom64 > key.rom64) {
      entries[j] = entries[j - 1U];
      --j;
    }
    entries[j] = key;
  }
  return true;
}

bool CopyLabelToDescriptor(const ISdFinalizedHourV2LabelSource* labels,
                           const SensorWorkEntry& entry,
                           const HistorySlotDescriptor& slot,
                           bool node_label,
                           SdFinalizedHourV2Descriptor* descriptor) {
  if (descriptor == nullptr) return false;
  uint8_t* out = node_label ? descriptor->node_label : descriptor->sensor_label;
  const size_t out_size = node_label ? kSdFinalizedHourV2NodeLabelMaxBytes
                                     : kSdFinalizedHourV2SensorLabelMaxBytes;
  std::memset(out, 0, out_size);
  if (labels == nullptr) return true;

  size_t out_len = 0;
  bool truncated = false;
  const bool ok = node_label
      ? labels->CopyNodeLabel(slot.last_known_node_id, out, out_size, &out_len,
                              &truncated)
      : labels->CopySensorLabel(entry.rom64, slot.last_known_node_id, out,
                                out_size, &out_len, &truncated);
  if (!ok || out_len > out_size) return false;
  if (node_label) {
    descriptor->node_label_len = static_cast<uint8_t>(out_len);
    if (truncated) {
      descriptor->descriptor_flags |=
          kSdFinalizedHourV2DescriptorFlagNodeLabelTruncated;
    }
  } else {
    descriptor->sensor_label_len = static_cast<uint8_t>(out_len);
    if (truncated) {
      descriptor->descriptor_flags |=
          kSdFinalizedHourV2DescriptorFlagSensorLabelTruncated;
    }
  }
  return true;
}

bool BuildDescriptor(const HistoryHourSnapshot& snapshot,
                     const SensorWorkEntry& entry,
                     const ISdFinalizedHourV2LabelSource* labels,
                     SdFinalizedHourV2Descriptor* descriptor) {
  if (descriptor == nullptr || entry.slot_index >= kHistorySlotCapacity) return false;
  const HistorySlotDescriptor& slot = snapshot.slots[entry.slot_index];
  *descriptor = SdFinalizedHourV2Descriptor{};
  descriptor->rom64 = entry.rom64;
  descriptor->last_known_node_id = slot.last_known_node_id;
  descriptor->first_seen_minute = slot.first_seen_minute;
  descriptor->last_seen_minute = slot.last_seen_minute;
  descriptor->valid_sample_count = entry.valid_sample_count;
  descriptor->missing_or_invalid_count = entry.missing_or_invalid_count;
  descriptor->corrected_sample_count = entry.corrected_sample_count;
  return CopyLabelToDescriptor(labels, entry, slot, true, descriptor) &&
         CopyLabelToDescriptor(labels, entry, slot, false, descriptor);
}

bool BuildPayload(const HistoryHourSnapshot& snapshot,
                  const SensorWorkEntry& entry,
                  SdFinalizedHourV2Payload* payload) {
  if (payload == nullptr) return false;
  *payload = SdFinalizedHourV2Payload{};
  for (uint8_t minute = 0; minute < kHistoryMinutesPerHour; ++minute) {
    const HistoryMinuteFrame& frame = snapshot.frames[minute];
    if (frame.IsPresent(entry.slot_index)) {
      payload->presence_bitmap[minute / 8U] |= MinuteMask(minute);
      if (frame.IsCorrected(entry.slot_index)) {
        payload->corrected_bitmap[minute / 8U] |= MinuteMask(minute);
      }
      payload->samples[minute] = frame.TemperatureCentiC(entry.slot_index);
    }
  }
  FillMissingSdFinalizedHourV2Samples(payload);
  return SdFinalizedHourV2PayloadBitmapsAreValid(*payload);
}

bool BuildFinalBlock(const HistoryHourSnapshot& snapshot,
                     const SensorWorkEntry& entry,
                     const ISdFinalizedHourV2LabelSource* labels,
                     uint8_t* block,
                     size_t block_size) {
  if (block == nullptr || block_size < kSdFinalizedHourV2FixedBlockBytes) {
    return false;
  }

  SdFinalizedHourV2Descriptor descriptor;
  SdFinalizedHourV2Payload payload;
  if (!BuildDescriptor(snapshot, entry, labels, &descriptor) ||
      !BuildPayload(snapshot, entry, &payload)) {
    return false;
  }

  SdFinalizedHourV2BlockHeader block_header;
  block_header.block_crc32 = 0;
  if (!EncodeSdFinalizedHourV2BlockHeader(block_header, block,
                                          kSdFinalizedHourV2BlockHeaderBytes) ||
      !EncodeSdFinalizedHourV2Descriptor(
          descriptor, block + kSdFinalizedHourV2BlockHeaderBytes,
          kSdFinalizedHourV2DescriptorBytes) ||
      !EncodeSdFinalizedHourV2Payload(
          payload,
          block + kSdFinalizedHourV2BlockHeaderBytes +
              kSdFinalizedHourV2DescriptorBytes,
          kSdFinalizedHourV2PayloadBytes)) {
    return false;
  }

  block_header.block_crc32 =
      ComputeSdFinalizedHourV2BlockCrc32(block,
                                         kSdFinalizedHourV2FixedBlockBytes);
  return EncodeSdFinalizedHourV2BlockHeader(
      block_header, block, kSdFinalizedHourV2BlockHeaderBytes);
}

bool EncodeIndexEntry(const SensorWorkEntry& entry,
                      uint16_t sorted_index,
                      uint16_t sensor_count,
                      uint8_t* out,
                      size_t out_size) {
  if (out == nullptr || out_size < kSdFinalizedHourV2IndexEntryBytes) return false;
  SdFinalizedHourV2IndexEntry index_entry;
  index_entry.rom64 = entry.rom64;
  index_entry.sensor_block_offset_from_record_start =
      static_cast<uint32_t>(kSdFinalizedHourV2HeaderBytes) +
      (static_cast<uint32_t>(sensor_count) * kSdFinalizedHourV2IndexEntryBytes) +
      (static_cast<uint32_t>(sorted_index) * kSdFinalizedHourV2FixedBlockBytes);
  return EncodeSdFinalizedHourV2IndexEntry(index_entry, out, out_size);
}

bool ComputePayloadCrc(const HistoryHourSnapshot& snapshot,
                       const SensorWorkEntry* entries,
                       uint16_t sensor_count,
                       const ISdFinalizedHourV2LabelSource* labels,
                       uint32_t* out_crc) {
  if (entries == nullptr || out_crc == nullptr) return false;
  uint8_t index_bytes[kSdFinalizedHourV2IndexEntryBytes] = {};
  uint8_t block[kSdFinalizedHourV2FixedBlockBytes] = {};
  uint32_t crc = Crc32IsoHdlcBegin();
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!EncodeIndexEntry(entries[i], i, sensor_count, index_bytes,
                          sizeof(index_bytes))) {
      return false;
    }
    crc = Crc32IsoHdlcUpdate(crc, index_bytes, sizeof(index_bytes));
  }
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!BuildFinalBlock(snapshot, entries[i], labels, block, sizeof(block))) {
      return false;
    }
    crc = Crc32IsoHdlcUpdate(crc, block, kSdFinalizedHourV2FixedBlockBytes);
  }
  *out_crc = Crc32IsoHdlcFinalize(crc);
  return true;
}

bool BuildHeader(uint32_t hour_start_epoch_minute,
                 uint16_t sensor_count,
                 uint32_t payload_crc,
                 uint8_t* header_bytes,
                 size_t header_size,
                 SdFinalizedHourV2WriteStatus* status) {
  if (header_bytes == nullptr || header_size < kSdFinalizedHourV2HeaderBytes) {
    return false;
  }
  SdFinalizedHourV2Header header;
  header.hour_start_epoch_minute = hour_start_epoch_minute;
  header.sensor_count = sensor_count;
  header.index_offset = kSdFinalizedHourV2HeaderBytes;
  header.index_bytes = static_cast<uint32_t>(sensor_count) *
                       kSdFinalizedHourV2IndexEntryBytes;
  header.sensor_blocks_offset = kSdFinalizedHourV2HeaderBytes + header.index_bytes;
  header.sensor_blocks_bytes = static_cast<uint32_t>(sensor_count) *
                               kSdFinalizedHourV2FixedBlockBytes;
  header.record_bytes = kSdFinalizedHourV2HeaderBytes + header.index_bytes +
                        header.sensor_blocks_bytes;
  header.payload_crc32 = payload_crc;
  header.header_crc32 = 0;
  if (!EncodeSdFinalizedHourV2Header(header, header_bytes, header_size)) return false;
  header.header_crc32 =
      ComputeSdFinalizedHourV2HeaderCrc32(header_bytes,
                                          kSdFinalizedHourV2HeaderBytes);
  if (!EncodeSdFinalizedHourV2Header(header, header_bytes, header_size)) return false;
  if (status != nullptr) {
    status->sensor_count = sensor_count;
    status->record_bytes = header.record_bytes;
    status->payload_crc32 = payload_crc;
    status->header_crc32 = header.header_crc32;
  }
  return true;
}

}  // namespace

bool WriteSdFinalizedHourV2Record(
    const HistoryHourSnapshot& snapshot,
    const ISdFinalizedHourV2LabelSource* labels,
    SdFinalizedHourV2WriteFn write_fn,
    void* ctx,
    SdFinalizedHourV2WriteStatus* out_status) {
  SdFinalizedHourV2WriteStatus local_status;
  SdFinalizedHourV2WriteStatus* status =
      (out_status != nullptr) ? out_status : &local_status;
  *status = SdFinalizedHourV2WriteStatus{};

  SensorWorkEntry entries[kHistorySlotCapacity] = {};
  uint16_t sensor_count = 0;
  if (!PrepareSensorWork(snapshot, entries, &sensor_count, status)) {
    return false;
  }

  for (uint16_t i = 0; i < sensor_count; ++i) {
    status->corrected_without_presence_sanitized +=
        entries[i].sanitized_corrected_count;
  }

  if (sensor_count == 0U) {
    status->skipped_zero_sensor_hour = true;
    status->failure_reason = SdFinalizedHourV2WriteFailureReason::kNone;
    return true;
  }

  uint32_t payload_crc = 0;
  if (!ComputePayloadCrc(snapshot, entries, sensor_count, labels, &payload_crc)) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSerializationFailure);
    return false;
  }

  uint8_t header_bytes[kSdFinalizedHourV2HeaderBytes] = {};
  if (!BuildHeader(snapshot.hour_start_epoch_minute, sensor_count, payload_crc,
                   header_bytes, sizeof(header_bytes), status)) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSerializationFailure);
    return false;
  }

  uint8_t index_bytes[kSdFinalizedHourV2IndexEntryBytes] = {};
  uint8_t block[kSdFinalizedHourV2FixedBlockBytes] = {};
  if (!WriteAll(write_fn, ctx, header_bytes, sizeof(header_bytes), status)) {
    return false;
  }
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!EncodeIndexEntry(entries[i], i, sensor_count, index_bytes,
                          sizeof(index_bytes))) {
      SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSerializationFailure);
      return false;
    }
    if (!WriteAll(write_fn, ctx, index_bytes, sizeof(index_bytes), status)) {
      return false;
    }
  }
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!BuildFinalBlock(snapshot, entries[i], labels, block, sizeof(block))) {
      SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSerializationFailure);
      return false;
    }
    if (!WriteAll(write_fn, ctx, block, kSdFinalizedHourV2FixedBlockBytes,
                  status)) {
      return false;
    }
  }

  if (status->bytes_written != status->record_bytes) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSinkFailure);
    return false;
  }
  return true;
}
