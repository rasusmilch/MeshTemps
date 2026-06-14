# Project Intent Anchor

Project: MeshTemps  
Area: GUI-node history storage, SD archive, recovery, and chart hardening  
Anchor purpose: Guide future ChatGPT/Codex planning, execution, validation, and review tasks.  
Status: Committed repository anchor; updated after PR #55 merge and user clarification of finalized-hour SD archive intent.  
Last updated: 2026-06-11

## 1. Project purpose

MeshTemps is an ESP32-based mesh temperature monitoring system. Leaf nodes read DS18B20 temperature sensors, apply calibration/correction, and report corrected/display temperatures to the GUI node. The GUI node displays live room/sensor values and historical chart views.

The current workstream replaces fragile retention-scaled GUI-node RAM history vectors with a bounded current-hour staging pipeline and a durable SD finalized-hour archive.

The product goal is reliable multi-day/multi-week chart history without ESP32 panics, UI freezes, large retention-scaled RAM vectors, unbounded heap growth, or loss of historical context when labels, nodes, mappings, or sensors change over time.

## 2. Primary users/operators/systems

Primary users/operators:

- MeshTemps owner/operator using the GUI touchscreen.
- Developer/operator using serial diagnostics, fake-history tools, SD/recovery checks, and hardware validation.
- Future ChatGPT/Codex workflows producing firmware changes.

Primary systems:

- MeshTemps GUI node on Waveshare ESP32-S3-Touch-LCD-7.
- MeshTemps leaf nodes with DS18B20 sensors and calibration/correction logic.
- Mesh/root transport delivering `temps` messages to the GUI node.
- Current target backend: bounded RAM current-hour stager.
- Later optional backend: I2C FRAM current-hour stager behind the same staging interface.
- TF/microSD card on the Waveshare board for finalized history archive.
- LVGL chart UI and range buttons.

## 3. Core workflows

### Live temperature workflow

Leaf nodes read DS18B20 sensors, apply calibration/correction, and transmit corrected/display temperature as `tC` with a `corr` flag. The GUI node updates live `MeshNode::Sensor` state and UI labels/ranks.

### Durable history workflow

Canonical workflow:

```text
live mesh sensor state
  -> periodic GUI-node snapshot / aggregator
  -> backend-agnostic current-hour stager
       -> RamHourStager first
       -> FramHourStager later if explicitly chosen
  -> sensor-major SD finalized-hour archive
  -> SD reader/query/reduction layer
  -> chart UI
```

RAM or FRAM is only current-hour staging. SD is the authoritative long-term archive.

### Chart workflow

The user taps a room/sensor and views ranges such as 1d, 2d, 5d, 7d, and 30d. Future chart code should query a storage/history service and stream/reduce records into chart buckets. It must not copy full per-sensor histories into RAM and must not perform long synchronous scans inside LVGL event callbacks.

### Diagnostic workflow

Serial diagnostics should expose storage health, current-hour status, SD archive status, recovery/CRC/error counters, append-blocked state, and whether chart data is coming from legacy RAM or validated SD records. GUI diagnostics may be later work.

## 4. Desired product behavior

The GUI node should survive multi-week operation with charts enabled.

History logging must not allocate large vectors based on retention days. The old per-sensor `std::vector<MeshNode::SensorHistorySample>` retention model is a legacy hazard and must be bypassed, isolated, capped, or removed from the durable-history path.

Current-hour staging must remain bounded, backend-neutral, and RAM-first for the initial implementation. FRAM remains optional later hardware, not a requirement for the first SD-backed rewrite.

SD finalized-hour storage must be a durable, auditable, self-describing, sensor-major archive:

- one finalized hour record per completed hour;
- one sensor block per sensor seen at least once during that hour;
- ROM64 as the canonical sensor identity;
- node ID only as reporting provenance/context;
- node label and sensor label captured as historical context at the time of logging;
- one fixed-width sample position per minute for each emitted sensor block;
- presence bitmap marking valid minutes;
- corrected bitmap marking corrected samples;
- temperatures stored as signed fixed-point centi-C, not floats;
- explicit lengths, counts, offsets, magic/sentinel values, and CRCs;
- validated records only exposed to chart/query code.

