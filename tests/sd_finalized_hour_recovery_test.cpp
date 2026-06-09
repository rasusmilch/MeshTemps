#include "history_hour_stager.h"
#include "sd_finalized_hour_block.h"
#include "sd_finalized_hour_recovery.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {

constexpr uint32_t kHourA = 30000000U;
constexpr uint32_t kHourB = kHourA + 60U;
constexpr size_t kScratchBytes = 17U;
constexpr size_t kHeaderCrcOffset = 36U;


struct TestHarness {
  const char* current_test = nullptr;
  uint32_t failures = 0;
};

TestHarness g_test;

bool CheckTrue(bool condition, const char* expr, const char* file, int line) {
  if (condition) return true;
  std::cerr << file << ":" << line;
  if (g_test.current_test != nullptr) {
    std::cerr << " in " << g_test.current_test;
  }
  std::cerr << ": CHECK_TRUE failed: " << expr << std::endl;
  ++g_test.failures;
  return false;
}

template <typename Actual, typename Expected>
bool CheckEq(const Actual& actual,
             const Expected& expected,
             const char* actual_expr,
             const char* expected_expr,
             const char* file,
             int line) {
  if (actual == expected) return true;
  std::cerr << file << ":" << line;
  if (g_test.current_test != nullptr) {
    std::cerr << " in " << g_test.current_test;
  }
  std::cerr << ": CHECK_EQ failed: " << actual_expr << " != "
            << expected_expr << std::endl;
  ++g_test.failures;
  return false;
}

#define CHECK_TRUE(expr) CheckTrue(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
  CheckEq((actual), (expected), #actual, #expected, __FILE__, __LINE__)

class MemoryReader final : public ISdFinalizedHourByteReader {
 public:
  explicit MemoryReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  bool Read(uint64_t offset,
            uint8_t* out,
            size_t len,
            size_t* bytes_read) override {
    if (bytes_read == nullptr) return false;
    *bytes_read = 0U;
    max_requested_len = std::max(max_requested_len, len);
    ++read_count;
    if (fail_reads && offset <= fail_offset && fail_offset < offset + len) {
      return false;
    }
    if (out == nullptr && len > 0U) return false;
    if (offset >= bytes_.size()) return true;
    const size_t available = bytes_.size() - static_cast<size_t>(offset);
    const size_t count = std::min(len, available);
    if (count > 0U) std::memcpy(out, bytes_.data() + offset, count);
    *bytes_read = count;
    return true;
  }

  void FailReadsAt(uint64_t offset) {
    fail_reads = true;
    fail_offset = offset;
  }

  size_t max_requested_len = 0U;
  size_t read_count = 0U;

 private:
  const std::vector<uint8_t>& bytes_;
  bool fail_reads = false;
  uint64_t fail_offset = std::numeric_limits<uint64_t>::max();
};

bool VectorSink(const uint8_t* data, size_t len, void* ctx) {
  auto* out = static_cast<std::vector<uint8_t>*>(ctx);
  if (out == nullptr || (data == nullptr && len > 0U)) return false;
  out->insert(out->end(), data, data + len);
  return true;
}

void WriteU16(std::vector<uint8_t>* bytes, size_t offset, uint16_t value) {
  if (!CHECK_TRUE(bytes != nullptr)) return;
  if (!CHECK_TRUE(offset + 2U <= bytes->size())) return;
  (*bytes)[offset] = static_cast<uint8_t>(value & 0xFFU);
  (*bytes)[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
  if (!CHECK_TRUE(offset + 4U <= bytes.size())) return 0U;
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4U; ++i) {
    value |= static_cast<uint32_t>(bytes[offset + i]) << (8U * i);
  }
  return value;
}

void WriteU32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  if (!CHECK_TRUE(bytes != nullptr)) return;
  if (!CHECK_TRUE(offset + 4U <= bytes->size())) return;
  for (uint8_t i = 0; i < 4U; ++i) {
    (*bytes)[offset + i] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
  }
}

