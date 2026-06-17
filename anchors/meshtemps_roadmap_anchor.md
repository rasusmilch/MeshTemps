# MeshTemps Roadmap Anchor

Project: MeshTemps
Workstream: GUI-node history storage, SD archive, recovery, and chart hardening
Anchor purpose: Keep future ChatGPT/Codex tasks sequenced, scoped, and aligned with current product intent.
Status: Committed repository roadmap after PR #55 merge and finalized-hour v2 intent clarification.
Last updated: 2026-06-11

## 1. Current project/workstream objective

Replace MeshTemps GUI-node retention-scaled RAM history vectors with a bounded current-hour staging pipeline and a durable SD finalized-hour archive.

Current target architecture:

```text
live mesh sensor state
  -> periodic snapshot / HistoryAggregator
  -> backend-agnostic current-hour stager
       -> RamHourStager first
       -> optional FramHourStager later if explicitly approved
  -> sensor-major finalized-hour SD archive
  -> validated SD reader/query/reduction layer
  -> chart UI
```

The objective is to stop history-related ESP32 panics/freezes while preserving multi-day/multi-week chart capability and maintaining enough historical context to decode old SD files years later.

## 2. Current active feature/fix

Active feature/fix:

**Task 10 series — RAM-first current-hour staging plus durable SD finalized-hour archive**

Current product decisions:

- RAM-first current-hour staging is the immediate implementation target.
- FRAM remains optional later hardware behind the same stager interface.
- Current-hour staging may remain slot/minute-major internally and capped at 64 active sensors.
- Finalized SD archive must move to sensor-major v2 records.
- Finalized SD records must not store durable slot IDs.
- ROM64 is the canonical sensor identity.
- Node ID is reporting provenance/context only.
- Node label and sensor label are historical context snapshots and should be stored in each finalized sensor block.
- `addr16` is derived from ROM64 for display/debug and should not be stored in finalized SD records unless a later task proves a need.
- Day files shall begin with a bounded ASCII reverse-engineering preamble/schema guide before binary records.
- Binary parsing must rely on explicit lengths, counts, offsets, magic/sentinel values, versions, flags, and CRCs, not prose or sentinel scanning.
- Raw minute-level finalized-hour records are authoritative. Rollups/indexes, if added, are derived and rebuildable only.
- Production finalized-hour path construction, serialization, append, and verification must remain bounded and heap-free.

## 3. MVP / immediate tasks

### Completed foundation already merged

These tasks/PRs are completed as foundation but remain subject to integrated validation after the v2 format change:

- Task 10B/10C storage foundation, RamHourStager, finalized-hour writer path, bounded path building, static write/verify buffers, and legacy storage cleanup from the PR #53 workstream.
- Task 10C-F1 / PR #54 read-only finalized-hour append-file scanner.
- Task 10C-F2-A / PR #55 non-destructive recovery policy seam and append-safety classifier.

Important caveat: the merged writer/scanner/policy work was built around the existing v1 slot/minute-major finalized-hour format. It is useful architecture and test infrastructure, but it is not the final SD binary layout.

### Task 10C-FMT0 — Plan finalized-hour v2 sensor-major ROM64 archive format

Type: read-only planning/spec task.
Risk: high.
PR/branch: may be a doc-only/anchor update; if Codex opens a PR, use a focused draft PR branch and do not mix implementation.
Checkpoint: required before any mutating recovery or runtime integration work.

Scope:

- Inspect current source and anchors.
- Define finalized-hour v2 binary field layout.
- Define day-file ASCII preamble/schema guide including field names, types, byte lengths, global endian, CRC, string, and temperature encoding rules.
- Define `HourRecordV2`, `SensorIndexEntryV2`, `SensorBlockHeaderV2`, `SensorDescriptorV2`, and `SensorPayloadV2`.
- Define ROM64+offset sensor index table.
- Define label bounds and truncation/rejection behavior.
- Define block magic/sentinel as validation aid only.
- Define CRC coverage and failure policy.
- Define how current slot/minute-major staging is converted to sensor-major SD output.
- Define changes needed to writer, scanner, policy tests, recovery anchors, requirements anchors, and roadmap/current-next-action anchors.

Explicit exclusions:

- No code implementation.
- No repair/truncate/quarantine.
- No runtime aggregator.
- No chart/query migration.
- No FRAM.
- No hardware claims.

