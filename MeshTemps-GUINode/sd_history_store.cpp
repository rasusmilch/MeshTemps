#include "sd_history_store.h"

#include "sd_finalized_hour_block.h"
#include "sd_finalized_hour_write_coalescer.h"

#include <string.h>
#include <time.h>

#include "history_crc.h"

namespace {

constexpr size_t kFinalizedHourWriteCoalescerBufferBytes = 4096U;
constexpr size_t kFinalizedVerifyBufferBytes = 512U;

// Shared finalized-hour SD I/O buffers. The owning storage/runtime service must
// serialize finalized-hour append/verify calls: this path is single-writer and
// non-reentrant, and must not be called concurrently or from LVGL callbacks.
alignas(4) static uint8_t
    g_finalized_hour_write_buffer[kFinalizedHourWriteCoalescerBufferBytes];
alignas(4) static uint8_t g_finalized_verify_buffer[kFinalizedVerifyBufferBytes];

struct FinalizedHourFileWriteContext {
  File* file = nullptr;
};

bool WriteFinalizedHourFileChunk(const uint8_t* data, size_t len, void* ctx) {
  FinalizedHourFileWriteContext* write_ctx =
      static_cast<FinalizedHourFileWriteContext*>(ctx);
  if (write_ctx == nullptr || write_ctx->file == nullptr) return false;
  if (len == 0U) return true;
  if (data == nullptr) return false;

  const size_t written = write_ctx->file->write(data, len);
  return written == len;
}

}  // namespace

bool SdHistoryStore::Begin(fs::FS& fs, const char* base_dir) {
  if (!SdHistoryCopyBaseDir(base_dir, base_dir_, sizeof(base_dir_))) return false;
  fs_ = &fs;

  // Legacy non-finalized-hour directories still use Arduino String and remain
  // allocation-audit debt. The finalized-hour path below uses only fixed char
  // buffers and custom bounded path builders.
  const String legacy_base(base_dir_);
  if (!EnsureDirExists_((legacy_base + "/minute").c_str())) return false;
  if (!EnsureDirExists_((legacy_base + "/hourly").c_str())) return false;
  if (!EnsureDirExists_((legacy_base + "/daily").c_str())) return false;

  return true;
}


bool SdHistoryStore::AppendFinalizedHourSnapshot(
    const HistoryHourSnapshot& snapshot) {
  if (fs_ == nullptr) return false;

  char finalized_dir[kSdHistoryPathMax];
  if (!BuildFinalizedDirPath_(finalized_dir, sizeof(finalized_dir))) return false;
  if (!EnsureDirExists_(finalized_dir)) return false;

  char path[kSdHistoryPathMax];
  if (!BuildFinalizedHourFilePath_(snapshot.hour_start_epoch_minute,
                                   path, sizeof(path))) {
    return false;
  }
  File file = fs_->open(path, FILE_APPEND);
  if (!file) return false;

  const uint32_t record_offset = static_cast<uint32_t>(file.position());
  FinalizedHourFileWriteContext write_ctx;
  write_ctx.file = &file;
  SdFinalizedHourWriteCoalescer coalescer(WriteFinalizedHourFileChunk,
                                          &write_ctx,
                                          g_finalized_hour_write_buffer,
                                          sizeof(g_finalized_hour_write_buffer));

  SdFinalizedHourBlockHeader header;
  SdFinalizedHourWriteStatus status;
  // Production finalized-hour writes are bounded: the codec streams header,
  // descriptors, and frames into a fixed-size coalescing sink. The coalescer is
  // flushed before File.flush()/close(), and read-back verification starts only
  // after the complete record has been flushed and closed.
  if (!WriteSdFinalizedHourBlock(snapshot, SdFinalizedHourCoalescerWriteFn,
                                 &coalescer, &header, &status)) {
    file.close();
    return false;
  }
  if (status.bytes_written != header.record_bytes ||
      coalescer.logical_bytes() != header.record_bytes) {
    file.close();
    return false;
  }
  if (!coalescer.Flush() || coalescer.failed() ||
      coalescer.physical_bytes() != header.record_bytes) {
    file.close();
    return false;
  }

  file.flush();
  file.close();
  return VerifyFinalizedHourRecord_(path, record_offset);
}

