#ifndef SD_FINALIZED_HOUR_BLOCK_H_
#define SD_FINALIZED_HOUR_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "history_hour_stager.h"

// Self-describing raw finalized-hour record for SD archive storage.
//
// HistoryHourSnapshot is the logical in-memory export shape. This helper writes
// an explicit little-endian byte format instead of treating the snapshot structs
// as a packed disk ABI. Raw 60-minute frames are authoritative; any diagnostic
// counters in the snapshot remain metadata only and are not used to decide which
// sample cells are valid. Presence bits are the validity source.

constexpr uint32_t kSdFinalizedHourMagic = 0x5248544Du;  // 'MTHR' little-endian
constexpr uint16_t kSdFinalizedHourVersion = 1;
constexpr uint16_t kSdFinalizedHourHeaderBytes = 48;
constexpr uint16_t kSdFinalizedHourDescriptorBytes = 32;
constexpr uint16_t kSdFinalizedHourFrameBytes =
    static_cast<uint16_t>((2U * kHistoryBitmapBytes) +
                          (kHistorySlotCapacity * sizeof(int16_t)));

struct SdFinalizedHourBlockHeader {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t header_bytes = 0;
  uint32_t record_bytes = 0;
  uint32_t hour_start_epoch_minute = 0;
  uint16_t active_slot_count = 0;
  uint16_t descriptor_entry_bytes = 0;
  uint16_t frame_count = 0;
  uint16_t frame_bytes = 0;
  uint32_t descriptor_bytes = 0;
  uint32_t payload_bytes = 0;
  uint32_t payload_crc32 = 0;
  uint32_t header_crc32 = 0;
  uint16_t snapshot_format_version = 0;
  uint16_t reserved0 = 0;
  uint32_t flags = 0;
};

typedef bool (*SdFinalizedHourWriteFn)(const uint8_t* data, size_t len, void* ctx);

struct SdFinalizedHourWriteStatus {
  uint32_t bytes_written = 0;
  uint32_t payload_bytes = 0;
  uint32_t payload_crc32 = 0;
  uint32_t header_crc32 = 0;
};

// Production writer path: streams the finalized-hour record to write_fn in
// bounded header/descriptor/frame chunks and never builds a full-record buffer.
bool WriteSdFinalizedHourBlock(const HistoryHourSnapshot& snapshot,
                               SdFinalizedHourWriteFn write_fn,
                               void* ctx,
                               SdFinalizedHourBlockHeader* out_header = nullptr,
                               SdFinalizedHourWriteStatus* out_status = nullptr);

// Test/convenience helper only. Production SD finalization must use
// WriteSdFinalizedHourBlock() or another bounded sink, not this vector-backed
// full-record encoder.
bool EncodeSdFinalizedHourBlock(const HistoryHourSnapshot& snapshot,
                                std::vector<uint8_t>* out_record);

bool DecodeSdFinalizedHourBlockHeader(const uint8_t* record,
                                      size_t record_length,
                                      SdFinalizedHourBlockHeader* out_header);

uint32_t ComputeSdFinalizedHourHeaderCrc32(const uint8_t* header_bytes,
                                           size_t header_length);

bool VerifySdFinalizedHourBlockHeaderCrc(const uint8_t* header_bytes,
                                         size_t header_length);

bool VerifySdFinalizedHourBlock(const uint8_t* record,
                                size_t record_length,
                                SdFinalizedHourBlockHeader* out_header = nullptr);

bool VerifySdFinalizedHourBlock(const std::vector<uint8_t>& record,
                                SdFinalizedHourBlockHeader* out_header = nullptr);

#endif  // SD_FINALIZED_HOUR_BLOCK_H_
