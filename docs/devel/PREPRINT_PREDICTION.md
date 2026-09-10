# Preprint ETA Prediction

How HelixScreen predicts the duration of PRINT_START macro phases (heating, homing, leveling, etc.) and displays real-time remaining time estimates during print preparation.

---

## Overview

Every 3D print begins with a preparation sequence: heating the bed and nozzle, homing axes, probing the bed mesh, purging filament. On many printers this takes 2-10 minutes, but the user has no visibility into how long it will take.

The preprint prediction system solves this by:

1. **Recording** per-phase durations each time a print starts (homing took 25s, bed mesh took 90s, etc.)
2. **Persisting** those entries to disk (the predictor holds up to `MAX_ENTRIES` = 10 for the bucket it loaded; the file keeps at most 15 across all buckets)
3. **Predicting** future preparation time using a weighted average that favors recent entries
4. **Displaying** a live countdown during preparation ("~3:20 left") that accounts for completed and in-progress phases

Predictions improve with each completed print. Before any history exists the countdown is
theoretical - the thermal model's heating estimate plus per-printer default phase durations -
and the print select panel adds no preprint overhead at all until the first entry is recorded.

---

## Architecture

```
PrintStartCollector (owns lifecycle, phase detection, timing)
  |
  +-> PreprintPredictor (pure logic: weighted averages, remaining time)
  |     No LVGL, no Config, no threads. Fully unit-testable.
  |
  +-> Config persistence (/print_start_history/entries in settings.json)
  |
  +-> PrinterState subjects (print_start_time_left, preprint_remaining, preprint_elapsed)
        |
        +-> XML bindings (panel_widget_print_status.xml, print_status_preview_card.xml)
        +-> PrintStatusPanel observers (remaining/elapsed display integration)
```

### Key Design Decisions

- **PreprintPredictor is a pure-logic class** with zero dependencies on LVGL, Config, or threading. This makes it trivially testable.
- **PrintStartCollector owns the predictor instance** and manages loading/saving. It also owns the LVGL timer that periodically updates the ETA display.
- **Static convenience method** `PreprintPredictor::predicted_total_from_config()` allows UI code (print select panel, print status panel) to get predictions without access to the collector.

---

## PreprintPredictor Engine

### Data Model

```cpp
struct PreprintEntry {
    int total_seconds;                  // Total pre-print duration
    int64_t timestamp;                  // Unix timestamp when recorded
    std::map<int, int> phase_durations; // PrintStartPhase enum -> seconds
    int temp_bucket;                    // 0 = unknown, 1 = cold start, 2 = warm start
    PreprintWindow window;              // which arming window measured this entry
};
```

Phase keys are integer values of the `PrintStartPhase` enum:

| Value | Phase | Description |
|-------|-------|-------------|
| 0 | IDLE | Not in PRINT_START |
| 1 | INITIALIZING | PRINT_START detected |
| 2 | HOMING | G28 / Home All Axes |
| 3 | HEATING_BED | M140/M190 |
| 4 | HEATING_NOZZLE | M104/M109 |
| 5 | QGL | QUAD_GANTRY_LEVEL |
| 6 | Z_TILT | Z_TILT_ADJUST |
| 7 | BED_MESH | Bed mesh calibrate/load |
| 8 | CLEANING | Nozzle wipe |
| 9 | PURGING | Purge line |
| 10 | COMPLETE | Transition to printing |

IDLE, INITIALIZING and COMPLETE are lifecycle markers and are never timed. The two heating
phases are modelled by `ThermalRateModel` (`include/thermal_rate_model.h`) rather than
recorded by the predictor, so an entry's `phase_durations` carries only HOMING, QGL,
Z_TILT, BED_MESH, CLEANING and PURGING
(`src/print/print_start_collector.cpp#save_prediction_entry`).

### FIFO Entry Management

The predictor keeps a maximum of `PreprintPredictor::MAX_ENTRIES` = **10** entries
(newest at the end of the vector). When an 11th is added the oldest is evicted.
Recency bias comes from the weighting (below), not from a short buffer, so the
history can be deep enough to survive one odd print without going stale.