Acceptance:

- Produces an approved concise spec and task breakdown.
- Confirms no v1 compatibility is required: no production v1 data, no migration, no dual-format support.
- Identifies all stale v1-slot terminology that later tasks must update.

### Task 10C-FMT1 — Implement finalized-hour v2 writer/scanner/tests

Type: focused execute after 10C-FMT0 approval.
Risk: high.
Status: writer/store append work merged in PR #58; scanner-stack work in PR #59 is accepted through local integrated C-V validation with source-identity, firmware-build, full-CI, hardware-SD/FAT, and power-loss caveats.
PR/branch: PR #59 remains draft until user-side title/body cleanup and ready-for-review decision.
Checkpoint: required before resuming 10C-F2-B/C recovery work.

Scope:

- Replace or version the finalized-hour writer with v2 sensor-major output.
- Add the day-file preamble writer for newly created day files.
- Add v2 field-by-field little-endian serialization; do not serialize raw structs.
- Emit one sensor block per ROM64 seen at least once during the hour.
- Store ROM64, last-known node ID, node label, sensor label, first/last seen minute, counts, presence/corrected bitmaps, and 60 int16 centi-C samples.
- Add ROM64+offset sensor index entries.
- Add block magic/sentinel and explicit `block_bytes`.
- Add header, payload, and block CRC validation as specified.
- Update scanner to validate v2 records and day-file binary-start offset.
- Add a read-only File/FS scanner seam in `SdHistoryStore` without recovery or append guard.
- Update tests for clean records, corrupt records, duplicate ROMs, bad offsets, bad lengths, label bounds, preamble schema drift, CRC failures, dangerous sizes, and read-only File/FS scan behavior.
- Preserve bounded/static-buffer SD append/verify/scanner constraints.

Accepted local PR #59 scanner-stack contents: pure finalized-hour v2 byte-reader scanner, expanded scanner corruption/failure coverage, neutral `history_storage_limits.h` product/domain limit owner, deterministic preamble marker placement, read-only `SdHistoryStore::ScanFinalizedHourFile` seam, scanner/store host tests, and preserved store append regression.

Explicit exclusions:

- No repair/truncate/quarantine behavior beyond scanner classification.
- No runtime aggregator integration.
- No chart/query migration.
- No FRAM.
- No retention/pruning.

### Task 10C-FMTV — Targeted validation of finalized-hour v2 format

Type: broader validation task after PR #59 cleanup/merge.
Risk: high.
PR/branch: follow the post-PR #59 validation branch/PR chosen by the user.
Checkpoint: must pass before 10C-F2-B/C.

Validate:

- Current PR head, branch, and changed files.
- v2 format matches approved spec.
- No durable slot ID or stored `addr16` in finalized SD records.
- ROM64 is the SD identity; labels/node ID are context.
- Preamble includes field names/types/byte lengths and parser guidance.
- Binary parsing uses lengths/counts/offsets/CRCs, not sentinel scanning.
- Writer remains bounded/heap-free in production paths.
- Scanner preserves valid-prefix/first-unsafe knowledge after corrupt tail without mutating files.
- Read-only File/FS scanner seam does not create, append, repair, truncate, quarantine, remove, rename, or mutate files.
- Host tests run normally and with assertions/checks not disabled where relevant.

## 4. Stabilization tasks

### Task 10C-F2-B — Implement approved repair/quarantine/fault behavior for v2

Type: focused execute after 10C-FMTV.
Risk: high.
Requires read-only planning first if 10C-FMT0 does not already settle truncate/copy/replace/fault policy.
PR/branch: may share one draft PR branch with 10C-F2-C if kept draft and gated; otherwise split if large.

Scope:

- Implement only the approved recovery policy.
- Prefer truncate only if exact target FS support is verified.
- Otherwise use copy/replace/quarantine or explicit fault-only append blocking.
- Preserve valid prefixes.
- Never delete or overwrite the only valid historical copy without validated replacement.
- Add idempotent boot/retry behavior if mutating repair is implemented.
- Add diagnostics counters/status.

Exclusions:

- No chart/query migration.
- No runtime aggregator unless specifically deferred into 10C-F2-C.
- No FRAM.
- No broad GUI changes.

### Task 10C-F2-C — Runtime append guard / recovery integration

Type: focused execute after 10C-F2-B validation or same draft PR branch gated after B is correct.
Risk: high.
Checkpoint: required before 10D.

