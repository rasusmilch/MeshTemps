# MeshTemps SD Durability and Recovery Anchor

Project: MeshTemps
Workstream: GUI-node history storage and chart hardening
Anchor purpose: Durable SD finalized-hour recovery guidance for future ChatGPT/Codex planning, execution, validation, and review tasks.
Status: Committed repository anchor after PR #54 scanner merge; current recovery work is split across 10C-F2 subtasks.
Last updated: 2026-06-09

## v2 format caveat

This recovery anchor was originally written around the current v1 finalized-hour header/descriptor/frame field names. Those v1 field-level examples are stale for the final v2 sensor-major day-file format. PR #59 / Task 10C-FMT1-C has landed the local read-only v2 scanner-stack validation basis: preamble and binary-start marker parsing, HourRecordV2 header checks, ROM64+offset index checks, sensor block checks, block/payload/header CRC checks, valid-prefix accounting, and a read-only `SdHistoryStore::ScanFinalizedHourFile` seam. The recovery principle remains current: valid history is the longest structurally and CRC-valid prefix, and append must stay blocked after a corrupt tail until approved recovery/fault policy makes the file safe. 10C-F2-B/C must still not proceed until PR #59 cleanup/merge and broader v2 integrated validation authorize recovery/append-guard work.

## 1. Why this anchor exists

The current finalized-hour SD path is bounded and improved, but capacity is not the limiting risk. A 4 GB card can hold decades of finalized-hour data at the planned data rate. The largest durability risk is power loss or reset during FAT32 append/write/flush/close.

Power loss will happen in this project. The design must assume the GUI node can lose power during:

- finalized-hour record append,
- SD controller internal programming,
- FAT or directory metadata update,
- file-size update,
- record read-back verification,
- boot recovery or repair itself.

`file.flush()` and `file.close()` are required, but they are not a complete durability strategy. They do not prove that the SD card and FAT metadata are power-fail atomic. The system needs an explicit recovery contract.

## 2. Current durable-storage model

Current approved storage direction:

```text
live mesh sensor state
  -> bounded current-hour stager
  -> finalized-hour SD archive
  -> later SD reader/query path
  -> later chart migration
```

The finalized-hour record is the authoritative long-term history unit. The record is self-describing and includes enough information to detect many forms of corruption:

- magic,
- version,
- header size,
- total record bytes,
- hour start epoch minute,
- active slot count,
- descriptor bytes,
- payload bytes,
- payload CRC,
- header CRC,
- descriptor/catalog snapshot,
- 60 raw minute frames.

Current finalized-hour production write behavior must remain:

```text
WriteSdFinalizedHourBlock()
  -> coalescer.Flush()
  -> file.flush()
  -> file.close()
  -> reopen/read-back verification
```

This proves the just-written record is readable after a clean write/flush/close cycle. It does not by itself define what happens after a later reboot if the file has a torn tail or FAT metadata damage.

## 3. Highest-priority durability gap

Highest-priority gap:

```text
No explicit boot/open recovery contract for finalized-hour append files after power loss.
```

The current format can detect bad records, but future code must define how to scan, ignore, truncate, repair, or quarantine a corrupt tail before appending more records or serving history queries.

This must be solved before runtime SD finalization is enabled as a normal operating path. Task 10D must not quietly depend on an unreviewed recovery behavior.

## 4. Recovery contract to implement

A finalized-hour append file must be treated as an append-only sequence of records. Valid history is the longest prefix of records that passes structural and CRC validation.

A recovery scanner must scan from offset zero:

1. If exactly at EOF, the file is clean.
2. Read `kSdFinalizedHourHeaderBytes` bytes.
3. If no bytes are available at offset zero, the file is empty and clean.
4. If a partial header is found, the file has a corrupt/torn tail.
5. Decode the header.
6. Validate magic, version, header size, header CRC, record size, descriptor size, frame count, frame size, active slot count, and payload size.
7. Reject impossible or dangerous sizes before seeking or reading, especially values larger than `kSdFinalizedHourMaxRecordBytes` or inconsistent descriptor/frame/payload lengths.
8. Read payload sequentially with a fixed-size buffer.
9. Verify payload CRC.
10. If the record is valid, advance by `record_bytes` and continue.
11. If any check fails after at least one valid record, stop at the last known-good offset and classify the rest as corrupt tail.

Reader/query code must only expose validated records from the good prefix.

Appender code must not append after a corrupt tail unless recovery has made the file safe.

## 5. Repair policy

Preferred repair policy:

```text
truncate append file to last known-good offset
```

However, Codex must not assume the Arduino/ESP32 FS/File API supports safe truncate without direct source/toolchain verification.

Task planning must verify whether the exact target filesystem layer supports:

- truncate to a specific file size,
- remove,
- rename,
- atomic-enough rename behavior on FAT32,
- opening temporary repair files,
- syncing/flushing replacement files.

If truncate is available and validated:

1. Scan file and find last known-good offset.
2. If corrupt tail exists, truncate to last known-good offset.
3. Flush/close.
4. Reopen and rescan to confirm the file is clean.
5. Record a recovery diagnostic counter/status.

If truncate is unavailable or untrusted, use a conservative copy/replace/quarantine plan:

1. Scan original file and find last known-good offset.
2. Stream-copy only the validated prefix to a temporary repair file in the same directory.
3. Flush/close the repair file.
4. Reopen and rescan the repair file.
5. Only after the repair file validates, quarantine the original file by renaming it with a corrupt/recovered suffix if supported.
6. Rename the repaired file to the canonical path if supported.
7. If safe rename/quarantine is not supported, do not append to the damaged canonical file. Report an explicit storage fault and require a future reviewed policy.

Repair must be idempotent. If power fails during repair, the next boot must be able to classify and safely resume from leftover temporary/quarantine files without deleting the only copy of valid data.

## 6. Temporary and quarantine file rules

Any repair task that creates temporary files must define deterministic names before implementation. Suggested names, subject to code review:

```text
d<epoch_day>.bin.repair.tmp
d<epoch_day>.bin.corrupt
```

Rules:

- Never delete the original file until the repaired replacement has been written, flushed, closed, reopened, and validated.
- Never append to a file with a known corrupt tail.
- Do not leave multiple ambiguous candidate files without deterministic cleanup rules.
- On boot, temporary repair files must be handled before normal appending begins.
- Quarantined corrupt files must not be served to chart/query code.

## 7. Diagnostics required

Storage diagnostics should eventually report:

- SD mount status,
- finalized archive scan status,
- number of files scanned,
- number of records accepted,
- number of corrupt tails detected,
- number of repairs attempted,
- number of repairs completed,
- number of repairs failed,
- last recovery error,
- append blocked due to unrepaired corruption,
- current-hour RAM loss after reset,
- whether runtime SD finalization is enabled.

Diagnostics may be serial-only at first. GUI diagnostics can be a later task.

## 8. Testing requirements

Host tests are required for recovery logic before target hardware validation.

Required pure/host recovery test scenarios:

- empty file,
- exactly one valid record,
- multiple valid records,
- valid records followed by clean EOF,
- partial header at offset zero,
- partial header after one valid record,
- bad magic,
- unsupported version,
- bad header size,
- bad header CRC,
- bad payload CRC,
- partial payload,
- `record_bytes` too small,
- `record_bytes` larger than maximum,
- descriptor bytes inconsistent with active slot count,
- frame count or frame bytes inconsistent with current format,
- active slot count greater than 64,
- garbage bytes after a valid record,
- repair/truncate to last known-good offset,
- idempotent second scan after repair,
- power-loss simulation during repair if copy/replace/quarantine is used.

Target hardware validation is separate and must not be claimed unless actually performed.

Required hardware validation before declaring durability complete:

- SD absent at boot,
- SD absent at hour rollover,
- reset during current-hour RAM staging,
- reset during finalized-hour write,
- reset after coalescer flush but before file close if testable,
- reset after file close but before read-back verification if testable,
- repeated power-pull tests during finalized-hour append,
- boot recovery after intentionally injected corrupt tail,
- append resumes only after file is clean or explicitly faults.

## 9. Recommended task plan

Insert a recovery-focused gate before normal runtime SD finalization is enabled.

Recommended sequence after Task 10C-E3 validation:

```text
10C-E3V legacy SdHistoryStore debt-removal validation
  -> 10C-F0 read-only SD recovery plan
  -> 10C-F1 finalized-hour append-file scanner / validation service
  -> 10C-F1 validation
  -> 10C-F2 safe tail repair or quarantine policy
  -> 10C-F2 validation
  -> 10D runtime HistoryAggregator snapshot path
```

If the user chooses to start Task 10D before recovery implementation, Task 10D must keep runtime SD finalization disabled or behind an explicit compile/runtime guard until 10C-F1/10C-F2 pass. Do not silently enable normal hourly SD appends without recovery behavior.

## 10. Task 10C-F0 — read-only SD recovery plan

Type: read-only planning task.
Risk: high.
Dependency: Task 10C-E3V complete.

Scope:

- Inspect current finalized-hour writer, block format, path builder, and SD wrapper behavior.
- Inspect Arduino/ESP32 FS/File capabilities available in the project environment.
- Determine whether truncate is available and safe enough to use.
- Define scanner API and result types.
- Define repair/quarantine policy.
- Define diagnostics fields/counters.
- Define host tests and hardware validation plan.

Explicit exclusions:

- No implementation.
- No Task 10D runtime aggregator.
- No chart/query migration.
- No FRAM.
- No retention/pruning.

Acceptance:

- Plan defines exact recovery states and transitions.
- Plan identifies whether truncate is available or requires copy/replace/quarantine fallback.
- Plan includes idempotent boot cleanup rules for temp/quarantine files.
- Plan lists behavior when repair fails.
- Plan keeps reader/query exposure limited to validated records only.

## 11. Task 10C-F1 — finalized-hour append-file scanner

Type: focused execute after 10C-F0 approval.
Risk: high; checkpoint validation required.

Scope:

- Add a pure scanner that validates finalized-hour append files record-by-record.
- Return last known-good offset, valid record count, and first failure reason.
- Use fixed-size buffers; no full-file or full-record heap loading.
- Reuse finalized-hour header decode and CRC helpers.
- Add host tests for clean and corrupt files using byte buffers or a test file abstraction.

Explicit exclusions:

- No repair/truncate yet unless 10C-F0 explicitly approves combining scanner and repair.
- No chart/query migration.
- No runtime aggregator.
- No FRAM.

Acceptance:

- Scanner accepts valid record sequences.
- Scanner rejects bad/torn records.
- Scanner stops at first corrupt tail.
- Scanner never reads or seeks based on unbounded unvalidated sizes.
- Tests cover all required corruption scenarios from this anchor that are feasible in host tests.

## 12. Task 10C-F2 — safe tail repair or quarantine

Type: split execute sequence after PR #54 / 10C-F1 scanner merge.
Risk: high; checkpoint validation and hardware validation required before declaring durability complete.

Current split sequence:

```text
10C-F2-A non-destructive recovery policy seam and append-safety classifier
  -> 10C-F2-A validation
  -> 10C-F2-B approved repair/quarantine/fault implementation
  -> 10C-F2-C runtime integration / append guard
  -> 10C-F2V recovery validation
  -> 10D runtime HistoryAggregator snapshot path
```

10C-F2-A scope:

- Add a pure, non-destructive policy seam that consumes scanner results.
- Classify append as allowed only for clean/empty scan results.
- Block append for corrupt tail, invalid-at-zero, read error, unsupported format, dangerous header, and unknown/default statuses.
- Add plain diagnostics/status fields for scan decisions.
- Add host tests and a fake byte/file store that proves no mutation was attempted.
- Do not integrate runtime append/boot behavior yet.

10C-F2-A explicit exclusions:

- No automatic repair.
- No truncate.
- No remove/delete of canonical finalized-hour files.
- No rename/quarantine/promotion.
- No copy/replace repair.
- No `SdHistoryStore` runtime behavior change.
- No chart/query migration.
- No runtime aggregator.
- No FRAM.
- No retention/pruning.
- No broad GUI diagnostics unless separately scoped.
- No hardware durability claim.

Later 10C-F2-B/C scope after approval:

- Implement the approved recovery policy only after product decision.
- Prefer truncate only if verified available and safe enough; in-place truncate is not assumed safe or available.
- Otherwise implement copy/replace/quarantine fallback or explicit fault behavior.
- Ensure repair/fault handling is idempotent across reset/power loss.
- Do not append to a known-corrupt canonical file.
- Add diagnostics counters/status.

Overall 10C-F2 acceptance:

- Corrupt tail is handled deterministically.
- Valid prefix is preserved.
- Repair failure or fault-only policy blocks append and reports a clear fault.
- Reboot after interrupted repair has deterministic behavior if mutating repair is implemented.
- Host tests pass.
- Hardware power-loss validation remains clearly marked until actually run.

## 13. Current open decisions

Open decisions for 10C-F2 recovery policy:

- Whether to repair by truncate, copy/replace/quarantine, or explicit fault-only behavior. Automatic destructive repair is not approved in 10C-F2-A.
- Whether recovery runs at `SdHistoryStore::Begin()` or lazily before appending each epoch-day file. 10C-F2-A does not change runtime `SdHistoryStore` behavior.
- How many finalized files to scan at boot before UI becomes available.
- Whether scanning all historical files is required or only current/recent append targets.
- How recovery status is exposed initially: serial only, GUI later, or both.
- Whether unrepaired corruption blocks all history appends or only the affected epoch-day file.
- Whether repair/quarantine filenames should be hidden from chart/query scans by suffix filtering.

## 14. Non-goals

This anchor does not require:

- storing every mesh packet directly to SD,
- changing finalized-hour binary layout,
- adding rollups/indexes,
- changing the 64-slot current-hour model,
- changing chart rendering,
- implementing FRAM,
- guaranteeing that FAT32 itself is power-fail atomic.

The goal is to make append-file recovery deterministic and to preserve the longest validated prefix after power loss.
