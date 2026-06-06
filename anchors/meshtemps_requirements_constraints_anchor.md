# MeshTemps Requirements and Constraints Anchor

Project: MeshTemps  
Workstream: GUI-node history storage and chart hardening  
Anchor purpose: Reusable requirements/constraints reference for future ChatGPT/Codex planning, execution, validation, and review tasks.  
Status: Requirements anchor artifact; not yet committed to the repository.  
Last updated: 2026-06-06

## 1. Product requirements

### Core product behavior

MeshTemps must provide reliable live and historical temperature visibility for a mesh of DS18B20-based sensor nodes.

The GUI node must support historical chart ranges such as 1d, 2d, 5d, 7d, and 30d without freezing the UI, exhausting heap, panicking the ESP32, or requiring power cycling.

Durable history must move away from retention-scaled per-sensor RAM vectors and toward:

```text
live mesh state
  -> backend-agnostic current-hour stager
  -> SD finalized-hour archive
  -> storage-backed chart/query path
```

The first implementation must use RAM-first bounded current-hour staging. FRAM must remain an optional later backend behind the same staging interface.

SD finalized-hour blocks are the authoritative long-term history.

Raw minute-level history is authoritative. Rollups/indexes may exist only as derived, rebuildable acceleration data.

Current-hour staging must be capped at 64 slots for now.

Sensors that appear mid-hour must be assigned a slot immediately and logged from that point forward.

DS18B20 ROM/physical address is the durable sensor identity. Node ID is context. Labels/ranks are UI metadata.

### Corrected temperature behavior

Leaf nodes send corrected/display temperature as `tC` and a `corr` flag. Durable history must store the received display/corrected value, not attempt to reconstruct raw values.

Temperature storage should use fixed-point centi-C:

```text
stored = round(temp_c * 100)
```

Corrected status must be preserved through a corrected bitmap or equivalent per-sample/per-slot flag.

### Missing data behavior

Missing/stale/offline readings must be represented explicitly or inferably without fabricating temperatures.

For current-hour fixed frames, absence is represented by a cleared presence bit for the slot/minute.

Chart output should show gaps or otherwise avoid implying a valid reading where none exists.

## 2. Workflow requirements

### User/operator workflow

The touchscreen user should be able to view current readings and historical chart windows without knowing whether history came from RAM staging, FRAM staging, or SD archive.

Storage failures should degrade gracefully and report diagnostics rather than freezing the UI.

### Developer/operator workflow

Serial diagnostics must expose enough state to validate storage behavior:

- active stager backend,
- slot count and catalog contents,
- current hour/minute status,
- SD mount/write/flush status,
- finalized-hour archive status,
- overflow/error counters,
- chart storage source,
- RAM-first current-hour loss limitation,
- later FRAM status if enabled.

Fake-history and diagnostic workflows must remain bounded and must not recreate large retention-scaled RAM allocation.

### Codex workflow

Nontrivial work must follow:

```text
Plan -> Review -> Execute -> Validate
```

The next task in this workstream must be read-only planning before implementation.

Execution tasks must be narrow and ordered. Storage model, SD writer, aggregator, legacy history isolation, SD reader, chart migration, and optional FRAM backend should not be collapsed into one unreviewable task.

## 3. Data/state requirements

### Current-hour staging state

The current-hour stager must contain:

- hour metadata,
- slot catalog,
- 60 minute frames,
- export path for SD finalization,
- backend status/diagnostics,
- bounded storage footprint.

Slot cap: 64 current-hour slots.

Expected minute frame content:

```text
presence bitmap for 64 slots
corrected bitmap for 64 slots
int16 temp_c_x100[64]
frame checksum/CRC if useful
padding/alignment if useful
```

The logical format must be backend-neutral so RAM and FRAM backends can export the same finalized hour.

### Slot catalog state

Each slot must map to durable sensor identity and reporting context.

Minimum catalog concepts:

- slot ID,
- DS18B20 ROM / 16-char address / rom64,
- last-known node ID,
- first-seen time/minute if available,
- last-seen time/minute if available,
- flags/status.

Slot identity must not depend on label text.

### SD finalized-hour state

