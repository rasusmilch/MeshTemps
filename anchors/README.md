# MeshTemps Anchor Index

Project: MeshTemps
Workstream: GUI-node history storage and chart hardening
Purpose: Quick entry point for future ChatGPT/Codex planning, execution, validation, and review tasks.
Status: Committed repository anchor index for PR #54 scanner merge and split 10C-F2 recovery workstream.
Last updated: 2026-06-09

## Required anchor reading order

Before generating or reviewing MeshTemps storage/history tasks, read these anchors in this order:

1. `anchors/README.md`
   - Anchor index and current workflow tie-breaker.

2. `anchors/meshtemps_current_next_action_anchor.md`
   - Current near-term sequencing for PR #53 and the next PR. Supersedes stale direct-to-Task-10D wording in older large anchors.

3. `anchors/meshtemps_project_intent_anchor.md`
   - Product intent, durable-history direction, user/operator workflow, and terminology.

4. `anchors/meshtemps_roadmap_anchor.md`
   - Task sequence, dependencies, checkpoint gates, and explicit exclusions.

5. `anchors/meshtemps_requirements_constraints_anchor.md`
   - Non-negotiable data, allocation, identity, SD-write, chart/query, testing, and Codex workflow constraints.

6. `anchors/meshtemps_testing_hardening_anchor.md`
   - Testing policy for future tests and existing/past tests that future work touches, relies on, or cites as validation evidence.

7. `anchors/meshtemps_sd_durability_recovery_anchor.md`
   - Power-loss durability risk, FAT32 append/recovery contract, scanner/repair/quarantine plan, and required recovery tests.

## Current planned storage sequence

Current intended sequence after PR #53 merge:

```text
10C-F0 read-only SD recovery plan
  -> 10C-F1 finalized-hour append-file scanner / validation service (merged in PR #54)
  -> 10C-F2-A non-destructive recovery policy seam and append-safety classifier
  -> 10C-F2-A validation
  -> 10C-F2-B approved repair/quarantine/fault implementation
  -> 10C-F2-C runtime integration / append guard
  -> 10C-F2V recovery validation
  -> 10D runtime HistoryAggregator snapshot path
```

If older anchor text still says Task 10D may follow directly after Task 10C-E3V, use this index and `meshtemps_current_next_action_anchor.md` as the current sequencing authority.

## Testing hardening caveat

Future and existing tests are not automatically trusted just because they compile or already exist. Use `meshtemps_testing_hardening_anchor.md` when planning, executing, or validating tests. Storage/recovery/chart-freeze work must harden weak tests before relying on them for final confidence.

## Current PR caveat

PR #53's body may lag behind this workstream. Treat the repository anchors and current source as authoritative over the PR body until the PR description is rewritten before final integrated review/merge.
