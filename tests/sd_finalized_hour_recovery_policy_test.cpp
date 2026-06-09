#include "history_crc.h"
#include "history_hour_stager.h"
#include "sd_finalized_hour_block.h"
#include "sd_finalized_hour_recovery.h"
#include "sd_finalized_hour_recovery_policy.h"

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

struct FakeFinalizedHourFileStore {
  class Reader final : public ISdFinalizedHourByteReader {
   public:
    explicit Reader(FakeFinalizedHourFileStore* store) : store_(store) {}

    bool Read(uint64_t offset,
              uint8_t* out,
              size_t len,
              size_t* bytes_read) override {
      if (store_ == nullptr || bytes_read == nullptr) return false;
      *bytes_read = 0U;
      ++store_->read_attempt_count;
      store_->max_requested_len = std::max(store_->max_requested_len, len);
      if (store_->fail_reads && offset <= store_->fail_read_offset &&
          store_->fail_read_offset < offset + len) {
        return false;
      }
      if (out == nullptr && len > 0U) return false;
      if (offset >= store_->bytes.size()) return true;
      const size_t available = store_->bytes.size() - static_cast<size_t>(offset);
      const size_t count = std::min(len, available);
      if (count > 0U) std::memcpy(out, store_->bytes.data() + offset, count);
      *bytes_read = count;
      return true;
    }

   private:
    FakeFinalizedHourFileStore* store_ = nullptr;
  };

  FakeFinalizedHourFileStore() : reader(this) {}

  void SetBytes(const std::vector<uint8_t>& new_bytes) { bytes = new_bytes; }

  void FailReadsAt(uint64_t offset) {
    fail_reads = true;
    fail_read_offset = offset;
  }

  bool FutureWriteTempForRepairOnly(const uint8_t*, size_t) {
    ++write_attempt_count;
    return false;
  }

  bool FutureRemoveForRepairOnly(const char*) {
    ++remove_attempt_count;
    return false;
  }

  bool FutureRenameForRepairOnly(const char*, const char*) {
    ++rename_attempt_count;
    return false;
  }

  bool FutureTruncateForRepairOnly(uint64_t) {
    ++truncate_attempt_count;
    return false;
  }

  void ExpectNoMutation() const {
    CHECK_EQ(write_attempt_count, 0U);
    CHECK_EQ(remove_attempt_count, 0U);
    CHECK_EQ(rename_attempt_count, 0U);
    CHECK_EQ(truncate_attempt_count, 0U);
  }

  std::vector<uint8_t> bytes;
  Reader reader;
  bool fail_reads = false;
  uint64_t fail_read_offset = std::numeric_limits<uint64_t>::max();
  size_t read_attempt_count = 0U;
  size_t max_requested_len = 0U;
  uint32_t write_attempt_count = 0;
  uint32_t remove_attempt_count = 0;
  uint32_t rename_attempt_count = 0;
  uint32_t truncate_attempt_count = 0;
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
  fixture_ok = CHECK_TRUE(stager.RecordSample("2800000000000061", 0x10000002U,
                                               31U, -4.50f, false)) &&
               fixture_ok;

  std::vector<uint8_t> bytes;
  const RamHourStagerFinalizedHourSource source(stager);
  SdFinalizedHourBlockHeader header;
  fixture_ok = CHECK_TRUE(WriteSdFinalizedHourBlock(source, VectorSink, &bytes,
                                                    &header, nullptr)) &&
               fixture_ok;
  fixture_ok = CHECK_TRUE(header.record_bytes == bytes.size()) && fixture_ok;
  fixture_ok = CHECK_TRUE(VerifySdFinalizedHourBlock(bytes.data(), bytes.size())) &&
               fixture_ok;
  if (!fixture_ok) return {};
  return bytes;
}