Every SD finalized-hour block must be self-describing. It must contain enough descriptor/catalog information to decode the hour without current RAM, FRAM, NVS, labels, or live mesh state.

Each finalized-hour block should include:

- hour start,
- format version,
- descriptor/catalog snapshot,
- raw minute frames,
- payload/header validation data,
- enough metadata to detect partial/corrupt writes.

SD is the reconstruction source for historical data.

### Registry state

A durable registry may exist, but old SD hour blocks must not require the current registry to decode historical readings.

If FRAM is later added, FRAM may hold active current-hour catalog/recovery state, but SD hour descriptors remain the durable decode source.

### Time state

Durable hour naming/finalization requires sane epoch time.

Behavior before valid time must be explicitly designed. Acceptable options include delaying durable history, using a temporary boot-relative staging state, or clearly logging skipped history. Codex must not silently invent an unreviewed time policy.

## 4. Architecture/model constraints

### Backend decoupling

History logic must depend on a current-hour staging interface, not directly on RAM, FRAM, Wire, Adafruit_FRAM_I2C, SD, or MeshNode internals.

Required dependency direction:

```text
MeshNode live state
  -> HistoryAggregator
  -> IHistoryHourStager or equivalent
       -> RamHourStager first
       -> FramHourStager later
  -> SdHistoryWriter
  -> SdHistoryReader / chart query service
```

Hardware-specific details must not leak into chart code, MeshNode code, or SD writer logic.

### RAM-first constraint

First implementation must use a bounded RAM stager. It must not be a retention-scaled RAM history replacement.

RAM-first staging may lose the unflushed current hour on reset/power loss. This limitation must be documented and surfaced in diagnostics.

### FRAM-later constraint

FRAM must remain optional/later. Do not block the first storage rewrite on FRAM hardware.

If later added, FRAM must be a drop-in backend behind the same logical stager interface.

FRAM hardware assumption, currently provisional: Adafruit-style I²C FRAM on the Waveshare exposed I²C header.

### SD archive constraint

SD receives finalized current-hour batches. The architecture must not require streaming every mesh packet to SD.

SD writes must not run from LVGL event callbacks.

SD file/block format must not depend on whether the hour was staged by RAM or FRAM.

### Chart/query constraint

Chart code must migrate away from full-history RAM copies.

Chart range handling must use a storage-backed query/reduction path that streams or bounds memory use.

Range-button callbacks must remain responsive.

### Identity constraint

Durable sensor identity is DS18B20 ROM. Node ID is last-known reporting context. Label/rank is UI metadata.

Moving a physical sensor to another node should not split history unless a later explicit product decision says location should override physical continuity.

## 5. Security/permission constraints

MeshTemps is an embedded local project, but storage and command surfaces still require control.

Serial commands that change persistent configuration, clear history, format SD, wipe storage, or alter labels/ranks must be explicit and guarded by clear confirmation syntax where practical.

Do not add network-exposed storage mutation APIs without explicit approval.

Do not store secrets in history files, logs, or diagnostics.

Do not leak Wi-Fi credentials, mesh passwords, or other sensitive config in serial dumps or SD diagnostic exports.

When adding diagnostics, prefer summarized status over raw memory dumps unless a task explicitly requests low-level debug output.

## 6. Validation/testing requirements

### General validation rules

Never claim tests passed unless they were actually run or the user provides output.

Receipts must distinguish:

- verified by code inspection,
- verified by tests/commands,
- hardware-unverified,
- environment-limited,
- assumed from prior context.

### Required behavior tests/checks where feasible

For stager behavior:

- creates slots by ROM identity,
- logs a new sensor mid-hour immediately,
- preserves corrected bitmap,
- leaves missing samples absent/presence=0,
- caps at 64 slots and reports overflow,
- exports deterministic hour snapshot,
- keeps RAM footprint bounded.

For SD writer/reader:

- writes self-describing finalized-hour block,
- stores descriptor snapshot,
- stores all 60 minute frames,
- validates header/payload CRC/checksum,
- detects partial/corrupt block,
- decodes history without current live registry.

For aggregator:

- snapshots live MeshNode values without long-held locks,
- converts float `tC` to centi-C,
- preserves `corr`,
- does not perform SD I/O in UI callbacks,
- handles invalid time policy explicitly.

