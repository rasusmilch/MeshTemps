#include "sd_history_store.h"

#include "sd_finalized_hour_v2_format.h"
#include "sd_finalized_hour_v2_scanner.h"
#include "sd_finalized_hour_v2_writer.h"
#include "sd_finalized_hour_write_coalescer.h"

#include "history_crc.h"

namespace {

constexpr size_t kFinalizedHourWriteCoalescerBufferBytes = 4096U;
constexpr size_t kFinalizedVerifyBufferBytes = 512U;
constexpr uint32_t kFinalizedHourV2MaxRecordBytes =
    kSdFinalizedHourV2HeaderBytes +
    (kHistorySlotCapacity * kSdFinalizedHourV2IndexEntryBytes) +
    (kHistorySlotCapacity * kSdFinalizedHourV2FixedBlockBytes);

// Shared finalized-hour SD I/O buffers and writer workspace. The owning
// storage/runtime service must serialize finalized-hour append/verify calls:
// this path is single-writer and non-reentrant, and must not be called
// concurrently or from LVGL callbacks.
alignas(4) static uint8_t
    g_finalized_hour_write_buffer[kFinalizedHourWriteCoalescerBufferBytes];
alignas(4) static uint8_t g_finalized_verify_buffer[kFinalizedVerifyBufferBytes];
alignas(4) static char g_finalized_hour_v2_preamble_buffer
    [kSdFinalizedHourV2PreambleMaxBytes];
static SdFinalizedHourV2WriterWorkspace g_finalized_hour_v2_writer_workspace;


class FinalizedHourFileByteReader final : public ISdFinalizedHourV2ByteReader {
 public:
  explicit FinalizedHourFileByteReader(File* file) : file_(file) {}

  bool Read(uint64_t offset,
            uint8_t* out,
            size_t len,
            size_t* bytes_read) override {
    if (bytes_read != nullptr) *bytes_read = 0;
    if (file_ == nullptr || !(*file_)) return false;
    if (len == 0U) return true;
    if (out == nullptr || offset > UINT32_MAX) return false;
    if (!file_->seek(static_cast<uint32_t>(offset))) return false;
    const int got = file_->read(out, len);
    if (got < 0) return false;
    if (bytes_read != nullptr) *bytes_read = static_cast<size_t>(got);
    return true;
  }

