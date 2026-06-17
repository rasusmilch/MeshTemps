#ifndef HISTORY_HOUR_STAGER_H_
#define HISTORY_HOUR_STAGER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "history_storage_limits.h"

// Backend-neutral current-hour history model for MeshTemps.
//
// This layer intentionally has no dependency on MeshNode, FRAM, SD, LVGL,
// Wire, or any hardware-specific storage. RamHourStager is the first backend;
// a future FramHourStager should implement the same IHistoryHourStager seam and
// export the same HistoryHourSnapshot shape.

constexpr std::size_t kHistorySlotCapacity =
    kMeshTempsHistoryMaxSensorsPerHour;
constexpr std::size_t kHistoryMinutesPerHour = kMeshTempsHistoryMinutesPerHour;
static_assert(kHistorySlotCapacity == kMeshTempsHistoryMaxSensorsPerHour,
              "stager slot capacity must follow the product history sensor limit");
static_assert(kHistoryMinutesPerHour == kMeshTempsHistoryMinutesPerHour,
              "stager minute count must follow the product history minute limit");
constexpr std::size_t kHistoryBitmapBytes =
    (kHistorySlotCapacity + 7U) / 8U;
constexpr uint16_t kHistoryHourSnapshotFormatVersion = 1;
constexpr int16_t kHistoryInvalidTempCentiC =
    std::numeric_limits<int16_t>::min();

struct HistoryCentiCResult {
  bool valid = false;
  int16_t value = kHistoryInvalidTempCentiC;
};

HistoryCentiCResult HistoryTempCToCentiC(float temp_c);
bool HistoryParseAddr16ToRom64(const char* addr16, uint64_t* out_rom64);
bool HistoryNormalizeAddr16(const char* addr16, char out_addr16[17]);

struct HistorySlotDescriptor {
  uint8_t slot_id = 0;
  bool active = false;
  char addr16[17] = {};
  uint64_t rom64 = 0;
  uint32_t last_known_node_id = 0;
  uint8_t first_seen_minute = 0;
  uint8_t last_seen_minute = 0;
  uint32_t sample_count = 0;
  uint32_t missing_or_invalid_count = 0;
};

struct HistoryMinuteFrame {
  std::array<uint8_t, kHistoryBitmapBytes> presence{};
  std::array<uint8_t, kHistoryBitmapBytes> corrected{};
  std::array<int16_t, kHistorySlotCapacity> temp_c_x100{};

  void Clear();
  bool IsPresent(uint8_t slot_id) const;
  bool IsCorrected(uint8_t slot_id) const;
  int16_t TemperatureCentiC(uint8_t slot_id) const;
  bool SetSample(uint8_t slot_id, int16_t temp_c_x100, bool corrected_value);
  void ClearSample(uint8_t slot_id);
};

struct HistoryStagerStatus {
  uint32_t hour_start_epoch_minute = 0;
  uint8_t active_slot_count = 0;
  uint32_t samples_recorded = 0;
  uint32_t missing_samples_recorded = 0;
  uint32_t overflow_count = 0;
  uint32_t invalid_argument_count = 0;
  uint32_t invalid_temperature_count = 0;
  bool hour_active = false;
  bool overflowed = false;
};

struct HistoryHourSnapshot {
  uint16_t format_version = kHistoryHourSnapshotFormatVersion;
  uint32_t hour_start_epoch_minute = 0;
  uint8_t active_slot_count = 0;
  std::array<HistorySlotDescriptor, kHistorySlotCapacity> slots{};
  std::array<HistoryMinuteFrame, kHistoryMinutesPerHour> frames{};
  HistoryStagerStatus status{};
};

class IHistoryHourStager {
 public:
  virtual ~IHistoryHourStager() = default;

  virtual void Clear() = 0;
  virtual bool ResetHour(uint32_t hour_start_epoch_minute) = 0;

  virtual bool FindOrCreateSlot(const char* addr16,
                                uint32_t node_id,
                                uint8_t minute_index,
                                uint8_t* out_slot_id) = 0;

  virtual bool RecordSample(const char* addr16,
                            uint32_t node_id,
                            uint8_t minute_index,
                            float temp_c,
                            bool corrected) = 0;

  virtual bool RecordSampleCentiC(const char* addr16,
                                  uint32_t node_id,
                                  uint8_t minute_index,
                                  int16_t temp_c_x100,
                                  bool corrected) = 0;

  virtual bool RecordMissing(const char* addr16,
                             uint32_t node_id,
                             uint8_t minute_index) = 0;

  virtual bool ExportSnapshot(HistoryHourSnapshot* out_snapshot) const = 0;
  virtual HistoryStagerStatus status() const = 0;
};

class RamHourStager final : public IHistoryHourStager {
 public:
  RamHourStager();

  void Clear() override;
  bool ResetHour(uint32_t hour_start_epoch_minute) override;

  bool FindOrCreateSlot(const char* addr16,
                        uint32_t node_id,
                        uint8_t minute_index,
                        uint8_t* out_slot_id) override;

  bool RecordSample(const char* addr16,
                    uint32_t node_id,
                    uint8_t minute_index,
                    float temp_c,
                    bool corrected) override;

  bool RecordSampleCentiC(const char* addr16,
                          uint32_t node_id,
                          uint8_t minute_index,
                          int16_t temp_c_x100,
                          bool corrected) override;

  bool RecordMissing(const char* addr16,
                     uint32_t node_id,
                     uint8_t minute_index) override;

  bool ExportSnapshot(HistoryHourSnapshot* out_snapshot) const override;
  HistoryStagerStatus status() const override { return status_; }

  const HistorySlotDescriptor* slot(uint8_t slot_id) const;
  const HistoryMinuteFrame* frame(uint8_t minute_index) const;

 private:
  bool ValidateMinute_(uint8_t minute_index);
  int FindSlotByRom_(uint64_t rom64) const;
  void MarkSlotSeen_(HistorySlotDescriptor* slot,
                     uint32_t node_id,
                     uint8_t minute_index);
  bool RecordSampleForSlot_(uint8_t slot_id,
                            uint8_t minute_index,
                            int16_t temp_c_x100,
                            bool corrected);
  bool RecordInvalidForSlot_(uint8_t slot_id, uint8_t minute_index);

  HistoryStagerStatus status_{};
  std::array<HistorySlotDescriptor, kHistorySlotCapacity> slots_{};
  std::array<HistoryMinuteFrame, kHistoryMinutesPerHour> frames_{};
};

#endif  // HISTORY_HOUR_STAGER_H_