```
Entry added -> entries_.push_back(entry)
              -> while (size > MAX_ENTRIES) erase(begin)  // FIFO trim
```

`load_entries()` replaces all existing data and applies the same trim.

---

## Measurement Windows

The collector is armed at one of two moments, and the two measure different
quantities:

| Window | Armed when | What the entry includes |
|---|---|---|
| `PrinterEdge` | The printer reports its own print-start edge | Only work inside the job (`PRINT_START` proper) |
| `HostPreStart` | User commit (`begin_preparing()`) | Additionally any host-side pre-start block - a forced bed mesh, a printer setup macro - dispatched before `start_print` |

A host-side block can add minutes (on a K2 Plus that window alone runs ~455s),
so entries from the two windows are **never averaged together**. Loading history
passes the window the collector is currently measuring
(`PreprintPredictor::load_entries()`'s `PreprintWindow` parameter, which filters
before the temp-bucket logic runs), because mixing the populations produces an
estimate wrong for both and feeds a too-small predicted total into the
collector's adaptive timeout. The arming paths are documented in
[PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md).

Legacy entries recorded before window tagging existed (`window` absent on disk,
read back as `Unknown`) count as `PrinterEdge` - not as a wildcard. Commit
arming did not exist when they were recorded, so they can only have measured a
printer-edge window: they stay usable for externally started prints but never
stand in for a host-pre-start window (`entry_matches_window()`,
`src/print/preprint_predictor.cpp#entry_matches_window`). Passing `Unknown` as the *filter* is
the wildcard - it means "no window filter".

---

## Weighted History Averaging

Predictions use an **exponential time-decay weighted average** where newer entries
count more. Weight for entry `i` (oldest first) is `exp(lambda * i)`, normalized to
sum to 1, with `lambda = 0.23` (`compute_weights()`). That constant is `ln(10)/10`,
chosen so the oldest of a full 10-entry history carries roughly one eighth the
weight of the newest.

The scheme is continuous in the number of entries - there is no per-count weight
table. A single entry gets 100% by normalization.

### Per-Phase Calculation

Each phase is predicted independently. This matters because not all prints go through the same phases (some skip bed mesh, some skip QGL).

For each phase that appears in *any* entry:

1. Collect the entries that have timing data for this phase
2. **Redistribute weights** among only those entries (normalize to sum to 1.0)
3. Compute the weighted average and round to the nearest integer

**Example**: Three entries, but only entries 0 and 2 recorded BED_MESH:

```
Base weights: [0.2, 0.3, 0.5]  (3-entry scheme)
Entry 0 has BED_MESH: 80s      (weight 0.2)
Entry 1 does NOT have BED_MESH
Entry 2 has BED_MESH: 100s     (weight 0.5)

Redistribute: total_weight = 0.2 + 0.5 = 0.7
  Entry 0: 80 * (0.2/0.7) = 22.9s
  Entry 2: 100 * (0.5/0.7) = 71.4s
  Predicted BED_MESH = round(22.9 + 71.4) = 94s
```

### The Predicted Total Is Not the Sum of the Phases

`predicted_total()` (`src/print/preprint_predictor.cpp#predicted_total`) applies the same
time-decay weights to each entry's recorded `total_seconds` - the wall clock of the whole
measured window. It is deliberately **not** the sum of `predicted_phases()`.

That sum is only as complete as the phase matcher was. Heating never appears in
`phase_durations` at all, time before the first detected phase belongs to no phase, and
any macro time the profile's patterns failed to map to a `PrintStartPhase` belongs to no
phase either - so summing the per-phase predictions silently drops all of it. On a printer
whose narration a profile barely matches, the phase map can account for well under half of
the real elapsed time. The recorded wall clock has no such hole, which is why the ETA's
absolute total comes from it
(`src/print/print_start_collector.cpp#compute_predicted_weights`) while the per-phase
predictions supply only the relative shape.

### First Print, No History

