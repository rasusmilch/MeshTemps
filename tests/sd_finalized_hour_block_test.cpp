#include "history_hour_stager.h"
#include "sd_finalized_hour_block.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

constexpr uint32_t kHour = 30000000U;
constexpr uint32_t kNodeA = 0x10000001U;
constexpr uint32_t kNodeB = 0x20000002U;

struct CaptureBuffer {
  uint8_t bytes[kSdFinalizedHourMaxRecordBytes] = {};
  size_t used = 0;
  bool overflow = false;
};

struct LimitedCaptureBuffer {
  uint8_t bytes[kSdFinalizedHourHeaderBytes] = {};
  size_t used = 0;
  bool overflow = false;
};

struct FailingSinkContext {
  size_t fail_after_bytes = 0;
  size_t bytes_seen = 0;
};

uint16_t ReadU16(const uint8_t* bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8U);
}

uint32_t ReadU32(const uint8_t* bytes, size_t offset) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4U; ++i) {
    value |= static_cast<uint32_t>(bytes[offset + i]) << (8U * i);
  }
  return value;
}

uint64_t ReadU64(const uint8_t* bytes, size_t offset) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < 8U; ++i) {
    value |= static_cast<uint64_t>(bytes[offset + i]) << (8U * i);
  }
  return value;
}

int16_t ReadI16(const uint8_t* bytes, size_t offset) {
  return static_cast<int16_t>(ReadU16(bytes, offset));
}

bool CaptureSink(const uint8_t* data, size_t len, void* ctx) {
  CaptureBuffer* capture = static_cast<CaptureBuffer*>(ctx);
  if (capture == nullptr || data == nullptr) return false;
  if (len > (sizeof(capture->bytes) - capture->used)) {
    capture->overflow = true;
    return false;
  }
  std::memcpy(capture->bytes + capture->used, data, len);
  capture->used += len;
  return true;
}

bool LimitedCaptureSink(const uint8_t* data, size_t len, void* ctx) {
  LimitedCaptureBuffer* capture = static_cast<LimitedCaptureBuffer*>(ctx);
  if (capture == nullptr || data == nullptr) return false;
  if (len > (sizeof(capture->bytes) - capture->used)) {
    capture->overflow = true;
    return false;
  }
  std::memcpy(capture->bytes + capture->used, data, len);
  capture->used += len;
  return true;
}

bool FailingSink(const uint8_t*, size_t len, void* ctx) {
  FailingSinkContext* sink = static_cast<FailingSinkContext*>(ctx);
  if (sink == nullptr) return false;
  if (sink->bytes_seen + len > sink->fail_after_bytes) {
    return false;
  }
  sink->bytes_seen += len;
  return true;
}

RamHourStager BuildTwoSensorStager() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));
  assert(stager.RecordSample("2800000000000060", kNodeA, 0, 12.34f, true));
  assert(stager.RecordSample("2800000000000061", kNodeB, 20, 21.25f, false));
  assert(stager.RecordSample("2800000000000061", kNodeB, 21, 21.50f, true));
  return stager;
}

HistoryHourSnapshot BuildTwoSensorSnapshot() {
  RamHourStager stager = BuildTwoSensorStager();
  HistoryHourSnapshot snapshot;
  assert(stager.ExportSnapshot(&snapshot));
  return snapshot;
}

bool EncodeToCapture(const HistoryHourSnapshot& snapshot,
                     CaptureBuffer* capture,
                     SdFinalizedHourBlockHeader* header = nullptr,
                     SdFinalizedHourWriteStatus* status = nullptr) {
  if (capture == nullptr) return false;
  capture->used = 0;
  capture->overflow = false;
  std::memset(capture->bytes, 0, sizeof(capture->bytes));
  return WriteSdFinalizedHourBlock(snapshot, CaptureSink, capture, header, status);
}

bool EncodeSourceToCapture(const ISdFinalizedHourSource& source,
                           CaptureBuffer* capture,
                           SdFinalizedHourBlockHeader* header = nullptr,
                           SdFinalizedHourWriteStatus* status = nullptr) {
  if (capture == nullptr) return false;
  capture->used = 0;
  capture->overflow = false;
  std::memset(capture->bytes, 0, sizeof(capture->bytes));
  return WriteSdFinalizedHourBlock(source, CaptureSink, capture, header, status);
}

