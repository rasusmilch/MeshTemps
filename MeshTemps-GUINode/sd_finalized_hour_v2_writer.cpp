#include "sd_finalized_hour_v2_writer.h"

#include <cstring>

#include "history_crc.h"

namespace {

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
                       SdFinalizedHourV2WriterWorkspace& workspace,
                       uint16_t* entry_count,
                       SdFinalizedHourV2WriteStatus* status) {
  if (entry_count == nullptr) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kInvalidArgument);
    return false;
  }
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
      if (workspace.entries[i].rom64 == slot.rom64) {
        SetFailure(status, SdFinalizedHourV2WriteFailureReason::kDuplicateRom64);
        return false;
      }
    }
    SdFinalizedHourV2WriterSensorWorkEntry entry;
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
    workspace.entries[*entry_count] = entry;
    ++(*entry_count);
  }

  for (uint16_t i = 1; i < *entry_count; ++i) {
    const SdFinalizedHourV2WriterSensorWorkEntry key = workspace.entries[i];
    uint16_t j = i;
    while (j > 0U && workspace.entries[j - 1U].rom64 > key.rom64) {
      workspace.entries[j] = workspace.entries[j - 1U];
      --j;
    }
    workspace.entries[j] = key;
  }
  return true;
}

bool CopyOneLabelSnapshot(const ISdFinalizedHourV2LabelSource* labels,
                          const SdFinalizedHourV2WriterSensorWorkEntry& entry,
                          const HistorySlotDescriptor& slot,
                          bool node_label,
                          SdFinalizedHourV2WriterLabelSnapshot* snapshot) {
  if (snapshot == nullptr) return false;
  uint8_t* out = node_label ? snapshot->node_label : snapshot->sensor_label;
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
    snapshot->node_label_len = static_cast<uint8_t>(out_len);
    snapshot->node_label_truncated = truncated;
  } else {
    snapshot->sensor_label_len = static_cast<uint8_t>(out_len);
    snapshot->sensor_label_truncated = truncated;
  }
  return true;
}

bool SnapshotLabels(const HistoryHourSnapshot& snapshot,
                    const ISdFinalizedHourV2LabelSource* labels,
                    SdFinalizedHourV2WriterWorkspace& workspace,
                    uint16_t sensor_count,
                    SdFinalizedHourV2WriteStatus* status) {
  for (uint16_t i = 0; i < sensor_count; ++i) {
    workspace.labels[i] = SdFinalizedHourV2WriterLabelSnapshot{};
    const SdFinalizedHourV2WriterSensorWorkEntry& entry = workspace.entries[i];
    if (entry.slot_index >= kHistorySlotCapacity) {
      SetFailure(status, SdFinalizedHourV2WriteFailureReason::kInvalidSnapshot);
      return false;
    }
    const HistorySlotDescriptor& slot = snapshot.slots[entry.slot_index];
    if (!CopyOneLabelSnapshot(labels, entry, slot, true, &workspace.labels[i]) ||
        !CopyOneLabelSnapshot(labels, entry, slot, false, &workspace.labels[i])) {
      SetFailure(status, SdFinalizedHourV2WriteFailureReason::kLabelSourceFailure);
      return false;
    }
  }
  return true;
}

bool BuildDescriptor(const HistoryHourSnapshot& snapshot,
                     const SdFinalizedHourV2WriterSensorWorkEntry& entry,
                     const SdFinalizedHourV2WriterLabelSnapshot& labels,
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
  descriptor->node_label_len = labels.node_label_len;
  descriptor->sensor_label_len = labels.sensor_label_len;
  if (labels.node_label_truncated) {
    descriptor->descriptor_flags |=
        kSdFinalizedHourV2DescriptorFlagNodeLabelTruncated;
  }
  if (labels.sensor_label_truncated) {
    descriptor->descriptor_flags |=
        kSdFinalizedHourV2DescriptorFlagSensorLabelTruncated;
  }
  std::memcpy(descriptor->node_label, labels.node_label,
              kSdFinalizedHourV2NodeLabelMaxBytes);
  std::memcpy(descriptor->sensor_label, labels.sensor_label,
              kSdFinalizedHourV2SensorLabelMaxBytes);
  return true;
}

