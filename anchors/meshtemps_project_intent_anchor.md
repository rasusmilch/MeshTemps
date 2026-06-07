# Project Intent Anchor

Project: MeshTemps  
Area: GUI-node history storage and chart hardening  
Anchor purpose: Guide future ChatGPT/Codex planning, execution, and validation tasks.  
Status: Intent anchor artifact; not yet committed to the repository.  
Last updated: 2026-06-07

## 1. Project purpose

MeshTemps is an ESP32-based mesh temperature monitoring system. Leaf nodes collect DS18B20 temperature readings, apply per-sensor calibration/correction, and report values through the mesh. The GUI node displays current room/sensor temperatures and historical chart views.

The current workstream is to replace fragile GUI-node per-sensor RAM history vectors with a bounded, backend-agnostic current-hour staging pipeline that writes finalized, self-describing history blocks to SD.

The immediate implementation direction is **RAM-first current-hour staging plus SD finalized-hour archive**.

The architecture must remain open to a later **Adafruit-style I²C FRAM current-hour staging backend** connected to the Waveshare ESP32-S3-Touch-LCD-7 exposed I²C header. FRAM is optional/later hardware, not required for the first storage rewrite.

The product goal is reliable multi-day/multi-week chart history without ESP32 panics, UI freezes, large retention-scaled RAM vectors, or loss of sensor identity when mesh sensors appear, disappear, or move.

## 2. Primary users/operators/systems

Primary users/operators:

- MeshTemps owner/operator using the GUI touchscreen.
- Developer/operator using serial diagnostics, fake-history tools, storage status checks, and hardware validation.
- Future Codex/ChatGPT workflows producing firmware changes.

Primary systems:

- MeshTemps GUI node on Waveshare ESP32-S3-Touch-LCD-7.
- MeshTemps leaf nodes with DS18B20 sensors and calibration/correction logic.
- Mesh/root transport delivering `temps` messages to the GUI node.
- Current target backend: bounded RAM current-hour stager.
- Later optional backend: Adafruit-style I²C FRAM current-hour stager on the Waveshare exposed I²C header.
- TF/microSD card on the Waveshare board for finalized history archive.
- LVGL chart UI and range buttons.

## 3. Core workflows

### Live temperature workflow

Leaf nodes read raw DS18B20 temperatures, apply stored calibration coefficients, and transmit corrected output temperature as `tC` with a `corr` flag indicating whether a non-identity correction was applied.

The GUI node receives the message and updates live `MeshNode::Sensor` state:

- physical sensor address / DS18B20 ROM hex string,
- latest corrected/display temperature,
- valid-value state,
- corrected-value state,
- last receive timestamp,
- UI label/rank metadata.

### Durable history workflow

Canonical workflow:

```text
live mesh sensor state
  -> periodic GUI-node snapshot
  -> backend-agnostic current-hour stager
       -> RamHourStager first
       -> FramHourStager later if wanted
  -> verified SD finalized-hour block
  -> chart/query layer reads SD history and/or derived indexes
```

RAM or FRAM is only current-hour staging. SD is the authoritative long-term archive.

### Chart workflow

User taps a room/sensor and views ranges such as 1d, 2d, 5d, 7d, and 30d.

Future chart code should query a storage/history service and stream/reduce into chart buckets. It should not copy full per-sensor histories into RAM and should not run long scans synchronously in LVGL event callbacks.

### Diagnostic workflow

Serial diagnostics should expose:

- current stager backend: RAM now, FRAM later if enabled,
- SD status and finalized-hour archive health,
- active current-hour state,
- slot catalog contents,
- current slot count versus 64-slot cap,
- current minute/frame status,
- SD flush status,
- recovery/CRC/error counters,
- chart storage source.

If FRAM is later implemented, diagnostics should also expose FRAM detection, address/size assumptions, metadata/catalog/frame status, and recovery state.

## 4. Desired product behavior

The GUI node should survive multi-week operation with charts enabled.

History logging must not allocate large vectors based on retention days. The old retention-scaled RAM history model must be replaced or tightly isolated.

Current-hour staging must:

- be accessed through an interface that hides whether the backend is RAM or FRAM,
- cap active history slots at 64 for now,
- store a slot catalog mapping compact slot IDs to durable sensor identity,
- allow sensors to appear mid-hour and be logged immediately,
- treat prior minutes for newly seen sensors as missing,
- preserve corrected-temperature status,
- store temperatures as fixed-point centi-C, not floats,
- use checksums/CRCs where practical,
- avoid descriptor records inside a wrapping sample stream.

