# Drying Filament on the Heated Bed

Spools lie on the build plate under a cover box while the bed heats, optionally with a
chamber appliance dryer running alongside (prestonbrown/helixscreen#1730). The procedure
is the usual one for heated-bed drying: unload,
clear the plate, move it as far from the nozzle as the printer allows, spools on the
plate under a box, flip them midway, let the bed cool before removing them.

## Key files

| File | Role |
|---|---|
| `include/bed_drying.h` | Every decision as a pure function: enclosure, availability, clearance Z, the material table and its cap, the unload offer, run timing, the cool-down gate, the persisted `RunRecord` |
| `include/bed_drying_controller.h`, `src/ui/filament/bed_drying_controller.cpp` | `BedDryingController`: the run itself (prepare, confirm_placed, tick, stop, confirm_removed, restore) and its subjects |
| `include/ui_bed_drying_modal.h`, `src/ui/filament/ui_bed_drying_modal.cpp` | The start modal and the prompts around it (unload offer, place, stop, remove) |
| `include/spool_latch_gate.h` | The send-layer allowlist |
| `src/api/moonraker_gcode_guards.cpp` | `reject_motion_while_spools_on_bed`, `reject_job_while_spools_on_bed` |
| `ui_xml/bed_drying_modal.xml`, `ui_xml/components/bed_drying_banner.xml` | Start modal; the banner above every panel |

## Who may run it

`printer_can_bed_dry` = heated bed, enclosed, and at least 130 mm of Z travel
(`bed_drying::available`). Open printers never offer it: the feature is hidden, with no
warn-and-allow path.

`printer_is_enclosed` comes from `bed_drying::is_enclosed`: the `"enclosed": true` key in
`assets/config/printer_database.json` (`PrinterDetector::is_enclosed`), else a configured
chamber heater. A chamber sensor alone proves nothing. The `EnclosureStyle` setting (Auto,
Enclosed, Open; Settings > Printing > Enclosure) exists so an owner can mark a DIY
enclosure; `PrinterState::refresh_bed_drying_capability` resolves both subjects whenever an
input changes.

Entry points: the **Dry Filament** button on the bed card of the temperature overlay, and
the **Dry Filament** row in Advanced. Both carry `moves_machine="true"`.

## The flow

1. **Unload offer.** `bed_drying::unload_offer` over `BedDryingController::toolhead_loaded`:
   a toolhead sensor saying empty skips the offer; a sensor or filament system saying
   loaded makes it the recommended action; otherwise it is offered beside a "make sure no
   filament is loaded" line. The unload runs the `UnloadFilament` standard macro. Skippable.
2. **Home** if any axis is unhomed, with the bed still empty.
3. **Clearance move** to the G-code-space Z max minus 20 (`bed_drying::clearance_z`), then
   the toolhead parks 10mm inside the rear of the plate (`helix::preset_area()`: the
   `[bed_mesh]` area, else travel, so never into tool docks past the plate), then `M400`.
   On a bed-moving printer that puts the plate 20mm above the bottom, clear of anything
   under it; on a gantry-moving one, the nozzle near the top. Either way a cover box fits.
4. **Latch on and persist** (`BedDryingController::begin_placement`), then the place
   prompt: clear above and below the plate, spools on the plate, cover with a box,
   close the door. Spools can land on the plate from the moment the prompt opens, so
   only its **No spools placed** button clears the latch; any other dismissal keeps it
   and asks for the removal confirmation. The idle timeout is held from here on.
5. **Start** (`confirm_placed`): the run is saved before any heat; a failed save
   refuses to heat, with a toast.
6. **Heat**: the bed at the material's value from the heated-bed table in `bed_drying.h`, capped at 90°C
   and at the bed max (`bed_drying::bed_temp_c`); the chamber dryer too when chosen, at
   the material's **air** temperature, a dryer-box figure (PLA 50°C, not
   the bed's 70), clamped by the dryer and started with `hold_idle=false`; Klipper's idle
   timeout held to the planned end plus 10 minutes.
7. **Run**: HelixScreen's 1 s timer. At the midpoint a "flip the spools" notification.
8. **End** (planned end, Stop, or the bed target dropping to 0): bed off, dryer stopped,
   and the idle timeout re-held for 24 h (`kSpoolsOnBedHoldS`). The latch stays. With
   the heat off, the timeout's only effect would be its `M84`, which lets a gantry sink
   onto the spools, so the configured value comes back only at removal.

   The hold is bounded at 24 h. If removal is never confirmed, Klipper's idle
   timeout fires then: the heaters are already off, but its `M84` runs and releases
   the steppers. The latch still refuses homing, moves and print starts from
   HelixScreen. At removal the configured value comes back, or Klipper's default 600 s
   when it was never read.
9. **Cool-down**: the removal prompt waits for the bed to read below 40°C. Tapping the
   banner earlier offers the removal behind a hot-plate warning.
10. **Remove**: only its confirm clears the latch and the persisted run, and restores the
    configured idle timeout.

Running the bed and a chamber appliance together is deliberate: the appliance adds
chamber heat the bed alone cannot.

## The latch

`PrinterPrintState::set_spool_latch` holds the latch (an atomic the send layer reads from
any thread) and the extra allowlist tokens a running dryer needs.

**Every app-originated motion or print start leaves through one of five places**, and the
latch gates all five:

| Choke point | Carries |
|---|---|
| `MoonrakerAPI::execute_gcode` | every `IMoonrakerAPI::execute_gcode` caller: macros, console, calibration, AMS ops, PRINT_START |
| `MoonrakerMotionAPI::execute_gcode` | all of `IMotionAPI`: jog pad, hold-to-repeat, keypad, homing |
| `MoonrakerJobAPI::start_print` / `start_modified_print` | print select, detail view, reprint |
| `MoonrakerJobAPI::resume_print` | resume, PLR resume |
| `MoonrakerQueueAPI::start_queue` | the job queue |

The gcode gates are an **allowlist**, not a denylist: a macro can home without saying
`G28`, so while latched a script passes only if every line's first token is a heater,
fan, light, read-only, `SET_IDLE_TIMEOUT` or `M112` command, or one of the
dryer's own tokens (`TemperatureController::chamber_dryer_tokens`). `M84`,
`FIRMWARE_RESTART` and `RESTART` are refused: each releases the steppers. The
`printer.restart` / `printer.firmware_restart` RPCs, a `klipper` service restart and the
recovery chain in `PrinterRecoveryService::recover` refuse too. `M112` always passes.

**The allowlist trusts command names.** It matches first tokens, so a macro that
overrides an allowlisted name (an `M140` wrapper that homes, a `SET_LED` that parks) runs
while latched. No stock or vendor macro known to HelixScreen does that; a user macro can.

Probe commands (`probe_send_gcode` in `ui_probe_overlay.cpp`) go through
`IMoonrakerAPI::execute_gcode` and so through the gate. The remaining direct
`gcode_script` senders move nothing: batch-feed interlock END, `TURN_OFF_HEATERS` at print
completion, `M300` beeps, and settings queries.

**Buttons**: `moves_machine="true"` binds disabled to `machine_motion_blocked`
(`job_holds_machine || spool_latch`), in `lv_xml_obj_parser.c`. `job_holds_machine`
keeps its meaning, "a print owns the machine", for its C++ readers. The gates are the
guarantee; the disabled buttons are the courtesy.

**What the latch cannot stop**: Mainsail, a macro run from elsewhere, a print sent from a
slicer. Stock Klipper has no runtime command that refuses a Z move or homing:
`SET_KINEMATIC_POSITION` lies about the position, `SET_STEPPER_ENABLE` refuses nothing,
`force_move` only enables moves, and `rename_existing` is config-only. The start modal
says so. An opt-in `[gcode_macro G28]` guard in `helix_macros.cfg` would cover homing
only and collides with vendor `G28` wrappers; it is future work.

## Subjects

`bed_drying_state` (the `State` int), `bed_drying_text`, and, while a cycle is `Running`,
`bed_drying_remaining_min` (minutes left, rounded up) and `bed_drying_progress_pct` (0..100).
Outside a running cycle both of the last two read -1. The Dryer home widget draws its bed row
from them (`src/ui/panel_widgets/dryer_model.cpp`). The percentage is `progress_pct_from()` over
the run record's duration, the same function the box dryers use; how the three drying sources are
folded into one summary is in [FILAMENT_ENVIRONMENT_ZONES.md](FILAMENT_ENVIRONMENT_ZONES.md#the-dryer-summary).

## Persistence and interruptions

The run is a `RunRecord` under `bed_drying` in settings.json
(`SettingsManager::set_bed_drying_record`), written and saved at once.
`BedDryingController::restore` runs from SubjectInitializer, before any panel exists.

| Event mid-run | What happens |
|---|---|
| HelixScreen restart | The latch returns at once. The run advances only once Klipper reports ready (a restore runs before Moonraker connects, and sends made then go nowhere): before the planned end the timer resumes from the stored end time, after it the end runs on the first ready tick |
| Klipper restart | Heater targets drop to 0; once the bed target was seen at the run's value, a 0 ends the run. The latch stays |
| Power loss | Klipper comes back cold and unhomed; HelixScreen comes back latched, so its first homing is refused until the spools are confirmed out |
| HelixScreen dies for good | The held idle timeout fires 10 minutes after the planned end and Klipper's own `TURN_OFF_HEATERS` ends the heat (measured on the U1: it does end a heater run) |

Idle timeout's `M84` also releases the motors. A gantry on belts or non-self-locking
lead screws can sink with it; on a gantry-moving printer that lowers the nozzle toward
the spools. The dead-man path is a backstop, not the plan.

## Mock

Under `--test` the run's clock follows `--sim-speed`, so a 12 h run plays out in minutes;
the mock's homing, moves and heaters behave as for any other flow.
