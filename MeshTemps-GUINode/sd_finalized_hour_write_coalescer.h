#ifndef SD_FINALIZED_HOUR_WRITE_COALESCER_H_
#define SD_FINALIZED_HOUR_WRITE_COALESCER_H_

#include <stddef.h>
#include <stdint.h>

constexpr size_t kSdFinalizedHourWriteCoalescerBufferBytes = 512U;

typedef bool (*SdFinalizedHourRawWriteFn)(const uint8_t* data,
                                          size_t len,
                                          void* ctx);

class SdFinalizedHourWriteCoalescer {
 public:
  SdFinalizedHourWriteCoalescer(SdFinalizedHourRawWriteFn write_fn,
                                void* ctx,
                                uint8_t* buffer,
                                size_t buffer_size);

  bool Append(const uint8_t* data, size_t len);
  bool Flush();

  uint32_t logical_bytes() const { return logical_bytes_; }
  uint32_t physical_bytes() const { return physical_bytes_; }
  bool failed() const { return failed_; }

 private:
  bool Fail_();
  bool WriteRaw_(const uint8_t* data, size_t len);

  SdFinalizedHourRawWriteFn write_fn_ = nullptr;
  void* ctx_ = nullptr;
  uint8_t* buffer_ = nullptr;
  size_t buffer_size_ = 0;
  size_t used_ = 0;
  uint32_t logical_bytes_ = 0;
  uint32_t physical_bytes_ = 0;
  bool failed_ = false;
};

bool SdFinalizedHourCoalescerWriteFn(const uint8_t* data, size_t len, void* ctx);

#endif  // SD_FINALIZED_HOUR_WRITE_COALESCER_H_