With an empty history `predicted_phases()` returns `default_phase_durations()`
(`src/print/preprint_predictor.cpp#default_phase_durations`): the detected printer's
`print_start_default_phases` map from the printer database when its entry has one,
otherwise a generic map covering HOMING, BED_MESH, QGL, Z_TILT, CLEANING and PURGING.
`compute_predicted_weights()` combines that shape with the thermal model's heating
estimate and uses their sum as the absolute total, so a printer with no recorded prints
still shows a countdown - a theoretical estimate rather than an empirical one.
`predicted_total()` falls back to the same map summed.

A database entry lists only the phases that printer's start sequence actually runs (a
Trident tilts its bed and has no gantry to level, a V0 does neither), because a phase the
machine never performs inflates the first-print estimate. Heating is left out: the thermal
model owns it.

`predicted_total_from_config()` does not use the defaults. It returns 0 when there is no
history, so the print select panel adds no preprint overhead to a slicer estimate until at
least one entry has been recorded.

---

## Anomaly Rejection (per-phase MAD)

`add_entry()` accepts **any** duration. There is no total-duration cap.

A blanket cap on total duration would be a bug: a legitimate pre-print can exceed any
round number. A K2 Plus screen-started print whose window includes a host-side bed mesh
runs ~1140s (see "Measurement Windows"), and a cold ASA soak pushes it further. Such a cap
discards exactly the slowest printers, which are the ones most in need of an estimate.

Outliers are handled per-phase instead, by **median absolute deviation**: a phase
duration more than 3x MAD from the median for that phase is dropped from the
average for that phase only, leaving the rest of the entry usable. This is strictly
better than a total-duration cap, because one anomalous probe sequence no longer
throws away good homing and heating data recorded in the same print.

Covered by `PreprintPredictor MAD anomaly rejection` and `PreprintPredictor: add_entry
accepts any duration (MAD handles outliers)` in `tests/unit/test_preprint_predictor.cpp`.

---

## Where Phases Come From

A phase reaches the predictor only after something detected it, and on most printers that
something is the active print-start profile. Two of its feeds carry the printer's own
narration - console lines and a status object - and both land in
`PrintStartCollector::apply_profile_match()`, so phase weights, progress and ETA
re-baselining behave identically whichever one produced the match. A third kind of
evidence, physical inference over status frames, is treated differently and is covered
below. The profile schema itself is documented in
[PRINT_START_PROFILES.md](PRINT_START_PROFILES.md); what matters here is that an
undetected phase is an untimed phase, and undetected phases are - alongside heating, which
is excluded by design - what separates an entry's `total` from the sum of its `phases`.

**Console narration** (`response_patterns`) matches lines arriving on `gcode_response`.
On a printer whose start sequence is a `gcode_macro`, that stream is close to silent:
Klipper does not echo the commands a macro runs, so the command names a generic profile
watches for - `G28`, `M109`, `Z_TILT_ADJUST`, `BED_MESH_CALIBRATE` - never reach the
console at all, and a printer that spends four minutes homing, tilting and meshing can
finish preparation having matched no console pattern whatsoever.

**Status-object narration** (`phase_object` + `state_patterns`) reads a string field out of
a Klipper status object instead, and is what closes that gap. Community `PRINT_START`
macros narrate their own progress for the operator with `SET_DISPLAY_TEXT` / `M117`, which
Klipper publishes as `display_status.message`. The generic profile therefore declares
`display_status` / `message` as its phase object
(`assets/config/print_start_profiles/default.json`) and matches the human-readable text
the macro is already putting on the screen. The declared object is subscribed
automatically during discovery, via `PrintStartProfile::required_status_objects()`.

A profile that declares a phase object but no `state_patterns` matches the state string
against its `response_patterns` instead
(`src/print/print_start_profile.cpp#try_match_state`). The two feeds share one phase
vocabulary and, for a generic profile, largely the same words - "Homing", "Heating Bed",
"Bed Mesh" - so one pattern list serves both and there is no second copy to drift. A
profile whose two feeds need different text declares `state_patterns`, and those take
precedence.

