#include "sd_finalized_hour_v2_scanner.h"

#include "history_hour_stager.h"
#include "sd_finalized_hour_v2_writer.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

struct TestHarness {
  const char* current_test = nullptr;
  uint32_t failures = 0;
};

TestHarness g_test;
static SdFinalizedHourV2WriterWorkspace g_writer_workspace;
static SdFinalizedHourV2ScannerWorkspace g_scanner_workspace;

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
  (void)actual;
  (void)expected;
  std::cerr << file << ":" << line << " in " << g_test.current_test
            << ": CHECK_EQ failed: " << a << " != " << e << std::endl;
  ++g_test.failures;
  return false;
}

#define CHECK_TRUE(expr) CheckTrue(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) CheckEq((actual), (expected), #actual, #expected, __FILE__, __LINE__)

constexpr uint32_t kHour = 28928160U;
constexpr size_t kCaptureBytes = 24000U;

struct CaptureSink {
  uint8_t bytes[kCaptureBytes] = {};
  size_t used = 0;
};

bool CaptureWrite(const uint8_t* data, size_t len, void* ctx) {
  CaptureSink* sink = static_cast<CaptureSink*>(ctx);
  if (sink == nullptr || (data == nullptr && len != 0U)) return false;
  if (sink->used + len > sizeof(sink->bytes)) return false;
  if (len != 0U) {
    std::memcpy(sink->bytes + sink->used, data, len);
    sink->used += len;
  }
  return true;
}

class VectorReader final : public ISdFinalizedHourV2ByteReader {
 public:
  explicit VectorReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
  uint64_t fail_offset = UINT64_MAX;

  bool Read(uint64_t offset,
            uint8_t* out,
            size_t len,
            size_t* bytes_read) override {
    if (bytes_read == nullptr) return false;
    *bytes_read = 0;
    if (offset == fail_offset) return false;
    if (len == 0U) return true;
    if (out == nullptr) return false;
    if (offset >= bytes_.size()) return true;
    const size_t available = bytes_.size() - static_cast<size_t>(offset);
    const size_t count = (available < len) ? available : len;
    std::memcpy(out, bytes_.data() + static_cast<size_t>(offset), count);
    *bytes_read = count;
    return true;
  }

 private:
  const std::vector<uint8_t>& bytes_;
};

size_t AppendPreamble(std::vector<uint8_t>* out) {
  char preamble[kSdFinalizedHourV2PreambleMaxBytes] = {};
  const size_t len = BuildSdFinalizedHourV2Preamble(preamble, sizeof(preamble));
  out->insert(out->end(), reinterpret_cast<const uint8_t*>(preamble),
              reinterpret_cast<const uint8_t*>(preamble) + len);
  return len;
}

bool BuildSnapshot(uint32_t hour, uint64_t suffix, HistoryHourSnapshot* out) {
  if (out == nullptr) return false;
  char rom[17] = {};
  static constexpr char kHex[] = "0123456789ABCDEF";
  const uint64_t rom64 = 0x2800000000000000ULL | (suffix & 0x00FFFFFFFFFFFFFFULL);
  for (int nibble = 15; nibble >= 0; --nibble) {
    rom[15 - nibble] = kHex[(rom64 >> (4U * nibble)) & 0xFU];
  }
  RamHourStager stager;
  bool ok = stager.ResetHour(hour);
  ok = stager.RecordSampleCentiC(rom, 0x10000001U, 5U, 2134, false) && ok;
  ok = stager.RecordSampleCentiC(rom, 0x10000001U, 6U, 2140, true) && ok;
  ok = stager.RecordMissing(rom, 0x10000001U, 7U) && ok;
  ok = stager.ExportSnapshot(out) && ok;
  return ok;
}


bool BuildTwoSensorSnapshot(HistoryHourSnapshot* out) {
  if (out == nullptr) return false;
  RamHourStager stager;
  bool ok = stager.ResetHour(kHour);
  ok = stager.RecordSampleCentiC("2800000000000060", 0x10000001U, 5U, 2134, false) && ok;
  ok = stager.RecordSampleCentiC("2800000000000061", 0x10000001U, 6U, 2234, false) && ok;
  ok = stager.ExportSnapshot(out) && ok;
  return ok;
}

