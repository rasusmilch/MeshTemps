# MeshTemps Requirements and Constraints Anchor

Project: MeshTemps  
Workstream: GUI-node history storage, SD archive, recovery, and chart hardening  
Anchor purpose: Reusable requirements/constraints reference for future ChatGPT/Codex planning, execution, validation, and review tasks.  
Status: Committed repository anchor after PR #55 merge and finalized-hour v2 intent/roadmap clarification.  
Last updated: 2026-06-11

## 1. Product requirements

MeshTemps must provide reliable live and historical temperature visibility for a mesh of DS18B20-based sensor nodes.

The GUI node must support historical chart ranges such as 1d, 2d, 5d, 7d, and 30d without freezing the UI, exhausting heap, panicking the ESP32, or requiring power cycling.

Durable history must move away from retention-scaled per-sensor RAM vectors and toward:

```text
live mesh state
  -> bounded current-hour stager
  -> sensor-major finalized-hour SD archive
  -> storage-backed reader/query/reduction path
  -> chart UI
```

The first implementation must use RAM-first bounded current-hour staging. FRAM must remain an optional later backend behind the same staging interface.

SD finalized-hour records are the authoritative long-term history. Raw minute-level finalized-hour data is authoritative. Rollups/indexes may exist only as derived, rebuildable acceleration data.

Finalized SD archive records must be durable, auditable, self-describing, sensor-major, ROM64-indexed records. They must preserve enough historical context to decode and understand old data even if current labels, node mappings, NVS state, or live configuration are lost.

## 2. Workflow requirements

### User/operator workflow

The touchscreen user should be able to view current readings and historical chart windows without knowing whether current-hour staging uses RAM or future FRAM, and without knowing which SD day files contain the requested range.

Storage failures must degrade gracefully and report diagnostics rather than freezing the UI.

Charts must not fabricate readings for missing samples. Missing data should appear as gaps or another explicit missing-data representation.

### Developer/operator workflow

Serial diagnostics must expose enough state to validate storage behavior:

- active current-hour stager backend;
- current-hour state and active count;
- SD mount/write/flush/read-back status;
- finalized archive scan status;
- append-blocked status;
- recovery/repair/fault counters;
- overflow/error counters;
- chart storage source;
- RAM-first current-hour loss limitation;
- later FRAM status if enabled.

Fake-history and diagnostic workflows must remain bounded and must not recreate large retention-scaled RAM allocation.

## File ownership, dependency direction, and debt avoidance

Do not create new files, shared headers, utility modules, or common constant containers merely for local convenience or cosmetic organization. Every new file must have a clear owner, a clear reason to exist, and a defined dependency direction.

Before moving a constant, function, class, or helper into a shared location, decide whether the item represents one stable concept or merely two separate concepts that currently have the same value. Share only when drift would be a correctness bug and a neutral owner exists. Otherwise keep the value local and test the mapping seam.

New authoritative modules must not depend on legacy or transitional modules to reuse constants or helpers. If a legacy module and a new module both need a concept, either the new module owns its ABI-specific value or a deliberately owned neutral module is created with explicit justification.

Avoid generic shared files such as `common.h`, `utils.h`, `shared_constants.h`, or miscellaneous helper modules unless their ownership and allowed contents are narrowly defined. Shared files are architectural commitments, not cleanup tools.

Promotion rule: promote a local constant/function/class to shared ownership only when all are true:
1. At least two stable non-legacy modules need the same concept.
2. The concept is semantically the same, not merely numerically/textually identical.
3. Drift between copies would be a correctness bug.
4. There is a clear neutral owner for the shared concept.
5. The shared file does not depend on either consumer.
6. Tests can enforce the shared contract.

Demotion/removal rule: a shared file or helper is suspicious and should be reviewed if it has only one real consumer, mixes unrelated constants/functions, depends on a higher-level module, exists only to reduce include typing, contains transitional/legacy concepts, or nobody can explain who owns it.

Finalized-hour v2 ABI constraints:
- No `reserved0`, fake padding, generic reserved bytes, or alignment filler merely to preserve mistaken byte counts.
- SensorBlockHeaderV2 is 32 bytes, SensorDescriptorV2 is 106 bytes, SensorPayloadV2 is 136 bytes, fixed SensorBlockV2 is 274 bytes, block CRC offset is 24, and descriptor_flags offset is 22.
- The v2 format module must not include or depend on `history_hour_stager.h`.
- The v2 format module owns its on-disk invalid-sample sentinel with a v2-specific name unless a future task proves a neutral shared-domain owner is warranted.
- Do not create a shared header solely for the v2 invalid-sample sentinel.

