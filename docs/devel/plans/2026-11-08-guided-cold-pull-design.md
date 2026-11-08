# Guided cold pull: design

Status: draft for Preston's review, 2026-11-08. No code yet. Tracks
prestonbrown/helixscreen#2011. Deleted in the change that ships the work (`docs/CLAUDE.md`).

## Why

A clog is already something we report (`key845` in `src/printer/klipper_error_table.cpp`,
hint "Run a cold pull or replace the nozzle"), and nothing stands behind the hint. Doing a cold
pull by hand today takes three screens (temperature, extrude, fan) and the cold-extrude
override (#978). The step people get wrong is the pull temperature, and a screen that heats,
cools with the fan and says "pull now" removes the guesswork. #1901 will add a cold pull
maintenance meter, whose reset belongs at the end of this flow.

## Goals

- One surface walks the user from setup to a clean tip: heat, feed, cool, pull, inspect,
  repeat or finish.
- Every exit (Done, Cancel, back, timeout, a print starting, a disconnect, app shutdown) leaves
  the heater off or in the hands of whoever took it, and the fan where it was.
- Works on every Klipper printer whose toolhead the user can feed and pull by hand, single or
  multi-head, with or without a part fan.

## Non-goals

- An extruder-driven pull below `min_extrude_temp`. We never send `FORCE_MOVE`, never edit
  `min_extrude_temp`, never send `SET_STEPPER_ENABLE` or `M18` to "free" the extruder.
- Running a cold pull during a print, paused or not.
- Detecting the nozzle type (coated, hardened, high-flow). The user is told; nothing is guessed.

## The signals

What the flow reads, who publishes it, and which printers lack it.

| Signal | What it measures | Source | Printers without it |
|---|---|---|---|
| Nozzle temperature / target | The chosen head's heater, deci-degrees | `extruder*` objects; `PrinterState::temperature_state()` subjects, per extruder by name (`FilamentOpHeater::temp_subject` reads them the same way) | None |
| `min_extrude_temp` | The temperature below which Klipper refuses `G1 E` | `configfile`; `SafetyLimits::min_extrude_temp_for()` (`include/moonraker_types.h`), rounded by `extrusion_floor_c()` (`src/ui/controls/ui_temperature_utils.cpp#extrusion_floor_c`) | None (Klipper default 170 °C) |
| Max nozzle temperature | The soak ceiling | `nozzle_max_temp_c()` in the same file | None |
| `homed_axes` | Whether the head may move | `toolhead_is_homed()` (`include/toolhead_homing.h`) | None |
| Part cooling fan | The fan that cools the nozzle tip | `ToolInfo::fan_name` per tool (`include/tool_state.h`), else `PrinterFanState::resolve_part_fan()`; speed from `get_fan_speed_subject(name)` | Printers whose part fan is an `output_pin` the firmware drives (`output_pin_fan_is_firmware_driven()`), and printers with none configured. They cool without a fan. |
| Toolhead filament sensor | Filament present at the extruder | `FilamentSensorManager::get_toolhead_detected_subject()`; -1 means no such sensor | Most printers. The flow then waits for a tap. |
| Print lifecycle | Whether a job owns the machine | `job_holds_machine()` (`include/print_lifecycle_state.h`) | None |
| Filament system lane state | What sits in the toolhead, bypass | `AmsSystemInfo`, `AmsBackend::is_bypass_active()`, `AmsBackend::is_filament_loaded()` | Printers with no filament system: the user unloads by hand |
| Park macro | A safe, reachable head position | `StandardMacroSlot::ParkToolhead` (`include/standard_macros.h`, patterns in `include/macro_patterns.h#park_toolhead`) | Printers with no PARK-like macro: the head stays where it is |
| Direct drive or bowden | Where the user grips and pulls | Nothing reports it. `pa::ExtruderKind` (`include/pa_tuning.h`) is a user choice in PA tuning, never detected. | Every printer: the flow shows both instructions or asks once (open question 3) |

## The procedure

The cold pull ("atomic pull") melts filament into the nozzle, lets it cool until it is solid
enough to hold together but soft enough to release the wall, then pulls it out by hand so
carbonised residue comes with it. A clean pull shows a cone with the nozzle's imprint and no
dark specks or colour of the old material.

Sources disagree by tens of degrees, so every number below is a default the user can edit.

| Material | Soak (°C) | Pull (°C) | Evidence |
|---|---|---|---|
| PLA (default) | 230 | 90 | Bambu P1S wiki soaks PLA at 230 °C ([p1s cold pull](https://wiki.bambulab.com/en/p1/manual/p1s-cold-pull)); Bambu X1 wiki pulls PLA at 70-100 °C, starting at 70 ([X1 cold pull](https://wiki.bambulab.com/en/x1/maintenance/what-is-cold-pull-and-how-to-perform-it)); Prusa's manual procedure soaks at 270 °C, pushes while cooling to ~170 °C, pulls at 100 °C ([Prusa MK4/XL](https://help.prusa3d.com/article/cold-pull-mk4-s-mk3-9-s-xl-13702-17702_445071)); a Prusa forum report needed 75 °C on a high-flow nozzle. 90 sits inside every range. |
| Nylon | 250 | 140 | 140-160 °C pull and a 240 °C soak in community guides ([3dprinterly](https://3dprinterly.com/how-to-do-a-cold-pull-on-a-3d-printer-cleaning-filament/), [All3DP](https://all3dp.com/2/3d-printer-clogged-nozzle-how-to-perform-a-cold-atomic-pull/)); a Prusa forum moderator recommends nylon 618 at 140. No vendor publishes a nylon number. |
| Cleaning filament | 260 | 100 | Ultimaker soaks cleaning filament at 260 °C ([Ultimaker support](https://support.ultimaker.com/articles/Knowledge/1667337897165)); FBRC8 pulls it at 145 on the UM2 ([FBRC8 atomic method](https://support.fbrc8.com/hc/en-us/articles/25226296958875-Atomic-Cleaning-Method)). No maker publishes its own cleaning filament's pull temperature; 100 is the issue's starting value and is open question 1. |

The issue proposed 230 °C for cleaning filament. Ultimaker's 260 is the only maker number we
found, so this plan uses it.

Rules that come from the sources, not from taste:

- **Soak at least as hot as what is already in the nozzle.** Prusa soaks PLA at 270 °C for this
  reason. The soak target goes through `SendOptions::keep_previous_hot` (the swap latch), which
  already raises a target to the last material's temperature.
- **Keep the nozzle full while it cools.** Prusa keeps pushing until ~170 °C. Klipper allows that
  push down to `min_extrude_temp`, so the flow sends small extrusions while the nozzle is above
  `floor + kHoldPressureMarginC` and stops there (see Cooling).
- **Too soft, the tip stretches; too cold, it snaps.** The inspect step says so, and editing the
  pull temperature is one tap away. Bambu's wiki adjusts in 5 °C steps.
- **Some nozzles must not be cold-pulled.** Prusa forbids it on Nozzle X (coating); E3D coatings
  draw the same warning. Setup shows a one-line warning; we cannot detect the nozzle.

## What exists and what this reuses

| Need | Reuse | Where |
|---|---|---|
| Pure event-to-script state machine | Pattern and the Feed sub-step itself | `GuidedLoadFlow` (`include/guided_load_flow.h`, `src/ui/filament/guided_load_flow.cpp`) |
| Step-wizard overlay with a pure step machine beside it | Shape: pure `plr_next_wizard_step` + `PlrWizardOverlay`; `DisplayAwakeToken` while it waits on the user; `on_dismissed` is the abort | `include/plr_wizard.h`, `src/ui/print/ui_plr_wizard_overlay.cpp#PlrWizardOverlay::on_dismissed` |
| Heater + fan run that survives the screensaver | `take_wake()`, `run_lifetime_`, `on_deactivating(Suspended)` returning early, heaters left alone while a job holds the machine | `src/ui/calibration/ui_panel_calibration_pid.cpp#PIDCalibrationPanel::on_activate`, `#PIDCalibrationPanel::on_deactivating` |
| Temperature targets | `TemperatureController::set_target(klipper_name, c, SendOptions)`, the only allowed path (lint-enforced) | `include/temperature_controller.h` |
| Heater ownership against other ops | `FilamentOpHeater::pending()`, `cancel_owned()`; its "target taken away", "job started", restore-prior rules are the model for ours | `include/filament_op_heater.h`, `src/ui/filament/filament_op_heater.cpp` |
| Preheat decision and reach test | `decide_preheat()` for the soak target, `preheat_reached()` and `kPreheatMarginC` for "soak reached" | `include/filament_op_heater.h` |
| Head choice on multi-head printers | `HeadPicker` chip row, `ToolState::heated_heads()`, `PhysicalHead::representative_tool()` | `include/ui_head_picker.h`, `include/tool_state.h` |
| Mounting the chosen head | `decide_tool_change()` / `tool_change_inputs()` and the existing dispatch | `include/tool_change_request.h` |
| Homing and park | `toolhead_is_homed()`, `ensure_homed_then()`; `StandardMacros` `ParkToolhead` | `include/toolhead_homing.h`, `include/standard_macros.h` |
| Fan command | `fan_gcode(fan, percent)`; returns empty for a firmware-driven fan | `include/fan_gcode.h` |
| Extrude script | `M83\nG1 E{mm} F{speed}` exists inline in `FilamentPanel::execute_extrude` and `execute_retract` (`src/ui/filament/ui_panel_filament.cpp`) and as `filament_purge_fallback_gcode()` (`src/ui/filament/filament_op_router.cpp`). Cold pull is the third caller: extract one `extrude_gcode(mm, speed_mm_min)` beside `filament_purge_fallback_gcode` and call it from all three. | `include/filament_op_router.h` |
| Clog alert button | `RecoveryAction` on `key845`; the presenter dispatch | `include/error_event.h#RecoveryAction`, `src/ui/notify/recovery_modal_presenter.cpp#RecoveryModalPresenter::on_recovery_tapped` |
| Opening a flow by id from generic code | `register_guided_device_flow()` / `find_guided_device_flow()` | `include/ui_guided_device_flow.h` |
| Progress ring | `helix_progress_arc` + `attach_progress_arc()` (diameter tiers 120-179 and 180-239) | `ui_xml/components/helix_progress_arc.xml`, `include/ui_progress_arc.h` |
| Maintenance meter reset | `maintenance::reset_meter()` | `include/maintenance_meters.h` |

Searched and not reused: `ui_cfs_device_flows.cpp` and `ui_indx_device_flows.cpp` are
confirm-then-send macro launchers with no heater ownership; `GuidedLoadModal` would stack a
second modal over the flow and its `opens_during()` rule is the same as ours, so only
`GuidedLoadFlow` is embedded, not the modal. `ui_manual_pull_prompt.cpp` toasts when filament
clears the toolhead sensor after a retract; the cold pull's pull is not a retract and needs no
toast.

## Shape

Three pieces, split the way `GuidedLoadFlow` / `GuidedLoadModal` and `FilamentOpHeater` split:

1. **`ColdPullFlow`** (`include/cold_pull_flow.h`, `src/ui/filament/cold_pull_flow.cpp`). Pure:
   no LVGL, no I/O, no clock of its own. Every event (time included) returns a `ColdPullActions`
   value. All stage, timeout and safety logic lives here and is tested here.
2. **`ColdPullRunner`** (`include/cold_pull_runner.h`, `src/ui/filament/cold_pull_runner.cpp`).
   App-lifetime, owned by `SubjectInitializer` like `FilamentOpHeater`, reached through
   `get_cold_pull_runner()`. Owns the one flow, the observers (nozzle temp/target, lifecycle,
   connection, toolhead sensor), the 1 s tick timer, and applies actions through
   `TemperatureController` and `IMoonrakerAPI`. Publishes the stage and numbers as subjects.
   The heater is watched by the runner, never by a widget tree, so closing or rebuilding the
   view cannot leave a hot nozzle unwatched.
3. **`ColdPullOverlay`** (`include/ui_cold_pull_overlay.h`, `src/ui/filament/ui_cold_pull_overlay.cpp`,
   `ui_xml/cold_pull_overlay.xml`). A view over the runner's subjects. `OverlayBase`,
   `destroy_on_close() == true`, one `<view>` per stage switched by a `cold_pull_stage` subject.
   Strings through `lv_tr`.

### Why an overlay and not a modal (measured)

`ctl geom` on a `--test` instance, PID calibration overlay and runout guidance modal:

| Surface | 800x480 | 480x272 |
|---|---|---|
| Overlay header | 708 x 56 | 430 x 40 |
| Overlay content (inner) | 676 x 382 | 414 x 208 |
| Modal dialog | 640 x 240 | 384 x 141 |
| Modal message area | 608 x 48 | 368 x 36 |
| Modal button row | 640 x 53 | 384 x 27 |
| Wide action button | 198 x 52 | 120 x 26 |
| Preset chip | 147 x 40 | 90 x 24 |

A modal at 480x272 has 141 px for a ring, an instruction and two buttons; the ring alone needs
120 to reach its third thickness tier. The overlay has 208, and the screensaver/wake lifecycle
(`take_wake()`) comes with `OverlayBase`.

## Step sequence and UI

Body layout at both sizes: a ring column on the left, instruction and controls on the right, an
action row at the bottom. Numbers are the inner content box above.

| | 800x480 | 480x272 |
|---|---|---|
| Action row | 52 high (button height) | 30 high |
| Body above it | 382 - 52 - 12 gap = 318 | 208 - 30 - 8 gap = 170 |
| Ring diameter | 220 (tier 3) | 120 (tier 2), leaving 50 under it for the caption |
| Text/controls column | 676 - 220 - 24 = 432 | 414 - 120 - 12 = 282 |

The ring shows the live nozzle temperature against the stage's goal: rising to soak while
heating, falling to pull while cooling. The caption under it reads `current / goal °C`.

### 1. Setup (`Setup`)

- Material chips: PLA, Nylon, Cleaning (three chips of 147 x 40 / 90 x 24 fit both widths).
  Under them, two editable numbers, Soak and Pull, through the existing numeric keypad with
  `TemperatureController::effective_keypad_max(HeaterType::Nozzle, ...)` as the soak ceiling.
  Pull is limited to `[kMinPullC, soak - kMinSoakPullGapC]`.
- Head chips (`HeadPicker`, prefix `cold_pull`) only when `ToolState::heated_heads().size() > 1`.
- One warning line: "Do not cold pull a coated nozzle."
- If the printer defines a cold pull macro (open question 4), a secondary "Use printer's cold
  pull" button runs it and closes the overlay; the built-in flow stays the primary button.
- No ring. Action row: Cancel, Start.
- Refused, with the reason in place of Start: a job holds the machine; `FilamentOpHeater` has a
  preheat pending; Klippy not ready; the chosen head's heater is unknown.

### 2. Prepare (`Preparing`)

- Multi-head: mount the chosen head through the tool-change dispatch when `decide_tool_change()`
  says it dispatches; a refusal ends the flow with that refusal's text.
- Park: when homed and `ParkToolhead` resolves to a macro, send it. When not homed, send
  nothing that moves; never home on our own (the H2D report in #2011 is a Z crash from exactly
  that). The step shows "Home and park" (`ensure_homed_then()` then the park macro) only when a
  park macro exists; otherwise "The head stays where it is."
- The filament in the head decides the rest of the flow, with `ColdPullParams::toolhead_loaded`
  set at start (a lane seated and no bypass active):
  - Filament system with a lane loaded into this head: nothing is unloaded. The pull uses the
    filament that is already there, and the step says so.
  - No filament system, or nothing loaded: "Unload the current filament, then continue."
    The user's own unload, or the standard unload, is theirs to run.
- Action row: Cancel, Continue.

### 3. Heat (`Heating`)

- `set_target(head.heater, soak_c, {keep_previous_hot = true})`. Reached when
  `preheat_reached(current, effective_soak, floor)`; effective soak is the target that actually
  landed (the latch may have raised it), read back from the target subject.
- Ring rises. Text: "Heating to 230 °C". Action row: Cancel.
- Timeouts: target never lands within `FilamentOpHeater::kTargetLandTimeout` (20 s); not reached
  within `FilamentOpHeater::kPreheatTimeout` (10 min).

### 4. Feed (`Feed`, embedding `GuidedLoadFlow`)

- An embedded `GuidedLoadFlow` with steps `{feed = extrude_gcode(kFeedMm, kFeedSpeed),
  retry = extrude_gcode(kPurgeMoreMm, kPurgeSpeed)}`, no flush, release or cleanup, and
  `waits_for_push = true`. Its stages map to text:
  - AwaitPush: "Push the cleaning filament into the extruder until it stops." Extrude button.
    A toolhead sensor edge (`filament_detected()`) feeds without the tap.
  - Feeding: spinner on the Extrude button.
  - ConfirmOutput: "Does clean filament come out of the nozzle?" Extrude more, Done.
- Seated lane (`ColdPullFeedMode`): first pass is `Loaded`, the embedded flow opens on
  ConfirmOutput (nothing to push, purge only: `kPurgeMoreMm`). After a pull it is `Refeed`: the
  hand-fed path, with "Cut off the pulled tip and push the filament back".
- People run this because of a clog, so nothing here depends on filament moving. Done is always
  available (even mid-extrude), a refused extrude leaves the step usable, and a line says
  extruding is optional. The hold-pressure option is off by default and Setup says to leave it
  off for a clog; its moves never gate the Pull.
- The ring holds at soak. Action row: Cancel, Extrude / Extrude more, Done.
- Idle timeout `kFeedIdleTimeout` (8 min, under Klipper's default 10 min `idle_timeout`), reset
  by every tap.

### 5. Cool (`Cooling`)

- `set_target(head.heater, pull_c)` and the head's part fan to 100 % through `fan_gcode`. The
  previous fan speed is read from its subject first and kept for restore. Setting the target to
  the pull temperature rather than 0 means the heater holds there once the nozzle arrives, so a
  user who looks away for a minute does not come back to a 40 °C nozzle.
- Hold pressure: every `kHoldPressureInterval` (3 s) while `current >= floor + kHoldPressureMarginC`,
  send `extrude_gcode(kHoldPressureMm, kHoldPressureSpeed)` (1 mm). Stops for good at the margin, so
  no extrusion is ever queued near `min_extrude_temp`. At most one in flight.
- No commandable part fan: no fan command, the text says "Cooling without a fan; this takes
  longer", and the timeout doubles.
- Ring falls. Text: "Cooling to 90 °C. Do not touch the filament yet." Action row: Cancel.
- Timeout: `kCoolTimeout` (10 min), `kCoolTimeoutNoFan` (20 min).

### 6. Pull (`Pull`)

- Entered when `current <= pull_c + kPullBandC` after the nozzle has been above it. Entering on
  the target change is the bug the issue names; the test pins it.
- Part fan back to its previous speed (the heater now holds the temperature).
- `DisplayAwakeToken` acquired: the screensaver must not hide the moment to act. Released on
  leaving the stage.
- Text: "Release the extruder tension and pull the filament out in one firm motion." Bowden
  variant: "Pull the tube from the extruder end, then the filament." Action row: Cancel, Pulled.
- Hold timeout `kPullHoldTimeout` (5 min): heater off, flow ends Abandoned with "The nozzle
  waited too long; start again."

### 7. Inspect (`Inspect`)

- Heater off on entry (`set_target(head.heater, 0)`); nothing is hot while the user looks.
- Two reference pictures side by side, clean cone and dirty tip, at 2 x 200 x 150 (800x480) or
  2 x 130 x 98 (480x272) with a caption under each. Hint line: "Stretched thin: pull a little
  colder. Snapped: pull a little warmer." with the Pull number tappable.
- Pass counter "Pass 2".
- Seated lane: after a hand pull the filament is out of the hotend while the backend still
  reports the lane loaded. `ColdPullFlow::lane_needs_reseat()` shows "This lane still shows as
  loaded..." here and on the end screen until the filament is fed back on a Repeat. The flow sends
  no backend command for this.
- Action row: Repeat (back to Heat, counter + 1), Done.

### 8. Finish (`Finished`, `Cancelled`, `Abandoned`)

- Done: heater already off, fan already restored, reset the cold pull meter when #1901 has
  added one (`MeterId` for it exists), then close. Cancel never resets the meter.
- Cancelled / Abandoned: one line naming why, then close on tap.

## Safety rules

1. **One owner of the heater.** `ColdPullRunner` sends every target. Start is refused while
   `FilamentOpHeater::pending()`. While the flow runs, a target on the head that differs from
   the last one we sent by more than `kForeignTargetToleranceC` ends the flow Abandoned
   ("Temperature changed elsewhere") and the heater is left where the other party set it; the
   fan is still restored.
2. **Every exit lands in `exit_actions()`.** Cancel, Abandoned, Done, the overlay's
   `on_dismissed()`, `DeactivateReason::Shutdown`, and the runner's destructor all call it.
   It turns the head's heater off (`set_target(heater, 0)`) unless a job now holds the machine,
   and restores the part fan to its saved speed unless a job holds the machine. Inspect and Done
   already have the heater off, so the call is idempotent.
3. **Screensaver is not an exit.** The overlay's `on_deactivating(Suspended)` returns early, as
   in `PIDCalibrationPanel::on_deactivating`; the runner keeps running because it never belonged
   to the view. `on_activate()` with `take_wake()` true redraws the current stage from the
   runner's subjects. Other `NavigateAway` reasons do not end the run either (the PLR rule): the
   overlay is still on the stack, and back or close is the abort.
4. **Timeouts per stage** (table above). Each ends Abandoned through `exit_actions()`.
   Timeouts are measured on the runner's injected steady clock, never on LVGL ticks.
5. **A print takes the machine.** `job_holds_machine()` turning true mid-flow ends it Abandoned
   with heaters and fan left to the print, as `PIDCalibrationPanel::on_deactivating` does.
6. **Disconnect or Klippy shutdown.** The flow ends Abandoned with "Lost the printer". Klipper
   may still be running with our target, so the runner keeps an owed cleanup: on the next
   ready connection, if no job holds the machine and the head's target still equals the last
   one we sent, it sends heater off and the fan restore (the `Owed` pattern in
   `FilamentOpHeater`). A Klipper restart already zeroed it; the check sees that and sends
   nothing.
7. **No motion on an unhomed machine.** The flow emits no `G0`/`G1 X/Y/Z`, no park macro and no
   `G28` unless the user tapped "Home and park". Extrusion (`G1 E`) is the only move it sends
   on its own, and only above `floor + kHoldPressureMarginC`.
8. **No cold moves.** Nothing below `min_extrude_temp` is ever sent: no `FORCE_MOVE`, no
   `SET_STEPPER_ENABLE`, no `M18`/`M84`, no `min_extrude_temp` edit, no
   `SafetySettingsManager::allow_cold_extrude` toggle.
9. **Multi-head isolation.** Only the chosen head's heater and its own part fan are commanded.
   On a shared part fan (single-carriage dual nozzle) the shared fan is the one used.

## Interfaces

Exact shapes; bodies are the implementer's. Every header below gets a
`scripts/syntax_check.py` pass before implementation starts.

```cpp
// include/cold_pull_flow.h
namespace helix {

enum class ColdPullMaterial : int { Pla = 0, Nylon = 1, Cleaning = 2 };
inline constexpr int kColdPullMaterialCount = 3;

struct ColdPullTemps {
    int soak_c = 0;
    int pull_c = 0;
};

/// Defaults per material, in ColdPullMaterial order (see the plan's table).
[[nodiscard]] ColdPullTemps cold_pull_default_temps(ColdPullMaterial m);

/// Clamps a user edit: soak within [floor_c + kMinSoakAboveFloorC, ceiling_c],
/// pull within [kMinPullC, soak_c - kMinSoakPullGapC].
[[nodiscard]] ColdPullTemps clamp_cold_pull_temps(ColdPullTemps t, int floor_c, int ceiling_c);

inline constexpr int kMinPullC = 40;
inline constexpr int kMinSoakPullGapC = 60;
inline constexpr int kMinSoakAboveFloorC = 20;
inline constexpr int kPullBandC = 3;
inline constexpr int kHoldPressureMarginC = 15;
inline constexpr int kForeignTargetToleranceC = 2;
inline constexpr int kFeedMm = 60, kFeedSpeedMmMin = 300;
inline constexpr int kPurgeMoreMm = 20, kPurgeSpeedMmMin = 300;
inline constexpr int kHoldPressureMm = 1, kHoldPressureSpeedMmMin = 60;
inline constexpr std::chrono::seconds kHoldPressureInterval{3};
inline constexpr std::chrono::minutes kFeedIdleTimeout{8};
inline constexpr std::chrono::minutes kCoolTimeout{10};
inline constexpr std::chrono::minutes kCoolTimeoutNoFan{20};
inline constexpr std::chrono::minutes kPullHoldTimeout{5};

enum class ColdPullStage : int {
    Setup = 0, Preparing, Heating, Feed, Cooling, Pull, Inspect,
    Finished, Cancelled, Abandoned,
};

enum class ColdPullEndReason : int {
    None = 0, UserCancel, Timeout, ForeignTarget, JobStarted, Disconnected,
    TargetNeverLanded,
};

/// Everything the flow fixes at start.
struct ColdPullParams {
    ColdPullTemps temps;
    std::string heater;            ///< Klipper heater object of the chosen head
    std::string part_fan;          ///< "" when no commandable part fan
    int prior_fan_percent = 0;     ///< Restored on every exit
    int floor_c = 0;               ///< extrusion_floor_c() for the head
    bool homed = false;
    bool has_park_macro = false;
    bool toolhead_loaded = false;  ///< A lane is seated in this head
};

/// What to send now. Applied in field order by ColdPullRunner.
struct ColdPullActions {
    std::optional<int> heater_target_c;   ///< via TemperatureController::set_target
    bool keep_previous_hot = false;       ///< SendOptions flag for heater_target_c
    std::optional<int> fan_percent;       ///< via fan_gcode(part_fan, ...)
    std::string gcode;                    ///< extrusion, park; "" for none
    bool run_park = false;                ///< park macro (only ever set when homed)
    bool home_then_park = false;          ///< ensure_homed_then + park (user tap only)
    bool reset_meter = false;             ///< maintenance cold pull meter (Done only)
    bool hold_awake = false;              ///< DisplayAwakeToken held while true
    [[nodiscard]] bool empty() const;
};

class ColdPullFlow {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit ColdPullFlow(ColdPullParams params);

    [[nodiscard]] ColdPullStage stage() const;
    [[nodiscard]] ColdPullEndReason end_reason() const;
    [[nodiscard]] bool ended() const;
    [[nodiscard]] int pass() const;               ///< 1-based
    [[nodiscard]] int goal_c() const;             ///< what the ring counts toward
    [[nodiscard]] GuidedLoadStage feed_stage() const;
    [[nodiscard]] bool hold_awake() const;

    // User events
    [[nodiscard]] ColdPullActions start(TimePoint now);         // Setup -> Preparing
    [[nodiscard]] ColdPullActions home_and_park(TimePoint now);
    [[nodiscard]] ColdPullActions toolhead_ready(TimePoint now); // -> Heating
    [[nodiscard]] ColdPullActions extrude(TimePoint now);
    [[nodiscard]] ColdPullActions fed(TimePoint now);            // Feed done -> Cooling
    [[nodiscard]] ColdPullActions pulled(TimePoint now);         // -> Inspect
    [[nodiscard]] ColdPullActions repeat(TimePoint now);         // Inspect -> Heating
    [[nodiscard]] ColdPullActions done(TimePoint now);           // Inspect -> Finished
    [[nodiscard]] ColdPullActions cancel();

    // Printer events
    [[nodiscard]] ColdPullActions temperature(int current_c, int target_c, TimePoint now);
    [[nodiscard]] ColdPullActions filament_detected(TimePoint now);
    [[nodiscard]] ColdPullActions command_finished(bool ok, TimePoint now);
    [[nodiscard]] ColdPullActions job_started();
    [[nodiscard]] ColdPullActions disconnected();
    [[nodiscard]] ColdPullActions tick(TimePoint now);

    /// The cleanup every ending applies; idempotent. @p job_holds_machine leaves
    /// the heater and fan to the job.
    [[nodiscard]] ColdPullActions exit_actions(bool job_holds_machine) const;
};

} // namespace helix
```

```cpp
// include/cold_pull_runner.h
namespace helix::ui {

/// Why a start is refused, or Ok.
enum class ColdPullRefusal { Ok, JobHoldsMachine, PreheatPending, NotReady, NoHeater, Running };

/// Pure: the refusal for these inputs.
[[nodiscard]] ColdPullRefusal cold_pull_refusal(PrintState lifecycle, bool preheat_pending,
                                                bool klippy_ready, bool head_has_heater,
                                                bool running);

class ColdPullRunner {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    ColdPullRunner(PrinterState& state, TemperatureController& temps, Clock clock = {});
    ~ColdPullRunner();   // applies exit_actions() if a flow is live

    [[nodiscard]] ColdPullRefusal can_start(const PhysicalHead& head) const;
    ColdPullRefusal start(const PhysicalHead& head, ColdPullMaterial material, ColdPullTemps temps);
    // User events, each forwarding to the flow and applying what it returns.
    void home_and_park();
    void toolhead_ready();
    void extrude();
    void fed();
    void pulled();
    void repeat();
    void done();
    void cancel();
    [[nodiscard]] bool running() const;

    /// Subjects for the overlay: cold_pull_stage, cold_pull_feed_stage, cold_pull_goal_c,
    /// cold_pull_current_c, cold_pull_pass, cold_pull_end_reason, cold_pull_has_fan,
    /// cold_pull_multi_head (HeadPicker's own), cold_pull_can_park.
    void init_subjects();
    void deinit_subjects();

    void set_api(IMoonrakerAPI* api);
};

[[nodiscard]] ColdPullRunner* get_cold_pull_runner();

/// Opens the overlay. Registered as guided device flow "cold_pull" so generic code
/// (recovery actions, maintenance, filament panel) opens it by id.
void open_cold_pull_overlay();

} // namespace helix::ui
```

Persistence: per-material temperatures are user settings, so they live in
`MaterialSettingsManager` (`include/material_settings_manager.h`) beside the material
overrides:

```cpp
[[nodiscard]] ColdPullTemps get_cold_pull_temps(ColdPullMaterial m) const; // default when unset
void set_cold_pull_temps(ColdPullMaterial m, ColdPullTemps t);
void reset_cold_pull_temps(ColdPullMaterial m);
[[nodiscard]] ColdPullMaterial get_cold_pull_material() const;           // last used, PLA default
void set_cold_pull_material(ColdPullMaterial m);
```

Small changes to existing code:

- `filament_op_router.h`: `[[nodiscard]] std::string extrude_gcode(int mm, int speed_mm_min);`
  (negative mm retracts). `FilamentPanel::execute_extrude`, `execute_retract` and
  `filament_purge_fallback_gcode()` call it.
- `GuidedLoadFlow`: its script joins skip empty parts, so steps with no flush and no cleanup
  return `""` from `done()` and move straight to `Finished` instead of sending `"\n"`. Existing
  callers always have a cleanup, so their output is unchanged; a test pins both.
- `RecoveryAction` gains `std::string guided_flow;` (a `find_guided_device_flow()` id).
  `RecoveryModalPresenter::on_recovery_tapped` starts that flow instead of dispatching gcode
  when it is set; `same_actions()` compares it. Actions are matched by `gcode` today, so the
  presenter's lookup must match on `guided_flow` too (an empty gcode is not unique).
  `KlipperErrorEntry` gains the matching `const char* guided_flow`; `key845` names
  `"cold_pull"`.
- `MaintenanceOverlay`: a "Cold pull" button on the nozzle card now; on #1901's cold pull meter
  once it exists.
- Filament panel: a "Cold pull" entry (placement is open question 5).

## Tests

All in `tests/unit/`, tags `[cold_pull]` plus the area tag. The flow tests drive time through
`TimePoint` arguments; the runner tests inject `Clock`.

| # | Test | Pins | Fails if |
|---|---|---|---|
| 1 | Happy path PLA, single head, homed, park macro: Setup → Preparing (park) → Heating → Feed → Cooling → Pull → Inspect → Done | Stage order and every `ColdPullActions` field per step | Any stage is skipped or an action is missing |
| 2 | Pull fires on temperature, not on target: Cooling sets target 90; `temperature(225, 90)` stays Cooling; `temperature(92, 90)` enters Pull | `kPullBandC` and the "came from above" rule | Pull is entered on the target change |
| 3 | Pull not entered when the nozzle never got hot (starts below pull band) | The from-above latch | A cold nozzle skips Cooling |
| 4 | Repeat loop: Inspect → repeat() → Heating, pass 2, soak resent; three passes then Done | Counter and soak resend | Counter does not advance, or Repeat skips Heat |
| 5 | Cancel from each of the 8 live stages | `exit_actions`: heater 0, fan restored to `prior_fan_percent` | Any stage leaves a heater on or the fan at 100 |
| 6 | Cancel twice, cancel after Finished | Idempotence | A second cleanup is sent |
| 7 | Job starts mid-Cooling | Abandoned/JobStarted, no heater or fan command | We fight the print for the heater |
| 8 | Disconnect mid-Heating, then runner reconnect with target unchanged → heater off sent; with target 0 → nothing | Owed cleanup | A restarted Klipper gets a stray command, or a live one keeps a hot nozzle |
| 9 | Foreign target during Feed (target jumps to 250) | Abandoned/ForeignTarget, heater untouched, fan restored | We overwrite a user's choice |
| 10 | Each timeout: target never lands (20 s), heating (10 min), feed idle (8 min, reset by a tap), cooling (10 / 20 min no fan), pull hold (5 min) | Boundaries at T-1 s and T | A timeout fires early, late or never |
| 11 | Unhomed: start, Preparing emits no `run_park`, no gcode with X/Y/Z, no G28; `home_and_park()` with no park macro is ignored | No motion unhomed | Any move or home is emitted |
| 12 | Homed, no park macro: no motion emitted | Park only by macro | A hand-written move appears |
| 13 | Hold pressure: Cooling with floor 170 emits 1 mm every 3 s while current ≥ 185, none at 184, never again after; one in flight at a time | `kHoldPressureMarginC`, single in-flight | An extrusion is queued near `min_extrude_temp` |
| 14 | No part fan: no `fan_percent` in any action; cool timeout is 20 min | Fanless path | A fan command goes to an empty name |
| 15 | Multi-head (runner, mock ToolState with two heads, own fans): only head 2's heater and fan named in every send | Isolation | Head 1 is touched |
| 16 | Toolhead loaded on a filament-system printer: Preparing → Heating with nothing unloaded; Feed opens on ConfirmOutput (`Loaded`, 20 mm purge); after Pulled, `lane_needs_reseat()` holds through Done and a cancelled Repeat and clears when Repeat feeds it back (`Refeed`, 60 mm) | Using the loaded filament | The flow asks for a hand push of filament already seated, or forgets to warn that the lane reads loaded after the pull |
| 17 | Feed: sensor edge feeds without a tap; extrude while Feeding is ignored; command_finished(false) stays in Feed; Done works before, during and after any extrude | Embedded `GuidedLoadFlow` | Double feed, or a clogged head cannot reach the pull |
| 18 | Meter: Done sets `reset_meter`; Cancel, Abandoned never do | #1901 tie-in | Cancel resets the meter |
| 19 | `cold_pull_refusal()`: each refusal for Printing, Paused, Preparing, preheat pending, not ready, no heater, already running | Start gate | The flow starts during a job |
| 20 | `clamp_cold_pull_temps()`: soak above ceiling, soak below floor + 20, pull above soak - 60, pull below 40 | Edit bounds | A pull above the soak is accepted |
| 21 | `MaterialSettingsManager` round-trip, unset → defaults, corrupt values → defaults | Persistence | Edits are lost or garbage loads |
| 22 | `extrude_gcode(60, 300)`, `extrude_gcode(-10, 1200)`; filament panel extrude and purge fallback still emit their exact strings | DRY extraction | A caller's script changes |
| 23 | `GuidedLoadFlow` with empty flush and cleanup: `done()` returns "" and ends Finished; with cleanup present output unchanged | Join change | Existing guided loads send different gcode |
| 24 | Recovery: `key845` classification carries the `cold_pull` action; tapping it calls the registered flow, sends no gcode; `same_actions()` distinguishes two actions that differ only in `guided_flow` | Clog alert button | The tap sends an empty gcode |
| 25 | Overlay (UI test, `HelixTestFixture`): Suspended deactivation keeps the runner running; wake redraws the current stage; `on_dismissed` mid-Cooling cancels | Lifecycle | Screensaver aborts a run, or closing leaves it running |
| 26 | Runner destructor with a live flow sends `exit_actions` | Shutdown | App exit leaves a hot nozzle |

Each new test goes red once by hand on the line it pins before it is trusted.

## Printers that cannot do it, and why

- **Any printer while a job holds the machine.** Refused (`job_holds_machine`).
- **Toolheads the user cannot reach or grip.** The pull is by hand, out of the top of the
  extruder. Where the filament enters through an enclosed path with no bypass and no detachable
  tube (some filament-system toolheads with an inline cutter and no bypass), the user cannot feed
  or pull. Nothing reports this, so the flow cannot refuse it; the Prepare text says what has to
  be reachable, and the flow is offered everywhere else.
- **Extruders with no tension release that do not back-drive.** A geared extruder with no idler
  lever holds the filament; the pull then needs the tube removed and the filament pulled
  from below the gears, or partial disassembly. Out of scope for the flow's text beyond a
  "If the extruder has no lever, see your printer's manual" line.
- **Coated nozzles** (Prusa Nozzle X, some E3D coatings). The maker forbids it. Not detectable;
  warned at Setup.
- **Printers whose part fan the screen may not command** (firmware-driven `output_pin` fans).
  Not refused: they take the fanless path.
- **Toolchangers whose head cannot be mounted** (spools on the bed, filament system needs
  attention): the tool-change refusal ends the flow with its own text.
- **ESP32 builds.** Not excluded by any rule, but see Firmware size.

## Firmware size

`ColdPullFlow` is small and pure. The overlay is comparable to the PLR wizard, which the ESP32
build compiles (`firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`), and the two
reference pictures are the size risk. Proposal: draw the two tips with LVGL primitives (a cone
and a ragged cone with specks) in a small widget, so no image assets ship; a PNG pair would add
tens of KB to flash on every target. Open question 2.

## Open questions for Preston

1. **Cleaning filament pull temperature.** No maker publishes one. 100 °C (issue) or 140 °C
   (closer to FBRC8's 145 and to nylon, which most cleaning filaments resemble)?
2. **Reference pictures:** drawn tips (no assets, works on ESP32) or photographs (clearer, tens
   of KB each, and we need photos we may use)?
3. **Direct drive vs bowden text:** show both instructions on the Pull step, or ask once and
   persist the answer per printer (nothing in Klipper reports it)?
4. **Printer's own cold pull macro.** `StandardMacros` has no slot for it. Add a
   `StandardMacroSlot::ColdPull` with patterns `COLD_PULL`, `ATOMIC_PULL`, `NOZZLE_COLD_PULL`,
   and offer it as a secondary button on Setup? Or leave macros to the Macros panel?
5. **Filament panel entry point:** the filament panel has no overflow menu today. A row in the
   Advanced panel's tool group instead, or a new overflow on the filament panel?
6. **Hold pressure while cooling** (Prusa's step). On by default, or opt-in? It sends small
   extrusions the user did not tap for.
7. **Extruder-assisted pull** where `min_extrude_temp` from the config is already at or below
   the pull temperature (the printer owner allowed cold moves themselves): offer a "Pull with
   the extruder" button there, or always hand pull as the issue leans?
8. **Filament-system printers:** settled by Preston: use the seated filament, no unload. Whether
   Done should also offer a guided re-seat is open; today it is an instruction.
