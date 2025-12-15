#include "fram_hour_journal.h"

#include <string.h>

#include "history_crc.h"

namespace {

constexpr size_t kHeaderReserveBytes = 256;
constexpr uint16_t kInvalidTempSentinel = 0x8000u;

bool IsHeaderSane(const FramHourJournal::Header& header) {
  if (header.magic != FramHourJournal::kMagic) return false;
  if (header.version != FramHourJournal::kVersion) return false;
  if (header.header_bytes != sizeof(FramHourJournal::Header)) return false;
  if (header.sensor_count == 0 || header.sensor_count > 1000) return false;
  if (header.frame_bytes == 0 || header.frame_bytes > 4096) return false;
  if (header.minutes_written > 60) return false;
  return true;
}

uint32_t ComputeHeaderCrc32(FramHourJournal::Header header_without_crc) {
  header_without_crc.header_crc32 = 0;
  return Crc32(reinterpret_cast<const uint8_t*>(&header_without_crc),
               sizeof(header_without_crc));
}

}  // namespace

bool FramHourJournal::Begin(FramStorageInterface* fram,
                            uint32_t base_address,
                            uint32_t region_bytes,
                            uint16_t sensor_count,
                            uint32_t now_epoch_seconds) {
  fram_ = fram;
  base_address_ = base_address;
  region_bytes_ = region_bytes;
  sensor_count_ = sensor_count;

  if (fram_ == nullptr) return false;
  if (region_bytes_ < (0x200u + 60u * 16u)) return false;  // minimal sanity
  if (sensor_count_ == 0 || sensor_count_ > 100) return false;

  // Compute frame sizing.
  const size_t raw_presence_bytes = (static_cast<size_t>(sensor_count_) + 7u) / 8u;
  presence_bytes_ = static_cast<uint16_t>(AlignUp(raw_presence_bytes, 4u));
  const size_t values_bytes = static_cast<size_t>(sensor_count_) * sizeof(int16_t);
  const size_t crc_bytes = sizeof(uint16_t);
  frame_bytes_ = static_cast<uint16_t>(AlignUp(presence_bytes_ + values_bytes + crc_bytes, 4u));

  Header header;
  if (LoadBestHeader(&header)) {
    // Adopt header state.
    header_sequence_ = header.sequence;
    sensor_count_ = header.sensor_count;
    frame_bytes_ = header.frame_bytes;
    hour_start_epoch_minute_ = header.hour_start_epoch_minute;
    last_epoch_minute_written_ = header.last_epoch_minute_written;
    minutes_written_ = header.minutes_written;

    // If the header claims a different sensor_count/frame_bytes than current request,
    // we honor the on-FRAM state (resume) only if it matches the requested sensor_count.
    if (sensor_count_ != sensor_count) {
      // Caller requested a different layout; start fresh.
      return ResetToCurrentHour(now_epoch_seconds);
    }

    initialized_ = true;
    return true;
  }

  // No valid header; start fresh.
  return ResetToCurrentHour(now_epoch_seconds);
}

bool FramHourJournal::ResetToCurrentHour(uint32_t now_epoch_seconds) {
  if (fram_ == nullptr) return false;

  const uint32_t now_epoch_minute = EpochSecondsToEpochMinute(now_epoch_seconds);
  hour_start_epoch_minute_ = HourStartEpochMinute(now_epoch_minute);
  last_epoch_minute_written_ = hour_start_epoch_minute_ - 1u;
  minutes_written_ = 0;

  Header header;
  memset(&header, 0, sizeof(header));
  header.magic = kMagic;
  header.version = kVersion;
  header.header_bytes = sizeof(Header);
  header.sequence = header_sequence_ + 1u;
  header.sensor_count = sensor_count_;
  header.frame_bytes = frame_bytes_;
  header.hour_start_epoch_minute = hour_start_epoch_minute_;
  header.minutes_written = minutes_written_;
  header.last_epoch_minute_written = last_epoch_minute_written_;
  header.header_crc32 = ComputeHeaderCrc32(header);

  if (!WriteHeaderAtomic(header)) return false;

  header_sequence_ = header.sequence;
  initialized_ = true;
  return true;
}

