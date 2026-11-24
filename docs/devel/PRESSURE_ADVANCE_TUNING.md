# Pressure Advance Tuning

Tuning Klipper's pressure advance (K) by eye on a live test print, plus a plain live
K stepper in the print Tune overlay. Issue: prestonbrown/helixscreen#982. The automatic,
firmware-measured calibration is a separate screen (`include/ui_panel_calibration_pa.h`,
`helix::pacal`); HelixScreen applies nothing from it, the printer does or you copy the value to the slicer.

## Key files

| File | Role |
|------|------|
| `include/pa_tuning.h`, `src/printer/pa_tuning.cpp` | Pure logic: tower geometry, gcode generator, ramp, `BandLog`, `TuningSession` state machine, Moonraker record merge |
| `include/ui_overlay_pa_tuning.h`, `src/ui/calibration/ui_overlay_pa_tuning.cpp`, `ui_xml/pa_tuning_overlay.xml` | The tuner: setup, running dial, band picker, result |
| `src/print/tune_controller.cpp#set_pressure_advance` | The one send path, shared by the tuner, the Tune overlay row and spool activation |
| `ui_xml/components/tune_pa_row.xml` | The Tune overlay's live K row, both orientations |
| `src/printer/printer_temperature_state.cpp` | Parses `extruder*.pressure_advance` / `smooth_time`; `extruder_pressure_advance` subject (K x 10000, -1 unknown) |
| `src/api/moonraker_spoolman_api.cpp#set_spoolman_filament_extra_number` | Writes the Spoolman filament extra field, defining it first |
| `src/printer/spoolman_active_spool_sync.cpp#apply_saved_pressure_advance` | Applies a spool's saved K when it becomes active |

## How a run works

1. Advanced > Pressure Advance Tuning. Pick tool (multi-tool printers), material
   preset (nozzle and bed temperature come from `presets::slot_temps()`), extruder kind
   (direct drive sweeps 0 to 0.1, bowden 0 to 1.0) and edit the start and end K.
2. `pa::generate_test_gcode()` writes a 40 mm hollow square tower centred on the bed
   (`toolhead.axis_minimum/maximum` in gcode space): two walls at 100 mm/s, 0.2 mm
   layers (line width from `[extruder] nozzle_diameter`), a 1 mm base, ten 3 mm bands, a
   brim and a prime line (skipped when the start macro already purges, per
   `PrintStartAnalyzer`). It starts through the printer's own start macro, found by
   `pa::pick_machine_macros()` among `PrintStartAnalyzer::kMacroNames` in the discovered
   macro set, passing bed and nozzle temperatures under the parameter names that macro
   takes (`BED_TEMP`/`BED`/..., `EXTRUDER_TEMP`/`EXTRUDER`/...). A macro that takes no
   recognisable temperatures is called bare with the heaters already started; with no
   start macro the file homes and heats itself. `M190`/`M109` follow either way. It ends
   through the matching end macro (`kEndMacroNames`) or a plain cooldown. It reports
   layers with `SET_PRINT_STATS_INFO` and never sets K. It is uploaded
   to the gcodes root as `helix_pa_tuning.gcode` (one fixed name, overwritten each run)
   and started.
3. HelixScreen drives K: when `print_stats.info.current_layer` enters a new band it sends
   `SET_PRESSURE_ADVANCE EXTRUDER=<tuned extruder> ADVANCE=<k>`. Driving it from the
   host rather than a `TUNING_TOWER` command in the file is what lets the user take over
   at any moment.
4. The tuner raises itself over the auto-opened Print Status and lists the bands printed
   so far (newest first) under the K readout. A -/+ tap (step 0.001,
   0.005, 0.01 or 0.05) stops the ramp for the rest of the print, so the value the user
   is judging is not moved away under them.
5. Each layer records the K the printer **reported** (`extruder*.pressure_advance`, the one
field added to the extruder subscription),
   whoever changed it, so the log is the truth even when a console command or a macro
   interfered.
