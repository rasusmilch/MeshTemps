# MeshTemps Anchor Index

Project: MeshTemps
Workstream: GUI-node history storage and chart hardening
Purpose: Quick entry point for future ChatGPT/Codex planning, execution, validation, and review tasks.
Status: Committed repository anchor index refreshed for the finalized-hour v2 ABI approval gate before implementation.
Last updated: 2026-06-12

## Required anchor reading order

Before generating or reviewing MeshTemps storage/history tasks, read these anchors in this order:

1. `anchors/README.md`
   - Anchor index and current workflow summary.

2. `anchors/meshtemps_current_next_action_anchor.md`
   - Current near-term sequencing. This file must agree with the updated roadmap, requirements, and decision log; it no longer supersedes them.

3. `anchors/meshtemps_project_intent_anchor.md`
   - Product intent, durable-history direction, user/operator workflow, and terminology.

4. `anchors/meshtemps_roadmap_anchor.md`
   - Task sequence, dependencies, checkpoint gates, and explicit exclusions.

5. `anchors/meshtemps_requirements_constraints_anchor.md`
   - Non-negotiable data, allocation, identity, SD-write, chart/query, testing, and Codex workflow constraints.

6. `anchors/meshtemps_decision_log_anchor.md`
   - Settled/proposed/open decisions, including the finalized-hour v2 ABI approval table that must be resolved before implementation.

7. `anchors/meshtemps_testing_hardening_anchor.md`
   - Testing policy for future tests and existing/past tests that future work touches, relies on, or cites as validation evidence.

8. `anchors/meshtemps_sd_durability_recovery_anchor.md`
   - Power-loss durability risk, FAT32 append/recovery contract, scanner/repair/quarantine plan, and required recovery tests. Some field-level v1 examples are stale until the v2 scanner lands; the longest-valid-prefix recovery principle remains current.

## Current planned storage sequence

Current intended sequence after the v2 format clarification:

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

The old PR #54/#55 v1-recovery sequence is no longer the immediate next workflow. PR #54 scanner and PR #55 recovery-policy work remain useful infrastructure, but future recovery and runtime work must be updated/revalidated for finalized-hour v2 before 10C-F2-B/C or 10D proceeds.

Implementation must not begin until the compact v2 ABI decision table in `anchors/meshtemps_decision_log_anchor.md` is reviewed and the choices needed for 10C-FMT1-A are approved. The first implementation task after that approval is the narrow 10C-FMT1-A pure format/schema/preamble skeleton.

If any older anchor text still says Task 10D may follow directly after Task 10C-E3V, or routes immediately to 10C-F2-B/C v1 repair/quarantine work, treat that text as stale and reconcile it with this index, the current-next-action anchor, the roadmap, the requirements anchor, and the decision log before executing work.

## Testing hardening caveat

Future and existing tests are not automatically trusted just because they compile or already exist. Use `meshtemps_testing_hardening_anchor.md` when planning, executing, or validating tests. Storage/recovery/chart-freeze work must harden weak tests before relying on them for final confidence.

## Current PR caveat

PR bodies may lag behind this workstream. Treat the repository anchors and current source as authoritative over PR descriptions until a PR description is rewritten before final integrated review/merge.
