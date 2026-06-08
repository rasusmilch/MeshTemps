#include "sd_finalized_hour_recovery.h"

#include "history_crc.h"
#include "history_hour_stager.h"

namespace {

constexpr uint32_t kHeaderCrcOffset = 36U;

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

SdFinalizedHourBlockHeader ParseHeaderFields(const uint8_t* header_bytes) {
  SdFinalizedHourBlockHeader header;
  header.magic = ReadU32(header_bytes + 0U);
  header.version = ReadU16(header_bytes + 4U);
  header.header_bytes = ReadU16(header_bytes + 6U);
  header.record_bytes = ReadU32(header_bytes + 8U);
  header.hour_start_epoch_minute = ReadU32(header_bytes + 12U);
  header.active_slot_count = ReadU16(header_bytes + 16U);
  header.descriptor_entry_bytes = ReadU16(header_bytes + 18U);
  header.frame_count = ReadU16(header_bytes + 20U);
  header.frame_bytes = ReadU16(header_bytes + 22U);
  header.descriptor_bytes = ReadU32(header_bytes + 24U);
  header.payload_bytes = ReadU32(header_bytes + 28U);
  header.payload_crc32 = ReadU32(header_bytes + 32U);
  header.header_crc32 = ReadU32(header_bytes + kHeaderCrcOffset);
  header.snapshot_format_version = ReadU16(header_bytes + 40U);
  header.reserved0 = ReadU16(header_bytes + 42U);
  header.flags = ReadU32(header_bytes + 44U);
  return header;
}

SdFinalizedHourScanFailureReason ValidateHeaderFields(
    const SdFinalizedHourBlockHeader& header) {
  if (header.magic != kSdFinalizedHourMagic) {
    return SdFinalizedHourScanFailureReason::kBadMagic;
  }
  if (header.version != kSdFinalizedHourVersion) {
    return SdFinalizedHourScanFailureReason::kUnsupportedVersion;
  }
  if (header.header_bytes != kSdFinalizedHourHeaderBytes) {
    return SdFinalizedHourScanFailureReason::kBadHeaderSize;
  }
  if (header.record_bytes < kSdFinalizedHourHeaderBytes) {
    return SdFinalizedHourScanFailureReason::kRecordBytesTooSmall;
  }
  if (header.record_bytes > kSdFinalizedHourMaxRecordBytes) {
    return SdFinalizedHourScanFailureReason::kRecordBytesTooLarge;
  }
  if (header.snapshot_format_version != kHistoryHourSnapshotFormatVersion) {
    return SdFinalizedHourScanFailureReason::kBadSnapshotFormatVersion;
  }
  if (header.hour_start_epoch_minute == 0U) {
    return SdFinalizedHourScanFailureReason::kZeroHour;
  }
  if (header.active_slot_count > kHistorySlotCapacity) {
    return SdFinalizedHourScanFailureReason::kActiveSlotCountTooLarge;
  }
  if (header.descriptor_entry_bytes != kSdFinalizedHourDescriptorBytes) {
    return SdFinalizedHourScanFailureReason::kBadDescriptorEntryBytes;
  }
  if (header.frame_count != kHistoryMinutesPerHour) {
    return SdFinalizedHourScanFailureReason::kBadFrameCount;
  }
  if (header.frame_bytes != kSdFinalizedHourFrameBytes) {
    return SdFinalizedHourScanFailureReason::kBadFrameBytes;
  }

  const uint32_t expected_descriptor_bytes =
      static_cast<uint32_t>(header.active_slot_count) *
      kSdFinalizedHourDescriptorBytes;
  if (header.descriptor_bytes != expected_descriptor_bytes) {
    return SdFinalizedHourScanFailureReason::kDescriptorBytesMismatch;
  }

  const uint32_t expected_frame_payload_bytes =
      static_cast<uint32_t>(kHistoryMinutesPerHour) * kSdFinalizedHourFrameBytes;
  if (header.payload_bytes != expected_descriptor_bytes + expected_frame_payload_bytes) {
    return SdFinalizedHourScanFailureReason::kPayloadBytesMismatch;
  }
  if (header.record_bytes != header.header_bytes + header.payload_bytes) {
    return SdFinalizedHourScanFailureReason::kRecordBytesMismatch;
  }
  return SdFinalizedHourScanFailureReason::kNone;
}

bool IsUnsupportedFormat(SdFinalizedHourScanFailureReason reason) {
  return reason == SdFinalizedHourScanFailureReason::kUnsupportedVersion ||
         reason == SdFinalizedHourScanFailureReason::kBadSnapshotFormatVersion;
}

bool IsDangerousHeader(SdFinalizedHourScanFailureReason reason) {
  switch (reason) {
    case SdFinalizedHourScanFailureReason::kBadHeaderSize:
    case SdFinalizedHourScanFailureReason::kActiveSlotCountTooLarge:
    case SdFinalizedHourScanFailureReason::kBadDescriptorEntryBytes:
    case SdFinalizedHourScanFailureReason::kBadFrameCount:
    case SdFinalizedHourScanFailureReason::kBadFrameBytes:
    case SdFinalizedHourScanFailureReason::kDescriptorBytesMismatch:
    case SdFinalizedHourScanFailureReason::kPayloadBytesMismatch:
    case SdFinalizedHourScanFailureReason::kRecordBytesMismatch:
    case SdFinalizedHourScanFailureReason::kRecordBytesTooSmall:
    case SdFinalizedHourScanFailureReason::kRecordBytesTooLarge:
      return true;
    default:
      return false;
  }
}

SdFinalizedHourScanStatus StatusForFailure(
    SdFinalizedHourScanFailureReason reason,
    uint32_t valid_record_count) {
  if (reason == SdFinalizedHourScanFailureReason::kReadError) {
    return SdFinalizedHourScanStatus::kReadError;
  }
  if (valid_record_count > 0U) {
    return SdFinalizedHourScanStatus::kCorruptTail;
  }
  if (IsUnsupportedFormat(reason)) {
    return SdFinalizedHourScanStatus::kUnsupportedFormat;
  }
  if (IsDangerousHeader(reason)) {
    return SdFinalizedHourScanStatus::kDangerousHeader;
  }
  return SdFinalizedHourScanStatus::kInvalidAtZero;
}

SdFinalizedHourScanResult MakeFailureResult(
    const SdFinalizedHourScanResult& current,
    SdFinalizedHourScanFailureReason reason,
    uint64_t first_bad_offset) {
  SdFinalizedHourScanResult result = current;
  result.status = StatusForFailure(reason, current.valid_record_count);
  result.first_failure_reason = reason;
  result.first_bad_offset = first_bad_offset;
  result.expected_next_offset = current.last_good_offset;
  result.saw_partial_header =
      reason == SdFinalizedHourScanFailureReason::kPartialHeader;
  result.saw_partial_payload =
      reason == SdFinalizedHourScanFailureReason::kPartialPayload;
  result.saw_bad_header_crc =
      reason == SdFinalizedHourScanFailureReason::kBadHeaderCrc;
  result.saw_bad_payload_crc =
      reason == SdFinalizedHourScanFailureReason::kBadPayloadCrc;
  return result;
}

bool ReadExactOrShort(ISdFinalizedHourByteReader& reader,
                      uint64_t offset,
                      uint8_t* out,
                      size_t len,
                      size_t* bytes_read) {
  if (bytes_read == nullptr) return false;
  *bytes_read = 0U;
  if (len == 0U) return true;
  if (out == nullptr) return false;
  return reader.Read(offset, out, len, bytes_read);
}

}  // namespace

