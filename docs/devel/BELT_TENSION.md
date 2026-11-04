# Belt Tension (resonance comparison)

Developer guide for the belt tension check: two Klipper resonance sweeps, one per CoreXY
diagonal, compared as curves. The predecessor tool (hand-plucked belts on a live
accelerometer stream) is `BELT_TUNER.md`.

**User guide**: `../user/guide/calibration.md` § Belt Tension

---

## Overview

The check runs Klipper's `TEST_RESONANCES` down each belt path, reads the CSV Klipper
writes, and overlays both frequency responses. The two curves are compared the way
Shake&Tune compares belts: a band-limited Pearson similarity as the headline verdict, plus
each path's peaks paired by frequency with unpaired peaks flagged. A re-test sweeps one
path and keeps the other, ghosting the replaced curve.

Path A is the `AXIS=1,-1` diagonal and Path B the `AXIS=1,1` one. The letters follow the
Voron motor names Shake&Tune uses: a 1,1 move turns only `stepper_x`, which on a Voron is
the B motor, so a user comparing this screen with Shake&Tune or the Voron documentation
sees the same letters on the same belts.

### Which belt is which

CoreXY moves `stepper_x` by x+y and `stepper_y` by x-y, so Path A's sweep turns only the
`stepper_y` motor and Path B's only `stepper_x`, on every CoreXY whatever its belt routing
(`include/belt_tension_types.h#belt_path_stepper`). Which physical belt that motor drives
depends on the printer, so the user names the paths rather than the app guessing:

- **Identify** (`BeltTensionPanel::handle_identify_clicked`) buzzes a path's motor with
  `STEPPER_BUZZ STEPPER=<stepper>` through `BeltTensionCalibrator::buzz_path`. Klipper's
  `force_move` is always loaded and the buzz needs no homing: the motor moves 1 mm back and
  forth ten times (about 10 s). On an AWD CoreXY it moves only the named motor of the pair on
  that belt, which still moves the belt and so still identifies it. The calibrator reads `BUZZING` until Klipper answers, which
  closes Start's gate (`bt_can_start`), so a buzz is allowed exactly when Start is, never
  during a sweep and never two at once. A refusal shows Klipper's message in the dialog
  (`bt_identify_status`, `bt_identify_state` 2) and leaves the view where it was.
