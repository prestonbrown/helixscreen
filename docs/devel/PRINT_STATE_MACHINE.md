# PrintLifecycleState -- Print State Machine

## Overview

`PrintLifecycleState` is a pure-logic state machine (no LVGL dependencies) that maps
Moonraker's raw `PrintJobState` + `PrintOutcome` into UI-level `PrintState` values.

**It is not the app-wide authority, and it is not the only consumer of this
question.** The instance lives as a private member of `PrintStatusPanel`
(`include/ui_panel_print_status.h`), so components that are not that panel cannot
reach it and historically each kept its own previous-state variable with its own
rules. The authoritative value is now the `print_lifecycle` subject on
`PrinterPrintState`, derived once by `derive_print_state()`; observe that rather
than re-deriving from the raw job-state enum.

The mapping itself lives in exactly one function:

```cpp
PrintState derive_print_state(PrintJobState job_state, int start_phase);
```

A live pre-print phase outranks the job state, **except** for `PAUSED` - a pause is
user-visible and must not be masked by a phase left set when the printer stopped
mid-`PRINT_START` (a runout during the purge line, an `M600`).
It guards against Moonraker race conditions where zeroed progress/layer/duration
values arrive after a print has already reached a terminal state (Complete, Cancelled,
Error). The UI layer consumes `StateChangeResult` structs to react to transitions
without embedding widget logic in the state machine.

**Files:**
- `include/print_lifecycle_state.h` -- enum, result struct, class declaration
- `src/printer/print_lifecycle_state.cpp` -- transition logic and guards

---

## State Transition Diagram

```
                          start_phase != 0
                   +---------------------------+
                   |                           |
                   v                           |
               Preparing --+                   |
               (UI-only)   |                   |
                   |       | start_phase == 0  |
                   |       | (restore from     |
                   |       |  job_state)        |
                   |       v                   |
     STANDBY   PRINTING   PAUSED              |
    +-------> Idle    Printing <---> Paused    |
    |           ^      |    ^         |        |
    |           |      |    +---------+        |
    |           |      |   PAUSED/PRINTING     |
    |           |      |                       |
    |  STANDBY  |      +-------+-------+-------+
    |  (from    |      |       |       |
    |  any)     |  COMPLETE CANCELLED ERROR
    |           |      |       |       |
    |           |      v       v       v
    |           |   Complete Cancelled Error
    |           |      |       |       |
    |           +------+-------+-------+
    |              STANDBY (print_ended)
    +------------------------------------------+
```

Key points:
- **Preparing** is a UI-only state driven by `on_start_phase_changed()`, not by
  Moonraker's `job_state`. It represents pre-print operations (homing, bed leveling,
  heating). When `start_phase` returns to 0, state restores from the current
  `job_state` (typically Printing).
- Terminal states (Complete, Cancelled, Error) persist until Moonraker sends STANDBY,
  which transitions to Idle and fires `print_ended`.

---

## State Transition Table

### Transitions from `on_job_state_changed()`

| From | To | Moonraker Trigger | Side-Effects |
|------|----|-------------------|--------------|
| Any | Idle | STANDBY | `print_ended=true`, `should_show_viewer=false` (gcode_loaded is cleared internally) |
| Idle / Preparing | Printing | PRINTING | `should_reset_progress_bar=true`, `should_clear_excluded_objects=true`, `should_show_viewer` (if gcode loaded) |
| Paused | Printing | PRINTING | (resume -- no reset, no clear) |
| Printing | Paused | PAUSED | `should_show_viewer` preserved |
| Any active | Complete | COMPLETE | `should_freeze_complete=true`: progress forced to 100, layer forced to total, remaining forced to 0, elapsed frozen. `should_show_viewer` preserved |
| Any active | Cancelled | CANCELLED | `should_animate_cancelled=true`, `should_show_viewer` preserved |
| Any active | Error | ERROR | `should_animate_error=true`, `should_show_viewer` preserved |

### Transitions from `on_start_phase_changed()`

| From | To | Trigger | Notes |
|------|----|---------|-------|
| Any | Preparing | `phase != 0` | Resets `preprint_elapsed` and `preprint_remaining` to 0 |
| Preparing | Printing | `phase == 0`, `job_state == PRINTING` | Restores state from current Moonraker job_state |
| Preparing | Paused | `phase == 0`, `job_state == PAUSED` | (unlikely but handled) |
| Preparing | Idle | `phase == 0`, `job_state == other` | Fallback |

