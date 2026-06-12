# MeshTemps Decision Log Anchor

Project: MeshTemps  
Workstream: GUI-node history storage, SD archive, recovery, and chart hardening  
Anchor purpose: Record settled, provisional, deprecated, and rejected project decisions for future ChatGPT/Codex planning, execution, validation, and review tasks.  
Status: Created after PR #55 merge and finalized-hour v2 intent/roadmap/requirements clarification.  
Last updated: 2026-06-11

## Decision 1 — RAM-first current-hour staging, SD as long-term archive

Date/reference: Task 10 workstream; reaffirmed 2026-06-11.  
Status: settled.

Decision: MeshTemps will use bounded current-hour staging first in RAM. SD stores finalized long-term history. FRAM remains optional later hardware and is not required for the first storage rewrite.

Rationale: This fixes the immediate RAM-history/chart-freeze class without blocking on extra hardware. Current-hour data loss on reset is acceptable for the first RAM implementation if documented and surfaced in diagnostics.

Evidence/context references: `anchors/meshtemps_project_intent_anchor.md`; `anchors/meshtemps_roadmap_anchor.md`; `anchors/meshtemps_requirements_constraints_anchor.md`; MeshTemps history storage handoff dated 2026-06-07.

Consequences: Runtime and chart work must not depend on multi-week RAM history. FRAM tasks must stay deferred until RAM-first SD archive works.

Rejected alternatives: FRAM-first implementation; multi-week history retained in RAM; writing every mesh packet directly to SD.

Follow-up tasks/validation: 10C-FMT0, 10C-FMT1, 10C-FMTV, 10C-F2, 10D, 10F, 10G.

## Decision 2 — Finalized SD archive moves to sensor-major v2

Date/reference: 2026-06-11 user clarification; anchor commits `eb70f16...`, `d69392d...`, `252f199...`.  
Status: settled.

Decision: Finalized SD records must move away from the current v1 fixed 64-slot minute-frame layout. The intended SD archive is sensor-major: one finalized hour record contains one sensor block per ROM64 sensor seen during that hour.

Rationale: The user clarified that the SD archive should serialize each active sensor for the hour, not write all 64 slot positions for every minute. This makes storage scale with actual sensors and aligns data with chart/query use by sensor.

Evidence/context references: current source still has v1 `HistoryMinuteFrame` with presence/corrected bitmaps and `temp_c_x100[64]`; current `sd_finalized_hour_block.h` computes frame bytes from 64 slots; updated project intent/roadmap/requirements anchors record the v2 direction.

Consequences: Existing v1 writer/scanner/tests are not final. Future repair/quarantine/runtime work must be gated behind v2 format planning and validation.

Rejected alternatives: Treating the current v1 fixed 64-slot SD record as final; optimizing v1 instead of replacing the finalized SD binary ABI.

Follow-up tasks/validation: 10C-FMT0 read-only v2 plan/spec; 10C-FMT1 implementation; 10C-FMTV validation.

## Decision 3 — No durable slot ID in finalized SD records

Date/reference: 2026-06-11 user clarification.  
Status: settled.

Decision: Finalized SD records must not store `slot_id` as a durable field. Slot IDs may exist only as internal current-hour staging handles.

Rationale: Slot IDs are transient implementation details. The finalized SD archive should identify sensors by ROM64, not by a temporary staging index.

Evidence/context references: user rejected slot ID in the SD record; current source still has `HistorySlotDescriptor::slot_id`, which is now staging-only for future design purposes.

Consequences: v2 writer must translate from any internal slot/minute-major staging representation into sensor-major ROM64 blocks.

Rejected alternatives: Durable SD slot catalog; treating slot order as sensor identity; using slot ID in the day-file sensor index.

Follow-up tasks/validation: 10C-FMT0 must define the conversion seam; 10C-FMTV must verify no durable slot ID in finalized SD records.

## Decision 4 — ROM64 is canonical sensor identity

Date/reference: 2026-06-11 identity discussion.  
Status: settled.

