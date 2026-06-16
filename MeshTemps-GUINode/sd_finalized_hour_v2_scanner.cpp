#include "sd_finalized_hour_v2_scanner.h"

#include "history_crc.h"

namespace {

bool AddU64(uint64_t a, uint64_t b, uint64_t* out) {
  if (out == nullptr) return false;
  if (UINT64_MAX - a < b) return false;
  *out = a + b;
  return true;
}

bool MulU32(uint32_t a, uint32_t b, uint32_t* out) {
  if (out == nullptr) return false;
  if (a != 0U && b > UINT32_MAX / a) return false;
  *out = a * b;
  return true;
}

bool ReadExact(ISdFinalizedHourV2ByteReader& reader,
               uint64_t offset,
               uint8_t* out,
               size_t len,
               bool* short_read) {
  if (short_read != nullptr) *short_read = false;
  size_t got = 0;
  if (!reader.Read(offset, out, len, &got)) return false;
  if (got != len) {
    if (short_read != nullptr) *short_read = true;
    return false;
  }
  return true;
}

SdFinalizedHourV2ScanStatus StatusForFailure(
    SdFinalizedHourV2ScanFailureReason reason,
    uint32_t valid_record_count) {
  if (reason == SdFinalizedHourV2ScanFailureReason::kReadError) {
    return SdFinalizedHourV2ScanStatus::kReadError;
  }
  if (reason == SdFinalizedHourV2ScanFailureReason::kMarkerMissing) {
    return SdFinalizedHourV2ScanStatus::kMissingMarker;
  }
  if (reason == SdFinalizedHourV2ScanFailureReason::kMarkerTooLate) {
    return SdFinalizedHourV2ScanStatus::kMarkerTooLate;
  }
  if (valid_record_count > 0U) return SdFinalizedHourV2ScanStatus::kCorruptTail;
  if (reason == SdFinalizedHourV2ScanFailureReason::kUnsupportedRecordVersion ||
      reason == SdFinalizedHourV2ScanFailureReason::kUnsupportedBlockVersion) {
    return SdFinalizedHourV2ScanStatus::kUnsupportedFormat;
  }
  switch (reason) {
    case SdFinalizedHourV2ScanFailureReason::kRecordBytesTooSmall:
    case SdFinalizedHourV2ScanFailureReason::kRecordBytesTooLarge:
    case SdFinalizedHourV2ScanFailureReason::kSensorCountTooLarge:
    case SdFinalizedHourV2ScanFailureReason::kOffsetOverflow:
      return SdFinalizedHourV2ScanStatus::kDangerousSizeOrOffset;
    default:
      return SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart;
  }
}

void SetFailureFlags(SdFinalizedHourV2ScanFailureReason reason,
                     SdFinalizedHourV2ScanResult* result) {
  if (result == nullptr) return;
  result->saw_partial_header =
      reason == SdFinalizedHourV2ScanFailureReason::kPartialHeader;
  result->saw_partial_payload =
      reason == SdFinalizedHourV2ScanFailureReason::kPartialIndex ||
      reason == SdFinalizedHourV2ScanFailureReason::kPartialBlock;
  result->saw_bad_header_crc =
      reason == SdFinalizedHourV2ScanFailureReason::kBadHeaderCrc;
  result->saw_bad_payload_crc =
      reason == SdFinalizedHourV2ScanFailureReason::kBadPayloadCrc;
  result->saw_bad_block_crc =
      reason == SdFinalizedHourV2ScanFailureReason::kBadBlockCrc;
  result->saw_duplicate_rom64 =
      reason == SdFinalizedHourV2ScanFailureReason::kDuplicateRom64;
  switch (reason) {
    case SdFinalizedHourV2ScanFailureReason::kRecordBytesTooSmall:
    case SdFinalizedHourV2ScanFailureReason::kRecordBytesTooLarge:
    case SdFinalizedHourV2ScanFailureReason::kSensorCountTooLarge:
    case SdFinalizedHourV2ScanFailureReason::kOffsetOverflow:
      result->saw_dangerous_size_or_offset = true;
      break;
    default:
      break;
  }
}

SdFinalizedHourV2ScanResult MakeFailure(
    const SdFinalizedHourV2ScanResult& current,
    SdFinalizedHourV2ScanFailureReason reason,
    uint64_t unsafe_offset) {
  SdFinalizedHourV2ScanResult result = current;
  result.status = StatusForFailure(reason, current.valid_record_count);
  result.first_failure_reason = reason;
  result.first_unsafe_offset = unsafe_offset;
  result.expected_next_record_offset = current.valid_prefix_end_offset;
  SetFailureFlags(reason, &result);
  return result;
}

bool FindMarker(ISdFinalizedHourV2ByteReader& reader,
                SdFinalizedHourV2ScanResult* result) {
  if (result == nullptr) return false;
  const char* marker = kSdFinalizedHourV2BinaryStartMarker;
  const size_t marker_len = sizeof(kSdFinalizedHourV2BinaryStartMarker) - 1U;
  size_t matched = 0;
  uint8_t byte = 0;
  for (uint64_t offset = 0; offset < kSdFinalizedHourV2PreambleMaxBytes; ++offset) {
    size_t got = 0;
    if (!reader.Read(offset, &byte, 1U, &got)) {
      *result = MakeFailure(*result, SdFinalizedHourV2ScanFailureReason::kReadError,
                            offset);
      return false;
    }
    if (got == 0U) {
      *result = MakeFailure(*result,
                            SdFinalizedHourV2ScanFailureReason::kMarkerMissing,
                            0U);
      return false;
    }
    const char ch = static_cast<char>(byte);
    if (ch == marker[matched]) {
      ++matched;
      if (matched == marker_len) {
        result->saw_marker = true;
        result->marker_start_offset = offset + 1U - marker_len;
        result->binary_stream_start_offset = offset + 1U;
        result->valid_prefix_end_offset = result->binary_stream_start_offset;
        result->expected_next_record_offset = result->binary_stream_start_offset;
        result->first_unsafe_offset = result->binary_stream_start_offset;
        return true;
      }
    } else {
      matched = (ch == marker[0]) ? 1U : 0U;
    }
  }
  *result = MakeFailure(*result, SdFinalizedHourV2ScanFailureReason::kMarkerTooLate,
                        0U);
  return false;
}

SdFinalizedHourV2ScanFailureReason ValidateHeaderLayout(
    const SdFinalizedHourV2Header& header) {
  if (header.record_magic != kSdFinalizedHourV2RecordMagic) {
    return SdFinalizedHourV2ScanFailureReason::kBadRecordMagic;
  }
  if (header.record_version != kSdFinalizedHourV2RecordVersion) {
    return SdFinalizedHourV2ScanFailureReason::kUnsupportedRecordVersion;
  }
  if (header.header_bytes != kSdFinalizedHourV2HeaderBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadHeaderBytes;
  }
  if (header.record_bytes < kSdFinalizedHourV2HeaderBytes) {
    return SdFinalizedHourV2ScanFailureReason::kRecordBytesTooSmall;
  }
  if (header.record_bytes > kSdFinalizedHourV2ScannerMaxRecordBytes) {
    return SdFinalizedHourV2ScanFailureReason::kRecordBytesTooLarge;
  }
  if (header.sensor_count == 0U) {
    return SdFinalizedHourV2ScanFailureReason::kZeroSensorCount;
  }
  if (header.sensor_count > kSdFinalizedHourV2ScannerMaxSensors) {
    return SdFinalizedHourV2ScanFailureReason::kSensorCountTooLarge;
  }
  if (header.index_entry_bytes != kSdFinalizedHourV2IndexEntryBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadIndexEntryBytes;
  }
  uint32_t index_bytes = 0;
  uint32_t block_bytes = 0;
  if (!MulU32(header.sensor_count, kSdFinalizedHourV2IndexEntryBytes,
              &index_bytes) ||
      !MulU32(header.sensor_count, kSdFinalizedHourV2FixedBlockBytes,
              &block_bytes)) {
    return SdFinalizedHourV2ScanFailureReason::kOffsetOverflow;
  }
  if (header.index_offset != kSdFinalizedHourV2HeaderBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadIndexOffset;
  }
  if (header.index_bytes != index_bytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadIndexBytes;
  }
  const uint32_t expected_blocks_offset = kSdFinalizedHourV2HeaderBytes + index_bytes;
  if (expected_blocks_offset < kSdFinalizedHourV2HeaderBytes) {
    return SdFinalizedHourV2ScanFailureReason::kOffsetOverflow;
  }
  if (header.sensor_blocks_offset != expected_blocks_offset) {
    return SdFinalizedHourV2ScanFailureReason::kBadSensorBlocksOffset;
  }
  if (header.sensor_blocks_bytes != block_bytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadSensorBlocksBytes;
  }
  const uint32_t expected_record_bytes = expected_blocks_offset + block_bytes;
  if (expected_record_bytes < expected_blocks_offset) {
    return SdFinalizedHourV2ScanFailureReason::kOffsetOverflow;
  }
  if (header.record_bytes != expected_record_bytes) {
    return SdFinalizedHourV2ScanFailureReason::kRecordBytesMismatch;
  }
  return SdFinalizedHourV2ScanFailureReason::kNone;
}

SdFinalizedHourV2ScanFailureReason ValidateBlock(
    const uint8_t* block,
    uint64_t expected_rom64,
    uint32_t* payload_crc_state) {
  if (block == nullptr || payload_crc_state == nullptr) {
    return SdFinalizedHourV2ScanFailureReason::kReadError;
  }
  SdFinalizedHourV2BlockHeader header;
  if (!DecodeSdFinalizedHourV2BlockHeader(block, kSdFinalizedHourV2BlockHeaderBytes,
                                          &header)) {
    return SdFinalizedHourV2ScanFailureReason::kPartialBlock;
  }
  if (header.block_magic != kSdFinalizedHourV2BlockMagic) {
    return SdFinalizedHourV2ScanFailureReason::kBadBlockMagic;
  }
  if (header.block_version != kSdFinalizedHourV2BlockVersion) {
    return SdFinalizedHourV2ScanFailureReason::kUnsupportedBlockVersion;
  }
  if (header.block_header_bytes != kSdFinalizedHourV2BlockHeaderBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadBlockHeaderBytes;
  }
  if (header.block_bytes != kSdFinalizedHourV2FixedBlockBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadBlockBytes;
  }
  if (header.descriptor_bytes != kSdFinalizedHourV2DescriptorBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadDescriptorBytes;
  }
  if (header.payload_bytes != kSdFinalizedHourV2PayloadBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadPayloadBytes;
  }
  if (header.bitmap_bytes != kSdFinalizedHourV2BitmapBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadBitmapBytes;
  }
  if (header.sample_count != kSdFinalizedHourV2SampleCount) {
    return SdFinalizedHourV2ScanFailureReason::kBadSampleCount;
  }
  if (header.sample_bytes != kSdFinalizedHourV2SampleBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadSampleBytes;
  }
  if (header.sample_encoding != kSdFinalizedHourV2SampleEncodingInt16CentiCLe) {
    return SdFinalizedHourV2ScanFailureReason::kBadSampleEncoding;
  }
  if (ComputeSdFinalizedHourV2BlockCrc32(block, kSdFinalizedHourV2FixedBlockBytes) !=
      header.block_crc32) {
    return SdFinalizedHourV2ScanFailureReason::kBadBlockCrc;
  }
  SdFinalizedHourV2Descriptor descriptor;
  if (!DecodeSdFinalizedHourV2Descriptor(
          block + kSdFinalizedHourV2BlockHeaderBytes,
          kSdFinalizedHourV2DescriptorBytes, &descriptor)) {
    return SdFinalizedHourV2ScanFailureReason::kBadDescriptorBytes;
  }
  if (descriptor.rom64 != expected_rom64) {
    return SdFinalizedHourV2ScanFailureReason::kDescriptorRom64Mismatch;
  }
  if (descriptor.node_label_len > kSdFinalizedHourV2NodeLabelMaxBytes ||
      descriptor.sensor_label_len > kSdFinalizedHourV2SensorLabelMaxBytes) {
    return SdFinalizedHourV2ScanFailureReason::kBadDescriptorLabelLength;
  }
  if (descriptor.valid_sample_count > kSdFinalizedHourV2SampleCount ||
      descriptor.missing_or_invalid_count > kSdFinalizedHourV2SampleCount ||
      descriptor.corrected_sample_count > descriptor.valid_sample_count) {
    return SdFinalizedHourV2ScanFailureReason::kBadDescriptorCounts;
  }
  SdFinalizedHourV2Payload payload;
  if (!DecodeSdFinalizedHourV2Payload(
          block + kSdFinalizedHourV2BlockHeaderBytes +
              kSdFinalizedHourV2DescriptorBytes,
          kSdFinalizedHourV2PayloadBytes, &payload)) {
    return SdFinalizedHourV2ScanFailureReason::kBadPayloadBytes;
  }
  if (!SdFinalizedHourV2PayloadBitmapsAreValid(payload)) {
    return SdFinalizedHourV2ScanFailureReason::kCorrectedWithoutPresence;
  }
  *payload_crc_state = Crc32IsoHdlcUpdate(
      *payload_crc_state, block, kSdFinalizedHourV2FixedBlockBytes);
  return SdFinalizedHourV2ScanFailureReason::kNone;
}

}  // namespace