bool AppendTwoSensorRecord(std::vector<uint8_t>* out) {
  HistoryHourSnapshot snapshot;
  if (!BuildTwoSensorSnapshot(&snapshot)) return false;
  CaptureSink sink;
  g_writer_workspace = SdFinalizedHourV2WriterWorkspace{};
  SdFinalizedHourV2WriteStatus status;
  if (!WriteSdFinalizedHourV2Record(snapshot, nullptr, g_writer_workspace,
                                    CaptureWrite, &sink, &status)) {
    return false;
  }
  if (status.sensor_count != 2U || sink.used != status.record_bytes) return false;
  out->insert(out->end(), sink.bytes, sink.bytes + sink.used);
  return true;
}

bool AppendRecord(std::vector<uint8_t>* out, uint32_t hour, uint64_t suffix) {
  HistoryHourSnapshot snapshot;
  if (!BuildSnapshot(hour, suffix, &snapshot)) return false;
  CaptureSink sink;
  g_writer_workspace = SdFinalizedHourV2WriterWorkspace{};
  SdFinalizedHourV2WriteStatus status;
  if (!WriteSdFinalizedHourV2Record(snapshot, nullptr, g_writer_workspace,
                                    CaptureWrite, &sink, &status)) {
    return false;
  }
  if (status.skipped_zero_sensor_hour || sink.used != status.record_bytes) return false;
  out->insert(out->end(), sink.bytes, sink.bytes + sink.used);
  return true;
}

std::vector<uint8_t> BuildDay(uint8_t record_count) {
  std::vector<uint8_t> out;
  AppendPreamble(&out);
  for (uint8_t i = 0; i < record_count; ++i) {
    CHECK_TRUE(AppendRecord(&out, kHour + (60U * i), 0x60U + i));
  }
  return out;
}

SdFinalizedHourV2ScanResult Scan(const std::vector<uint8_t>& bytes) {
  VectorReader reader(bytes);
  g_scanner_workspace = SdFinalizedHourV2ScannerWorkspace{};
  return ScanSdFinalizedHourV2DayFile(reader, g_scanner_workspace);
}

size_t BinaryStart(const std::vector<uint8_t>& bytes) {
  const char* marker = kSdFinalizedHourV2BinaryStartMarker;
  const size_t marker_len = std::strlen(marker);
  for (size_t i = 0; i + marker_len <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, marker, marker_len) == 0) return i + marker_len;
  }
  return 0U;
}

SdFinalizedHourV2Header DecodeHeaderAt(const std::vector<uint8_t>& bytes,
                                       size_t offset) {
  SdFinalizedHourV2Header header;
  CHECK_TRUE(DecodeSdFinalizedHourV2Header(bytes.data() + offset,
                                           kSdFinalizedHourV2HeaderBytes,
                                           &header));
  return header;
}

void EncodeHeaderAt(std::vector<uint8_t>* bytes,
                    size_t offset,
                    SdFinalizedHourV2Header header,
                    bool recompute_header_crc) {
  if (recompute_header_crc) {
    header.header_crc32 = 0;
    CHECK_TRUE(EncodeSdFinalizedHourV2Header(header, bytes->data() + offset,
                                             kSdFinalizedHourV2HeaderBytes));
    header.header_crc32 = ComputeSdFinalizedHourV2HeaderCrc32(
        bytes->data() + offset, kSdFinalizedHourV2HeaderBytes);
  }
  CHECK_TRUE(EncodeSdFinalizedHourV2Header(header, bytes->data() + offset,
                                           kSdFinalizedHourV2HeaderBytes));
}


SdFinalizedHourV2BlockHeader DecodeBlockHeaderAt(const std::vector<uint8_t>& bytes,
                                                size_t block_offset) {
  SdFinalizedHourV2BlockHeader header;
  CHECK_TRUE(DecodeSdFinalizedHourV2BlockHeader(
      bytes.data() + block_offset, kSdFinalizedHourV2BlockHeaderBytes, &header));
  return header;
}

