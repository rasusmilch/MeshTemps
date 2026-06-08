# MeshTemps Anchor Index

Project: MeshTemps  
Workstream: GUI-node history storage and chart hardening  
Purpose: Quick entry point for future ChatGPT/Codex planning, execution, validation, and review tasks.  
Status: Committed repository anchor index for PR #53 storage-foundation workstream.  
Last updated: 2026-06-08

## Required anchor reading order

Before generating or reviewing MeshTemps storage/history tasks, read these anchors in this order:

1. `anchors/meshtemps_project_intent_anchor.md`
   - Product intent, durable-history direction, user/operator workflow, terminology, and current next workflow.

2. `anchors/meshtemps_roadmap_anchor.md`
   - Task sequence, dependencies, checkpoint gates, explicit exclusions, and current next task.

3. `anchors/meshtemps_requirements_constraints_anchor.md`
   - Non-negotiable data, allocation, identity, SD-write, chart/query, and Codex workflow constraints.

4. `anchors/meshtemps_sd_durability_recovery_anchor.md`
   - Power-loss durability risk, FAT32 append/recovery contract, scanner/repair/quarantine plan, and required recovery tests.

## Current durability warning

Raw capacity is not the limiting SD-card issue for the planned finalized-hour data rate. The major durability risk is power loss or reset during FAT32 append/write/flush/close.

Future runtime SD finalization must not silently assume that `file.flush()` and `file.close()` are enough. The finalized-hour append-file recovery contract in `meshtemps_sd_durability_recovery_anchor.md` must be planned and implemented before normal runtime SD finalization is enabled, or runtime finalization must remain explicitly guarded/disabled until recovery passes validation.

## Current planned storage sequence

Current intended sequence after the legacy SdHistoryStore cleanup gate:

```text
10C-E3V legacy SdHistoryStore debt-removal validation
  -> 10C-F0 read-only SD recovery plan
  -> 10C-F1 finalized-hour append-file scanner / validation service
  -> 10C-F1 validation
  -> 10C-F2 safe tail repair or quarantine policy
  -> 10C-F2 validation
  -> 10D runtime HistoryAggregator snapshot path
```

If the user explicitly chooses to start Task 10D before 10C-F1/10C-F2, Task 10D must keep normal runtime SD finalization disabled or behind a reviewed compile/runtime guard. Do not enable normal hourly SD appends without finalized-file recovery behavior.

## Current PR caveat

PR #53's body may lag behind this workstream. Treat the repository anchors and current source as authoritative over the PR body until the PR description is rewritten before final integrated review/merge.
