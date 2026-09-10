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
  +-> PreprintPredictor (pure logic: weighted averages, per-phase shape)
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

In memory the keys are `PrintStartPhase` enum ints. On disk they are the phase
NAMES (`include/print_start_phase.h`), because an ordinal is a position rather
than an identity: inserting a phase renumbers every phase after it, and a stored
number then names a different one.

| Phase | Stored as | Description |
|-------|-----------|-------------|
| IDLE | `IDLE` | Not in PRINT_START |
| INITIALIZING | `INITIALIZING` | PRINT_START detected |
| HOMING | `HOMING` | G28 / Home All Axes |
| HEATING_BED | `HEATING_BED` | M140/M190 |
| SOAKING | `SOAKING` | Holding at temperature: heat-soak dwell or chamber wait |
| HEATING_NOZZLE | `HEATING_NOZZLE` | M104/M109 |
| QGL | `QGL` | QUAD_GANTRY_LEVEL |
| Z_TILT | `Z_TILT` | Z_TILT_ADJUST |
| BED_MESH | `BED_MESH` | Bed mesh calibrate/load |
| CLEANING | `CLEANING` | Nozzle wipe |
| PURGING | `PURGING` | Purge line |
| COMPLETE | `COMPLETE` | Transition to printing |

`print_start_phase_stores_duration()` is the single answer to which of these the
history keeps a duration for: HOMING, SOAKING, QGL, Z_TILT, BED_MESH, CLEANING
and PURGING. IDLE, INITIALIZING and COMPLETE are lifecycle markers with no dwell
of their own, and the two heating phases are owned by `ThermalRateModel`, which
predicts them from measured heat rates.

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

`compute_predicted_weights()` builds the composite duration map the countdown reads:
`ThermalRateModel` estimates for the two heating phases, `predicted_phases()` for the rest,
normalized into `predicted_phase_weights_` as fractions summing to 1.0. The absolute total is
`predictor_.predicted_total()` when history exists and the composite sum when it does not.
Weights are recomputed when a heater target first appears or rises substantially, because a
bed-first macro issues its `M109` long after preparation begins and the nozzle phase carries
no weight until it does.

The live countdown is the collector's, not the predictor's: `PrintStartCollector#update_eta_display`. The predictor supplies the historical shape (per-phase weights and a wall-clock total); the collector is what turns that into a number on screen, because three of the four inputs are live printer state the predictor never sees.

Each phase's duration is `weight * predicted_total_seconds_`, and each phase contributes:

| Phase | Contribution |
|-------|--------------|
| Completed | 0 - the time was actually spent, not predicted |
| Current, heating | `duration * (1 - compute_heating_fraction())` - temperature progress, not elapsed time |
| Current, bed mesh with live probe timing | `mesh_seconds_per_probe_ * probes_left` - measured per-probe rate overrides history |
| Current, anything else | `max(0, duration - elapsed_in_phase)` |
| Future, heating | `duration * (1 - heating fraction)` - concurrent-heat firmware starts a heater before the chain reaches its phase, so it only owes the unheated remainder |
| Future, anything else | full duration |

A phase whose elapsed time exceeds its prediction contributes 0, never a negative.

Three guards then shape the raw number before it reaches a subject, because a recomputation of the weights (a newly discovered heater target, say) can move it sharply in either direction:

- **Monotonic clamp** - remaining never increases.
- **Monotonic bias under two minutes** - inside the last 120s an increase is suppressed outright unless it overruns the predicted total by more than 20%.
- **Downward rate limit** - each tick may drop at most `max(15, last/6)` seconds, so a phase change eases the number down instead of collapsing it (111 -> 57 -> 31 in three ticks reads as broken).

### Update Frequency

An LVGL timer (`eta_timer_`) fires every **5 seconds** and calls `update_eta_display()`, which:

1. Updates `preprint_elapsed_seconds` subject (total time since preparation started)
2. Computes remaining from the composite weights above
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
          "HOMING": 25,
          "SOAKING": 90,
          "BED_MESH": 30,
          "PURGING": 20
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
- `phases`: Map of `PrintStartPhase` NAME to duration in seconds. A name this build does not
  recognise is skipped and the rest of the entry still loads, so a document written by a newer
  build stays usable. Configs written before schema version 25 keyed this by enum ordinal;
  `migrate_v24_to_v25` converts them.
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