6. When the print ends by any route (completion, End Test, error) the picker shows the
   log merged into bands (height range -> K). Pick one or keep the current value. The K
   the run started with is put back the moment the print ends, so a cancelled tower never
   leaves a test K live; only an accepted band changes it. A run that printed no layer,
   or one the user discards, also restores the K it started with.
7. The result is applied live and shown with a note to turn pressure advance off in the slicer's
   filament profile. Save for <material> stores it per material (below). Save to Config writes `pressure_advance` into the extruder's section (the
   same `KlipperConfigEditor` route the nonlinear tuner saves through, with a Klipper
   restart). Save to Spool writes it to the active spool's Spoolman filament. It is always
   published to Moonraker's database (below).

The run belongs to the overlay object, not its widget tree: backing out to Print Status
keeps the ramp going, and the Tune overlay shows a Tuner button while a run is active.

## Rules the run keeps

- **Heads, not tools.** The tool chips are `ToolState::heated_heads()`; lanes feeding one
  hotend are one head. A tool is selected (`T<n>`, the head's representative tool, so
  sparse numbering works) only with `has_multiple_extruders()`. The nozzle is heated by
  heater name (`SET_HEATER_TEMPERATURE` / `TEMPERATURE_WAIT`) and is hot before any
  `T<n>`.
- **Original K.** The run records the extruder's reported K at start (unknown stays
  unknown: no restore). The end gcode reports a layer past the last, which the session
  treats as the end of logging, then sets the original K back before the end macro;
  Keep current re-sends the K the tower ended on. Both usually arrive in one status
  frame, and PrinterState parses the extruder before print_stats, so the restore can be
  seen while the top layer is still current: an unasked report of the original K on the
  top layer ends logging the same way.
- **The file is spent.** When the tower's print ends by any route, or fails to start,
  `helix_pa_tuning.gcode` is deleted from the printer; history then marks the job's
  file as gone and offers no reprint, which would restore a stale K.
- **One question before a tower.** Both tuners ask "Print the test tower?" and nothing
  else (`confirm_tower_print()`, `include/pa_tower_start.h`); Start marks the bed cleared.
  An empty toolhead refuses the tower first (`refuse_empty_toolhead()`, from the head's own
  `head_has_filament()` or the toolhead sensors) and offers the Filament panel; an unknown
  state never blocks.
- **No K before layer 1.** The start macro is still heating until the first layer reports,
  and a `SET_PRESSURE_ADVANCE` sent earlier waits behind its `M190` and times out. The
  ramp's first K goes out with layer 1.
- **A nonlinear extruder refuses the classic tower**: a bare `ADVANCE=` replaces only
  `linear_advance` and keeps the tuned offset. The setup steers to the Nonlinear tuner.
- **Done** leaves the tower's Print Status page off the stack, so it returns to where the
  tuner was opened and no Reprint of the tower stays reachable.
- **Foreign prints.** Outside the run's own print, nothing sends K while
  `job_holds_machine()`: a pick, a discard or a restore never lands in another job.
- **Printable area.** `pa::printable_area()`: the `[bed_mesh]` plate within the axes, so
  a negative `position_min` for a purge bucket never carries the prime line off the plate;
  on a delta the square inscribed in the reachable circle.
- **Start.** The uploaded file starts through `PrintStartController::initiate_reprint()`,
  the path any file already on the printer takes, with an explicit identity routing for
  the tuned tool (`initiate_reprint_with_routing()`), so a U1's pre-print send puts the
  tower on the head that was heated rather than the last job's crossover; a cooldown
  follows the end macro. Known gap: it does not go through
  `PrintPreparationManager::start_print()`, so that path's file scan, pre-print options,
  AMS remap and spools-on-bed checks do not run for the tower.
- **Bands.** More bands than rows are merged pairwise where K differs least
  (`pa::fit_bands()`), so late nudges stay pickable.

## Persistence

### Per material