Decision: ROM64 is the canonical durable sensor identity in finalized SD history. Node ID, labels, and ordering metadata are context only.

Rationale: DS18B20 ROM64 identifies the physical sensor. A sensor can move between nodes or be renamed without splitting physical history.

Evidence/context references: current stager already finds existing slots by parsed ROM64; user confirmed ROM-only identity.

Consequences: Reader/query/chart continuity must key by ROM64. Node ID and labels may be displayed or used for audit context but must not define identity.

Rejected alternatives: Node ID as identity; label text as identity; durable `addr16` string as primary stored identity.

Follow-up tasks/validation: v2 spec must define ROM64 byte order and schema text; tests must cover duplicate ROM64 entries and moved-node context.

## Decision 5 — Do not store addr16 by default

Date/reference: 2026-06-11 identity discussion.  
Status: settled.

Decision: Finalized SD records should not store `addr16` by default. `addr16` is a printable representation derived from ROM64 for display/debug.

Rationale: Storing both ROM64 and addr16 creates duplicate identity fields and mismatch failure modes. ROM64 is sufficient for machine identity.

Evidence/context references: user asked whether everything can link through ROM64; current source stores both in staging, but v2 intent removes duplicate identity from SD.

Consequences: Parser/export tools must derive display strings from ROM64. Tests must reject accidental storage of addr16 in finalized SD format unless a later approved task changes this.

Rejected alternatives: Belt-and-suspenders storage of ROM64 and addr16; trusting addr16 over ROM64 on mismatch.

Follow-up tasks/validation: 10C-FMT0 must specify ROM64 formatting for human export/debug.

## Decision 6 — Store node ID only as reporting provenance

Date/reference: 2026-06-11 node ID discussion.  
Status: settled.

Decision: Store node ID, if present, only as last-known reporting node provenance/context. It is not a sensor identity.

Rationale: Node ID helps diagnose which mesh node reported a sensor, whether a sensor moved, or whether a node/bus produced suspicious data. It should not split physical history.

Evidence/context references: current `MeshNode` model exposes `node_id()`; current stager stores `last_known_node_id`; user questioned node ID and accepted historical/provenance reasoning when paired with label context.

Consequences: v2 descriptor should include last-known node ID as context unless a later task removes it by explicit decision.

Rejected alternatives: Node ID as primary key; omitting all node provenance from historical archive.

Follow-up tasks/validation: 10C-FMT0 must decide exact field name and behavior if unknown.

## Decision 7 — Store node label and sensor label as historical context

Date/reference: 2026-06-11 label/history discussion.  
Status: settled.

Decision: Finalized sensor blocks should store node label and sensor label snapshots as they existed at logging time, with bounded length rules.

Rationale: ROM64 alone does not help a future human know where a sensor was located or what a node was called years later. Historical archives should remain understandable even if current mappings are lost.

Evidence/context references: current `MeshNode::Sensor` has a human-friendly `label`; current `MeshNode` has node `label()`/`set_label()` and stored `label_`; user explicitly requested historical tie-back to place.

Consequences: v2 descriptor must include bounded label lengths and label bytes. Labels are historical annotations, not identity keys.

Rejected alternatives: Labels as UI-only metadata; external mapping database required to decode historical locations; labels as primary keys.

Follow-up tasks/validation: Decide max byte lengths; test truncation/rejection and schema preamble documentation.

## Decision 8 — Day files include ASCII reverse-engineering preamble

Date/reference: 2026-06-11 archive auditability discussion.  
Status: settled.

Decision: Each new finalized day file shall begin with a bounded human-readable ASCII preamble before binary hour records. The preamble is a compact reverse-engineering guide/schema.

Rationale: A future human should be able to write a parser years later without guessing field order, types, lengths, endian, CRC, string, and sample encodings.

Evidence/context references: user requested a commented header and then clarified it must include field type and field length information; updated intent/requirements anchors require a preamble.