void EncodeBlockHeaderAt(std::vector<uint8_t>* bytes,
                         size_t block_offset,
                         SdFinalizedHourV2BlockHeader header,
                         bool recompute_block_crc) {
  if (recompute_block_crc) {
    header.block_crc32 = 0;
    CHECK_TRUE(EncodeSdFinalizedHourV2BlockHeader(
        header, bytes->data() + block_offset, kSdFinalizedHourV2BlockHeaderBytes));
    header.block_crc32 = ComputeSdFinalizedHourV2BlockCrc32(
        bytes->data() + block_offset, kSdFinalizedHourV2FixedBlockBytes);
  }
  CHECK_TRUE(EncodeSdFinalizedHourV2BlockHeader(
      header, bytes->data() + block_offset, kSdFinalizedHourV2BlockHeaderBytes));
}

size_t FirstBlockOffset(const std::vector<uint8_t>& bytes, size_t record_offset) {
  const SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, record_offset);
  return record_offset + header.sensor_blocks_offset;
}

void RecomputeBlockCrc(std::vector<uint8_t>* bytes, size_t block_offset) {
  SdFinalizedHourV2BlockHeader block;
  CHECK_TRUE(DecodeSdFinalizedHourV2BlockHeader(
      bytes->data() + block_offset, kSdFinalizedHourV2BlockHeaderBytes, &block));
  block.block_crc32 = 0;
  CHECK_TRUE(EncodeSdFinalizedHourV2BlockHeader(
      block, bytes->data() + block_offset, kSdFinalizedHourV2BlockHeaderBytes));
  block.block_crc32 = ComputeSdFinalizedHourV2BlockCrc32(
      bytes->data() + block_offset, kSdFinalizedHourV2FixedBlockBytes);
  CHECK_TRUE(EncodeSdFinalizedHourV2BlockHeader(
      block, bytes->data() + block_offset, kSdFinalizedHourV2BlockHeaderBytes));
}

void RecomputePayloadAndHeaderCrc(std::vector<uint8_t>* bytes, size_t record_offset) {
  SdFinalizedHourV2Header header = DecodeHeaderAt(*bytes, record_offset);
  header.payload_crc32 = ComputeSdFinalizedHourV2PayloadCrc32(
      bytes->data() + record_offset + header.index_offset,
      static_cast<size_t>(header.index_bytes + header.sensor_blocks_bytes));
  EncodeHeaderAt(bytes, record_offset, header, true);
}

void ExpectReason(const std::vector<uint8_t>& bytes,
                  SdFinalizedHourV2ScanStatus status,
                  SdFinalizedHourV2ScanFailureReason reason) {
  const SdFinalizedHourV2ScanResult result = Scan(bytes);
  CHECK_EQ(result.status, status);
  CHECK_EQ(result.first_failure_reason, reason);
}

void TestCleanPreambleOnly() {
  std::vector<uint8_t> bytes;
  const size_t start = AppendPreamble(&bytes);
  const SdFinalizedHourV2ScanResult result = Scan(bytes);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kEmptyPreambleOnly);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kNone);
  CHECK_EQ(result.valid_record_count, 0U);
  CHECK_EQ(result.binary_stream_start_offset, static_cast<uint64_t>(start));
  CHECK_EQ(result.valid_prefix_end_offset, static_cast<uint64_t>(start));
}

void TestCleanOneRecord() {
  const std::vector<uint8_t> bytes = BuildDay(1);
  const SdFinalizedHourV2ScanResult result = Scan(bytes);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kClean);
  CHECK_EQ(result.valid_record_count, 1U);
  CHECK_EQ(result.valid_prefix_end_offset, static_cast<uint64_t>(bytes.size()));
  CHECK_EQ(result.first_unsafe_offset, static_cast<uint64_t>(bytes.size()));
}

void TestCleanTwoRecords() {
  const std::vector<uint8_t> bytes = BuildDay(2);
  const size_t start = BinaryStart(bytes);
  const SdFinalizedHourV2Header first = DecodeHeaderAt(bytes, start);
  const SdFinalizedHourV2ScanResult result = Scan(bytes);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kClean);
  CHECK_EQ(result.valid_record_count, 2U);
  CHECK_EQ(result.valid_prefix_end_offset, static_cast<uint64_t>(bytes.size()));
  CHECK_TRUE(start + first.record_bytes < bytes.size());
}

