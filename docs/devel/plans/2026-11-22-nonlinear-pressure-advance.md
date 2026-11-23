# Nonlinear pressure advance: show it, turn it on, tune it

Status: in progress on `feature/kalico-nonlinear-pa` (`.worktrees/kalico-nonlinear-pa`).
Decided with Preston 2026-11-22: drive the firmware's own tower (`PRINT_PA_TOWER` +
`TUNING_TOWER`), hide Save to Spool in nonlinear mode, prove it on the V0 after moving it
to Kalico `bleeding-edge-v2`.

## The signal

Classic pressure advance is one constant K: extruder lead is proportional to extrusion
velocity. A nonlinear model replaces the line with a curve that rises steeply at low
velocity and settles into a linear slope.

Only Kalico's `bleeding-edge-v2` branch has it (`klippy/kinematics/extruder.py`,
`PATanhModel` / `PAReciprModel`). Kalico main and mainline Klipper do not. No printer in
the fleet runs it today.

What the firmware publishes in the `extruder` status object:

| Key | Classic Klipper / Kalico main | bleeding-edge-v2, `linear` | bleeding-edge-v2, `tanh`/`recipr` |
|---|---|---|---|
| `pressure_advance_model` | absent | `"linear"` | `"tanh"` / `"recipr"` |
| `pressure_advance` | K | K | **absent** |
| `linear_advance`, `nonlinear_offset`, `linearization_velocity` | absent | absent | present |
| `time_offset` | absent | present | present |
| `smooth_time` | present | present | present |

So the presence of `pressure_advance_model` is the capability, and its value is the switch.

Commands (bleeding-edge-v2):

- `SET_PRESSURE_ADVANCE [EXTRUDER=] MODEL=linear|tanh|recipr ADVANCE= OFFSET= VELOCITY= TIME_OFFSET= SMOOTH_TIME=`
  switches the model at runtime, no restart. `ADVANCE` is K under `linear` and
  `linear_advance` under a nonlinear model. `OFFSET>0` requires `VELOCITY>0`.
- `PRINT_PA_TOWER NOZZLE= TARGET_TEMP= [SLOW_VELOCITY= MEDIUM_VELOCITY= FAST_VELOCITY= ...]`
  needs a `[pa_test]` section (every option defaults; the tower centres and sizes itself
  on the bed) and `virtual_sdcard`. It refuses unless the extruder's target equals
  `TARGET_TEMP`. It prints through `virtual_sdcard` as a gcode provider, so
  `print_stats` sees a print; it emits no layer markers.
- `TUNING_TOWER COMMAND=SET_PRESSURE_ADVANCE PARAMETER=ADVANCE|OFFSET|TIME_OFFSET START= FACTOR=`
  sets `START + z * FACTOR` on each new extrusion height. It stays armed until the next
  `TUNING_TOWER` or an extrusion lower than the last Z.

Kalico's procedure (`docs.kalico.gg/Nonlinear_Pressure_Advance.html`), starting from
`tanh`, all zero, `linearization_velocity` 1 (Bowden 2), `smooth_time` 0.02:

- Direct drive: OFFSET (judge the left side) → ADVANCE (left and front must look best at
  the same height; if left is lower, offset −10% and repeat, if higher +10%) →
  TIME_OFFSET (front only, subtle).
- Bowden: ADVANCE (front; keep 80%) → OFFSET (left vs front; adjust advance ±10%) →
  TIME_OFFSET → OFFSET again.
- Default factors at START=0: ADVANCE .001 (Bowden .01), OFFSET .01 (Bowden .02),
  TIME_OFFSET .0001.

## Shape

One capability module, `pa_model` (`include/pa_model.h`, `src/printer/pa_model.cpp`),
in the `zoffset` shape: no generic code names Kalico.

- `PaModel read_pa_model(const json& extruder_status, PaModel prev)`: model kind,
  `selectable` (the firmware reports a model), the parameters, `primary_value()`
  (K, or linear_advance).
- `enable_nonlinear_gcode(extruder, kind)`, `restore_linear_gcode(extruder, k)`.
- `tower_status_objects()` (`pa_test`), `tower_pass_gcode(pass, params)`.
- `config_edits(result)`: the `[extruder]` keys to write and `pressure_advance` to comment
  out (Klipper refuses an option the active model never reads).
- The pass plan: `next_pass(kind, history, verdict)` for both procedures above.

## Phase 1: correct display in nonlinear mode (Preston's item 3)

- `PrinterTemperatureState` keeps a `PaModel` per extruder; the active-extruder PA
  subject carries `primary_value()`; a new int subject `extruder_pa_nonlinear`.
- Print Tune labels the value "Linear advance" when nonlinear (XML binding).
- The classic tower on a nonlinear model: result labelled linear advance, Save to Spool
  hidden, no Moonraker publish (a slicer reading it would apply it as classic K).
- Spool activation skips auto-applying a saved K on a nonlinear extruder.
- Mock: `HELIX_MOCK_PA_MODEL=absent|linear|tanh|recipr`, the status keys above, and
  `SET_PRESSURE_ADVANCE MODEL=/OFFSET=/VELOCITY=/TIME_OFFSET=`.

## Phase 2: turn it on and tune it

- PA tuning setup, when `selectable` and the model is `linear`: a card offering
  nonlinear PA. Turning it on is runtime only (`MODEL=tanh`, zeros, velocity 1 or 2);
  abandoning restores `MODEL=linear ADVANCE=<original K>`.
- No `[pa_test]`: offer to add it (`KlipperConfigEditor::add_section` + restart via
  `safe_multi_edit`'s restart path) before turning the model on, since a restart resets it.
- A pass: heat as the classic tower does, `TUNING_TOWER ...`, then `PRINT_PA_TOWER`.
  The overlay logs the REPORTED parameter against toolhead Z (the BandLog, keyed by
  synthetic layers of `[pa_test] layer_height`), the user picks a height per side, the
  pass plan says what next.
- Save: `safe_multi_edit` on `[extruder]` with `config_edits()`, then restart.
- Mock: `pa_test` object, `PRINT_PA_TOWER` as a simulated print rising in Z,
  `TUNING_TOWER` applying `START + z * FACTOR`.

## Phase 3: hardware

V0 (192.168.1.133) to Kalico bleeding-edge-v2 once the ACE session releases
`device:voronv0` and Preston says go: back up `~/printer_data/config`, reflash the SKR
Pico, SHT36 and the host MCU (`klipper-mcu`), rerun the AFC DEV install so its extras
link into Kalico, check AFC, `autotune_tmc` and the macros still load. Then run both
procedures end to end. A `hw-verify` issue tracks it until done.

## Tests

`[pa_model]` (pure: status reading, gcode, config edits, pass plan), `[pa_tuning]`,
`[temperature_state]`, `[print_tune]`, `[spoolman]` touched cases. Each new test goes
red once by hand. One review, one screenshot per changed screen, one `land-gate` at the
finish.