bool SdHistoryStore::AppendMinuteHourBlock(
    uint32_t hour_start_epoch_minute,
    uint16_t sensor_count,
    uint16_t frame_bytes,
    uint16_t presence_bytes_padded,
    uint64_t bad_frame_mask,
    uint32_t payload_crc32,
    const uint32_t* sensor_node_ids,
    const uint64_t* sensor_rom64,
    const std::function<bool(std::function<bool(const void*, size_t)>)>& payload_streamer) {
  if (fs_ == nullptr) return false;
  if (sensor_count == 0 || sensor_count > 100) return false;
  if (!payload_streamer) return false;
  if (sensor_node_ids == nullptr || sensor_rom64 == nullptr) return false;

  const String path = MakeMinuteFilePath_(hour_start_epoch_minute);

  File file = fs_->open(path.c_str(), FILE_APPEND);
  if (!file) return false;

  const uint32_t record_offset = static_cast<uint32_t>(file.position());

  const uint32_t descriptor_bytes =
      static_cast<uint32_t>(sensor_count) * (sizeof(uint32_t) + sizeof(uint64_t));
  const uint32_t payload_bytes =
      static_cast<uint32_t>(frame_bytes) * 60u;

  MinuteHeader header;
  memset(&header, 0, sizeof(header));
  header.magic = kMagicMinute;
  header.version = kVersion;
  header.header_bytes = sizeof(MinuteHeader);
  header.hour_start_epoch_minute = hour_start_epoch_minute;
  header.sensor_count = sensor_count;
  header.frame_bytes = frame_bytes;
  header.presence_bytes_padded = presence_bytes_padded;
  header.bad_frame_mask = bad_frame_mask;
  header.descriptor_bytes = descriptor_bytes;
  header.payload_bytes = payload_bytes;
  header.payload_crc32 = payload_crc32;
  header.record_bytes = sizeof(MinuteHeader) + descriptor_bytes + payload_bytes;

  header.header_crc32 = 0;
  header.header_crc32 = ComputeHeaderCrc32_(&header, sizeof(header));

  if (!WriteAll_(file, &header, sizeof(header))) {
    file.close();
    return false;
  }

  // Descriptor: {node_id, rom64} repeated
  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!WriteAll_(file, &sensor_node_ids[i], sizeof(uint32_t))) { file.close(); return false; }
    if (!WriteAll_(file, &sensor_rom64[i], sizeof(uint64_t))) { file.close(); return false; }
  }

  // Payload: stream frames in large-ish chunks; payload_streamer decides its chunking.
  const bool streamed_ok = payload_streamer([&](const void* data, size_t length) -> bool {
    return WriteAll_(file, data, length);
  });

  if (!streamed_ok) {
    file.close();
    return false;
  }

  file.flush();
  file.close();

  // Read-back verify CRC (no silent corruption).
  return VerifyMinuteRecord_(path, record_offset);
}

bool SdHistoryStore::AppendHourlyRollupBlock(uint32_t hour_start_epoch_minute,
                                             uint16_t sensor_count,
                                             const uint32_t* sensor_node_ids,
                                             const uint64_t* sensor_rom64,
                                             const HourlyRollupEntry* rollups) {
  if (fs_ == nullptr) return false;
  if (sensor_count == 0 || sensor_count > 100) return false;
  if (sensor_node_ids == nullptr || sensor_rom64 == nullptr || rollups == nullptr) return false;

  const String path = MakeHourlyFilePath_(hour_start_epoch_minute);

  // Precompute CRC32 over descriptor+rollup so we never need to seek-back patch.
  uint32_t crc = 0xFFFFFFFFu;
  for (uint16_t i = 0; i < sensor_count; ++i) {
    crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&sensor_node_ids[i]), sizeof(uint32_t));
    crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&sensor_rom64[i]), sizeof(uint64_t));
  }
  const uint32_t rollup_bytes = static_cast<uint32_t>(sensor_count) * sizeof(HourlyRollupEntry);
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(rollups), rollup_bytes);

  File file = fs_->open(path.c_str(), FILE_APPEND);
  if (!file) return false;

  const uint32_t record_offset = static_cast<uint32_t>(file.position());

  const uint32_t descriptor_bytes =
      static_cast<uint32_t>(sensor_count) * (sizeof(uint32_t) + sizeof(uint64_t));

  RollupHeader header;
  memset(&header, 0, sizeof(header));
  header.magic = kMagicRollup;
  header.version = kVersion;
  header.header_bytes = sizeof(RollupHeader);
  header.hour_start_epoch_minute = hour_start_epoch_minute;
  header.sensor_count = sensor_count;
  header.descriptor_bytes = descriptor_bytes;
  header.rollup_bytes = rollup_bytes;
  header.payload_crc32 = crc;
  header.record_bytes = sizeof(RollupHeader) + descriptor_bytes + rollup_bytes;

  header.header_crc32 = 0;
  header.header_crc32 = ComputeHeaderCrc32_(&header, sizeof(header));

  if (!WriteAll_(file, &header, sizeof(header))) { file.close(); return false; }

  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!WriteAll_(file, &sensor_node_ids[i], sizeof(uint32_t))) { file.close(); return false; }
    if (!WriteAll_(file, &sensor_rom64[i], sizeof(uint64_t))) { file.close(); return false; }
  }

  if (!WriteAll_(file, rollups, rollup_bytes)) { file.close(); return false; }

  file.flush();
  file.close();

  return VerifyRollupRecord_(path, record_offset);
}

