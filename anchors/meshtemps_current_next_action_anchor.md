# MeshTemps Current Next Action Anchor

Project: MeshTemps
Workstream: GUI-node history storage and chart hardening
Anchor purpose: Current near-term sequencing for MeshTemps storage/history tasks.
Status: Refreshed after PR #57 review; current work is a focused PR #57 code revision before checkpoint validation.
Last updated: 2026-06-13

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
Task 10C-FMT1-A-R2 — revise PR #57 v2 format code before checkpoint validation
```

PR #57 needs a focused code revision before 10C-FMT1-A-V checkpoint validation. The revision must remove `reserved0`/fake padding from the finalized-hour v2 ABI, correct SensorBlockHeaderV2 to 32 bytes, correct SensorDescriptorV2 to 106 bytes, correct fixed SensorBlockV2 to 274 bytes, use block CRC offset 24 and descriptor_flags offset 22, remove the `history_hour_stager.h` dependency from the v2 format module, and use a v2-owned invalid-sample sentinel instead of borrowing a transitional stager constant.

## Next action after 10C-FMT1-A-R2

After the PR #57 R2 code revision lands, the next action is checkpoint validation:

```text
Task 10C-FMT1-A-V — checkpoint validation for the pure finalized-hour v2 format/schema/preamble skeleton
```

10C-FMT1-A-V must confirm the no-padding ABI, corrected sizes/offsets, v2-owned invalid sentinel, no dependency on legacy/transitional stager headers, no-v1 policy, generated schema coverage, and scope boundaries before 10C-FMT1-B writer integration begins.

## Required v2 sequence

Current intended sequence:

```text
10C-FMT0 read-only v2 format plan/spec
  -> 10C-FMT0-A v2 ABI decision/anchor cleanup
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