Consequences: Day-file parser must handle binary records starting after a fixed binary-start marker. Tests must guard schema text against drift.

Rejected alternatives: Prose-only comment; no day-file guide; relying only on firmware source to decode old files.

Follow-up tasks/validation: 10C-FMT0 must define exact preamble text/schema ID; 10C-FMT1 must implement and test it.

## Decision 9 — Use explicit lengths, offsets, versions, flags, and CRCs; do not serialize raw structs

Date/reference: 2026-06-11 format discussion.  
Status: settled.

Decision: Binary format must be field-by-field explicit little-endian serialization with lengths/counts/offsets/versions/flags/CRCs. Do not serialize compiler-padded C/C++ structs directly.

Rationale: Explicit serialization avoids compiler padding, ABI drift, alignment issues, and ambiguous future parsing. It also supports bounded scanning and recovery.

Evidence/context references: current finalized writer already uses explicit byte helpers; user questioned generic reserved space as waste; anchors now require byte counts and flags over generic padding.

Consequences: v2 spec must list every field in order with type and byte length. Any reserved fields must have a specific purpose or be omitted.

Rejected alternatives: Raw struct ABI; generic per-sensor reserved arrays; undocumented padding.

Follow-up tasks/validation: v2 tests must decode exact bytes and check preamble/schema agreement.

## Decision 10 — ROM64+offset sensor index table

Date/reference: 2026-06-11 sensor-major layout discussion.  
Status: settled.

Decision: HourRecordV2 should include a sensor index table with entries containing ROM64 and sensor block offset from the start of the hour record.

Rationale: Offsets alone do not identify which sensor block they point to. ROM64+offset enables direct lookup without relying on block ordering.

Evidence/context references: user approved an offset table; assistant clarified that offsets should include ROM64 to make lookup useful.

Consequences: Scanner must validate index bounds, offsets, duplicates, and consistency with sensor blocks.

Rejected alternatives: Offset-only table; deterministic order only with no index; scanning all blocks for every lookup.

Follow-up tasks/validation: 10C-FMT0 must define index entry type/length and duplicate handling.

## Decision 11 — Fixed 60 samples per sensor block with presence/corrected bitmaps

Date/reference: 2026-06-11 serialization discussion.  
Status: settled.

Decision: Each emitted sensor block stores 60 fixed-width minute sample positions, plus 60-minute presence and corrected bitmaps.

Rationale: This keeps decoding simple and deterministic while still scaling by active sensor count. Sparse only-present sample storage is unnecessary at this stage.

Evidence/context references: user proposed one fixed-width packet per minute for each sensor seen at least once that hour.

Consequences: Missing minutes are represented by cleared presence bits, not by omitted sample positions. Sample values for missing minutes must not be treated as valid.

Rejected alternatives: Fully sparse only-present sample lists; global fixed 64-slot minute frames in finalized SD records.

Follow-up tasks/validation: Tests must cover missing minutes, corrected bits, and invalid/missing sample handling.

## Decision 12 — Sensor block magic/sentinel is validation aid only

Date/reference: 2026-06-11 sentinel discussion.  
Status: settled.

Decision: Include a block magic/sentinel at the beginning of each sensor block as an additional safeguard. Do not rely on sentinel scanning for normal parsing.

Rationale: Sentinels help identify corruption but can appear accidentally or be lost in torn writes. Lengths, offsets, and CRCs are safer parser boundaries.

Evidence/context references: user requested a sentinel but agreed not to rely on sentinels for parsing.

Consequences: Parser validates expected block magic at known offsets. It must not search for the next sentinel as its normal recovery strategy.

Rejected alternatives: End-of-stream sentinel as primary boundary; sentinel hunting after corruption.

Follow-up tasks/validation: v2 scanner tests for bad block magic and no sentinel-scanning dependence.

## Decision 13 — CRC first; FEC deferred

Date/reference: 2026-06-11 integrity discussion.  
Status: settled/deferred.

Decision: Use CRC-based validation first. FEC is deferred unless a later task proves a specific error model and need.