Scope:

- Integrate v2 scanner/policy/repair or fault behavior with `SdHistoryStore` append/open path.
- Prevent appending to any known-corrupt day file.
- Decide and implement whether recovery runs at begin/open, lazily before append, or both.
- Expose append-blocked diagnostics.
- Keep runtime SD finalization disabled/guarded until recovery behavior is validated.

### Task 10C-F2V — Recovery validation for v2

Type: targeted validation.
Risk: high.
Required before normal runtime SD finalization.

Validate:

- Clean/empty day files allow append.
- Corrupt tail blocks append until repaired/faulted by approved behavior.
- Invalid-at-zero, unsupported format, dangerous header/size, bad CRC, bad index, bad block bytes, and read errors are handled deterministically.
- Valid prefix is preserved.
- Interrupted repair is idempotent if mutating repair exists.
- Hardware power-loss validation remains explicitly unclaimed unless actually run.

## 5. Runtime, pilot, and validation tasks

### Task 10D — Add runtime HistoryAggregator snapshot path

Type: focused execute after 10C-F2V.
Risk: high.
PR/branch: separate draft PR branch from format/recovery unless user explicitly approves combining.
Checkpoint: required before chart/query migration.

Scope:

- Snapshot live mesh sensor state on cadence.
- Feed bounded current-hour stager.
- Preserve immediate mid-hour sensor discovery.
- Rollover completed hours to v2 SD finalization path only after recovery/append guard is active.
- Do not stack-allocate large `HistoryHourSnapshot` in callbacks/loops/small tasks/LVGL/mesh callbacks.
- Do not hold mesh locks across SD I/O.
- Do not resurrect deleted stale aggregator/storage APIs.

### Task 10D-V — Runtime aggregation validation

Type: targeted validation.
Risk: high.

Validate:

- Rollover timing.
- Missing/corrected bitmap behavior.
- Labels captured at record time.
- Sensor move by ROM64 keeps continuity while node context updates.
- SD absent/unavailable behavior.
- No UI callback SD writes.
- No large heap/stack regression.

### Task 10E — Pilot/hardware validation of SD finalization and recovery

Type: pilot/validation task.
Risk: high.

Scope:

- Run target hardware tests after host validation.
- Validate SD mount/write/flush/close/read-back.
- Validate reset during staging, reset during append, corrupt-tail injection, boot recovery, append-blocking, and diagnostics.
- Document exact board, SD card, firmware commit, serial logs, and test results.

## 6. Expansion / future tasks

### Task 10F — SD reader/query/reduction service

Type: focused execute after 10D/D-V and enough v2 records exist for tests.
Risk: high.

Scope:

- Stream validated v2 day files by record offset.
- Query by ROM64 and historical labels.
- Reduce ranges for charting without full-history heap vectors.
- Treat rollups/indexes only as optional rebuildable acceleration.

### Task 10G — Chart migration to SD-backed query path

Type: focused execute after 10F validation.
Risk: high.

Scope:

- Move range buttons/history chart away from full RAM vector copies.
- Keep LVGL callbacks responsive.
- Display gaps for missing data.
- Preserve existing user-facing chart behavior where practical.

### Task 10H — Operator diagnostics and maintenance UI

Type: focused execute after recovery/runtime basics.
Risk: medium/high.

Scope:

- Serial-first diagnostics for SD archive, recovery, append-blocked status, current-hour state, and record counts.
- GUI diagnostics only if explicitly scoped.
- Guard destructive commands with explicit confirmation syntax.

### Task 10I — Retention/pruning policy

Type: read-only planning before execute.
Risk: medium/high.

Scope:

- Define retention policy for raw v2 day files.
- Preserve auditability and safe deletion rules.
- Do not prune until validated archive/recovery behavior exists.

### Task 10J — Optional FRAM backend

Type: read-only hardware/design planning first.
Risk: high.

Scope:

- Only after RAM-first SD archive works.
- Verify actual FRAM module, size, address, wiring, I2C bus behavior, and recovery model.
- Implement behind same current-hour stager interface.
- FRAM is current-hour recovery only, not long-term history.

## 7. Explicitly deferred tasks

Deferred until v2 format, recovery, runtime, and reader/query foundations are validated:

- FRAM backend.
- SD rollups/indexes.
- Retention/pruning.
- GUI diagnostics screen.
- Broad allocation audit outside finalized-hour/runtime history path.
- Full chart redesign or LVGL styling changes.
- Network-exposed storage mutation APIs.
- Partial per-sensor salvage from a record with failed whole-record CRC.
- FEC/error-correction coding for SD records.

## 8. Explicitly rejected or deprecated paths

Rejected/deprecated:

- Treating v1 fixed 64-slot minute-frame SD format as final.
- Storing durable `slot_id` in finalized SD records.
- Storing `addr16` redundantly beside ROM64 by default.
- Using node ID as the sensor identity.
- Using labels/ranks as identity keys.
- Storing every mesh packet directly to SD.
- Making rollups the only retained history.
- Building complete finalized-hour records in heap vectors for production write/verify.
- Reading complete finalized-hour records into heap vectors for production verification.
- Using Arduino `String`, `std::string`, printf-family formatting, or libc calendar conversion in finalized-hour low-level path construction.
- Using sentinel hunting as the normal parser boundary.
- Holding mesh/history locks across SD I/O.
- Moving to Task 10D runtime SD finalization before v2 format and append recovery/guard are validated.

## 9. Task dependencies and sequencing

Current required sequence:

```text
10C-FMT0 read-only v2 format plan/spec
  -> 10C-FMT1 v2 writer/scanner/preamble/tests
  -> 10C-FMTV v2 validation
  -> 10C-F2-B approved v2 repair/quarantine/fault implementation
  -> 10C-F2-C runtime append guard/recovery integration
  -> 10C-F2V v2 recovery validation
  -> 10D runtime HistoryAggregator snapshot path
  -> 10D-V runtime aggregation validation
  -> 10E hardware/pilot validation
  -> 10F SD reader/query/reduction service
  -> 10G chart migration
```

Do not skip 10C-FMT0. Do not build mutating recovery on v1 unless the user explicitly reverses the v2 direction.

## 10. Draft PR branch guidance

Recommended branch/PR grouping:

- 10C-FMT0: no implementation PR required unless anchor/spec files are changed; if changed, use a small doc/spec PR or direct anchor commit as explicitly requested.
- 10C-FMT1/PR #59: keep draft until PR title/body cleanup and ready-for-review decision; run broader 10C-FMTV after PR #59 cleanup/merge before recovery/append guard.
- 10C-F2-B + 10C-F2-C + 10C-F2V: one draft PR branch is acceptable if kept gated and not marked ready until validation passes; split if repair policy grows large.
- 10D + 10D-V: separate draft PR branch.
- 10F reader/query: separate draft PR branch.
- 10G chart migration: separate draft PR branch.
- FRAM/retention/diagnostics expansion: separate PRs unless explicitly approved.

Codex may check out a PR into a local branch named `work`; receipts must report the actual remote branch, PR, and head SHA, not just the local checkout name.

## 11. Tasks requiring read-only planning first

Read-only planning/spec required before execution:

- 10C-FMT0 v2 finalized-hour/day-file format.
- Any repair policy not fully settled by 10C-FMT0, especially truncate vs copy/replace/quarantine vs fault-only.
- 10I retention/pruning.
- 10J FRAM backend.
- Any destructive storage command or automated repair policy not already approved.
- Any major chart UX redesign beyond storage-backed migration.

## 12. Tasks that can be focused execute tasks

Focused execute tasks after their prerequisites:

- 10C-FMT1-A after 10C-FMT0-A resolves/approves the v2 on-disk format decision table.
- 10C-F2-B after v2 validation and repair policy approval.
- 10C-F2-C after 10C-F2-B validation or within gated same draft PR.
- 10D after 10C-F2V.
- 10F after 10D/D-V.
- 10G after 10F validation.
- 10H serial diagnostics after runtime/recovery basics are stable.

## 13. High-risk tasks requiring checkpoint validation

High-risk checkpoint validations are required for:

- 10C-FMT0 review approval plus 10C-FMT0-A on-disk format decision-table approval before implementation.
- 10C-FMT1-A-V / 10C-FMT1-B-V / 10C-FMT1-C-V checkpoint validations as the v2 format work is split; PR #59 scanner stack is accepted through local C-V with caveats.
- 10C-FMTV v2 writer/scanner/preamble validation.
- 10C-F2V recovery validation.
- 10D-V runtime aggregation validation.
- 10E hardware/pilot validation.
- 10F reader/query validation.
- 10G chart migration validation.
- Any FRAM backend validation.
- Any destructive repair/prune/format command validation.