size_t DescriptorOffset(uint8_t slot_id) {
  return kSdFinalizedHourHeaderBytes +
         static_cast<size_t>(slot_id) * kSdFinalizedHourDescriptorBytes;
}

size_t FrameOffset(uint8_t minute_index) {
  return kSdFinalizedHourHeaderBytes +
         (2U * kSdFinalizedHourDescriptorBytes) +
         static_cast<size_t>(minute_index) * kSdFinalizedHourFrameBytes;
}

size_t TempOffset(uint8_t minute_index, uint8_t slot_id) {
  return FrameOffset(minute_index) + (2U * kHistoryBitmapBytes) +
         static_cast<size_t>(slot_id) * sizeof(int16_t);
}

class FakeFinalizedHourSource final : public ISdFinalizedHourSource {
 public:
  FakeFinalizedHourSource() {
    RamHourStager stager = BuildTwoSensorStager();
    assert(stager.ExportSnapshot(&snapshot_));
  }

  uint32_t hour_start_epoch_minute() const override {
    return override_hour_start_ ? 0U : snapshot_.hour_start_epoch_minute;
  }
  uint16_t active_slot_count() const override {
    return override_slot_count_ ? static_cast<uint16_t>(kHistorySlotCapacity + 1U)
                                : snapshot_.active_slot_count;
  }
  bool hour_active() const override {
    return override_inactive_ ? false : snapshot_.status.hour_active;
  }
  uint16_t format_version() const override {
    return override_bad_format_ ? static_cast<uint16_t>(0xFFFFU)
                                : snapshot_.format_version;
  }
  HistoryStagerStatus status() const override {
    HistoryStagerStatus status = snapshot_.status;
    if (override_inactive_) status.hour_active = false;
    if (override_hour_start_) status.hour_start_epoch_minute = 0U;
    if (override_slot_count_) status.active_slot_count = kHistorySlotCapacity + 1U;
    if (mismatch_status_hour_start_) {
      status.hour_start_epoch_minute = snapshot_.hour_start_epoch_minute + 1U;
    }
    if (mismatch_status_active_slot_count_) {
      status.active_slot_count =
          (snapshot_.active_slot_count > 0U) ? snapshot_.active_slot_count - 1U : 1U;
    }
    return status;
  }
  const HistorySlotDescriptor* slot(uint8_t slot_id) const override {
    if (return_null_slot_ && slot_id == 0U) return nullptr;
    if (slot_id >= kHistorySlotCapacity) return nullptr;
    temp_slot_ = snapshot_.slots[slot_id];
    if (slot_id == 0U) {
      if (slot_id_mismatch_) temp_slot_.slot_id = 1U;
      if (zero_rom_) temp_slot_.rom64 = 0U;
      if (bad_addr_) temp_slot_.addr16[0] = 'Z';
    }
    return &temp_slot_;
  }
  const HistoryMinuteFrame* frame(uint8_t minute_index) const override {
    if (return_null_frame_ && minute_index == 0U) return nullptr;
    return (minute_index < kHistoryMinutesPerHour) ? &snapshot_.frames[minute_index]
                                                   : nullptr;
  }

  bool override_inactive_ = false;
  bool override_hour_start_ = false;
  bool override_slot_count_ = false;
  bool mismatch_status_hour_start_ = false;
  bool mismatch_status_active_slot_count_ = false;
  bool override_bad_format_ = false;
  bool return_null_slot_ = false;
  bool return_null_frame_ = false;
  bool slot_id_mismatch_ = false;
  bool zero_rom_ = false;
  bool bad_addr_ = false;

 private:
  HistoryHourSnapshot snapshot_{};
  mutable HistorySlotDescriptor temp_slot_{};
};

void TestEncodeValidSnapshot() {
  const HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();
  CaptureBuffer record;
  assert(EncodeToCapture(snapshot, &record));

  SdFinalizedHourBlockHeader header;
  assert(VerifySdFinalizedHourBlock(record.bytes, record.used, &header));
  assert(header.magic == kSdFinalizedHourMagic);
  assert(header.version == kSdFinalizedHourVersion);
  assert(header.hour_start_epoch_minute == kHour);
  assert(header.active_slot_count == 2);
  assert(header.descriptor_bytes == 2U * kSdFinalizedHourDescriptorBytes);
  assert(header.frame_count == kHistoryMinutesPerHour);
  assert(header.frame_bytes == kSdFinalizedHourFrameBytes);
  assert(header.payload_bytes == header.descriptor_bytes +
                                     kHistoryMinutesPerHour * kSdFinalizedHourFrameBytes);
  assert(record.used == header.record_bytes);
  assert(record.used <= kSdFinalizedHourMaxRecordBytes);
}