 private:
  File* file_ = nullptr;
};

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

bool WriteFinalizedHourV2Preamble(File* file) {
  if (file == nullptr) return false;
  const size_t preamble_bytes = BuildSdFinalizedHourV2Preamble(
      g_finalized_hour_v2_preamble_buffer,
      sizeof(g_finalized_hour_v2_preamble_buffer));
  if (preamble_bytes == 0U ||
      preamble_bytes > sizeof(g_finalized_hour_v2_preamble_buffer)) {
    return false;
  }
  const size_t written = file->write(
      reinterpret_cast<const uint8_t*>(g_finalized_hour_v2_preamble_buffer),
      preamble_bytes);
  return written == preamble_bytes;
}

bool FinalizedHourFileContainsV2Marker(File* file) {
  if (file == nullptr) return false;
  if (!file->seek(0)) return false;

  const char* marker = kSdFinalizedHourV2BinaryStartMarker;
  const size_t marker_len = sizeof(kSdFinalizedHourV2BinaryStartMarker) - 1U;
  size_t matched = 0;
  size_t scanned = 0;
  while (scanned < kSdFinalizedHourV2PreambleMaxBytes) {
    const size_t remaining = kSdFinalizedHourV2PreambleMaxBytes - scanned;
    const size_t chunk = (remaining < sizeof(g_finalized_verify_buffer))
                             ? remaining
                             : sizeof(g_finalized_verify_buffer);
    const int bytes_read = file->read(g_finalized_verify_buffer, chunk);
    if (bytes_read <= 0) return false;
    for (int i = 0; i < bytes_read; ++i) {
      const char ch = static_cast<char>(g_finalized_verify_buffer[i]);
      if (ch == marker[matched]) {
        ++matched;
        if (matched == marker_len) return true;
      } else {
        matched = (ch == marker[0]) ? 1U : 0U;
      }
    }
    scanned += static_cast<size_t>(bytes_read);
  }
  return false;
}

bool ValidateFinalizedHourV2Header(const SdFinalizedHourV2Header& header) {
  if (header.record_magic != kSdFinalizedHourV2RecordMagic ||
      header.record_version != kSdFinalizedHourV2RecordVersion ||
      header.header_bytes != kSdFinalizedHourV2HeaderBytes ||
      header.index_entry_bytes != kSdFinalizedHourV2IndexEntryBytes ||
      header.sensor_count == 0U ||
      header.sensor_count > kHistorySlotCapacity) {
    return false;
  }

  const uint32_t expected_index_bytes =
      static_cast<uint32_t>(header.sensor_count) *
      kSdFinalizedHourV2IndexEntryBytes;
  const uint32_t expected_sensor_blocks_bytes =
      static_cast<uint32_t>(header.sensor_count) *
      kSdFinalizedHourV2FixedBlockBytes;
  const uint32_t expected_record_bytes =
      kSdFinalizedHourV2HeaderBytes + expected_index_bytes +
      expected_sensor_blocks_bytes;
  return header.index_offset == kSdFinalizedHourV2HeaderBytes &&
         header.index_bytes == expected_index_bytes &&
         header.sensor_blocks_offset ==
             kSdFinalizedHourV2HeaderBytes + expected_index_bytes &&
         header.sensor_blocks_bytes == expected_sensor_blocks_bytes &&
         header.record_bytes == expected_record_bytes &&
         header.record_bytes <= kFinalizedHourV2MaxRecordBytes;
}

}  // namespace

bool SdHistoryStore::Begin(fs::FS& fs, const char* base_dir) {
  if (!SdHistoryCopyBaseDir(base_dir, base_dir_, sizeof(base_dir_))) return false;
  fs_ = &fs;

  char finalized_dir[kSdHistoryPathMax];
  if (!BuildFinalizedDirPath_(finalized_dir, sizeof(finalized_dir))) return false;
  return EnsureDirExists_(finalized_dir);
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

  if (file.position() != 0U && !FinalizedHourFileHasV2Marker_(path)) {
    file.close();
    return false;
  }

  if (file.position() == 0U) {
    if (!WriteFinalizedHourV2Preamble(&file)) {
      file.close();
      return false;
    }
  }

  const uint32_t record_offset = static_cast<uint32_t>(file.position());
  FinalizedHourFileWriteContext write_ctx;
  write_ctx.file = &file;
  SdFinalizedHourWriteCoalescer coalescer(WriteFinalizedHourFileChunk,
                                          &write_ctx,
                                          g_finalized_hour_write_buffer,
                                          sizeof(g_finalized_hour_write_buffer));

  SdFinalizedHourV2WriteStatus status;
  // Production finalized-hour v2 writes are bounded: the pure writer streams
  // header, index entries, and sensor blocks into the fixed-size coalescing
  // sink. The coalescer is flushed before File.flush()/close(), and read-back
  // verification starts only after the complete record has been flushed and
  // closed. Labels are not yet available at this layer, so v2 records use empty
  // bounded label snapshots.
  if (!WriteSdFinalizedHourV2Record(snapshot, nullptr,
                                    g_finalized_hour_v2_writer_workspace,
                                    SdFinalizedHourCoalescerWriteFn,
                                    &coalescer, &status)) {
    file.close();
    return false;
  }
  if (status.skipped_zero_sensor_hour) {
    file.flush();
    file.close();
    return true;
  }
  if (status.bytes_written != status.record_bytes ||
      coalescer.logical_bytes() != status.record_bytes) {
    file.close();
    return false;
  }
  if (!coalescer.Flush() || coalescer.failed() ||
      coalescer.physical_bytes() != status.record_bytes) {
    file.close();
    return false;
  }

  file.flush();
  file.close();
  return VerifyFinalizedHourRecord_(path, record_offset);
}