Rationale: SD/FAT failure risks are mainly torn writes, stale metadata, missing/corrupt chunks, and power loss, not isolated random bit flips that simple FEC would reliably repair.

Evidence/context references: user proposed FEC as a possibility; current anchors now state CRC before FEC.

Consequences: v2 spec should define header/payload/block CRC coverage and failure behavior. FEC must not be added opportunistically.

Rejected alternatives: Adding FEC immediately; relying on checksums only without recovery policy.

Follow-up tasks/validation: Decide exact CRC32 variant and coverage in 10C-FMT0.

## Decision 14 — Recovery/repair must wait for v2 format gate

Date/reference: Roadmap update commit `d69392d...`; requirements update commit `252f199...`.  
Status: settled.

Decision: Do not continue mutating recovery/quarantine/fault implementation against v1 layout. Insert 10C-FMT0/FMT1/FMTV before 10C-F2-B/C.

Rationale: Repair/quarantine logic depends on record structure. Building repair behavior around a deprecated v1 layout would create rework and possibly codify the wrong ABI.

Evidence/context references: roadmap now makes 10C-FMT0 the current next required action.

Consequences: PR #54 scanner and PR #55 policy remain useful seams but must be updated/revalidated for v2 before runtime append/recovery is enabled.

Rejected alternatives: Continue directly to 10C-F2-B; proceed to Task 10D with runtime SD finalization; repair v1 then replace it later.

Follow-up tasks/validation: 10C-FMT0, 10C-FMT1, 10C-FMTV.

## Decision 15 — Production finalized-hour SD path remains bounded/heap-free

Date/reference: 2026-06-07 handoff and prior 10C-E2-A decisions; reaffirmed 2026-06-11.  
Status: settled.

Decision: Production finalized-hour append/verify paths must use bounded streaming/fixed buffers. Approved buffers remain file-scope static 4096-byte write coalescer and file-scope static 512-byte verification buffer, single-writer/non-reentrant.

Rationale: Avoid heap fragmentation, hidden allocations, and large stack usage on ESP32. User explicitly rejected large stack/task buffers and settled on static buffers.

Evidence/context references: MeshTemps history storage handoff dated 2026-06-07; current requirements anchor.

Consequences: v2 writer/preamble/scanner changes must preserve bounded file-scope buffer policy. Tests should check for regression in production paths.

Rejected alternatives: Full-record heap vector buffers; large local stack buffers; PSRAM by default; Arduino String/printf/libc calendar path construction.

Follow-up tasks/validation: 10C-FMTV and every runtime validation must re-check these constraints.

## Decision 16 — v1 compatibility is not required unless inspection proves otherwise

Date/reference: 2026-06-11 user clarification.  
Status: provisional.

Decision: The working assumption is that v1 finalized-hour file compatibility is not required because v1 implementation is not deployed/finished.

Rationale: User stated there are no finished v1 files to worry about. Supporting v1 would add complexity and preserve the wrong model.

Evidence/context references: user question/answer in current chat; roadmap/requirements mark v1 compatibility as open/provisional.

Consequences: 10C-FMT0 must confirm no real data/migration requirement before deleting/replacing v1 format support.

Rejected alternatives: Multi-format v1+v2 support by default; migration code for nonexistent deployed data.

Follow-up tasks/validation: 10C-FMT0 source/context check.

## Decision 17 — Label length and exact binary constants remain open

Date/reference: 2026-06-11 v2 format discussion.  
Status: provisional.

Decision: Exact field order, magic values, flags, sample encoding enum, CRC variant, and max node/sensor label lengths are not yet settled.

Rationale: These are binary ABI details and should be finalized in the dedicated v2 spec task, not ad hoc in conversation.

Evidence/context references: project intent/roadmap/requirements anchors list these as open decisions.

Consequences: Execution tasks must not invent these values without a read-only plan/spec review.

Rejected alternatives: Codex picking arbitrary field constants during implementation.