bool BuildPayload(const HistoryHourSnapshot& snapshot,
                  const SdFinalizedHourV2WriterSensorWorkEntry& entry,
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
                     const SdFinalizedHourV2WriterSensorWorkEntry& entry,
                     const SdFinalizedHourV2WriterLabelSnapshot& labels,
                     SdFinalizedHourV2WriterWorkspace& workspace) {
  if (!BuildDescriptor(snapshot, entry, labels, &workspace.descriptor) ||
      !BuildPayload(snapshot, entry, &workspace.payload)) {
    return false;
  }

  workspace.block_header = SdFinalizedHourV2BlockHeader{};
  workspace.block_header.block_crc32 = 0;
  if (!EncodeSdFinalizedHourV2BlockHeader(
          workspace.block_header, workspace.block,
          kSdFinalizedHourV2BlockHeaderBytes) ||
      !EncodeSdFinalizedHourV2Descriptor(
          workspace.descriptor,
          workspace.block + kSdFinalizedHourV2BlockHeaderBytes,
          kSdFinalizedHourV2DescriptorBytes) ||
      !EncodeSdFinalizedHourV2Payload(
          workspace.payload,
          workspace.block + kSdFinalizedHourV2BlockHeaderBytes +
              kSdFinalizedHourV2DescriptorBytes,
          kSdFinalizedHourV2PayloadBytes)) {
    return false;
  }

  workspace.block_header.block_crc32 = ComputeSdFinalizedHourV2BlockCrc32(
      workspace.block, kSdFinalizedHourV2FixedBlockBytes);
  return EncodeSdFinalizedHourV2BlockHeader(
      workspace.block_header, workspace.block, kSdFinalizedHourV2BlockHeaderBytes);
}

bool EncodeIndexEntry(const SdFinalizedHourV2WriterSensorWorkEntry& entry,
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
                       SdFinalizedHourV2WriterWorkspace& workspace,
                       uint16_t sensor_count,
                       uint32_t* out_crc) {
  if (out_crc == nullptr) return false;
  uint32_t crc = Crc32IsoHdlcBegin();
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!EncodeIndexEntry(workspace.entries[i], i, sensor_count,
                          workspace.index_bytes,
                          sizeof(workspace.index_bytes))) {
      return false;
    }
    crc = Crc32IsoHdlcUpdate(crc, workspace.index_bytes,
                             sizeof(workspace.index_bytes));
  }
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!BuildFinalBlock(snapshot, workspace.entries[i], workspace.labels[i],
                         workspace)) {
      return false;
    }
    crc = Crc32IsoHdlcUpdate(crc, workspace.block,
                             kSdFinalizedHourV2FixedBlockBytes);
  }
  *out_crc = Crc32IsoHdlcFinalize(crc);
  return true;
}