## 14. Completed tasks still needing integrated validation

Completed/merged work still needs broader integrated validation after PR #59 cleanup/merge:

- Finalized-hour v2 format, writer, scanner, preamble, path builder, coalescer, and read-only File/FS scan seam should be validated together in 10C-FMTV before recovery/append guard.
- PR #54/#55 v1 scanner/recovery-policy architecture remains useful background only; v1 field names, active slot count, descriptor bytes, frame bytes, and fixed 64-slot frame assumptions must not be cited as final v2 confidence.
- Existing host tests that rely on v1 field names, active slot count, descriptor bytes, frame bytes, or fixed 64-slot frames must be updated before being cited as final confidence.
- Anchor set should be kept aligned by the current 10C-FMT1-D cleanup and any later PR-readiness notes.

## 15. Current next required action

Current next required action:

```text
Task 10C-FMT1-D — scanner-stack anchor and PR-readiness cleanup after C-V
```

After 10C-FMT1-D, PR #59 still needs user-side PR title/body cleanup and the user's ready-for-review decision. After PR #59 cleanup/merge, run broader 10C-FMTV validation of the v2 format/writer/scanner/preamble/store-scan path before recovery/append guard or runtime aggregation.

10C-FMTV must validate the v2 format/writer/scanner/preamble path before 10C-F2-B repair/quarantine/fault implementation, 10C-F2-C runtime append guard, or 10D runtime aggregator.

## 16. Last updated context

Inspected current source/context for this roadmap update:

- No current local repository snapshot was available in the active environment. GitHub was used as source of truth.
- GitHub repository: `rasusmilch/MeshTemps`.
- Branch inspected and updated: `feature/ram-backed-sd-hist`.
- Current branch head before this roadmap update: `eb70f16b3d95e63f5a5771fa4a8c79c895f849f4`.
- PR #55 inspected: merged, title `Add non-destructive finalized-hour recovery policy and diagnostics with tests`, head `32f70f41739e093fe3df77789d646cb3f8028b37`, base `d455a2e21cc8777c7d355eee0332157e7e2d55f7`, merge commit `6bafc4e34b2ba2349c1b828261d5b5573c20ebfb`.
- Source files inspected:
  - `MeshTemps-GUINode/history_hour_stager.h`
  - `MeshTemps-GUINode/sd_finalized_hour_block.h`
  - `MeshTemps-GUINode/sd_finalized_hour_recovery_policy.h`
- Anchor files inspected:
  - `anchors/README.md`
  - `anchors/meshtemps_current_next_action_anchor.md`
  - `anchors/meshtemps_project_intent_anchor.md`
  - `anchors/meshtemps_roadmap_anchor.md`
  - `anchors/meshtemps_requirements_constraints_anchor.md`
  - `anchors/meshtemps_sd_durability_recovery_anchor.md`
- Prior uploaded/context references used in summarized form:
  - MeshTemps history storage handoff dated 2026-06-07.
  - PR #54 scanner work and PR #55 recovery-policy work as represented by current GitHub PR metadata and prior receipts.
  - Current user clarification in this chat: no slot ID in finalized SD records; ROM64-only sensor identity; include node/sensor label snapshots; include ROM64+offset sensor index table; include block bytes; include block magic/sentinel only as additional safeguard; include day-file ASCII schema preamble with field names, field types, byte lengths, and parser guidance.
- Expected but missing/unverified:
  - No current local snapshot was inspected.
  - No compile/tests were run for this anchor-only update.
  - No hardware validation was performed.
  - Decision log and validation ledger anchors were not found during the prior project-intent anchor search.


### PR #57 R2 gate before 10C-FMT1-A-V

Before 10C-FMT1-A-V checkpoint validation, PR #57 needs a focused R2 code revision to remove `reserved0`/fake padding from finalized-hour v2, correct SensorBlockHeaderV2 to 32 bytes, SensorDescriptorV2 to 106 bytes, fixed SensorBlockV2 to 274 bytes, block CRC offset to 24, descriptor_flags offset to 22, remove the `history_hour_stager.h` dependency from the v2 format module, and use a v2-owned invalid-sample sentinel without creating a shared header solely for that value.