## Codex workflow

Nontrivial work must follow:

```text
Plan -> Review -> Execute -> Validate
```

The current next required task is read-only planning/spec work: `10C-FMT0` finalized-hour v2 format planning. Do not continue mutating repair/quarantine, runtime append guard, or Task 10D runtime aggregation before the v2 format gate is reviewed and approved.

Execution tasks must be narrow and ordered. Format, scanner, repair, runtime aggregation, reader/query, chart migration, diagnostics, pruning, and optional FRAM work must not be collapsed into one unreviewable task.

## 3. Data/state requirements

### Current-hour staging state

The current-hour stager stores only active current-hour state. It is not long-term history.

The current source uses a slot/minute-major logical staging model with slot descriptors and 60 minute frames. That is acceptable as an internal staging implementation if bounded, but it is not the finalized SD binary ABI.

Current-hour staging requirements:

- bounded RAM-first implementation now;
- optional FRAM backend later only if approved;
- current-hour active sensor cap currently 64;
- sensors appearing mid-hour must be represented immediately;
- prior minutes for newly seen sensors are missing;
- corrected status must be preserved;
- temperatures must convert to signed centi-C before durable storage;
- overflow beyond the supported active sensor count must be explicit and diagnostic-visible.

### Finalized SD archive state

Finalized SD records must be sensor-major v2 records. Each hour record contains one sensor block for each ROM64 seen at least once during that hour.

Finalized day file layout:

```text
ASCII day-file preamble / compact schema guide
%%MESH_TEMPS_BINARY_START%%
HourRecordV2
HourRecordV2
...
```

The day-file preamble must be bounded and deterministic for a schema/version. It must describe field names, field types, byte lengths, global endianness, string encoding, CRC type, temperature encoding, file layout, hour record layout, sensor index layout, sensor block layout, payload layout, and validity/recovery rules.

Binary parsing must not depend on the prose. Parsing must use explicit binary lengths, counts, offsets, versions, flags, magic/sentinel values, and CRCs.

Required finalized-hour v2 concepts:

```text
HourRecordHeaderV2
SensorIndexTableV2: repeated { rom64, sensor_block_offset_from_record_start }
SensorBlockV2 repeated sensor_count times
```

Required sensor block concepts:

```text
SensorBlockHeaderV2 with block magic/sentinel, block bytes, descriptor bytes, bitmap bytes, sample count, sample encoding, CRC, flags
SensorDescriptorV2 with ROM64, last-known node ID, node label, sensor label, first/last seen minute, counts, flags
SensorPayloadV2 with 60-minute presence bitmap, 60-minute corrected bitmap, and 60 int16 centi-C samples
```

Each emitted sensor block stores one fixed-width sample position per minute. Presence bitmap bits determine which minute positions are valid. Corrected bitmap bits determine which valid samples were corrected.

### Identity and label state

ROM64 is the canonical durable sensor identity.

`addr16` is a printable representation derived from ROM64 and should not be stored in finalized SD records by default.

Node ID is reporting provenance/context, not sensor identity.

Node label and sensor label are historical context snapshots and must be stored in each finalized sensor block, subject to explicit bounded length rules defined by the v2 spec. Labels are not keys and must not determine continuity.

A physical DS18B20 moving to another node should preserve historical continuity by ROM64 while node ID/label context updates.

### Time state

Durable hour naming/finalization requires sane epoch time. Behavior before valid time must be explicitly designed. Codex must not silently invent a time policy.

Current deterministic day-file naming remains epoch-day based unless a future task changes it:

```text
<base>/finalized/d<epoch_day>.bin
where epoch_day = hour_start_epoch_minute / 1440
```

## 4. Architecture/model constraints

History logic must depend on a current-hour staging interface and storage/query services, not direct UI/chart access to old `MeshNode` history vectors.

Required dependency direction:

```text
MeshNode live state
  -> HistoryAggregator / snapshot cadence
  -> IHistoryHourStager or equivalent
       -> RamHourStager first
       -> FramHourStager later if approved
  -> finalized-hour SD writer
  -> finalized-hour scanner/recovery policy
  -> finalized-hour reader/query/reduction service
  -> chart UI
```

Hardware-specific details must not leak into chart code, MeshNode code, or SD writer logic.

The current-hour stager format is not the SD binary ABI. The SD writer must be free to serialize sensor-major v2 output even if RAM staging remains slot/minute-major internally.

