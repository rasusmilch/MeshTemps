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

bool WriteSdFinalizedHourV2Record(
    const HistoryHourSnapshot& snapshot,
    const ISdFinalizedHourV2LabelSource* labels,
    SdFinalizedHourV2WriteFn write_fn,
    void* ctx,
    SdFinalizedHourV2WriteStatus* out_status = nullptr);

#endif  // SD_FINALIZED_HOUR_V2_WRITER_H_
