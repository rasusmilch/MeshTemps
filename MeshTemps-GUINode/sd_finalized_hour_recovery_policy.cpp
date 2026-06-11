#include "sd_finalized_hour_recovery_policy.h"

namespace {

SdFinalizedHourRecoveryDecision MakeBaseDecision(
    const SdFinalizedHourScanResult& scan) {
  SdFinalizedHourRecoveryDecision decision;
  decision.scan_status = scan.status;
  decision.failure_reason = scan.first_failure_reason;
  decision.valid_record_count = scan.valid_record_count;
  decision.last_good_offset = scan.last_good_offset;
  decision.first_bad_offset = scan.first_bad_offset;
  decision.expected_next_offset = scan.expected_next_offset;
  decision.last_valid_hour_start_epoch_minute =
      scan.last_valid_hour_start_epoch_minute;
  return decision;
}

}  // namespace

SdFinalizedHourRecoveryDecision ClassifyFinalizedHourAppendSafety(
    const SdFinalizedHourScanResult& scan) {
  SdFinalizedHourRecoveryDecision decision = MakeBaseDecision(scan);

  switch (scan.status) {
    case SdFinalizedHourScanStatus::kClean:
    case SdFinalizedHourScanStatus::kEmpty:
      decision.append_safety = SdFinalizedHourAppendSafety::kAppendAllowed;
      decision.recovery_action = SdFinalizedHourRecoveryAction::kNoActionNeeded;
      break;
    case SdFinalizedHourScanStatus::kCorruptTail:
      decision.append_safety =
          SdFinalizedHourAppendSafety::kAppendBlockedCorruptTail;
      decision.recovery_action =
          SdFinalizedHourRecoveryAction::kRepairRequiredButNotAttempted;
      break;
    case SdFinalizedHourScanStatus::kInvalidAtZero:
      decision.append_safety =
          SdFinalizedHourAppendSafety::kAppendBlockedInvalidAtZero;
      decision.recovery_action =
          SdFinalizedHourRecoveryAction::kManualRecoveryRequired;
      break;
    case SdFinalizedHourScanStatus::kReadError:
      decision.append_safety = SdFinalizedHourAppendSafety::kAppendBlockedReadError;
      decision.recovery_action = SdFinalizedHourRecoveryAction::kAppendFault;
      break;
    case SdFinalizedHourScanStatus::kUnsupportedFormat:
      decision.append_safety =
          SdFinalizedHourAppendSafety::kAppendBlockedUnsupportedFormat;
      decision.recovery_action =
          SdFinalizedHourRecoveryAction::kManualRecoveryRequired;
      break;
    case SdFinalizedHourScanStatus::kDangerousHeader:
      decision.append_safety =
          SdFinalizedHourAppendSafety::kAppendBlockedDangerousHeader;
      decision.recovery_action = SdFinalizedHourRecoveryAction::kAppendFault;
      break;
    default:
      decision.append_safety = SdFinalizedHourAppendSafety::kAppendBlockedUnknown;
      decision.recovery_action = SdFinalizedHourRecoveryAction::kAppendFault;
      break;
  }

  return decision;
}

SdFinalizedHourRecoveryDecision ClassifyFinalizedHourAppendSafety(
    const SdFinalizedHourScanResult& scan,
    SdFinalizedHourRecoveryDiagnostics* diagnostics) {
  const SdFinalizedHourRecoveryDecision decision =
      ClassifyFinalizedHourAppendSafety(scan);
  UpdateSdFinalizedHourRecoveryDiagnostics(decision, diagnostics);
  return decision;
}

void UpdateSdFinalizedHourRecoveryDiagnostics(
    const SdFinalizedHourRecoveryDecision& decision,
    SdFinalizedHourRecoveryDiagnostics* diagnostics) {
  if (diagnostics == nullptr) return;

  ++diagnostics->scans_attempted;
  diagnostics->last_scan_status = decision.scan_status;
  diagnostics->last_failure_reason = decision.failure_reason;
  diagnostics->last_valid_record_count = decision.valid_record_count;
  diagnostics->last_good_offset = decision.last_good_offset;
  diagnostics->last_first_bad_offset = decision.first_bad_offset;
  diagnostics->last_expected_next_offset = decision.expected_next_offset;
  diagnostics->last_valid_hour_start_epoch_minute =
      decision.last_valid_hour_start_epoch_minute;

  switch (decision.scan_status) {
    case SdFinalizedHourScanStatus::kClean:
      ++diagnostics->scans_clean;
      break;
    case SdFinalizedHourScanStatus::kEmpty:
      ++diagnostics->scans_empty;
      break;
    case SdFinalizedHourScanStatus::kCorruptTail:
      ++diagnostics->corrupt_tails_detected;
      break;
    case SdFinalizedHourScanStatus::kInvalidAtZero:
      ++diagnostics->invalid_at_zero_detected;
      break;
    case SdFinalizedHourScanStatus::kReadError:
      ++diagnostics->read_errors_detected;
      break;
    case SdFinalizedHourScanStatus::kUnsupportedFormat:
      ++diagnostics->unsupported_format_detected;
      break;
    case SdFinalizedHourScanStatus::kDangerousHeader:
      ++diagnostics->dangerous_header_detected;
      break;
    default:
      break;
  }

  if (decision.append_allowed()) {
    ++diagnostics->append_allowed_count;
  } else {
    ++diagnostics->append_blocked_count;
  }
}
