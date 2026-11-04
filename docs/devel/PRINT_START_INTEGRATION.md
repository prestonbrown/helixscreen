# Print Start Phase Detection

HelixScreen detects when your print's preparation phase (heating, homing, leveling, etc.) is complete and actual printing begins. This document is the user/modder side of that: making your setup legible to HelixScreen with macros and slicer options, controlling pre-print operations per print, and troubleshooting a preparation phase that misbehaves.

**Developer guide**: how detection works internally - the observer pipeline and its five signal sources, the evidence kinds, the JSON profile schema, and how to author a profile for a new printer - is [PRINT_START_PROFILES.md](PRINT_START_PROFILES.md); the ETA engine is [PREPRINT_PREDICTION.md](PREPRINT_PREDICTION.md).

## How It Works

When a print starts, HelixScreen shows a "Preparing Print" overlay on the home panel with a progress bar showing the current phase (homing, heating, leveling, etc.). Once the preparation is complete, the UI transitions to show normal print status.

### When detection starts: two arming paths

The detection window opens at one of two moments, and which one it is matters for
everything below.

| Path | Opens when | Covers |
|---|---|---|
| **Commit-armed** | The user presses Print on the screen | Any host-side pre-start work *plus* the file's own `PRINT_START` |
| **Printer-edge** | `print_stats` transitions into `printing` | The file's own `PRINT_START` only |

A print started from Mainsail, Fluidd, or the printer's own panel has no commit for
HelixScreen to observe, so it always takes the printer-edge path. That path is
unchanged and must stay working - it is the only one an externally started print has.

The distinction exists because some pre-print work runs *in front of* the job rather
than inside it. An optional forced bed mesh, or a printer-level setup macro, is
dispatched by HelixScreen as a blocking `gcode_script` before `start_print` is ever
called. Klipper reports `print_stats=complete` (describing the *previous* job) for
that entire time. On a K2 Plus that window is around 455 seconds. Commit arming is
what lets the UI describe it instead of rendering stale job data.

Consequence for timing history: the two paths measure different quantities, so
recorded pre-print durations are bucketed by which window they came from and are
never averaged together. See [PREPRINT_PREDICTION.md](PREPRINT_PREDICTION.md).

HelixScreen uses a modular profile system with printer-specific signal matching. Known printers (like the FlashForge AD5M) get accurate, per-phase progress tracking. Unknown printers use generic pattern matching that works with standard G-code commands.

Detection uses a multi-signal system with these fallbacks for completion:

| Priority | Signal | How It Works |
|----------|--------|--------------|
| 1 | **Macro Variables** | Watches `gcode_macro _HELIX_STATE.print_started` or `gcode_macro _START_PRINT.print_started` |
| 2 | **G-code Console** | Parses console output for `HELIX:READY`, `LAYER: 1`, `;LAYER:1`, `First layer`, or `SET_PRINT_STATS_INFO CURRENT_LAYER=` |
| 3 | **Layer Count** | Monitors `print_stats.info.current_layer` becoming ≥1 |
| 4 | **Progress + Temps** | Print progress ≥2% AND temps within 5°C of target |
| 5 | **Timeout** | Adaptive; see Troubleshooting below. Never a flat 45s. |

## Making Your Setup HelixScreen-Friendly

### Option 1: Install HelixScreen Macros (Recommended)

The HelixScreen macro file provides:
- **Instant detection**: `_HELIX_READY` signal at the end of your PRINT_START
- **Phase tracking**: Optional `_HELIX_PHASE_*` macros for detailed progress display
- **Pre-print helpers**: `HELIX_BED_MESH_IF_NEEDED`, `HELIX_CLEAN_NOZZLE`, `HELIX_START_PRINT`

**Installation via the UI:**

1. Open the **Advanced** panel from the navbar and find the **Helper Macros** section
2. Tap **Install Helper Macros** (the row reads **Update Helper Macros** when an update is available, and **Helper Macros - Installed** when current)
3. Restart Klipper when prompted

**Manual Installation:**

1. Copy `assets/config/helix_macros.cfg` to your Klipper config directory
2. Add to your `printer.cfg`:

```ini
[include helix_macros.cfg]
```

3. Add `_HELIX_READY` at the end of your PRINT_START macro:

```gcode
[gcode_macro PRINT_START]
gcode:
    # Your heating commands
    M190 S{BED_TEMP}
    M109 S{EXTRUDER_TEMP}

    # Your homing and leveling
    G28
    BED_MESH_CALIBRATE

    # Signal HelixScreen that prep is done
    _HELIX_READY

    # Purge line or first layer starts here
```

### Option 1b: With Phase Tracking

Phase display works out of the box: with no instrumentation at all, HelixScreen infers the current phase from the console stream, probe lines, bed-mesh status, and toolhead position (how that inference works is developer territory - see [PRINT_START_PROFILES.md](PRINT_START_PROFILES.md)). Adding explicit phase signals is a manual opt-in for exact, firmware-announced phases on a setup where the heuristics pick wrong:

```gcode
[gcode_macro PRINT_START]
gcode:
    _HELIX_PHASE_HOMING
    G28

    _HELIX_PHASE_HEATING_BED
    M190 S{BED_TEMP}

    _HELIX_PHASE_BED_MESH
    BED_MESH_CALIBRATE

    _HELIX_PHASE_HEATING_NOZZLE
    M109 S{EXTRUDER_TEMP}

    _HELIX_PHASE_PURGING
    # Your purge line code here

    # Signal preparation complete
    _HELIX_READY
```

### Option 2: Add Variables to Existing Macro

If you have an existing `_START_PRINT` or `START_PRINT` macro, add state tracking:

```gcode
[gcode_macro _START_PRINT]
variable_print_started: False
gcode:
    # ... your heating, homing, leveling code ...

    # At the end, signal completion:
    SET_GCODE_VARIABLE MACRO=_START_PRINT VARIABLE=print_started VALUE=True
```

### Option 3: Use SET_PRINT_STATS_INFO (Slicer-Based)

If your slicer supports it, enable layer info in your G-code:

**PrusaSlicer/SuperSlicer:**
- Enable "Verbose G-code" in Printer Settings → General → Advanced
- The slicer will insert `SET_PRINT_STATS_INFO CURRENT_LAYER=N` commands

**Cura:**
- Use a post-processing script to add layer comments
- HelixScreen looks for `LAYER:N` or `LAYER: N` patterns

## Available Macros

When `helix_macros.cfg` is installed, these macros are available:

The signal and phase macros below start with an underscore so Fluidd and Mainsail leave them out of their macro lists. A printer that already has the pack under the names without an underscore (`HELIX_READY`, `HELIX_PHASE_HOMING` and so on) keeps those names when it updates, because its own macros and sliced files may call them, so an existing `PRINT_START` needs no change. `MacroManager::macro_content_for` makes that choice when the app installs or updates the pack. Copying the file over by hand always gives the underscore names, so a printer that calls the old names needs those calls changed as well.

### Core Signals

| Macro | Purpose |
|-------|---------|
| `_HELIX_READY` | Signal that print preparation is complete |
| `_HELIX_ENDED` | Signal that print has ended (called in PRINT_END) |
| `_HELIX_RESET` | Reset state (for cancel/error recovery) |

### Phase Tracking (Optional)

| Macro | Phase Displayed |
|-------|-----------------|
| `_HELIX_PHASE_HOMING` | "Homing..." |
| `_HELIX_PHASE_HEATING_BED` | "Heating Bed..." |
| `_HELIX_PHASE_HEATING_NOZZLE` | "Heating Nozzle..." |
| `_HELIX_PHASE_BED_MESH` | "Loading Bed Mesh..." |
| `_HELIX_PHASE_QGL` | "Leveling Gantry..." |
| `_HELIX_PHASE_Z_TILT` | "Z Tilt Adjust..." |
| `_HELIX_PHASE_CLEANING` | "Cleaning Nozzle..." |
| `_HELIX_PHASE_PURGING` | "Purging..." |

### Pre-Print Helpers

| Macro | Purpose |
|-------|---------|
| `HELIX_BED_MESH_IF_NEEDED` | Run bed mesh only if stale (configurable max age) |
| `HELIX_CLEAN_NOZZLE` | Nozzle cleaning sequence (configure brush position) |
| `HELIX_START_PRINT` | Complete start print sequence with all options |

### Uninstalling