std::vector<uint8_t> Concat(const std::vector<uint8_t>& a,
                            const std::vector<uint8_t>& b) {
  std::vector<uint8_t> out = a;
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

SdFinalizedHourScanResult ScanStore(FakeFinalizedHourFileStore* store) {
  uint8_t scratch[kScratchBytes] = {};
  return ScanSdFinalizedHourAppendFile(store->reader, scratch, sizeof(scratch));
}

SdFinalizedHourRecoveryDecision ClassifyStore(
    FakeFinalizedHourFileStore* store,
    SdFinalizedHourRecoveryDiagnostics* diagnostics = nullptr) {
  const SdFinalizedHourScanResult scan = ScanStore(store);
  if (diagnostics != nullptr) {
    return ClassifyFinalizedHourAppendSafety(scan, diagnostics);
  }
  return ClassifyFinalizedHourAppendSafety(scan);
}

void ExpectDecisionPropagatesScan(
    const SdFinalizedHourRecoveryDecision& decision,
    const SdFinalizedHourScanResult& scan) {
  CHECK_EQ(decision.scan_status, scan.status);
  CHECK_EQ(decision.failure_reason, scan.first_failure_reason);
  CHECK_EQ(decision.valid_record_count, scan.valid_record_count);
  CHECK_EQ(decision.last_good_offset, scan.last_good_offset);
  CHECK_EQ(decision.first_bad_offset, scan.first_bad_offset);
  CHECK_EQ(decision.expected_next_offset, scan.expected_next_offset);
  CHECK_EQ(decision.last_valid_hour_start_epoch_minute,
           scan.last_valid_hour_start_epoch_minute);
}

void ExpectAllowed(const SdFinalizedHourRecoveryDecision& decision) {
  CHECK_EQ(decision.append_safety, SdFinalizedHourAppendSafety::kAppendAllowed);
  CHECK_EQ(decision.recovery_action,
           SdFinalizedHourRecoveryAction::kNoActionNeeded);
  CHECK_TRUE(decision.append_allowed());
}

void ExpectBlocked(const SdFinalizedHourRecoveryDecision& decision,
                   SdFinalizedHourAppendSafety safety,
                   SdFinalizedHourRecoveryAction action) {
  CHECK_EQ(decision.append_safety, safety);
  CHECK_EQ(decision.recovery_action, action);
  CHECK_TRUE(!decision.append_allowed());
}

void TestCleanEmptyFileAllowsAppend() {
  FakeFinalizedHourFileStore store;
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectAllowed(decision);
  CHECK_EQ(decision.scan_status, SdFinalizedHourScanStatus::kEmpty);
  store.ExpectNoMutation();
}

void TestOneValidRecordAllowsAppend() {
  FakeFinalizedHourFileStore store;
  const std::vector<uint8_t> record = BuildRecord(kHourA);
  store.SetBytes(record);
  const SdFinalizedHourScanResult scan = ScanStore(&store);
  const SdFinalizedHourRecoveryDecision decision =
      ClassifyFinalizedHourAppendSafety(scan);
  ExpectAllowed(decision);
  ExpectDecisionPropagatesScan(decision, scan);
  CHECK_EQ(decision.scan_status, SdFinalizedHourScanStatus::kClean);
  CHECK_EQ(decision.valid_record_count, 1U);
  CHECK_EQ(decision.last_good_offset, static_cast<uint64_t>(record.size()));
  CHECK_EQ(decision.last_valid_hour_start_epoch_minute, kHourA);
  store.ExpectNoMutation();
}

void TestMultipleValidRecordsAllowAppend() {
  FakeFinalizedHourFileStore store;
  const std::vector<uint8_t> bytes = Concat(BuildRecord(kHourA), BuildRecord(kHourB));
  store.SetBytes(bytes);
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectAllowed(decision);
  CHECK_EQ(decision.scan_status, SdFinalizedHourScanStatus::kClean);
  CHECK_EQ(decision.valid_record_count, 2U);
  CHECK_EQ(decision.last_good_offset, static_cast<uint64_t>(bytes.size()));
  CHECK_EQ(decision.last_valid_hour_start_epoch_minute, kHourB);
  store.ExpectNoMutation();
}

void TestCorruptTailAfterValidPrefixBlocksAppend() {
  FakeFinalizedHourFileStore store;
  const std::vector<uint8_t> prefix = BuildRecord(kHourA);
  std::vector<uint8_t> tail = BuildRecord(kHourB);
  WriteU32(&tail, 0U, 0x11111111U);
  RecomputeHeaderCrc(&tail);
  store.SetBytes(Concat(prefix, tail));
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedCorruptTail,
                SdFinalizedHourRecoveryAction::kRepairRequiredButNotAttempted);
  CHECK_EQ(decision.failure_reason, SdFinalizedHourScanFailureReason::kBadMagic);
  CHECK_EQ(decision.valid_record_count, 1U);
  CHECK_EQ(decision.last_good_offset, static_cast<uint64_t>(prefix.size()));
  CHECK_EQ(decision.first_bad_offset, static_cast<uint64_t>(prefix.size()));
  store.ExpectNoMutation();
}

