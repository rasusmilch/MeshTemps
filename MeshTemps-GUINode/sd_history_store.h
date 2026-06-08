#ifndef SD_HISTORY_STORE_H_
#define SD_HISTORY_STORE_H_

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

#include "history_hour_stager.h"
#include "sd_history_path_builder.h"

class SdHistoryStore {
 public:
  static constexpr uint16_t kVersion = 1;

  bool Begin(fs::FS& fs, const char* base_dir);

  // Append one authoritative raw finalized-hour block exported from the
  // backend-neutral current-hour stager. HistoryHourSnapshot is a logical
  // in-memory shape; the SD record is explicitly serialized with its own
  // magic/version/header CRC/payload CRC. Snapshot counters are diagnostics
  // only and are not used as sample/payload counts.
  bool AppendFinalizedHourSnapshot(const HistoryHourSnapshot& snapshot);

 private:
  bool EnsureDirExists_(const char* path);
  bool BuildFinalizedDirPath_(char* out, size_t out_size) const;
  bool BuildFinalizedHourFilePath_(uint32_t hour_start_epoch_minute,
                                   char* out,
                                   size_t out_size) const;
  bool VerifyFinalizedHourRecord_(const char* path, uint32_t record_offset) const;

  fs::FS* fs_ = nullptr;
  char base_dir_[kSdHistoryBaseDirMax + 1U] = {};
};

#endif  // SD_HISTORY_STORE_H_
