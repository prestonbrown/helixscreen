# Calibration Subsystem: Catalog, Calibration Screen and Guided Run

HelixScreen knows which calibrations a printer has, how much each matters on that printer, what
each last measured, and how to run several of them in one sitting with a single save at the end.
Four layers carry that:

- **The catalog** decides which calibration steps the connected printer has, in what order, at
  what tier, and what each needs from the owner. It is pure data in, data out.
- **The Calibration screen** (Advanced > Calibration) shows one tile per step the catalog found,
  with the last result from the history, or the value the Klipper config already holds.
- **The guided run** takes a few of those steps one after another, stages every result in
  Klipper without saving, and ends with one Save all or one Discard all.
- **The save contract** (`SaveConfigWatch`) sends that one `SAVE_CONFIG` and reports its real
  outcome, including on Kalico, where a save can skip the Klipper restart.

The individual calibration screens (Z offset, heaters, bed mesh, input shaper, belt tension and
the rest) predate this subsystem and keep their own docs. The guided run wraps them rather than
reimplementing them: [INPUT_SHAPER.md](INPUT_SHAPER.md), [BELT_TENSION.md](BELT_TENSION.md),
[PRESSURE_ADVANCE_TUNING.md](PRESSURE_ADVANCE_TUNING.md),
[MOTOR_RIPPLE_CHECK.md](MOTOR_RIPPLE_CHECK.md) and [MATERIAL_Z_OFFSET.md](MATERIAL_Z_OFFSET.md)
cover those. The end-user view is [docs/user/guide/calibration.md](../user/guide/calibration.md).

---

## Key Files

| File | Purpose |
|------|---------|
| `include/calibration_catalog.h`, `src/printer/calibration_catalog.cpp` | The step catalog: `StepKind`, `Tier`, `Prereq`, `StepInfo`, `CatalogInputs`, `applicable_steps()`. No LVGL |
| `include/calibration_catalog_inputs.h`, `src/printer/calibration_catalog_inputs.cpp` | `catalog_inputs_from()`: fills `CatalogInputs` from the live `PrinterState` and the printer database |
| `include/calibration_step.h` | The step interface the run drives: `CalibrationStep`, `StepContext`, `StepOutcome`, `StepProgress`, `StepResult` |
| `include/calibration_run.h`, `src/calibration/calibration_run.cpp` | The run engine: `guided_run_steps()`, `picked_by_default()`, `prereq_lines()`, `CalibrationRun`, `RunFinisher`. No LVGL |
| `include/calibration_run_steps.h`, `src/ui/calibration/calibration_run_steps.cpp` | The four real steps (Z offset, heaters, bed mesh, input shaper) and `MoonrakerRunFinisher` |
| `include/calibration_history_store.h`, `src/calibration/calibration_history_store.cpp` | `CalibrationHistory`: each step's last result in the Moonraker database |
| `include/calibration_config_results.h`, `src/calibration/calibration_config_results.cpp` | The In-config readers, and the one spelling of every measured value (`format_z_offset()`, `join_shaper_axes()`, ...) |
| `include/ui_calibration_hub_overlay.h`, `src/ui/calibration/ui_calibration_hub_overlay.cpp`, `ui_xml/calibration_hub_overlay.xml` | The Calibration screen: tiles and the Guided calibration banner |
| `include/ui_calibration_run_overlay.h`, `src/ui/calibration/ui_calibration_run_overlay.cpp`, `ui_xml/calibration_run_overlay.xml` | The guided run's four pages: Pick, Before you start, Running, Results |
| `include/ui_wizard_calibration.h`, `src/ui/wizard/ui_wizard_calibration.cpp`, `ui_xml/wizard_calibration.xml` | Setup's last page: Calibrate now or Skip for now |
| `include/save_config_restart.h`, `src/ui/calibration/save_config_restart.cpp` | `SaveConfigWatch`: one `SAVE_CONFIG` and its real outcome; `query_restarts()` |
| `include/save_config_live.h`, `src/printer/save_config_live.cpp` | `save_config::restart_required()`: whether a Kalico save may skip the restart |
| `include/pid_calibrate_tolerance.h`, `src/api/pid_calibrate_tolerance.cpp` | Kalico's PID `TOLERANCE`: 0.05 for a bed, 0.02 for everything else |
| `include/hardware_object_table.h`, `src/printer/hardware_object_table.cpp` | `hw::is_fallback_probe_section()`: a K2's `[prtouch]` is the Z probe only when no other probe is configured |
| `assets/config/printer_database.json` | Per-printer tiers under `calibration.steps` |