`helix::material_pa` (`include/material_pa.h`, tests `tests/unit/test_material_pa.cpp` tag
`[material_pa]` and `tests/python/test_material_pa_macro.py`) is what both tuners' **Save for
<material>** writes, through `MaterialZOffsetState::save_pa()`: `SAVE_VARIABLE` lines into
`[save_variables]`, then `helix_material_pa_enabled` 1. Nothing restarts.

| Variable | Linear entry | Nonlinear entry |
|---|---|---|
| `helix_pa_<key>_model` | `"linear"` | `"tanh"` / `"recipr"` |
| `helix_pa_<key>_advance` | K | `linear_advance` |
| `_offset`, `_velocity`, `_time_offset`, `_smooth_time` | not written | written |

The key is the Z offset macro's (`material_zoffset::helix_key()`), since `HELIX_MATERIAL_OFFSET`
builds both and passes it as `_HELIX_MATERIAL_PA KEY="<key>"`, only while the switch is on, so
the off path sends nothing new. The macro drops `" ' \ # ; *` from the material and turns a tab
into a space first: Klipper's parser cuts arguments at `#`, `;` and `*`, and a tab splits one. An
empty material has no entry. A save clears the model (`""`) first and writes it last, and the
macro uses an entry only with a model it can run, so a save that stops partway is never used.

`_HELIX_MATERIAL_PA` (`assets/config/helix_macros.cfg`) sends one `SET_PRESSURE_ADVANCE EXTRUDER=
<active extruder>` naming every parameter the firmware has: `MODEL`, `ADVANCE`, `OFFSET`,
`VELOCITY`, `TIME_OFFSET`, `SMOOTH_TIME` under a nonlinear model; `MODEL=linear ADVANCE
TIME_OFFSET SMOOTH_TIME` on firmware that reports `pressure_advance_model`; `ADVANCE SMOOTH_TIME`
on classic firmware. Every parameter must be named because Kalico's `cmd_SET_PRESSURE_ADVANCE`
builds a fresh model on a model change and `PANonLinearModel.update` keeps any one left out. A
parameter the entry does not carry (all but `advance` for a linear entry) comes from
`configfile.settings[<extruder>]`, so a linear entry never picks up a nonlinear entry's leftovers.
A material with no entry gets the config's values, not the live ones a slicer left behind. So
does an entry whose model the firmware cannot run (unknown, empty, or nonlinear on firmware
without models), with a console line: sending nothing would leave the previous print's material
value live. `applied` records the entry used, or `CONFIG`.

Offered (`material_pa_ready`) only where the HelixScreen macro row of Z offset by material
matches, the printer has `[save_variables]`, and the pack defines `_HELIX_MATERIAL_PA`
(`material_zoffset::pressure_advance_ready()`); the pack's version rung for 2.4.0 is that macro.
The per-printer setting `material_pa/enabled` is off by default, a Save turns it on, and the
Settings row **Pressure advance by material** turns it off; `reconcile()` corrects the printer's
switch to match when Settings opens.

**Never on more than one extruder.** `pressure_advance_ready()` is false when the printer has an
`extruder1`, and the macro does nothing when `configfile.settings` has one. The slicer's
material is the initial tool's, `PRINT_START` usually runs before that tool's `T<n>`, and neither
tuner records which extruder it tuned, so a per-material value would land on whichever tool was
active. Per tool needs the entry to carry the extruder and the macro to run at tool pickup.

The nonlinear tuner's procedure and its saved session belong to one head, extruder kind and
material (`PaNonlinearOverlay::start`, `saved_session`): opening it for another material starts a
fresh procedure, so Save for <material> never stores one material's passes under another's name.
The classic tuner's Save for <material> follows `pa_tune_can_save_material`, 0 whenever the
extruder runs a nonlinear model (the result is then a `linear_advance`, and a linear entry would
switch the extruder to `MODEL=linear` at print start).

Limits: the time offset is stored per material, though it may be a property of the machine. Firmware that owns the Z offset (ZMOD,
Forge-X, Helper-Script) and printers on the `_START_PRINT_VARS` row get no per-material pressure
advance: neither calls HelixScreen's macro.

