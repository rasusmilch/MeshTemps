#ifndef SD_FINALIZED_HOUR_BLOCK_H_
#define SD_FINALIZED_HOUR_BLOCK_H_

#include <cstddef>
#include <cstdint>

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
constexpr size_t kSdFinalizedHourMaxRecordBytes =
    kSdFinalizedHourHeaderBytes +
    (kHistorySlotCapacity * kSdFinalizedHourDescriptorBytes) +
    (kHistoryMinutesPerHour * kSdFinalizedHourFrameBytes);

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

class ISdFinalizedHourSource {
 public:
  virtual ~ISdFinalizedHourSource() = default;

  virtual uint32_t hour_start_epoch_minute() const = 0;
  virtual uint16_t active_slot_count() const = 0;
  virtual bool hour_active() const = 0;
  virtual uint16_t format_version() const = 0;
  virtual HistoryStagerStatus status() const = 0;
  virtual const HistorySlotDescriptor* slot(uint8_t slot_id) const = 0;
  virtual const HistoryMinuteFrame* frame(uint8_t minute_index) const = 0;
};

class HistoryHourSnapshotFinalizedHourSource final : public ISdFinalizedHourSource {
 public:
  explicit HistoryHourSnapshotFinalizedHourSource(const HistoryHourSnapshot& snapshot)
      : snapshot_(snapshot) {}

  uint32_t hour_start_epoch_minute() const override {
    return snapshot_.hour_start_epoch_minute;
  }
  uint16_t active_slot_count() const override { return snapshot_.active_slot_count; }
  bool hour_active() const override { return snapshot_.status.hour_active; }
  uint16_t format_version() const override { return snapshot_.format_version; }
  HistoryStagerStatus status() const override { return snapshot_.status; }
  const HistorySlotDescriptor* slot(uint8_t slot_id) const override {
    return (slot_id < kHistorySlotCapacity) ? &snapshot_.slots[slot_id] : nullptr;
  }
  const HistoryMinuteFrame* frame(uint8_t minute_index) const override {
    return (minute_index < kHistoryMinutesPerHour) ? &snapshot_.frames[minute_index]
                                                   : nullptr;
  }

 private:
  const HistoryHourSnapshot& snapshot_;
};

class RamHourStagerFinalizedHourSource final : public ISdFinalizedHourSource {
 public:
  explicit RamHourStagerFinalizedHourSource(const RamHourStager& stager)
      : stager_(stager) {}

  uint32_t hour_start_epoch_minute() const override {
    return stager_.status().hour_start_epoch_minute;
  }
  uint16_t active_slot_count() const override {
    return stager_.status().active_slot_count;
  }
  bool hour_active() const override { return stager_.status().hour_active; }
  uint16_t format_version() const override { return kHistoryHourSnapshotFormatVersion; }
  HistoryStagerStatus status() const override { return stager_.status(); }
  const HistorySlotDescriptor* slot(uint8_t slot_id) const override {
    return stager_.slot(slot_id);
  }
  const HistoryMinuteFrame* frame(uint8_t minute_index) const override {
    return stager_.frame(minute_index);
  }

 private:
  const RamHourStager& stager_;
};

// Production writer path: streams the finalized-hour record to write_fn in
// bounded header/descriptor/frame chunks and never builds a full-record buffer.
// The source/view overload reads descriptors and frames by index without owning
// or materializing a HistoryHourSnapshot.
bool WriteSdFinalizedHourBlock(const ISdFinalizedHourSource& source,
                               SdFinalizedHourWriteFn write_fn,
                               void* ctx,
                               SdFinalizedHourBlockHeader* out_header = nullptr,
                               SdFinalizedHourWriteStatus* out_status = nullptr);

// Compatibility/test adapter for existing logical snapshots. This wrapper does
// not copy or allocate the snapshot; runtime finalization should prefer the
// source/view overload.
bool WriteSdFinalizedHourBlock(const HistoryHourSnapshot& snapshot,
                               SdFinalizedHourWriteFn write_fn,
                               void* ctx,
                               SdFinalizedHourBlockHeader* out_header = nullptr,
                               SdFinalizedHourWriteStatus* out_status = nullptr);

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

#endif  // SD_FINALIZED_HOUR_BLOCK_H_