bool SdHistoryStore::ScanFinalizedHourFile(
    uint32_t hour_start_epoch_minute,
    SdFinalizedHourV2ScannerWorkspace& workspace,
    SdFinalizedHourV2ScanResult* out_result) const {
  if (out_result == nullptr) return false;
  *out_result = SdFinalizedHourV2ScanResult{};
  if (fs_ == nullptr) return false;

  char path[kSdHistoryPathMax];
  if (!BuildFinalizedHourFilePath_(hour_start_epoch_minute, path, sizeof(path))) {
    return false;
  }

  File file = fs_->open(path, FILE_READ);
  if (!file) return false;

  FinalizedHourFileByteReader reader(&file);
  *out_result = ScanSdFinalizedHourV2DayFile(reader, workspace);
  file.close();
  return true;
}

bool SdHistoryStore::EnsureDirExists_(const char* path) {
  if (fs_ == nullptr) return false;
  if (fs_->exists(path)) return true;
  return fs_->mkdir(path);
}

bool SdHistoryStore::BuildFinalizedDirPath_(char* out, size_t out_size) const {
  return SdHistoryBuildFinalizedDirPath(base_dir_, out, out_size);
}

bool SdHistoryStore::BuildFinalizedHourFilePath_(
    uint32_t hour_start_epoch_minute, char* out, size_t out_size) const {
  return SdHistoryBuildFinalizedHourFilePath(hour_start_epoch_minute, base_dir_,
                                            out, out_size);
}

bool SdHistoryStore::FinalizedHourFileHasV2Marker_(const char* path) const {
  if (path == nullptr || path[0] == '\0' || fs_ == nullptr) return false;
  File file = fs_->open(path, FILE_READ);
  if (!file) return false;
  const bool has_marker = FinalizedHourFileContainsV2Marker(&file);
  file.close();
  return has_marker;
}

bool SdHistoryStore::VerifyFinalizedHourRecord_(
    const char* path, uint32_t record_offset) const {
  if (path == nullptr || path[0] == '\0') return false;
  File file = fs_->open(path, FILE_READ);
  if (!file) return false;
  if (!file.seek(record_offset)) { file.close(); return false; }

  uint8_t header_bytes[kSdFinalizedHourV2HeaderBytes];
  if (file.read(header_bytes, sizeof(header_bytes)) !=
      static_cast<int>(sizeof(header_bytes))) {
    file.close();
    return false;
  }

  SdFinalizedHourV2Header header;
  if (!DecodeSdFinalizedHourV2Header(header_bytes, sizeof(header_bytes), &header) ||
      ComputeSdFinalizedHourV2HeaderCrc32(header_bytes, sizeof(header_bytes)) !=
          header.header_crc32 ||
      !ValidateFinalizedHourV2Header(header)) {
    file.close();
    return false;
  }

  uint32_t payload_crc = Crc32IsoHdlcBegin();
  uint32_t remaining = header.record_bytes - header.header_bytes;
  while (remaining > 0U) {
    const size_t chunk = (remaining < sizeof(g_finalized_verify_buffer))
                             ? static_cast<size_t>(remaining)
                             : sizeof(g_finalized_verify_buffer);
    const int bytes_read = file.read(g_finalized_verify_buffer, chunk);
    if (bytes_read != static_cast<int>(chunk)) {
      file.close();
      return false;
    }
    payload_crc = Crc32IsoHdlcUpdate(payload_crc, g_finalized_verify_buffer, chunk);
    remaining -= static_cast<uint32_t>(chunk);
  }

  file.close();
  return Crc32IsoHdlcFinalize(payload_crc) == header.payload_crc32;
}
