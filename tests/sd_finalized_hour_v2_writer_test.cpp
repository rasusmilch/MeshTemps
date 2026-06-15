#include "sd_finalized_hour_v2_writer.h"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

struct TestHarness {
  const char* current_test = nullptr;
  uint32_t failures = 0;
};

TestHarness g_test;
static_assert(sizeof(SdFinalizedHourV2WriterWorkspace) > 1024U,
              "SdFinalizedHourV2WriterWorkspace is intentionally not stack-small; "
              "production callers must not allocate it on task/callback stack.");
static SdFinalizedHourV2WriterWorkspace g_writer_workspace;

bool CheckTrue(bool condition, const char* expr, const char* file, int line) {
  if (condition) return true;
  std::cerr << file << ":" << line << " in " << g_test.current_test
            << ": CHECK_TRUE failed: " << expr << std::endl;
  ++g_test.failures;
  return false;
}

template <typename A, typename E>
bool CheckEq(const A& actual, const E& expected, const char* a, const char* e,
             const char* file, int line) {
  if (actual == expected) return true;
  std::cerr << file << ":" << line << " in " << g_test.current_test
            << ": CHECK_EQ failed: " << a << " != " << e << std::endl;
  ++g_test.failures;
  return false;
}

#define CHECK_TRUE(expr) CheckTrue(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) CheckEq((actual), (expected), #actual, #expected, __FILE__, __LINE__)

constexpr uint32_t kHour = 28928160U;
constexpr size_t kCaptureBytes = 20000U;

struct CaptureSink {
  uint8_t bytes[kCaptureBytes] = {};
  size_t used = 0;
  size_t fail_after = kCaptureBytes + 1U;
  uint32_t calls = 0;
};

bool CaptureWrite(const uint8_t* data, size_t len, void* ctx) {
  CaptureSink* sink = static_cast<CaptureSink*>(ctx);
  if (sink == nullptr || (data == nullptr && len != 0U)) return false;
  ++sink->calls;
  if (sink->used + len > sink->fail_after || sink->used + len > sizeof(sink->bytes)) {
    return false;
  }
  if (len != 0U) {
    std::memcpy(sink->bytes + sink->used, data, len);
    sink->used += len;
  }
  return true;
}

size_t CopyLiteral(const char* text,
                   uint8_t* out,
                   size_t out_size,
                   bool* truncated) {
  if (truncated != nullptr) *truncated = false;
  if (text == nullptr || out == nullptr) return 0U;
  const size_t source_len = std::strlen(text);
  const size_t copy_len = (source_len < out_size) ? source_len : out_size;
  if (copy_len != 0U) std::memcpy(out, text, copy_len);
  if (truncated != nullptr) *truncated = source_len > out_size;
  return copy_len;
}

class TestLabels final : public ISdFinalizedHourV2LabelSource {
 public:
  const char* node_label = nullptr;
  const char* sensor_label = nullptr;

  bool CopyNodeLabel(uint32_t node_id,
                     uint8_t* out,
                     size_t out_size,
                     size_t* out_len,
                     bool* out_truncated) const override {
    (void)node_id;
    if (out_len == nullptr || out_truncated == nullptr) return false;
    *out_len = CopyLiteral(node_label, out, out_size, out_truncated);
    return true;
  }

  bool CopySensorLabel(uint64_t rom64,
                       uint32_t node_id,
                       uint8_t* out,
                       size_t out_size,
                       size_t* out_len,
                       bool* out_truncated) const override {
    (void)rom64;
    (void)node_id;
    if (out_len == nullptr || out_truncated == nullptr) return false;
    *out_len = CopyLiteral(sensor_label, out, out_size, out_truncated);
    return true;
  }
};

class ChangingLabels final : public ISdFinalizedHourV2LabelSource {
 public:
  mutable uint32_t node_calls = 0;
  mutable uint32_t sensor_calls = 0;

