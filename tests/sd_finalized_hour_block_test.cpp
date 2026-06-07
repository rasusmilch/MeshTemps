#include "history_hour_stager.h"
#include "sd_finalized_hour_block.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

constexpr uint32_t kHour = 30000000U;
constexpr uint32_t kNodeA = 0x10000001U;
constexpr uint32_t kNodeB = 0x20000002U;

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8U);
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4U; ++i) {
    value |= static_cast<uint32_t>(bytes[offset + i]) << (8U * i);
  }
  return value;
}

uint64_t ReadU64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < 8U; ++i) {
    value |= static_cast<uint64_t>(bytes[offset + i]) << (8U * i);
  }
  return value;
}

int16_t ReadI16(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<int16_t>(ReadU16(bytes, offset));
}


struct TestVectorSinkContext {
  std::vector<uint8_t>* bytes = nullptr;
};

bool TestVectorSink(const uint8_t* data, size_t len, void* ctx) {
  TestVectorSinkContext* sink = static_cast<TestVectorSinkContext*>(ctx);
  if (sink == nullptr || sink->bytes == nullptr || data == nullptr) return false;
  sink->bytes->insert(sink->bytes->end(), data, data + len);
  return true;
}

struct FailingSinkContext {
  size_t fail_after_bytes = 0;
  size_t bytes_seen = 0;
};

bool FailingSink(const uint8_t*, size_t len, void* ctx) {
  FailingSinkContext* sink = static_cast<FailingSinkContext*>(ctx);
  if (sink == nullptr) return false;
  if (sink->bytes_seen + len > sink->fail_after_bytes) {
    return false;
  }
  sink->bytes_seen += len;
  return true;
}

HistoryHourSnapshot BuildTwoSensorSnapshot() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));
  assert(stager.RecordSample("2800000000000060", kNodeA, 0, 12.34f, true));
  assert(stager.RecordSample("2800000000000061", kNodeB, 20, 21.25f, false));
  assert(stager.RecordSample("2800000000000061", kNodeB, 21, 21.50f, true));

  HistoryHourSnapshot snapshot;
  assert(stager.ExportSnapshot(&snapshot));
  return snapshot;
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

void TestEncodeValidSnapshot() {
  const HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();
  std::vector<uint8_t> record;
  assert(EncodeSdFinalizedHourBlock(snapshot, &record));

  SdFinalizedHourBlockHeader header;
  assert(VerifySdFinalizedHourBlock(record, &header));
  assert(header.magic == kSdFinalizedHourMagic);
  assert(header.version == kSdFinalizedHourVersion);
  assert(header.hour_start_epoch_minute == kHour);
  assert(header.active_slot_count == 2);
  assert(header.descriptor_bytes == 2U * kSdFinalizedHourDescriptorBytes);
  assert(header.frame_count == kHistoryMinutesPerHour);
  assert(header.frame_bytes == kSdFinalizedHourFrameBytes);
  assert(header.payload_bytes == header.descriptor_bytes +
                                     kHistoryMinutesPerHour * kSdFinalizedHourFrameBytes);
  assert(record.size() == header.record_bytes);
}

void TestDescriptorAndFramePayload() {
  std::vector<uint8_t> record;
  assert(EncodeSdFinalizedHourBlock(BuildTwoSensorSnapshot(), &record));

  const size_t desc0 = DescriptorOffset(0);
  assert(record[desc0 + 0] == 0);
  assert(record[desc0 + 1] == 0);
  assert(record[desc0 + 2] == 0);
  assert(record[desc0 + 3] == 1);
  assert(ReadU32(record, desc0 + 4) == kNodeA);
  assert(ReadU64(record, desc0 + 8) == 0x2800000000000060ULL);
  assert(std::memcmp(record.data() + desc0 + 16, "2800000000000060", 16) == 0);

  const size_t desc1 = DescriptorOffset(1);
  assert(record[desc1 + 0] == 1);
  assert(record[desc1 + 1] == 20);
  assert(record[desc1 + 2] == 21);
  assert(record[desc1 + 3] == 1);
  assert(ReadU32(record, desc1 + 4) == kNodeB);
  assert(ReadU64(record, desc1 + 8) == 0x2800000000000061ULL);

  const size_t frame0 = FrameOffset(0);
  assert((record[frame0] & 0x01U) != 0);       // slot 0 present
  assert((record[frame0 + kHistoryBitmapBytes] & 0x01U) != 0);  // slot 0 corrected
  assert(ReadI16(record, TempOffset(0, 0)) == 1234);

  const size_t frame19 = FrameOffset(19);
  assert((record[frame19] & 0x02U) == 0);  // slot 1 absent before discovery

  const size_t frame20 = FrameOffset(20);
  assert((record[frame20] & 0x02U) != 0);  // slot 1 present
  assert((record[frame20 + kHistoryBitmapBytes] & 0x02U) == 0);  // not corrected
  assert(ReadI16(record, TempOffset(20, 1)) == 2125);

  const size_t frame21 = FrameOffset(21);
  assert((record[frame21] & 0x02U) != 0);
  assert((record[frame21 + kHistoryBitmapBytes] & 0x02U) != 0);
  assert(ReadI16(record, TempOffset(21, 1)) == 2150);
}