Tests: `tests/unit/test_calibration_catalog.cpp` (`[calibration_catalog]`),
`tests/unit/test_calibration_run.cpp` and `tests/unit/test_calibration_run_finish.cpp`
(`[calibration_run]`), `tests/unit/test_calibration_history.cpp` (`[calibration_history]`,
`[calibration_hub]`), `tests/unit/test_save_config_watch.cpp` (`[save_config]`),
`tests/unit/test_save_config_policy.cpp` (`[save_config_policy]`),
`tests/unit/test_pid_calibrate_collector.cpp` (`[pid_calibrate]`) and
`tests/unit/test_z_offset_calibration_send.cpp`.

---

## The Step Catalog

### What a step is

The catalog knows thirteen steps (`kStepCount`), each a row of `kSteps` in
`src/printer/calibration_catalog.cpp#helix::calibration`. A row holds the facts a step has on a
printer with no database tier for it: its stable id (also its history key), its untranslated name,
its default tier, its time in minutes, whether it heats, is loud, moves the machine or needs the
owner's hands, and its prerequisites. **Row order is the run order**; `StepInfo::order` records
it.

```cpp
constexpr std::array<StepRow, kStepCount> kSteps{{
    {StepKind::BeltTension, "belt_tension", TR_NOOP("Belt tension"), Tier::Recommended, 3,
     false, true, true, false, kPrereqBedClear},
    {StepKind::CutterPosition, "cutter", TR_NOOP("Cutter position"), Tier::Recommended, 2,
     false, false, true, false, kPrereqBedClear},
    {StepKind::ZOffset, "z_offset", TR_NOOP("Z offset"), Tier::Recommended, 5,
     false, false, true, false, kPrereqBedClear | kPrereqBuildPlate},
    {StepKind::Heaters, "heaters", TR_NOOP("Heaters"), Tier::Recommended, 10,
     true, false, false, false, kPrereqBuildPlate},
    // ... bed_mesh, input_shaper, pressure_advance, belt_compare, bed_screws,
    // tool_offsets, vertical_artifact, pa_tuning, vibration_check
}};
```

| Id | Name | Default tier | Shown when |
|----|------|--------------|------------|
| `belt_tension` | Belt tension | Recommended | `CanTensionBelts` (motorized tensioning, K2 family) |
| `cutter` | Cutter position | Recommended | `CanCalibrateCutter` |
| `z_offset` | Z offset | Recommended | a probe, the printer does not re-apply a Z offset per head, and the tool offset calibration does not already set the reference tool's Z |
| `heaters` | Heaters | Recommended | always |
| `bed_mesh` | Bed mesh | Recommended | `HasBedMesh` |
| `input_shaper` | Input shaper | Recommended | `HasAccelerometer` |
| `pressure_advance` | Pressure advance | Optional | `HasPaCal` (a printer that measures pressure advance itself) |
| `belt_compare` | Belt compare | Optional | an accelerometer and `SupportsBeltCompare` |
| `bed_screws` | Bed screws | Optional | `HasScrewsTilt` |
| `tool_offsets` | Tool offsets | Recommended | `HasToolOffsetCal` |
| `vertical_artifact` | Vertical artifact compensation | OnRequest | `HasStepperResonanceCal` |
| `pa_tuning` | Pressure advance tuning | OnRequest | always |
| `vibration_check` | Vertical artifact check | OnRequest | `SupportsRippleCheck` |

The "Shown when" column is `applies()` (`src/printer/calibration_catalog.cpp#applies`), which
mirrors the capability conditions the Advanced panel's old calibration rows used.

### applicable_steps()

`applicable_steps(const CatalogInputs&)` walks `kSteps`, keeps the rows `applies()` accepts, and
adjusts each kept row for this printer:

- **The Z offset step becomes hands-on** with a paper prerequisite when it is a paper test: the
  strategy is not `FIRMWARE_MANAGED` and the Z probe does not measure with the nozzle itself
  (`z_offset_is_paper_test()`).
- **A database tier replaces the default tier** for any step the printer's entry names.

`CatalogInputs` is plain data so the catalog is testable without a printer.
`catalog_inputs_from()` (`src/printer/calibration_catalog_inputs.cpp#catalog_inputs_from`) fills
it from the capability subjects, the profile state (Z offset strategy, per-head offsets), the
enabled Z probe's type (`probe_contacts_nozzle()`), and the database tiers.
`current_printer_steps(state)` is `applicable_steps(catalog_inputs_from(state))`, which is what
the Calibration screen, the guided run and the setup page all call.