void RecomputeHeaderCrc(std::vector<uint8_t>* bytes, size_t record_offset = 0U) {
  if (!CHECK_TRUE(bytes != nullptr)) return;
  if (!CHECK_TRUE(record_offset + kSdFinalizedHourHeaderBytes <= bytes->size())) {
    return;
  }
  for (uint8_t i = 0; i < sizeof(uint32_t); ++i) {
    (*bytes)[record_offset + kHeaderCrcOffset + i] = 0U;
  }
  const uint32_t crc = ComputeSdFinalizedHourHeaderCrc32(
      bytes->data() + record_offset, kSdFinalizedHourHeaderBytes);
  WriteU32(bytes, record_offset + kHeaderCrcOffset, crc);
}

std::vector<uint8_t> BuildRecord(uint32_t hour_start_epoch_minute) {
  RamHourStager stager;
  bool fixture_ok = true;
  fixture_ok = CHECK_TRUE(stager.ResetHour(hour_start_epoch_minute)) && fixture_ok;
  fixture_ok = CHECK_TRUE(stager.RecordSample("2800000000000060", 0x10000001U,
                                               0U, 12.34f, true)) &&
               fixture_ok;
  fixture_ok = CHECK_TRUE(stager.RecordSample("2800000000000061", 0x20000002U,
                                               20U, 21.25f, false)) &&
               fixture_ok;
  fixture_ok = CHECK_TRUE(stager.RecordSample("2800000000000061", 0x20000002U,
                                               21U, 21.50f, true)) &&
               fixture_ok;
  if (!fixture_ok) return {};

  std::vector<uint8_t> bytes;
  RamHourStagerFinalizedHourSource source(stager);
  fixture_ok = CHECK_TRUE(WriteSdFinalizedHourBlock(source, VectorSink, &bytes)) &&
               fixture_ok;
  fixture_ok = CHECK_TRUE(VerifySdFinalizedHourBlock(bytes.data(), bytes.size())) &&
               fixture_ok;
  return fixture_ok ? bytes : std::vector<uint8_t>{};
}

std::vector<uint8_t> AppendBytes(std::vector<uint8_t> a,
                                 const std::vector<uint8_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

SdFinalizedHourScanResult Scan(const std::vector<uint8_t>& bytes) {
  uint8_t scratch[kScratchBytes] = {};
  MemoryReader reader(bytes);
  return ScanSdFinalizedHourAppendFile(reader, scratch, sizeof(scratch));
}

void ExpectClean(const SdFinalizedHourScanResult& result,
                 uint32_t record_count,
                 uint64_t expected_offset,
                 uint32_t last_hour) {
  CHECK_EQ(result.status, SdFinalizedHourScanStatus::kClean);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourScanFailureReason::kNone);
  CHECK_EQ(result.valid_record_count, record_count);
  CHECK_EQ(result.last_good_offset, expected_offset);
  CHECK_EQ(result.first_bad_offset, expected_offset);
  CHECK_EQ(result.expected_next_offset, expected_offset);
  CHECK_EQ(result.last_valid_hour_start_epoch_minute, last_hour);
  CHECK_TRUE(!result.saw_partial_header);
  CHECK_TRUE(!result.saw_partial_payload);
  CHECK_TRUE(!result.saw_bad_header_crc);
  CHECK_TRUE(!result.saw_bad_payload_crc);
}

void ExpectFailure(const SdFinalizedHourScanResult& result,
                   SdFinalizedHourScanStatus status,
                   SdFinalizedHourScanFailureReason reason,
                   uint32_t record_count,
                   uint64_t last_good_offset,
                   uint64_t first_bad_offset) {
  CHECK_EQ(result.status, status);
  CHECK_EQ(result.first_failure_reason, reason);
  CHECK_EQ(result.valid_record_count, record_count);
  CHECK_EQ(result.last_good_offset, last_good_offset);
  CHECK_EQ(result.first_bad_offset, first_bad_offset);
  CHECK_EQ(result.expected_next_offset, last_good_offset);
  CHECK_TRUE(result.saw_partial_header ==
         (reason == SdFinalizedHourScanFailureReason::kPartialHeader));
  CHECK_TRUE(result.saw_partial_payload ==
         (reason == SdFinalizedHourScanFailureReason::kPartialPayload));
  CHECK_TRUE(result.saw_bad_header_crc ==
         (reason == SdFinalizedHourScanFailureReason::kBadHeaderCrc));
  CHECK_TRUE(result.saw_bad_payload_crc ==
         (reason == SdFinalizedHourScanFailureReason::kBadPayloadCrc));
}

