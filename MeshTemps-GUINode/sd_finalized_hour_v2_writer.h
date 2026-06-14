#ifndef SD_FINALIZED_HOUR_V2_WRITER_H_
#define SD_FINALIZED_HOUR_V2_WRITER_H_

#include <cstddef>
#include <cstdint>

#include "history_hour_stager.h"
#include "sd_finalized_hour_v2_format.h"

typedef bool (*SdFinalizedHourV2WriteFn)(const uint8_t* data,
                                         size_t len,
                                         void* ctx);

enum class SdFinalizedHourV2WriteFailureReason {
  kNone,
  kInvalidArgument,
  kUnsupportedSnapshot,
  kInvalidSnapshot,
  kDuplicateRom64,
  kLabelSourceFailure,
  kSerializationFailure,
  kSinkFailure,
};

struct SdFinalizedHourV2WriteStatus {
  uint32_t bytes_written = 0;
  uint16_t sensor_count = 0;
  uint32_t record_bytes = 0;
  uint32_t payload_crc32 = 0;
  uint32_t header_crc32 = 0;
  uint32_t corrected_without_presence_sanitized = 0;
  bool skipped_zero_sensor_hour = false;
  SdFinalizedHourV2WriteFailureReason failure_reason =
      SdFinalizedHourV2WriteFailureReason::kNone;
};

class ISdFinalizedHourV2LabelSource {
 public:
  virtual ~ISdFinalizedHourV2LabelSource() = default;

  virtual bool CopyNodeLabel(uint32_t node_id,
                             uint8_t* out,
                             size_t out_size,
                             size_t* out_len,
                             bool* out_truncated) const = 0;

  virtual bool CopySensorLabel(uint64_t rom64,
                               uint32_t node_id,
                               uint8_t* out,
                               size_t out_size,
                               size_t* out_len,
                               bool* out_truncated) const = 0;
};

struct SdFinalizedHourV2WriterSensorWorkEntry {
  uint8_t slot_index = 0;
  uint64_t rom64 = 0;
  uint16_t valid_sample_count = 0;
  uint16_t missing_or_invalid_count = 0;
  uint16_t corrected_sample_count = 0;
  uint16_t sanitized_corrected_count = 0;
};

struct SdFinalizedHourV2WriterLabelSnapshot {
  uint8_t node_label[kSdFinalizedHourV2NodeLabelMaxBytes] = {};
  uint8_t sensor_label[kSdFinalizedHourV2SensorLabelMaxBytes] = {};
  uint8_t node_label_len = 0;
  uint8_t sensor_label_len = 0;
  bool node_label_truncated = false;
  bool sensor_label_truncated = false;
};

struct SdFinalizedHourV2WriterWorkspace {
  SdFinalizedHourV2WriterSensorWorkEntry entries[kHistorySlotCapacity] = {};
  SdFinalizedHourV2WriterLabelSnapshot labels[kHistorySlotCapacity] = {};
  SdFinalizedHourV2Header header = {};
  SdFinalizedHourV2BlockHeader block_header = {};
  SdFinalizedHourV2Descriptor descriptor = {};
  SdFinalizedHourV2Payload payload = {};
  uint8_t header_bytes[kSdFinalizedHourV2HeaderBytes] = {};
  uint8_t index_bytes[kSdFinalizedHourV2IndexEntryBytes] = {};
  uint8_t block[kSdFinalizedHourV2FixedBlockBytes] = {};
};

bool WriteSdFinalizedHourV2Record(
    const HistoryHourSnapshot& snapshot,
    const ISdFinalizedHourV2LabelSource* labels,
    SdFinalizedHourV2WriterWorkspace& workspace,
    SdFinalizedHourV2WriteFn write_fn,
    void* ctx,
    SdFinalizedHourV2WriteStatus* out_status = nullptr);

#endif  // SD_FINALIZED_HOUR_V2_WRITER_H_