### Status Signals Are Inference, Not Narration

A `status_signals` rule is a predicate over status frames - a heater below its target, the
toolhead parked at a cutter - so a match says what the printer *is doing*, never what it
*said*. `handle_status_signals()` applies its matches with `marks_real_signal=false`
(`src/print/print_start_collector.cpp#handle_status_signals`).

That flag gates the proactive temperature detector in `check_fallback_completion()`, which
derives HOMING and the two heating phases from live temperatures and homed axes for
printers that narrate nothing. The detector reads the same heater frames a status-signal
rule reads, so counting such a rule as a real signal would switch the detector off with
its own input and lose the phases only it can supply. Console lines and phase-object
states are narration, and those do mark a real signal: once the firmware is talking it is
authoritative and the proactive detector must stay quiet.

---

## Phase Timing Tracking

Phase durations are computed from timestamps, not explicitly timed:

1. When `update_phase()` is called with a new phase, the enter time is recorded in `phase_enter_times_` (a `map<int, steady_clock::time_point>`). IDLE, INITIALIZING and COMPLETE are not recorded, and a phase entered a second time keeps its first timestamp
2. On COMPLETE, `save_prediction_entry()` sorts phases by enter time and computes each phase's duration as the interval to the next phase (or to "now" for the last phase)

```
Phase enter times:          Duration calculation:
  HOMING      @ T+0s         HOMING: T+25 - T+0  = 25s
  HEATING_BED @ T+25s        (heating: not recorded in the entry)
  BED_MESH    @ T+115s       BED_MESH: T+145 - T+115 = 30s
  PURGING     @ T+145s       PURGING: now - T+145 = ~20s
```

The 90 seconds between T+25 and T+115 belong to no phase in that entry: the phases sum to
75s while the entry's `total` is the ~165s of wall clock. Three exclusions shape what gets
recorded:

- **Heating phases.** HEATING_BED and HEATING_NOZZLE are skipped. Heating time is modelled by `ThermalRateModel` from live temperatures, which adapts to the actual start temperature and target instead of averaging over prints that heated to different numbers.
- **Zero-second phases.** Two phases entered inside the same second give a duration of 0, which would drag that phase's median down and suppress the default floor that `compute_predicted_weights()` applies.
- **Whole entries from fallback completions.** When the collector completes on a timeout rather than on an end-of-preparation signal, `save_prediction_entry()` returns without saving. Those phases may be interrupted or incomplete, and a short bogus entry would shorten the adaptive timeout on the next print, which shortens the entry after that.

An entry with no timed phases at all is dropped ("No phase timings to save"), so a printer
whose narration the profile never matches accumulates no history.

---

## Real-Time Remaining Calculation

The live countdown is computed by `PrintStartCollector::update_eta_display()`, not by the
predictor alone: the predictor does not record heating, which is usually the largest part
of a pre-print.

`compute_predicted_weights()` builds a composite per-phase duration map - `ThermalRateModel`
estimates for the two heating phases, `predicted_phases()` for the rest - and normalizes it
into `predicted_phase_weights_`, fractions summing to 1.0. The absolute total is
`predictor_.predicted_total()` when there is history and the composite sum when there is
not. Weights are recomputed when a heater target first appears or rises substantially,
because a bed-first macro issues its `M109` long after preparation begins and the nozzle
phase carries no weight until it does.

Each tick sums `weight * predicted_total_seconds_` across the phases:

1. **Completed phases**: skip (actual time was spent, not predicted)
2. **Current phase**: a heating phase contributes its unheated fraction; BED_MESH extrapolates from the measured per-probe time once probes are arriving; anything else contributes `max(0, predicted - elapsed)`, so a phase that overruns contributes 0 rather than a negative
3. **Future phases**: the full predicted duration, except a heating phase whose heater is already climbing, which owes only its unheated fraction

