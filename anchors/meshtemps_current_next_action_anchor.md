# MeshTemps Current Next Action Anchor

Project: MeshTemps
Workstream: GUI-node history storage and chart hardening
Anchor purpose: Current near-term sequencing for MeshTemps storage/history tasks.
Status: PR #59 has merged; current next action is Task 10C-FMTV integrated finalized-hour v2 validation before 10C-F2-B/C recovery/append-guard planning.
Last updated: 2026-06-19

## Authority

This file is a short current-action pointer. It must agree with:

- `anchors/meshtemps_project_intent_anchor.md`
- `anchors/meshtemps_roadmap_anchor.md`
- `anchors/meshtemps_requirements_constraints_anchor.md`
- `anchors/meshtemps_decision_log_anchor.md`

It no longer supersedes the updated roadmap, requirements, or decision log. If this anchor conflicts with those anchors, stop and replan rather than executing stale recovery/runtime work.

`anchors/meshtemps_testing_hardening_anchor.md` remains the workflow authority for test quality. Future tasks must harden weak tests they add, touch, rely on, or cite as validation evidence. Existing tests are not grandfathered as sufficient.

## Current next action

Current next action:

```text
Task 10C-FMTV — integrated validation of finalized-hour v2 format/writer/scanner/preamble/store-scan path after PR #59 merge
```

PR #57 has merged into `feature/ram-backed-sd-hist` and completed the pure finalized-hour v2 format/schema/preamble skeleton. PR #58 has merged into `feature/ram-backed-sd-hist` and completed the finalized-hour v2 writer plus `SdHistoryStore` append/day-file behavior without enabling normal runtime SD finalization.

PR #59 has merged into `feature/ram-backed-sd-hist`. Task 10C-FMT1-C is accepted for the read-only scanner/policy-mapping stack: pure finalized-hour v2 byte-reader scanner, expanded corruption/failure coverage, neutral `history_storage_limits.h` product/domain limit owner, deterministic preamble marker placement, read-only `SdHistoryStore::ScanFinalizedHourFile` File/FS seam, scanner/store host tests, and preserved store append regression.

C-V caveats carried into 10C-FMTV: Arduino firmware build was not run, full CI was not run, hardware SD/FAT behavior was not tested, and power-loss behavior was not tested.

Task 10C-FMTV is validation only except for narrow post-merge anchor/status reconciliation. Do not change scanner/store/source behavior. Do not enable normal runtime SD finalization. Do not start recovery/append guard, repair/truncate/quarantine, or runtime aggregation yet. `10C-F2-B/C` repair/quarantine/append-guard work remains blocked until after broader v2 integrated validation authorizes it, and `10D` runtime aggregation remains blocked.

The durable-history constraints remain intact: finalized-hour v2 is sensor-major, ROM64-indexed, has no durable `slot_id`, stores no `addr16` by default, treats node ID as provenance only, uses bounded labels as context, writes the generated v2 preamble plus binary-start marker before binary records, has no fake padding/reserved fields, preserves the corrected v2 sizes/offsets/CRC rules, keeps production scanner/write paths bounded and heap-free, and keeps runtime finalization blocked until v2 recovery/append-guard validation.

## Required v2 sequence

Current intended sequence:

```text
10C-FMT0 read-only v2 format plan/spec
  -> 10C-FMT0-A v2 on-disk format decision/anchor cleanup
  -> 10C-FMT1-A pure v2 format/schema/preamble skeleton
  -> 10C-FMT1-A-V checkpoint validation
  -> 10C-FMT1-B writer integration
  -> 10C-FMT1-B-V checkpoint validation
  -> 10C-FMT1-C scanner/policy mapping (merged in PR #59)
  -> 10C-FMT1-D anchor/PR-readiness cleanup (merged in PR #59)
  -> 10C-FMTV broader integrated v2 validation after PR #59 merge (current)
  -> 10C-F2-B/C v2 recovery/append guard
  -> 10D runtime aggregator
```

10C-F2-B/C repair/quarantine/append-guard work and 10D runtime aggregation are blocked until after broader 10C-FMTV validates the v2 format/writer/scanner/preamble path after PR #59 cleanup/merge. Do not continue mutating recovery work against the deprecated v1 slot/minute-major layout unless the user explicitly reverses the v2 direction.

## Durable-history rules to preserve

- Finalized SD archive must be sensor-major v2, ROM64-indexed, and not durable-slot/minute-major v1.
- No durable `slot_id` in finalized SD records.
- No stored `addr16` by default; derive printable addr16 from ROM64 for display/debug.
- Node ID is provenance/context only.
- Node label and sensor label snapshots are historical context and require bounded encoding rules.
- Day files require a bounded ASCII schema/preamble and the fixed `%%MESH_TEMPS_BINARY_START%%\n` marker before binary HourRecordV2 records.
- Block magic/sentinel is a validation aid only; normal parsing must use explicit lengths, counts, offsets, versions, flags, and CRCs.
- Production finalized-hour write/verify/scanner paths must remain bounded and heap-free.

- Finalized-hour v2 must not include `reserved0`, fake padding, generic reserved bytes, or alignment filler merely to preserve mistaken byte counts; v2 has no production compatibility constraint, so byte counts come from semantic fields.
- Correct finalized-hour v2 sizes are: HourRecordHeaderV2 48 bytes, SensorIndexEntryV2 12 bytes, SensorBlockHeaderV2 32 bytes, SensorDescriptorV2 106 bytes, SensorPayloadV2 136 bytes, fixed SensorBlockV2 274 bytes; block CRC offset is 24 and descriptor_flags offset is 22.
- The authoritative v2 format module must not include or depend on `history_hour_stager.h`; it owns its v2 on-disk invalid-sample sentinel unless a future task proves a neutral shared-domain owner is warranted.

## Durability rule

Power interruption during FAT32 append/write/flush/close is expected. Before normal runtime SD finalization is enabled, the project needs v2 scanner validation plus approved recovery/append-guard behavior, or the runtime path must remain guarded so it cannot perform normal SD finalization.

Normal runtime SD finalization must not be enabled before recovery/append guard validation.

## Testing hardening caveat

Storage/recovery tests added, touched, relied on, or cited as validation evidence must use explicit behavior checks and cover negative/failure paths. Existing v1 tests that encode active slot count, descriptor bytes, frame bytes, or fixed 64-slot frame assumptions are obsolete as final-format confidence and must be removed or replaced as v2 writer/scanner/runtime replacements land.