void TestDescriptorAndFramePayload() {
  CaptureBuffer record;
  assert(EncodeToCapture(BuildTwoSensorSnapshot(), &record));

  const size_t desc0 = DescriptorOffset(0);
  assert(record.bytes[desc0 + 0] == 0);
  assert(record.bytes[desc0 + 1] == 0);
  assert(record.bytes[desc0 + 2] == 0);
  assert(record.bytes[desc0 + 3] == 1);
  assert(ReadU32(record.bytes, desc0 + 4) == kNodeA);
  assert(ReadU64(record.bytes, desc0 + 8) == 0x2800000000000060ULL);
  assert(std::memcmp(record.bytes + desc0 + 16, "2800000000000060", 16) == 0);

  const size_t desc1 = DescriptorOffset(1);
  assert(record.bytes[desc1 + 0] == 1);
  assert(record.bytes[desc1 + 1] == 20);
  assert(record.bytes[desc1 + 2] == 21);
  assert(record.bytes[desc1 + 3] == 1);
  assert(ReadU32(record.bytes, desc1 + 4) == kNodeB);
  assert(ReadU64(record.bytes, desc1 + 8) == 0x2800000000000061ULL);

  const size_t frame0 = FrameOffset(0);
  assert((record.bytes[frame0] & 0x01U) != 0);       // slot 0 present
  assert((record.bytes[frame0 + kHistoryBitmapBytes] & 0x01U) != 0);  // slot 0 corrected
  assert(ReadI16(record.bytes, TempOffset(0, 0)) == 1234);

  const size_t frame19 = FrameOffset(19);
  assert((record.bytes[frame19] & 0x02U) == 0);  // slot 1 absent before discovery

  const size_t frame20 = FrameOffset(20);
  assert((record.bytes[frame20] & 0x02U) != 0);  // slot 1 present
  assert((record.bytes[frame20 + kHistoryBitmapBytes] & 0x02U) == 0);  // not corrected
  assert(ReadI16(record.bytes, TempOffset(20, 1)) == 2125);

  const size_t frame21 = FrameOffset(21);
  assert((record.bytes[frame21] & 0x02U) != 0);
  assert((record.bytes[frame21 + kHistoryBitmapBytes] & 0x02U) != 0);
  assert(ReadI16(record.bytes, TempOffset(21, 1)) == 2150);
}

void TestCountersAreDiagnosticOnly() {
  HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();
  CaptureBuffer baseline;
  assert(EncodeToCapture(snapshot, &baseline));

  snapshot.status.samples_recorded = 9999;
  snapshot.status.missing_samples_recorded = 7777;
  snapshot.slots[0].sample_count = 5555;
  snapshot.slots[1].missing_or_invalid_count = 4444;

  CaptureBuffer changed_counters;
  assert(EncodeToCapture(snapshot, &changed_counters));
  assert(baseline.used == changed_counters.used);
  assert(std::memcmp(baseline.bytes, changed_counters.bytes, baseline.used) == 0);
}

void TestCorruptionDetection() {
  CaptureBuffer record;
  assert(EncodeToCapture(BuildTwoSensorSnapshot(), &record));

  uint8_t bad_header[kSdFinalizedHourMaxRecordBytes];
  std::memcpy(bad_header, record.bytes, record.used);
  bad_header[12] ^= 0x01U;  // hour_start_epoch_minute participates in header CRC.
  assert(!VerifySdFinalizedHourBlock(bad_header, record.used));

  uint8_t bad_payload[kSdFinalizedHourMaxRecordBytes];
  std::memcpy(bad_payload, record.bytes, record.used);
  bad_payload[record.used - 1U] ^= 0x01U;
  assert(!VerifySdFinalizedHourBlock(bad_payload, record.used));

  assert(!VerifySdFinalizedHourBlock(record.bytes, record.used - 1U));
}

