# MeshTemps Roadmap Anchor

Project: MeshTemps  
Workstream: GUI-node history storage and chart hardening  
Anchor purpose: Keep future ChatGPT/Codex tasks sequenced, scoped, and aligned with current product intent.  
Status: Roadmap anchor artifact; not yet committed to the repository.  
Last updated: 2026-06-07

## 1. Current project/workstream objective

Replace MeshTemps GUI-node retention-scaled RAM history vectors with a bounded, backend-agnostic current-hour staging pipeline that writes finalized, self-describing history blocks to SD.

The active direction is:

```text
live mesh sensor state
  -> backend-agnostic current-hour stager
       -> RamHourStager first
       -> optional FramHourStager later
  -> SD finalized-hour archive
  -> storage-backed chart/query path
```

The objective is to stop history-related ESP32 panics/freezes while preserving multi-day/multi-week chart capability and supporting mesh sensors that appear, disappear, or move between nodes.

## 2. Current active feature/fix

Current active feature/fix:

**Task 10 series — Backend-agnostic current-hour staging and SD finalized-hour history archive**

Current product decisions:

- RAM-first current-hour staging is the immediate implementation target.
- FRAM remains a later optional backend behind the same staging interface.
- Current-hour slot cap is 64.
- SD finalized-hour blocks are the authoritative long-term history.
- Raw minute-level SD history is authoritative.
- Rollups/indexes, if added, are derived and rebuildable only.
- New sensors discovered mid-hour should be assigned a slot immediately and logged from that point forward.
- DS18B20 ROM/physical address is the durable sensor identity.
- Labels/ranks are UI metadata, not durable storage keys.
- Production SD finalized-hour writer/verifier paths must be heap-free/bounded: no large dynamic allocations and no full-record heap buffers.
- SD finalized-hour commit order is complete write, flush/close, then read-back verification with a fixed-size buffer; do not interleave write/read verification chunks during the write.
- Finalized-hour SD path construction must use custom bounded append helpers and fixed caller-owned `char` buffers: no Arduino `String`, `std::string`, printf-family formatting, libc calendar/timezone conversion, malloc/new, or hidden dynamic path building. Filenames use `<base>/finalized/d<epoch_day>.bin`, where `epoch_day = hour_start_epoch_minute / 1440`.

## 3. MVP / immediate tasks

### Task 10A — Read-only architecture plan

Type: read-only planning task.  
PR: no implementation PR required unless Codex produces only documentation/plan files by explicit instruction.  
Risk: high; must prevent wrong architecture from being hardened.

Goal:

Codex must inspect the latest MeshTemps snapshot and PT100_Mesh_Datalogger template directly, then produce a narrow plan for:

- `IHistoryHourStager` or equivalent backend seam,
- `RamHourStager` first implementation,
- later optional `FramHourStager`,
- 64-slot current-hour catalog and minute frame layout,
- corrected-temperature bitmap,
- SD finalized-hour block format,
- raw SD blocks as authoritative,
- chart migration away from per-sensor RAM vectors,
- diagnostics and recovery model,
- staged implementation tasks and validation checkpoints.

Acceptance for planning:

- It must not treat the current per-sensor RAM vector model as acceptable long-term storage.
- It must not require FRAM hardware for the first implementation.
- It must explicitly compare PT100 template patterns against MeshTemps needs.
- It must call out PT100 issue #367 and avoid copying the `FramHourJournal` region-size bug.
- It must define behavior for mid-hour sensor discovery, >64-slot overflow, reset/power loss under RAM staging, SD unavailable, invalid time, and partial SD writes.

### Task 10B — Add backend-neutral history data model and stager interface

Type: focused execute task after 10A plan approval.  
PR: should share the same draft PR branch as 10C/10D unless the plan says otherwise.  
Risk: medium/high; requires checkpoint validation.

Scope:

- Add bounded data structures for 64-slot current-hour catalog and 60 minute frames.
- Add backend-neutral stager interface.
- Add `RamHourStager` implementation only.
- Add fixed-point centi-C conversion helpers.
- Add CRC/checksum helpers if needed by the staging/export format.
- Add unit/host-style tests if feasible for slot assignment, mid-hour discovery, corrected bitmap, missing samples, slot cap, and export behavior.

Explicit exclusions:

- No FRAM driver.
- No SD writer integration unless explicitly split into this task by approved plan.
- No chart UI changes.
- No removal of old RAM history yet except safe isolation if required for compile.

### Task 10C — Add SD finalized-hour writer and file/block format