Use field-by-field explicit little-endian serialization. Do not serialize raw compiler-padded structs. Avoid generic reserved byte arrays unless a specific format purpose is approved; use version fields, byte counts, flags, and explicit lengths instead.

Block magic/sentinel is allowed as an additional safeguard, preferably at the beginning of each sensor block. Do not rely on sentinel hunting as the normal parser boundary.

CRC is required before considering FEC. FEC is deferred unless a later task proves a specific error model and need.

### SD archive implementation constraints

SD receives finalized hourly batches. The architecture must not require streaming every mesh packet to SD.

SD writes must not run from LVGL event callbacks.

Production finalized-hour path construction, serialization, append, and verification must not use large dynamic allocations, full-record heap buffers, or large task-stack/callback-stack buffers.

Low-level finalized-hour production code must avoid:

- Arduino `String`;
- `std::string`;
- `std::vector` ownership/full-record buffers;
- `std::function`;
- printf-family path construction: `snprintf`, `sprintf`, `asprintf`, etc.;
- `malloc`, `calloc`, `realloc`, `new`, `delete`;
- libc calendar/timezone conversion in finalized path construction: `<ctime>`, `<time.h>`, `localtime_r`, `localtime`, `gmtime`, `gmtime_r`, `mktime`, `strftime`, `setenv`, `tzset`;
- hidden dynamic allocation.

Approved finalized-hour I/O buffer policy remains:

- file-scope static 4096-byte SD write coalescer buffer;
- file-scope static 512-byte read-back verification buffer;
- single-writer/non-reentrant ownership under the storage/runtime service;
- no PSRAM for finalized-hour SD I/O buffers unless a future task verifies the exact SD stack/DMA/backend behavior and explicitly changes the decision.

Finalized-hour write order remains:

```text
complete record write
  -> coalescer flush
  -> file.flush()
  -> file.close()
  -> reopen/read-back verification
```

Do not interleave write/read verification chunks during the normal write path.

### Chart/query constraints

Chart code must migrate away from full-history RAM copies. Reader/query work must stream validated finalized-hour records by offsets and reduce ranges without loading whole day files, large record sets, or full histories into heap vectors.

Range-button callbacks must remain responsive.

## 5. Security/permission constraints

MeshTemps is an embedded local project, but storage and command surfaces still require control.

Serial commands that change persistent configuration, clear history, format SD, wipe storage, repair files, quarantine files, prune history, or alter labels/ranks must be explicit and guarded by clear confirmation syntax where practical.

Do not add network-exposed storage mutation APIs without explicit approval.

Do not store secrets in history files, logs, or diagnostics.

Do not leak Wi-Fi credentials, mesh passwords, or other sensitive config in serial dumps or SD diagnostic exports.

When adding diagnostics, prefer summarized status over raw memory dumps unless a task explicitly requests low-level debug output.

## 6. Validation/testing requirements

Never claim tests passed unless they were actually run or the user provides output.

Receipts must distinguish:

- verified by code inspection;
- verified by tests/commands;
- hardware-unverified;
- environment-limited;
- assumed from prior context.

Host-side tests are required for pure binary format, CRC, scanner, recovery-policy, current-hour stager, and serializer behavior where feasible.

Tests added, touched, relied on, or cited by future tasks must not depend on raw `assert()` behavior if the task needs confidence under `-DNDEBUG`. Use explicit check harnesses for new/critical storage/recovery tests.

### Required v2 format tests

Future v2 writer/scanner tests must cover:

- day-file preamble and binary-start marker;
- schema text includes field names, types, byte lengths, endian, CRC, string, sample, and recovery rules;
- clean single and multi-record day files;
- empty day file after preamble;
- no durable slot ID stored in finalized SD records;
- no stored `addr16` unless explicitly approved;
- ROM64+offset index entries;
- valid sensor blocks with labels;
- bounded label length behavior;
- duplicate ROM64 entries;
- bad index offset;
- offset outside record;
- bad block magic/sentinel;
- bad block bytes;
- bad descriptor bytes;
- bad sample count/sample bytes/sample encoding;
- bad header CRC;
- bad payload CRC;
- bad block CRC;
- partial header;
- partial index;
- partial sensor block;
- dangerous sizes before reading/seeking;
- unsupported version;
- corrupt tail after valid records;
- invalid at offset zero.

### Required recovery tests

Recovery validation must cover:

- clean and empty files allow append;
- corrupt tail blocks append until approved recovery handles it;
- invalid-at-zero blocks append or faults;
- repair preserves valid prefix;
- repair failure blocks append and reports clear fault;
- interrupted repair is idempotent if mutating repair exists;
- chart/query only exposes validated records.

### Required runtime/chart tests

Runtime aggregation and chart migration validation must cover:

- snapshot cadence and rollover;
- immediate mid-hour sensor discovery;
- missing/corrected bitmap behavior;
- labels captured at record time;
- sensor movement by ROM64 with node context update;
- SD absent/unavailable behavior;
- no SD I/O in UI callbacks;
- no large heap/stack regression;
- no full-history vector copy for large chart ranges;
- 1d/2d/5d/7d/30d chart ranges remain responsive.

### Build/environment checks

Codex tasks should run relevant compile/static checks available in the repo/environment.

Task receipts must distinguish host-tested pure codec behavior from Arduino/SD wrapper behavior. Arduino/ESP32 compile and SD hardware behavior remain environment-limited unless actually run on the target/toolchain and reported with exact board/settings.

## 7. Documentation requirements

Anchor files must be kept current when product intent, roadmap sequence, binary format, recovery behavior, or constraints change.

Every new finalized day file must begin with a bounded ASCII preamble that acts as a compact reverse-engineering guide. The preamble must not be vague prose only; it must list binary structures and fields with types and byte lengths.

The preamble must be tested against the implemented constants/schema so it does not drift silently from the writer/scanner.

Task receipts must summarize decisions and validation, not paste large raw receipts.

Documentation must clearly mark hardware-unverified assumptions.

## 8. Deployment/environment constraints

Target GUI node is the Waveshare ESP32-S3-Touch-LCD-7 class board used by MeshTemps.

Primary archive medium is TF/microSD on the GUI node.

FRAM is optional/later and must not block RAM-first SD archive work. If later used, exact module, size, address, wiring, and bus behavior must be verified.

The firmware must tolerate SD absent, SD mount failure, SD read/write failure, and reset/power loss during staging or append.

SD card endurance is not expected to be the limiting factor at MeshTemps logging rates; durability focus is power-loss recovery, filesystem safety, controller behavior, diagnostics, and conservative append blocking.

## 9. Code style and maintainability requirements

Keep storage primitives small, explicit, and host-testable.

Separate pure byte-format logic from Arduino `File`/FS wrappers.

Use explicit status/result types; avoid ambiguous `bool`-only APIs for storage/recovery decisions when diagnostics matter.

Keep destructive/mutating storage operations isolated from read-only scanner/classifier code.

Use deterministic names, deterministic ordering where it helps tests, and deterministic cleanup rules for temporary/quarantine files.

Prefer simple fixed-width sample encoding and explicit bitmaps over clever compression unless a later task proves the need.

Avoid hidden global state except approved file-scope static I/O buffers with documented single-writer ownership.

## 10. Prompting and Codex receipt formatting requirements

Codex-facing prompts must be self-contained and include:

- exact repo/branch/PR/head SHA to inspect when known;
- required current-source inspection;
- scope and explicit exclusions;
- tests/checks to run;
- receipt format;
- requirement to distinguish code inspection, tests run, and unverified assumptions.

Codex receipts must report:

- repository path or remote branch/PR actually inspected;
- actual head SHA;
- changed files;
- tests/commands run and exact results;
- tests not run and why;
- assumptions;
- deviations from task;
- whether PR/branch is draft/ready/merged if relevant.

A local checkout named `work` is not a product branch name. Receipts must report the actual remote branch/PR/head SHA.

## 11. Scope-control rules

Do not start Task 10D runtime aggregation until 10C-FMT0/FMT1/FMTV and v2 recovery/append guard gates pass, unless the user explicitly chooses to defer recovery and keep runtime SD finalization disabled/guarded.

Do not combine v2 format, mutating recovery, runtime aggregator, reader/query, chart migration, FRAM, pruning, and GUI diagnostics into one task.

Do not broaden a storage-format task into unrelated GUI styling, map layout, buzzer behavior, mesh transport, leaf calibration, Wi-Fi, or root-node behavior.

Do not preserve stale comments/docs/tests just because they existed. Treat them as claims to verify.

## 12. Known project-specific hazards