RAM-first behavior:

- current unflushed hour can be lost on reset/power loss;
- this is acceptable for the first implementation if documented and surfaced in diagnostics;
- SD finalized-hour format must not depend on whether RAM or FRAM produced the hour.

Later FRAM behavior:

- FRAM should provide current-hour recovery across reset/power loss;
- FRAM should use the same logical stager interface and export the same finalized-hour structure to SD;
- FRAM hardware details must not leak into chart, SD writer, or MeshNode code.

SD finalized-hour storage must:

- receive finalized hour batches from the current-hour stager,
- store a complete descriptor/catalog snapshot with every hour block,
- store raw minute-level data as authoritative history,
- optionally store derived indexes/rollups only as rebuildable acceleration data,
- verify writes where practical,
- remain decodable even if FRAM/NVS/current labels are lost,
- use **heap-free SD finalization** in production: no large dynamic allocations and no full-record heap buffers in the finalized-hour writer or read-back verifier.

Production heap-free SD finalization means:

```text
live/staged snapshot
  -> streaming finalized-hour encoder
  -> bounded File.write chunks
  -> flush/close completed record
  -> reopen/read-back verification with a fixed-size buffer
```

Read-back verification must happen after the complete SD record has been written and flushed/closed. Do not interleave write/read/verify chunks during the normal write path. Finalized-hour encoders and tests should use streaming or fixed-size capture buffers; do not preserve vector-backed finalized-hour full-record helpers as an API or test pattern. Finalized-hour SD path construction must also use custom bounded append helpers with caller-owned fixed `char` buffers: no Arduino `String`, `std::string`, `snprintf`/`sprintf`/`asprintf`/printf-family formatting, `malloc`/`new`, or hidden/dynamic path building.

Current-hour RAM/FRAM staging must stay bounded, and SD finalization must also stay bounded. Do not call SD finalization from LVGL callbacks. Do not hold mesh/history locks across SD I/O.

Mesh churn is expected:

- sensor disappears briefly: absence/presence bit marks missing samples,
- sensor returns: reuse same slot if in current catalog,
- new sensor appears mid-hour: assign next free slot immediately and log current/future samples,
- sensor moves to another leaf: preserve DS18B20 ROM identity and update last-known node ID,
- label changes: do not split physical history.

## 5. Current known behavior

The current GUI history implementation has used a per-sensor `std::vector<MeshNode::SensorHistorySample>` ring buffer.

The current sample object has contained monotonic milliseconds, optional epoch, float temperature, and flags. It has been observed to occupy 24 bytes per sample on ESP32-S3.

The known failure mode is a panic when the GUI node attempts to resize per-sensor history vectors under unsafe history settings. The failure path is through `MaybeLogHistorySample()` and `std::vector<MeshNode::SensorHistorySample>::resize()`. Legacy per-sensor `std::vector` history remains a known allocation hazard until isolated, bypassed, or removed from the durable-history path.

Chart range buttons previously froze the UI with more than roughly one day of history. Earlier work added fake-history diagnostics, chart-series preparation seams, and ring-buffer helpers, but the storage allocation model remains the larger blocker.

Leaf nodes already send corrected/display temperatures, not both raw and corrected values. The GUI stores incoming `tC` as the sensor temperature and stores `corr` as corrected status.

## 6. Explicit non-goals and exclusions

Do not continue chart Phase 3C bounded chart-scan work until storage architecture is planned and the RAM vector panic path is contained or bypassed.

Do not implement a large RAM-budget hardening plan as the long-term architecture.

Do not implement RAM-first staging as another retention-scaled `std::vector<SensorHistorySample>` model.

Do not require FRAM hardware for the first SD-backed storage rewrite.

Do not try to store multi-week history entirely in RAM or I²C FRAM.

Do not make rollups the only retained record. Raw finalized hour blocks are authoritative.

Do not use labels as durable history keys.

Do not use `node_id` alone as durable sensor identity.

Do not mix sensor descriptor/catalog records into a wrapping sample ring.

Do not write every mesh packet to SD. SD receives finalized batches from the current-hour stager.

Do not implement production SD finalized-hour writing by building a complete `std::vector<uint8_t>` record before writing, or by reading a complete finalized-hour record into a heap vector for verification. That pattern is deprecated; finalized-hour tests should use fixed-size capture buffers instead of vector-backed full-record helpers. Do not use Arduino `String` concatenation or printf-family formatting for finalized-hour directory/file path construction.

