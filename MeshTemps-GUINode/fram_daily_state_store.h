#ifndef FRAM_DAILY_STATE_STORE_H_
#define FRAM_DAILY_STATE_STORE_H_

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "fram_storage_interface.h"

class FramDailyStateStore {
 public:
  static constexpr uint32_t kMagic = 0x53445246u;  // 'FRDS'
  static constexpr uint16_t kVersion = 1;

  struct Entry {
    uint32_t node_id = 0;
    uint64_t rom64 = 0;

    int64_t sum_centi_c = 0;
    uint16_t sample_count = 0;
    int16_t min_centi_c = 0x7FFF;
    int16_t max_centi_c = static_cast<int16_t>(0x8001);
  };

  struct State {
    uint32_t day_start_epoch_minute = 0;
    uint32_t sequence = 0;
    std::vector<Entry> entries;
  };

  // Layout inside the provided region:
  //   slot A @ base + 0 * slot_bytes
  //   slot B @ base + 1 * slot_bytes
  // Each slot is: Header (64 bytes) + payload (entries) up to slot_bytes.
  bool Begin(FramStorageInterface* fram, uint32_t base_address, uint32_t region_bytes);

  bool Load(State* out) const;
  bool Save(const State& state);

 private:
  static constexpr uint32_t kHeaderBytes = 64;

  struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t sequence;

    uint32_t day_start_epoch_minute;
    uint16_t entry_count;
    uint16_t entry_bytes;

    uint32_t payload_bytes;
    uint32_t payload_crc32;

    uint32_t header_crc32;
    uint8_t reserved[kHeaderBytes - 4 - 2 - 2 - 4 - 4 - 2 - 2 - 4 - 4 - 4];
  };

  static_assert(sizeof(Header) == kHeaderBytes, "Header must be 64 bytes");

  uint32_t SlotBaseA_() const { return base_address_ + 0u * slot_bytes_; }
  uint32_t SlotBaseB_() const { return base_address_ + 1u * slot_bytes_; }

  static uint32_t ComputeHeaderCrc32_(Header header);
  static uint32_t ComputePayloadCrc32_(const void* data, size_t length);

  bool ReadHeader_(uint32_t slot_base, Header* out) const;
  bool ReadPayloadAndVerify_(uint32_t slot_base, const Header& header,
                             std::vector<Entry>* out_entries) const;

  bool WriteBytesVerified_(uint32_t address, const void* data, size_t length) const;
  bool ReadBytesChecked_(uint32_t address, void* data, size_t length) const;

  FramStorageInterface* fram_ = nullptr;
  uint32_t base_address_ = 0;
  uint32_t region_bytes_ = 0;

  uint32_t slot_bytes_ = 4096;  // fixed (must fit entries)
  bool initialized_ = false;
};

#endif  // FRAM_DAILY_STATE_STORE_H_