bool SdHistoryStore::EnsureDirExists_(const char* path) {
  if (fs_ == nullptr) return false;
  if (fs_->exists(path)) return true;
  return fs_->mkdir(path);
}

String SdHistoryStore::MakeMinuteFilePath_(uint32_t hour_start_epoch_minute) const {
  const time_t epoch_seconds = static_cast<time_t>(hour_start_epoch_minute) * 60;
  struct tm tm_buf;
  char date_buf[16];
  if (localtime_r(&epoch_seconds, &tm_buf) == nullptr ||
      strftime(date_buf, sizeof(date_buf), "%Y%m%d", &tm_buf) == 0) {
    return String(base_dir_) + "/minute/unknown.bin";
  }
  return String(base_dir_) + "/minute/" + String(date_buf) + ".bin";
}

String SdHistoryStore::MakeHourlyFilePath_(uint32_t hour_start_epoch_minute) const {
  const time_t epoch_seconds = static_cast<time_t>(hour_start_epoch_minute) * 60;
  struct tm tm_buf;
  char month_buf[16];
  if (localtime_r(&epoch_seconds, &tm_buf) == nullptr ||
      strftime(month_buf, sizeof(month_buf), "%Y%m", &tm_buf) == 0) {
    return String(base_dir_) + "/hourly/unknown.bin";
  }
  return String(base_dir_) + "/hourly/" + String(month_buf) + ".bin";
}

bool SdHistoryStore::BuildFinalizedDirPath_(char* out, size_t out_size) const {
  return SdHistoryBuildFinalizedDirPath(base_dir_, out, out_size);
}

bool SdHistoryStore::BuildFinalizedHourFilePath_(
    uint32_t hour_start_epoch_minute, char* out, size_t out_size) const {
  return SdHistoryBuildFinalizedHourFilePath(hour_start_epoch_minute, base_dir_,
                                            out, out_size);
}

bool SdHistoryStore::WriteAll_(File& file, const void* data, size_t length) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  size_t written_total = 0;
  while (written_total < length) {
    const size_t to_write = length - written_total;
    const size_t written = file.write(bytes + written_total, to_write);
    if (written == 0) return false;
    written_total += written;
  }
  return true;
}

uint32_t SdHistoryStore::ComputeHeaderCrc32_(const void* header, size_t header_bytes) {
  // Caller must have header_crc32 already set to 0.
  return Crc32(reinterpret_cast<const uint8_t*>(header), header_bytes);
}