bool FramHourJournal::AppendMinute(uint32_t epoch_seconds,
                                   const uint8_t* presence_bitmap,
                                   size_t presence_bytes,
                                   const int16_t* values_centi_c,
                                   size_t value_count) {
  if (!initialized_ || fram_ == nullptr) return false;
  if (presence_bitmap == nullptr || values_centi_c == nullptr) return false;
  if (value_count != sensor_count_) return false;

  const uint32_t epoch_minute = EpochSecondsToEpochMinute(epoch_seconds);
  const uint32_t hour_start = HourStartEpochMinute(epoch_minute);

  // If we crossed into a new hour, start a new journal.
  if (hour_start != hour_start_epoch_minute_) {
    if (!ResetToCurrentHour(epoch_seconds)) return false;
  }

  // Fill any missing minutes (gaps) up to this epoch_minute.
  if (!FillMissingFramesUpTo(epoch_minute)) return false;

  if (minutes_written_ >= 60) {
    return true;  // hour already complete; caller should flush/reset
  }

  // Write the actual frame.
  if (!WriteFrame(minutes_written_, presence_bitmap, presence_bytes, values_centi_c, value_count)) {
    return false;
  }

  last_epoch_minute_written_ = epoch_minute;
  ++minutes_written_;

  // Update header atomically.
  Header header;
  memset(&header, 0, sizeof(header));
  header.magic = kMagic;
  header.version = kVersion;
  header.header_bytes = sizeof(Header);
  header.sequence = header_sequence_ + 1u;
  header.sensor_count = sensor_count_;
  header.frame_bytes = frame_bytes_;
  header.hour_start_epoch_minute = hour_start_epoch_minute_;
  header.minutes_written = minutes_written_;
  header.last_epoch_minute_written = last_epoch_minute_written_;
  header.header_crc32 = ComputeHeaderCrc32(header);

  if (!WriteHeaderAtomic(header)) return false;
  header_sequence_ = header.sequence;

  return true;
}

bool FramHourJournal::FillMissingFramesUpTo(uint32_t target_epoch_minute) {
  if (minutes_written_ >= 60) return true;

  uint32_t next_expected = last_epoch_minute_written_ + 1u;
  while (next_expected < target_epoch_minute && minutes_written_ < 60) {
    // Write an all-missing frame.
    uint8_t presence[32];
    memset(presence, 0, sizeof(presence));

    // For missing values we still write sentinel to avoid stale bytes.
    // We write in chunks to avoid large stack arrays.
    const uint16_t missing = static_cast<uint16_t>(kInvalidTempSentinel);

    // Build a temp frame in a small buffer and stream it into FRAM.
    // For first pass: use WriteFrame with a generated values array chunked.
    int16_t values[100];
    for (uint16_t i = 0; i < sensor_count_; ++i) {
      values[i] = static_cast<int16_t>(missing);
    }

    if (!WriteFrame(minutes_written_, presence, presence_bytes_, values, sensor_count_)) {
      return false;
    }

    last_epoch_minute_written_ = next_expected;
    ++minutes_written_;
    ++next_expected;
  }
  return true;
}

