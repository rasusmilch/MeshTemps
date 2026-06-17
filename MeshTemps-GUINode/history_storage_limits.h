#ifndef HISTORY_STORAGE_LIMITS_H_
#define HISTORY_STORAGE_LIMITS_H_

#include <cstddef>

// Neutral MeshTemps history/storage product-domain limits shared by current-hour
// staging, finalized-hour writing, scanning, and focused host tests. This file
// intentionally owns only stable capacity limits; it is not a v2 on-disk format
// owner and must not become a generic shared constants bucket.
constexpr std::size_t kMeshTempsHistoryMaxSensorsPerHour = 64;
constexpr std::size_t kMeshTempsHistoryMinutesPerHour = 60;

#endif  // HISTORY_STORAGE_LIMITS_H_