  bool CopyNodeLabel(uint32_t node_id,
                     uint8_t* out,
                     size_t out_size,
                     size_t* out_len,
                     bool* out_truncated) const override {
    (void)node_id;
    if (out_len == nullptr || out_truncated == nullptr) return false;
    ++node_calls;
    *out_len = CopyLiteral((node_calls == 1U) ? "Node First" : "Node Later",
                           out, out_size, out_truncated);
    return true;
  }

  bool CopySensorLabel(uint64_t rom64,
                       uint32_t node_id,
                       uint8_t* out,
                       size_t out_size,
                       size_t* out_len,
                       bool* out_truncated) const override {
    (void)rom64;
    (void)node_id;
    if (out_len == nullptr || out_truncated == nullptr) return false;
    ++sensor_calls;
    *out_len = CopyLiteral((sensor_calls == 1U) ? "Sensor First" : "Sensor Later",
                           out, out_size, out_truncated);
    return true;
  }
};

class FailingLabels final : public ISdFinalizedHourV2LabelSource {
 public:
  bool fail_node = true;
  mutable uint32_t node_calls = 0;
  mutable uint32_t sensor_calls = 0;

  bool CopyNodeLabel(uint32_t node_id,
                     uint8_t* out,
                     size_t out_size,
                     size_t* out_len,
                     bool* out_truncated) const override {
    (void)node_id;
    (void)out;
    (void)out_size;
    if (out_len == nullptr || out_truncated == nullptr) return false;
    ++node_calls;
    *out_len = 0;
    *out_truncated = false;
    return !fail_node;
  }

  bool CopySensorLabel(uint64_t rom64,
                       uint32_t node_id,
                       uint8_t* out,
                       size_t out_size,
                       size_t* out_len,
                       bool* out_truncated) const override {
    (void)rom64;
    (void)node_id;
    if (out_len == nullptr || out_truncated == nullptr) return false;
    ++sensor_calls;
    *out_len = CopyLiteral("Sensor OK", out, out_size, out_truncated);
    return fail_node;
  }
};

bool BuildSnapshot(HistoryHourSnapshot* out) {
  if (out == nullptr) return false;
  RamHourStager stager;
  bool ok = stager.ResetHour(kHour);
  ok = stager.RecordSampleCentiC("2800000000000060", 0x10000001U, 5U,
                                 2134, false) && ok;
  ok = stager.ExportSnapshot(out) && ok;
  return ok;
}

bool BuildCorrectedMissingSnapshot(HistoryHourSnapshot* out) {
  if (out == nullptr) return false;
  RamHourStager stager;
  bool ok = stager.ResetHour(kHour);
  ok = stager.RecordSampleCentiC("2800000000000060", 0x10000001U, 10U,
                                 2000, false) && ok;
  ok = stager.RecordSampleCentiC("2800000000000060", 0x10000001U, 11U,
                                 2010, true) && ok;
  ok = stager.RecordMissing("2800000000000060", 0x10000001U, 12U) && ok;
  ok = stager.ExportSnapshot(out) && ok;
  return ok;
}

bool WriteSnapshot(const HistoryHourSnapshot& snapshot,
                   const ISdFinalizedHourV2LabelSource* labels,
                   CaptureSink* sink,
                   SdFinalizedHourV2WriteStatus* status) {
  g_writer_workspace = SdFinalizedHourV2WriterWorkspace{};
  return WriteSdFinalizedHourV2Record(snapshot, labels, g_writer_workspace,
                                      CaptureWrite, sink, status);
}

const uint8_t* BlockAt(const CaptureSink& sink, uint16_t index, uint16_t sensor_count) {
  const size_t offset = kSdFinalizedHourV2HeaderBytes +
                        static_cast<size_t>(sensor_count) *
                            kSdFinalizedHourV2IndexEntryBytes +
                        static_cast<size_t>(index) *
                            kSdFinalizedHourV2FixedBlockBytes;
  return (offset + kSdFinalizedHourV2FixedBlockBytes <= sink.used)
             ? sink.bytes + offset
             : nullptr;
}