- Legacy `std::vector<SensorHistorySample>` RAM history rings and full-history copy paths can trigger heap pressure and UI freezes.
- Current v1 finalized-hour binary layout is slot/minute-major and does not match final v2 intent.
- Current scanner/recovery tests may encode v1 assumptions and must be updated before being cited as final confidence.
- Large `HistoryHourSnapshot` objects must not be stack-allocated in callbacks, loops, small FreeRTOS tasks, LVGL handlers, or mesh callbacks.
- Power loss during FAT32 append/write/flush/close is expected.
- `file.flush()`/`file.close()` alone are not a durability strategy.
- Arduino/ESP32 FS truncate/rename behavior must not be assumed safe without direct verification.
- Labels can change; store them as historical context but never key identity by label.
- Node assignments can change; preserve continuity by ROM64, not node ID.
- Existing anchors outside project intent/roadmap may be stale until updated.

## 13. Requirements that are settled

Settled requirements:

- RAM-first current-hour staging is first.
- FRAM is optional later.
- SD is authoritative long-term archive.
- Finalized SD archive must be sensor-major v2, not durable-slot/minute-major v1.
- No durable `slot_id` in finalized SD records.
- ROM64 is the finalized SD identity.
- Node ID is context only.
- Node label and sensor label should be stored as historical context in finalized sensor blocks.
- `addr16` should be derived, not stored by default.
- Sensor blocks store 60 fixed-width minute samples with presence/corrected bitmaps.
- Day files need a bounded ASCII schema/preamble before binary records.
- Block magic/sentinel is an additional validation aid only.
- CRCs are required; FEC is deferred.
- Use explicit lengths/counts/offsets/versions/flags, not raw C struct ABI.
- Production finalized-hour writer/verifier must remain bounded/heap-free and single-writer/non-reentrant.
- Runtime SD finalization must not be enabled normally before recovery/append guard is validated.

## 14. Requirements that are provisional or need user decision

Provisional/open requirements:

- Exact v2 field order, constants, magic values, flag definitions, sample encoding enum, and CRC variant.
- Maximum node label and sensor label byte lengths.
- Whether sensor blocks must be sorted by ROM64 for deterministic output despite the index table.
- Whether one bad sensor block invalidates the whole hour record. Initial recommendation: yes, whole record invalid unless all required CRCs pass.
- Whether v1 records need any compatibility path. Current assumption: no, because v1 is not deployed.
- Exact day-file preamble wording and test method for schema drift.
- Truncate vs copy/replace/quarantine vs fault-only recovery policy.
- Recovery timing: at `SdHistoryStore::Begin()`, lazily before append/query, or both.
- Serial-only vs GUI-visible diagnostics for first recovery/runtime pass.
- Behavior when time is invalid at boot or hour rollover.
- Retention/pruning policy.
- Whether and when to add FRAM.

## 15. Last updated context

Inspected current source/context for this requirements update:

- No current local repository snapshot was available in the active environment. GitHub was used as source of truth.
- GitHub repository: `rasusmilch/MeshTemps`.
- Branch inspected and updated: `feature/ram-backed-sd-hist`.
- Current branch head before this requirements update: `d69392d1a38bc1ae2cdc58b4d939d587498048cb`.
- Source files inspected:
  - `MeshTemps-GUINode/history_hour_stager.h`
  - `MeshTemps-GUINode/sd_finalized_hour_block.h`
  - `MeshTemps-GUINode/sd_finalized_hour_recovery_policy.h`
  - `MeshTemps-GUINode/mesh_node.h`
- Anchor files inspected:
  - `anchors/meshtemps_project_intent_anchor.md`
  - `anchors/meshtemps_roadmap_anchor.md`
  - `anchors/meshtemps_requirements_constraints_anchor.md`
  - previous inspection context from `anchors/README.md`, `anchors/meshtemps_current_next_action_anchor.md`, and `anchors/meshtemps_sd_durability_recovery_anchor.md` during the immediately preceding anchor updates.
- Prior uploaded/context references used in summarized form:
  - MeshTemps history storage handoff dated 2026-06-07.
  - PR #54 scanner work and PR #55 recovery-policy work as represented by current GitHub PR metadata and prior receipts.
  - Current user clarification in this chat: no slot ID in finalized SD records; ROM64-only sensor identity; include node/sensor label snapshots; include ROM64+offset sensor index table; include block bytes; include block magic/sentinel only as additional safeguard; include day-file ASCII schema preamble with field names, field types, byte lengths, and parser guidance.
- Expected but missing/unverified:
  - No current local snapshot was inspected.
  - No compile/tests were run for this anchor-only update.
  - No hardware validation was performed.
  - Decision log and validation ledger anchors were not found during the prior anchor search.