void ExpectCorruptTailAfterPrefix(
    const std::vector<uint8_t>& valid_prefix,
    const std::vector<uint8_t>& tail,
    SdFinalizedHourScanFailureReason reason) {
  const std::vector<uint8_t> bytes = AppendBytes(valid_prefix, tail);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kCorruptTail, reason, 1U,
                valid_prefix.size(), valid_prefix.size());
  CHECK_EQ(result.last_valid_hour_start_epoch_minute, kHourA);
}

void TestEmptyFile() {
  const std::vector<uint8_t> bytes;
  const SdFinalizedHourScanResult result = Scan(bytes);
  CHECK_EQ(result.status, SdFinalizedHourScanStatus::kEmpty);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourScanFailureReason::kNone);
  CHECK_EQ(result.valid_record_count, 0U);
  CHECK_EQ(result.last_good_offset, 0U);
  CHECK_EQ(result.first_bad_offset, 0U);
  CHECK_EQ(result.expected_next_offset, 0U);
}

void TestOneValidRecord() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  const SdFinalizedHourScanResult result = Scan(record);
  ExpectClean(result, 1U, record.size(), kHourA);
}

void TestMultipleValidRecords() {
  const std::vector<uint8_t> first = BuildRecord(kHourA);
  const std::vector<uint8_t> second = BuildRecord(kHourB);
  const std::vector<uint8_t> bytes = AppendBytes(first, second);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectClean(result, 2U, bytes.size(), kHourB);
}

void TestValidRecordsFollowedByCleanEof() {
  const std::vector<uint8_t> first = BuildRecord(kHourA);
  const std::vector<uint8_t> second = BuildRecord(kHourB);
  const std::vector<uint8_t> bytes = AppendBytes(first, second);
  MemoryReader reader(bytes);
  uint8_t scratch[kScratchBytes] = {};
  const SdFinalizedHourScanResult result =
      ScanSdFinalizedHourAppendFile(reader, scratch, sizeof(scratch));
  ExpectClean(result, 2U, bytes.size(), kHourB);
  CHECK_TRUE(reader.read_count > 2U);
}

void TestPartialHeaderAtOffsetZero() {
  const std::vector<uint8_t> bytes(3U, 0xA5U);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kInvalidAtZero,
                SdFinalizedHourScanFailureReason::kPartialHeader, 0U, 0U, 0U);
}

void TestPartialHeaderAfterOneValidRecord() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  const std::vector<uint8_t> tail(5U, 0x5AU);
  ExpectCorruptTailAfterPrefix(
      record, tail, SdFinalizedHourScanFailureReason::kPartialHeader);
}

void TestBadMagicAtOffsetZero() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU32(&bytes, 0U, 0x12345678U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kInvalidAtZero,
                SdFinalizedHourScanFailureReason::kBadMagic, 0U, 0U, 0U);
}

void TestBadMagicAfterOneValidRecord() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  std::vector<uint8_t> tail = BuildRecord(kHourB);
  WriteU32(&tail, 0U, 0x12345678U);
  RecomputeHeaderCrc(&tail);
  ExpectCorruptTailAfterPrefix(record, tail,
                               SdFinalizedHourScanFailureReason::kBadMagic);
}

void TestUnsupportedVersion() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 4U, kSdFinalizedHourVersion + 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kUnsupportedFormat,
                SdFinalizedHourScanFailureReason::kUnsupportedVersion, 0U, 0U,
                0U);
}

void TestBadHeaderSize() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 6U, kSdFinalizedHourHeaderBytes + 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kBadHeaderSize, 0U, 0U, 0U);
}