bool DecodeFirstRecord(const CaptureSink& sink,
                       SdFinalizedHourV2Header* header,
                       SdFinalizedHourV2IndexEntry* index,
                       SdFinalizedHourV2BlockHeader* block_header,
                       SdFinalizedHourV2Descriptor* descriptor,
                       SdFinalizedHourV2Payload* payload) {
  if (header == nullptr || index == nullptr || block_header == nullptr ||
      descriptor == nullptr || payload == nullptr) {
    return false;
  }
  if (!DecodeSdFinalizedHourV2Header(sink.bytes, sink.used, header)) return false;
  if (!DecodeSdFinalizedHourV2IndexEntry(
          sink.bytes + header->index_offset, sink.used - header->index_offset,
          index)) {
    return false;
  }
  const uint8_t* block = sink.bytes + index->sensor_block_offset_from_record_start;
  return DecodeSdFinalizedHourV2BlockHeader(
             block, kSdFinalizedHourV2FixedBlockBytes, block_header) &&
         DecodeSdFinalizedHourV2Descriptor(
             block + kSdFinalizedHourV2BlockHeaderBytes,
             kSdFinalizedHourV2DescriptorBytes, descriptor) &&
         DecodeSdFinalizedHourV2Payload(
             block + kSdFinalizedHourV2BlockHeaderBytes +
                 kSdFinalizedHourV2DescriptorBytes,
             kSdFinalizedHourV2PayloadBytes, payload);
}

bool RecordCrcsValidate(const CaptureSink& sink) {
  SdFinalizedHourV2Header header;
  if (!DecodeSdFinalizedHourV2Header(sink.bytes, sink.used, &header)) return false;
  if (ComputeSdFinalizedHourV2HeaderCrc32(
          sink.bytes, kSdFinalizedHourV2HeaderBytes) != header.header_crc32) {
    return false;
  }
  if (ComputeSdFinalizedHourV2PayloadCrc32(
          sink.bytes + kSdFinalizedHourV2HeaderBytes,
          sink.used - kSdFinalizedHourV2HeaderBytes) != header.payload_crc32) {
    return false;
  }
  for (uint16_t i = 0; i < header.sensor_count; ++i) {
    const uint8_t* block = BlockAt(sink, i, header.sensor_count);
    if (block == nullptr) return false;
    SdFinalizedHourV2BlockHeader block_header;
    if (!DecodeSdFinalizedHourV2BlockHeader(
            block, kSdFinalizedHourV2FixedBlockBytes, &block_header)) {
      return false;
    }
    if (ComputeSdFinalizedHourV2BlockCrc32(
            block, kSdFinalizedHourV2FixedBlockBytes) != block_header.block_crc32) {
      return false;
    }
  }
  return true;
}

void TestOneSensorBasicStructure() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(WriteSnapshot(snapshot, nullptr, &sink, &status));
  CHECK_TRUE(!status.skipped_zero_sensor_hour);
  CHECK_EQ(status.sensor_count, static_cast<uint16_t>(1));
  CHECK_EQ(status.record_bytes,
           static_cast<uint32_t>(kSdFinalizedHourV2HeaderBytes +
                                 kSdFinalizedHourV2IndexEntryBytes +
                                 kSdFinalizedHourV2FixedBlockBytes));
  CHECK_EQ(sink.used, static_cast<size_t>(status.record_bytes));

  SdFinalizedHourV2Header header;
  SdFinalizedHourV2IndexEntry index;
  SdFinalizedHourV2BlockHeader block_header;
  SdFinalizedHourV2Descriptor descriptor;
  SdFinalizedHourV2Payload payload;
  CHECK_TRUE(DecodeFirstRecord(sink, &header, &index, &block_header,
                               &descriptor, &payload));
  CHECK_EQ(header.record_magic, kSdFinalizedHourV2RecordMagic);
  CHECK_EQ(header.record_version, kSdFinalizedHourV2RecordVersion);
  CHECK_EQ(header.header_bytes, kSdFinalizedHourV2HeaderBytes);
  CHECK_EQ(header.sensor_count, static_cast<uint16_t>(1));
  CHECK_EQ(header.index_offset, static_cast<uint32_t>(kSdFinalizedHourV2HeaderBytes));
  CHECK_EQ(header.index_bytes,
           static_cast<uint32_t>(kSdFinalizedHourV2IndexEntryBytes));
  CHECK_EQ(header.sensor_blocks_offset,
           static_cast<uint32_t>(kSdFinalizedHourV2HeaderBytes +
                                 kSdFinalizedHourV2IndexEntryBytes));
  CHECK_EQ(header.sensor_blocks_bytes,
           static_cast<uint32_t>(kSdFinalizedHourV2FixedBlockBytes));
  CHECK_EQ(index.rom64, 0x2800000000000060ULL);
  CHECK_EQ(index.sensor_block_offset_from_record_start,
           header.sensor_blocks_offset);
  CHECK_EQ(block_header.block_magic, kSdFinalizedHourV2BlockMagic);
  CHECK_EQ(block_header.block_bytes,
           static_cast<uint32_t>(kSdFinalizedHourV2FixedBlockBytes));
  CHECK_EQ(block_header.descriptor_bytes, kSdFinalizedHourV2DescriptorBytes);
  CHECK_EQ(block_header.payload_bytes, kSdFinalizedHourV2PayloadBytes);
}