The archive should remain understandable years later even if current labels, node mappings, NVS state, or live configuration are lost.

## 5. Current known behavior

Current source still contains a slot/minute-major staging model. `HistoryHourSnapshot` contains 64-slot descriptors and 60 minute frames. `HistoryMinuteFrame` stores presence bitmap, corrected bitmap, and `temp_c_x100[64]`. The current finalized-hour writer serializes each minute frame by writing all presence bytes, corrected bytes, and all 64 temperature slots.

That current v1 binary behavior does not match the clarified SD archive intent. Slot IDs and fixed 64-slot minute frames may remain internal current-hour staging details, but they must not be treated as the desired finalized SD binary ABI.

PR #54 added a read-only finalized-hour append-file scanner for the existing finalized-hour format. PR #55 added a non-destructive recovery policy seam/classifier. Both are useful recovery architecture work, but future work must account for the clarified v2 sensor-major SD format before building more repair/quarantine behavior around the v1 layout.

Known legacy hazard: the GUI history path has used per-sensor RAM `std::vector<MeshNode::SensorHistorySample>` rings. The observed failure class included UI freezes/panics during larger history/chart use and unsafe vector resizing under retention-scaled settings.


### Finalized-hour v2 ABI ownership and no-padding intent

The finalized-hour v2 byte format is a new authoritative archive ABI, not a compatibility wrapper around v1. It must derive byte sizes from the semantic field list rather than preserving mistaken byte counts.

Correct semantic sizes are: HourRecordHeaderV2 48 bytes, SensorIndexEntryV2 12 bytes, SensorBlockHeaderV2 32 bytes, SensorDescriptorV2 106 bytes, SensorPayloadV2 136 bytes, and fixed SensorBlockV2 274 bytes. Block CRC offset is 24. descriptor_flags offset is 22.

Do not add `reserved0`, fake padding, generic reserved bytes, or alignment filler to finalized-hour v2 unless a future product decision approves a named semantic expansion field with explicit purpose and tests.

The v2 format module owns v2 ABI constants and must not depend on current-hour staging internals or legacy/transitional stager headers such as `history_hour_stager.h`. The v2 on-disk invalid-sample sentinel should have a v2-specific name and local ownership unless a future task proves a neutral shared-domain owner is warranted. Do not create a shared header solely for this sentinel.

## 6. Explicit non-goals and exclusions

Do not treat fixed 64-slot minute-frame SD records as the intended final archive format.

Do not store `slot_id` in finalized SD records.

Do not store `addr16` in finalized SD records unless a future task proves a specific need. `addr16` is a printable form derived from ROM64.

Do not use labels as primary identity. Labels are historical annotations.

Do not use `node_id` as primary sensor identity. Node ID is reporting provenance/context.

Do not stream every mesh packet to SD. SD receives finalized hourly batches.

Do not make rollups the only retained record. Raw finalized minute-level hour records are authoritative. Rollups/indexes may be added only as rebuildable acceleration data.

Do not implement production SD finalized-hour writing by building a full-record heap vector or reading a full finalized-hour record into a heap vector for verification.

Do not use Arduino `String`, `std::string`, `std::vector`, `std::function`, printf-family formatting, `malloc`/`new`, libc calendar/timezone conversion, or hidden dynamic allocation in low-level finalized-hour path construction/serialization/verification primitives.

Do not hold mesh/history locks across SD I/O.

Do not claim hardware validation, compile success, or test success unless actually run and shown in the receipt.

Do not continue recovery repair/quarantine implementation against v1 layout without first resolving the v2 format plan.

## 7. Canonical terminology

GUI node: ESP32-S3 touchscreen node that displays live values and charts.

Leaf node: ESP32 node that reads DS18B20 sensors and sends corrected/display values.

ROM64: DS18B20 64-bit physical sensor identity. This is the canonical durable sensor key in SD history.

addr16: 16-character printable hex representation of ROM64. Derive for display/debug; do not store in finalized SD records by default.

Node ID: mesh node ID that reported a sensor. Provenance/context only, not sensor identity.

Node label: human-readable node name at time of recording. Historical context.

Sensor label: human-readable sensor/location name at time of recording. Historical context.