### Tiers

A tier says how much a step matters on this printer. `Tier` is ordered by significance:
`Required`, `Recommended`, `Optional`, `OnRequest`.

| Tier | Calibration screen tile with no result | Guided run |
|------|----------------------------------------|------------|
| Required | "Never run · required", due color | offered, ticked and locked on |
| Recommended | "Never run", due color | offered, ticked |
| Optional | "Never run · optional" | offered, unticked |
| OnRequest | "Run when needed" | never offered |

A printer's database entry overrides tiers in a `calibration.steps` object of step id to tier
spelling (`required`, `recommended`, `optional`, `on_request`). Today one entry carries one,
Jacobean's K2 Plus:

```json
"calibration": {
  "steps": {
    "cutter": "required",
    "heaters": "required",
    "bed_mesh": "required",
    "z_offset": "recommended",
    "belt_tension": "recommended",
    "input_shaper": "recommended",
    "pressure_advance": "optional"
  }
}
```

`PrinterDetector::get_calibration_step_tiers()`
(`src/printer/printer_detector.cpp#get_calibration_step_tiers`) returns the raw pairs, and
`tier_overrides_from()` drops any entry whose step id or tier the catalog does not know, so a typo
in the database is ignored rather than inventing a step. A tier never makes a step appear: a step
the printer lacks stays hidden whatever its tier says.

---

## The Calibration Screen

`CalibrationHubOverlay` (`src/ui/calibration/ui_calibration_hub_overlay.cpp`) is opened by the
Advanced panel's Calibration row. `ui_xml/calibration_hub_overlay.xml` declares one
`calibration_hub_tile` per catalog step, in catalog order, each with the callback that opens that
step's own screen (the same callback the Advanced panel's row for it used). The overlay publishes
three subjects per step id, `cal_shown_<id>`, `cal_tone_<id>` and `cal_status_<id>`, and the XML
binds to them; it never builds tiles in C++.

`refresh()` runs on every show, on every return from a step's screen (`on_activate()`), and
whenever the history's version subject changes, since the history loads asynchronously and a
guided run records into it while the screen may be open. For each step it asks `tile_status()`:

```cpp
TileStatus tile_status(const calibration::StepInfo& step, const calibration::HistoryEntry* last,
                       const std::string* in_config) {
    if (last) {
        // "Done Nov 21 · 0.125 mm", the done color
    }
    if (in_config) {
        // Both axes' shapers fill a half-width tile at 800x480 on their own;
        // they only ever come from the config, so they need no label.
        if (step.kind == calibration::StepKind::InputShaper) {
            return {*in_config, TileTone::Idle};
        }
        return {fmt::format(lv_tr("In config · {}"), *in_config), TileTone::Idle};
    }
    // otherwise the tier's line
}
```

The precedence is the history's last result, then the value the Klipper config holds, then the
tier. `TileTone` values (`Idle`, `Due`, `Done`) are the ones `calibration_hub_tile.xml` maps to
colors.

**The Guided calibration banner** sits above the tiles. It shows when the run would have at least
one step ticked by default, and says how many of those never ran and how long the run takes
(`guided_banner_text()`): "2 steps never run · about 20 min", or "Run everything again · about
25 min" once all have a result. The ESP32 firmware leaves the guided run out of its build, so the
banner never shows there.

### In config: results the printer already holds

A printer tuned before HelixScreen kept any history still has its tuning in `printer.cfg`.
`results_in_config()` (`src/calibration/calibration_config_results.cpp#results_in_config`) reads
`configfile.settings` once at discovery and returns a short summary per step; `PrinterDiscovery`
keeps it as `calibration_in_config()`. A value that is unset, or at the value a stock config
starts from, is absent, so the tile falls back to its tier line.

| Step | Read from |
|------|-----------|
| `z_offset` | A Beacon proximity model's `model_offset`, else a Cartographer touch model's then scan model's `z_offset`, else the first probe-type section with a nonzero `z_offset`. Zero counts as untuned |
| `heaters` | The `[extruder]` `control` when it is `pid`, `pid_v` or `mpc` ("PID", "MPC"). A vendor's shipped values count |
| `bed_mesh` | A saved `[bed_mesh <name>]` profile, `default` preferred ("profile default") |
| `input_shaper` | `shaper_type_<axis>` and `shaper_freq_<axis>` for each axis with a frequency above 0 ("X: MZV 36.7 · Y: EI 47.6") |