void TestMappingPresenceMissingCorrectedCounts() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildCorrectedMissingSnapshot(&snapshot));
  snapshot.frames[13].corrected[0] = 1U;
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(WriteSnapshot(snapshot, nullptr, &sink, &status));
  CHECK_EQ(status.corrected_without_presence_sanitized, static_cast<uint32_t>(1));

  SdFinalizedHourV2Header header;
  SdFinalizedHourV2IndexEntry index;
  SdFinalizedHourV2BlockHeader block_header;
  SdFinalizedHourV2Descriptor descriptor;
  SdFinalizedHourV2Payload payload;
  CHECK_TRUE(DecodeFirstRecord(sink, &header, &index, &block_header,
                               &descriptor, &payload));
  (void)header;
  (void)index;
  (void)block_header;
  CHECK_EQ(descriptor.last_known_node_id, 0x10000001U);
  CHECK_EQ(descriptor.first_seen_minute, static_cast<uint8_t>(10));
  CHECK_EQ(descriptor.last_seen_minute, static_cast<uint8_t>(12));
  CHECK_EQ(descriptor.valid_sample_count, static_cast<uint16_t>(2));
  CHECK_EQ(descriptor.missing_or_invalid_count, static_cast<uint16_t>(1));
  CHECK_EQ(descriptor.corrected_sample_count, static_cast<uint16_t>(1));
  CHECK_TRUE((payload.presence_bitmap[10U / 8U] & (1U << (10U % 8U))) != 0U);
  CHECK_TRUE((payload.presence_bitmap[11U / 8U] & (1U << (11U % 8U))) != 0U);
  CHECK_TRUE((payload.presence_bitmap[12U / 8U] & (1U << (12U % 8U))) == 0U);
  CHECK_TRUE((payload.corrected_bitmap[11U / 8U] & (1U << (11U % 8U))) != 0U);
  CHECK_TRUE((payload.corrected_bitmap[13U / 8U] & (1U << (13U % 8U))) == 0U);
  CHECK_EQ(payload.samples[10], static_cast<int16_t>(2000));
  CHECK_EQ(payload.samples[11], static_cast<int16_t>(2010));
  CHECK_EQ(payload.samples[12], kSdFinalizedHourV2InvalidTempCentiC);
  CHECK_EQ(payload.samples[59], kSdFinalizedHourV2InvalidTempCentiC);
}