void TestPartialHeaderAfterValidPrefixBlocksAppend() {
  FakeFinalizedHourFileStore store;
  const std::vector<uint8_t> prefix = BuildRecord(kHourA);
  std::vector<uint8_t> tail = BuildRecord(kHourB);
  tail.resize(kSdFinalizedHourHeaderBytes - 1U);
  store.SetBytes(Concat(prefix, tail));
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedCorruptTail,
                SdFinalizedHourRecoveryAction::kRepairRequiredButNotAttempted);
  CHECK_EQ(decision.failure_reason,
           SdFinalizedHourScanFailureReason::kPartialHeader);
  CHECK_EQ(decision.last_good_offset, static_cast<uint64_t>(prefix.size()));
  CHECK_EQ(decision.first_bad_offset, static_cast<uint64_t>(prefix.size()));
  store.ExpectNoMutation();
}

void TestPartialPayloadAfterValidPrefixBlocksAppend() {
  FakeFinalizedHourFileStore store;
  const std::vector<uint8_t> prefix = BuildRecord(kHourA);
  std::vector<uint8_t> tail = BuildRecord(kHourB);
  tail.resize(kSdFinalizedHourHeaderBytes + 3U);
  store.SetBytes(Concat(prefix, tail));
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedCorruptTail,
                SdFinalizedHourRecoveryAction::kRepairRequiredButNotAttempted);
  CHECK_EQ(decision.failure_reason,
           SdFinalizedHourScanFailureReason::kPartialPayload);
  CHECK_EQ(decision.last_good_offset, static_cast<uint64_t>(prefix.size()));
  CHECK_EQ(decision.first_bad_offset, static_cast<uint64_t>(prefix.size()));
  store.ExpectNoMutation();
}

void TestBadPayloadCrcAfterValidPrefixBlocksAppend() {
  FakeFinalizedHourFileStore store;
  const std::vector<uint8_t> prefix = BuildRecord(kHourA);
  std::vector<uint8_t> tail = BuildRecord(kHourB);
  tail[kSdFinalizedHourHeaderBytes] ^= 0x01U;
  store.SetBytes(Concat(prefix, tail));
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedCorruptTail,
                SdFinalizedHourRecoveryAction::kRepairRequiredButNotAttempted);
  CHECK_EQ(decision.failure_reason,
           SdFinalizedHourScanFailureReason::kBadPayloadCrc);
  CHECK_EQ(decision.last_good_offset, static_cast<uint64_t>(prefix.size()));
  CHECK_EQ(decision.first_bad_offset, static_cast<uint64_t>(prefix.size()));
  store.ExpectNoMutation();
}

void TestInvalidAtZeroBlocksAppend() {
  FakeFinalizedHourFileStore store;
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU32(&bytes, 0U, 0x11111111U);
  RecomputeHeaderCrc(&bytes);
  store.SetBytes(bytes);
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedInvalidAtZero,
                SdFinalizedHourRecoveryAction::kManualRecoveryRequired);
  CHECK_EQ(decision.scan_status, SdFinalizedHourScanStatus::kInvalidAtZero);
  CHECK_EQ(decision.valid_record_count, 0U);
  CHECK_EQ(decision.last_good_offset, 0U);
  store.ExpectNoMutation();
}