Current-hour stager: bounded RAM/optional-FRAM component holding only the current hour before SD finalization.

Slot ID: internal current-hour staging handle only. Deprecated as finalized SD terminology.

Finalized hour record: one immutable SD archive record for one completed hour.

Sensor block: v2 SD record sub-block for one ROM64 sensor seen during that hour.

Sensor index table: v2 SD record table mapping ROM64 to sensor block offset.

Presence bitmap: 60-minute bitset marking which minute samples are valid for a sensor block.

Corrected bitmap: 60-minute bitset marking which valid minute samples were corrected values.

Centi-C: signed fixed-point temperature encoding where `stored = round(temp_c * 100)`.

Day-file preamble: human-readable ASCII schema/reverse-engineering guide written once at the beginning of each new finalized day file before binary records.

## 8. Misleading or deprecated terminology/patterns to avoid preserving

Deprecated or misleading:

- finalized-hour SD format described as a slot catalog plus fixed 64-slot minute frames;
- `slot_id` as durable SD identity;
- `addr16` stored redundantly beside ROM64;
- labels/ranks as durable keys;
- node ID as sensor identity;
- retention-scaled per-sensor RAM `std::vector<SensorHistorySample>` as durable history;
- full selected-sensor history copies before chart preparation;
- vector-backed full-record finalized-hour encoder helpers as production or primary-test APIs;
- descriptor records mixed into a wrapping sample stream;
- sentinel scanning as the primary parser boundary;
- “FRAM history” if it implies long-term FRAM storage;
- “rollup storage” if it implies rollups replace raw records.

Internal slot/minute-major staging may continue if bounded and useful, but future tasks must not preserve the current v1 SD binary layout just because current staging is slot-indexed.

## 9. Data/state lifecycle assumptions

### Sensor data

Leaf nodes send corrected/display temperature as `tC` and corrected status as `corr`. Durable history stores the received display/corrected value as centi-C and preserves corrected status.

### Current-hour staging lifecycle

The current-hour stager stores only active current-hour state. First implementation is RAM. Optional later FRAM backend may recover the current unflushed hour across reset/power loss, but FRAM is not long-term history.

The stager may use temporary slot IDs internally to collect the hour. The finalized SD archive must serialize by sensor identity, not by durable slot ID.

RAM-first staging may lose the current unflushed hour on reset/power loss. This is acceptable for the first implementation if documented and surfaced in diagnostics.

### SD archive lifecycle

Finalized SD day files contain:

```text
ASCII day-file preamble / compact schema guide
%%MESH_TEMPS_BINARY_START%%
HourRecordV2
HourRecordV2
...
```

The preamble must be bounded, deterministic for a format version, and useful as a minimal reverse-engineering guide. It must include field names, field types, field byte lengths, global endianness, string encoding, CRC type, temperature encoding, file layout, hour record layout, sensor index layout, sensor block layout, payload layout, and recovery/validity rules.

Binary parsing must not depend on the prose. It must use explicit binary lengths, counts, offsets, magic/sentinel values, and CRCs.

### Finalized-hour v2 lifecycle

Each hour record should contain:

```text
HourRecordHeaderV2
SensorIndexTableV2: repeated { rom64, sensor_block_offset_from_record_start }
SensorBlockV2 repeated sensor_count times
```

Each sensor block should contain:

```text
SensorBlockHeaderV2 with block magic/sentinel, lengths, encoding, CRC, flags
SensorDescriptorV2 with ROM64, node ID/context, labels, counts, first/last seen
SensorPayloadV2 with 60-minute presence bitmap, corrected bitmap, and 60 int16 centi-C samples
```

A sensor block is emitted for every sensor seen at least once during the hour. It still contains 60 fixed-width sample positions; absence is represented by presence bitmap bits, not by omitting minute positions.

### Label lifecycle

ROM64 is identity. Node and sensor labels are historical context snapshots. Store them in each finalized sensor block so old files remain understandable without the current GUI/NVS mapping. Labels should be length-prefixed bounded UTF-8/ASCII bytes, not null-terminated strings and not unbounded heap strings.

## 10. Safety, security, permission, validation, logging, audit, and reliability expectations

Storage must be bounded and allocation-safe.