bool SdHistoryStore::VerifyFinalizedHourRecord_(
    const char* path, uint32_t record_offset) const {
  if (path == nullptr || path[0] == '\0') return false;
  File file = fs_->open(path, FILE_READ);
  if (!file) return false;
  if (!file.seek(record_offset)) { file.close(); return false; }

  uint8_t header_bytes[kSdFinalizedHourHeaderBytes];
  if (file.read(header_bytes, sizeof(header_bytes)) !=
      static_cast<int>(sizeof(header_bytes))) {
    file.close();
    return false;
  }

  SdFinalizedHourBlockHeader header;
  if (!DecodeSdFinalizedHourBlockHeader(header_bytes, sizeof(header_bytes), &header) ||
      !VerifySdFinalizedHourBlockHeaderCrc(header_bytes, sizeof(header_bytes))) {
    file.close();
    return false;
  }

  uint32_t payload_crc = 0xFFFFFFFFu;
  uint32_t remaining = header.payload_bytes;
  while (remaining > 0U) {
    const size_t chunk = (remaining < sizeof(g_finalized_verify_buffer))
                             ? static_cast<size_t>(remaining)
                             : sizeof(g_finalized_verify_buffer);
    const int bytes_read = file.read(g_finalized_verify_buffer, chunk);
    if (bytes_read != static_cast<int>(chunk)) {
      file.close();
      return false;
    }
    payload_crc = Crc32Update(payload_crc, g_finalized_verify_buffer, chunk);
    remaining -= static_cast<uint32_t>(chunk);
  }

  file.close();
  return payload_crc == header.payload_crc32;
}

bool SdHistoryStore::VerifyMinuteRecord_(const String& path, uint32_t record_offset) const {
  File file = fs_->open(path.c_str(), FILE_READ);
  if (!file) return false;
  if (!file.seek(record_offset)) { file.close(); return false; }

  MinuteHeader header;
  if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    file.close(); return false;
  }
  if (header.magic != kMagicMinute) { file.close(); return false; }
  if (header.version != kVersion) { file.close(); return false; }
  if (header.header_bytes != sizeof(MinuteHeader)) { file.close(); return false; }

  const uint32_t stored_header_crc = header.header_crc32;
  header.header_crc32 = 0;
  const uint32_t expected_header_crc = ComputeHeaderCrc32_(&header, sizeof(header));
  if (expected_header_crc != stored_header_crc) { file.close(); return false; }
  header.header_crc32 = stored_header_crc;

  // CRC32 over descriptor+payload.
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t remaining = header.descriptor_bytes + header.payload_bytes;

  uint8_t buffer[512];
  while (remaining > 0) {
    const uint32_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
    if (file.read(buffer, chunk) != static_cast<int>(chunk)) { file.close(); return false; }
    crc = Crc32Update(crc, buffer, chunk);
    remaining -= chunk;
  }

  file.close();
  return (crc == header.payload_crc32);
}

bool SdHistoryStore::VerifyRollupRecord_(const String& path, uint32_t record_offset) const {
  File file = fs_->open(path.c_str(), FILE_READ);
  if (!file) return false;
  if (!file.seek(record_offset)) { file.close(); return false; }

  RollupHeader header;
  if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    file.close(); return false;
  }
  if (header.magic != kMagicRollup) { file.close(); return false; }
  if (header.version != kVersion) { file.close(); return false; }
  if (header.header_bytes != sizeof(RollupHeader)) { file.close(); return false; }

  const uint32_t stored_header_crc = header.header_crc32;
  header.header_crc32 = 0;
  const uint32_t expected_header_crc = ComputeHeaderCrc32_(&header, sizeof(header));
  if (expected_header_crc != stored_header_crc) { file.close(); return false; }
  header.header_crc32 = stored_header_crc;

  uint32_t crc = 0xFFFFFFFFu;
  uint32_t remaining = header.descriptor_bytes + header.rollup_bytes;

  uint8_t buffer[512];
  while (remaining > 0) {
    const uint32_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
    if (file.read(buffer, chunk) != static_cast<int>(chunk)) { file.close(); return false; }
    crc = Crc32Update(crc, buffer, chunk);
    remaining -= chunk;
  }

  file.close();
  return (crc == header.payload_crc32);
}

String SdHistoryStore::MakeDailyFilePath_(uint32_t day_start_epoch_minute) const {
  const time_t epoch_seconds = static_cast<time_t>(day_start_epoch_minute) * 60;
  struct tm tm_buf;
  char year_buf[8];
  if (localtime_r(&epoch_seconds, &tm_buf) == nullptr ||
      strftime(year_buf, sizeof(year_buf), "%Y", &tm_buf) == 0) {
    return String(base_dir_) + "/daily/unknown.bin";
  }
  return String(base_dir_) + "/daily/" + String(year_buf) + ".bin";  // yearly rotation
}