void TestLabelsCopiedMissingAndTruncated() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  TestLabels labels;
  labels.node_label = "Node A";
  labels.sensor_label = "Room Sensor";
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(WriteSnapshot(snapshot, &labels, &sink, &status));

  SdFinalizedHourV2Header header;
  SdFinalizedHourV2IndexEntry index;
  SdFinalizedHourV2BlockHeader block_header;
  SdFinalizedHourV2Descriptor descriptor;
  SdFinalizedHourV2Payload payload;
  CHECK_TRUE(DecodeFirstRecord(sink, &header, &index, &block_header,
                               &descriptor, &payload));
  (void)header;
  (void)index;
  (void)block_header;
  (void)payload;
  CHECK_EQ(descriptor.node_label_len, static_cast<uint8_t>(6));
  CHECK_EQ(descriptor.sensor_label_len, static_cast<uint8_t>(11));
  CHECK_TRUE(std::memcmp(descriptor.node_label, "Node A", 6) == 0);
  CHECK_TRUE(std::memcmp(descriptor.sensor_label, "Room Sensor", 11) == 0);
  CHECK_EQ(descriptor.descriptor_flags, 0U);

  labels.node_label = nullptr;
  labels.sensor_label = nullptr;
  sink = CaptureSink{};
  CHECK_TRUE(WriteSnapshot(snapshot, &labels, &sink, &status));
  CHECK_TRUE(DecodeFirstRecord(sink, &header, &index, &block_header,
                               &descriptor, &payload));
  CHECK_EQ(descriptor.node_label_len, static_cast<uint8_t>(0));
  CHECK_EQ(descriptor.sensor_label_len, static_cast<uint8_t>(0));

  labels.node_label = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghi";
  labels.sensor_label = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  sink = CaptureSink{};
  CHECK_TRUE(WriteSnapshot(snapshot, &labels, &sink, &status));
  CHECK_TRUE(DecodeFirstRecord(sink, &header, &index, &block_header,
                               &descriptor, &payload));
  CHECK_EQ(descriptor.node_label_len, kSdFinalizedHourV2NodeLabelMaxBytes);
  CHECK_EQ(descriptor.sensor_label_len, kSdFinalizedHourV2SensorLabelMaxBytes);
  CHECK_TRUE((descriptor.descriptor_flags &
              kSdFinalizedHourV2DescriptorFlagNodeLabelTruncated) != 0U);
  CHECK_TRUE((descriptor.descriptor_flags &
              kSdFinalizedHourV2DescriptorFlagSensorLabelTruncated) != 0U);
}

void TestOrderingAndIdentity() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  snapshot.active_slot_count = 3;
  snapshot.status.active_slot_count = 3;
  snapshot.slots[1] = snapshot.slots[0];
  snapshot.slots[1].slot_id = 1;
  snapshot.slots[1].rom64 = 0x2800000000000010ULL;
  snapshot.slots[1].last_known_node_id = 2;
  snapshot.frames[2].SetSample(1, 1800, false);
  ++snapshot.slots[1].sample_count;
  snapshot.slots[1].first_seen_minute = 2;
  snapshot.slots[1].last_seen_minute = 2;
  snapshot.slots[2] = snapshot.slots[0];
  snapshot.slots[2].slot_id = 2;
  snapshot.slots[2].rom64 = 0x28000000000000A0ULL;
  snapshot.slots[2].last_known_node_id = 3;
  snapshot.frames[3].SetSample(2, 1900, false);
  ++snapshot.slots[2].sample_count;
  snapshot.slots[2].first_seen_minute = 3;
  snapshot.slots[2].last_seen_minute = 3;

  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(WriteSnapshot(snapshot, nullptr, &sink, &status));
  CHECK_EQ(status.sensor_count, static_cast<uint16_t>(3));
  SdFinalizedHourV2IndexEntry e0;
  SdFinalizedHourV2IndexEntry e1;
  SdFinalizedHourV2IndexEntry e2;
  CHECK_TRUE(DecodeSdFinalizedHourV2IndexEntry(
      sink.bytes + kSdFinalizedHourV2HeaderBytes,
      kSdFinalizedHourV2IndexEntryBytes, &e0));
  CHECK_TRUE(DecodeSdFinalizedHourV2IndexEntry(
      sink.bytes + kSdFinalizedHourV2HeaderBytes + kSdFinalizedHourV2IndexEntryBytes,
      kSdFinalizedHourV2IndexEntryBytes, &e1));
  CHECK_TRUE(DecodeSdFinalizedHourV2IndexEntry(
      sink.bytes + kSdFinalizedHourV2HeaderBytes +
          (2U * kSdFinalizedHourV2IndexEntryBytes),
      kSdFinalizedHourV2IndexEntryBytes, &e2));
  CHECK_EQ(e0.rom64, 0x2800000000000010ULL);
  CHECK_EQ(e1.rom64, 0x2800000000000060ULL);
  CHECK_EQ(e2.rom64, 0x28000000000000A0ULL);
}