Production finalized-hour SD writer/verifier paths must use streaming/fixed buffers and must avoid large stack buffers, heap allocation, and hidden dynamic allocation. Current finalized-hour write buffer policy remains: file-scope static 4096-byte write coalescer and file-scope static 512-byte verification buffer, single-writer/non-reentrant.

Finalized-hour write order remains:

```text
complete record write
  -> coalescer flush
  -> file.flush()
  -> file.close()
  -> reopen/read-back verification
```

Validated records only may be exposed to chart/query code. If a record fails validation, the reader/scanner must stop at the last valid record or otherwise follow an approved recovery policy.

Power loss during FAT32 append/write/flush/close is expected. `file.flush()` and `file.close()` are required but are not a complete durability strategy.

Appender code must not append after a corrupt tail unless approved recovery has made the file safe or the system has explicitly faulted/blocked appending.

Repair/quarantine must preserve valid prefixes and must never silently delete the only valid copy of history. If truncate support is unavailable or untrusted, prefer explicit fault behavior or copy/replace/quarantine only after a reviewed policy.

Serial commands that format, wipe, repair, quarantine, or mutate storage must be explicit and guarded by clear confirmation syntax where practical.

Diagnostics must distinguish clean, empty, corrupt tail, invalid-at-zero, unsupported format, dangerous header/size, read error, repair attempted/completed/failed, and append-blocked states.

## 11. Architecture/model principles

Use service-layer storage interfaces over direct UI/chart access to `MeshNode` history vectors.

Dependency direction:

```text
MeshNode live state
  -> HistoryAggregator / snapshot cadence
  -> IHistoryHourStager or equivalent
       -> RamHourStager first
       -> FramHourStager later if approved
  -> finalized-hour SD writer
  -> finalized-hour scanner/reader/query service
  -> chart reduction/UI
```

The current-hour stager format is not the SD binary ABI. The SD writer should be free to serialize sensor-major v2 records even if RAM staging remains slot/minute-major internally.

Use ROM64 as durable identity. Use labels and node context as historical annotations. Use centi-C fixed-point storage. Use CRCs before FEC unless a later task proves a need for FEC.

Use explicit lengths, counts, offsets, versions, flags, and CRCs instead of reserved padding or raw C struct serialization. Serialize fields in a defined order and endian form; do not serialize compiler-padded structs directly.

Block magic/sentinel is allowed as an additional safeguard, preferably at the beginning of each sensor block. Do not use sentinel hunting as the normal parser boundary.

## 12. Known risks and failure scenarios

RAM-first staging loses the current unflushed hour on reset/power loss.

SD may be absent, corrupted, slow, fail to mount, or lose power during append/flush/close.

Time may be invalid at boot. Durable hour naming/finalization should wait for sane epoch time or use a documented recovery/skipping policy.

A corrupt day-file preamble may make human recovery harder, but binary records still self-identify after the binary-start marker. The marker itself should be short, fixed, and tested.

Changing from v1 fixed 64-slot SD layout to v2 sensor-major layout invalidates any assumptions in current scanner/writer tests that treat v1 as final. Existing scanner/policy seams remain useful but must be updated or replaced for v2.

Sensor labels and node labels may change over time. Store label snapshots for audit, but do not key continuity by label.

A physical DS18B20 sensor may move to another node. History should remain continuous by ROM64 while reporting context updates.

More than the supported active sensor count in an hour is an overflow condition. Behavior must be explicit.

## 13. Open product decisions

Open / deferred decisions after this update:

- Exact finalized-hour v2 field order and constants for header magic, block magic, schema ID, flags, sample encoding, and CRC variant.
- Maximum byte length for node label and sensor label; likely 32 or 48 bytes each, but not yet finalized.
- Whether sensor blocks should be sorted by ROM64 for deterministic tests even though the index table allows direct lookup.
- Whether per-sensor block CRC failure invalidates the whole hour record or can later allow partial-sensor salvage. Initial recommendation: whole record invalid unless all required CRCs pass.
- Runtime v1 support is no longer open for finalized-hour archives: product decision is no v1 compatibility because v1 is not deployed; no production v1 data, no v1 migration, and no v1 dual-format support.
- Exact day-file preamble wording and how tests prevent schema drift from binary constants.
- Whether recovery repair uses truncate, copy/replace/quarantine, or explicit fault-only behavior.
- Whether recovery runs at `SdHistoryStore::Begin()` or lazily before appending/querying a specific day file.
- Initial diagnostic exposure: serial-only, GUI, or both.
- Whether and when to add optional FRAM backend after RAM-first SD archive works.