void TestUnsupportedFormatBlocksAppend() {
  FakeFinalizedHourFileStore store;
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 4U, 0xFFFFU);
  RecomputeHeaderCrc(&bytes);
  store.SetBytes(bytes);
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedUnsupportedFormat,
                SdFinalizedHourRecoveryAction::kManualRecoveryRequired);
  CHECK_EQ(decision.scan_status, SdFinalizedHourScanStatus::kUnsupportedFormat);
  CHECK_EQ(decision.failure_reason,
           SdFinalizedHourScanFailureReason::kUnsupportedVersion);
  store.ExpectNoMutation();
}

void TestBadSnapshotFormatBlocksAppend() {
  FakeFinalizedHourFileStore store;
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU16(&bytes, 40U, 0xFFFFU);
  RecomputeHeaderCrc(&bytes);
  store.SetBytes(bytes);
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedUnsupportedFormat,
                SdFinalizedHourRecoveryAction::kManualRecoveryRequired);
  CHECK_EQ(decision.scan_status, SdFinalizedHourScanStatus::kUnsupportedFormat);
  CHECK_EQ(decision.failure_reason,
           SdFinalizedHourScanFailureReason::kBadSnapshotFormatVersion);
  store.ExpectNoMutation();
}

void TestDangerousHeaderBlocksAppend() {
  FakeFinalizedHourFileStore store;
  std::vector<uint8_t> bytes = BuildRecord(kHourA);
  WriteU32(&bytes, 28U, 0x7FFFFFFFU);
  RecomputeHeaderCrc(&bytes);
  store.SetBytes(bytes);
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedDangerousHeader,
                SdFinalizedHourRecoveryAction::kAppendFault);
  CHECK_EQ(decision.scan_status, SdFinalizedHourScanStatus::kDangerousHeader);
  CHECK_EQ(decision.failure_reason,
           SdFinalizedHourScanFailureReason::kPayloadBytesMismatch);
  CHECK_EQ(store.max_requested_len, kSdFinalizedHourHeaderBytes);
  store.ExpectNoMutation();
}

void TestReadErrorBlocksAppend() {
  FakeFinalizedHourFileStore store;
  const std::vector<uint8_t> bytes = BuildRecord(kHourA);
  store.SetBytes(bytes);
  store.FailReadsAt(0U);
  const SdFinalizedHourRecoveryDecision decision = ClassifyStore(&store);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedReadError,
                SdFinalizedHourRecoveryAction::kAppendFault);
  CHECK_EQ(decision.scan_status, SdFinalizedHourScanStatus::kReadError);
  CHECK_EQ(decision.failure_reason, SdFinalizedHourScanFailureReason::kReadError);
  store.ExpectNoMutation();
}

void TestNullAndZeroScratchScannerFailuresBlockAppend() {
  FakeFinalizedHourFileStore store;
  store.SetBytes(BuildRecord(kHourA));
  uint8_t scratch[kScratchBytes] = {};
  const SdFinalizedHourScanResult null_result =
      ScanSdFinalizedHourAppendFile(store.reader, nullptr, kScratchBytes);
  const SdFinalizedHourRecoveryDecision null_decision =
      ClassifyFinalizedHourAppendSafety(null_result);
  ExpectBlocked(null_decision,
                SdFinalizedHourAppendSafety::kAppendBlockedReadError,
                SdFinalizedHourRecoveryAction::kAppendFault);

  const SdFinalizedHourScanResult zero_result =
      ScanSdFinalizedHourAppendFile(store.reader, scratch, 0U);
  const SdFinalizedHourRecoveryDecision zero_decision =
      ClassifyFinalizedHourAppendSafety(zero_result);
  ExpectBlocked(zero_decision,
                SdFinalizedHourAppendSafety::kAppendBlockedReadError,
                SdFinalizedHourRecoveryAction::kAppendFault);
  store.ExpectNoMutation();
}

