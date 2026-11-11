# Guided Cold Pull

A walkthrough that cleans a clogged nozzle: heat, feed, cool, pull by hand, inspect, then
repeat or finish (prestonbrown/helixscreen#2011). The screen heats the nozzle, cools it with the
part fan and says when to pull, which removes the step people get wrong: the pull temperature.

## Key files

| File | Role |
|---|---|
| `include/cold_pull_temps.h` | Materials, default soak and pull temperatures, edit bounds, the `kColdPullFlowId` guided flow id. Header-only, no LVGL |
| `include/cold_pull_flow.h`, `src/ui/filament/cold_pull_flow.cpp` | `ColdPullFlow`: the pure step machine. Every event, time included, returns the `ColdPullActions` to send. All stage, timeout and safety logic lives and is tested here |
| `include/cold_pull_runner.h`, `src/ui/filament/cold_pull_runner.cpp` | `ColdPullRunner`: app-lifetime, owned by `SubjectInitializer`. Feeds the flow the printer's events and a 1 s tick, applies its actions through `TemperatureController` and `IMoonrakerAPI`, publishes the `cold_pull_*` subjects, and owns the start refusal (`cold_pull_refusal()`) |
| `include/ui_cold_pull_overlay.h`, `src/ui/filament/ui_cold_pull_overlay.cpp`, `ui_xml/cold_pull_overlay.xml` | The overlay: a view over the runner's subjects, one view per stage |
| `src/ui/filament/ui_cold_pull_tip_sketch.cpp`, `ui_xml/components/cold_pull_tip.xml` | The clean and dirty reference tips at Inspect, drawn with LVGL primitives so no image ships |
| `src/system/material_settings_manager.cpp` | Persists the last material and each material's edited temperatures |

## Entry points

Every entry calls `open_cold_pull_overlay()`, which reopens Setup with the refusal `Start` would
meet and shows the overlay. Opening never heats; only `Start` does, and it checks the same refusal.

- **Advanced panel**, the Cold Pull row (`ui_xml/advanced_panel.xml`).
- **The clog alert.** `key845` ("Nozzle clog detected") carries a Cold Pull recovery action whose
  `RecoveryAction::guided_flow` is `kColdPullFlowId` and whose gcode is empty, beside an OK that only closes the alert (a blocking table code that offers a fix always keeps a plain way out). The runner registers
  the overlay as that guided device flow (`register_cold_pull_flow()`), so the alert, which lives
  below the UI layer, names the flow by id. `RecoveryFaultModal` hands the tapped action to
  `RecoveryModalPresenter::on_guided_flow_tapped()`, which marks the fault answered and opens the
  flow from the update queue, after the dialog has closed. An unregistered id opens nothing and
  sends nothing.

Any other alert whose fix is the cold pull gets the same action: give its table row a
`KlipperErrorAction` with `guided_flow = kColdPullFlowId`.

## Start refusals

`cold_pull_refusal()` checks, in order: a job holds the machine (`job_holds_machine()`, so
printing, paused and preparing all refuse), `FilamentOpHeater` has a preheat pending, Klippy is not
ready or the printer is not connected, the active tool has no heater, a pull is already running.
Setup shows the reason in red and `Start` stays refused. A clog reported mid-print therefore opens
Setup with "A print holds the printer." until the job ends.

## The procedure

The pull runs on the **active tool**: its heater, its part fan. Sources disagree by tens of
degrees, so every number is a default the user can edit at Setup, in 5 °C steps.

| Material | Soak (°C) | Pull (°C) | Evidence |
|---|---|---|---|
| PLA (default) | 230 | 90 | Bambu P1S soaks PLA at 230; Bambu X1 pulls PLA at 70-100; Prusa's manual procedure pulls at 100. 90 sits inside every range |
| Nylon | 250 | 140 | Community guides pull at 140-160 after a 240 soak. No vendor publishes a nylon number |
| Cleaning filament | 260 | 100 | Ultimaker soaks cleaning filament at 260. No maker publishes a pull temperature; FBRC8 pulls at 145 on the UM2, so 140 is the other candidate |

Edits are clamped by `clamp_cold_pull_temps()`: soak within
`[extrusion floor + kMinSoakAboveFloorC, max nozzle temp]`, pull within
`[kMinPullC, soak - kMinSoakPullGapC]`.

| Stage | What happens |
|---|---|
| Setup | Material, soak and pull, the hold-pressure option (off by default, and Setup says to leave it off for a clog), a warning not to cold pull a coated nozzle |
| Preparing | Park through the `ParkToolhead` standard macro, only when homed. "Home and park" homes only on the user's tap. The user unloads by hand unless a filament system has a lane seated in the head, in which case that filament is used |
| Heating | Soak target with `keep_previous_hot`, so the nozzle gets at least as hot as the last material |
| Feed | An embedded `GuidedLoadFlow`: push the filament until it stops (a toolhead sensor edge feeds without a tap), extrude, confirm clean output. Done is always available, because a clogged nozzle may extrude nothing |
| Cooling | Target set to the pull temperature, not 0, so the heater holds there; part fan at 100 %. Hold pressure, when chosen, sends 1 mm every 3 s while the nozzle is above floor + `kHoldPressureMarginC` |
| Pull | Entered on the measured temperature reaching the pull band after the nozzle was above it, never on the target change. Fan restored, display kept awake. The user releases the extruder tension and pulls by hand |
| Inspect | Heater off on entry. Reference tips, pass counter, Repeat (back to Heating) or Done. After a seated lane is pulled, the flow warns that the lane still reads loaded |

Timeouts (each ends Abandoned with the cleanup): target never lands, 20 s; heating, 10 min; feed
idle, 8 min; cooling, 10 min (20 without a part fan); waiting at Pull, 5 min.

## Safety rules

1. **One owner of the heater.** The runner sends every target through `TemperatureController`.
   A target on the head that differs from the last one sent by more than
   `kForeignTargetToleranceC` ends the flow Abandoned and leaves the heater to whoever set it.
2. **Every exit lands in `exit_actions()`.** Cancel, Abandoned, Done, closing the overlay mid-run,
   teardown (`before_disconnect()`), the runner's destructor and SIGTERM turn the heater off and
   restore the part fan, unless a job now holds the machine.
3. **The screensaver is not an exit.** The run belongs to the runner, not the overlay.
4. **A print takes the machine.** A job starting mid-flow ends it Abandoned with the heater and
   fan left to the print.
5. **Disconnect or Klippy shutdown** ends the flow Abandoned and keeps an owed cleanup: on the
   next ready connection, heater off only when the head's target still equals the one we sent.
6. **No motion on an unhomed machine** unless the user taps "Home and park". Extrusion is the only
   move the flow sends on its own.
7. **No cold moves.** Nothing is sent below `min_extrude_temp`: no `FORCE_MOVE`, no
   `SET_STEPPER_ENABLE`, no `M18`/`M84`, no `min_extrude_temp` edit, no cold-extrude toggle.

## Printers that cannot do it

- Any printer while a job holds the machine: refused.
- Toolheads the user cannot reach or grip (an enclosed filament path with no bypass or detachable
  tube). Nothing reports this, so the flow cannot refuse it.
- Geared extruders with no tension release that do not back-drive: the pull needs the tube removed
  or partial disassembly.
- Coated nozzles: the makers forbid it. Not detectable; Setup warns.
- Printers whose part fan the screen may not command (a firmware-driven `output_pin` fan) or have
  none: not refused, they cool without a fan and get the longer timeout.

## Not built

- A choice of head on multi-head printers: the pull runs on the active tool.
- Offering the printer's own cold pull macro: `StandardMacros` has no slot for one.
- A filament panel entry.
- The maintenance meter reset at Done: the flow asks for it (`ColdPullActions::reset_meter`), and
  the runner has no cold pull meter to reset yet.
- An extruder-assisted pull where the printer's config already allows cold moves.

## Tests

`[cold_pull]`: `tests/unit/test_cold_pull_flow.cpp` (every stage, timeout and refusal),
`tests/unit/test_cold_pull_runner.cpp` (what reaches Moonraker, owed cleanup, teardown), and the
clog alert's path in `tests/unit/test_recovery_modal_presenter.cpp`.