void TestDuplicateRom64RejectedBeforeWriting() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  snapshot.active_slot_count = 2;
  snapshot.status.active_slot_count = 2;
  snapshot.slots[1] = snapshot.slots[0];
  snapshot.slots[1].slot_id = 1;
  snapshot.slots[1].last_known_node_id = 0x20000002U;
  snapshot.frames[6].SetSample(1, 2200, false);
  ++snapshot.slots[1].sample_count;
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(!WriteSnapshot(snapshot, nullptr, &sink, &status));
  CHECK_EQ(status.failure_reason,
           SdFinalizedHourV2WriteFailureReason::kDuplicateRom64);
  CHECK_EQ(sink.used, static_cast<size_t>(0));
}

void TestZeroSensorSkippedWithoutSink() {
  HistoryHourSnapshot snapshot;
  snapshot.format_version = kHistoryHourSnapshotFormatVersion;
  snapshot.hour_start_epoch_minute = kHour;
  snapshot.status.hour_active = true;
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(WriteSnapshot(snapshot, nullptr, &sink, &status));
  CHECK_TRUE(status.skipped_zero_sensor_hour);
  CHECK_EQ(status.bytes_written, static_cast<uint32_t>(0));
  CHECK_EQ(sink.calls, static_cast<uint32_t>(0));
}

void TestCrcsValidateAndMutationsChangeCrcs() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(WriteSnapshot(snapshot, nullptr, &sink, &status));
  SdFinalizedHourV2Header header;
  CHECK_TRUE(DecodeSdFinalizedHourV2Header(sink.bytes, sink.used, &header));
  CHECK_EQ(ComputeSdFinalizedHourV2HeaderCrc32(
               sink.bytes, kSdFinalizedHourV2HeaderBytes),
           header.header_crc32);
  CHECK_EQ(ComputeSdFinalizedHourV2PayloadCrc32(
               sink.bytes + kSdFinalizedHourV2HeaderBytes,
               sink.used - kSdFinalizedHourV2HeaderBytes),
           header.payload_crc32);
  const uint8_t* block = BlockAt(sink, 0, 1);
  CHECK_TRUE(block != nullptr);
  SdFinalizedHourV2BlockHeader block_header;
  CHECK_TRUE(DecodeSdFinalizedHourV2BlockHeader(
      block, kSdFinalizedHourV2FixedBlockBytes, &block_header));
  CHECK_EQ(ComputeSdFinalizedHourV2BlockCrc32(
               block, kSdFinalizedHourV2FixedBlockBytes),
           block_header.block_crc32);

  uint8_t mutated_header[kSdFinalizedHourV2HeaderBytes] = {};
  std::memcpy(mutated_header, sink.bytes, sizeof(mutated_header));
  mutated_header[12] ^= 0x01U;
  CHECK_TRUE(ComputeSdFinalizedHourV2HeaderCrc32(
                 mutated_header, sizeof(mutated_header)) != header.header_crc32);
  std::memcpy(mutated_header, sink.bytes, sizeof(mutated_header));
  mutated_header[40] ^= 0x01U;
  CHECK_EQ(ComputeSdFinalizedHourV2HeaderCrc32(
               mutated_header, sizeof(mutated_header)),
           header.header_crc32);

  uint8_t mutated_block[kSdFinalizedHourV2FixedBlockBytes] = {};
  std::memcpy(mutated_block, block, sizeof(mutated_block));
  mutated_block[kSdFinalizedHourV2BlockHeaderBytes + 14U] ^= 0x01U;
  CHECK_TRUE(ComputeSdFinalizedHourV2BlockCrc32(
                 mutated_block, sizeof(mutated_block)) !=
             block_header.block_crc32);
  CHECK_TRUE(ComputeSdFinalizedHourV2PayloadCrc32(
                 sink.bytes + kSdFinalizedHourV2HeaderBytes,
                 sink.used - kSdFinalizedHourV2HeaderBytes) ==
             header.payload_crc32);
  sink.bytes[kSdFinalizedHourV2HeaderBytes +
             kSdFinalizedHourV2IndexEntryBytes +
             kSdFinalizedHourV2BlockHeaderBytes + 14U] ^= 0x01U;
  CHECK_TRUE(ComputeSdFinalizedHourV2PayloadCrc32(
                 sink.bytes + kSdFinalizedHourV2HeaderBytes,
                 sink.used - kSdFinalizedHourV2HeaderBytes) !=
             header.payload_crc32);
}