The uninstall path (`moonraker-plugin/install.sh`) also runs `strip_phase_tracking.py` over the Klipper config tree. Each marker block is a pair of `HELIX_TRACKING` comment lines wrapped around a single `HELIX_PHASE_*`/`HELIX_READY` call; the script removes those blocks from inside `[gcode_macro ...]` bodies, writes a `.bak.<timestamp>` backup of every file it edits, and never touches a file whose markers look anomalous (unmatched, nested, or not exactly the three lines a marker block consists of). It does not restart Klipper - the edit takes effect at the next restart. Exit status: `0` all clean or stripped, `2` nothing failed but a file needs a human to look, `1` an edit failed.

## Controllable Pre-Print Operations

HelixScreen can analyze your `PRINT_START` macro to detect which operations can be toggled from the print details panel. This allows you to skip bed mesh, QGL, etc. on a per-print basis.

### Stock-printer toggles (pre-start gcode)

On printers whose stock firmware has no toggleable `PRINT_START` parameters, HelixScreen drives the operation itself: the print details toggle dispatches a **pre-start gcode block** to the printer before the job starts, and only then calls `start_print`. The block runs under a 20-minute ceiling with a busy→idle wait, so long operations (a full bed mesh) survive.

This is how the shipped "Auto Bed Mesh" toggle works on the Creality K2, K2 Plus, K2 Pro and K1C (not the `creality_k2_plus_k2improvements` entry, which declares no `bed_mesh` option). The gcode comes from the printer's entry in `assets/config/printer_database.json` (`pre_print_options` → `strategy: pre_start_gcode`):