Do not hold mesh/history locks during long storage I/O.

Do not port PT100 code blindly. Use it only as an adapted template after verifying assumptions and fixing known issues.

Do not redesign unrelated GUI screens, root-node behavior, leaf calibration, mesh transport, or LVGL styling unless a scoped task explicitly requires it.

## 7. Canonical terminology

MeshTemps GUI node: ESP32-S3 touchscreen node that displays current values and charts.

Leaf node: ESP32 node that reads DS18B20 sensors and sends corrected values to the mesh.

Sensor identity: DS18B20 physical ROM/address, represented as `rom64` or a 16-character hex string. This is the durable identity.

Node identity: mesh node ID that last reported a sensor. Useful context, not the primary durable identity.

Slot ID: compact current-history handle assigned to a known sensor identity for staging/SD storage.

Slot catalog: metadata mapping slot IDs to sensor identities and last-known reporting context.

Current-hour stager: backend-agnostic component that stores the current hour's slot catalog and minute frames before SD finalization.

RamHourStager: first implementation of the current-hour stager using bounded RAM.

FramHourStager: later optional implementation of the current-hour stager using I²C FRAM.

Finalized hour block: immutable SD record containing descriptor/catalog snapshot plus raw minute-level samples for one hour.

Presence bitmap: per-minute bitset indicating which slots have valid samples.

Corrected bitmap: per-minute bitset indicating which valid samples were corrected values.

Raw minute data: authoritative minute-resolution finalized samples stored to SD.

Rollup/index: derived summary data for faster charting. Rebuildable; not authoritative.

Centi-C: fixed-point temperature encoding where `stored = round(temp_c * 100)`.

## 8. Misleading or deprecated patterns to avoid preserving

Deprecated:

- per-sensor RAM `std::vector<SensorHistorySample>` sized by retention period,
- full selected-sensor history copy before chart preparation,
- `hist set 60 30` or similar high-retention RAM examples as safe examples,
- `float` plus `time_t` per stored durable history sample,
- “new sensors discovered mid-hour wait until next hour.”

Misleading:

- “FRAM history” if it implies long-term FRAM storage,
- “RAM history” if it implies retention-scaled RAM vectors,
- “rollup storage” if it implies rollups replace raw data,
- descriptor records in the same wrapping stream as sample records.

Canonical direction: **backend-agnostic current-hour staging plus SD finalized-hour archive**.

## 9. Data/state lifecycle assumptions

### Sensor data

Leaf nodes send corrected/display temperature as `tC` and a `corr` flag. Durable history stores `tC` as centi-C and preserves corrected status with a corrected bitmap.

### Current-hour staging lifecycle

The current-hour stager stores only active current-hour state, not long-term history.

First implementation: `RamHourStager`.

Later optional implementation: `FramHourStager`.

Logical staging model:

- hour metadata,
- slot catalog,
- 60 current-hour minute frames,
- export support for SD finalization,
- backend status and diagnostics.

Current product decision: cap slots at 64.

Expected current-hour frame model:

```text
presence bitmap for 64 slots
corrected bitmap for 64 slots
int16 temp_c_x100[64]
frame checksum/CRC
padding/alignment if useful
```

Approximate frame size with 64 slots is around 148 bytes depending on exact CRC/alignment. Sixty frames are roughly 9 KB, so the RAM footprint is bounded and predictable.

### RAM lifecycle

RAM-first staging loses the current unflushed hour if the GUI node resets or loses power before SD finalization.

This is acceptable for the first implementation if documented.

RAM-first staging validates the SD/archive/query architecture before FRAM hardware is required.

### FRAM lifecycle

FRAM is a later optional backend.

If implemented, FRAM should contain sequence/CRC-protected metadata, slot catalog, current-hour frames, and recovery state sufficient to resume or flush after reboot.

Assumed later FRAM hardware: Adafruit-style I²C FRAM module on the Waveshare exposed I²C header. Exact physical module and address remain hardware-unverified.

### SD lifecycle

SD stores finalized immutable hour blocks.

Every finalized hour block should be self-describing and include the descriptor/catalog snapshot needed to decode samples without current FRAM/NVS/RAM state.

### Registry lifecycle

The sensor registry should be reconstructable from SD finalized hour blocks.

If FRAM is later implemented, FRAM may hold active current-hour catalog/recovery state, but SD hour-block descriptor snapshots remain the durable reconstruction source.