void TestStreamingWriterStatusAndHeaderCrc() {
  const HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();
  CaptureBuffer record;
  SdFinalizedHourBlockHeader header;
  SdFinalizedHourWriteStatus status;
  assert(EncodeToCapture(snapshot, &record, &header, &status));

  assert(status.bytes_written == record.used);
  assert(status.payload_bytes == header.payload_bytes);
  assert(status.payload_crc32 == header.payload_crc32);
  assert(status.header_crc32 == header.header_crc32);
  assert(VerifySdFinalizedHourBlockHeaderCrc(record.bytes,
                                            kSdFinalizedHourHeaderBytes));
}

void TestStreamingWriterPropagatesWriteFailure() {
  const HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();
  FailingSinkContext sink;
  sink.fail_after_bytes = kSdFinalizedHourHeaderBytes - 1U;

  assert(!WriteSdFinalizedHourBlock(snapshot, FailingSink, &sink, nullptr, nullptr));
}

void TestFixedCaptureOverflowFails() {
  const HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();
  LimitedCaptureBuffer capture;
  assert(!WriteSdFinalizedHourBlock(snapshot, LimitedCaptureSink, &capture,
                                    nullptr, nullptr));
  assert(capture.overflow);
}

void TestInvalidSnapshotsRejected() {
  CaptureBuffer record;
  HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();

  snapshot.status.hour_active = false;
  assert(!EncodeToCapture(snapshot, &record));
  assert(record.used == 0);

  snapshot = BuildTwoSensorSnapshot();
  snapshot.hour_start_epoch_minute = 0;
  assert(!EncodeToCapture(snapshot, &record));
  assert(record.used == 0);

  snapshot = BuildTwoSensorSnapshot();
  snapshot.active_slot_count = kHistorySlotCapacity + 1U;
  snapshot.status.active_slot_count = snapshot.active_slot_count;
  assert(!EncodeToCapture(snapshot, &record));
  assert(record.used == 0);
}

void TestRamHourStagerSourceMatchesSnapshotBytes() {
  RamHourStager stager = BuildTwoSensorStager();

  HistoryHourSnapshot snapshot;
  assert(stager.ExportSnapshot(&snapshot));

  CaptureBuffer snapshot_record;
  assert(EncodeToCapture(snapshot, &snapshot_record));

  const RamHourStagerFinalizedHourSource source(stager);
  CaptureBuffer source_record;
  SdFinalizedHourBlockHeader header;
  SdFinalizedHourWriteStatus status;
  assert(EncodeSourceToCapture(source, &source_record, &header, &status));

  assert(source_record.used == snapshot_record.used);
  assert(std::memcmp(source_record.bytes, snapshot_record.bytes, source_record.used) == 0);
  assert(VerifySdFinalizedHourBlock(source_record.bytes, source_record.used));
  assert(status.bytes_written == source_record.used);
  assert(header.record_bytes == source_record.used);
}

void TestInvalidSourcesRejected() {
  CaptureBuffer record;

  FakeFinalizedHourSource source;
  source.override_inactive_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.override_hour_start_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.override_slot_count_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.mismatch_status_hour_start_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.mismatch_status_active_slot_count_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.override_bad_format_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.return_null_slot_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.return_null_frame_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.slot_id_mismatch_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.zero_rom_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);

  source = FakeFinalizedHourSource();
  source.bad_addr_ = true;
  assert(!EncodeSourceToCapture(source, &record));
  assert(record.used == 0);
}

void TestSourceWriterPropagatesWriteFailure() {
  const RamHourStager stager = BuildTwoSensorStager();
  const RamHourStagerFinalizedHourSource source(stager);
  FailingSinkContext sink;
  sink.fail_after_bytes = kSdFinalizedHourHeaderBytes - 1U;

  assert(!WriteSdFinalizedHourBlock(source, FailingSink, &sink, nullptr, nullptr));
}

}  // namespace

int main() {
  TestEncodeValidSnapshot();
  TestDescriptorAndFramePayload();
  TestCountersAreDiagnosticOnly();
  TestStreamingWriterStatusAndHeaderCrc();
  TestStreamingWriterPropagatesWriteFailure();
  TestFixedCaptureOverflowFails();
  TestCorruptionDetection();
  TestInvalidSnapshotsRejected();
  TestRamHourStagerSourceMatchesSnapshotBytes();
  TestInvalidSourcesRejected();
  TestSourceWriterPropagatesWriteFailure();
  std::cout << "sd_finalized_hour_block_test: PASS" << std::endl;
  return 0;
}