Follow-up tasks/validation: 10C-FMT0.

## Deprecated assumptions

- Finalized SD records are a slot catalog plus 60 fixed 64-slot minute frames.
- Slot ID is a durable identity.
- `addr16` must be stored because live code uses address strings.
- Labels are UI-only and not part of the historical archive.
- Node ID can identify a sensor.
- Recovery/quarantine can be implemented safely before the v2 format is settled.
- A sentinel can be used as the normal parser boundary.
- Generic reserved bytes are harmless in repeated sensor descriptors.
- Existing v1 writer/scanner tests can be cited as final storage-format confidence.

## Misleading implementation artifacts discovered

- `HistorySlotDescriptor::slot_id` and `addr16[17]` exist in current staging source, but are not desired finalized SD fields.
- `HistoryMinuteFrame::temp_c_x100[64]` exists in current staging source, but the finalized SD archive should not write 64 slot values per minute as final ABI.
- `SdFinalizedHourBlockHeader` v1 fields such as `active_slot_count`, `descriptor_entry_bytes`, `frame_count`, `frame_bytes`, and `reserved0` describe the current implementation, not the clarified final v2 archive.
- PR #54 scanner and PR #55 policy classify v1 records today; their seams are useful, but their v1 field assumptions are not final authority.
- Some older anchors still contain stale direct-to-10D or v1-slot wording; the updated project intent, roadmap, requirements, and this decision log supersede those stale assumptions.

## Patterns/rules/comments/tests/docs that should not be treated as authority

- Comments claiming the finalized-hour SD format is the current v1 fixed-frame format.
- Tests that only prove v1 `active_slot_count`/descriptor/frame byte behavior.
- Existing raw `assert()` tests if future tasks need confidence under `-DNDEBUG`.
- PR bodies or receipts that predate the v2 format decision.
- Old task sequences that route directly from recovery policy to runtime aggregation without 10C-FMT0/FMT1/FMTV.
- Any local Codex branch named `work` as evidence of the actual product branch.

## Last updated context

Inspected current source/context for this decision log update:

- No current local repository snapshot was available in the active environment. GitHub was used as source of truth.
- GitHub repository: `rasusmilch/MeshTemps`.
- Branch inspected and updated: `feature/ram-backed-sd-hist`.
- Current branch head before this decision-log update: `252f199f9a8d6942f5184ba5e67f6d9fc5707d29`.
- Existing decision-log anchor search found no prior decision log file.
- Source files inspected:
  - `MeshTemps-GUINode/history_hour_stager.h`
  - `MeshTemps-GUINode/sd_finalized_hour_block.h`
  - previous immediate inspection context from `MeshTemps-GUINode/sd_finalized_hour_recovery_policy.h` and `MeshTemps-GUINode/mesh_node.h` during requirements/roadmap updates.
- Anchor files inspected:
  - `anchors/README.md`
  - `anchors/meshtemps_project_intent_anchor.md`
  - previous immediate inspection context from `anchors/meshtemps_roadmap_anchor.md`, `anchors/meshtemps_requirements_constraints_anchor.md`, `anchors/meshtemps_current_next_action_anchor.md`, and `anchors/meshtemps_sd_durability_recovery_anchor.md`.
- Prior uploaded/context references used in summarized form:
  - MeshTemps history storage handoff dated 2026-06-07.
  - PR #54 scanner work and PR #55 recovery-policy work as represented by current GitHub PR metadata and prior receipts.
  - Current user clarification in this chat: no slot ID in finalized SD records; ROM64-only sensor identity; include node/sensor label snapshots; include ROM64+offset sensor index table; include block bytes; include block magic/sentinel only as additional safeguard; include day-file ASCII schema preamble with field names, field types, byte lengths, and parser guidance.
- Expected but missing/unverified:
  - No current local snapshot was inspected.
  - No compile/tests were run for this anchor-only update.
  - No hardware validation was performed.
  - Exact v2 binary constants/field order remain unapproved pending 10C-FMT0.