void TestMissingMarker() {
  const std::vector<uint8_t> bytes = {'n', 'o', ' ', 'm', 'a', 'r', 'k', 'e', 'r'};
  const SdFinalizedHourV2ScanResult result = Scan(bytes);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kMissingMarker);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kMarkerMissing);
  CHECK_EQ(result.valid_prefix_end_offset, 0ULL);
}

void TestTruncatedHeaderAtBinaryStart() {
  std::vector<uint8_t> bytes;
  const size_t start = AppendPreamble(&bytes);
  bytes.push_back('x');
  const SdFinalizedHourV2ScanResult result = Scan(bytes);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kPartialHeader);
  CHECK_EQ(result.first_unsafe_offset, static_cast<uint64_t>(start));
}

void TestValidRecordThenTruncatedHeader() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t unsafe = bytes.size();
  bytes.push_back(0x4D);
  const SdFinalizedHourV2ScanResult result = Scan(bytes);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kCorruptTail);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kPartialHeader);
  CHECK_EQ(result.valid_record_count, 1U);
  CHECK_EQ(result.valid_prefix_end_offset, static_cast<uint64_t>(unsafe));
  CHECK_EQ(result.first_unsafe_offset, static_cast<uint64_t>(unsafe));
}

void TestBadRecordMagic() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  bytes[start] ^= 0xFFU;
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadRecordMagic);
}

void TestBadHeaderCrc() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  bytes[start + 12U] ^= 0x01U;
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadHeaderCrc);
}

void TestBadPayloadCrc() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  const size_t block = start + header.sensor_blocks_offset;
  bytes[block + kSdFinalizedHourV2BlockHeaderBytes + kSdFinalizedHourV2DescriptorBytes + 16U] ^= 0x01U;
  RecomputeBlockCrc(&bytes, block);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadPayloadCrc);
}

void TestBadBlockCrc() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  bytes[start + header.sensor_blocks_offset + kSdFinalizedHourV2BlockHeaderBytes + 8U] ^= 0x01U;
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadBlockCrc);
}

void TestSensorCountZero() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  header.sensor_count = 0;
  EncodeHeaderAt(&bytes, start, header, true);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kZeroSensorCount);
}

void TestSensorCountTooLarge() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  header.sensor_count = kSdFinalizedHourV2ScannerMaxSensors + 1U;
  EncodeHeaderAt(&bytes, start, header, true);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kDangerousSizeOrOffset,
               SdFinalizedHourV2ScanFailureReason::kSensorCountTooLarge);
}

void TestBadOffsetsAndBytes() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  header.index_offset = 99U;
  EncodeHeaderAt(&bytes, start, header, true);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadIndexOffset);

  bytes = BuildDay(1);
  header = DecodeHeaderAt(bytes, start);
  header.index_bytes += 1U;
  EncodeHeaderAt(&bytes, start, header, true);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadIndexBytes);

  bytes = BuildDay(1);
  header = DecodeHeaderAt(bytes, start);
  header.sensor_blocks_offset += 1U;
  EncodeHeaderAt(&bytes, start, header, true);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadSensorBlocksOffset);
}

void TestDuplicateRom64() {
  std::vector<uint8_t> bytes;
  AppendPreamble(&bytes);
  CHECK_TRUE(AppendTwoSensorRecord(&bytes));
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  CHECK_EQ(header.sensor_count, static_cast<uint16_t>(2));
  const size_t first_index = start + header.index_offset;
  const size_t second_index = first_index + kSdFinalizedHourV2IndexEntryBytes;
  std::memcpy(bytes.data() + second_index, bytes.data() + first_index, sizeof(uint64_t));
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kDuplicateRom64);
}

void TestDescriptorRom64Mismatch() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  const size_t descriptor = start + header.sensor_blocks_offset + kSdFinalizedHourV2BlockHeaderBytes;
  bytes[descriptor] ^= 0x01U;
  RecomputeBlockCrc(&bytes, start + header.sensor_blocks_offset);
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kDescriptorRom64Mismatch);
}