bool BuildHeader(uint32_t hour_start_epoch_minute,
                 uint16_t sensor_count,
                 uint32_t payload_crc,
                 SdFinalizedHourV2WriterWorkspace& workspace,
                 SdFinalizedHourV2WriteStatus* status) {
  workspace.header = SdFinalizedHourV2Header{};
  workspace.header.hour_start_epoch_minute = hour_start_epoch_minute;
  workspace.header.sensor_count = sensor_count;
  workspace.header.index_offset = kSdFinalizedHourV2HeaderBytes;
  workspace.header.index_bytes = static_cast<uint32_t>(sensor_count) *
                                 kSdFinalizedHourV2IndexEntryBytes;
  workspace.header.sensor_blocks_offset =
      kSdFinalizedHourV2HeaderBytes + workspace.header.index_bytes;
  workspace.header.sensor_blocks_bytes = static_cast<uint32_t>(sensor_count) *
                                         kSdFinalizedHourV2FixedBlockBytes;
  workspace.header.record_bytes = kSdFinalizedHourV2HeaderBytes +
                                  workspace.header.index_bytes +
                                  workspace.header.sensor_blocks_bytes;
  workspace.header.payload_crc32 = payload_crc;
  workspace.header.header_crc32 = 0;
  if (!EncodeSdFinalizedHourV2Header(workspace.header, workspace.header_bytes,
                                     sizeof(workspace.header_bytes))) {
    return false;
  }
  workspace.header.header_crc32 = ComputeSdFinalizedHourV2HeaderCrc32(
      workspace.header_bytes, kSdFinalizedHourV2HeaderBytes);
  if (!EncodeSdFinalizedHourV2Header(workspace.header, workspace.header_bytes,
                                     sizeof(workspace.header_bytes))) {
    return false;
  }
  if (status != nullptr) {
    status->sensor_count = sensor_count;
    status->record_bytes = workspace.header.record_bytes;
    status->payload_crc32 = payload_crc;
    status->header_crc32 = workspace.header.header_crc32;
  }
  return true;
}

}  // namespace

bool WriteSdFinalizedHourV2Record(
    const HistoryHourSnapshot& snapshot,
    const ISdFinalizedHourV2LabelSource* labels,
    SdFinalizedHourV2WriterWorkspace& workspace,
    SdFinalizedHourV2WriteFn write_fn,
    void* ctx,
    SdFinalizedHourV2WriteStatus* out_status) {
  SdFinalizedHourV2WriteStatus local_status;
  SdFinalizedHourV2WriteStatus* status =
      (out_status != nullptr) ? out_status : &local_status;
  *status = SdFinalizedHourV2WriteStatus{};

  uint16_t sensor_count = 0;
  if (!PrepareSensorWork(snapshot, workspace, &sensor_count, status)) {
    return false;
  }

  for (uint16_t i = 0; i < sensor_count; ++i) {
    status->corrected_without_presence_sanitized +=
        workspace.entries[i].sanitized_corrected_count;
  }

  if (sensor_count == 0U) {
    status->skipped_zero_sensor_hour = true;
    status->failure_reason = SdFinalizedHourV2WriteFailureReason::kNone;
    return true;
  }

  if (!SnapshotLabels(snapshot, labels, workspace, sensor_count, status)) {
    return false;
  }

  uint32_t payload_crc = 0;
  if (!ComputePayloadCrc(snapshot, workspace, sensor_count, &payload_crc)) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSerializationFailure);
    return false;
  }

  if (!BuildHeader(snapshot.hour_start_epoch_minute, sensor_count, payload_crc,
                   workspace, status)) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSerializationFailure);
    return false;
  }

  if (!WriteAll(write_fn, ctx, workspace.header_bytes,
                sizeof(workspace.header_bytes), status)) {
    return false;
  }
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!EncodeIndexEntry(workspace.entries[i], i, sensor_count,
                          workspace.index_bytes,
                          sizeof(workspace.index_bytes))) {
      SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSerializationFailure);
      return false;
    }
    if (!WriteAll(write_fn, ctx, workspace.index_bytes,
                  sizeof(workspace.index_bytes), status)) {
      return false;
    }
  }
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!BuildFinalBlock(snapshot, workspace.entries[i], workspace.labels[i],
                         workspace)) {
      SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSerializationFailure);
      return false;
    }
    if (!WriteAll(write_fn, ctx, workspace.block,
                  kSdFinalizedHourV2FixedBlockBytes, status)) {
      return false;
    }
  }

  if (status->bytes_written != status->record_bytes) {
    SetFailure(status, SdFinalizedHourV2WriteFailureReason::kSinkFailure);
    return false;
  }
  return true;
}