### Label lifecycle

Labels and ranks are UI metadata. History storage should store physical identity and possibly label snapshots for convenience, but must not require labels to decode data.

## 10. Safety, validation, logging, audit, and reliability expectations

Storage must be bounded and allocation-safe.

The first implementation must eliminate or bypass the retention-scaled vector allocation failure path for durable history.

For RAM staging:

- fixed-size/bounded allocation must be clear and testable;
- reset/power-loss loss of current hour must be documented;
- SD finalized blocks must be verified where practical.

For later FRAM staging:

- use A/B metadata, sequence counters, CRCs/checksums, or comparable recovery-safe patterns;
- partially written minute frames must not be treated as valid without validation;
- recovery after reset must be tested.

For SD storage:

- partially written SD hour blocks must be detected and ignored or repaired without corrupting prior data;
- writes should be append-only where practical;
- raw finalized hour blocks are authoritative;
- derived rollups/indexes are optional and rebuildable.

Common rules:

- staging operations must be bounded,
- SD writes must not occur in LVGL event callbacks,
- mesh locks must not be held across long storage I/O,
- serial diagnostics should report storage health and failure counters,
- no task/PR may claim hardware validation, compile success, or tests passed unless actually run and shown in the receipt.

## 11. Architecture/model principles

Use service-layer storage interfaces over direct UI/chart access to `MeshNode` history vectors.

Use a `HistoryStore`, `HistoryAggregator`, or equivalent abstraction so chart/query code does not depend on whether data came from RAM, FRAM, or SD.

The central seam is a current-hour staging interface, not a FRAM-specific API.

Illustrative interface shape:

```cpp
class IHistoryHourStager {
 public:
  virtual bool BeginHour(uint32_t hour_start_epoch_minute) = 0;
  virtual bool FindOrCreateSlot(uint64_t rom64, uint32_t node_id, uint16_t* out_slot_id) = 0;
  virtual bool RecordSample(uint8_t minute_offset, uint16_t slot_id, int16_t temp_c_x100, bool corrected) = 0;
  virtual bool ExportHour(HistoryHourSnapshotWriter& writer) const = 0;
  virtual bool ResetAfterFlush(uint32_t next_hour_start_epoch_minute) = 0;
  virtual HistoryStagerStatus GetStatus() const = 0;
  virtual ~IHistoryHourStager() = default;
};
```

Exact C++ names may differ after code inspection, but dependency direction must remain:

```text
MeshNode live state
  -> HistoryAggregator
  -> IHistoryHourStager
       -> RamHourStager first
       -> FramHourStager later
  -> SdHistoryWriter
  -> SdHistoryReader / chart query service
```

Do not let `Adafruit_FRAM_I2C`, `Wire`, raw FRAM addresses, or RAM storage details appear in chart code, MeshNode code, or SD writer logic.

A later FRAM driver should be behind a byte-storage interface similar to:

```cpp
ReadBytes(address, buffer, length)
WriteBytes(address, buffer, length)
```

Separate responsibilities:

- live mesh state owns current readings,
- history aggregator snapshots readings on cadence,
- current-hour stager stages the current hour,
- SD store writes finalized immutable hour blocks,
- chart query layer reads finalized history and reduces/streams for UI,
- diagnostics expose state and failures.

Use fixed current-hour frame shape with a dynamic slot catalog capped at 64.

Use DS18B20 ROM as durable identity.

Use centi-C fixed-point storage for corrected/display temperatures.

## 12. Known risks and failure scenarios

RAM-first staging loses the current unflushed hour on reset/power loss. This is accepted for the first implementation only if documented.

FRAM module capacity is limited. Adafruit-style I²C FRAM is assumed around 32 KB. Designs requiring more than current-hour staging are wrong.

I²C FRAM, if later used, shares the Waveshare I²C bus with touch and CH422G. Bus conflicts, address conflicts, blocking, or overuse can affect UI behavior.

SD may be absent, corrupted, slow, or fail to mount. Behavior for SD unavailable at hour rollover must be defined.

Time may be invalid at boot. Durable hour naming and SD finalization should wait for sane epoch time or use a documented recovery path.

Power can fail during SD flush. If FRAM is later used, power can also fail during catalog/frame update.

New sensors can appear mid-hour. Current decision: assign a new slot immediately if fewer than 64 slots are active in the current-hour catalog.