void TestCorrectedWithoutPresence() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  const size_t block = start + header.sensor_blocks_offset;
  const size_t payload = block + kSdFinalizedHourV2BlockHeaderBytes + kSdFinalizedHourV2DescriptorBytes;
  bytes[payload] = 0U;
  bytes[payload + kSdFinalizedHourV2BitmapBytes] = 0x01U;
  RecomputeBlockCrc(&bytes, block);
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kCorrectedWithoutPresence);
}

void TestDangerousRecordSize() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  header.record_bytes = kSdFinalizedHourV2HeaderBytes - 1U;
  EncodeHeaderAt(&bytes, start, header, true);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kDangerousSizeOrOffset,
               SdFinalizedHourV2ScanFailureReason::kRecordBytesTooSmall);
}


void TestMarkerTooLate() {
  std::vector<uint8_t> bytes(kSdFinalizedHourV2PreambleMaxBytes, static_cast<uint8_t>('A'));
  const char* marker = kSdFinalizedHourV2BinaryStartMarker;
  bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(marker),
               reinterpret_cast<const uint8_t*>(marker) + std::strlen(marker));
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kMarkerTooLate,
               SdFinalizedHourV2ScanFailureReason::kMarkerTooLate);
}

void TestUnsupportedRecordVersion() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  header.record_version = kSdFinalizedHourV2RecordVersion + 1U;
  EncodeHeaderAt(&bytes, start, header, true);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kUnsupportedFormat,
               SdFinalizedHourV2ScanFailureReason::kUnsupportedRecordVersion);
}

void TestUnsupportedBlockVersion() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  const size_t block = FirstBlockOffset(bytes, start);
  SdFinalizedHourV2BlockHeader header = DecodeBlockHeaderAt(bytes, block);
  header.block_version = kSdFinalizedHourV2BlockVersion + 1U;
  EncodeBlockHeaderAt(&bytes, block, header, true);
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kUnsupportedFormat,
               SdFinalizedHourV2ScanFailureReason::kUnsupportedBlockVersion);
}

void TestBadBlockMagic() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  const size_t block = FirstBlockOffset(bytes, start);
  SdFinalizedHourV2BlockHeader header = DecodeBlockHeaderAt(bytes, block);
  header.block_magic ^= 0x000000FFU;
  EncodeBlockHeaderAt(&bytes, block, header, true);
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadBlockMagic);
}

void TestWrongSampleCountBytesEncoding() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  size_t block = FirstBlockOffset(bytes, start);
  SdFinalizedHourV2BlockHeader header = DecodeBlockHeaderAt(bytes, block);
  header.sample_count = kSdFinalizedHourV2SampleCount - 1U;
  EncodeBlockHeaderAt(&bytes, block, header, true);
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadSampleCount);

  bytes = BuildDay(1);
  block = FirstBlockOffset(bytes, start);
  header = DecodeBlockHeaderAt(bytes, block);
  header.sample_bytes = kSdFinalizedHourV2SampleBytes + 1U;
  EncodeBlockHeaderAt(&bytes, block, header, true);
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadSampleBytes);

  bytes = BuildDay(1);
  block = FirstBlockOffset(bytes, start);
  header = DecodeBlockHeaderAt(bytes, block);
  header.sample_encoding = kSdFinalizedHourV2SampleEncodingInt16CentiCLe + 1U;
  EncodeBlockHeaderAt(&bytes, block, header, true);
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadSampleEncoding);
}

void TestBadDescriptorLabelLength() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  const size_t block = FirstBlockOffset(bytes, start);
  const size_t descriptor = block + kSdFinalizedHourV2BlockHeaderBytes;
  bytes[descriptor + 20U] = kSdFinalizedHourV2NodeLabelMaxBytes + 1U;
  RecomputeBlockCrc(&bytes, block);
  RecomputePayloadAndHeaderCrc(&bytes, start);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart,
               SdFinalizedHourV2ScanFailureReason::kBadDescriptorLabelLength);
}