bool SdHistoryStore::AppendDailyRollupBlock(uint32_t day_start_epoch_minute,
                                            uint16_t sensor_count,
                                            const uint32_t* sensor_node_ids,
                                            const uint64_t* sensor_rom64,
                                            const HourlyRollupEntry* daily_rollups) {
  if (fs_ == nullptr) return false;
  if (sensor_count == 0 || sensor_count > 100) return false;
  if (sensor_node_ids == nullptr || sensor_rom64 == nullptr || daily_rollups == nullptr) return false;

  const String daily_dir = String(base_dir_) + "/daily";
  if (!EnsureDirExists_(daily_dir.c_str())) return false;

  const String path = MakeDailyFilePath_(day_start_epoch_minute);

  // Precompute CRC32 over descriptor+payload so we never seek-back patch.
  uint32_t crc = 0xFFFFFFFFu;
  for (uint16_t i = 0; i < sensor_count; ++i) {
    crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&sensor_node_ids[i]), sizeof(uint32_t));
    crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&sensor_rom64[i]), sizeof(uint64_t));
  }
  const uint32_t rollup_bytes = static_cast<uint32_t>(sensor_count) * sizeof(HourlyRollupEntry);
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(daily_rollups), rollup_bytes);

  File file = fs_->open(path.c_str(), FILE_APPEND);
  if (!file) return false;

  const uint32_t record_offset = static_cast<uint32_t>(file.position());

  const uint32_t descriptor_bytes =
      static_cast<uint32_t>(sensor_count) * (sizeof(uint32_t) + sizeof(uint64_t));

  DailyHeader header;
  memset(&header, 0, sizeof(header));
  header.magic = kMagicDaily;
  header.version = kVersion;
  header.header_bytes = sizeof(DailyHeader);
  header.day_start_epoch_minute = day_start_epoch_minute;
  header.sensor_count = sensor_count;
  header.descriptor_bytes = descriptor_bytes;
  header.rollup_bytes = rollup_bytes;
  header.payload_crc32 = crc;
  header.record_bytes = sizeof(DailyHeader) + descriptor_bytes + rollup_bytes;

  header.header_crc32 = 0;
  header.header_crc32 = ComputeHeaderCrc32_(&header, sizeof(header));

  if (!WriteAll_(file, &header, sizeof(header))) { file.close(); return false; }

  for (uint16_t i = 0; i < sensor_count; ++i) {
    if (!WriteAll_(file, &sensor_node_ids[i], sizeof(uint32_t))) { file.close(); return false; }
    if (!WriteAll_(file, &sensor_rom64[i], sizeof(uint64_t))) { file.close(); return false; }
  }

  if (!WriteAll_(file, daily_rollups, rollup_bytes)) { file.close(); return false; }

  file.flush();
  file.close();

  return VerifyDailyRecord_(path, record_offset);
}

bool SdHistoryStore::VerifyDailyRecord_(const String& path, uint32_t record_offset) const {
  File file = fs_->open(path.c_str(), FILE_READ);
  if (!file) return false;
  if (!file.seek(record_offset)) { file.close(); return false; }

  DailyHeader header;
  if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    file.close(); return false;
  }
  if (header.magic != kMagicDaily) { file.close(); return false; }
  if (header.version != kVersion) { file.close(); return false; }
  if (header.header_bytes != sizeof(DailyHeader)) { file.close(); return false; }

  const uint32_t stored_header_crc = header.header_crc32;
  header.header_crc32 = 0;
  const uint32_t expected_header_crc = ComputeHeaderCrc32_(&header, sizeof(header));
  if (expected_header_crc != stored_header_crc) { file.close(); return false; }

  uint32_t crc = 0xFFFFFFFFu;
  uint32_t remaining = header.descriptor_bytes + header.rollup_bytes;

  uint8_t buffer[512];
  while (remaining > 0) {
    const uint32_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
    if (file.read(buffer, chunk) != static_cast<int>(chunk)) { file.close(); return false; }
    crc = Crc32Update(crc, buffer, chunk);
    remaining -= chunk;
  }

  file.close();
  return (crc == header.payload_crc32);
}