SdFinalizedHourV2ScanResult ScanSdFinalizedHourV2DayFile(
    ISdFinalizedHourV2ByteReader& reader,
    SdFinalizedHourV2ScannerWorkspace& workspace) {
  SdFinalizedHourV2ScanResult result;
  result.status = SdFinalizedHourV2ScanStatus::kMissingMarker;
  result.first_failure_reason = SdFinalizedHourV2ScanFailureReason::kMarkerMissing;

  if (!FindMarker(reader, &result)) return result;

  uint64_t offset = result.binary_stream_start_offset;
  while (true) {
    bool short_read = false;
    size_t got = 0;
    if (!reader.Read(offset, workspace.header, sizeof(workspace.header), &got)) {
      return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kReadError,
                         offset);
    }
    if (got == 0U) {
      result.status = (result.valid_record_count == 0U)
                          ? SdFinalizedHourV2ScanStatus::kEmptyPreambleOnly
                          : SdFinalizedHourV2ScanStatus::kClean;
      result.first_failure_reason = SdFinalizedHourV2ScanFailureReason::kNone;
      result.first_unsafe_offset = offset;
      result.expected_next_record_offset = offset;
      result.valid_prefix_end_offset = offset;
      return result;
    }
    if (got != sizeof(workspace.header)) {
      return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kPartialHeader,
                         offset);
    }

    SdFinalizedHourV2Header header;
    if (!DecodeSdFinalizedHourV2Header(workspace.header, sizeof(workspace.header),
                                       &header)) {
      return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kPartialHeader,
                         offset);
    }
    const SdFinalizedHourV2ScanFailureReason layout_reason =
        ValidateHeaderLayout(header);
    if (layout_reason != SdFinalizedHourV2ScanFailureReason::kNone) {
      return MakeFailure(result, layout_reason, offset);
    }
    if (ComputeSdFinalizedHourV2HeaderCrc32(workspace.header,
                                            sizeof(workspace.header)) !=
        header.header_crc32) {
      return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kBadHeaderCrc,
                         offset);
    }

    uint64_t record_end = 0;
    if (!AddU64(offset, header.record_bytes, &record_end)) {
      return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kOffsetOverflow,
                         offset);
    }

    uint32_t payload_crc = Crc32IsoHdlcBegin();
    for (uint16_t i = 0; i < header.sensor_count; ++i) {
      uint64_t index_offset = 0;
      if (!AddU64(offset, header.index_offset, &index_offset) ||
          !AddU64(index_offset,
                  static_cast<uint64_t>(i) * kSdFinalizedHourV2IndexEntryBytes,
                  &index_offset)) {
        return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kOffsetOverflow,
                           offset);
      }
      if (!ReadExact(reader, index_offset, workspace.index_entry,
                     sizeof(workspace.index_entry), &short_read)) {
        return MakeFailure(result,
                           short_read ? SdFinalizedHourV2ScanFailureReason::kPartialIndex
                                      : SdFinalizedHourV2ScanFailureReason::kReadError,
                           index_offset);
      }
      SdFinalizedHourV2IndexEntry entry;
      if (!DecodeSdFinalizedHourV2IndexEntry(
              workspace.index_entry, sizeof(workspace.index_entry), &entry)) {
        return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kPartialIndex,
                           index_offset);
      }
      for (uint16_t j = 0; j < i; ++j) {
        if (workspace.index_rom64[j] == entry.rom64) {
          return MakeFailure(result,
                             SdFinalizedHourV2ScanFailureReason::kDuplicateRom64,
                             index_offset);
        }
      }
      workspace.index_rom64[i] = entry.rom64;
      const uint32_t expected_block_offset =
          header.sensor_blocks_offset +
          (static_cast<uint32_t>(i) * kSdFinalizedHourV2FixedBlockBytes);
      if (entry.sensor_block_offset_from_record_start != expected_block_offset ||
          entry.sensor_block_offset_from_record_start +
                  kSdFinalizedHourV2FixedBlockBytes >
              header.record_bytes) {
        return MakeFailure(result,
                           SdFinalizedHourV2ScanFailureReason::kBadIndexBlockOffset,
                           index_offset);
      }
      payload_crc = Crc32IsoHdlcUpdate(payload_crc, workspace.index_entry,
                                       sizeof(workspace.index_entry));
    }

    for (uint16_t i = 0; i < header.sensor_count; ++i) {
      uint64_t block_offset = 0;
      if (!AddU64(offset, header.sensor_blocks_offset, &block_offset) ||
          !AddU64(block_offset,
                  static_cast<uint64_t>(i) * kSdFinalizedHourV2FixedBlockBytes,
                  &block_offset)) {
        return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kOffsetOverflow,
                           offset);
      }
      if (!ReadExact(reader, block_offset, workspace.block, sizeof(workspace.block),
                     &short_read)) {
        return MakeFailure(result,
                           short_read ? SdFinalizedHourV2ScanFailureReason::kPartialBlock
                                      : SdFinalizedHourV2ScanFailureReason::kReadError,
                           block_offset);
      }
      const SdFinalizedHourV2ScanFailureReason block_reason =
          ValidateBlock(workspace.block, workspace.index_rom64[i], &payload_crc);
      if (block_reason != SdFinalizedHourV2ScanFailureReason::kNone) {
        return MakeFailure(result, block_reason, block_offset);
      }
    }
    if (Crc32IsoHdlcFinalize(payload_crc) != header.payload_crc32) {
      return MakeFailure(result, SdFinalizedHourV2ScanFailureReason::kBadPayloadCrc,
                         offset);
    }

    ++result.valid_record_count;
    result.valid_prefix_end_offset = record_end;
    result.expected_next_record_offset = record_end;
    result.first_unsafe_offset = record_end;
    result.last_valid_hour_start_epoch_minute = header.hour_start_epoch_minute;
    offset = record_end;
  }
}