void TestDiagnosticsCountersUpdateForAllowedAndBlockedOutcomes() {
  SdFinalizedHourRecoveryDiagnostics diagnostics;

  FakeFinalizedHourFileStore empty_store;
  const SdFinalizedHourRecoveryDecision empty_decision =
      ClassifyStore(&empty_store, &diagnostics);
  ExpectAllowed(empty_decision);
  CHECK_EQ(diagnostics.scans_attempted, 1U);
  CHECK_EQ(diagnostics.scans_empty, 1U);
  CHECK_EQ(diagnostics.append_allowed_count, 1U);
  CHECK_EQ(diagnostics.append_blocked_count, 0U);

  FakeFinalizedHourFileStore clean_store;
  clean_store.SetBytes(BuildRecord(kHourA));
  const SdFinalizedHourRecoveryDecision clean_decision =
      ClassifyStore(&clean_store, &diagnostics);
  ExpectAllowed(clean_decision);
  CHECK_EQ(diagnostics.scans_attempted, 2U);
  CHECK_EQ(diagnostics.scans_clean, 1U);
  CHECK_EQ(diagnostics.append_allowed_count, 2U);

  FakeFinalizedHourFileStore corrupt_store;
  const std::vector<uint8_t> prefix = BuildRecord(kHourA);
  std::vector<uint8_t> tail = BuildRecord(kHourB);
  tail.resize(kSdFinalizedHourHeaderBytes - 1U);
  corrupt_store.SetBytes(Concat(prefix, tail));
  const SdFinalizedHourRecoveryDecision corrupt_decision =
      ClassifyStore(&corrupt_store, &diagnostics);
  ExpectBlocked(corrupt_decision,
                SdFinalizedHourAppendSafety::kAppendBlockedCorruptTail,
                SdFinalizedHourRecoveryAction::kRepairRequiredButNotAttempted);
  CHECK_EQ(diagnostics.scans_attempted, 3U);
  CHECK_EQ(diagnostics.corrupt_tails_detected, 1U);
  CHECK_EQ(diagnostics.append_blocked_count, 1U);
  CHECK_EQ(diagnostics.last_scan_status, corrupt_decision.scan_status);
  CHECK_EQ(diagnostics.last_failure_reason, corrupt_decision.failure_reason);
  CHECK_EQ(diagnostics.last_valid_record_count, corrupt_decision.valid_record_count);
  CHECK_EQ(diagnostics.last_good_offset, corrupt_decision.last_good_offset);
  CHECK_EQ(diagnostics.last_first_bad_offset, corrupt_decision.first_bad_offset);
  CHECK_EQ(diagnostics.last_expected_next_offset,
           corrupt_decision.expected_next_offset);
  CHECK_EQ(diagnostics.last_valid_hour_start_epoch_minute,
           corrupt_decision.last_valid_hour_start_epoch_minute);

  empty_store.ExpectNoMutation();
  clean_store.ExpectNoMutation();
  corrupt_store.ExpectNoMutation();
}

void TestDiagnosticsCountersCoverBlockedStatusKinds() {
  SdFinalizedHourRecoveryDiagnostics diagnostics;

  SdFinalizedHourScanResult invalid;
  invalid.status = SdFinalizedHourScanStatus::kInvalidAtZero;
  invalid.first_failure_reason = SdFinalizedHourScanFailureReason::kBadMagic;
  ClassifyFinalizedHourAppendSafety(invalid, &diagnostics);

  SdFinalizedHourScanResult read_error;
  read_error.status = SdFinalizedHourScanStatus::kReadError;
  read_error.first_failure_reason = SdFinalizedHourScanFailureReason::kReadError;
  ClassifyFinalizedHourAppendSafety(read_error, &diagnostics);

  SdFinalizedHourScanResult unsupported;
  unsupported.status = SdFinalizedHourScanStatus::kUnsupportedFormat;
  unsupported.first_failure_reason =
      SdFinalizedHourScanFailureReason::kUnsupportedVersion;
  ClassifyFinalizedHourAppendSafety(unsupported, &diagnostics);

  SdFinalizedHourScanResult dangerous;
  dangerous.status = SdFinalizedHourScanStatus::kDangerousHeader;
  dangerous.first_failure_reason =
      SdFinalizedHourScanFailureReason::kRecordBytesTooLarge;
  ClassifyFinalizedHourAppendSafety(dangerous, &diagnostics);

  CHECK_EQ(diagnostics.scans_attempted, 4U);
  CHECK_EQ(diagnostics.invalid_at_zero_detected, 1U);
  CHECK_EQ(diagnostics.read_errors_detected, 1U);
  CHECK_EQ(diagnostics.unsupported_format_detected, 1U);
  CHECK_EQ(diagnostics.dangerous_header_detected, 1U);
  CHECK_EQ(diagnostics.append_allowed_count, 0U);
  CHECK_EQ(diagnostics.append_blocked_count, 4U);
}