void TestBadSnapshotFormatVersion() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 40U, kHistoryHourSnapshotFormatVersion + 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kUnsupportedFormat,
                SdFinalizedHourScanFailureReason::kBadSnapshotFormatVersion,
                0U, 0U, 0U);
}

void TestZeroHourRejected() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU32(&bytes, 12U, 0U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kInvalidAtZero,
                SdFinalizedHourScanFailureReason::kZeroHour, 0U, 0U, 0U);
}

void TestBadHeaderCrc() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  bytes[kHeaderCrcOffset] ^= 0x01U;
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kInvalidAtZero,
                SdFinalizedHourScanFailureReason::kBadHeaderCrc, 0U, 0U, 0U);
}

void TestBadPayloadCrc() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  bytes[kSdFinalizedHourHeaderBytes] ^= 0x01U;
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kInvalidAtZero,
                SdFinalizedHourScanFailureReason::kBadPayloadCrc, 0U, 0U, 0U);
}

void TestPartialPayloadAtOffsetZero() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  bytes.resize(kSdFinalizedHourHeaderBytes + 3U);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kInvalidAtZero,
                SdFinalizedHourScanFailureReason::kPartialPayload, 0U, 0U, 0U);
}

void TestPartialPayloadAfterOneValidRecord() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  std::vector<uint8_t> tail = BuildRecord(kHourB);
  tail.resize(kSdFinalizedHourHeaderBytes + 3U);
  ExpectCorruptTailAfterPrefix(
      record, tail, SdFinalizedHourScanFailureReason::kPartialPayload);
}

void TestRecordBytesTooSmall() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU32(&bytes, 8U, kSdFinalizedHourHeaderBytes - 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kRecordBytesTooSmall, 0U, 0U,
                0U);
}

void TestRecordBytesTooLarge() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU32(&bytes, 8U, static_cast<uint32_t>(kSdFinalizedHourMaxRecordBytes + 1U));
  RecomputeHeaderCrc(&bytes);
  MemoryReader reader(bytes);
  uint8_t scratch[kScratchBytes] = {};
  const SdFinalizedHourScanResult result =
      ScanSdFinalizedHourAppendFile(reader, scratch, sizeof(scratch));
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kRecordBytesTooLarge, 0U, 0U,
                0U);
  CHECK_EQ(reader.max_requested_len, kSdFinalizedHourHeaderBytes);
}

void TestRecordBytesMismatch() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  const uint32_t record_bytes = ReadU32(bytes, 8U);
  CHECK_TRUE(record_bytes >= kSdFinalizedHourHeaderBytes);
  CHECK_TRUE(record_bytes < kSdFinalizedHourMaxRecordBytes);
  WriteU32(&bytes, 8U, record_bytes + 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kRecordBytesMismatch, 0U, 0U,
                0U);
}

void TestDescriptorBytesMismatch() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU32(&bytes, 24U, 999U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kDescriptorBytesMismatch, 0U,
                0U, 0U);
}

void TestBadDescriptorEntryBytes() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 18U, kSdFinalizedHourDescriptorBytes + 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kBadDescriptorEntryBytes, 0U,
                0U, 0U);
}

void TestFrameCountMismatch() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 20U, kHistoryMinutesPerHour + 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kBadFrameCount, 0U, 0U, 0U);
}

void TestFrameBytesMismatch() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 22U, kSdFinalizedHourFrameBytes + 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kBadFrameBytes, 0U, 0U, 0U);
}

void TestActiveSlotCountGreaterThanCapacity() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 16U, kHistorySlotCapacity + 1U);
  RecomputeHeaderCrc(&bytes);
  const SdFinalizedHourScanResult result = Scan(bytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kActiveSlotCountTooLarge, 0U,
                0U, 0U);
}

void TestGarbageBytesAfterValidRecord() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  const std::vector<uint8_t> tail(kSdFinalizedHourHeaderBytes, 0xFFU);
  ExpectCorruptTailAfterPrefix(record, tail,
                               SdFinalizedHourScanFailureReason::kBadMagic);
}