**The fallback probe rule.** Jacobean's K2 firmware keeps a Creality `[prtouch]` section beside a
Cartographer. Klipper allows one Z probe, so when another probe is configured that one is the Z
probe and the `[prtouch]` offset is not the printer's Z offset. `hw::is_fallback_probe_section()`
(`src/printer/hardware_object_table.cpp#is_fallback_probe_section`) names those sections, and the
Z offset reader skips them whenever any other probe section exists:

```cpp
bool other_probe = false;
for (const auto& [name, body] : settings.items()) {
    other_probe = other_probe || (is_probe(name) && !hw::is_fallback_probe_section(name));
}
for (const auto& [name, body] : settings.items()) {
    // ...
    if (!body.is_object() || !is_probe(name) || modeled ||
        (other_probe && hw::is_fallback_probe_section(name))) {
        continue;
    }
    // first nonzero z_offset wins
}
```

On a K2 with only `[prtouch]`, its `z_offset` shows as In config.

**One spelling per value.** The same file holds the formatters the guided run's results use
(`format_z_offset()`, `format_shaper_axis()`, `join_shaper_axes()`, `format_pid()`,
`format_mpc()`, `format_mesh_range()`), so a tile reads the same before and after a run. It also
holds `staged_z_offset()`, which reads the value a Z offset calibration left in
`configfile.save_config_pending_items`.

---

## The Calibration History

`CalibrationHistory` (`include/calibration_history_store.h`) keeps each step's last result in the
printer's Moonraker database, namespace `helix-screen`, key `calibration_history`:

```json
{
  "version": 1,
  "steps": {
    "z_offset": {"date": "2026-11-21", "summary": "-1.245 mm", "values": {"z_offset": -1.245}},
    "heaters": {"date": "2026-11-21", "summary": "Nozzle 22.1 / 1.08 / 113 · Bed ...",
                "values": {"extruder": {"control": "pid", "temp": 210, "kp": 22.1, "...": 0}}}
  }
}
```

- `summary` is ready to show; `values` keeps the numbers for later steps to estimate from.
- **The guided run is the only writer.** `MoonrakerRunFinisher::record()` records each finished
  step once its Save all is known to have happened. A calibration run from its own screen does
  not record; its tile keeps showing the In-config value instead.
- **It belongs to one printer.** Session wiring clears and reloads it after every discovery
  (`src/application/session_wiring.cpp`), so a printer switch or a reconnect reads the new
  printer's history.
- **A history that will not load is never overwritten.** A result recorded before a load answers
  waits in `pending_` and is merged into the loaded history, then written. A load that fails for
  any reason but "not found", or returns a document of another schema version, leaves both the
  stored value and the pending results alone until the next load.
- `version_subject()` bumps on every change, which is what the Calibration screen observes.

---

## The Guided Run

### Which steps a run offers

`guided_run_steps()` (`src/calibration/calibration_run.cpp#guided_run_steps`) filters the
applicable steps to the four a run can drive, Z offset, Heaters, Bed mesh and Input shaper,
drops any whose tier is `OnRequest`, and sorts them hands-on first:

```cpp
/// Hands-on first, then the order the catalog already ranks them in.
int run_rank(StepKind kind) {
    switch (kind) {
    case StepKind::ZOffset:
        return 0;
    case StepKind::Heaters:
        return 1;
    case StepKind::BedMesh:
        return 2;
    default:
        return 3;
    }
}
```

The Z offset goes first because it is the step that may need the owner at the printer; after it
the owner can walk away.
`picked_by_default()` ticks Required and Recommended steps; the run overlay locks Required ones on.

### The four pages

`CalibrationRunOverlay` (`src/ui/calibration/ui_calibration_run_overlay.cpp`) owns the run, not
the view: closing the screen mid-run leaves the run going, and `open()` reopens at the page the
run is on. Every page binds to subjects the overlay publishes; the subject names are in
`init_subjects()`.

1. **Pick.** One row per offered step with its time, whether it needs the owner, a paper test,
   heat or noise, and its tile line. The summary reads "3 steps · about 20 min" and says how much
   of that is hands-on.
2. **Before you start.** `prereq_lines()` lists every prerequisite of the picked steps once, in
   `kPrereqOrder`, with the steps that need it ("For: Z offset, Bed mesh"). The owner ticks each
   one; an accelerometer is pre-ticked, since the run only offers input shaping where discovery
   found one. Start is enabled once nothing is left to check. Coming back to this page clears the
   ticks.
