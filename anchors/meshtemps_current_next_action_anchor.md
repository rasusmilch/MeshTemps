# MeshTemps Current Next Action Anchor

Project: MeshTemps
Workstream: GUI-node history storage and chart hardening
Anchor purpose: Current near-term sequencing for MeshTemps storage/history tasks.
Status: Refreshed during PR #58 stacked writer work; current work is the B1-R2 writer workspace/label-failure hardening before B1 validation.
Last updated: 2026-06-14

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
Task 10C-FMT1-B1-R2 — harden finalized-hour v2 writer workspace and label-failure contract tests
```

PR #57 has merged into `feature/ram-backed-sd-hist` and completed the pure finalized-hour v2 format/schema/preamble skeleton, the R1 anchor correction, the R2 no-padding/no-stager-dependency code correction, and the 10C-FMT1-A-V targeted checkpoint validation. The v2 format skeleton passed targeted validation.

PR #58 is the current draft PR stack. It contains the anchor terminology cleanup plus the pure 10C-FMT1-B1 writer core work. B1-R1 added the caller-provided writer workspace, moved large writer scratch out of automatic stack locals, and snapshotted labels once before CRC/write passes. The current correction is B1-R2: document the workspace storage/lifetime contract, add a workspace-size tripwire, and independently cover node-label and sensor-label source failures before validation.

After B1-R2 lands, run:

```text
Task 10C-FMT1-B1-V — validate pure finalized-hour v2 writer core and host tests
```

Do not start 10C-FMT1-B2 SdHistoryStore/day-file append integration until B1-R1, B1-R2, and B1-V are complete, reviewed, and accepted.

## Required v2 sequence

Current intended sequence:

```text
10C-FMT0 read-only v2 format plan/spec
  -> 10C-FMT0-A v2 on-disk format decision/anchor cleanup
  -> 10C-FMT1-A pure v2 format/schema/preamble skeleton
  -> 10C-FMT1-A-V checkpoint validation
  -> 10C-FMT1-B writer integration
  -> 10C-FMT1-B-V checkpoint validation
  -> 10C-FMT1-C scanner/policy mapping
  -> 10C-FMT1-C-V checkpoint validation
  -> 10C-FMT1-D tests/docs cleanup
  -> 10C-FMTV integrated v2 validation
  -> 10C-F2-B/C v2 recovery/append guard
  -> 10D runtime aggregator
```

10C-F2-B/C repair/quarantine/append-guard work and 10D runtime aggregation are blocked until after 10C-FMTV validates the v2 format/writer/scanner/preamble path. Do not continue mutating recovery work against the deprecated v1 slot/minute-major layout unless the user explicitly reverses the v2 direction.

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