For chart migration:

- no full-history vector copy for large ranges,
- range buttons remain responsive,
- 1d/2d/5d/7d/30d views render without freeze,
- missing samples do not appear as fabricated readings.

For legacy RAM history isolation:

- unsafe `std::vector<SensorHistorySample>::resize()` path is removed, capped, bypassed, or proven inactive for durable history,
- misleading high-retention examples are removed or corrected.

### Command/build checks

Codex tasks should run relevant compile/static checks available in the repo/environment.

If Arduino/ESP32 build cannot run in the environment, receipt must say so explicitly and identify what was checked instead.

Host-side tests are preferred for pure binary format, CRC, slot catalog, SD block encoding/decoding, and stager behavior.

### Hardware validation

Hardware validation is separate from compile/test validation.

Unverified until physically tested:

- SD mount/write performance on the Waveshare GUI node,
- I²C FRAM wiring/address/bus coexistence,
- touch/CH422G coexistence under future FRAM load,
- long-run GUI behavior with real sensors.

## 7. Documentation requirements

Documentation must be updated when behavior changes.

Required docs/notes as implementation progresses:

- project intent anchor,
- roadmap anchor,
- requirements/constraints anchor,
- storage format documentation,
- serial diagnostic command documentation,
- RAM-first limitation documentation,
- SD archive authority/recovery documentation,
- FRAM-later wiring assumptions if/when FRAM is added,
- migration notes explaining deprecated RAM history behavior.

Docs must clearly state:

- RAM current-hour staging loses current hour on reset/power loss,
- SD finalized-hour blocks are authoritative,
- rollups/indexes are derived/rebuildable,
- labels are not durable storage keys,
- 64-slot cap and overflow behavior.

Do not paste giant receipts into anchors. Summarize decisions and reference artifacts.

## 8. Deployment/environment constraints

Target GUI hardware:

- Waveshare ESP32-S3-Touch-LCD-7.
- Existing board config uses I²C on GPIO8/GPIO9 for touch and CH422G.
- TF/microSD is present on the Waveshare board and is intended for long-term archive.
- Later optional FRAM would share the exposed I²C header.

Target firmware environment:

- Arduino/ESP32-style project structure unless the latest snapshot proves otherwise.
- LVGL UI.
- ESP32 memory limits require bounded allocations and careful event handling.

Current first-stage implementation must not require FRAM hardware.

SD card presence and performance must be treated as fallible.

## 9. Code style and maintainability requirements

Prefer small service-layer components with clear responsibilities.

Avoid hardware details in domain logic.

Avoid large dynamic allocations in UI/event paths.

Avoid retaining full historical vectors in RAM.

Use fixed-width integer types for storage formats.

Version all binary storage formats.

Use explicit endian/layout decisions for SD files and document them.

Avoid undefined behavior from unaligned `reinterpret_cast` on packed binary data. Use `memcpy` for unaligned fixed-width fields.

Use CRC/checksum helpers consistently.

Keep fake-history/test helpers isolated from production history state.

Keep old compatibility paths clearly marked and bounded during migration.

Do not harden wrong names, comments, labels, or tests just because they already exist.

## 10. Prompting and Codex receipt formatting requirements

All Codex-facing tasks, plans, validations, receipts, and handoffs must be in fenced copy/paste text blocks.

Codex task titles should use a clear bordered title block.

Codex tasks must include:

- Goal,
- Background,
- Changes,
- Tests,
- Commands,
- Acceptance criteria.

Codex execution receipts must include:

- files changed,
- behavior implemented,
- tests/commands run,
- command results,
- unverified/environment-limited items,
- deviations from task,
- remaining risks.

Codex validation receipts must include:

- files inspected,
- changed and adjacent behavior verified,
- claims checked against code/tests/docs,
- scope creep check,
- wrong-pattern hardening check,
- GO/NO-GO decision,
- unverified items.

For nontrivial work, require Plan -> Review -> Execute -> Validate.

For snapshot validation, Codex/reviewer must state the snapshot/branch/commit/file set used.

Do not let Codex treat its own plan or receipt as authoritative.

## 11. Scope-control rules