void TestReadErrorWhileReadingHeader() {
  const std::vector<uint8_t> bytes = BuildRecord(kHourA);
  MemoryReader reader(bytes);
  reader.FailReadsAt(0U);
  uint8_t scratch[kScratchBytes] = {};
  const SdFinalizedHourScanResult result =
      ScanSdFinalizedHourAppendFile(reader, scratch, sizeof(scratch));
  ExpectFailure(result, SdFinalizedHourScanStatus::kReadError,
                SdFinalizedHourScanFailureReason::kReadError, 0U, 0U, 0U);
}

void TestReadErrorWhileReadingPayload() {
  const std::vector<uint8_t> bytes = BuildRecord(kHourA);
  MemoryReader reader(bytes);
  reader.FailReadsAt(kSdFinalizedHourHeaderBytes + 1U);
  uint8_t scratch[kScratchBytes] = {};
  const SdFinalizedHourScanResult result =
      ScanSdFinalizedHourAppendFile(reader, scratch, sizeof(scratch));
  ExpectFailure(result, SdFinalizedHourScanStatus::kReadError,
                SdFinalizedHourScanFailureReason::kReadError, 0U, 0U, 0U);
}

void TestCorruptTailOffsetsForBadPayloadCrc() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  std::vector<uint8_t> tail = BuildRecord(kHourB);
  tail[kSdFinalizedHourHeaderBytes] ^= 0x01U;
  ExpectCorruptTailAfterPrefix(
      record, tail, SdFinalizedHourScanFailureReason::kBadPayloadCrc);
}

void TestInvalidAtZeroDiffersFromCorruptTail() {
  std::vector<uint8_t> invalid_first = BuildRecord(kHourA);
  WriteU32(&invalid_first, 0U, 0x11111111U);
  RecomputeHeaderCrc(&invalid_first);
  const SdFinalizedHourScanResult invalid = Scan(invalid_first);
  CHECK_EQ(invalid.status, SdFinalizedHourScanStatus::kInvalidAtZero);

  const std::vector<uint8_t> record = BuildRecord(kHourA);
  ExpectCorruptTailAfterPrefix(
      record, invalid_first, SdFinalizedHourScanFailureReason::kBadMagic);
}

void TestScannerRejectsInvalidSizesBeforePayloadReads() {
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU32(&bytes, 28U, 0x7FFFFFFFU);
  RecomputeHeaderCrc(&bytes);
  MemoryReader reader(bytes);
  uint8_t scratch[kScratchBytes] = {};
  const SdFinalizedHourScanResult result =
      ScanSdFinalizedHourAppendFile(reader, scratch, sizeof(scratch));
  ExpectFailure(result, SdFinalizedHourScanStatus::kDangerousHeader,
                SdFinalizedHourScanFailureReason::kPayloadBytesMismatch, 0U,
                0U, 0U);
  CHECK_EQ(reader.max_requested_len, kSdFinalizedHourHeaderBytes);
}

void TestTinyScratchBufferStillStreams() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  MemoryReader reader(record);
  uint8_t scratch[1] = {};
  const SdFinalizedHourScanResult result =
      ScanSdFinalizedHourAppendFile(reader, scratch, sizeof(scratch));
  ExpectClean(result, 1U, record.size(), kHourA);
  CHECK_EQ(reader.max_requested_len, kSdFinalizedHourHeaderBytes);
}

void TestNullScratchRejected() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  MemoryReader reader(record);
  const SdFinalizedHourScanResult result =
      ScanSdFinalizedHourAppendFile(reader, nullptr, kScratchBytes);
  ExpectFailure(result, SdFinalizedHourScanStatus::kReadError,
                SdFinalizedHourScanFailureReason::kReadError, 0U, 0U, 0U);
  CHECK_EQ(reader.read_count, 0U);
}

void TestZeroScratchSizeRejected() {
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  MemoryReader reader(record);
  uint8_t scratch[kScratchBytes] = {};
  const SdFinalizedHourScanResult result =
      ScanSdFinalizedHourAppendFile(reader, scratch, 0U);
  ExpectFailure(result, SdFinalizedHourScanStatus::kReadError,
                SdFinalizedHourScanFailureReason::kReadError, 0U, 0U, 0U);
  CHECK_EQ(reader.read_count, 0U);
}

}  // namespace

