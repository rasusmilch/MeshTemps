# MeshTemps GUI history console commands

This page summarizes the GUI-node serial console commands for normal sensor
history and diagnostic fake-history chart testing.

## Quick reference

| Command | Purpose |
| --- | --- |
| `hist show` | Show the current runtime history logging configuration. |
| `hist get` | Alias for `hist show`. |
| `hist set <interval_s> <retention_days>` | Set the runtime history interval in seconds and retention in days. |
| `hist clear` | Clear all in-memory sensor histories and diagnostic history markers. |
| `hist clear <addr16>` | Clear one sensor history and its diagnostic marker. |
| `hist view <addr16> [max_samples]` | Print one sensor's history samples, oldest to newest. |
| `hist fake <hours> <min_c> <max_c>` | Replace live GUI-observed sensor histories with synthetic diagnostic data. |
| `hist fake targets` | Preview the live nodes/sensors that `hist fake` would target. |
| `hist fake status` | Show active/idle fake-history job status and diagnostic history count. |
| `hist fake cancel` | Request cancellation of an active fake-history background job. |

## Normal history configuration

`hist set` takes the interval in **seconds**, not milliseconds:

```text
hist set <interval_s> <retention_days>
```

Safe examples:

```text
hist set 60 7
hist set 60 30
```

These examples configure a 60-second / 1-minute history interval with either
7 or 30 days of retention.

Do **not** enter milliseconds for `interval_s`. For example:

```text
hist set 60000 7
```

means 60,000 seconds, not 60,000 ms. That interval would produce only 10
samples over 7 days and is almost certainly a unit mistake. The updated console
command rejects this suspicious value and suggests the intended seconds form:

```text
hist set 60 7
```

`hist set` is a runtime console setting in the GUI-node process. Do not assume a
new value survived reboot unless you verify it after boot with:

```text
hist show
```

## Capacity math

The per-sensor history capacity is calculated from the interval and retention:

```text
capacity = floor(retention_days * 24 * 60 * 60 / interval_s)
```

Examples:

| Interval | Retention | Capacity |
| --- | ---: | ---: |
| 60 s | 7 days | 10,080 samples |
| 60 s | 30 days | 43,200 samples |
| 300 s | 30 days | 8,640 samples |
| 60,000 s | 7 days | 10 samples |

The 60,000-second example is included to show why accidentally entering
milliseconds makes history look nearly empty.

## Viewing and clearing history

Use `hist view` to inspect samples for one sensor address:

```text
hist view <addr16> [max_samples]
```

Use `hist clear` to clear all in-memory sensor histories:

```text
hist clear
```

Use `hist clear <addr16>` to clear only one sensor history:

```text
hist clear 28737A5700000051
```

Clearing a history also clears any diagnostic fake-history marker for the
cleared sensor history.

## Diagnostic fake history

The `hist fake` commands are for chart and history diagnostics. They generate
in-memory synthetic samples so long-range chart behavior can be tested without
waiting for real time to pass.

Diagnostic fake history:

- requires valid epoch time before generation starts;
- targets live GUI-observed nodes and sensors shown by the GUI model;
- replaces the selected sensor histories instead of appending;
- uses fixed 5-minute synthetic sample spacing;
- accepts minimum and maximum temperatures in Celsius;
- uses deterministic daily waveform data for chart testing;
- is protected from normal live logging so it does not collapse immediately;
- is cleared by `hist clear` or `hist clear <addr16>`;
- is in-memory diagnostic data and should not be used to test persistence.

Typical workflow:

```text
hist fake targets
hist fake 720 15 28
hist fake status
```

After the job completes, open a history chart and switch between the 1d, 2d,
5d, 7d, and 30d ranges to validate long-history chart behavior.

When finished, clear diagnostic history before returning to normal history
collection:

```text
hist clear
```
