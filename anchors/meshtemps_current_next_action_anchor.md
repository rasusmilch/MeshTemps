# MeshTemps Current Next Action Anchor

Project: MeshTemps
Workstream: GUI-node history storage and chart hardening
Anchor purpose: Current near-term sequencing for MeshTemps storage/history tasks.
Status: Refreshed after Task 10C-FMT1-A implemented the pure finalized-hour v2 format/schema/preamble skeleton; current work is checkpoint validation.
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
Task 10C-FMT1-A-V — checkpoint validation for the pure finalized-hour v2 format/schema/preamble skeleton
```

Task 10C-FMT1-A has implemented the pure v2 byte-format constants, field tables, preamble generator, CRC-32/ISO-HDLC helper, and host-testable fixed-structure encode/decode skeleton. Checkpoint validation must confirm the approved ABI, no-v1 policy, generated schema coverage, and scope boundaries before 10C-FMT1-B writer integration begins.

## Next action after 10C-FMT1-A-V

The first implementation task after checkpoint approval is:

```text
Task 10C-FMT1-B — finalized-hour v2 writer integration
```

10C-FMT1-B must stay focused on writer/day-file integration and must not broaden into scanner recovery repair, runtime aggregation, chart/query, FRAM, retention/pruning, or hardware validation.

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

## Durability rule

Power interruption during FAT32 append/write/flush/close is expected. Before normal runtime SD finalization is enabled, the project needs v2 scanner validation plus approved recovery/append-guard behavior, or the runtime path must remain guarded so it cannot perform normal SD finalization.

Normal runtime SD finalization must not be enabled before recovery/append guard validation.

## Testing hardening caveat

Storage/recovery tests added, touched, relied on, or cited as validation evidence must use explicit behavior checks and cover negative/failure paths. Existing v1 tests that encode active slot count, descriptor bytes, frame bytes, or fixed 64-slot frame assumptions are obsolete as final-format confidence and must be removed or replaced as v2 writer/scanner/runtime replacements land.