void TestCountersAreDiagnosticOnly() {
  HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();
  std::vector<uint8_t> baseline;
  assert(EncodeSdFinalizedHourBlock(snapshot, &baseline));

  snapshot.status.samples_recorded = 9999;
  snapshot.status.missing_samples_recorded = 7777;
  snapshot.slots[0].sample_count = 5555;
  snapshot.slots[1].missing_or_invalid_count = 4444;

  std::vector<uint8_t> changed_counters;
  assert(EncodeSdFinalizedHourBlock(snapshot, &changed_counters));
  assert(baseline == changed_counters);
}

void TestCorruptionDetection() {
  std::vector<uint8_t> record;
  assert(EncodeSdFinalizedHourBlock(BuildTwoSensorSnapshot(), &record));

  std::vector<uint8_t> bad_header = record;
  bad_header[12] ^= 0x01U;  // hour_start_epoch_minute participates in header CRC.
  assert(!VerifySdFinalizedHourBlock(bad_header));

  std::vector<uint8_t> bad_payload = record;
  bad_payload.back() ^= 0x01U;
  assert(!VerifySdFinalizedHourBlock(bad_payload));

  std::vector<uint8_t> truncated = record;
  truncated.resize(truncated.size() - 1U);
  assert(!VerifySdFinalizedHourBlock(truncated));
}


void TestStreamingWriterMatchesVectorEncode() {
  const HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();

  std::vector<uint8_t> vector_encoded;
  assert(EncodeSdFinalizedHourBlock(snapshot, &vector_encoded));

  std::vector<uint8_t> streamed;
  TestVectorSinkContext sink{&streamed};
  SdFinalizedHourBlockHeader header;
  SdFinalizedHourWriteStatus status;
  assert(WriteSdFinalizedHourBlock(snapshot, TestVectorSink, &sink, &header, &status));

  assert(streamed == vector_encoded);
  assert(status.bytes_written == vector_encoded.size());
  assert(status.payload_bytes == header.payload_bytes);
  assert(status.payload_crc32 == header.payload_crc32);
  assert(status.header_crc32 == header.header_crc32);
  assert(VerifySdFinalizedHourBlockHeaderCrc(streamed.data(),
                                            kSdFinalizedHourHeaderBytes));
}

void TestStreamingWriterPropagatesWriteFailure() {
  const HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();
  FailingSinkContext sink;
  sink.fail_after_bytes = kSdFinalizedHourHeaderBytes - 1U;

  assert(!WriteSdFinalizedHourBlock(snapshot, FailingSink, &sink, nullptr, nullptr));
}

void TestInvalidSnapshotsRejected() {
  std::vector<uint8_t> record;
  HistoryHourSnapshot snapshot = BuildTwoSensorSnapshot();

  snapshot.status.hour_active = false;
  assert(!EncodeSdFinalizedHourBlock(snapshot, &record));
  assert(record.empty());

  snapshot = BuildTwoSensorSnapshot();
  snapshot.hour_start_epoch_minute = 0;
  assert(!EncodeSdFinalizedHourBlock(snapshot, &record));
  assert(record.empty());

  snapshot = BuildTwoSensorSnapshot();
  snapshot.active_slot_count = kHistorySlotCapacity + 1U;
  snapshot.status.active_slot_count = snapshot.active_slot_count;
  assert(!EncodeSdFinalizedHourBlock(snapshot, &record));
  assert(record.empty());
}

}  // namespace

int main() {
  TestEncodeValidSnapshot();
  TestDescriptorAndFramePayload();
  TestCountersAreDiagnosticOnly();
  TestStreamingWriterMatchesVectorEncode();
  TestStreamingWriterPropagatesWriteFailure();
  TestCorruptionDetection();
  TestInvalidSnapshotsRejected();
  std::cout << "sd_finalized_hour_block_test: PASS" << std::endl;
  return 0;
}