bool FramHourJournal::WriteFrame(uint8_t minute_index,
                                 const uint8_t* presence_bitmap,
                                 size_t presence_bytes,
                                 const int16_t* values_centi_c,
                                 size_t value_count) {
  if (minute_index >= 60) return false;

  // Frame layout:
  //   [presence_bytes_] presence bitmap (padded to 4-byte alignment)
  //   [sensor_count_*2] int16 values
  //   [2]               CRC16 over presence+values
  //   [pad]             alignment to frame_bytes_
  const uint32_t frame_addr = FrameAddress(minute_index);

  // Write presence bitmap (padded).
  uint8_t presence_padded[32];
  memset(presence_padded, 0, sizeof(presence_padded));
  const size_t to_copy = (presence_bytes < presence_bytes_) ? presence_bytes : presence_bytes_;
  memcpy(presence_padded, presence_bitmap, to_copy);

  if (!WriteBytesVerified(frame_addr, presence_padded, presence_bytes_)) return false;

  // Write values.
  const uint32_t values_addr = frame_addr + presence_bytes_;
  const size_t values_bytes = static_cast<size_t>(value_count) * sizeof(int16_t);
  if (!WriteBytesVerified(values_addr, values_centi_c, values_bytes)) return false;

  // Compute CRC16 over presence+values by reading back (guarantees we CRC what is in FRAM).
  // This also serves as verification against I2C/SPI write glitches.
  uint16_t crc16 = 0xFFFFu;

  // Read back presence+values in chunks.
  uint8_t buffer[64];
  uint32_t read_addr = frame_addr;
  size_t remaining = presence_bytes_ + values_bytes;
  while (remaining > 0) {
    const size_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
    if (!ReadBytesChecked(read_addr, buffer, chunk)) return false;
    crc16 = Crc16CcittFalseUpdate(crc16, buffer, chunk);
    read_addr += static_cast<uint32_t>(chunk);
    remaining -= chunk;
  }

  // Write CRC16 last (atomic-ish).
  const uint32_t crc_addr = frame_addr + presence_bytes_ + static_cast<uint32_t>(values_bytes);
  if (!WriteBytesVerified(crc_addr, &crc16, sizeof(crc16))) return false;

  // After writing CRC16 in WriteFrame(), add:
  const uint32_t pad_addr = crc_addr + sizeof(uint16_t);
  const size_t used = presence_bytes_ + values_bytes + sizeof(uint16_t);
  if (frame_bytes_ > used) {
    const size_t pad_len = static_cast<size_t>(frame_bytes_) - used;
    uint8_t zeros[32];
    memset(zeros, 0, sizeof(zeros));
    size_t remaining = pad_len;
    uint32_t addr = pad_addr;
    while (remaining > 0) {
        const size_t chunk = (remaining > sizeof(zeros)) ? sizeof(zeros) : remaining;
        if (!WriteBytesVerified(addr, zeros, chunk)) return false;
        addr += static_cast<uint32_t>(chunk);
        remaining -= chunk;
    }
  }


  return true;
}

bool FramHourJournal::ExportFullHourFrames(
    const std::function<bool(uint32_t offset, const void* data, size_t length)>& writer) const {
  if (!initialized_ || fram_ == nullptr) return false;
  if (!writer) return false;

  // Ensure the first pass exports exactly 60 frames worth of bytes.
  const size_t total = static_cast<size_t>(frame_bytes_) * 60u;

  uint8_t buffer[128];
  size_t offset = 0;
  while (offset < total) {
    const size_t chunk = ((total - offset) > sizeof(buffer)) ? sizeof(buffer) : (total - offset);
    const uint32_t addr = FramesBaseAddress() + static_cast<uint32_t>(offset);
    if (!ReadBytesChecked(addr, buffer, chunk)) return false;
    if (!writer(static_cast<uint32_t>(offset), buffer, chunk)) return false;
    offset += chunk;
  }
  return true;
}

bool FramHourJournal::LoadBestHeader(Header* out_header) const {
  if (fram_ == nullptr || out_header == nullptr) return false;

  Header a, b;
  if (!ReadBytesChecked(HeaderAddressA(), &a, sizeof(a))) memset(&a, 0, sizeof(a));
  if (!ReadBytesChecked(HeaderAddressB(), &b, sizeof(b))) memset(&b, 0, sizeof(b));

  auto is_valid = [](const Header& h) -> bool {
    if (!IsHeaderSane(h)) return false;
    const uint32_t expected = ComputeHeaderCrc32(h);
    return expected == h.header_crc32;
  };

  const bool a_valid = is_valid(a);
  const bool b_valid = is_valid(b);

  if (!a_valid && !b_valid) return false;
  if (a_valid && !b_valid) { *out_header = a; return true; }
  if (!a_valid && b_valid) { *out_header = b; return true; }

  // Both valid: pick highest sequence.
  *out_header = (a.sequence >= b.sequence) ? a : b;
  return true;
}