## 14. References and related anchors

Use the repository anchor reading order in `anchors/README.md`.

Related anchors:

- `anchors/meshtemps_current_next_action_anchor.md` — current sequencing authority, but now needs a follow-up update to insert the v2 format planning gate before mutating recovery work.
- `anchors/meshtemps_roadmap_anchor.md` — task sequence and storage/charts roadmap; parts referring to fixed 64-slot SD blocks are stale for finalized SD format.
- `anchors/meshtemps_requirements_constraints_anchor.md` — constraints; parts referring to labels as optional UI-only metadata are stale for finalized SD archive context.
- `anchors/meshtemps_testing_hardening_anchor.md` — test-quality requirements.
- `anchors/meshtemps_sd_durability_recovery_anchor.md` — recovery contract; v1-specific field validation must be updated for v2 before 10C-F2-B/C.

Decision log and validation ledger anchors were searched for by name but were not found in this inspection pass.

Recommended next workflow change:

```text
10C-FMT0 — Plan finalized-hour v2 sensor-major ROM64-indexed day-file/archive format
  -> 10C-FMT1 — Implement v2 writer/scanner/tests or update existing writer/scanner to v2
  -> 10C-FMTV — Validate v2 format, preamble, and scanner
  -> resume 10C-F2-B/C repair/quarantine/fault implementation against v2
  -> 10D runtime HistoryAggregator snapshot path
```

## 15. Last updated context

Inspected current source/context for this update:

- No current local repository snapshot was available in the active environment. GitHub was used as source of truth.
- GitHub repository: `rasusmilch/MeshTemps`.
- Branch inspected and updated: `feature/ram-backed-sd-hist`.
- Current branch head before this anchor update: merge commit `6bafc4e34b2ba2349c1b828261d5b5573c20ebfb` from PR #55.
- PR #55 inspected: merged, title `Add non-destructive finalized-hour recovery policy and diagnostics with tests`, head `32f70f41739e093fe3df77789d646cb3f8028b37`, base `d455a2e21cc8777c7d355eee0332157e7e2d55f7`, merge commit `6bafc4e34b2ba2349c1b828261d5b5573c20ebfb`.
- Source files inspected:
  - `MeshTemps-GUINode/history_hour_stager.h`
  - `MeshTemps-GUINode/history_hour_stager.cpp`
  - `MeshTemps-GUINode/sd_finalized_hour_block.h`
  - `MeshTemps-GUINode/sd_finalized_hour_block.cpp`
  - `MeshTemps-GUINode/mesh_node.h`
  - `MeshTemps-GUINode/mesh_node.cpp`
  - `MeshTemps-RootNode/MeshTemps-RootNode.ino` for room/label context.
- Anchor files inspected:
  - `anchors/README.md`
  - `anchors/meshtemps_current_next_action_anchor.md`
  - `anchors/meshtemps_project_intent_anchor.md`
  - `anchors/meshtemps_roadmap_anchor.md`
  - `anchors/meshtemps_requirements_constraints_anchor.md`
  - `anchors/meshtemps_sd_durability_recovery_anchor.md`
- Prior uploaded/context references used in summarized form:
  - MeshTemps history storage handoff dated 2026-06-07.
  - PR #54 scanner work and PR #55 recovery-policy work as represented by current GitHub PR metadata and prior receipts.
  - Current user clarification in this chat: no slot ID in SD records; ROM64-only sensor identity; include node/sensor label snapshots; include sensor index table; include block bytes; include block magic/sentinel only as additional safeguard; include day-file ASCII schema preamble with field names, types, byte lengths, and parser guidance.
- Expected but missing/unverified:
  - No current local snapshot was inspected.
  - No hardware validation was performed.
  - No compile/tests were run for this anchor-only update.
  - Decision log and validation ledger anchors were not found by repository search.
