#include "sd_finalized_hour_write_coalescer.h"

#include <string.h>

SdFinalizedHourWriteCoalescer::SdFinalizedHourWriteCoalescer(
    SdFinalizedHourRawWriteFn write_fn,
    void* ctx,
    uint8_t* buffer,
    size_t buffer_size)
    : write_fn_(write_fn), ctx_(ctx), buffer_(buffer), buffer_size_(buffer_size) {}

bool SdFinalizedHourWriteCoalescer::Fail_() {
  failed_ = true;
  return false;
}

bool SdFinalizedHourWriteCoalescer::WriteRaw_(const uint8_t* data, size_t len) {
  if (failed_) return false;
  if (len == 0U) return true;
  if (write_fn_ == nullptr || data == nullptr) return Fail_();
  if (!write_fn_(data, len, ctx_)) return Fail_();
  physical_bytes_ += static_cast<uint32_t>(len);
  return true;
}

bool SdFinalizedHourWriteCoalescer::Append(const uint8_t* data, size_t len) {
  if (failed_) return false;
  if (len == 0U) return true;
  if (data == nullptr || write_fn_ == nullptr || ctx_ == nullptr ||
      buffer_ == nullptr || buffer_size_ == 0U) {
    return Fail_();
  }

  if (len > buffer_size_) {
    if (!Flush()) return false;
    if (!WriteRaw_(data, len)) return false;
    logical_bytes_ += static_cast<uint32_t>(len);
    return true;
  }

  if ((buffer_size_ - used_) < len) {
    if (!Flush()) return false;
  }

  memcpy(buffer_ + used_, data, len);
  used_ += len;
  logical_bytes_ += static_cast<uint32_t>(len);

  if (used_ == buffer_size_) {
    return Flush();
  }
  return true;
}

bool SdFinalizedHourWriteCoalescer::Flush() {
  if (failed_) return false;
  if (used_ == 0U) return true;
  if (!WriteRaw_(buffer_, used_)) return false;
  used_ = 0U;
  return true;
}

bool SdFinalizedHourCoalescerWriteFn(const uint8_t* data, size_t len, void* ctx) {
  if (ctx == nullptr) return false;
  return static_cast<SdFinalizedHourWriteCoalescer*>(ctx)->Append(data, len);
}
