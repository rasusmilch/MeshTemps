# MeshTemps Testing Hardening Anchor

Project: MeshTemps  
Workstream: GUI-node history storage and chart hardening  
Anchor purpose: Durable testing policy for future and existing MeshTemps tests.  
Status: Committed repository anchor for storage/history workstream.  
Last updated: 2026-06-08

## 1. Why this anchor exists

MeshTemps is moving storage, history, SD recovery, and chart behavior into code paths where false confidence is dangerous. Tests must not merely compile or exercise happy paths; they must make incorrect behavior hard to miss.

This anchor records the user decision that MeshTemps testing must be hardened for both:

- future tests added by new tasks, and
- past/existing tests that future work relies on, touches, or cites as validation evidence.

Do not treat existing test style as automatically acceptable just because it is already present in the repo.

## 2. Authority

Future ChatGPT/Codex planning, execution, validation, and review tasks must treat this file as a testing policy anchor.

If a task touches a module, depends on a module's tests, or cites an existing test as proof of behavior, Codex must inspect those tests for quality. Weak existing tests become visible technical debt and must be either:

1. hardened in the same focused task when the fix is small and local, or
2. captured as an explicit follow-up before those tests are used as final confidence for risky behavior.

Do not silently rely on weak tests.

## 3. Future test requirements

New tests must be behavior tests, not just compile checks.

For new host tests, prefer explicit checks that remain active regardless of `NDEBUG`, such as a small local `CHECK` / `CHECK_EQ` harness or an approved test framework. Raw `assert()` may remain acceptable only for internal fixture sanity checks where failure means the test setup itself is invalid. Behavior expectations should not depend on raw `assert()`.

New tests must provide useful failure diagnostics. A failing test should identify which behavior failed, not only abort on the first opaque expression.

Where a module exposes public status enums, error enums, failure reasons, result fields, state-machine states, or policy branches, tests must directly cover those branches unless a validation receipt explicitly justifies why a branch is unreachable or covered elsewhere.

Where a module claims a corruption matrix, state matrix, or workflow matrix is covered, the receipt must include a matrix mapping each required case to a test. Do not claim exhaustive coverage without a direct mapping.

For storage/history code, tests must include negative and failure-path cases, not only valid-path encoding/decoding.

## 4. Existing/past test hardening requirement

Existing tests are not grandfathered as sufficient.

When future work touches or relies on existing tests, Codex must audit them for:

- raw `assert()` used for behavior expectations,
- checks disabled by `NDEBUG`,
- missing failure-path coverage,
- missing enum/status/result-field coverage,
- tests that only prove compile success,
- tests that use synthetic data when real writer/reader paths are available,
- tests that miss offset, boundary, overflow, partial-read, or corrupt-data behavior,
- tests that cannot fail for the intended bug,
- tests whose names or receipts overstate actual coverage.

The audit must be proportional. Do not convert the whole repository in one unrelated task. Harden the tests in the area being changed, especially when the code is storage, recovery, allocation-sensitive, or UI-freeze related.

## 5. Raw assert policy

Raw `assert()` is a weak behavior-test mechanism because it can be compiled out with `NDEBUG`, aborts on first failure, and provides poor test summaries.

Policy:

- Do not add new raw-`assert()` behavior tests unless explicitly approved.
- Prefer `CHECK`, `CHECK_EQ`, or equivalent explicit test assertions that cannot be disabled by `NDEBUG`.
- Existing raw-`assert()` behavior tests must be hardened when touched or used as important validation evidence.
- Raw `assert()` may remain for local fixture invariants, bounds checks in test-only byte patch helpers, or sanity checks that indicate the test itself was constructed incorrectly.

## 6. Storage/recovery test requirements

Storage and recovery code has higher test requirements because a small mistake can cause data loss, corrupted history, or false recovery confidence.

For finalized-hour SD archive, scanner, repair, reader/query, and chart migration work, tests must cover applicable cases such as:

- empty input,
- one valid record,
- multiple valid records,
- clean EOF,
- partial header,
- partial payload,
- bad magic/version/format,
- bad header CRC,
- bad payload CRC,
- dangerous sizes rejected before read/seek/allocation,
- descriptor/frame size mismatch,
- record-size mismatch,
- corrupt tail after a valid prefix,
- invalid data at offset zero,
- read errors,
- offset accounting,
- tiny caller-owned buffers,
- null/zero buffer rejection where applicable,
- every public status/failure enum path.

Tests may use host-side vectors or helper buffers as fixtures, but production firmware paths must remain bounded and must not use full-file or full-record heap buffering unless explicitly approved by a future task.

## 7. Validation receipt requirements

Validation receipts must distinguish:

- code inspected,
- tests run with exact commands and output,
- tests reviewed but not run,
- behavior covered by direct tests,
- behavior covered indirectly,
- behavior not covered,
- hardware-unverified items,
- environment-limited items,
- claims from PR bodies or receipts that are overstated.

Any phrase like `comprehensive`, `all cases`, `all enums`, `fully covered`, or `complete validation` must be backed by a concrete coverage matrix.

## 8. Immediate application to Task 10C-F1 / PR #54

Task 10C-F1 introduced the finalized-hour append-file scanner. Its tests are meaningful because they build real finalized-hour records through the current writer path and mutate bytes to force scanner failures.

However, the scanner test file originally used raw `assert()` for behavior expectations. That is now considered test-hardening debt before PR #54 should be marked ready.

For PR #54 readiness, harden `tests/sd_finalized_hour_recovery_test.cpp` so behavior expectations do not depend on raw `assert()`. Use a small local explicit-check harness or equivalent. Keep raw `assert()` only for test fixture sanity if needed.

## 9. Roadmap consequence

Before moving from scanner work into repair/quarantine or runtime SD finalization, the tests for the scanner and the tests being relied on for storage behavior must be strong enough to trust.

Do not proceed to Task 10C-F2 or Task 10D on the basis of weak or overstated tests.