### Spoolman

Filament extra field `pressure_advance` (`kSpoolmanPaExtraKey`), one per filament: K
belongs to the material and the extruder, not to a single spool. Spoolman refuses an
extra key with no field definition, so the first save defines it as a `float` field
named "Pressure advance" (`POST /v1/field/filament/pressure_advance`). A field the user
already defined is written in its own type (a `text` field gets a quoted string). Older
Spoolman (0.22 among them) replaces a filament's whole `extra` dict on `PATCH`, newer
releases merge per key; the write reads the filament first and sends the merged dict, so
both keep the user's other extra fields. The mock replaces, the stricter behaviour.

When a spool becomes active (`notify_active_spool_set`, never the startup sync) and its
filament carries a K, HelixScreen applies it to the extruder whose tool holds that spool
(`pa::extruder_for_spool()`; the active extruder otherwise), **unless a print is running**.
Save to Spool writes to the tuned tool's spool when tools carry spools, else Spoolman's
active spool. A slicer that sets PA in its start gcode (OrcaSlicer does when the filament
profile enables pressure advance) runs after the activation and wins, and an AMS swap
mid-print never overrides the slicer.

### Moonraker database

Namespace `helix-screen`, key `pressure_advance`. Readable by any client with
`GET /server/database/item?namespace=helix-screen&key=pressure_advance`:

```json
{
  "version": 1,
  "last":      { "k": 0.0421, "extruder": "extruder", "material": "PLA", "nozzle_temp": 210,
                 "spool_id": 3, "filament_id": 7, "filament_name": "Polymaker Jet Black PLA",
                 "updated": "2026-10-16T20:00:00Z" },
  "extruders": { "extruder": { "...same shape..." } },
  "filaments": { "7": { "...same shape..." } }
}
```

`last` is the latest result, `extruders` the latest per Klipper extruder, `filaments`
the latest per Spoolman filament id (only results taken with an active spool). Entries
for other extruders and filaments are kept on every write.

## OrcaSlicer

What OrcaSlicer reads today (checked against `OrcaSlicer/OrcaSlicer` main, 2026-10):

- No Spoolman integration in the slicer itself (the only implementation, PR #4771, is
  still open and carries PA only inside its own serialized preset blob). A Spoolman extra
  field does not reach Orca on its own.