| Token | Substitutes |
|-------|-------------|
| `{file}` | Filename being printed |
| `{bed_temp}` / `{extruder_temp}` | Job temperatures from the file's **own** `START_PRINT` line (slicer metadata is wrong on multi-material files); `0` when the file carries none |
| `{value}` | `1` when the toggle is on, `0` when off |
| `{?ext}…{/?}` | Segment emitted only when the extruder temperature is known - for firmwares whose commands reject a literal `EXTRUDER_TEMP=0` (Creality K1's Python `get_float(minval=...)` raises). `BED_TEMP=0` needs no marker: the firmware accepts it, and an unheated bed is a real configuration |

An option with `emit_when_disabled: false` has no "off" gcode at all: unchecking it sends nothing and the firmware's own default sequence runs.

`emit_when_disabled: false` has a second meaning (`include/pre_print_option.h#emit_when_disabled`): the line *is* the step, so it is not sent when the started file calls the same command itself. For an enabled option whose id maps to a file-embeddable operation (`bed_mesh`, `qgl`, `z_tilt`, `nozzle_clean`), `collect_pre_start_gcode_lines()` drops the line when `file_runs_command()` finds the rendered line's command word, under that operation type, in the scan's `operation_commands` (`src/ui/print/ui_print_preparation_manager.cpp#file_runs_command`). `GCodeOpsDetector` records there the command word of every line that matches an operation pattern, while `operations` keeps only the first line of each type for the file-modification path. On a K2 only a direct `BED_MESH_CALIBRATE_START_PRINT` call counts, wherever it sits among the file's `BED_MESH` lines; `BED_MESH_PROFILE LOAD`, `BED_MESH_CLEAR` and `START_PRINT` mesh parameters do not, and the line is still sent. The scan is `GCodeOpsDetector` with its default `DetectionConfig` (first 500 lines or 50 KB, thumbnail comments counted, stopping at the first extruding move or layer marker), so a call past that window is not seen and the line is sent. A start with no scan for its file skips nothing.

The pre-start block exists only on starts HelixScreen makes (`PrintPreparationManager::start_print`): the file view's Print, the queue's file-view start and the Job Queue widget's Start next. A job started by a slicer upload, another web UI or Moonraker's `automatic_transition` never gets it.

**Which option states a queued start uses.** Options picked at queue time live in the Moonraker database, namespace `helix-screen`, one key per job at `queued_job_options.<job_id>` (`include/queued_job_options.h`). A job queued elsewhere (Mainsail, Fluidd, a slicer) has no entry, and `load_queued_job_options()` answers an empty map for it, as it does for a read error. The Job Queue widget's Start next (`QueuedJobStarter`, `src/ui/print/queued_job_start.cpp`) installs `saved_option_state()` as the start's option override; an id with no saved state returns -1 and falls through to `option_state_provider_` (only bound while the detail view has rendered rows), then the option's `default_enabled` (`PrintPreparationManager::get_option_state`). The file-view path (`PrintSelectPanel::start_queued_job`) seeds the detail view's rows from the saved entry; rows it does not name keep their `default_enabled`. Either way a K2 job queued from another client starts with `bed_mesh` on.

**K1C specifics.** The K1 firmware's `START_PRINT` runs its full preparation chain whenever its `prepare` variable is 0 (homing, nozzle wipe, accurate Z homing, then `CX_PRINT_LEVELING_CALIBRATE`). That command is a 4-corner bed check - it re-meshes the whole bed only when ≥2 corners drift beyond tolerance, and the whole decision is internal: none of it reaches the gcode console. The shipped toggle front-runs exactly what Creality's own app sends - heat, home, nozzle wipe, accurate home, `BED_MESH_CALIBRATE`, then `PRINT_PREPARED` (which makes the file's `START_PRINT` skip its now-redundant chain) - so one preparation pass runs, at print temperature, with a guaranteed mesh.

Only the K1C entries use this. The K1, K1 Max, and K1 SE entries still map `bed_mesh` to the legacy `PREPARE` macro-param, deliberately: their stock firmware ships a different `START_PRINT` (the older K1 macro branches on `custom_macro.leveling_calibration` instead of `prepare`), so the same front-run sequence is unverified there. Convert them only after capturing a real print start from each model.

### Skipping steps PRINT_START always runs (helix_skips.cfg)

A `PRINT_START` that runs `BED_MESH_CALIBRATE`, `QUAD_GANTRY_LEVEL` or `Z_TILT_ADJUST`
with no parameter to turn it off can still be skipped for one print, without editing
it. Installing or updating the helper macros also writes `helix_skips.cfg`
(`src/printer/preprint_skip_wrappers.cpp#generate`) and puts its include on the first
line of `printer.cfg`:

- `[gcode_macro _HELIX_PREP]` holds one flag per step, `run_bed_mesh`, `run_qgl` and
  `run_z_tilt`, each 1 (run) unless a skip is set. Calling it resets them all.
- Each wrapped command is a `gcode_macro` that renames the built-in
  (`rename_existing: _HELIX_BASE_<COMMAND>`) and runs it with `{rawparams}` unless its
  flag is 0. A skip is one-shot: the wrapper sets the flag back to 1 as it skips, and
  says so on the console.
- `BED_MESH_CLEAR` is wrapped with the mesh calibration and does nothing while a mesh
  skip is pending: `PRINT_START` commonly clears right before it calibrates, which
  would throw away the mesh the skip keeps. While a mesh skip is pending, a
  `BED_MESH_CLEAR` elsewhere (a `PRINT_END`) is skipped too; the pending skip is
  reset as soon as no job holds the machine.

The print-detail panel offers a step's toggle only while skipping it is safe: a mesh
is loaded (`bed_mesh.probed_matrix`), or the gantry or bed has been leveled since the
last motors-off (`quad_gantry_level.applied`, `z_tilt.applied`, Kalico's
`z_tilt_ng.applied`). Turning one off sends
`SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_<step> VALUE=0` in the pre-start
block, which always opens with `_HELIX_PREP`. No HelixPrint plugin is involved. A
database option or a `PRINT_START` skip parameter for the same step wins over the
toggle, and a skip still set while no job holds the machine (the job ended before
reaching the step, or HelixScreen reconnects to find one) is reset with `_HELIX_PREP`,
so a later print started from anywhere runs every step.

What keeps it safe to install:

| Case | What happens |
|------|--------------|
| A user `[gcode_macro BED_MESH_CALIBRATE]` (KAMP, Beacon helpers, vendor macros) already exists | That step is not wrapped (`wrappable()` reads `configfile.settings`) |
| The user adds one later | Klipper merges same-named macro sections, later file winning per option. `helix_skips.cfg` loads first, so the user's macro wins, `ours_intact()` reads false, and the toggle hides. The leftover section in `helix_skips.cfg` is inert, and the next regeneration leaves the step out |
| Klipper reports an error after the restart that loads the file | The Advanced panel removes the file and its include and restarts once (`skip_wrappers::LoadWatch`) |
| Beacon, Cartographer or Creality prtouch replace the command in Python | They do it at config load; `rename_existing` runs later at connect, so the wrapper layers over them |
| A wrappable step is not wrapped (the file or its include is missing, or the printer gained `[z_tilt]`) | The helper macros read as Outdated, and Update stages the file |
| The user later removes `[bed_mesh]`, `[quad_gantry_level]` or `[z_tilt]` while its wrapper is loaded | Klipper refuses to start: `Existing command '...' not found in gcode_macro rename`. Only the restart HelixScreen itself requests is watched, so this one is not rolled back. Remove the `[include helix_skips.cfg]` line, or reinstall the helper macros after restarting |
| A Klipper error after HelixScreen's restart has another cause (a CAN board that did not come up) | The rollback still removes the file, since it cannot tell the causes apart; Update stages it again once Klipper is healthy |

### How a choice reaches the sliced file

A toggle that turns off a step the sliced file runs itself, or a skip parameter added to
the file's `PRINT_START` line, means printing a rewritten copy.
`PrintPreparationManager::modify_and_print` builds the edits once from the file's own
ops scan (`GCodeFileModifier::disable_operations`, `add_print_start_skip_params`): a
comment-out for a detected command, a replace for a `PRINT_START` line or a macro
parameter. Every edit carries the line the scan saw, and a file whose line differs is
refused rather than rewritten, wherever the rewrite runs. Filament Mapping's tool remap
([FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md)) takes the same route.

Where the rewrite runs is one pure decision, `gcode::choose_edit_transport`
(`include/gcode_edit_list.h`):

| HelixPrint `server_edits` feature | Route |
|---|---|
| Ready (`plugin_feature("server_edits", 1)`), the list can travel, not a `.3mf` plate | `server.helix.print_edited`: the app sends the edit list (`gcode::to_server_edits`) and the plugin rewrites beside the original, then starts the copy as `print_modified` does. Nothing is downloaded or uploaded. The plugin refuses with 507 when the copy would leave less than 100 MB free (`helix_storage.py`), as `print_modified` does |
| Anything else | `rewrite_and_print`: download, `GCodeFileModifier::apply_streaming` (or `GcodeToolRemapper::apply_to_file`) file to file, upload, then `print_modified` (or a plain start without the plugin). The upload waits for `storage::check_upload_space`: a copy that would leave less than 100 MB free on the gcodes volume is not uploaded and the start is abandoned with a toast |

A list cannot travel when it holds text that is not UTF-8, both edits and a tool remap,
or a range on an edit kind that has none; it then rewrites on the screen. A `.3mf` plate
always does, since the plate has to be extracted first. The plugin refusing a stale line
ends the start with an error, as the screen's own rewrite would.

The two rewriters are one grammar in two languages. `tests/fixtures/gcode_edits/` holds
input, edit list and expected bytes for each case; `tests/unit/test_gcode_edit_list.cpp`
and `moonraker-plugin/tests/test_helix_edits.py` both apply every case, and a new edit
kind or line rule lands in both with a fixture.

The copy ends with a newline exactly when the original does. Klipper's `virtual_sdcard`
never runs an unterminated final line, so a copy that dropped the newline would skip
the file's last command.

### Parameter Semantics

HelixScreen recognizes two styles of parameter control:

| Style | Example | When checkbox is unchecked |
|-------|---------|---------------------------|
| **Opt-IN** (recommended) | `PERFORM_BED_MESH=1` | Passes `PERFORM_BED_MESH=0` |
| **Opt-OUT** | `SKIP_BED_MESH=1` | Passes `SKIP_BED_MESH=1` |

**Opt-IN (PERFORM_*)**: Operation is skipped by default; checkbox enables it.
**Opt-OUT (SKIP_*)**: Operation runs by default; checkbox disables it.

### Recognized Parameter Names

HelixScreen detects these parameter patterns in your `PRINT_START` macro:

| Operation | Opt-IN Patterns | Opt-OUT Patterns |
|-----------|-----------------|------------------|
| Bed Mesh | `PERFORM_BED_MESH`, `DO_BED_MESH`, `FORCE_BED_MESH`, `FORCE_LEVELING` | `SKIP_BED_MESH`, `SKIP_MESH`, `NO_MESH` |
| QGL | `PERFORM_QGL`, `DO_QGL`, `FORCE_QGL` | `SKIP_QGL`, `NO_QGL` |
| Z-Tilt | `PERFORM_Z_TILT`, `DO_Z_TILT`, `FORCE_Z_TILT` | `SKIP_Z_TILT`, `NO_Z_TILT` |
| Nozzle Clean | `PERFORM_NOZZLE_CLEAN`, `DO_NOZZLE_CLEAN` | `SKIP_NOZZLE_CLEAN`, `SKIP_CLEAN` |

### Making Your Macro Controllable

Wrap operations in Jinja conditionals using recognized parameters:

**Opt-IN Style (recommended):**

```gcode
[gcode_macro PRINT_START]
gcode:
    {% set perform_bed_mesh = params.PERFORM_BED_MESH|default(0)|int %}
    {% set perform_qgl = params.PERFORM_QGL|default(0)|int %}

    G28  ; Always home

    {% if perform_qgl == 1 %}
        QUAD_GANTRY_LEVEL
    {% endif %}

    {% if perform_bed_mesh == 1 %}
        BED_MESH_CALIBRATE
    {% endif %}

    _HELIX_READY
```

**Opt-OUT Style:**

```gcode
[gcode_macro PRINT_START]
gcode:
    {% set skip_bed_mesh = params.SKIP_BED_MESH|default(0)|int %}

    G28  ; Always home

    {% if skip_bed_mesh == 0 %}
        BED_MESH_CALIBRATE
    {% endif %}

    _HELIX_READY
```

### Using HELIX_START_PRINT

The bundled `HELIX_START_PRINT` macro supports all controllable operations:

```gcode
; In your slicer's start G-code:
HELIX_START_PRINT BED_TEMP={first_layer_bed_temperature} EXTRUDER_TEMP={first_layer_temperature} PERFORM_BED_MESH=1 PERFORM_QGL=1
```

Available parameters:
- `BED_TEMP` - Bed temperature (default: 60)
- `EXTRUDER_TEMP` - Extruder temperature (default: 200)
- `PERFORM_QGL` - Run quad gantry level (0 or 1)
- `PERFORM_Z_TILT` - Run Z-tilt adjust (0 or 1)
- `PERFORM_BED_MESH` - Run bed mesh calibrate (0 or 1)
- `PERFORM_NOZZLE_CLEAN` - Run nozzle cleaning (0 or 1)

## Troubleshooting

### "Preparing" Stuck Forever

If the home panel stays on "Preparing Print" indefinitely:

1. **Check console output**: Run your print and look at the Klipper console. Do you see any layer markers?
2. **Verify macro variables**: Query `gcode_macro _HELIX_STATE` via Moonraker to see if `print_started` is being set
3. **Check whether the printer is still talking.** The deadline timeouts wait for 90
   seconds without pre-print activity: a matched line, a probe line, a status-signal
   rule firing, or a bed or nozzle climbing past its highest reading under its current
   target. The ceiling (1800s, or 2.5x the predicted total when that is longer) ignores
   temperatures, climbing heaters and status-signal rules, but still waits for 90
   seconds without a matched line or probe line, so a slow bed mesh or a narrated
   chamber soak is not cut off mid-sequence. Only the backstop, at twice the ceiling,
   ignores everything: a printer that keeps talking past it still leaves Preparing.
4. **Check for a host-side pre-start block.** If the printer is commit-armed and a
   forced bed mesh or setup macro is running in front of the job, `print_stats` will
   read `complete` or `standby` for the whole block. That is expected, not a hang -
   look for the blocking `gcode_script` in the log and its elapsed time.

### Quick Detection Not Working

If detection falls all the way through to a timeout:

1. **Add _HELIX_READY**: The most reliable option
2. **Check if your slicer sets layer info**: Look for `SET_PRINT_STATS_INFO` in your G-code
3. **Verify macro object subscriptions**: HelixScreen subscribes to `gcode_macro _HELIX_STATE`, `gcode_macro _START_PRINT`, and `gcode_macro START_PRINT`
4. **ForgeX users**: Detection should be instant via the Forge-X profile (the `operation_context` status object on 1.4.x, `// State:` lines on older versions), the `_START_PRINT.print_started` and `START_PRINT.preparation_done` macro variables

## Files

| File | Purpose |
|------|---------|
| `assets/config/helix_macros.cfg` | The Klipper macros this doc describes: install it or copy the calls into your own macros |
| `moonraker-plugin/install.sh` | Installs and strips the phase-tracking marker blocks (the uninstall path above) |
| `src/printer/macro_manager.cpp` | Macro installation management behind the UI button |