bool FramHourJournal::WriteHeaderAtomic(const Header& header) {
  // Alternate header slots by sequence parity.
  const bool write_a = (header.sequence % 2u) == 0u;
  const uint32_t addr = write_a ? HeaderAddressA() : HeaderAddressB();
  return WriteBytesVerified(addr, &header, sizeof(header));
}

bool FramHourJournal::ReadBytesChecked(uint32_t address, void* data, size_t length) const {
  if (fram_ == nullptr) return false;
  return fram_->ReadBytes(address, data, length);
}

bool FramHourJournal::WriteBytesVerified(uint32_t address, const void* data, size_t length) const {
  if (fram_ == nullptr) return false;
  if (!fram_->WriteBytes(address, data, length)) return false;

  // Read-back verify in small chunks.
  const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
  uint8_t buffer[64];
  size_t offset = 0;
  while (offset < length) {
    const size_t chunk = ((length - offset) > sizeof(buffer)) ? sizeof(buffer) : (length - offset);
    if (!fram_->ReadBytes(address + static_cast<uint32_t>(offset), buffer, chunk)) return false;
    if (memcmp(buffer, src + offset, chunk) != 0) return false;
    offset += chunk;
  }
  return true;
}

bool FramHourJournal::ReadRawFrame(uint8_t minute_index, void* buffer, size_t length) const {
  if (!initialized_ || fram_ == nullptr) return false;
  if (minute_index >= 60) return false;
  if (length < frame_bytes_) return false;
  return ReadBytesChecked(FrameAddress(minute_index), buffer, frame_bytes_);
}

bool FramHourJournal::FrameCrcOk(const uint8_t* frame_bytes, size_t frame_len) const {
  if (frame_bytes == nullptr) return false;
  if (frame_len < frame_bytes_) return false;

  const size_t values_bytes = static_cast<size_t>(sensor_count_) * sizeof(int16_t);
  const size_t crc_offset = static_cast<size_t>(presence_bytes_) + values_bytes;
  if (crc_offset + sizeof(uint16_t) > frame_bytes_) return false;

  uint16_t stored_crc16 = 0;
  memcpy(&stored_crc16, frame_bytes + crc_offset, sizeof(stored_crc16));

  uint16_t computed_crc16 = 0xFFFFu;
  computed_crc16 = Crc16CcittFalseUpdate(computed_crc16, frame_bytes, crc_offset);
  return (computed_crc16 == stored_crc16);
}

bool FramHourJournal::BuildMissingFrame(uint8_t* out_frame_bytes, size_t out_len) const {
  if (out_frame_bytes == nullptr) return false;
  if (out_len < frame_bytes_) return false;

  memset(out_frame_bytes, 0, frame_bytes_);

  // Presence bitmap (padded) already zero.
  const size_t values_bytes = static_cast<size_t>(sensor_count_) * sizeof(int16_t);
  const size_t values_offset = presence_bytes_;
  const size_t crc_offset = values_offset + values_bytes;

  // Fill values with sentinel.
  for (uint16_t sensor_index = 0; sensor_index < sensor_count_; ++sensor_index) {
    const int16_t sentinel = static_cast<int16_t>(0x8000);
    memcpy(out_frame_bytes + values_offset + sensor_index * sizeof(int16_t),
           &sentinel, sizeof(sentinel));
  }

  // CRC16 over presence+padded values.
  uint16_t crc16 = 0xFFFFu;
  crc16 = Crc16CcittFalseUpdate(crc16, out_frame_bytes, crc_offset);
  memcpy(out_frame_bytes + crc_offset, &crc16, sizeof(crc16));

  // Padding after CRC16 remains zero due to memset.
  return true;
}