void TestSinkFailures() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  SdFinalizedHourV2WriteStatus status;
  g_writer_workspace = SdFinalizedHourV2WriterWorkspace{};
  CHECK_TRUE(!WriteSdFinalizedHourV2Record(snapshot, nullptr, g_writer_workspace,
                                           nullptr, nullptr, &status));
  CHECK_EQ(status.failure_reason,
           SdFinalizedHourV2WriteFailureReason::kSinkFailure);
  CaptureSink sink;
  sink.fail_after = 10U;
  CHECK_TRUE(!WriteSnapshot(snapshot, nullptr, &sink, &status));
  CHECK_EQ(status.failure_reason,
           SdFinalizedHourV2WriteFailureReason::kSinkFailure);
  CHECK_TRUE(status.bytes_written != status.record_bytes);
}

void TestSchemaHasNoDurableSlotIdOrAddr16() {
  size_t count = 0;
  const SdFinalizedHourV2FieldSpec* tables[] = {
      SdFinalizedHourV2HeaderFields(&count),
      SdFinalizedHourV2IndexEntryFields(&count),
      SdFinalizedHourV2BlockHeaderFields(&count),
      SdFinalizedHourV2DescriptorFields(&count),
      SdFinalizedHourV2PayloadFields(&count),
  };
  (void)tables;
  const SdFinalizedHourV2FieldSpec* fields[] = {
      SdFinalizedHourV2HeaderFields(nullptr),
      SdFinalizedHourV2IndexEntryFields(nullptr),
      SdFinalizedHourV2BlockHeaderFields(nullptr),
      SdFinalizedHourV2DescriptorFields(nullptr),
      SdFinalizedHourV2PayloadFields(nullptr),
  };
  size_t counts[5] = {};
  SdFinalizedHourV2HeaderFields(&counts[0]);
  SdFinalizedHourV2IndexEntryFields(&counts[1]);
  SdFinalizedHourV2BlockHeaderFields(&counts[2]);
  SdFinalizedHourV2DescriptorFields(&counts[3]);
  SdFinalizedHourV2PayloadFields(&counts[4]);
  for (size_t t = 0; t < 5U; ++t) {
    for (size_t i = 0; i < counts[t]; ++i) {
      CHECK_TRUE(std::strcmp(fields[t][i].name, "slot_id") != 0);
      CHECK_TRUE(std::strcmp(fields[t][i].name, "addr16") != 0);
    }
  }
}


