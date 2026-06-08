#include "sd_history_store.h"

#include "sd_finalized_hour_block.h"
#include "sd_finalized_hour_write_coalescer.h"


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