- The path that works today is [spoolman2slicer](https://github.com/bofh69/spoolman2slicer),
  which turns Spoolman filaments into Orca filament presets. Its default templates already
  read a filament-level extra field named `pressure_advance`, which is why HelixScreen
  writes exactly that key.
- Orca's Moonraker printer agent (`MoonrakerPrinterAgent::fetch_moonraker_filament_data`)
  reads the Moonraker database namespace `lane_data` (`color`, `material`, `bed_temp`,
  `nozzle_temp` per lane). No pressure advance.
- Filament settings `enable_pressure_advance` (default off) and `pressure_advance`. When
  enabled, Orca emits `SET_PRESSURE_ADVANCE ADVANCE=<k> EXTRUDER=...` at print start and
  on tool changes, overriding whatever the printer had. HelixScreen's activation apply runs
  before the print and never during one, so what the slicer sends wins for that print.
- Python plugins (merged in PR #14530, 2.5.0-alpha) get **read-only** preset access
  (`orca.host` `config_value` / `current_filament_presets`); writing presets is only
  proposed (PR #16518). Network access goes through a per-plugin permission prompt.

So a plugin today can only read the Moonraker key above and show it next to the
filament's `pressure_advance`. Once presets become writable it needs: the printer's
Moonraker URL from the physical printer preset, one `GET` of the key, a match from the
selected filament preset to `filaments[<id>]` (by Spoolman filament id if the preset
records one) or `extruders[<name>]` / `last`, and a write of `pressure_advance` with
`enable_pressure_advance = true` into that preset.

## Nonlinear models (Kalico bleeding-edge-v2)

Kalico's `bleeding-edge-v2` branch adds selectable models (Kalico's extruder module,
`PATanhModel` / `PAReciprModel`); Kalico main and mainline Klipper have none. The capability is
read from the `extruder` status object, behind `pa_model` (`include/pa_model.h`), so no
generic code names the firmware:

| Key | Classic | `linear` | `tanh` / `recipr` |
|---|---|---|---|
| `pressure_advance_model` | absent | `"linear"` | `"tanh"` / `"recipr"` |
| `pressure_advance` | K | K | absent |
| `linear_advance`, `nonlinear_offset`, `linearization_velocity` | absent | absent | present |
| `time_offset`, `smooth_time` | `smooth_time` only | present | present |

- `PaModel::primary_value()` is what `SET_PRESSURE_ADVANCE ADVANCE=` sets: K, or
  `linear_advance`. `PrinterTemperatureState` keeps a model per extruder; the
  `extruder_pressure_advance` subject carries the primary value and `extruder_pa_nonlinear`
  relabels the Tune row.
- Under a nonlinear model a classic result is never saved to a spool, published, or
  auto-applied on spool activation (`pa::should_auto_apply_on_activation`).
- `SET_PRESSURE_ADVANCE MODEL=` switches models at runtime. The firmware keeps any parameter a
  command leaves out, so `pa::restore_model_gcode` names every one.

### The guided tuner

`PaNonlinearOverlay` (`src/ui/calibration/ui_overlay_pa_nonlinear.cpp`) runs
`pa::NonlinearProcedure` (`include/pa_nonlinear_tuning.h`), Kalico's documented direct-drive
and Bowden orders, on the firmware's own tower (`[pa_test]`, `PRINT_PA_TOWER`). Each pass sends
`pa::machine_ready_lines` (shared with the classic tower), the model switch, `TUNING_TOWER
COMMAND=SET_PRESSURE_ADVANCE PARAMETER=<p> START=0 FACTOR=<f>` and `PRINT_PA_TOWER NOZZLE=
TARGET_TEMP=`. The tower prints through `virtual_sdcard` as print_stats file `PA tower`.

Firmware behavior the overlay is built around:

- **A sweep outlives the tower.** `TUNING_TOWER` has no end command: it stays armed until the
  next `TUNING_TOWER` or an extrusion more than 2 mm below the last height. Every exit sends
  `pa::disarm_tuning_tower_gcode()`, a constant zero dwell that runs once and never again.
- **The temperature check runs late.** `PRINT_PA_TOWER` returns before the tower's generator
  checks `TARGET_TEMP`, so a refusal is a print that ends in error, possibly with no
  `printing` frame. After the command returns, a terminal state for `PA tower` ends
  PREPARING, and a tower not printing within `kTowerStartTimeoutMs` gives up.
- **Save** writes `pa::nonlinear_config_keys` into the extruder's section through
  `KlipperConfigEditor::safe_multi_edit` and comments out `pressure_advance`, which Klipper
  refuses once nothing reads it. A `pressure_advance` in another file's `[extruder]` is not
  reached; the error then says so.

Not yet verified on hardware: nothing in the fleet runs bleeding-edge-v2.

## Mock

`--test` handles `SET_PRESSURE_ADVANCE` per extruder (`MoonrakerClientMock::
pressure_advance_for`), reports `pressure_advance`/`smooth_time` in status, keeps an
uploaded gcode file on disk so the print reads its own layer count and estimate, and its
Spoolman serves `/v1/field/filament` and filament extra values. The seeded mock spools
carry no filament id, so Save to Spool stays disabled there unless a spool with one is
active.

`HELIX_MOCK_PA_MODEL=linear|tanh|recipr` makes the mock a model-aware firmware, and
`HELIX_MOCK_PA_TEST=1` adds `[pa_test]` with `TUNING_TOWER` and a simulated `PRINT_PA_TOWER`
(`src/mock/moonraker_client_mock_pa.cpp`; `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`). The mock
serves no config files, so the tuner's Save fails there with "Section [extruder] not found".