bool SdHistoryStore::ScanHourlyRollups(
    uint32_t start_epoch_minute,
    uint32_t end_epoch_minute,
    const std::function<void(uint32_t hour_start_epoch_minute,
                             uint16_t sensor_count,
                             const uint32_t* sensor_node_ids,
                             const uint64_t* sensor_rom64,
                             const HourlyRollupEntry* rollups)>& on_record) const {
  if (fs_ == nullptr) return false;
  if (start_epoch_minute >= end_epoch_minute) return true;

  auto scan_one_file = [&](const String& path) -> bool {
    File file = fs_->open(path.c_str(), FILE_READ);
    if (!file) return true;  // no data is not a failure

    while (true) {
      const uint32_t record_offset = static_cast<uint32_t>(file.position());

      RollupHeader header;
      const int read_bytes =
          file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
      if (read_bytes == 0) break;  // EOF
      if (read_bytes != static_cast<int>(sizeof(header))) break;

      if (header.magic != kMagicRollup ||
          header.version != kVersion ||
          header.header_bytes != sizeof(RollupHeader)) {
        break;  // treat as tail corruption
      }

      const uint32_t stored_header_crc = header.header_crc32;
      header.header_crc32 = 0;
      const uint32_t expected_header_crc = ComputeHeaderCrc32_(&header, sizeof(header));
      header.header_crc32 = stored_header_crc;
      if (expected_header_crc != stored_header_crc) {
        break;  // tail corruption
      }

      const uint16_t sensor_count = header.sensor_count;
      if (sensor_count == 0 || sensor_count > 100) break;

      const uint32_t expected_desc_bytes =
          static_cast<uint32_t>(sensor_count) * (sizeof(uint32_t) + sizeof(uint64_t));
      const uint32_t expected_rollup_bytes =
          static_cast<uint32_t>(sensor_count) * sizeof(HourlyRollupEntry);

      if (header.descriptor_bytes != expected_desc_bytes) break;
      if (header.rollup_bytes != expected_rollup_bytes) break;

      // Quick stop if records are chronological and we're past the range.
      if (header.hour_start_epoch_minute >= end_epoch_minute) {
        break;
      }

      std::vector<uint32_t> node_ids(sensor_count);
      std::vector<uint64_t> rom64(sensor_count);
      std::vector<HourlyRollupEntry> rollups(sensor_count);

      uint32_t crc = 0xFFFFFFFFu;

      for (uint16_t i = 0; i < sensor_count; ++i) {
        if (file.read(reinterpret_cast<uint8_t*>(&node_ids[i]), sizeof(uint32_t)) !=
            static_cast<int>(sizeof(uint32_t))) {
          file.close();
          return true;
        }
        crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&node_ids[i]), sizeof(uint32_t));

        if (file.read(reinterpret_cast<uint8_t*>(&rom64[i]), sizeof(uint64_t)) !=
            static_cast<int>(sizeof(uint64_t))) {
          file.close();
          return true;
        }
        crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&rom64[i]), sizeof(uint64_t));
      }

      if (file.read(reinterpret_cast<uint8_t*>(rollups.data()), header.rollup_bytes) !=
          static_cast<int>(header.rollup_bytes)) {
        file.close();
        return true;
      }
      crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(rollups.data()), header.rollup_bytes);

      const bool payload_ok = (crc == header.payload_crc32);

      if (payload_ok &&
          header.hour_start_epoch_minute >= start_epoch_minute &&
          header.hour_start_epoch_minute < end_epoch_minute) {
        on_record(header.hour_start_epoch_minute,
                  sensor_count,
                  node_ids.data(),
                  rom64.data(),
                  rollups.data());
      }

      // Seek using record_bytes to tolerate future extensions.
      const uint32_t next_offset = record_offset + header.record_bytes;
      if (!file.seek(next_offset)) break;
    }

    file.close();
    return true;
  };

  // Scan month file for start, and month file for end-1 (covers month boundary).
  const String start_path = MakeHourlyFilePath_(start_epoch_minute);
  if (!scan_one_file(start_path)) return false;

  const String end_path = MakeHourlyFilePath_(end_epoch_minute - 1);
  if (end_path != start_path) {
    if (!scan_one_file(end_path)) return false;
  }
  return true;
}