SdFinalizedHourScanResult ScanSdFinalizedHourAppendFile(
    ISdFinalizedHourByteReader& reader,
    uint8_t* scratch,
    size_t scratch_size) {
  SdFinalizedHourScanResult result;
  if (scratch == nullptr || scratch_size == 0U) {
    result.status = SdFinalizedHourScanStatus::kReadError;
    result.first_failure_reason = SdFinalizedHourScanFailureReason::kReadError;
    return result;
  }

  uint64_t offset = 0U;
  uint8_t header_bytes[kSdFinalizedHourHeaderBytes];
  while (true) {
    size_t header_read = 0U;
    if (!ReadExactOrShort(reader, offset, header_bytes, sizeof(header_bytes),
                          &header_read)) {
      return MakeFailureResult(result, SdFinalizedHourScanFailureReason::kReadError,
                               offset);
    }
    if (header_read == 0U) {
      result.status = (result.valid_record_count == 0U)
                          ? SdFinalizedHourScanStatus::kEmpty
                          : SdFinalizedHourScanStatus::kClean;
      result.first_failure_reason = SdFinalizedHourScanFailureReason::kNone;
      result.first_bad_offset = offset;
      result.expected_next_offset = offset;
      return result;
    }
    if (header_read != sizeof(header_bytes)) {
      return MakeFailureResult(result,
                               SdFinalizedHourScanFailureReason::kPartialHeader,
                               offset);
    }

    const SdFinalizedHourBlockHeader parsed_header =
        ParseHeaderFields(header_bytes);
    const SdFinalizedHourScanFailureReason field_reason =
        ValidateHeaderFields(parsed_header);
    if (field_reason != SdFinalizedHourScanFailureReason::kNone) {
      return MakeFailureResult(result, field_reason, offset);
    }

    SdFinalizedHourBlockHeader decoded_header;
    if (!DecodeSdFinalizedHourBlockHeader(header_bytes, sizeof(header_bytes),
                                          &decoded_header)) {
      return MakeFailureResult(result,
                               SdFinalizedHourScanFailureReason::kRecordBytesMismatch,
                               offset);
    }
    if (!VerifySdFinalizedHourBlockHeaderCrc(header_bytes, sizeof(header_bytes))) {
      return MakeFailureResult(result,
                               SdFinalizedHourScanFailureReason::kBadHeaderCrc,
                               offset);
    }

    uint32_t remaining = decoded_header.payload_bytes;
    uint64_t payload_offset = offset + kSdFinalizedHourHeaderBytes;
    uint32_t payload_crc = 0xFFFFFFFFu;
    while (remaining > 0U) {
      const size_t chunk = (remaining < scratch_size)
                               ? static_cast<size_t>(remaining)
                               : scratch_size;
      size_t bytes_read = 0U;
      if (!ReadExactOrShort(reader, payload_offset, scratch, chunk, &bytes_read)) {
        return MakeFailureResult(result,
                                 SdFinalizedHourScanFailureReason::kReadError,
                                 offset);
      }
      if (bytes_read != chunk) {
        return MakeFailureResult(result,
                                 SdFinalizedHourScanFailureReason::kPartialPayload,
                                 offset);
      }
      payload_crc = Crc32Update(payload_crc, scratch, chunk);
      payload_offset += static_cast<uint64_t>(chunk);
      remaining -= static_cast<uint32_t>(chunk);
    }
    if (payload_crc != decoded_header.payload_crc32) {
      return MakeFailureResult(result,
                               SdFinalizedHourScanFailureReason::kBadPayloadCrc,
                               offset);
    }

    ++result.valid_record_count;
    result.last_good_offset = offset + decoded_header.record_bytes;
    result.expected_next_offset = result.last_good_offset;
    result.last_valid_hour_start_epoch_minute =
        decoded_header.hour_start_epoch_minute;
    offset = result.last_good_offset;
  }
}