More than 64 sensors in an hour is an overflow condition. Behavior remains open, but must be explicit.

If a DS18B20 moves to another leaf, history should remain continuous by ROM identity while node context updates.

If labels change, old history remains keyed by physical identity.

PT100 template risk: it contains a known region-size accounting bug in `FramHourJournal` where required region size undercounts one 256-byte header slot. Do not copy that bug.

PT100 template risk: it writes derived hourly/daily rollups. That is acceptable only if raw minute-hour blocks remain authoritative and retained.

## 13. Open product decisions

- Exact SD file layout and naming convention for MeshTemps finalized hour blocks.
- Whether to store optional label/rank snapshots in SD hour descriptors.
- Whether to store derived hourly/daily rollups or only raw hour blocks initially.
- How long to retain raw minute-level SD history.
- UI behavior when a room label maps to a sensor whose physical ROM changed.
- Slot overflow behavior beyond 64 sensors in one hour.
- GUI versus serial-only storage diagnostics.
- Whether and when to add the FRAM backend after RAM-first SD archive is working.
- Exact FRAM module capacity and driver choice remain assumed, not hardware-verified.
- SD flush retry policy when SD is absent or unavailable at hour rollover.
- Whether old RAM history remains as a short live cache or is removed/capped once durable storage exists.

## 14. References and related anchors

Available context references:

- Uploaded handoff: `meshtemps_handoff_history_chart_storage.md`.
- MeshTemps workstream branch referenced in handoff: `fix/chart-hardening`.
- Prior MeshTemps PRs referenced in handoff: PR #50, PR #51, PR #52.
- PT100_Mesh_Datalogger template repository: `rasusmilch/PT100_Mesh_Datalogger`.
- PT100 bug filed from this chat: issue #367, `FramHourJournal undercounts required FRAM region by one header slot`.
- Prior generated but not repo-confirmed roadmap artifact: `meshtemps_history_chart_roadmap_anchor.md`.

Unverified repository anchors:

- No committed MeshTemps project intent anchor was found in a quick connector search.
- The prior roadmap anchor was reported by handoff as a downloadable artifact and not present in the repository at that time.
- A formal validation ledger anchor was not found or inspected in this pass.
- A decision log anchor was not found or inspected in this pass.

## 15. Last updated context

This anchor used:

- Current chat discussion on MeshTemps history freeze, RAM vector allocation failure, FRAM/SD direction, slot/catalog model, and backend-agnostic stager design.
- Uploaded file: `meshtemps_handoff_history_chart_storage.md`.
- Current user clarification:
  - use RAM current-hour staging first;
  - keep helpers/backend interface decoupled so RAM or FRAM can be dropped into the pipeline later;
  - preserve the option for Adafruit-style I²C FRAM on the Waveshare exposed I²C header;
  - cap current-hour history slots at 64 for now.
- Prior code inspections in this chat:
  - MeshTemps GUI `mesh_node.h/.cpp` history structures and logging path.
  - MeshTemps leaf corrected-temperature send path.
  - MeshTemps GUI receive path for `tC` and `corr`.
  - Waveshare ESP32-S3-Touch-LCD-7 board config showing shared I²C bus on GPIO8/GPIO9.
  - PT100_Mesh_Datalogger FRAM/SD storage files.
  - Task 10C review finding that production `SdHistoryStore` finalized-hour write/read-back paths still allocate full-record `std::vector<uint8_t>` buffers.
  - Task 10C-R anchor update requiring heap-free SD finalization and Task 10C-A before Task 10D runtime aggregation.

Current unresolved assumptions:

- Exact FRAM module is assumed to be the Adafruit I²C FRAM breakout class, likely 256 Kbit / 32 KB, but FRAM is no longer required for first implementation.
- Physical FRAM wiring and I²C address scan have not been validated.
- SD card mount/write performance on the target MeshTemps GUI node has not been validated in this workstream.

## 16. Next recommended Codex workflow

Next Codex task should be a focused execute follow-up, not Task 10D.

Recommended task title:

```text
Task 10C-A — Remove production dynamic allocation from finalized-hour SD writer
```

The task should keep the Task 10C finalized-hour file/block format, preserve host-test codec coverage, and replace production full-record heap buffers in `SdHistoryStore::AppendFinalizedHourSnapshot()` and `VerifyFinalizedHourRecord_()` with a streaming/bounded write and read-back verification path. Task 10D runtime aggregation must wait until Task 10C-A checkpoint validation passes.