void TestReadErrorDistinctFromShortRead() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  VectorReader reader(bytes);
  reader.fail_offset = start;
  g_scanner_workspace = SdFinalizedHourV2ScannerWorkspace{};
  const SdFinalizedHourV2ScanResult read_error =
      ScanSdFinalizedHourV2DayFile(reader, g_scanner_workspace);
  CHECK_EQ(read_error.status, SdFinalizedHourV2ScanStatus::kReadError);
  CHECK_EQ(read_error.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kReadError);

  std::vector<uint8_t> truncated;
  const size_t binary_start = AppendPreamble(&truncated);
  truncated.push_back('x');
  const SdFinalizedHourV2ScanResult short_read = Scan(truncated);
  CHECK_EQ(short_read.status, SdFinalizedHourV2ScanStatus::kInvalidAtBinaryStart);
  CHECK_EQ(short_read.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kPartialHeader);
  CHECK_EQ(short_read.first_unsafe_offset, static_cast<uint64_t>(binary_start));
}

void TestRecordBytesTooLarge() {
  std::vector<uint8_t> bytes = BuildDay(1);
  const size_t start = BinaryStart(bytes);
  SdFinalizedHourV2Header header = DecodeHeaderAt(bytes, start);
  header.record_bytes = kSdFinalizedHourV2ScannerMaxRecordBytes + 1U;
  EncodeHeaderAt(&bytes, start, header, true);
  ExpectReason(bytes, SdFinalizedHourV2ScanStatus::kDangerousSizeOrOffset,
               SdFinalizedHourV2ScanFailureReason::kRecordBytesTooLarge);
}

void TestPreambleMarkerExactlyOnceAtEnd() {
  std::vector<uint8_t> bytes;
  const size_t preamble_bytes = AppendPreamble(&bytes);
  const char* marker = kSdFinalizedHourV2BinaryStartMarker;
  const size_t marker_len = std::strlen(marker);
  size_t count = 0;
  size_t last_start = 0;
  for (size_t i = 0; i + marker_len <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, marker, marker_len) == 0) {
      ++count;
      last_start = i;
    }
  }
  CHECK_EQ(count, static_cast<size_t>(1));
  CHECK_EQ(last_start + marker_len, preamble_bytes);
}

void Run(const char* name, void (*fn)()) {
  g_test.current_test = name;
  fn();
}

}  // namespace

int main() {
  Run("CleanPreambleOnly", TestCleanPreambleOnly);
  Run("CleanOneRecord", TestCleanOneRecord);
  Run("CleanTwoRecords", TestCleanTwoRecords);
  Run("MissingMarker", TestMissingMarker);
  Run("TruncatedHeaderAtBinaryStart", TestTruncatedHeaderAtBinaryStart);
  Run("ValidRecordThenTruncatedHeader", TestValidRecordThenTruncatedHeader);
  Run("BadRecordMagic", TestBadRecordMagic);
  Run("BadHeaderCrc", TestBadHeaderCrc);
  Run("BadPayloadCrc", TestBadPayloadCrc);
  Run("BadBlockCrc", TestBadBlockCrc);
  Run("SensorCountZero", TestSensorCountZero);
  Run("SensorCountTooLarge", TestSensorCountTooLarge);
  Run("BadOffsetsAndBytes", TestBadOffsetsAndBytes);
  Run("DuplicateRom64", TestDuplicateRom64);
  Run("DescriptorRom64Mismatch", TestDescriptorRom64Mismatch);
  Run("CorrectedWithoutPresence", TestCorrectedWithoutPresence);
  Run("DangerousRecordSize", TestDangerousRecordSize);
  Run("MarkerTooLate", TestMarkerTooLate);
  Run("UnsupportedRecordVersion", TestUnsupportedRecordVersion);
  Run("UnsupportedBlockVersion", TestUnsupportedBlockVersion);
  Run("BadBlockMagic", TestBadBlockMagic);
  Run("WrongSampleCountBytesEncoding", TestWrongSampleCountBytesEncoding);
  Run("BadDescriptorLabelLength", TestBadDescriptorLabelLength);
  Run("ReadErrorDistinctFromShortRead", TestReadErrorDistinctFromShortRead);
  Run("RecordBytesTooLarge", TestRecordBytesTooLarge);
  Run("PreambleMarkerExactlyOnceAtEnd", TestPreambleMarkerExactlyOnceAtEnd);

  if (g_test.failures != 0U) {
    std::cerr << g_test.failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "sd_finalized_hour_v2_scanner_test passed" << std::endl;
  return 0;
}