---

## The preparing job

`Preparing` is reachable *before* the printer reports a print. The window between
the user committing to a job and the printer confirming it belongs to
`PrinterPrintState`:

```cpp
void begin_preparing(const PrintJobRef& job);   // commit
void retire_preparing(PreparingExit reason);    // settle
bool has_preparing_job() const;
const PrintJobRef& preparing_job() const;
```

`begin_preparing()` is **synchronous**. `set_print_start_state()` defers because it
is called from WebSocket callbacks; a button press is already on the main thread, so
the previous job's outcome and progress are cleared before anything can render a
`Preparing` state beside the finished job's numbers.

Two things arm this window, and both are required:

| Arming path | Trigger | Ambiguous? |
|---|---|---|
| Commit | `PrintStartController` - user pressed Print | No |
| Printer edge | `standby -> printing` with no live preparing job | Yes; keeps `should_start_print_collector()`'s guards (#1042) |

Externally started prints (Mainsail, Fluidd, Orca) have no commit, so removing the
printer-edge path would stop them entering `Preparing` at all.

### Why a live preparing job relaxes two guards

`set_print_start_state()` used to drop every phase update after the first while
`print_active == 0`, and a safety reset forced the phase back to IDLE on the
`print_active -> 0` edge. Both assumed preparation only happens *inside* Moonraker's
PRINTING window. That holds when `PRINT_START` owns the work; it does not when a
host-side pre-start block runs before the printer is handed the job, because
`print_stats` keeps the previous job's terminal state for the whole duration
(measured at 455s on a K2 Plus running `BED_MESH_CALIBRATE_START_PRINT`). Both
guards now yield to a live preparing job.

### Reconciliation

Only a **PRINTING** report settles a preparing job; a terminal report is the very
scenario this exists for and leaves the claim intact. Matching is on the bare name
after `resolve_gcode_filename()`, since the report may be path-qualified and a
modification rewrite substitutes a temp file.

| Exit | Meaning |
|---|---|
| `Confirmed` | The printer took our job. **Does not end preparation** - `PRINT_START` runs inside the job, so the phase and overlay legitimately outlive the handoff |
| `Superseded` | The printer named a different job; something else started a print while ours prepared, so the claim is dropped rather than silently adopted |
| `Failed` | The start could not complete |
| `Cancelled` | The user abandoned it |
| `TimedOut` | No confirmation arrived. Ungated - a commit-armed job can be raised on a printer that never reaches `state=printing` |

`preparing_epoch` is bumped per preparing job and reads 0 when none is live.
Consumers that must adopt the job's identity - `ActivePrintMediaManager` above all -
observe it rather than relying on each start path remembering to tell them.

## Asking whether a job owns the machine

`job_holds_machine(PrintState)` (`include/print_lifecycle_state.h`) is the answer
to *"would acting now fight the printer for the toolhead?"*. True for
`Preparing`, `Printing` and `Paused`.

Ask it before anything that emits G-code of its own -- motion, calibration,
filament operations, tool changes, cooldowns, macro buttons. The subject-shaped
form for XML bindings is `job_holds_machine`, published from
`PrinterPrintState::publish_lifecycle_state()` alongside `print_lifecycle` so the
two can never be seen disagreeing.