The result is then smoothed before it is published: it never increases, and each tick's
decrease is capped at `max(15s, previous/6)`, so a phase transition eases the number down
instead of snapping it. A phase change releases the monotonic anchor, letting the
countdown step to an honest larger value when entering a phase costs more than the
countdown had left.

`PreprintPredictor::remaining_seconds()` implements the same skip / count-down / add walk
over `predicted_phases()` alone, without the thermal model or the smoothing. It is the
predictor's pure-logic form of the calculation and is what the predictor unit tests
exercise; no production path calls it.

### Update Frequency

An LVGL timer (`eta_timer_`) fires every **5 seconds** and calls `update_eta_display()`, which:

1. Updates `preprint_elapsed_seconds` subject (total time since preparation started)
2. Recomputes remaining from the composite phase weights (above)
3. Updates `preprint_remaining_seconds` subject (integer, for programmatic use)
4. Updates `print_start_progress` when a predicted total exists, never letting it regress
5. Formats and sets `print_start_time_left` subject (string, e.g. "~3:20 left")

When remaining reaches 0, the display shows "Almost ready".

---

## History Persistence

### Storage Location

Entries are stored in the main config file (`config/settings.json`) at the JSON path `/print_start_history/entries`.

### JSON Schema

```json
{
  "print_start_history": {
    "entries": [
      {
        "total": 165,
        "timestamp": 1700000000,
        "phases": {
          "2": 25,
          "7": 30,
          "9": 20
        },
        "temp_bucket": 1,
        "window": 2
      }
    ]
  }
}
```

- `total`: wall-clock seconds for the whole measured window, from the moment the collector armed to completion (`src/print/print_start_collector.cpp#save_prediction_entry`). Larger than the sum of `phases`, and deliberately so - heating never appears in `phases` (the 90s in the timing example above has no home there), nor does time before the first detected phase, nor any stretch the matcher did not map. **The gap between the two numbers is a diagnostic.** Once heating is accounted for, a `total` still far above the sum of its `phases` means the profile is missing phases on that printer
- `timestamp`: Unix timestamp when the entry was recorded
- `phases`: Map of `PrintStartPhase` enum int (as string key) to duration in seconds
- `temp_bucket`: Optional. 1 = cold start (bed below 40°C at start), 2 = warm start (40°C or above); omitted means unknown. Older versions stored the raw nozzle target temperature here; those values are dropped at load time as a one-shot migration.
- `window`: Optional. `PreprintWindow` enum int - 1 = `PrinterEdge`, 2 = `HostPreStart`; omitted means legacy (read back as `Unknown`, treated as `PrinterEdge` when filtering). See "Measurement windows" above.

### Save Flow

1. `PrintStartCollector::save_prediction_entry()` computes durations from `phase_enter_times_`, returning early on a fallback-timeout completion or when no phase was timed
2. Adds the entry to the in-memory predictor via `add_entry()` (which enforces the FIFO trim to `MAX_ENTRIES`)
3. Takes the predictor's entries - which cover only the temp bucket it loaded - and merges them with the entries already on disk that belong to *other* buckets, trimming the merged list to 15
4. Schedules the serialization plus `Config::set()` and `Config::save()` onto the main thread via `queue_update()`. The same save flushes the thermal model's learned heating rates (`ThermalRateManager::save_to_config()`), which is where the heating half of an ETA lives

### Load Flow

1. `PrintStartCollector::start()` calls `load_prediction_history()`
2. Which calls `PreprintPredictor::load_entries_from_config()` (static method)
3. Reads JSON from Config, deserializes to `vector<PreprintEntry>`
4. Passes to predictor's `load_entries()`

### Caching

`predicted_total_from_config()` caches its result for **60 seconds** using atomic variables. This avoids re-parsing config JSON for every file in the print selection list (each file card calls this to augment the slicer time estimate with preprint overhead).

---

## Integration with Print Status UI

### LVGL Subjects

Three subjects carry prediction data from the collector to the UI:

| Subject Name | Type | Description |
|-------------|------|-------------|
| `print_start_time_left` | string | Formatted ETA (e.g., "~2:30 left", "Almost ready", or "") |
| `preprint_remaining` | int | Remaining seconds for pre-print (for programmatic use) |
| `preprint_elapsed` | int | Seconds since preparation started |

### XML Bindings

**Home panel print-status widget** (`ui_xml/components/panel_widget_print_status.xml`): shows the ETA text below the phase message during preparation:
```xml
<text_small name="print_start_eta" bind_text="print_start_time_left"
            style_text_color="#text_muted"/>
```

**Print status panel** (`ui_xml/components/print_status_preview_card.xml`, shared by
the landscape and portrait layouts): Shows ETA in the preparing overlay:
```xml
<text_small name="preparing_eta" bind_text="print_start_time_left"
            style_text_color="#text" style_text_opa="180"/>
```

### PrintStatusPanel Observer Integration

The print status panel uses `preprint_remaining` and `preprint_elapsed` observers to take over the standard elapsed/remaining time displays during the Preparing state:

- **`on_preprint_elapsed_changed()`**: During Preparing, updates the elapsed time display. This provides accurate phase-level elapsed tracking (Moonraker's `total_duration` includes all time since the job was queued, which may not align with actual preparation start).

- **`on_preprint_remaining_changed()`**: During Preparing, combines the preprint remaining seconds with the slicer's estimated print time to show a total wall-clock remaining time. Formula: `slicer_time + preprint_remaining`.

- **State transition to Printing**: When the state changes from Preparing to Printing, the panel transitions the remaining display back to Moonraker's `time_left` value. Without this explicit transition, the display would stay stuck on the last preprint prediction value.

### Print Select Panel Integration

The print file browser (`ui_panel_print_select.cpp`) augments the slicer's estimated print time with the predicted preprint overhead:

```cpp
int preprint_seconds = helix::PreprintPredictor::predicted_total_from_config();
int total_minutes = print_time_minutes + (preprint_seconds + 30) / 60;
```

This gives users a more realistic wall-clock time estimate that includes heating and preparation, not just the slicer's extrusion time.

---

## Troubleshooting Inaccurate Predictions

### Predictions Too High

- **Cause**: History includes entries where heating took unusually long (cold room, different filament requiring higher temp).
- **Fix**: Predictions naturally improve after 2-3 prints as old entries are evicted by FIFO.

### Predictions Too Low

- **Cause**: New phase added to PRINT_START macro (e.g., added QGL) that wasn't in history.
- **Fix**: The system handles missing phases via weight redistribution. After 1-2 prints with the new phase, predictions will include it.

### Generic Estimate on the First Print

- **Cause**: No history exists yet (fresh install or config reset), so the countdown runs off the thermal model plus `default_phase_durations()` and the print select panel adds no preprint overhead at all.
- **Fix**: Complete one print. History-based prediction starts with the first recorded entry.

### Predictions Not Updating

- **Cause**: The loaded history was filtered to empty - by window (a commit-armed print sees only `HostPreStart` entries, and legacy entries never match it), by temp bucket, or by the one-shot migration that drops legacy `temp_bucket` values.
- **Cause**: Nothing is being recorded. A fallback-timeout completion and an entry with no timed phase are both dropped, so a printer that keeps timing out, or whose phases the profile never matches, never adds history.
- **Debug**: Run with `-vv` and look for `[PrintStartCollector] Saved prediction history` or `No phase timings to save` log messages, and the predictor's dropped-legacy-entries count at load.

### `total` Far Above the Sum of `phases`

- **Cause**: The profile is not matching that printer's narration, so most of preparation falls outside any phase. On a macro-driven Klipper printer that usually means the macro echoes nothing to the console and writes nothing to `display_status.message` either.
- **Fix**: Capture what the printer actually says and give its profile the patterns, or a `phase_object`, to match it - [PRINT_START_PROFILES.md](PRINT_START_PROFILES.md) covers both. The ETA total stays honest either way, since it comes from wall clock, but progress and the per-phase countdown get coarser the more of the sequence goes undetected.

### Clearing Prediction History

Delete the `print_start_history` key from `config/settings.json` and restart:

```json
{
  "print_start_history": {
    "entries": []
  }
}
```

---

## Developer Guide: Tuning Weights

### Changing the Weighting Scheme

Weights are computed continuously in `PreprintPredictor::compute_weights()` in `src/print/preprint_predictor.cpp`: weight for entry `i` (oldest first) is `exp(lambda * i)`, normalized to sum to 1, with `lambda = 0.23` (= `ln(10)/10`, so the oldest of a full 10-entry history carries roughly one eighth the weight of the newest).

To change the recency bias, change `lambda`. **More aggressive recency** (ignores old data faster): raise it. **More conservative** (smooths out variance): lower it. There is no per-count weight table to keep in sync - any entry count works, and a single entry gets 100% by normalization.

### Changing the History Depth

`MAX_ENTRIES` controls how many entries are kept. It is 10 (`include/preprint_predictor.h`). Raising it:

- **Pros**: More data smooths out outliers
- **Cons**: Slower to adapt to changes (new filament, different printer config)

Because the weighting is continuous (above), no other code needs to change with the entry count.

### Changing the Anomaly Threshold

There is no total-duration cap. Outlier rejection is per-phase MAD: a duration more than 3x the median absolute deviation from that phase's median is dropped from that phase's average (`src/print/preprint_predictor.cpp`, the `3.0 * mad` comparison in `predicted_phases()`). Adjust the multiplier there if legitimate variation is being filtered.

### Changing the ETA Update Interval

`ETA_UPDATE_INTERVAL_MS = 5000` in `print_start_collector.h`. Lower values give smoother countdowns but consume more CPU. Since the countdown subtracts elapsed time in real-time, 5 seconds is a reasonable balance.

---

## Testing

Unit tests are in `tests/unit/test_preprint_predictor.cpp`. Cases carry either
`[print][predictor]` or `[preprint_predictor]`, so run both tags:

```bash
./build/bin/helix-tests "[predictor],[preprint_predictor]"
```

Tests cover:
- Empty state (no predictions without history)
- Single entry (100% weight)
- Two entries favor the newer entry; three favor the newest
- FIFO trimming to `MAX_ENTRIES` (11th entry evicts oldest)
- `add_entry` accepts any duration - MAD handles outliers (no total-duration cap)
- Phases appearing in subset of entries (weight redistribution)
- Remaining time with no progress, partial progress, exceeded prediction
- All phases completed returns 0
- Unknown current phase contributes 0
- `load_entries` replaces existing data
- `load_entries` caps at `MAX_ENTRIES`
- `temp_bucket` filtering, including legacy bucket=0 entries matching any filter
- Window filtering: host-pre-start rejects printer-edge history and vice versa; legacy entries count as printer-edge, not host-pre-start

The predictor has no LVGL or Config dependencies, making tests fast and deterministic.

---

## Key Source Files

| File | Purpose |
|------|---------|
| `include/preprint_predictor.h` | PreprintPredictor class and PreprintEntry struct |
| `src/print/preprint_predictor.cpp` | Weighted average algorithm, config loading, caching |
| `include/print_start_collector.h` | PrintStartCollector with predictor ownership and ETA timer |
| `src/print/print_start_collector.cpp` | Phase timing, history persistence, ETA display updates |
| `include/printer_print_state.h` | Subject declarations (preprint_remaining, preprint_elapsed, etc.) |
| `src/printer/printer_print_state.cpp` | Subject initialization and setters |
| `src/ui/ui_panel_print_status.cpp` | Observer integration for elapsed/remaining display |
| `src/ui/ui_panel_print_select.cpp` | Augments slicer time estimates with preprint prediction |
| `include/thermal_rate_model.h` | Heating-time model - owns the two phases the predictor never records |
| `assets/config/print_start_profiles/default.json` | Generic profile: the patterns and phase object that decide which phases get detected at all |
| `tests/unit/test_preprint_predictor.cpp` | Unit tests for prediction logic |