void TestChangingLabelsAreSnapshottedOnceAndCrcsValidate() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  ChangingLabels labels;
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(WriteSnapshot(snapshot, &labels, &sink, &status));
  CHECK_EQ(labels.node_calls, static_cast<uint32_t>(1));
  CHECK_EQ(labels.sensor_calls, static_cast<uint32_t>(1));

  SdFinalizedHourV2Header header;
  SdFinalizedHourV2IndexEntry index;
  SdFinalizedHourV2BlockHeader block_header;
  SdFinalizedHourV2Descriptor descriptor;
  SdFinalizedHourV2Payload payload;
  CHECK_TRUE(DecodeFirstRecord(sink, &header, &index, &block_header,
                               &descriptor, &payload));
  (void)header;
  (void)index;
  (void)block_header;
  (void)payload;
  CHECK_EQ(descriptor.node_label_len, static_cast<uint8_t>(10));
  CHECK_EQ(descriptor.sensor_label_len, static_cast<uint8_t>(12));
  CHECK_TRUE(std::memcmp(descriptor.node_label, "Node First", 10) == 0);
  CHECK_TRUE(std::memcmp(descriptor.sensor_label, "Sensor First", 12) == 0);
  CHECK_TRUE(RecordCrcsValidate(sink));
}

void TestNodeLabelSourceFailureWritesNoBytes() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  FailingLabels labels;
  labels.fail_node = true;
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(!WriteSnapshot(snapshot, &labels, &sink, &status));
  CHECK_EQ(status.failure_reason,
           SdFinalizedHourV2WriteFailureReason::kLabelSourceFailure);
  CHECK_EQ(sink.used, static_cast<size_t>(0));
  CHECK_EQ(sink.calls, static_cast<uint32_t>(0));
  CHECK_EQ(status.bytes_written, static_cast<uint32_t>(0));
  CHECK_EQ(labels.node_calls, static_cast<uint32_t>(1));
  CHECK_EQ(labels.sensor_calls, static_cast<uint32_t>(0));
}

void TestSensorLabelSourceFailureWritesNoBytes() {
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(&snapshot));
  FailingLabels labels;
  labels.fail_node = false;
  CaptureSink sink;
  SdFinalizedHourV2WriteStatus status;
  CHECK_TRUE(!WriteSnapshot(snapshot, &labels, &sink, &status));
  CHECK_EQ(status.failure_reason,
           SdFinalizedHourV2WriteFailureReason::kLabelSourceFailure);
  CHECK_EQ(sink.used, static_cast<size_t>(0));
  CHECK_EQ(sink.calls, static_cast<uint32_t>(0));
  CHECK_EQ(status.bytes_written, static_cast<uint32_t>(0));
  CHECK_EQ(labels.node_calls, static_cast<uint32_t>(1));
  CHECK_EQ(labels.sensor_calls, static_cast<uint32_t>(1));
}

void Run(const char* name, void (*fn)()) {
  g_test.current_test = name;
  fn();
}

}  // namespace

int main() {
  Run("TestOneSensorBasicStructure", TestOneSensorBasicStructure);
  Run("TestMappingPresenceMissingCorrectedCounts", TestMappingPresenceMissingCorrectedCounts);
  Run("TestLabelsCopiedMissingAndTruncated", TestLabelsCopiedMissingAndTruncated);
  Run("TestOrderingAndIdentity", TestOrderingAndIdentity);
  Run("TestDuplicateRom64RejectedBeforeWriting", TestDuplicateRom64RejectedBeforeWriting);
  Run("TestZeroSensorSkippedWithoutSink", TestZeroSensorSkippedWithoutSink);
  Run("TestCrcsValidateAndMutationsChangeCrcs", TestCrcsValidateAndMutationsChangeCrcs);
  Run("TestSinkFailures", TestSinkFailures);
  Run("TestSchemaHasNoDurableSlotIdOrAddr16", TestSchemaHasNoDurableSlotIdOrAddr16);
  Run("TestChangingLabelsAreSnapshottedOnceAndCrcsValidate", TestChangingLabelsAreSnapshottedOnceAndCrcsValidate);
  Run("TestNodeLabelSourceFailureWritesNoBytes", TestNodeLabelSourceFailureWritesNoBytes);
  Run("TestSensorLabelSourceFailureWritesNoBytes", TestSensorLabelSourceFailureWritesNoBytes);
  if (g_test.failures != 0U) {
    std::cerr << g_test.failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "sd_finalized_hour_v2_writer_test passed" << std::endl;
  return 0;
}