**Do not use `print_active` for this.** `print_active` is `PRINTING || PAUSED`
read off `print_stats.state`, so it is 0 for the entire duration of a *host-side*
pre-print block -- the K2's forced bed mesh being the motivating case -- while the
toolhead is homing and probing. It remains correct for its own question ("is
Moonraker running a job right now"), and card visibility and the discovery-time
idle gate still want exactly that.

**It is a convenience over the lifecycle, not a replacement for it.** A caller
that must distinguish `Paused` still switches on `PrintState`.
`ams_subscription_backend.cpp` is the standing example: it deliberately *allows*
a filament operation on a paused print when the backend does **not** self-home,
because then no firmware macro can hide a `G28`.

## What just happened: `print_lifecycle_prev` and `should_notify_print_ended()`

Consumers that need the *transition* - "what did we just leave?" - no longer keep
a private previous-state variable. `PrinterPrintState` publishes
`print_lifecycle_prev` (`include/printer_print_state.h:321`-333) from the same
place that computes the transition, with three deliberate properties:

- **Written only when the state actually changes** (`src/printer/printer_print_state.cpp:1432`-1437). Rewriting it unconditionally would collapse it onto the current state and every consumer would see a self-transition.
- **Written *before* `print_lifecycle`**, so an observer firing on the new value already sees a consistent pair.
- **Initialized to `Idle`**, so booting straight into a terminal state reads as `Idle -> Complete` and correctly does not notify.

Eight private previous-state variables existed before this subject, and they
disagreed at the edges.

The edge consumer is `should_notify_print_ended(prev, current, outcome)`
(`include/print_completion.h:61`, `src/print/print_completion.cpp:141`), which
decides whether a lifecycle transition means *a print the user was watching
ended*. `Printing`/`Paused` -> terminal notifies. `Preparing` -> terminal is the
interesting arm, and it gates on `outcome` in both directions:

- **A print that dies inside `PRINT_START` never passes through `Printing`.** A
  live phase outranks the job state, so the lifecycle holds at `Preparing` while
  `print_stats` already reads printing, then jumps straight to the terminal value
  when the phase clears. The outcome was recorded for THIS attempt, so it is not
  NONE and the death is reported - without this arm, a Klipper fault inside
  `PRINT_START` or a cancel during a long bed mesh was reported by nothing (the
  preparing-exit observer cannot cover it either: the job was retired as
  `Confirmed` the moment the printer took it).
- **An abandoned host-side pre-start block derives the same `Preparing` ->
  terminal shape**, but from the PREVIOUS job's stale `print_stats`.
  `begin_preparing()` clears `print_outcome`, so that case reads NONE and stays
  silent - otherwise abandoning a start would announce the last print's
  completion.

The completion observer (`print_completion.cpp:312`) reads both halves of the
transition from `PrinterPrintState`; it no longer owns a latch of its own.

---

## Guards

The state machine rejects stale updates that Moonraker sends after a print ends.

### Terminal state guards (Complete, Cancelled, Error)

These methods return `false` and discard the update:
- `on_progress_changed()` -- prevents progress resetting to 0
- `on_layer_changed()` -- prevents layer count resetting to 0
- `on_duration_changed()` -- prevents elapsed time resetting
- `on_time_left_changed()` -- prevents remaining time resetting

### Outcome guards

`on_duration_changed()` and `on_time_left_changed()` also reject updates when
`outcome != PrintOutcome::NONE`. This catches the case where Moonraker reports
a completed/cancelled outcome before the state machine has transitioned.

### Preparing state guards

During Preparing, `on_duration_changed()` and `on_time_left_changed()` store the
value internally but return `false`. This signals the UI that the preprint observer
owns the time display, not the normal print timer. Preprint time is updated
separately via `on_preprint_elapsed_changed()` and `on_preprint_remaining_changed()`.

### Always accepted

These are never guarded and update in all states:
- `on_temperature_changed()` -- nozzle/bed current and target
- `on_speed_changed()` -- speed override percentage
- `on_flow_changed()` -- flow override percentage

---

## Resource Lifecycle

### print_ended

`print_ended` fires **only** on transition to Idle (when Moonraker sends STANDBY
after a terminal state). It does NOT fire on transition to Complete, Cancelled, or
Error. This allows the UI to keep resources (thumbnail, stats overlay, viewer) visible
while the user reviews the final print state. The UI tears down print resources only
when `print_ended` is true.

### gcode_loaded

`gcode_loaded` is preserved through terminal states so the 3D viewer stays visible
showing where the print stopped. It is cleared only on transition to Idle: the
transition computes a local `clear_gcode_loaded` from `print_ended` and applies it
in the same pass (`src/printer/print_lifecycle_state.cpp:96`) - it is not a
`StateChangeResult` field. The flag can be set externally via `set_gcode_loaded()`.

### 3D Viewer visibility

`should_show_viewer` in `StateChangeResult` is computed as `want_viewer && gcode_loaded`.
`want_viewer()` returns true for all states except Idle -- this means the viewer
remains visible through Preparing, Printing, Paused, Complete, Cancelled, and Error.
The viewer disappears only when transitioning to Idle.

### Complete state freeze

On transition to Complete, the state machine freezes display values:
- `progress` = 100
- `current_layer` = `total_layers` (if total > 0)
- `remaining_seconds` = 0
- `elapsed_seconds` = frozen at last known value

This prevents Moonraker's post-completion zeroed values from corrupting the display.
