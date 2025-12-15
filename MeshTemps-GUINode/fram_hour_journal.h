#ifndef FRAM_HOUR_JOURNAL_H_
#define FRAM_HOUR_JOURNAL_H_

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <functional>

#include "fram_storage_interface.h"

class FramHourJournal {
 public:
  static constexpr uint32_t kMagic = 0x4A485246u;  // 'FRHJ'
  static constexpr uint16_t kVersion = 1;

  // FRAM region layout:
  //   base + 0x000: Header A (256 bytes reserved)
  //   base + 0x100: Header B (256 bytes reserved)
  //   base + 0x200: Frames region
  //
  // This class assumes the region is dedicated to the hour journal.
  bool Begin(FramStorageInterface* fram,
             uint32_t base_address,
             uint32_t region_bytes,
             uint16_t sensor_count,
             uint32_t now_epoch_seconds);

  // Append one minute snapshot (epoch_seconds determines minute index in hour).
  //
  // presence_bitmap: ceil(sensor_count/8) bits, packed low-bit-first within each byte.
  // values_centi_c: int16 values in the current descriptor order (length = sensor_count)
  //
  // Returns false if write/verify fails.
  bool AppendMinute(uint32_t epoch_seconds,
                    const uint8_t* presence_bitmap,
                    size_t presence_bytes,
                    const int16_t* values_centi_c,
                    size_t value_count);

  bool IsHourComplete() const { return minutes_written_ >= 60; }
  uint8_t minutes_written() const { return minutes_written_; }
  uint32_t hour_start_epoch_minute() const { return hour_start_epoch_minute_; }
  uint16_t sensor_count() const { return sensor_count_; }
  uint16_t frame_bytes() const { return frame_bytes_; }

  // Export exactly 60 frames (filled gaps become missing frames) to |writer|.
  // The callback is invoked with a byte buffer in increasing offset order.
  bool ExportFullHourFrames(const std::function<bool(uint32_t offset, const void* data, size_t length)>& writer) const;

  // Reset journal to start a new hour based on now_epoch_seconds.
  bool ResetToCurrentHour(uint32_t now_epoch_seconds);

  uint16_t presence_bytes_padded() const { return presence_bytes_; }

  bool ReadRawFrame(uint8_t minute_index, void* buffer, size_t length) const;
  bool FrameCrcOk(const uint8_t* frame_bytes, size_t frame_len) const;

  // Build a canonical "missing" frame (presence=0, all values=0x8000, CRC computed,
  // padding zeroed). Used if FRAM corruption is detected.
  bool BuildMissingFrame(uint8_t* out_frame_bytes, size_t out_len) const;

 private:
  struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t sequence;
    uint16_t sensor_count;
    uint16_t frame_bytes;
    uint32_t hour_start_epoch_minute;
    uint8_t minutes_written;
    uint8_t reserved0[3];
    uint32_t last_epoch_minute_written;
    uint32_t header_crc32;
  };

  bool LoadBestHeader(Header* out_header) const;
  bool WriteHeaderAtomic(const Header& header);

  bool WriteBytesVerified(uint32_t address, const void* data, size_t length) const;
  bool ReadBytesChecked(uint32_t address, void* data, size_t length) const;

  uint32_t HeaderAddressA() const { return base_address_ + 0x000u; }
  uint32_t HeaderAddressB() const { return base_address_ + 0x100u; }
  uint32_t FramesBaseAddress() const { return base_address_ + 0x200u; }

  uint32_t FrameAddress(uint8_t minute_index) const {
    return FramesBaseAddress() + static_cast<uint32_t>(minute_index) * frame_bytes_;
  }

  static uint32_t EpochSecondsToEpochMinute(uint32_t epoch_seconds) {
    return epoch_seconds / 60u;
  }

  static uint32_t HourStartEpochMinute(uint32_t epoch_minute) {
    return epoch_minute - (epoch_minute % 60u);
  }

  static size_t AlignUp(size_t value, size_t alignment) {
    return (value + (alignment - 1)) & ~(alignment - 1);
  }

  bool FillMissingFramesUpTo(uint32_t target_epoch_minute);
  bool WriteFrame(uint8_t minute_index,
                  const uint8_t* presence_bitmap,
                  size_t presence_bytes,
                  const int16_t* values_centi_c,
                  size_t value_count);

  FramStorageInterface* fram_ = nullptr;
  uint32_t base_address_ = 0;
  uint32_t region_bytes_ = 0;

  uint16_t sensor_count_ = 0;
  uint16_t frame_bytes_ = 0;
  uint16_t presence_bytes_ = 0;

  uint32_t hour_start_epoch_minute_ = 0;
  uint32_t last_epoch_minute_written_ = 0;
  uint8_t minutes_written_ = 0;

  uint32_t header_sequence_ = 0;
  bool initialized_ = false;
};

#endif  // FRAM_HOUR_JOURNAL_H_