void RunTest(const char* name, void (*test_fn)()) {
  const uint32_t failures_before = g_test.failures;
  g_test.current_test = name;
  test_fn();
  if (g_test.failures == failures_before) {
    std::cout << name << ": PASS" << std::endl;
  } else {
    std::cerr << name << ": FAIL" << std::endl;
  }
  g_test.current_test = nullptr;
}

int main() {
  RunTest("TestEmptyFile", TestEmptyFile);
  RunTest("TestOneValidRecord", TestOneValidRecord);
  RunTest("TestMultipleValidRecords", TestMultipleValidRecords);
  RunTest("TestValidRecordsFollowedByCleanEof", TestValidRecordsFollowedByCleanEof);
  RunTest("TestPartialHeaderAtOffsetZero", TestPartialHeaderAtOffsetZero);
  RunTest("TestPartialHeaderAfterOneValidRecord", TestPartialHeaderAfterOneValidRecord);
  RunTest("TestBadMagicAtOffsetZero", TestBadMagicAtOffsetZero);
  RunTest("TestBadMagicAfterOneValidRecord", TestBadMagicAfterOneValidRecord);
  RunTest("TestUnsupportedVersion", TestUnsupportedVersion);
  RunTest("TestBadHeaderSize", TestBadHeaderSize);
  RunTest("TestBadSnapshotFormatVersion", TestBadSnapshotFormatVersion);
  RunTest("TestZeroHourRejected", TestZeroHourRejected);
  RunTest("TestBadHeaderCrc", TestBadHeaderCrc);
  RunTest("TestBadPayloadCrc", TestBadPayloadCrc);
  RunTest("TestPartialPayloadAtOffsetZero", TestPartialPayloadAtOffsetZero);
  RunTest("TestPartialPayloadAfterOneValidRecord", TestPartialPayloadAfterOneValidRecord);
  RunTest("TestRecordBytesTooSmall", TestRecordBytesTooSmall);
  RunTest("TestRecordBytesTooLarge", TestRecordBytesTooLarge);
  RunTest("TestRecordBytesMismatch", TestRecordBytesMismatch);
  RunTest("TestDescriptorBytesMismatch", TestDescriptorBytesMismatch);
  RunTest("TestBadDescriptorEntryBytes", TestBadDescriptorEntryBytes);
  RunTest("TestFrameCountMismatch", TestFrameCountMismatch);
  RunTest("TestFrameBytesMismatch", TestFrameBytesMismatch);
  RunTest("TestActiveSlotCountGreaterThanCapacity", TestActiveSlotCountGreaterThanCapacity);
  RunTest("TestGarbageBytesAfterValidRecord", TestGarbageBytesAfterValidRecord);
  RunTest("TestReadErrorWhileReadingHeader", TestReadErrorWhileReadingHeader);
  RunTest("TestReadErrorWhileReadingPayload", TestReadErrorWhileReadingPayload);
  RunTest("TestCorruptTailOffsetsForBadPayloadCrc", TestCorruptTailOffsetsForBadPayloadCrc);
  RunTest("TestInvalidAtZeroDiffersFromCorruptTail", TestInvalidAtZeroDiffersFromCorruptTail);
  RunTest("TestScannerRejectsInvalidSizesBeforePayloadReads", TestScannerRejectsInvalidSizesBeforePayloadReads);
  RunTest("TestTinyScratchBufferStillStreams", TestTinyScratchBufferStillStreams);
  RunTest("TestNullScratchRejected", TestNullScratchRejected);
  RunTest("TestZeroScratchSizeRejected", TestZeroScratchSizeRejected);

  if (g_test.failures != 0U) {
    std::cerr << "sd_finalized_hour_recovery_test: FAIL ("
              << g_test.failures << " check failures)" << std::endl;
    return 1;
  }
  std::cout << "sd_finalized_hour_recovery_test: PASS" << std::endl;
  return 0;
}