Keep the active workstream focused on GUI-node history storage and chart stability.

Do not combine unrelated GUI redesign, mesh protocol redesign, leaf calibration changes, SD retention policy, FRAM driver work, and chart migration into a single task.

RAM-first storage work and later FRAM backend work should be separate phases.

Chart migration should not start until SD writer/current-hour staging are validated.

FRAM implementation should not start until RAM-first SD archive passes shared stager behavior tests.

Derived rollups/indexes require separate planning before implementation.

Removing legacy history code requires proof that replacement storage/query behavior works or a rollback path exists.

Explicitly mark scope exclusions in every Codex task.

## 12. Known project-specific hazards

Known hazards:

- ESP32 panic from retention-scaled per-sensor history vector resize.
- LVGL/UI freeze from range-button history processing with large data.
- Full-history RAM copy paths.
- Misleading history commands/examples that imply large RAM retention is safe.
- Mesh sensors appearing/disappearing mid-hour.
- Sensor physical ROM moving to a different node ID.
- Labels changing over time.
- SD absent, slow, corrupt, or partially written.
- Invalid time at boot.
- Current-hour RAM staging lost on reset/power loss.
- Future FRAM sharing I²C bus with touch/CH422G.
- PT100 template contains known `FramHourJournal` required-region undercount bug.
- PT100 template has rollups; these must remain derived-only if adapted.

## 13. Settled requirements

Settled as of this anchor:

- First implementation is RAM-first current-hour staging.
- FRAM is optional/later and must be backend-swappable.
- Current-hour slot cap is 64.
- SD finalized-hour archive is authoritative long-term storage.
- Raw minute-level SD history is authoritative.
- Rollups/indexes are derived and rebuildable only.
- New sensors discovered mid-hour are logged immediately.
- DS18B20 ROM is durable sensor identity.
- Labels/ranks are not durable storage keys.
- Temperatures are stored as fixed-point centi-C.
- Corrected status must be preserved.
- Chart migration must avoid full-history RAM copies.
- Codex must do read-only planning before implementation.

## 14. Provisional requirements / user decisions still needed

Still provisional or needing future decision:

- Exact SD file layout and naming convention.
- Whether SD hour descriptors include label/rank snapshots.
- Whether to add rollups/indexes initially or defer until chart performance requires them.
- Raw minute-level SD retention duration.
- Exact behavior when more than 64 sensors appear in one hour.
- SD unavailable policy at hour rollover.
- Invalid-time policy before epoch sync.
- Whether old RAM history remains as a short live cache during/after migration.
- Whether and when to implement FRAM backend.
- Exact FRAM module capacity/address/library if FRAM is added.
- GUI versus serial-only storage diagnostics.
- Whether chart should display corrected status visually or only preserve it in data.

## 15. Last updated context

Used context:

- Current chat through the RAM-first, backend-agnostic staging decision.
- User clarification that helpers must be decoupled so RAM or FRAM can be dropped into the pipeline.
- User clarification that the current-hour slot cap is 64.
- Generated project intent anchor: `meshtemps_project_intent_anchor_v2.md`.
- Generated roadmap anchor: `meshtemps_roadmap_anchor.md`.
- Uploaded handoff: `meshtemps_handoff_history_chart_storage.md`.
- Prior inspected MeshTemps code:
  - GUI `mesh_node.h/.cpp` current history structures and allocation path.
  - Leaf corrected-temperature send path.
  - GUI receive path for `tC` and `corr`.
  - Waveshare I²C pin configuration.
- Prior inspected PT100_Mesh_Datalogger files:
  - `fram_storage_interface.h`,
  - `fram_hour_journal.h/.cpp`,
  - `sd_history_store.h/.cpp`,
  - `history_aggregator.h/.cpp`.
- PT100 issue #367 filed from this chat: `FramHourJournal undercounts required FRAM region by one header slot`.

Unverified:

- No committed MeshTemps requirements anchor was found in a quick connector search.
- Exact latest branch state after the handoff was not revalidated from a fresh local checkout.
- Exact current PR number/branch for the next MeshTemps work was not confirmed.
- SD card mount/write behavior on target GUI node remains hardware-unverified.
- FRAM hardware is deferred and remains physically unverified.
