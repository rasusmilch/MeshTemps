#ifndef SD_FINALIZED_HOUR_RECOVERY_POLICY_H_
#define SD_FINALIZED_HOUR_RECOVERY_POLICY_H_

#include <cstdint>

#include "sd_finalized_hour_recovery.h"

// Non-destructive finalized-hour append-safety policy.
//
// This module only classifies scanner results and records decision diagnostics.
// It does not repair, truncate, remove, rename, quarantine, copy/replace, append,
// or depend on Arduino File/FS APIs.

enum class SdFinalizedHourAppendSafety {
  kAppendAllowed,
  kAppendBlockedCorruptTail,
  kAppendBlockedInvalidAtZero,
  kAppendBlockedReadError,
  kAppendBlockedUnsupportedFormat,
  kAppendBlockedDangerousHeader,
  kAppendBlockedUnknown,
};

enum class SdFinalizedHourRecoveryAction {
  kNoActionNeeded,
  kRepairRequiredButNotAttempted,
  kManualRecoveryRequired,
  kAppendFault,
};

struct SdFinalizedHourRecoveryDecision {
  SdFinalizedHourAppendSafety append_safety =
      SdFinalizedHourAppendSafety::kAppendBlockedUnknown;
  SdFinalizedHourRecoveryAction recovery_action =
      SdFinalizedHourRecoveryAction::kAppendFault;
  SdFinalizedHourScanStatus scan_status = SdFinalizedHourScanStatus::kReadError;
  SdFinalizedHourScanFailureReason failure_reason =
      SdFinalizedHourScanFailureReason::kReadError;
  uint32_t valid_record_count = 0;
  uint64_t last_good_offset = 0;
  uint64_t first_bad_offset = 0;
  uint64_t expected_next_offset = 0;
  uint32_t last_valid_hour_start_epoch_minute = 0;

  bool append_allowed() const {
    return append_safety == SdFinalizedHourAppendSafety::kAppendAllowed;
  }
};

struct SdFinalizedHourRecoveryDiagnostics {
  uint32_t scans_attempted = 0;
  uint32_t scans_clean = 0;
  uint32_t scans_empty = 0;
  uint32_t corrupt_tails_detected = 0;
  uint32_t invalid_at_zero_detected = 0;
  uint32_t read_errors_detected = 0;
  uint32_t unsupported_format_detected = 0;
  uint32_t dangerous_header_detected = 0;
  uint32_t append_allowed_count = 0;
  uint32_t append_blocked_count = 0;
  SdFinalizedHourScanStatus last_scan_status = SdFinalizedHourScanStatus::kReadError;
  SdFinalizedHourScanFailureReason last_failure_reason =
      SdFinalizedHourScanFailureReason::kReadError;
  uint32_t last_valid_record_count = 0;
  uint64_t last_good_offset = 0;
  uint64_t last_first_bad_offset = 0;
  uint64_t last_expected_next_offset = 0;
  uint32_t last_valid_hour_start_epoch_minute = 0;
};

SdFinalizedHourRecoveryDecision ClassifyFinalizedHourAppendSafety(
    const SdFinalizedHourScanResult& scan);

SdFinalizedHourRecoveryDecision ClassifyFinalizedHourAppendSafety(
    const SdFinalizedHourScanResult& scan,
    SdFinalizedHourRecoveryDiagnostics* diagnostics);

void UpdateSdFinalizedHourRecoveryDiagnostics(
    const SdFinalizedHourRecoveryDecision& decision,
    SdFinalizedHourRecoveryDiagnostics* diagnostics);

#endif  // SD_FINALIZED_HOUR_RECOVERY_POLICY_H_
