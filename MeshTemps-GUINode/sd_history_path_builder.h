#ifndef SD_HISTORY_PATH_BUILDER_H_
#define SD_HISTORY_PATH_BUILDER_H_

#include <cstddef>
#include <cstdint>

// Fixed bounds for MeshTemps SD history paths. The base directory limit excludes
// the trailing NUL; full path buffers include the trailing NUL in their capacity.
constexpr size_t kSdHistoryBaseDirMax = 48;
constexpr size_t kSdHistoryPathMax = 96;

bool SdHistoryCopyBaseDir(const char* base_dir, char* out, size_t out_size);
bool SdHistoryBuildFinalizedDirPath(const char* base_dir,
                                    char* out,
                                    size_t out_size);
bool SdHistoryBuildFinalizedHourFilePath(uint32_t hour_start_epoch_minute,
                                         const char* base_dir,
                                         char* out,
                                         size_t out_size);

#endif  // SD_HISTORY_PATH_BUILDER_H_