3. **Running.** The current step's name, its phase line and a progress bar (or an indeterminate
   one while the step reports `percent = -1`), a safety line per step ("Do not touch the nozzle or
   the bed."), live temperatures during the heater step, and "Step 2 of 4 · about 15 min left".
4. **Results.** Per step, the value before the run (its history summary, else its In-config
   value, captured when the run started) beside what the run measured; then Save all and Discard
   all.

**Setup's last page.** The setup wizard ends on a Calibration page (`WizardCalibrationStep`)
that lists the steps a run would tick and how long they take. **Calibrate now** finishes setup
first (`complete_then()`) and then opens the run on Home; the footer's Finish reads **Skip for
now** and only finishes. The page is skipped when there is no printer connection. While Calibrate
now's run is open the first-run tour waits; the run releases it in `on_dismissed()`.

### CalibrationRun, StepContext and the step contract

`CalibrationRun` (`include/calibration_run.h`) is LVGL-free and runs a vector of
`CalibrationStep`s in order. It gives each step a `StepContext` that lives as long as the run:

```cpp
struct StepContext {
    IMoonrakerAPI& api;
    std::function<void(const StepProgress&)> progress;
    std::function<void(StepOutcome)> done;
    /// Stop the whole run, as its Stop button does: this step's abort() is
    /// called and later steps do not run.
    std::function<void()> stop_run;
};
```

A step's contract (`include/calibration_step.h`):

- `start()` begins the step on the main thread and **never sends `SAVE_CONFIG`**: its result
  stays pending in Klipper until the run saves everything at once.
- `done()` is called exactly once per `start()`, with one of `Finished`, `Failed`, `Skipped` or
  `Aborted`. A step may call it from inside its own `start()`; `CalibrationRun::advance()` loops
  instead of nesting.
- `StepOutcome::failed_after(why, partial)` reports a failure after part of the result was
  staged (one heater tuned before the next failed, X shaped before Y failed). The results line
  then reads "Failed: ... Saves ...", and Save all writes that part with the rest.
- `abort()` stops the step at any point. `abort_restarted_firmware()` says whether that took an
  emergency stop and restart, which drops everything the run had staged.
- `restores_on_discard()` and `restore()` are for a result the firmware persisted itself, outside
  Klipper's pending config (below).

A failed or skipped step does not stop the run; the next one starts. The run's phases are
`Ready`, `Running`, `Results`, `Saving`, `Saved`, `SaveFailed`, `Discarding` and `Discarded`, and
every status or progress change calls the overlay's `on_change`, which republishes.

`klippy_restarted()` handles a Klipper shutdown or restart the run did not ask for: anything
staged is gone, so `results_lost()` turns true, each finished row reads "Dropped when the printer
restarted", and `can_save()` is false. The run's own save and discard restarts are excluded.

### The four steps

`make_run_step()` (`src/ui/calibration/calibration_run_steps.cpp#make_run_step`) returns the step
for a `StepInfo`, or nullptr for a step a run does not drive. Each wraps the code its own screen
already runs:

| Step | What it runs | Its result |
|------|--------------|------------|
| `ZOffsetStep` | The Z offset panel opened in run mode (`ZOffsetCalibrationPanel::open_for_run()`) | The value `ACCEPT` staged, read back from the pending items (`staged_z_offset()`); "Accepted at Z ..." when the firmware reports none |
| `HeatersStep` | `get_heater_control_type()` per heater, then `MPC_CALIBRATE` where the heater already runs MPC, else `PID_CALIBRATE`: the active extruder at 210 °C, then the bed at 60 °C when there is one (`kRunNozzleTempC`, `kRunBedTempC`) | Each heater's gains or model, joined |
| `BedMeshStep` | The bed mesh panel's own calibration into the default profile, without its questions, toasts or save prompt (`BedMeshPanel::calibrate_for_run()`) | The stored mesh's range, or its profile name |
| `InputShaperStep` | `InputShaperCalibrator::run_both_axes()`: the accelerometer noise check, then X, then Y | Both axes' shaper and frequency |

### Z offset run mode

In run mode the Z offset panel behaves differently in four ways:

- **No intro and no bed-clear question.** The run's Before you start page already had the bed
  cleared and the paper ready, so the panel homes and probes on the activation its
  `open_for_run()` queued and opens on the tape screen. Retry starts over the same way. The
  panel's Warm Bed switch does not apply: the Pick page's **Warm the bed first** chip on the Z
  offset card does (`RunOptions::zoffset_warm_bed`, offered where there is a heated bed), and
  the panel then warms the bed through the same path its switch uses before probing.
  `with_run_options()` adds `kZOffsetWarmBedMinutes` to the step's minutes, so the card, the
  Pick totals, the hands-on minutes and the Running estimate all include the warm-up.
- **Accept only stages.** It sends `ACCEPT` and no `SAVE_CONFIG`; the completion state says the
  value is saved with the other results, and its Done button becomes **Continue**, which returns
  to the run. A firmware-managed printer applies the offset at once instead
  (`SET_GCODE_OFFSET` and the firmware's own save), and the step reports it as finished with
  nothing staged.
- **Leaving without Accept asks.** Closing the panel without an accepted result calls the run's
  callback with `accepted = false`. The steps after this one heat and move the printer, so going
  on is the owner's choice:

```cpp
/// The screen closed without Accept. The steps after this one heat and
/// move the printer, so going on to them is the owner's choice; anything
/// but Continue stops the run.
void ask_continue() {
    helix::ui::ConfirmOptions options;
    options.cancel_text = lv_tr("Stop calibration");
    options.on_cancel = [this, alive = std::weak_ptr<int>(alive_)]() {
        if (!alive.expired() && ctx_) {
            ctx_->stop_run();
        }
    };
    options.on_dismiss = options.on_cancel;
    helix::ui::modal_confirm(
        lv_tr("Z offset not set"),
        lv_tr("Continue with the remaining steps, or stop the calibration?"),
        ModalSeverity::Warning, lv_tr("Continue"),
        /* Continue: done(StepOutcome::skipped()) */, options);
}
```

Continue skips the step; Stop calibration, a backdrop tap or any other dismissal stops the run
through `StepContext::stop_run`.

### Heater calibration on Kalico: TOLERANCE and per-cycle progress

Mainline Klipper's `PID_CALIBRATE` runs a fixed five cycles. Kalico's repeats cycles until the
last ones agree within `TOLERANCE`, 0.02 by default, and a bed's cycles take minutes each, so a
bed could run for most of an hour at the default. Discovery marks Kalico from `printer.info`'s
`app` field, which sets `PrinterDiscovery::pid_calibrate_takes_tolerance()` (and
`save_config_takes_restart_param()`, below). `helix::pid::calibrate_tolerance()` then picks:

```cpp
// Kalico's own default (pid_calibrate.py TUNE_PID_TOL).
constexpr double kHotendTolerance = 0.02;
// Kalico's docs: noisy readings need 0.03 or more; a bed's cycles run for
// minutes each, so it settles for a looser fit.
constexpr double kBedTolerance = 0.05;
```

`calibrate_command()` appends `TOLERANCE=0.05` for `heater_bed` and `TOLERANCE=0.02` for any other
heater on Kalico, and nothing on mainline. `MoonrakerAdvancedAPI::start_pid_calibrate()` builds
its command through it, so the Heaters screen and the guided run send the same thing.

The PID progress collector parses each `sample:N pwm:... asymmetry:... tolerance:...` line
(asymmetry may be negative). The heater step turns it into progress
(`HeatersStep::report_pid_cycle()`): with a tolerance target the bar follows how close the cycles
are on a log scale, and the phase line reads "Bed: PID at 60°C · cycle 4, 0.27 (needs 0.05)";
without one it counts cycles out of `PidProgressTracker::kExpectedCycles`. Each heater gets an
equal share of the step's bar.

### Stopping: the least destructive stop

Stop asks first, then calls `CalibrationRun::stop()`, which marks the running step Aborted and the
rest Not run before calling the step's `abort()`. A Klipper command that is already running can
only be stopped with `M112` and a firmware restart, and that restart drops everything the run had
staged. So each step sends `M112` only when its command is actually running:

| Step | Stop while waiting | Stop while its command runs |
|------|--------------------|-----------------------------|
| Z offset | nothing is sent | a manual probe ends with `ABORT`; no restart |
| Heaters | the control-type query is dropped | `M112` and a firmware restart |
| Bed mesh | the panel's preparation is cancelled and the heaters it turned on go off | `M112` and a firmware restart, once `BED_MESH_CALIBRATE` was sent |
| Input shaper | homing or the noise check: dropping the calibrator ends its chain | `M112` and a firmware restart during either axis's test |

When the abort restarted the firmware and something was staged, `results_lost()` turns true and
the Results page says nothing can be saved. Stop before the run starts marks every step Not run.

### Save all and Discard all

**Save all** (`CalibrationRun::save_all()`) is enabled when some entry `saves()`: it finished or
failed after staging part of its result, and its result is not one the firmware persisted. It
calls `RunFinisher::save_all()`, which in `MoonrakerRunFinisher` is one
`SaveConfigWatch::begin()`. Only once the watch reports the save happened does the run record
each finished step into the history and move to `Saved`. A failed save moves to `SaveFailed`
with its reason, where the owner can save again or discard.

**Discard all** (`CalibrationRun::discard_all()`) works in two passes:

1. **Restore** each finished step with `restores_on_discard()`, one at a time. Today that is a
   firmware-managed Z offset: the firmware stored it the moment it was set, so no restart undoes
   it. `ZOffsetStep::restore()` sends the offset from before the step through the same
   `zoffset::firmware_managed_offset_gcode()` the step used, and the results row reads "Restored
   to -1.245 mm" or "Could not restore: ...".
2. **Ask Klipper** whether anything is pending (`RunFinisher::read_pending()`, an unreadable
   answer counts as pending). Nothing pending finishes the discard with no restart. Otherwise
   `discard_all()` restarts Klipper through the expected-restart contract, which drops every
   pending item. The decision is what Klipper holds, not what the steps reported, because a failed
   step can have staged part of its result.

A failed restart returns to Results with the reason, since the results are still pending.

---

## Saving: SaveConfigWatch

Klipper's `SAVE_CONFIG` writes `printer.cfg` and restarts as its last act, so it never
acknowledges the command: the rpc fails with "Klippy Disconnected" for a save that worked. Every
caller that sends it holds a `SaveConfigWatch` (`include/save_config_restart.h`), which owns the
whole contract: arm the expected-restart suppressions, send the save, absorb the dropped rpc, and
report success when Klipper is back READY. It still reports a real failure, an rpc error with no
restart in sight.

### Kalico: SAVE_CONFIG RESTART=0

Kalico's `SAVE_CONFIG` takes `RESTART=0`: it writes the file and stays up. Writing the file
changes nothing in the running Klipper, though, so a value Klipper reads once at startup keeps
its old value until a restart. Skipping the restart is safe only when every pending item is
already in effect, because the command that staged it also applied it.
`save_config::restart_required()` (`src/printer/save_config_live.cpp#restart_required`) decides:

```cpp
bool restart_required(const PrinterDiscovery& hw, const nlohmann::json& pending_items) {
    if (!hw.save_config_takes_restart_param() || !pending_items.is_object()) {
        return true;
    }
    for (const auto& [section, options] : pending_items.items()) {
        if (!item_live(section, options, hw)) {
            return true;
        }
    }
    return false;
}
```

`item_live()` accepts an item when one of the live rows does, each citing the Klipper code that
applies its value as it stages it:

| Row | Pending item | Live because |
|-----|--------------|--------------|
| `bed_mesh_profile_live` | `[bed_mesh <name>]`, saved or removed | the profile manager copies the mesh into its profiles as it stages it |
| `heater_pid_live` | a heater section's PID options, with `control` absent or a PID kind | `PID_CALIBRATE` switches the heater to the tuned control before staging it. MPC does not qualify: `MPC_CALIBRATE` restores the old control when it ends |
| `input_shaper_live` | `[input_shaper]` shaper types and frequencies, when an `[input_shaper]` is configured | `SHAPER_CALIBRATE` applies the shaper before staging it |
| `tool_offset_live` | `[tool <name>]` `gcode_{x,y,z}_offset` | `SET_TOOL_PARAMETER` applied it; `SAVE_TOOL_PARAMETER` only stages it |

Anything else restarts, including a probe's `z_offset` and `[stepper_z] position_endstop`: both
are only written to the config, and the running Klipper read the old value at startup. So on
Kalico a guided run of heaters (PID), bed mesh and input shaper saves without a restart, while a
run that staged a probe Z offset or an MPC model still restarts. On mainline Klipper every save
restarts.

### How begin() decides, and why each save carries a number

`begin()` reads `configfile.save_config_pending_items` first on a firmware that takes
`RESTART=0`, and branches:

- **Restarts:** `begin_with_restart()` watches the klippy state subject, arms the
  expected-restart suppressions, and sends `SAVE_CONFIG`. Klipper returning READY settles it.
- **Stays up:** `begin_without_restart()` sends `SAVE_CONFIG RESTART=0`. No suppressions are
  armed and the rpc's own reply is the outcome.

A pending-items read that fails assumes a restart. Each `begin()` takes a new number (`save_`),
and every callback that save sends checks `current(save)`, so a late reply or READY from an ended
save can neither settle the next save nor start a `SAVE_CONFIG` for it.

### query_restarts(): wording a confirmation

A confirmation before a save should say whether Klipper restarts. `query_restarts()` answers by
the same rule `begin()` uses, without saving:

```cpp
/// Whether a SAVE_CONFIG sent now would restart Klipper, decided as begin()
/// decides it. @p on_answer runs once, on the main thread, unless this watch
/// is destroyed first. A firmware that always restarts answers true without
/// asking; so does a pending-items read that fails.
void query_restarts(IMoonrakerAPI* api, helix::SharedCallback<void(bool)> on_answer);
```

The Z offset screen's Save and the INDX heater save ask it before confirming, so on Kalico a
save that skips the restart no longer warns of one. `begin()` asks again when it runs, so a change
staged between the question and the save still gets the restart it needs.

---

## Adding a Step

### A step the catalog and the Calibration screen know

1. **Add the kind and the row.** A `StepKind` value in `include/calibration_catalog.h`, a row in
   `kSteps` at its place in the run order, and bump `kStepCount`. The id is permanent: it is the
   history key and the subject suffix.
2. **Say when it applies.** A case in `applies()`, from a `CatalogInputs` field. A new field is
   filled in `catalog_inputs_from()` from a capability subject, never by asking the printer
   directly, so the catalog stays testable.
3. **Add its tile.** A `calibration_hub_tile` in `ui_xml/calibration_hub_overlay.xml` with
   `step="<id>"`, its label (sentence case), icon and the callback that opens its screen. Mark
   the name with `TR_NOOP` and add the strings to every locale.
4. **Optionally, an In-config reader.** A function in `results_in_config()` that returns a short
   summary when the config holds a tuned value, built from the shared formatters.
5. **Optionally, a database tier.** A `calibration.steps` entry for the printers where its tier
   differs from the default.
6. **Tests.** `[calibration_catalog]` cases for `applies()` and the tier, and
   `[calibration_history]` for any reader.

### A step the guided run drives

1. **Wrap the code that already runs it.** Subclass `CalibrationStep` in
   `src/ui/calibration/calibration_run_steps.cpp`: call the API, calibrator or panel its screen
   uses, report `StepProgress`, and call `done()` exactly once. Never send `SAVE_CONFIG`; give a
   panel a run mode, as the Z offset and bed mesh panels have, when it would.
2. **Make abort least destructive.** Send `M112` (`stop_with_firmware_restart()`) only while a
   Klipper command is running, and return true from `abort_restarted_firmware()` only then.
3. **Report partial results** with `StepOutcome::failed_after()` when a failure leaves part of the
   result pending.
4. **Persisted results restore.** If the firmware stores the result itself, implement
   `restores_on_discard()` and `restore()`.
5. **Offer it.** Add its kind to `in_guided_run()` and `run_rank()` in `calibration_run.cpp`, a
   case in `make_run_step()`, its id to `kRunStepIds` in `include/ui_calibration_run_overlay.h`,
   rows in `ui_xml/calibration_run_overlay.xml`, and a safety line in `warning_for()` if it heats
   or moves.
6. **Check the save.** If its result is applied live when staged, add a row to `kLiveRows` in
   `save_config_live.cpp` citing the firmware code that applies it; otherwise a Kalico save that
   includes it restarts, which is the safe default.
7. **Tests.** `[calibration_run]` cases with a fake step and a fake `RunFinisher` cover ordering,
   stop, save and discard without a printer; follow the existing ones in
   `tests/unit/test_calibration_run.cpp`.

---

## Mock and Debugging

- `./build/bin/helix-screen --test -vv`, then Advanced > Calibration, drives the screen and the
  run against the mock printer.
- `HELIX_MOCK_KALICO=1` makes the mock report Kalico: MPC heater control, and `SAVE_CONFIG
  RESTART=0` that writes without restarting
  ([MOCK_ENVIRONMENT_VARIABLES.md](MOCK_ENVIRONMENT_VARIABLES.md#helix_mock_kalico)).
- The run logs every step start, outcome, stop, save and discard under `[CalibrationRun]`;
  `SaveConfigWatch` logs whether a save restarts and why under `[SaveConfigWatch]`, including the
  pending items that forced a restart.
- Screenshot recipes: `calibration`, `pid`, `input-shaper` and `zoffset` in
  `scripts/screenshot-recipes.sh`.
