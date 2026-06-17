#ifndef SD_FINALIZED_HOUR_V2_SCANNER_H_
#define SD_FINALIZED_HOUR_V2_SCANNER_H_

#include <cstddef>
#include <cstdint>
#include <limits>

#include "history_storage_limits.h"

#include "sd_finalized_hour_v2_format.h"

static_assert(kMeshTempsHistoryMaxSensorsPerHour <=
                  static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()),
              "scanner max sensor count must fit uint16_t record counts");
static_assert(kSdFinalizedHourV2SampleCount == kMeshTempsHistoryMinutesPerHour,
              "v2 sample count remains an on-disk format field but must match the product hour minute count");
constexpr uint16_t kSdFinalizedHourV2ScannerMaxSensors =
    static_cast<uint16_t>(kMeshTempsHistoryMaxSensorsPerHour);
static_assert(kSdFinalizedHourV2ScannerMaxSensors ==
                  kMeshTempsHistoryMaxSensorsPerHour,
              "scanner max sensors must follow the product history sensor limit");
constexpr uint32_t kSdFinalizedHourV2ScannerMaxRecordBytes =
    kSdFinalizedHourV2HeaderBytes +
    (static_cast<uint32_t>(kSdFinalizedHourV2ScannerMaxSensors) *
     kSdFinalizedHourV2IndexEntryBytes) +
    (static_cast<uint32_t>(kSdFinalizedHourV2ScannerMaxSensors) *
     kSdFinalizedHourV2FixedBlockBytes);

class ISdFinalizedHourV2ByteReader {
 public:
  virtual ~ISdFinalizedHourV2ByteReader() = default;

  virtual bool Read(uint64_t offset,
                    uint8_t* out,
                    size_t len,
                    size_t* bytes_read) = 0;
};

enum class SdFinalizedHourV2ScanStatus {
  kClean,
  kEmptyPreambleOnly,
  kMissingMarker,
  kMarkerTooLate,
  kInvalidAtBinaryStart,
  kCorruptTail,
  kReadError,
  kUnsupportedFormat,
  kDangerousSizeOrOffset,
};

enum class SdFinalizedHourV2ScanFailureReason {
  kNone,
  kReadError,
  kMarkerMissing,
  kMarkerTooLate,
  kPartialHeader,
  kBadRecordMagic,
  kUnsupportedRecordVersion,
  kBadHeaderBytes,
  kRecordBytesTooSmall,
  kRecordBytesTooLarge,
  kRecordBytesMismatch,
  kBadHeaderCrc,
  kZeroSensorCount,
  kSensorCountTooLarge,
  kBadIndexEntryBytes,
  kBadIndexOffset,
  kBadIndexBytes,
  kBadSensorBlocksOffset,
  kBadSensorBlocksBytes,
  kOffsetOverflow,
  kPartialIndex,
  kDuplicateRom64,
  kBadIndexBlockOffset,
  kPartialBlock,
  kBadBlockMagic,
  kUnsupportedBlockVersion,
  kBadBlockHeaderBytes,
  kBadBlockBytes,
  kBadDescriptorBytes,
  kBadPayloadBytes,
  kBadBitmapBytes,
  kBadSampleCount,
  kBadSampleBytes,
  kBadSampleEncoding,
  kBadBlockCrc,
  kDescriptorRom64Mismatch,
  kBadDescriptorLabelLength,
  kBadDescriptorCounts,
  kCorrectedWithoutPresence,
  kBadPayloadCrc,
};

struct SdFinalizedHourV2ScanResult {
  SdFinalizedHourV2ScanStatus status = SdFinalizedHourV2ScanStatus::kReadError;
  SdFinalizedHourV2ScanFailureReason first_failure_reason =
      SdFinalizedHourV2ScanFailureReason::kReadError;
  uint64_t marker_start_offset = 0;
  uint64_t binary_stream_start_offset = 0;
  uint32_t valid_record_count = 0;
  uint64_t valid_prefix_end_offset = 0;
  uint64_t first_unsafe_offset = 0;
  uint64_t expected_next_record_offset = 0;
  uint32_t last_valid_hour_start_epoch_minute = 0;
  bool saw_marker = false;
  bool saw_partial_header = false;
  bool saw_partial_payload = false;
  bool saw_bad_header_crc = false;
  bool saw_bad_payload_crc = false;
  bool saw_bad_block_crc = false;
  bool saw_duplicate_rom64 = false;
  bool saw_dangerous_size_or_offset = false;
};

struct SdFinalizedHourV2ScannerWorkspace {
  uint64_t index_rom64[kSdFinalizedHourV2ScannerMaxSensors] = {};
  uint8_t header[kSdFinalizedHourV2HeaderBytes] = {};
  uint8_t index_entry[kSdFinalizedHourV2IndexEntryBytes] = {};
  uint8_t block[kSdFinalizedHourV2FixedBlockBytes] = {};
};

SdFinalizedHourV2ScanResult ScanSdFinalizedHourV2DayFile(
    ISdFinalizedHourV2ByteReader& reader,
    SdFinalizedHourV2ScannerWorkspace& workspace);

#endif  // SD_FINALIZED_HOUR_V2_SCANNER_H_
