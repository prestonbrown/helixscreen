# Pressure Advance Tuning

Tuning Klipper's pressure advance (K) by eye on a live test print, plus a plain live
K stepper in the print Tune overlay. Issue: prestonbrown/helixscreen#982. The automatic,
firmware-measured calibration is a separate screen (`include/ui_panel_calibration_pa.h`,
`helix::pacal`); HelixScreen applies nothing from it, the printer does or you copy the value to the slicer.

## Key files

| File | Role |
|------|------|
| `include/pa_tuning.h`, `src/printer/pa_tuning.cpp` | Pure logic: tower geometry, gcode generator, ramp, `BandLog`, `TuningSession` state machine, Moonraker record merge |
| `include/ui_overlay_pa_tuning.h`, `src/ui/ui_overlay_pa_tuning.cpp`, `ui_xml/pa_tuning_overlay.xml` | The tuner: setup, running dial, band picker, result |
| `src/ui/print/tune_controller.cpp#set_pressure_advance` | The one send path, shared by the tuner, the Tune overlay row and spool activation |
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
   brim and a prime line. It starts through the printer's own start macro, found by
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
   log merged into bands (height range -> K). Pick one or keep the current value. A run
   that printed no layer, or one the user discards, restores the K it started with.
7. The result is applied live and shown with a note to enter it in the slicer's filament
   profile. Save to Spool writes it to the active spool's Spoolman filament. It is always
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

## Mock

`--test` handles `SET_PRESSURE_ADVANCE` per extruder (`MoonrakerClientMock::
pressure_advance_for`), reports `pressure_advance`/`smooth_time` in status, keeps an
uploaded gcode file on disk so the print reads its own layer count and estimate, and its
Spoolman serves `/v1/field/filament` and filament extra values. The seeded mock spools
carry no filament id, so Save to Spool stays disabled there unless a spool with one is
active.
