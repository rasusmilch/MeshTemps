#ifndef HISTORY_AGGREGATOR_H_
#define HISTORY_AGGREGATOR_H_

#include <Arduino.h>
#include <stdint.h>
#include <vector>

#include "fram_hour_journal.h"
#include "sd_history_store.h"
#include "mesh_node.h"
#include "fram_daily_state_store.h"

// Aggregates current MeshNode live sensor values into 1-minute frames.
//
// This is intended to run on the GUI node:
//  - Serial RX updates MeshNode objects in RAM.
//  - Once per minute, PollAndAppendMinute() snapshots values into FRAM.
//  - On hour completion, FlushHourToSd() writes FRAM frames to SD with CRC+verify.
//
// Dynamic sensors:
//  - First pass: descriptor (slot order) is rebuilt only at hour boundary.
//  - New sensors seen mid-hour are not logged until next hour boundary.
class HistoryAggregator {
 public:
  bool Begin(FramStorageInterface* fram,
             uint32_t fram_base_address,
             uint32_t fram_region_bytes,
             fs::FS& sd_fs,
             const char* sd_base_dir);

  // Call frequently from loop() (e.g. each iteration).
  // This checks for minute rollovers and triggers snapshots/flushes.
  void Tick(uint32_t now_ms);

  void set_stale_threshold_ms(uint32_t threshold_ms) {
    stale_threshold_ms_ = threshold_ms;
  }

  void set_max_sensors(uint16_t max_sensors) { max_sensors_ = max_sensors; }

  struct HourComputationResult {
    uint32_t payload_crc32 = 0;
    uint64_t bad_frame_mask = 0;
    std::vector<SdHistoryStore::HourlyRollupEntry> rollups;
  };

  bool ComputeHourCrcAndRollup_(HourComputationResult* out) const;

  bool StreamHourPayloadForSd_(const HourComputationResult& info,
                                std::function<bool(const void*, size_t)> sink) const;

  static uint32_t DayStartEpochMinuteLocal_(uint32_t epoch_seconds);

  void MaybeRotateDay_(uint32_t epoch_seconds);
  void UpdateDailyFromHourly_(const std::vector<SdHistoryStore::HourlyRollupEntry>& hourly_rollups);
  bool FlushDailyToSd_();
  bool FlushHourToSd_(uint32_t hour_start_epoch_minute,
                      std::vector<SdHistoryStore::HourlyRollupEntry>* out_hourly_rollups);

  bool InitializeIfTimeValid_(uint32_t epoch_seconds);
  bool LoadOrRebuildDailyState_(uint32_t epoch_seconds);
  bool RebuildDailyFromSd_(uint32_t day_start_epoch_minute, uint32_t day_end_epoch_minute);
  bool SaveDailyStateToFram_();

  uint32_t fram_base_address_ = 0;
  uint32_t fram_region_bytes_ = 0;

  uint32_t fram_hour_base_address_ = 0;
  uint32_t fram_hour_region_bytes_ = 0;

  uint32_t fram_daily_base_address_ = 0;
  uint32_t fram_daily_region_bytes_ = 0;

  FramDailyStateStore daily_state_store_;
  uint32_t daily_state_sequence_ = 0;
  bool storages_initialized_ = false;

 private:
  struct SensorSlot {
    uint32_t node_id;
    String address_hex16;  // DS18B20 ROM as 16 hex chars
    uint64_t rom64;        // parsed ROM for compact SD descriptor storage
  };

  struct DailyAccumulatorEntry {
    uint32_t node_id = 0;
    uint64_t rom64 = 0;

    int64_t sum_centi_c = 0;        // accumulated sum of minute samples (approx via mean*count)
    uint16_t sample_count = 0;      // 0..1440
    int16_t min_centi_c = 0x7FFF;
    int16_t max_centi_c = static_cast<int16_t>(0x8001);
  };

  uint32_t current_day_start_epoch_minute_ = 0;
  std::vector<DailyAccumulatorEntry> daily_entries_;

  bool IsTimeValid_() const;
  uint32_t NowEpochSeconds_() const;

  bool RebuildDescriptorIfNeeded_(bool force);
  bool PollAndAppendMinute_(uint32_t now_ms, uint32_t epoch_seconds);

  bool FlushHourToSd_(uint32_t hour_start_epoch_minute);

  static bool ParseHex16ToU64_(const String& hex16, uint64_t* out);

  FramHourJournal fram_journal_;
  SdHistoryStore sd_store_;

  FramStorageInterface* fram_ = nullptr;

  std::vector<SensorSlot> slots_;
  uint16_t max_sensors_ = 100;

  uint32_t stale_threshold_ms_ = 180000u;  // 3 minutes default
  int32_t last_epoch_minute_seen_ = -1;
};

#endif  // HISTORY_AGGREGATOR_H_