void TestUnknownStatusBlocksAppend() {
  SdFinalizedHourScanResult scan;
  scan.status = static_cast<SdFinalizedHourScanStatus>(255);
  scan.first_failure_reason = SdFinalizedHourScanFailureReason::kNone;
  const SdFinalizedHourRecoveryDecision decision =
      ClassifyFinalizedHourAppendSafety(scan);
  ExpectBlocked(decision,
                SdFinalizedHourAppendSafety::kAppendBlockedUnknown,
                SdFinalizedHourRecoveryAction::kAppendFault);
  CHECK_EQ(decision.scan_status, scan.status);
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
  RunTest("TestCleanEmptyFileAllowsAppend", TestCleanEmptyFileAllowsAppend);
  RunTest("TestOneValidRecordAllowsAppend", TestOneValidRecordAllowsAppend);
  RunTest("TestMultipleValidRecordsAllowAppend", TestMultipleValidRecordsAllowAppend);
  RunTest("TestCorruptTailAfterValidPrefixBlocksAppend",
          TestCorruptTailAfterValidPrefixBlocksAppend);
  RunTest("TestPartialHeaderAfterValidPrefixBlocksAppend",
          TestPartialHeaderAfterValidPrefixBlocksAppend);
  RunTest("TestPartialPayloadAfterValidPrefixBlocksAppend",
          TestPartialPayloadAfterValidPrefixBlocksAppend);
  RunTest("TestBadPayloadCrcAfterValidPrefixBlocksAppend",
          TestBadPayloadCrcAfterValidPrefixBlocksAppend);
  RunTest("TestInvalidAtZeroBlocksAppend", TestInvalidAtZeroBlocksAppend);
  RunTest("TestUnsupportedFormatBlocksAppend", TestUnsupportedFormatBlocksAppend);
  RunTest("TestBadSnapshotFormatBlocksAppend", TestBadSnapshotFormatBlocksAppend);
  RunTest("TestDangerousHeaderBlocksAppend", TestDangerousHeaderBlocksAppend);
  RunTest("TestReadErrorBlocksAppend", TestReadErrorBlocksAppend);
  RunTest("TestNullAndZeroScratchScannerFailuresBlockAppend",
          TestNullAndZeroScratchScannerFailuresBlockAppend);
  RunTest("TestDiagnosticsCountersUpdateForAllowedAndBlockedOutcomes",
          TestDiagnosticsCountersUpdateForAllowedAndBlockedOutcomes);
  RunTest("TestDiagnosticsCountersCoverBlockedStatusKinds",
          TestDiagnosticsCountersCoverBlockedStatusKinds);
  RunTest("TestUnknownStatusBlocksAppend", TestUnknownStatusBlocksAppend);

  if (g_test.failures != 0U) {
    std::cerr << "sd_finalized_hour_recovery_policy_test: FAIL ("
              << g_test.failures << " check failures)" << std::endl;
    return 1;
  }
  std::cout << "sd_finalized_hour_recovery_policy_test: PASS" << std::endl;
  return 0;
}
