#ifndef SD_FINALIZED_HOUR_RECOVERY_H_
#define SD_FINALIZED_HOUR_RECOVERY_H_

#include <cstddef>
#include <cstdint>

#include "sd_finalized_hour_block.h"

// Read-only scanner support for finalized-hour append files.  This module does
// not repair, truncate, rename, delete, quarantine, or append; it only validates
// bytes supplied by a caller-owned reader and reports the longest valid prefix.

class ISdFinalizedHourByteReader {
 public:
  virtual ~ISdFinalizedHourByteReader() = default;

  // Reads up to len bytes starting at offset.  Returns false for an I/O/read
  // failure.  A successful short read reports EOF/torn data through bytes_read.
  virtual bool Read(uint64_t offset,
                    uint8_t* out,
                    size_t len,
                    size_t* bytes_read) = 0;
};

enum class SdFinalizedHourScanStatus {
  kClean,
  kEmpty,
  kCorruptTail,
  kInvalidAtZero,
  kReadError,
  kUnsupportedFormat,
  kDangerousHeader,
};

enum class SdFinalizedHourScanFailureReason {
  kNone,
  kPartialHeader,
  kBadMagic,
  kUnsupportedVersion,
  kBadHeaderSize,
  kBadSnapshotFormatVersion,
  kZeroHour,
  kActiveSlotCountTooLarge,
  kBadDescriptorEntryBytes,
  kBadFrameCount,
  kBadFrameBytes,
  kDescriptorBytesMismatch,
  kPayloadBytesMismatch,
  kRecordBytesMismatch,
  kRecordBytesTooSmall,
  kRecordBytesTooLarge,
  kBadHeaderCrc,
  kPartialPayload,
  kBadPayloadCrc,
  kReadError,
};

struct SdFinalizedHourScanResult {
  SdFinalizedHourScanStatus status = SdFinalizedHourScanStatus::kClean;
  SdFinalizedHourScanFailureReason first_failure_reason =
      SdFinalizedHourScanFailureReason::kNone;
  uint32_t valid_record_count = 0;
  uint64_t last_good_offset = 0;
  uint64_t first_bad_offset = 0;
  uint64_t expected_next_offset = 0;
  uint32_t last_valid_hour_start_epoch_minute = 0;
  bool saw_partial_header = false;
  bool saw_partial_payload = false;
  bool saw_bad_header_crc = false;
  bool saw_bad_payload_crc = false;
};

SdFinalizedHourScanResult ScanSdFinalizedHourAppendFile(
    ISdFinalizedHourByteReader& reader,
    uint8_t* scratch,
    size_t scratch_size);

#endif  // SD_FINALIZED_HOUR_RECOVERY_H_
