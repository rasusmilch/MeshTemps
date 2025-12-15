#ifndef SD_HISTORY_STORE_H_
#define SD_HISTORY_STORE_H_

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>
#include <vector>
#include <functional>

class SdHistoryStore {
 public:
  static constexpr uint16_t kVersion = 1;

  struct HourlyRollupEntry {
    int16_t mean_centi_c;   // 0x8000 if no samples
    int16_t min_centi_c;    // 0x8000 if no samples
    int16_t max_centi_c;    // 0x8000 if no samples
    uint16_t sample_count;  // 0..60
  };

  bool Begin(fs::FS& fs, const char* base_dir);

  bool AppendMinuteHourBlock(uint32_t hour_start_epoch_minute,
                             uint16_t sensor_count,
                             uint16_t frame_bytes,
                             uint16_t presence_bytes_padded,
                             uint64_t bad_frame_mask,
                             uint32_t payload_crc32,
                             const uint32_t* sensor_node_ids,
                             const uint64_t* sensor_rom64,
                             const std::function<bool(std::function<bool(const void*, size_t)>)>& payload_streamer);

  bool AppendHourlyRollupBlock(uint32_t hour_start_epoch_minute,
                               uint16_t sensor_count,
                               const uint32_t* sensor_node_ids,
                               const uint64_t* sensor_rom64,
                               const HourlyRollupEntry* rollups);

  bool AppendDailyRollupBlock(uint32_t day_start_epoch_minute,
                              uint16_t sensor_count,
                              const uint32_t* sensor_node_ids,
                              const uint64_t* sensor_rom64,
                              const HourlyRollupEntry* daily_rollups);

  // Scan hourly rollup records in [start_epoch_minute, end_epoch_minute).
  // Callback pointers are only valid during the callback.
  bool ScanHourlyRollups(
      uint32_t start_epoch_minute,
      uint32_t end_epoch_minute,
      const std::function<void(uint32_t hour_start_epoch_minute,
                               uint16_t sensor_count,
                               const uint32_t* sensor_node_ids,
                               const uint64_t* sensor_rom64,
                               const HourlyRollupEntry* rollups)>& on_record) const;
  
  bool HasMinuteHourBlock(uint32_t hour_start_epoch_minute) const;
  bool HasHourlyRollupBlock(uint32_t hour_start_epoch_minute) const;

 private:
  static constexpr uint32_t FourCc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
  }

  static constexpr uint32_t kMagicMinute = FourCc('M','I','N','H');
  static constexpr uint32_t kMagicRollup = FourCc('H','R','O','L');

  struct MinuteHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t record_bytes;

    uint32_t hour_start_epoch_minute;
    uint16_t sensor_count;
    uint16_t frame_bytes;
    uint16_t presence_bytes_padded;
    uint16_t reserved0;

    uint64_t bad_frame_mask;   // 1 bit per minute (0..59) that was replaced with a missing frame
    uint32_t descriptor_bytes;
    uint32_t payload_bytes;
    uint32_t payload_crc32;    // CRC32 over descriptor + payload

    uint32_t header_crc32;     // CRC32 over header with header_crc32=0
  };

  struct RollupHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t record_bytes;

    uint32_t hour_start_epoch_minute;
    uint16_t sensor_count;
    uint16_t reserved0;

    uint32_t descriptor_bytes;
    uint32_t rollup_bytes;
    uint32_t payload_crc32;    // CRC32 over descriptor + rollup payload
    uint32_t header_crc32;     // CRC32 over header with header_crc32=0
  };

  static constexpr uint32_t kMagicDaily = FourCc('D','R','O','L');  // Daily ROLlup

  struct DailyHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t record_bytes;

    uint32_t day_start_epoch_minute;  // local midnight, epoch_minutes
    uint16_t sensor_count;
    uint16_t reserved0;

    uint32_t descriptor_bytes;        // sensor_count * (u32 + u64)
    uint32_t rollup_bytes;            // sensor_count * sizeof(HourlyRollupEntry)
    uint32_t payload_crc32;           // CRC32 over descriptor + rollup payload
    uint32_t header_crc32;            // CRC32 over header with header_crc32=0
  };

  bool EnsureDirExists_(const char* path);
  String MakeMinuteFilePath_(uint32_t hour_start_epoch_minute) const;
  String MakeHourlyFilePath_(uint32_t hour_start_epoch_minute) const;

  static bool WriteAll_(File& file, const void* data, size_t length);
  static uint32_t ComputeHeaderCrc32_(const void* header, size_t header_bytes);

  bool VerifyMinuteRecord_(const String& path, uint32_t record_offset) const;
  bool VerifyRollupRecord_(const String& path, uint32_t record_offset) const;

  String MakeDailyFilePath_(uint32_t day_start_epoch_minute) const;
  bool VerifyDailyRecord_(const String& path, uint32_t record_offset) const;

  fs::FS* fs_ = nullptr;
  String base_dir_;
};

#endif  // SD_HISTORY_STORE_H_
