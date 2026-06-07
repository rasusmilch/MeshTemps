#include "sd_history_path_builder.h"

#include <ctime>

namespace {

bool Init(char* out, size_t out_size, size_t* len) {
  if (out == nullptr || out_size == 0U || len == nullptr) return false;
  out[0] = '\0';
  *len = 0U;
  return true;
}

bool AppendChar(char* out, size_t out_size, size_t* len, char value) {
  if (out == nullptr || len == nullptr || *len + 1U >= out_size) return false;
  out[*len] = value;
  ++(*len);
  out[*len] = '\0';
  return true;
}

bool AppendLiteral(char* out, size_t out_size, size_t* len, const char* literal) {
  if (literal == nullptr) return false;
  for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
    if (!AppendChar(out, out_size, len, *cursor)) return false;
  }
  return true;
}

bool AppendUnsignedDecimal(char* out,
                           size_t out_size,
                           size_t* len,
                           uint32_t value,
                           uint8_t min_width) {
  char digits[10];
  size_t digit_count = 0U;
  do {
    digits[digit_count++] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  } while (value != 0U && digit_count < sizeof(digits));

  while (digit_count < min_width) {
    if (!AppendChar(out, out_size, len, '0')) return false;
    --min_width;
  }
  while (digit_count > 0U) {
    --digit_count;
    if (!AppendChar(out, out_size, len, digits[digit_count])) return false;
  }
  return true;
}

bool AppendBase(char* out, size_t out_size, size_t* len, const char* base_dir) {
  if (base_dir == nullptr || base_dir[0] == '\0') return false;
  size_t base_len = 0U;
  while (base_dir[base_len] != '\0') {
    if (base_len >= kSdHistoryBaseDirMax) return false;
    ++base_len;
  }
  return AppendLiteral(out, out_size, len, base_dir);
}

bool AppendFinalizedPrefix(const char* base_dir,
                           char* out,
                           size_t out_size,
                           size_t* len) {
  if (!Init(out, out_size, len)) return false;
  if (!AppendBase(out, out_size, len, base_dir)) return false;
  return AppendLiteral(out, out_size, len, "/finalized");
}

bool AppendYyyyMmDd(uint32_t hour_start_epoch_minute,
                    char* out,
                    size_t out_size,
                    size_t* len) {
  const time_t epoch_seconds = static_cast<time_t>(hour_start_epoch_minute) * 60;
  struct tm tm_buf;
  if (localtime_r(&epoch_seconds, &tm_buf) == nullptr) {
    return AppendLiteral(out, out_size, len, "unknown");
  }

  const uint32_t year = static_cast<uint32_t>(tm_buf.tm_year + 1900);
  const uint32_t month = static_cast<uint32_t>(tm_buf.tm_mon + 1);
  const uint32_t day = static_cast<uint32_t>(tm_buf.tm_mday);
  return AppendUnsignedDecimal(out, out_size, len, year, 4) &&
         AppendUnsignedDecimal(out, out_size, len, month, 2) &&
         AppendUnsignedDecimal(out, out_size, len, day, 2);
}

}  // namespace

bool SdHistoryCopyBaseDir(const char* base_dir, char* out, size_t out_size) {
  size_t len = 0U;
  if (!Init(out, out_size, &len)) return false;
  return AppendBase(out, out_size, &len, base_dir);
}

bool SdHistoryBuildFinalizedDirPath(const char* base_dir,
                                    char* out,
                                    size_t out_size) {
  size_t len = 0U;
  return AppendFinalizedPrefix(base_dir, out, out_size, &len);
}

bool SdHistoryBuildFinalizedHourFilePath(uint32_t hour_start_epoch_minute,
                                         const char* base_dir,
                                         char* out,
                                         size_t out_size) {
  size_t len = 0U;
  if (!AppendFinalizedPrefix(base_dir, out, out_size, &len)) return false;
  if (!AppendChar(out, out_size, &len, '/')) return false;
  if (!AppendYyyyMmDd(hour_start_epoch_minute, out, out_size, &len)) return false;
  return AppendLiteral(out, out_size, &len, ".bin");
}