Type: focused execute task after 10B validation.  
PR: should share Task 10B draft PR branch if 10B is incomplete feature infrastructure.  
Risk: high; checkpoint validation required.

Scope:

- Implement SD finalized-hour block writer.
- Store self-describing descriptor/catalog snapshot with every hour block.
- Store 60 raw minute frames.
- Include header/payload validation/checksums.
- Use append-only or otherwise corruption-tolerant write pattern.
- Add tests for encoding/decoding, CRC failure, partial write detection where feasible.
- Add writer status/status return values or internal diagnostics as useful; defer operator-visible serial diagnostics to runtime integration or a later diagnostics/status task.

Explicit exclusions:

- No chart query rewrite yet.
- No rollups as authoritative data.
- No FRAM backend.
- No frequent per-packet SD streaming.

### Task 10C-A — Remove production dynamic allocation from finalized-hour SD writer

Type: focused execute follow-up after Task 10C.
PR: same draft PR branch as Tasks 10B/10C unless user says otherwise.
Risk: high; checkpoint validation required.

Scope:

- Keep the finalized-hour file/block format from Task 10C.
- Preserve the pure codec/host-test split.
- Replace production `std::vector<uint8_t>` full-record buffering in `SdHistoryStore::AppendFinalizedHourSnapshot()`.
- Replace production `std::vector<uint8_t> record(header.record_bytes)` full-record read-back in `VerifyFinalizedHourRecord_()`.
- Write the complete finalized-hour record to SD first, flush/close, then perform a read-back verification pass.
- Compute payload CRC without buffering the full record.
- Write descriptor and frame bytes in bounded chunks.
- Verify read-back with a fixed-size stack/static buffer, such as 256 or 512 bytes.
- Remove vector-backed finalized-hour helpers from APIs, implementation, and tests; use fixed-size capture buffers for host tests.
- Keep host tests for finalized-hour format and streaming writer behavior.

Explicit exclusions:

- No Task 10D runtime aggregator.
- No chart migration.
- No FRAM.
- No SD reader/query service.
- No retention/pruning.
- No rollups/indexes.
- No serial command changes unless required by compile.

Acceptance/checkpoint:

- Production finalized-hour writer/verifier has no full-record heap vector and no large dynamic allocation.
- Full SD record is written and flushed/closed before read-back verification begins.
- Header/payload CRCs are computed and verified with bounded buffers/chunks.
- Tests use fixed-size capture buffers and validate production streaming/bounded writer behavior.

### Task 10C-C — Remove Arduino String path allocation from finalized-hour SD path

Type: focused execute follow-up after Task 10C-A/10C-B.
PR: same draft PR branch as Tasks 10B/10C unless user says otherwise.
Risk: high; checkpoint validation required before Task 10C-D.

Scope:

- Keep the finalized-hour file/block format and streaming writer unchanged.
- Remove Arduino `String`, `std::string`, `std::vector`, printf-family formatting, malloc/new, and hidden dynamic path construction from the finalized-hour SD append/verify path.
- Store/copy the base directory into bounded fixed storage before finalized-hour path use.
- Build finalized-hour directory and file paths with custom bounded append helpers and caller-owned fixed `char` buffers.
- Reject too-long base directories and path overflow safely.
- Keep legacy `SdHistoryStore` minute/hourly/daily/rollup `String`/`std::function`/`std::vector` patterns as deferred allocation-audit debt unless compile requires a narrow adapter.
- Add host tests for pure fixed path-builder helpers where feasible.

Explicit exclusions:

- No Task 10D runtime aggregator.
- No chart migration.
- No FRAM.
- No SD reader/query service.
- No rollups/indexes.
- No serial command changes.
- No broad allocation audit.

Acceptance/checkpoint:

- `AppendFinalizedHourSnapshot()` and `VerifyFinalizedHourRecord_()` use fixed `char` paths and no Arduino `String`/printf-family formatting.
- Finalized-hour path builder rejects overflows and too-long base directories.
- Existing heap-free streaming writer and fixed-buffer verifier behavior remains intact.

### Task 10C-D — Remove libc time conversion from finalized-hour path construction

Type: focused execute follow-up after Task 10C-C.
Risk: high; checkpoint validation required before Task 10C-E1.

Scope:

- Change finalized-hour append-file naming from local-calendar `YYYYMMDD.bin` to deterministic epoch-day buckets: `<base>/finalized/d<epoch_day>.bin`, where `epoch_day = hour_start_epoch_minute / 1440`.
- Remove `<ctime>`, `<time.h>`, `localtime_r`, `localtime`, `gmtime`, `gmtime_r`, `mktime`, `strftime`, `setenv`, and `tzset` from finalized-hour path construction.
- Keep custom bounded append helpers and fixed caller-owned `char` buffers.
- Preserve finalized-hour binary record format, streaming writer, and fixed-buffer verifier behavior.
- Keep legacy minute/hourly/daily/rollup calendar/String behavior as deferred allocation-audit debt.

Explicit exclusions:

- No source/view streaming writer.
- No fixed-size SD write coalescer.
- No Task 10D runtime aggregator.
- No chart migration, FRAM, SD reader/query service, rollups/indexes, or broad allocation audit.

### Task 10C-E1 — Add finalized-hour source/view streaming writer

Type: focused execute follow-up after Task 10C-D validation.
Risk: high; checkpoint validation required before Task 10C-E2.

Scope:

- Add a finalized-hour source/view streaming writer so runtime finalization does not need to materialize a large `HistoryHourSnapshot` on stack or heap.
- Preserve the existing finalized-hour binary record format and CRC semantics.
- Do not stack-allocate `HistoryHourSnapshot` in callbacks, loops, small FreeRTOS tasks, LVGL handlers, or mesh callbacks.
- Do not add runtime aggregator integration in this task.

### Task 10C-E2 — Add fixed-size SD write coalescer for finalized-hour writes

Type: focused execute follow-up after Task 10C-E1 validation.
Risk: high; checkpoint validation required before Task 10D.

Scope:

- Add a fixed-size SD write coalescer so finalized-hour output is written in larger bounded chunks without dynamic allocation.
- Preserve complete-write, flush/close, then read-back verification ordering.
- Do not change finalized-hour record format.
- Do not add runtime aggregator integration in this task.

### Task 10D — Add history aggregator snapshot path

Type: focused execute task after 10B, 10C, 10C-A, 10C-B, 10C-C, 10C-D, 10C-E1, and 10C-E2 are validated.
PR: should share the same draft PR branch as 10B/10C/10C-A/10C-B/10C-C/10C-D/10C-E1/10C-E2 unless branch size becomes unmanageable.
Risk: high; checkpoint validation required.

Scope:

- Add `HistoryAggregator` or equivalent service.
- Snapshot live `MeshNode::Sensor` state on a controlled cadence, likely once per minute.
- Convert `sensor.temp_c` to centi-C and preserve `sensor.corrected`.
- Assign slots by durable DS18B20 ROM identity.
- Log mid-hour new sensors immediately.
- Do not hold mesh locks during storage I/O.
- Do not write from LVGL event callbacks.
- Treat SD finalization as a bounded batch operation that starts only after Task 10C-A/10C-B/10C-C/10C-D/10C-E1/10C-E2 checkpoint validation proves the writer/verifier avoids production full-record heap buffers, vector-backed helpers, dynamic path construction, large runtime snapshots, and uncoalesced tiny SD writes.
- Add serial diagnostics for current-hour status.
- Keep old chart behavior unchanged unless required to compile.

Explicit exclusions:

- No chart migration yet.
- No FRAM backend.
- No long-term RAM retention vectors.

### Task 10E — Disable, bypass, or cap unsafe retention-scaled RAM history

Type: focused execute task, likely after aggregator/SD path exists.  
PR: can share Task 10B-10D draft PR if all are part of one incomplete storage transition; otherwise use a follow-up draft PR.  
Risk: high because it touches current behavior and diagnostics; checkpoint validation required.

Scope:

- Prevent unsafe `std::vector<SensorHistorySample>::resize()` paths from panicking the GUI node.
- Remove misleading `hist set` examples that imply high-retention RAM history is safe.
- Make `hist show` or equivalent diagnostics report the new storage path and any legacy fallback status.
- Preserve fake-history diagnostics only if they remain bounded and useful.

Explicit exclusions:

- Do not remove old chart UI until replacement query path is ready.
- Do not claim the chart freeze is fully resolved until chart query migration is validated.

## 4. Stabilization tasks

### Task 10F — Add SD history reader/query service

Type: focused execute task after SD writer has validated sample files.  
PR: likely separate draft PR unless 10B-10E are not mergeable alone.  
Risk: high; checkpoint validation required.

Scope:

- Read finalized-hour SD blocks.
- Resolve sensor identity by ROM/slot descriptor.
- Stream finalized-hour records and scan append files sequentially by record offset and `record_bytes`.
- Do not load whole daily finalized files into RAM.
- Do not load large record sets or full histories into heap vectors.
- Validate each finalized-hour record by header and payload CRC before using its contents.
- Use presence bitmaps as authoritative; do not treat diagnostic counters as authoritative sample counts.
- Reduce/query requested ranges without full-history copies.
- Provide bucket/reduction output for chart code.
- Treat raw SD blocks as authoritative.
- Optionally ignore/rebuild derived indexes.

### Task 10G — Migrate chart history reads to storage-backed query path

Type: focused execute task after 10F.  
PR: likely separate draft PR.  
Risk: high UI/performance risk; checkpoint validation required.

Scope:

- Replace full RAM-history copy path for chart ranges.
- Keep LVGL event callbacks responsive.
- Use bounded streaming/reduction for 1d/2d/5d/7d/30d.
- Preserve existing chart behavior where possible.
- Add diagnostics/timing around chart queries.

Explicit exclusions:

- No GUI redesign.
- No new chart styling unless required for missing-data display.
- No FRAM backend.

### Task 10H — Integrated storage/chart validation

Type: validation task, not implementation.  
PR: validates the active draft PR or merged branch.  
Risk: high.

Scope:

- Validate RAM-first staging, SD finalized-hour output, and chart query behavior together.
- Check that unsafe vector allocation is no longer in the active durable-history path.
- Check that range buttons no longer trigger full-history RAM copies or long synchronous work.
- Validate missing samples, corrected bitmap, mid-hour sensors, and 64-slot overflow behavior.
- Mark hardware-limited items clearly.

## 5. Pilot / validation tasks

### Bench validation — RAM-first storage

Type: manual/firmware validation.

Validate:

- GUI node runs with storage enabled for multiple hours.
- SD finalized-hour blocks are written and readable.
- Reset before hour flush loses only current RAM-staged hour and reports this limitation clearly.
- Reset after SD flush preserves finalized history.
- Serial diagnostics are understandable.

### Bench validation — chart behavior

Type: manual/firmware validation.

Validate:

- 1d, 2d, 5d, 7d, and 30d range buttons do not freeze UI.
- Chart renders gaps for missing samples.
- Corrected values remain displayed as values, with corrected status preserved for future use.
- Large history ranges do not allocate old per-sensor history vectors.

### Synthetic validation — mesh churn

Type: behavior test / fake-history / manual simulation.

Validate:

- New sensor appears mid-hour and receives a slot immediately.
- Sensor disappears and returns in the same hour.
- Same DS18B20 ROM appears from a different node ID.
- More than 64 sensors in the hour triggers documented overflow behavior without crash.

### SD failure validation

Type: manual/firmware validation.

Validate:

- SD absent at boot.
- SD removed or unavailable at hour rollover.
- Partial/corrupt hour block handling.
- Diagnostics report failures without UI crash.

## 6. Expansion / future tasks

### Optional Task 11A — Plan I²C FRAM backend

Type: read-only planning task.  
Dependency: RAM-first SD archive is working and validated.  
Risk: high.

Scope:

- Plan `FramHourStager` behind the same stager interface.
- Use Adafruit-style I²C FRAM module on Waveshare I²C header.
- Preserve the same exported finalized-hour format.
- Define FRAM A/B metadata, catalog, frame validation, and recovery.
- Verify I²C address, bus speed, and coexistence with touch/CH422G.

### Optional Task 11B — Implement I²C FRAM backend

Type: focused execute task after 11A approval.  
Risk: high; checkpoint validation required.

Scope:

- Add byte-storage abstraction if not already present.
- Add Adafruit/I²C FRAM adapter.
- Add `FramHourStager`.
- Run the same stager behavior tests used for RAM backend.
- Add recovery tests for power/reset scenarios where feasible.

### Optional Task 12A — Derived rollups/indexes for chart acceleration

Type: read-only planning first.  
Risk: medium/high because audit/authority semantics must stay clear.

Scope:

- Add hourly/daily derived indexes only if chart performance requires it.
- Raw finalized-hour blocks remain authoritative.
- Derived indexes must be rebuildable.

### Optional Task 13A — GUI diagnostics/status screen

Type: planning or focused execute depending on scope.  
Risk: medium.

Scope:

- Expose storage backend, SD status, current-hour status, and error counters on GUI.
- Do not redesign main UI unless scoped separately.

## 7. Explicitly deferred tasks

Deferred until RAM-first SD archive is implemented and validated:

- FRAM backend implementation.
- Hardware wiring validation for Adafruit I²C FRAM.
- Chart visual redesign.
- Rollup/index acceleration.
- Long-term SD retention/pruning policy.
- SD export/import tools.
- Cloud/off-device history sync.
- Backporting the new storage model to root/leaf code beyond what is needed for GUI-node history.
- Removing all legacy history code before replacement chart path is proven.

## 8. Explicitly rejected or deprecated paths

Rejected/deprecated:

- Retention-scaled per-sensor RAM `std::vector<SensorHistorySample>` as durable history.
- Making FRAM the long-term multi-week history store.
- Requiring FRAM hardware before storage rewrite can start.
- Writing every mesh packet directly to SD as the primary design.
- Mixing sensor descriptor records into a wrapping sample ring.
- Using labels as durable history keys.
- Using node ID alone as durable sensor identity.
- Treating derived rollups as authoritative history.
- Copying PT100 `FramHourJournal` code without fixing/adapting known layout-region issue.
- Continuing chart hardening as if storage allocation is not the main blocker.

## 9. Task dependencies and sequencing

Recommended sequence:

```text
10A read-only plan
  -> plan review
  -> 10B stager interface + RamHourStager
  -> 10B checkpoint validation
  -> 10C SD finalized-hour writer
  -> 10C-A remove production SD full-record heap buffering
  -> 10C-B remove finalized-hour vector helpers
  -> 10C-C remove finalized-hour dynamic path construction
  -> 10C-D remove finalized-hour libc time conversion
  -> 10C-D checkpoint validation
  -> 10C-E1 finalized-hour source/view streaming writer
  -> 10C-E1 checkpoint validation
  -> 10C-E2 fixed-size SD write coalescer
  -> 10C-E2 checkpoint validation
  -> 10D HistoryAggregator snapshot path
  -> 10D checkpoint validation
  -> 10E legacy RAM-history safety/bypass
  -> integrated validation
  -> 10F SD history reader/query service
  -> 10G chart migration
  -> integrated storage/chart validation
  -> optional 11A/11B FRAM backend
```

Do not start 10F/10G chart migration until the SD writer and current-hour staging path are proven.

Do not start FRAM implementation until RAM-first backend passes shared stager behavior tests and SD archive validation.

Do not remove old history/chart paths until replacement query behavior is validated or a rollback path exists.

## 10. Draft PR branch guidance

One draft PR branch should contain the first incomplete storage transition if the code is not independently useful until integrated:

- 10B
- 10C
- 10C-A
- 10C-B
- 10C-C
- 10C-D
- 10C-E1
- 10C-E2
- 10D
- possibly 10E

This draft PR should be checkpoint-validated between subtasks but not treated as complete until integrated validation passes.

A later separate draft PR should likely contain:

- 10F SD reader/query service
- 10G chart migration

A still later separate draft PR should contain:

- 11A/11B FRAM backend planning/implementation, if pursued.

Avoid mixing FRAM backend work into the RAM-first SD archive PR.

## 11. Tasks requiring read-only planning first

Read-only planning required:

- 10A backend-agnostic staging and SD archive architecture.
- 11A optional FRAM backend.
- 12A derived rollups/indexes.
- Any SD retention/pruning policy.
- Any GUI diagnostic/status screen that changes user workflow substantially.
- Any removal of legacy history code before replacement query path is proven.

## 12. Tasks that can be focused execute tasks

Can be focused execute tasks after 10A approval:

- 10B stager interface + RamHourStager.
- 10C SD finalized-hour writer.
- 10C-A production SD writer/verifier allocation cleanup.
- 10C-B finalized-hour vector helper removal.
- 10C-C finalized-hour fixed path construction cleanup.
- 10C-D finalized-hour epoch-day bucket path cleanup.
- 10C-E1 finalized-hour source/view streaming writer.
- 10C-E2 finalized-hour fixed-size SD write coalescer.
- 10D HistoryAggregator snapshot path after 10C-E2 validation.
- 10E legacy RAM-history safety/bypass.
- 10F SD reader/query service.
- 10G chart migration, after 10F.
- FRAM backend implementation only after 11A planning approval.

Each execute task must include behavior tests or command checks where feasible, and a final receipt with files changed, commands run, results, and unverified items.

## 13. High-risk tasks requiring checkpoint validation

Checkpoint validation required after:

- 10B, because it defines the core storage model.
- 10C, because it defines SD archive format and corruption behavior.
- 10C-A, because it gates heap-free production SD finalization before runtime integration.
- 10C-B, because it removes vector-backed finalized-hour API/test patterns.
- 10C-C, because it removes dynamic finalized-hour SD path construction before runtime integration.
- 10C-D, because it removes libc calendar/timezone conversion from finalized-hour path construction.
- 10C-E1, because it prevents large `HistoryHourSnapshot` materialization in runtime finalization.
- 10C-E2, because it coalesces finalized-hour SD writes with bounded buffers before runtime integration.
- 10D, because it touches live mesh state and timing.
- 10E, because it changes/isolates legacy history behavior.
- 10F, because it reads durable history and may affect chart data correctness.
- 10G, because it touches LVGL event responsiveness and chart behavior.
- 11B, because FRAM recovery and I²C behavior are hardware-sensitive.

Integrated validation required before declaring the workstream complete:

- After 10B-10E together.
- After 10F-10G together.
- After any FRAM backend is added.

## 14. Completed tasks still needing integrated validation

Prior completed or partially completed work from the chart-hardening thread still needs integrated validation against the new storage direction:

- Fake-history diagnostics and fake waveform support.
- Chart-series preparation seams.
- Ring-buffer/history helper work.
- History layout diagnostics.
- Any prior Task 3A / 3A-A / 3A-B / 3C-adjacent work referenced in the handoff.

These should not be assumed sufficient to solve the freeze until validated with the storage replacement and chart query path.

The PT100 issue #367 bug report was created separately and is not a MeshTemps completion item. It remains a warning not to copy that template bug.

## 15. Current next required action

Generate the focused execute follow-up task:

```text
Task 10C-D — Remove libc time conversion from finalized-hour path construction
```

Task 10C-D must be checkpoint-validated before Task 10C-E1. Task 10C-E1 should add a finalized-hour source/view streaming writer, and Task 10C-E2 should add a fixed-size SD write coalescer before Task 10D runtime aggregation.

## 16. Last updated context

Used context:

- Current chat discussion through the decision to use RAM-first current-hour staging and keep FRAM as a later backend.
- User clarification that helpers must stay decoupled so RAM or FRAM can be dropped into the pipeline.
- User clarification that the current-hour slot cap is 64.
- Generated project intent anchor: `meshtemps_project_intent_anchor_v2.md`.
- Uploaded handoff: `meshtemps_handoff_history_chart_storage.md`.
- Prior inspected MeshTemps code:
  - GUI `mesh_node.h/.cpp` current history structures and allocation path.
  - Leaf corrected-temperature send path.
  - GUI receive path for `tC` and `corr`.
  - Waveshare I²C pin configuration.
- Prior inspected PT100_Mesh_Datalogger files:
  - `fram_storage_interface.h`
  - `fram_hour_journal.h/.cpp`
  - `sd_history_store.h/.cpp`
  - `history_aggregator.h/.cpp`
- PT100 issue #367 filed from this chat: `FramHourJournal undercounts required FRAM region by one header slot`.
- Task 10C review finding that production finalized-hour write/read-back paths still use full-record `std::vector<uint8_t>` buffers.
- Task 10C-R/10C-C anchor updates inserting heap-free writer, vector-helper removal, and fixed-path construction gates before Task 10D.

Unverified:

- No committed MeshTemps roadmap anchor was found in a quick connector search.
- Exact latest branch state after the handoff was not revalidated from a fresh local checkout.
- Exact current PR number/branch for the next MeshTemps work was not confirmed.
- SD card mount/write behavior on the target GUI node remains hardware-unverified.
- FRAM hardware is now deferred and remains physically unverified.

## 17. Allocation audit carry-forward

Before runtime SD finalization is enabled, either as part of Task 10D/10E validation or a focused validation gate, audit production GUI-node allocation risks. The audit must separate MeshTemps application-code findings from vendor/demo library patterns and must inspect at least:

- the legacy `std::vector<SensorHistorySample>` history path and resize/reserve behavior,
- chart/history full-copy paths,
- `SerialConsole` `String`/`std::vector` token or cache buffers,
- `SdHistoryStore` Arduino `String`/`File` usage and old PT100-era direct struct writes,
- any `std::vector`, `String`, `reserve()`, `resize()`, `malloc()`, `calloc()`, `realloc()`, or `new` patterns in MeshTemps-GUINode production code.

Task receipts and validation reports must distinguish bounded, cold-path allocations from hot-path or storage-event allocations that can reintroduce ESP32 panic/freeze risk.
