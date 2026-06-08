# MeshTemps Current Next Action Anchor

Project: MeshTemps  
Workstream: GUI-node history storage and chart hardening  
Anchor purpose: Current near-term sequencing for PR #53 and the next PR.  
Status: Committed repository anchor for PR #53 storage-foundation workstream.  
Last updated: 2026-06-08

## Authority

This file and `anchors/README.md` are the current workflow authority for the next MeshTemps storage/history tasks.

They supersede older direct-to-Task-10D wording in:

- `anchors/meshtemps_project_intent_anchor.md`
- `anchors/meshtemps_roadmap_anchor.md`

Do not treat older statements that Task 10D may directly follow Task 10C-E3V as current workflow guidance.

## PR #53 scope

PR #53 is a storage-foundation PR only. It establishes bounded current-hour staging, finalized-hour SD archive format, source/view writing, fixed-size SD write coalescing, static finalized-hour write/verify buffers, deterministic finalized paths, and cleanup of stale storage debt.

PR #53 does not complete runtime aggregation, SD recovery, SD reader/query, chart migration, FRAM, retention/pruning, or hardware validation.

## Required next sequence after PR #53

The intended next sequence is:

```text
10C-F0 read-only SD recovery plan
  -> 10C-F1 finalized-hour append-file scanner / validation service
  -> 10C-F1 validation
  -> 10C-F2 safe tail repair or quarantine policy
  -> 10C-F2 validation
  -> 10D runtime HistoryAggregator snapshot path
```

Task 10D must not be treated as the normal next task unless the user explicitly chooses to defer recovery implementation and keep normal runtime SD finalization behind a reviewed guard.

## Durability rule

Power interruption during FAT32 append/write/flush/close is expected. The current finalized-hour append path verifies clean write/flush/close behavior, but it does not by itself define append-file recovery after an interrupted write.

Before normal runtime SD finalization is enabled, the project needs the recovery scanner and repair/quarantine policy described in `anchors/meshtemps_sd_durability_recovery_anchor.md`, or the runtime path must remain guarded so it cannot perform normal SD finalization.

## Final validation requirement before merge

Before PR #53 merge, final validation must confirm this anchor exists, `anchors/README.md` lists it in the required reading order, PR #53 remains storage-foundation only, and validation ran against the current PR head.