- **The layout** is one choice of pairs, so the two names can never contradict: not set
  ("Path A" / "Path B"), A left B right, A right B left, A upper B lower, A lower B upper.
  It is stored per printer as a `BeltLayout` int (`settings_belt_layout`,
  `belt_tension/layout` under the printer's config node) and saved the moment it is tapped.
- **Every visible name** comes from `include/ui_belt_path_names.h#belt_path_name`, which
  returns a `title` ("Left belt", for labels and the start of a sentence) and a `phrase`
  ("the left belt", inside a sentence). The panel observes the setting and the language and
  re-renders every name (`BeltTensionPanel::refresh_names`): the RESULTS headers and Re-test
  buttons, the unpaired-peak line, the run title, the tuning strings and the START sketch,
  which asks `belt_path_name` as it draws. Log lines keep the letter.

The dialog (`ui_xml/belt_names_modal.xml`, `helix::ui::BeltNamesModal`) opens from START's
"Which belt is which?" button, the RESULTS path headers, and in the tuning view the pencil
beside the tensioner line ("Tensioner: Left belt") or, on compact layouts, beside the one-line
instruction; it leaves the session as it was. The reference-lost hint says "Tap the pencil",
which names a target in every layout. A title stands only where it
reads as a label (a header, a button, after a colon, at the start of an English sentence whose
translations also start with it); everywhere else the phrase form goes inside the sentence, so
no locale gets a capitalised name mid-sentence.

Start is gated, in `src/calibration/belt_gating.cpp#evaluate_belt_gate`:

| Gate leg | Meaning |
|----------|---------|
| `NOT_CONNECTED` | no Moonraker, or klippy not READY (a dead klippy refuses gcode, so a sweep would only stall) |
| `NO_ACCELEROMETER` | no accelerometer in the configfile config |
| `NOT_COREXY` | kinematics is not `corexy`; the two diagonals the comparison needs exist only there |
| `NOT_COLOCATED` | Klipper's unix socket is not reachable: HelixScreen is not running on the printer's own computer, so it cannot read the CSV |
| `PRINTING` | a print is active; the toolhead is not free to move |

The panel computes the gate in one place, `src/ui/calibration/ui_panel_belt_tension.cpp#refresh_gate`,
plus a calibrator-idle leg; `bt_can_start` binds the Start button's disabled state, so the
gate holds however the panel was reached (row click, `ctl navigate`, a demo entry, an
accelerometer that drops out after entry).

Start and Re-test also pass through `BeltTensionPanel::run_after_ram_check`: on a host with
less than `kResonanceLowRamWarnMb` (200 MB) it shows input shaper's shared
`show_low_ram_resonance_warning` first. Klipper analyses each sweep on the printer's own
host, which is this one (the co-location gate), and on a 128 MB board that analysis can be
OOM-killed, leaving klippy wedged until it is restarted.

Z belts are deliberately unsupported; see the header comment on
`include/belt_tension_types.h` for the measurement that closed that door.

## Key files

| File | Role |
|------|------|
| `src/ui/calibration/ui_panel_belt_tension.cpp`, `include/ui_panel_belt_tension.h`, `ui_xml/panel_belt_tension.xml` | The panel: five view states (START, RUNNING, RESULTS, ERROR, TUNING), subjects, gate wiring, run queue, re-test and ghost bookkeeping |
| `src/ui/calibration/ui_belt_path_sketch.cpp` | The START-screen sketch widget drawing the two diagonals, labelled with the path names |
| `src/ui/calibration/ui_belt_path_names.cpp`, `include/ui_belt_path_names.h` | `belt_path_name`: every visible path name, from the saved layout |
| `src/ui/calibration/ui_belt_names_modal.cpp`, `ui_xml/belt_names_modal.xml` | "Which belt is which?": Identify and the layout choice |
| `src/calibration/belt_tune_session.cpp`, `include/belt_tune_session.h` | `BeltTuneSession`: the tuning loop's pure logic (zone candidates, already-matched entry, calibration, baseline pair and repeatability, centroid tracking, outcomes, advice, power hint) and the `belt_tune` thresholds |
| `src/calibration/belt_settle.cpp`, `include/belt_settle.h` | `plan_belt_settle`: the settling laps' plan and script |
| `src/ui/calibration/ui_belt_gap_gauge.cpp`, `include/ui_belt_gap_gauge.h` | The `<belt_gap_gauge>` widget and its pure helpers `belt_gap_gauge_x` / `belt_gap_severity` |
| `src/printer/printer_detector.cpp#get_belt_tensioner` | The `belt_tensioner` printer-database field as a `BeltTensionerKind` |
| `src/calibration/belt_tension_calibrator.cpp` | Measures one path per call (full, or over a window): home if needed, sweep, cancel, `emergency_abort()`; `settle()` runs the laps; `buzz_path()` identifies a path's motor |
| `src/calibration/belt_tension_types.cpp` | `compare_belt_paths` (band similarity, paired peaks) with `band_similarity` / `detect_belt_peaks` / `pair_belt_peaks` / `verdict_for_similarity` |
| `src/calibration/resonance_console.cpp` | `[resonance_tester]` config query and console-line parsing shared with input shaper |
| `src/calibration/shaper_csv_parser.cpp` | `parse_resonance_csv`: reads the CSV, returns `(freq, psd_xyz)` pairs plus the in-plane `psd_x + psd_y` curve, or a classified error |
| `src/api/moonraker_advanced_api.cpp` | `BeltResonanceCollector` (console follow + CSV read) and `test_belt_resonance` (entry point); `detect_belt_hardware` for kinematics |
| `src/calibration/live_sweep_preview.cpp`, `include/live_sweep_preview.h` | `LiveSweepPreview` and the single gate `live_sweep_target`: the live curve while a sweep runs (§ Live preview) |
| `src/calibration/sweep_response_estimator.cpp`, `include/sweep_response_estimator.h` | Pure estimator: `SweepResponseAccumulator` turns accelerometer samples into one PSD point per Hz, `DriveLock` finds where the ramp is in the samples |
| `src/ui/calibration/ui_frequency_response_chart.cpp` | Shared chart: series styles, muted ghost series, sweep cursor, numbered and hollow markers, percent gridline labels, label gutters sized from the font (see `INPUT_SHAPER.md` § Axis label gutters) |
| `src/calibration/belt_gating.cpp` | The pure Start-gate predicate |
| `src/api/moonraker_client_mock.cpp` | `dispatch_test_resonances_response`: the `--test` simulation of a sweep |

## Flow

```
BeltTensionPanel (LVGL thread)
  |  handle_start_clicked / handle_retest_clicked
  v
BeltTensionCalibrator::measure_path(path)              belt_tension_calibrator.cpp
  |  ensure_homed_then(...)                            toolhead_homing.h
  v
IAdvancedAPI::test_belt_resonance(axis, name, ...)     moonraker_advanced_api.cpp
  |  1. BeltResonanceCollector::start()                (console subscription)
  |  2. query_resonance_tester_config(client)          resonance_console.cpp
  |     -> collector->set_config(cfg)  [progress hold]
  |  3. execute_gcode TEST_RESONANCES AXIS=<1,-1|1,1>
  |                      OUTPUT=resonances NAME=helix_belt_{a,b} SWEEPING_PERIOD=0
  v
Klipper
  |  "Testing frequency %.0f Hz"          -> on_progress (percent + cursor)
  |  "Resonances data written to <path>"  -> read_result(path)
  v
parse_resonance_csv(path)                              shaper_csv_parser.cpp
  |  (freq, psd_xyz) pairs | classified error
  v  on_complete / on_error  (marshalled to the LVGL thread by lifetime_.bg_cb)
BeltTensionPanel::on_sweep_complete -> next path, then finish_run()
  |
  v
compare_belt_paths(curve_a, curve_b, sweep min, sweep max)  belt_tension_types.cpp
  -> BeltComparison -> populate_results(): similarity, verdict chip, peak facts, markers
```

Two threading rules shape the middle of that flow. Every collector callback arrives on the
WebSocket thread; the calibrator wraps each one in `lifetime_.bg_cb(...)` so the panel only
ever hears on the LVGL thread, and a run cancelled mid-flight expires the tokens so late
callbacks die at the guard. And the sweep's G-code rides the long calibration timeout with
`caller_surfaces_errors=true`; a transport loss is ignored (Klipper keeps sweeping and the
console lines keep arriving), everything else completes the collector with an error.

Progress needs the printer's own range: `sweep_percent` scales the current "Testing
frequency" line against the `[resonance_tester]` config queried alongside the sweep, and
the collector holds progress lines until `set_config` lands so the first percent is not
computed against the 5-135 Hz default when the printer sweeps something else. The config
query always answers (defaults on error), so the hold is finite.

The panel owns stall detection: `BeltTensionPanel::kStallTimeoutMs` (120 s, an
`OperationTimeoutGuard`) fires when no progress line arrived, because Klipper prints one
every second. `BeltResonanceCollector` carries no deadline of its own. The stall message
names the likeliest cause on a small board, Klipper's analysis running out of memory, and
tells the user to power-cycle (or restart Klipper over SSH): an OOM-killed analysis leaves
klippy wedged, the Moonraker firmware-restart RPC cannot reach it, and on the CC1 and AD5M
Moonraker's service manager does not list Klipper at all.

Start refuses outright when `MemAvailable + SwapFree` is under
`kResonanceMinHeadroomMb` (64 MB), and asks first on a board under 200 MB total
(`include/memory_utils.h#resonance_memory_check`, shared with input shaper). On the CC1
the OOM killer takes HelixScreen before the analysis, so the stall error never shows
there: the check has to happen before Start.

**Stop** is not a pause. A sweep cannot be resumed, so `handle_stop_clicked` calls
`BeltTensionCalibrator::emergency_abort`, which is
`include/calibration_abort.h#emergency_stop_and_restart`: M112, then FIRMWARE_RESTART to
clear the movement state the aborted sweep left behind. The button is "Stop" with the
`alert_octagon` icon on every view that can be measuring (`btn_stop`, `btn_tune_stop`),
matching the other emergency-stop controls.

## Live preview

While a sweep runs, the chart draws the measured response as it arrives instead of waiting
for the CSV. The sweep lasts minutes, and a flat chart for that long reads as a hang.
Classes: `include/live_sweep_preview.h#LiveSweepPreview` (stream lifecycle),
`SweepResponseAccumulator` in `include/sweep_response_estimator.h` (samples to points). The Input
Shaper panel and wizard step use the same two classes (`INPUT_SHAPER.md` § Live sweep chart).

Four signals meet here, and each answers a different question:

| Signal | Says | Used for |
|--------|------|----------|
| Accelerometer stream (klippy's dump endpoint, the client in `BELT_TUNER.md`) | The measured acceleration, per sample | The curve itself: a single-bin PSD per Hz |
| Console `Testing frequency N Hz` | Roughly where the sweep is; printed when a move is planned, a lookahead buffer ahead of the motion | Starts the stream, and narrows the drive search (`note_console_frequency`) |
| `[resonance_tester]` config | The ramp: start, end, `hz_per_sec`, the accelerometer chip | `live_sweep_target`, `sweep_plan_for` |
| The CSV | Klipper's own analysis of the finished sweep | Replaces the preview, and is the only source of every verdict |

**The lock.** The console frequency is not used to place samples on the frequency axis.
Because it runs ahead of the motion by an unknown buffer, a step binned by console time would
sit seconds off. `DriveLock` instead reads the drive frequency out of the samples (the
strongest line near the console value, against off-drive reference bins) and fixes the
ramp's start time once enough readings agree. Until it locks, no point is emitted as clean.

**NaN is a gap, not zero.** A step is unclean when its samples had a gap, fell out of the
buffer, or finished while the lock was lost. `layout_preview` puts NaN in that slot, and the
chart skips it (`INPUT_SHAPER.md` § NaN gaps). A zero would draw a false dip.

**Never a verdict.** The preview only draws. Similarity, peaks, the tuning loop's centroids
and every outcome come from the CSV curves, so a wrong or partial preview cannot move a
recommendation.

**No fallback.** With no stream the preview does not draw, and the chart behaves as it does
without the feature. No stream means: the EMBEDDED tier (no chart at all), a klippy socket
the screen cannot reach (`live_sweep_target` returns nullopt), a sweep recorded by two
different chips, a chart the layout hides, or `HELIX_SWEEP_PREVIEW_OFF=1`. The console
timing is deliberately not drawn in its place: it would present a guess as a measurement.

**Arming.** `BeltTensionPanel::arm_preview` runs when a path's sweep starts, after the
automatic settling laps, which are a separate step. The preview then waits for the sweep's
first console line before it opens the stream, so the tester's own client already holds the
chip. The socket path comes from the same probe the Start gate uses
(`BeltTensionPanel::probe_klippy_socket`), the ramp and chip from the panel's
`[resonance_tester]` answer (`sweep_cfg_`), and a narrowed tuning window from `run_window_`.

**Rebuild.** A hot-reload rebuild frees the widget tree without `on_ui_destroyed()`, and the
chart is a child of a host inside it, so `BeltTensionPanel::on_deactivating` calls
`on_ui_destroyed()` on `DeactivateReason::Rebuild` while the tree is alive. The run is
cancelled with it, and the next run makes the chart again.

**Per-path series and the CSV swap.** Each path has its own preview series
(`preview_series_`), drawn on its own 0-100% scale (the final curves share one, so the swap
rescales). What happens when a path's sweep completes depends on `run_kind_`:

| Run | Preview column | On `on_sweep_complete` |
|-----|----------------|------------------------|
| Full check (Test Both, Re-test): `RunKind::CHECK` | `SweepColumn::XYZ` | The CSV curve replaces that path's preview at once |
| Calibration, baseline and tune runs | `SweepColumn::IN_PLANE` (`psd_x + psd_y`) | The finished path's preview stays frozen until the round's chart push, because the loop draws only whole rounds |

The column matches the CSV column the same run reads, so the swap changes the numbers only
by what Klipper's own analysis adds. `BeltTensionPanel::refresh_preview_series` is the one
place this is decided.

**Threading.** The stream's callbacks arrive on its own libhv loop thread, where the
accumulator runs and nothing else. Finished points cross to the LVGL thread through a
`LifetimeToken::defer`; `LiveSweepPreview::stop` expires that token before it joins the
loop, so a point already queued is dropped instead of reaching a destroyed panel. The loop
thread takes nice 10 (`kPreviewLoopNice`), since a starved klippy fails the sweep's moves.
The stream stops at the last point, at `finish()`, on a source error and on every teardown,
so the preview never keeps the accelerometer running. A sweep's last console line (within
`kPreviewLastLineSlackHz` of the plan's end) ends a stream that never locked a grace later,
so a run with no lock does not stay subscribed through Klipper's analysis.

**CB1 cost.** The CPU cost on a BTT CB1 (Voron 2.4) is a pending hardware check, not a
measured fact. Procedure: run the same full check twice, once normally and once with
`HELIX_SWEEP_PREVIEW_OFF=1`, and compare the loop thread's CPU from the `[LiveSweepPreview]`
stop line, klippy's and helix-screen's CPU over the sweep from `/proc/<pid>/stat`, and the
count of `Timer too close` lines in klippy.log. The result will be recorded in the GitHub
issue for the preview (prestonbrown/helixscreen#1812); this section carries no numbers
until it lands.

## Klipper facts the implementation leans on

- `TEST_RESONANCES AXIS=1,-1` and `AXIS=1,1` excite the two CoreXY diagonals. Which
  physical belt each diagonal loads depends on the printer's geometry, so the app never
  guesses a belt name: the paths are "Path A" and "Path B" until the user names them,
  having identified each by buzzing its motor (§ Which belt is which).
- `STEPPER_BUZZ STEPPER=<name>` (Klipper's `force_move`, always loaded) moves one stepper
  1 mm and back ten times. It needs no homing and no `enable_force_move`.
- `SWEEPING_PERIOD=0` forces the pulse-only excitation. Mainline Klipper's
  `[resonance_tester]` defaults to the sweeping test (1.2 s), Kalico's to pulse-only, and
  firmware older than the sweeping test has no such parameter and ignores it. Shake&Tune's
  own guidance is that the sweep smooths over mechanical faults (loose belts, binding), which
  is what a belt comparison looks for.
- `OUTPUT=resonances` makes Klipper write `resonances_<axis>_<name>.csv`, usually in `/tmp`
  (the Snapmaker U1 writes to `/userdata/gcodes/shaper_calibrate/`). Only the path
  announced on the console in THIS run is read, so a stale file from an earlier run cannot
  be mistaken for a result (`BeltResonanceCollector::on_gcode_response`).
- The CSV's `psd_xyz` column is the combined response. Per-chip columns instead of
  `psd_xyz` mean more than one accelerometer reported; that is `ResonanceCsvError::MULTI_CHIP`,
  surfaced as "More than one accelerometer reported. Belt Tension supports one."
- `TEST_RESONANCES` cannot sweep Z at all, and a toolhead accelerometer barely sees a Z
  belt; see `include/belt_tension_types.h`.
- Klipper defaults for `[resonance_tester]` are 5-135 Hz at 1 Hz/s: two sweeps of about
  130 s each. The START screen shows the printer's actual range as
  "N-M Hz · about K min" from `query_hw_facts`, the settling laps included.
- `FREQ_START` / `FREQ_END` narrow a sweep, and Klipper takes a `FREQ_END` up to 300 Hz
  whatever `max_freq` says. A K1C (max_freq 133.33) shut its MCU down ("Move queue
  overflow") on a sweep to 200 Hz, so the tuning loop clamps every window inside the
  printer's range itself (§ Narrow sweeps and the range clamp).

## Verdicts, and why provisional

`include/belt_tension_types.h` (`belt_verdict` namespace). Every calculation uses only
bins inside the printer's `[resonance_tester]` sweep range: Klipper and Kalico write bins
past the sweep ceiling, and those carry no excitation.

| Verdict | Condition |
|---------|-----------|
| MATCHED ("Good match") | similarity >= 90% |
| CLOSE ("Fair match") | similarity >= 75% |
| ADJUST ("Poor match") | anything worse |

Similarity is the Pearson correlation of the two in-band PSDs (B interpolated onto A's
bins) x100, clamped to 0..100. Alongside it the comparison pairs local maxima by
frequency, Shake&Tune's `_pair_peaks`: a pair forms when two peaks sit within
min(median + 1.5 * IQR of all peak distances, 10 Hz), greedily closest-first, and a peak
with no partner is reported unpaired rather than differenced against the other path's
tallest peak: the two tallest peaks can be different modes entirely (a frame mode on one
path, a belt hump on the other).

The comparison is invalid when either curve has fewer than three in-band bins. The
constants are provisional (prestonbrown/helixscreen#1737). Six printers captured so far sort
plausibly under them (fixtures in `tests/fixtures/belt_sweeps/`, named by axis):

| Printer | Band (Hz) | Similarity | Verdict |
|---------|-----------|-----------:|---------|
| AD5X | 20-80 | 99.5% | Good match |
| K1C | 5-133.3 | 98.9% | Good match |
| AD5M | 5-100 | 97.4% | Good match |
| K2 Plus | 20-120 | 92.2% | Good match |
| U1 | 5-100 | 81.7% | Fair match (main pair 3 Hz apart) |
| Voron 2.4 (Kalico) | 5-135 | 48.3% | Poor match (uneven belts, gantry due for re-racking) |

A rerun of the same machine moves the number: the AD5M read 98.4% and the Voron 43.7% on a
second capture, so a cutoff a few points from a printer's reading can flip its verdict.

Belt modes sit where the machine puts them (40-60 Hz on the small factory printers,
110-130 Hz on the Voron), so no fixed "belt band" would work across the fleet. No capture
showed anything above 150 Hz stronger than 2% of its peak.

The panel shows no direction ("A tighter" / "B tighter"): a signed difference between two
single peaks read the Voron's 36 Hz frame mode against its 120 Hz belt hump. Direction
returns only if captures show a dependable signal (#1721).

Shake&Tune reports a higher similarity for the same printer (71% against 48% on the Voron):
it correlates 0-200 Hz, where both curves are nearly flat and so agree, while this check
correlates only the swept band.

## The RESULTS screen

- Centre: `bt_similarity` ("48%") and the verdict chip; after a re-test or Confirm,
  `bt_similarity_was` under the number carries the similarity the comparison showed before
  ("was 48%").
- Sides: two header slots, left then right, each a path name (`bt_slot_0_name` /
  `bt_slot_1_name`; tapping one opens "Which belt is which?") over its status
  (`bt_slot_0_note` / `bt_slot_1_note`). Path A is on the left unless the layout names A the
  right belt: then B takes the left slot (`bt_slots_swapped`, which swaps the name colors too, and each "Peaks" pair reads in the same order)
  so the screen matches the printer. The Re-test buttons and the compact Re-test menu follow
  the same order. One function decides it, `include/ui_belt_path_names.h#belt_path_in_slot`;
  the chart keeps its colors. The status: "sweeping" for the
  running path and "done" for one this run already measured; on RESULTS an age ("just now",
  "N min ago") that a 30 s timer keeps current. Ghost curves are drawn on their own scale;
  the two current curves share one.
- Under the chart: `bt_facts`, up to `MAX_LISTED_PEAKS` pairs strongest first as A/B
  ("Peaks 36/35 · 132/133 Hz"), and `bt_unpaired` ("Only on Path B: 120, 129 Hz"), hidden by
  `bt_has_unpaired` when empty.
- Chart: the listed pairs carry the same number on both curves; the listed unpaired peaks
  are hollow rings. Both curves (and the re-test ghosts) are trimmed to the sweep band and
  scaled by the tallest in-band point on screen, so the y axis reads 0-100%.

## Settling laps

After a tensioner turn the change stays mostly in the span beside the tensioner until the
gantry runs the belt loop through; without laps, readings sat 3-6 Hz off on the Voron and did
not come back when the turn was undone. So every run that can follow a belt change settles
first: Start, Re-test, Confirm, the calibration run and every tuning round. The one exception
is the baseline chained straight onto a registered calibration sweep, since no belt changed in
between.

`include/belt_settle.h#plan_belt_settle` (pure) plans the laps and emits one script:
`SAVE_GCODE_STATE`, `G90`, Z to the `[resonance_tester]` probe point's height, `kLaps` (3)
rectangles, back to the probe point's X/Y, `M400`, `RESTORE_GCODE_STATE`.

| Rule | Value |
|------|-------|
| Inset from `axis_minimum` / `axis_maximum` | max(`kInsetMinMm` 20 mm, `kInsetSpanFraction` 7% of the span) |
| Inset with several toolheads | `kMultiToolInsetSpanFraction`: the laps keep to the central half of each axis, because parked tools and their docks sit inside the travel. `BeltSettleInputs::tool_count` comes from `ToolState::has_multiple_toolheads()` |
| Speed | min(`kSpeedMaxMmS` 300, 0.6 x `max_velocity`); acceleration untouched |
| Timeout | max(60 s, 4 x the planned duration) |

`src/calibration/belt_tension_calibrator.cpp#settle` plans before it homes, so a refusal sends nothing, then homes
(`ensure_homed_then`) and runs the script. A refusal lands on ERROR with its reason
(`settle_refusal_message` in the panel): no axis limits yet, no `probe_points` (no safe
height), no `max_velocity` yet, or a lap side under `kMinSideMm`. A print in progress never
gets this far: the Start gate refuses it. The panel re-reads `[resonance_tester]` for its
probe point before every settle (`src/ui/calibration/ui_panel_belt_tension.cpp#start_settle`), the run title reads
"Settling belts" while they run, and every duration estimate counts them (`settle_seconds`).
On the Voron the laps take about 13 s.

## Tuning loop

The RESULTS screen offers **Tune Belts** (**Fine-tune** when the verdict is already MATCHED).
The button opens the TUNING view (`BeltTensionPanel::handle_tune_clicked`). The logic is
`BeltTuneSession` (`include/belt_tune_session.h`), which is pure (no LVGL, no I/O): the panel
feeds it round 0's curves, the calibration's full sweeps and each round's narrow sweeps, and
renders what it says.

A higher frequency means a tighter belt, and clockwise tightens. The session's gap is always
`adjusted - reference` within one round, so drift common to both paths cancels (the Voron
showed up to 5 Hz of it between runs); negative means the adjusted belt is looser.

### In-plane curves

Belt modes move the toolhead in the XY plane. `parse_resonance_csv` keeps `psd_x + psd_y`
beside `psd_xyz` (`BeltResponse::in_plane`), and tuning reads the in-plane curve
(`tuning_curve`), falling back to `psd_xyz` when a CSV has no per-axis columns. RESULTS keeps
`psd_xyz` for the similarity, the peaks and the chart. On the Voron, as A slackened its hump
became 78% Z: an `xyz` peak can be a frame mode tension barely moves.

### Stages

```
Tune Belts
  | BeltTuneSession(round 0) -> stage
  v
ALREADY_MATCHED --Good Enough--> RESULTS (no sweep)
  | Calibrate Anyway
  v
CALIBRATE  "Tighten Path B one full turn" -> Measure: settle + full sweep of A and B
  |  not registered -> CALIBRATE again ("didn't register yet"), up to kCalibrationMaxSteps
  |  gave up        -> NO_ZONE ("Turn B back 2 turns, then check the mechanics")
  v registered
BASELINE   the zone's narrow sweep twice (A, B, A, B), no laps when chained from calibration
  |  the pair disagrees -> UNREPEATABLE (no advice; Re-measure sweeps a fresh pair)
  v
TUNE       one narrow pair per Re-measure, a verdict per round -> MATCHED -> Next Zone | Confirm
```

SPRING tensioners skip calibration: the strongest candidate is the zone at once and its
baseline sweeps straight from Tune. The panel maps stages to `bt_tune_phase`
(`tune_phase_for_stage`): CALIBRATE 3, ALREADY_MATCHED 4, NO_ZONE and a matched zone 2
(MATCHED), everything else 0 (ADJUST); 1 (SWEEPING) while any run is in flight.

### Run kinds

`src/ui/calibration/ui_panel_belt_tension.cpp#begin_run` (queue, clear_previous, `RunKind`, settle_first) is the one entry for every sweep, and
the session is the one source of a window:

| `RunKind` | Sweeps | Feeds | Lands on |
|-----------|--------|-------|----------|
| `CHECK` | full, the queued paths | `runs_` | RESULTS (Start, Re-test, Confirm) |
| `CALIBRATION` | full, A then B | `tune_curves_` -> `finish_calibration` | TUNING |
| `TUNE_ROUND` | `sweep_window()`, A then B, twice for a baseline pair | `tune_curves_` -> `finish_tune_round` | TUNING |

A calibration run never writes `runs_`, so RESULTS keeps the comparison tuning started from.
Calibration and tuning runs keep the view on TUNING in phase SWEEPING, so Stop, Back, a
rebuild, the gate and the stall guard treat both alike (`measuring()`). In a baseline pair the
first two sweeps go to the session as the pair's first round mid-run, and the second pair
sweeps whatever window that round left (`src/ui/calibration/ui_panel_belt_tension.cpp#on_sweep_complete`).

### Zone candidates

`src/calibration/belt_tune_session.cpp#tune_zone_candidates` builds the zones a calibration turn tests from round 0's in-plane
comparison over the band, strongest first, at most `kMaxZoneCandidates` (4):

- every pair whose two peaks reach `kZoneMinAmplitudeFraction` (20%) of the in-band
  in-plane maximum and an in-plane share of at least `kInPlaneMinFraction` (50%,
  `in_plane_fraction`). A matched pair is still a candidate: the turn tests movement, not
  the gap;
- an unpaired tallest peak meeting both rules, steered against the other path's nearest such
  peak within `kZoneFallbackMaxHz` (30 Hz, three times the pairing ceiling)
  (`BeltZoneSource::TALLEST_UNPAIRED`). The mock's default printer needs it.

A candidate centred within `kZoneHalfMinHz` of a stronger one is dropped. Each window is the
two peaks' midpoint +- max(`kZoneHalfMinHz` 10, gap / 2 + `kZoneMarginHz` 5), clamped to
the band. The "Curves differ in shape" hint is latched from round 0
(`tallest_peak_unpaired`), whatever the candidates turn out to be.

### Already matched

At construction the strongest candidate's in-plane centroid gap is compared with
max(`tolerance_hz()`, `kCalibrationMinMoveHz`): the calibration's own measure of run-to-run
wander, so a smaller gap cannot be told from a match. Within it the stage is ALREADY_MATCHED
for every tensioner kind, and the panel offers **Good Enough** (back to RESULTS, no sweep) and
**Calibrate Anyway** (`calibrate_anyway()`, then on as an unmatched session would). The last
line reads "Within N Hz of each other at F Hz". The K1C's settled captures (gaps 0.14 and
0.23 Hz against a 1.54 Hz threshold) land here; the Voron's detuned state (1.95 Hz) does not.

### Calibration turn

The zone is chosen by what moves, not by height: on the Voron the tallest pair is the 25-45 Hz
frame mode, which tension barely moves. The user tightens the adjusted path (the looser one by
the strongest candidate's centroid gap) by `kCalibrationTurns` (one full turn) on SCREW
printers, "noticeably" on UNKNOWN ones, and **Measure** runs the laps and a full sweep of both
paths. `src/calibration/belt_tune_session.cpp#add_calibration_sweep` compares every candidate's gap with round 0's, over all
calibration turns so far, and counts only a move the advised way (the turned belt's side of
the gap rising for a tightening turn, falling for a loosening one) of at least
`kCalibrationMinMoveHz` (1.5 Hz).

**Adjust the other belt** at CALIBRATE is offered only before the first step
(`can_swap_adjusted_path`, `bt_tune_can_swap`): every step is measured against round 0 on the
same belt, so a swap after one would mix two belts' turns into one move and one turn count.
Before it, the swap makes the turn loosen the tighter belt ("Loosen A one full turn"), which
closes the gap from the other side.

- **registered**: the candidates that moved, best move first, become the zones Next Zone walks;
  the best one is entered with the calibration sweep's centroids, and on SCREW printers its move
  per calibration turn seeds Hz per turn. The panel chains the baseline with no laps
  (`start_tune_round(false)`);
- **not registered**: "That turn didn't register yet. Tighten Path B one more full turn." (UNKNOWN:
  "a little more"; after a swap, "Loosen A ...");
- **gave up** after `kCalibrationMaxSteps` (2) steps: NO_ZONE, "Nothing followed 2 turns of B.
  Turn B back 2 turns, then check the mechanics." (UNKNOWN: "Undo them"). That line is the
  whole message: the "gap is not closing" warning does not show, since nothing was tuned.

Calibration turns count toward the per-belt totals Confirm reports. The Voron data says the
first calibration turn will often not register there; that is the rule working (see the
hardware section below).

### Narrow sweeps and the range clamp

A round sweeps `sweep_window()`, `src/calibration/belt_tune_session.cpp#zone_sweep_window` of the zone: `kZoneSweepMarginHz` (10)
below the zone and `kZoneSweepTopMarginHz` (20) above it. The bottom margin keeps the start
transient (the 10-15 narrow sweeps bump at 97-104 Hz on a 95 Hz start) out of the analysed
bins; the top one keeps a narrow sweep's last 15 Hz, which in all 22 narrow Voron CSVs rose to
up to 15x the zone's own power, 6 Hz clear of the zone's top bin. The analysis reads the zone
bins only, so the margins never reach a centroid, an edge test or a power sum.

**The window never leaves the printer's `[resonance_tester]` range.** `zone_sweep_window`
returns whole-Hz bounds inside `[min_freq, max_freq]` (start rounded up, end rounded down), so
the API's own inward rounding of `FREQ_START` / `FREQ_END` (start up, end down) changes
nothing. Klipper accepts a
`FREQ_END` up to 300 Hz whatever `max_freq` says, and a K1C (max_freq 133.33) shut its MCU
down ("Move queue overflow") on a sweep to 200 Hz, so this clamp is the only guard. Where the
band cuts the top margin short, the margin is whatever remains. **Tune Belts** stays disabled
(`bt_tune_range_known`) while `sweep_cfg_` still holds Klipper's defaults
(`ResonanceTesterConfig::from_printer` false): on a K1C the default 135 would send
`FREQ_END=135`. Full sweeps send no `FREQ_*` at all.

A round takes about `2 x window / hz_per_sec` plus the laps, twice that for a baseline pair;
the Re-measure and Next Zone labels count both (`round_minutes`). Klipper writes the CSV across
the whole configured range, and the in-band bins outside the swept range read the belt
unexcited: `unexcited_floor_psd` is their median (at least `kUnexcitedMinBins`), and
`window_holds_peak` requires the zone's maximum to stand `kPeakMinProminence` (1.5x) over it
(over the zone's own minimum when there are too few such bins). A zone failing it is `NO_PEAK`.

### Baseline pair and repeatability

Every zone opens with a BASELINE: the zone swept twice on both paths with no turn between, so
every verdict compares narrow against narrow (narrow against full differed by 2.9 Hz on the
Voron). Neither round of the pair gives a verdict. The pair's second round is judged for
repeatability before anything else, including the edge rules: when the in-plane centroid gap
of the two differs by more than `kRepeatMaxGapHz` (1.5 Hz), or either path's in-plane zone
power differs by more than `kRepeatMaxPowerRatio` (2x), the stage is UNREPEATABLE. No turn
is advised then: the panel shows "Readings don't repeat", the line "These readings aren't
repeatable, so tuning can't be steered. Check the mechanics first." with the Why modal, no
arrow, amount or stepper, and offers Re-measure (a fresh pair) and Good Enough. The session's
direction field still carries the gap's sign there; the panel never shows it
(`bt_tune_unrepeatable`). The Voron's 10-15 narrow no-turn repeats land UNREPEATABLE; the
K1C's settled pair repeats.

### Centroid tracking

Each path's reading is the power-weighted centroid of its in-plane curve over the zone's bins
(`src/calibration/belt_tune_session.cpp#zone_centroid_hz`), not a refined tallest peak: the 105-140 Hz band on the Voron holds
several overlapping modes, its tallest peak hopped 2-5 Hz between identical runs, and the
centroid moved under 1 Hz. A reference whose maximum sits on the zone edge is `NO_PEAK` with
`reference_lost` (its peak left the zone, which a turn of the wrong belt does); an adjusted
path's maximum on the edge is `OUT_OF_WINDOW`, which re-centres the zone on the reference and
widens it once until a round measures again. Chart rings mark the centroids
(`reference_hz` / `adjusted_hz`).

### Outcomes

`src/calibration/belt_tune_session.cpp#add_round` classifies each TUNE round by the gap's change. The panel's line
(`src/ui/calibration/ui_panel_belt_tension.cpp#tune_last_message`) follows from it:

| `BeltTuneOutcome` | Condition | Line |
|-------------------|-----------|------|
| `MATCHED` | `|gap|` within the matched tolerance (`matched_tolerance_hz`: the mean bin spacing, at least `kMatchedMinHz`) | "Matched at N Hz" (instruction) |
| `CLOSER` | the gap shrank and kept its sign | "last turn: N Hz closer" |
| `OVERSHOT` | the gap changed sign | "Went past X by N Hz. Back it off a little." |
| `WRONG_WAY` | the gap moved against the advice by `kMinMoveHz` (0.8 Hz) or more | "The gap grew after tightening X. Check you turned X's tensioner." |
| `NO_MOVEMENT` | the gap moved under `kMinMoveHz`, or nothing was turned | "The gap didn't close (moved N Hz). Try a bigger turn.", or for a no-turn round "No turn: the gap moved N Hz" |
| `NO_PEAK` | a path's zone holds no clear peak, the reference left the zone, or the adjusted peak is still past the edge after one widening | "No clear peak in this zone", or with `reference_lost` "R's peak left the zone. Check that only X's tensioner was turned.", plus how to identify the belts |
| `OUT_OF_WINDOW` | the adjusted path's maximum sits on the zone edge | "X's peak moved out of the zone. Widening it." |
| `IMPLAUSIBLE` | the gap moved past the sanity bound | "That reading jumped too far to trust. Re-measure without turning." |
| `BASELINE` / `UNREPEATABLE` | a round of the baseline pair | none / the repeatability line |

**Sanity bound.** A gap move above max(`kImplausibleMinHz` 6, `kImplausibleRateFactor` 3 x
turns x Hz per turn), or `kImplausibleMinHz` x max(1, turns) without a rate, is a measurement
fault: the reading is dropped (gap, previous gap, gauge and rate unchanged) and the round's
turns stay pending into the next round's rate sample. `NO_PEAK`, `OUT_OF_WINDOW` and
`IMPLAUSIBLE` all carry their turns forward.

### Turn sizes and the stepper

On SCREW printers each measured round after recorded turns yields `|gap move| / turns` Hz per
turn (`hz_per_turn` is the median; the calibration seeds the first). The advice is
`|gap| / rate` in `kTurnStep` quarters, clamped to `kTurnMin` (½) .. `kTurnMax` (2), with
`kTurnMinNear` (¼) allowed once `|gap|` is under `kNearMatchFactor` (1.5) x tolerance.
Without a rate the first advice is `kFirstTurn` (1), and after a `NO_MOVEMENT` round it
doubles the turn that did nothing. The rate line shows one decimal ("~1.7 Hz / turn"); on the
Voron one full turn moved the gap 2-3 Hz once settled, so ⅛ and ¼ steps (0.3-0.7 Hz) cannot
be seen.

The instruction carries the advised amount ("Tighten Path B 1 turn"); the stepper records what was
actually turned, in quarters up to 2, and starts at "Not turned" after every round, a swap,
Next Zone and Calibrate Anyway. A round with nothing turned since the last measured round is a
no-turn round: it adopts the reading, reads `NO_MOVEMENT` unless `MATCHED`, `NO_PEAK`,
`OUT_OF_WINDOW` or `IMPLAUSIBLE`, teaches no rate, doubles nothing and is never a wrong-way
verdict. A turn on the wrong belt or the wrong way (`WRONG_WAY`) teaches nothing either.
Amounts are formatted by `format_belt_turns`, rounded to quarters.

### Power hint

When the zone's in-plane power differs by more than `kPowerHintRatio` (3x) between the paths
(`power_hint`, over the latest curves measured across the zone), TUNING shows a second warning
row under the shape warning: "Path B resonates much more than Path A at N Hz. That points at a
mechanical difference (pulley grub screws, idler bearings, belt tracking), not tension." It
opens the same Why modal. Peak height is not tension: the Voron's B path has a 120-133 Hz mode
with 3-15x A's in-plane power, which also caps Pearson similarity well below 100% however well
the tension matches. RESULTS does not show it.

### Next Zone and Confirm

On a matched zone the view offers **Keep Tuning** (`BeltTensionPanel::handle_tune_keep_clicked`, shown while `bt_tune_has_zone` is 1), which resets the turn stepper and returns to the ADJUST phase for the same zone without sweeping, beside **Next Zone** or **Confirm**.

**Next Zone** walks only the candidates the calibration registered, best move first (every
candidate for SPRING). It sweeps the next zone's baseline pair at once, with laps, since the
earlier zone's turns moved it; its label shows that duration (`peek_next_zone`). **Confirm**
runs a full two-path CHECK with the pre-tuning curves ghosted, and the facts line carries
" · Tuned in N rounds" (tuning rounds only; baseline rounds and calibration sweeps are not
counted, and neither are they in "Round N"; a zone that matched at its baseline reports no
count) plus, on SCREW printers, each path's net
turns in quarters, calibration turns included (`total_turns`).

Every sweep re-checks the Start gate (`gate_allows_sweep`): Measure, Re-measure, Next Zone,
Confirm, and Continue on the low-RAM warning, which a tuning session asks once. When a sweep
ends the gate is recomputed, since a gate subject that changed mid-sweep read the calibrator as
busy. The screensaver does not end a session: a `Suspended` deactivation keeps the view, the
run and the session, and the wake re-asks what the suspension expired (the probe point, the
klippy socket, the sweep range, hardware detection). A confirmed **Stop** acts only while a
sweep is still running.

### Tensioner kinds

`BeltTensionerKind` comes from the printer database's `belt_tensioner` field
(`PrinterDetector::get_belt_tensioner`, keyed on the detected printer name):

| Kind | Printers | Advice |
|------|----------|--------|
| `SCREW` | Voron 2.4 and Trident (`belt_tensioner: "screw"` in `assets/config/printer_database.json`) | Calibration by one full turn; then direction plus an amount in turns, the stepper, the learned Hz per turn |
| `SPRING` | none in the database yet | No calibration: the strongest candidate is the zone. Direction only: "Release X's tensioner, let the spring settle, lock it". Two rounds without improvement (`kSpringStallRounds`) show the mechanical hint |
| `UNKNOWN` | everything else | Calibration in words ("Tighten X noticeably"); then direction only, "Tighten X" / "Loosen X" |

SCREW printers show the mechanical hint after `kStallRounds` (8) unmatched rounds in a zone.
Add `"belt_tensioner": "screw"` to a database entry only once the printer is known to have a
screw tensioner with a repeatable relation between turns and frequency.

### Gap gauge

`<belt_gap_gauge>` (`ui_belt_gap_gauge_set`) is a horizontal track for the signed gap. It draws
a matched band of +- tolerance around the centre, a filled needle at this round's gap, a
hollow needle at the previous round's and an arrow between them, and no text. The track's
half-span is `tune_gauge_range_hz_`, set at the zone's first measurement
(`reset_tune_gauge_range`) to the larger of `2 * kGaugeWarnHz` and 1.25 x the first gap, so
the needle stays on the track while it closes. `belt_gap_gauge_x` maps a gap to a pixel
(centred at zero, clamped to the ends); `belt_gap_severity` classifies it as `MATCHED`,
`NEAR` (up to `kGaugeWarnHz`, 3 Hz) or `FAR`, and the panel maps that to colour through
`bt_tune_severity`.

### Compact layouts

Below the medium breakpoint, or in portrait (`ui_breakpoint le 2 or ui_is_portrait eq 1` in
`ui_xml/panel_belt_tension.xml`), the TUNING view and RESULTS screen shrink to fit: the hero
becomes one line (the instruction, the amount and a thin `bt_tune_line_gauge`; tapping it
adjusts the other path), the two Re-test buttons on RESULTS fold into one menu, and Confirm
drops its duration when the button row holds three buttons or the layout is compact.
Re-measure and Next Zone drop theirs on compact layouts. The wide layout and the compact one are the same subjects
bound to different widgets, so `refresh_tune` feeds both.

Portrait adds two rules of its own, because there width is scarce and height spare. The
button rows wrap: the turn stepper, Tune Belts and Confirm each take a line, so the other
buttons keep room for their labels in every locale. The charts grow into the spare height
only up to `belt_chart_max_h` (about the content width of each tier), so a tall screen
leaves space under the controls rather than drawing a chart taller than it is wide. START
stacks the sketch over the text instead of setting them side by side.
Portrait above micro has the height for the full tuning hero, so it gets it instead of the
one-line hero: the instruction and stepper beside the gap and gauge, the round, zone and
rate on one line under them, and the swap back as a button. Micro portrait (272x480) keeps
the one-line hero, where the full one would leave a 72 px chart. In portrait the shape
warning wraps instead of ending in "...".

### Ultrawide layout

On ultrawide screens (`ui_is_ultrawide eq 1`, wider than 2.5:1) the compact conditions give
way: those screens read a small or medium tier by their short axis, but the problem there is
width, not room. `overlay_content` becomes a row. While a state shows a chart (RUNNING,
RESULTS, TUNING) the chart sits in `chart_host_wide` on the left and grows, and `bt_main`,
holding everything else, becomes a sidebar `belt_sidebar_w` wide (640 small, 720 medium, 800
large, 960 xlarge, 1280 xxlarge, measured so the button labels fit in en and ru). The sidebar
carries the full tuning hero and the wide button variants, the RESULTS hero row and all four
RESULTS buttons, and the run status with Stop. START keeps the full width: the sketch grows
on the left and the text and Start take the sidebar width. The chart is a C++ widget, so
`BeltTensionPanel::chart_host_for` picks the wide host and an observer on `ui_is_ultrawide`
moves it when a rotation crosses the line. On the EMBEDDED tier, which has no chart, the
panel keeps the full width.

### Mock script

`HELIX_MOCK_BELT_TUNE_SCRIPT="B:98.3,104,104,104,106.5"` scripts one path's peak so the loop
can be driven without a printer. A path's first sweep reads its seeded peak; every later
sweep of the scripted path, full or windowed, plays the next entry, the last one holding, so
the calibration's full sweeps and the narrow rounds are all scripted. A script on either path
turns the re-measure walk off for both; without one the walk runs on full re-measures, so a
plain `--test` run registers its calibration (A 110 to 106, B 98 to 102). An item `flat`
writes a curve with no peak to reach `NO_PEAK`. The mock's curves are broad (10 Hz half
width), which pulls zone centroids toward the zone's middle to roughly a third of the peak's
move; the panel tests replay narrow peaks (`HELIX_MOCK_BELT_CSV_A`/`_B`, rewritten between
sweeps) where a verdict needs exact numbers. Full reference: `MOCK_ENVIRONMENT_VARIABLES.md`.

### Thresholds (provisional)

All in `belt_tune` (`include/belt_tune_session.h`). They come from the Voron 2.4 and K1C
sessions below and remain provisional until more printers have run the loop
(prestonbrown/helixscreen#1736).

| Constant | Value | Role |
|----------|-------|------|
| `kZoneMinAmplitudeFraction` | 0.20 | peaks under this fraction of the stronger path's in-band in-plane maximum never define a zone |
| `kInPlaneMinFraction` | 0.5 | a peak defines a zone only where psd_x + psd_y is at least this share of psd_xyz |
| `kZoneHalfMinHz` / `kZoneMarginHz` | 10 / 5 Hz | narrowest zone half-width / reach beyond the zone's peaks |
| `kZoneFallbackMaxHz` | 30 Hz | how close the other path's peak must be to steer an unpaired tallest peak |
| `kMaxZoneCandidates` | 4 | zones a calibration turn tests |
| `kZoneSweepMarginHz` / `kZoneSweepTopMarginHz` | 10 / 20 Hz | narrow sweep reach below / above the zone, inside the printer's range |
| `kCalibrationTurns` / `kCalibrationMaxSteps` | 1 turn / 2 | one calibration step / steps before NO_ZONE |
| `kCalibrationMinMoveHz` | 1.5 Hz | a calibration move under this is run-to-run wander; also the already-matched threshold's floor |
| `kMatchedMinHz` | 1 Hz | matched tolerance floor (the tolerance is at least the mean bin spacing) |
| `kMinMoveHz` | 0.8 Hz | smaller gap moves are noise |
| `kFirstTurn` / `kTurnStep` / `kTurnMin` / `kTurnMinNear` / `kTurnMax` | 1 / ¼ / ½ / ¼ / 2 turns | SCREW amounts |
| `kNearMatchFactor` | 1.5 | below this multiple of the tolerance, `kTurnMinNear` is allowed |
| `kImplausibleMinHz` / `kImplausibleRateFactor` | 6 Hz / 3 | the sanity bound |
| `kPowerHintRatio` | 3 | zone power ratio that shows the power hint |
| `kRepeatMaxGapHz` / `kRepeatMaxPowerRatio` | 1.5 Hz / 2 | a baseline pair disagreeing by more is UNREPEATABLE |
| `kStallRounds` / `kSpringStallRounds` | 8 / 2 | unmatched rounds before the mechanical hint (screw / spring) |
| `kPeakMinProminence` | 1.5x | a zone's maximum must stand this far over the unexcited floor |
| `kUnexcitedMinBins` | 8 | in-band bins outside the swept range needed to measure the floor |
| `kGaugeWarnHz` | 3 Hz | gap gauge: warning colour up to here |

### What the hardware data says

Read this before tuning any threshold. The captures are fixtures in
`tests/fixtures/belt_sweeps/`, and `tests/unit/test_belt_tune_session.cpp` pins each finding.

| Voron 2.4 (Kalico, max_freq 135) | Finding | Fixture |
|----------------------------------|---------|---------|
| Round 0, settled | two candidates: 118.6-135 Hz (centroids A 129.12 / B 127.17, gap 1.95: not matched, "Tighten Path B one full turn") and the 27.8-47.8 Hz frame mode | `voron24_settled_*` |
| Calibration, one turn | the 118.6-135 gap moved 1.17 Hz the wrong way: not registered | `voron24_turn1_*` |
| Calibration, two turns | moved +3.43 Hz the advised way: registered, 1.715 Hz per turn | `voron24_turn2_*` |
| No-turn repeat as a calibration | moved 1.96 Hz the wrong way: not registered | `voron24_settled_repeat_*` |
| Zone power | B over A 3.65x (round 0) and 15.3x (two turns): the power hint fires for B | `voron24_settled_*`, `voron24_turn2_*` |
| Narrow no-turn repeats (10-15) | the gap swung 4-8 Hz and B's zone power 2-5x: UNREPEATABLE | `voron24_narrow_tuned_*`, `voron24_narrow_nolaps_*` |
| Narrow no-turn repeats (10-14) | gap +0.05 then -1.22 Hz with no turn | `voron24_narrow_r1_*`, `voron24_narrow_r2_*` |

| K1C (Creality, max_freq 133.33, rigid toolhead) | Finding | Fixture |
|--------------------------------------------------|---------|---------|
| Settled, two runs | one candidate (48.4-68.4 Hz), gaps 0.14 and 0.23 Hz against a 1.54 Hz threshold: ALREADY_MATCHED; power B/A 1.11, no hint | `k1c_settled_*`, `k1c_settled_repeat_*` |
| B shifted -4 Hz, then a calibration sweep shifted -1.5 Hz | +2.20 Hz: registered on the first step | `k1c_settled_*` (shifted in the test) |
| The settled pair as a baseline | repeats | `k1c_settled_*`, `k1c_settled_repeat_*` |

One turn did not register on the Voron, and a no-turn repeat moved the same zone's gap almost
2 Hz, so on that printer the first calibration turn will often report "didn't register". That
is the rule working; do not lower `kCalibrationMinMoveHz` to make it register. After the
Voron's mechanical fix, a full turn of A moved nothing in 120-133 Hz, and NO_ZONE after two
steps is the honest answer there. A rigid-toolhead K1C gives one clean, repeatable peak per
belt.

## Mock knobs

The mock simulates the whole console sequence: "Testing frequency" lines, the terminal
written-file line, and a CSV with a Lorentzian peak over a noise floor at the requested
frequency (`dispatch_test_resonances_response` in `src/api/moonraker_client_mock.cpp`,
`write_mock_belt_csv` for the file). Full reference: `MOCK_ENVIRONMENT_VARIABLES.md`,
one `###` entry per variable.

| Variable | Effect |
|----------|--------|
| `HELIX_MOCK_BELT_A_HZ` / `HELIX_MOCK_BELT_B_HZ` | The two paths' simulated peaks. Defaults 110/98: 12 Hz apart, deliberately in "Poor match" territory. With no tune script, a full re-measure walks a path's peak toward the other's by up to 4 Hz per run, so the re-test loop converges the way cranking a tensioner does |
| `HELIX_MOCK_BELT_TUNE_SCRIPT` | `<A\|B>:<hz\|flat>,...`: that path's peak for each sweep after its first (§ Mock script) |
| `HELIX_MOCK_BELT_FAIL` | `stall` (no progress lines), `nofile` (CSV missing), `multichip` (per-chip columns), `error` (adxl error line), `kalico` (dialect the parser rejects) |
| `HELIX_MOCK_KINEMATICS` | Overrides the kinematics the mock reports; `cartesian` on the default Voron persona exercises the NOT_COREXY gate leg |
| `HELIX_MOCK_BELT_CSV_A` / `HELIX_MOCK_BELT_CSV_B` | Replay a real capture as that path's result instead of the synthetic curve (e.g. the fixtures in `tests/fixtures/belt_sweeps/`) |
| `HELIX_MOCK_BELT_RANGE` | `<min>-<max>`: the `[resonance_tester]` sweep range the mock reports and sweeps, to match a replayed capture's band |

## Tests

| File | Covers |
|------|--------|
| `tests/unit/test_belt_tension_calibrator.cpp` | The calibrator: homing order, sweep lifecycle, cancel-mid-run generation guard, emergency abort |
| `tests/unit/test_belt_tension_panel_states.cpp` | Panel view states, gate-to-button binding, similarity/facts/markers, ghost series on re-test, low-RAM warning, the tuning loop's run kinds and phases end to end, settling laps |
| `tests/unit/test_belt_settle.cpp` | `plan_belt_settle`: insets, the multi-tool central half, speed, refusals, timeout |
| `tests/unit/test_belt_compare.cpp` | `compare_belt_paths` / `pair_belt_peaks` / `band_similarity` against six printers' real captures (`tests/fixtures/belt_sweeps/`) and synthetic curves |
| `tests/unit/test_belt_gating.cpp` | Every gate leg of `evaluate_belt_gate` |
| `tests/unit/test_belt_tune_session.cpp` | `BeltTuneSession`: candidates, already matched, calibration, the range clamp, repeatability, centroids, every outcome, the sanity bound, turn sizes, the power hint, tensioner kinds; the Voron and K1C hardware captures |
| `tests/unit/test_belt_gap_gauge.cpp` | `belt_gap_gauge_x`, `belt_gap_severity` and the widget's set/get |
| `tests/unit/test_mock_test_resonances.cpp` | The mock's sweep simulation, including each `HELIX_MOCK_BELT_FAIL` mode |
| `tests/unit/test_live_sweep_preview.cpp`, `test_live_sweep_target.cpp`, `test_live_sweep_header_weight.cpp` | `LiveSweepPreview` states and threading, the `live_sweep_target` gate and `layout_preview`, the ESP32 header-weight rule |
| `tests/unit/test_sweep_response_estimator.cpp`, `test_sweep_synth.cpp`, `test_mock_accel_batch_source.cpp` | The estimator and drive lock, the synthetic sweep, the mock stream |
| `tests/unit/test_frequency_response_chart_style.cpp` (`[belt]` cases) | Series styling, muted ghost, sweep cursor, markers |

Run them with `make t F='[belt]'` (the `[belt]` tag also collects the pluck-tuner units
kept from `BELT_TUNER.md`; the panel and calibrator suites additionally carry
`[belt_tension]`).
