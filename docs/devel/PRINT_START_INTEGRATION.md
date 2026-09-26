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
- **Instant detection**: `HELIX_READY` signal at the end of your PRINT_START
- **Phase tracking**: Optional `HELIX_PHASE_*` macros for detailed progress display
- **Pre-print helpers**: `HELIX_BED_LEVEL_IF_NEEDED`, `HELIX_CLEAN_NOZZLE`, `HELIX_START_PRINT`

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

3. Add `HELIX_READY` at the end of your PRINT_START macro:

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
    HELIX_READY

    # Purge line or first layer starts here
```

### Option 1b: With Phase Tracking

Phase display works out of the box: with no instrumentation at all, HelixScreen infers the current phase from the console stream, probe lines, bed-mesh status, and toolhead position (how that inference works is developer territory - see [PRINT_START_PROFILES.md](PRINT_START_PROFILES.md)). Adding explicit phase signals is a manual opt-in for exact, firmware-announced phases on a setup where the heuristics pick wrong:

```gcode
[gcode_macro PRINT_START]
gcode:
    HELIX_PHASE_HOMING
    G28

    HELIX_PHASE_HEATING_BED
    M190 S{BED_TEMP}

    HELIX_PHASE_BED_MESH
    BED_MESH_CALIBRATE

    HELIX_PHASE_HEATING_NOZZLE
    M109 S{EXTRUDER_TEMP}

    HELIX_PHASE_PURGING
    # Your purge line code here

    # Signal preparation complete
    HELIX_READY
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

### Core Signals

| Macro | Purpose |
|-------|---------|
| `HELIX_READY` | Signal that print preparation is complete |
| `HELIX_ENDED` | Signal that print has ended (called in PRINT_END) |
| `HELIX_RESET` | Reset state (for cancel/error recovery) |

### Phase Tracking (Optional)

| Macro | Phase Displayed |
|-------|-----------------|
| `HELIX_PHASE_HOMING` | "Homing..." |
| `HELIX_PHASE_HEATING_BED` | "Heating Bed..." |
| `HELIX_PHASE_HEATING_NOZZLE` | "Heating Nozzle..." |
| `HELIX_PHASE_BED_MESH` | "Loading Bed Mesh..." |
| `HELIX_PHASE_QGL` | "Leveling Gantry..." |
| `HELIX_PHASE_Z_TILT` | "Z Tilt Adjust..." |
| `HELIX_PHASE_CLEANING` | "Cleaning Nozzle..." |
| `HELIX_PHASE_PURGING` | "Purging..." |

### Pre-Print Helpers

| Macro | Purpose |
|-------|---------|
| `HELIX_BED_LEVEL_IF_NEEDED` | Run bed mesh only if stale (configurable max age) |
| `HELIX_CLEAN_NOZZLE` | Nozzle cleaning sequence (configure brush position) |
| `HELIX_START_PRINT` | Complete start print sequence with all options |

### Uninstalling

The uninstall path (`moonraker-plugin/install.sh`) also runs `strip_phase_tracking.py` over the Klipper config tree. Each marker block is a pair of `HELIX_TRACKING` comment lines wrapped around a single `HELIX_PHASE_*`/`HELIX_READY` call; the script removes those blocks from inside `[gcode_macro ...]` bodies, writes a `.bak.<timestamp>` backup of every file it edits, and never touches a file whose markers look anomalous (unmatched, nested, or not exactly the three lines a marker block consists of). It does not restart Klipper - the edit takes effect at the next restart. Exit status: `0` all clean or stripped, `2` nothing failed but a file needs a human to look, `1` an edit failed.

## Controllable Pre-Print Operations

HelixScreen can analyze your `PRINT_START` macro to detect which operations can be toggled from the print details panel. This allows you to skip bed mesh, QGL, etc. on a per-print basis.

### Stock-printer toggles (pre-start gcode)

On printers whose stock firmware has no toggleable `PRINT_START` parameters, HelixScreen drives the operation itself: the print details toggle dispatches a **pre-start gcode block** to the printer before the job starts, and only then calls `start_print`. The block runs under a 20-minute ceiling with a busy→idle wait, so long operations (a full bed mesh) survive.

This is how the shipped "Auto Bed Mesh" toggle works on the Creality K2 Plus / K2 Pro and K1C. The gcode comes from the printer's entry in `assets/config/printer_database.json` (`pre_print_options` → `strategy: pre_start_gcode`):

| Token | Substitutes |
|-------|-------------|
| `{file}` | Filename being printed |
| `{bed_temp}` / `{extruder_temp}` | Job temperatures from the file's **own** `START_PRINT` line (slicer metadata is wrong on multi-material files); `0` when the file carries none |
| `{value}` | `1` when the toggle is on, `0` when off |
| `{?ext}…{/?}` | Segment emitted only when the extruder temperature is known - for firmwares whose commands reject a literal `EXTRUDER_TEMP=0` (Creality K1's Python `get_float(minval=...)` raises). `BED_TEMP=0` needs no marker: the firmware accepts it, and an unheated bed is a real configuration |

An option with `emit_when_disabled: false` has no "off" gcode at all: unchecking it sends nothing and the firmware's own default sequence runs.

**K1C specifics.** The K1 firmware's `START_PRINT` runs its full preparation chain whenever its `prepare` variable is 0 (homing, nozzle wipe, accurate Z homing, then `CX_PRINT_LEVELING_CALIBRATE`). That command is a 4-corner bed check - it re-meshes the whole bed only when ≥2 corners drift beyond tolerance, and the whole decision is internal: none of it reaches the gcode console. The shipped toggle front-runs exactly what Creality's own app sends - heat, home, nozzle wipe, accurate home, `BED_MESH_CALIBRATE`, then `PRINT_PREPARED` (which makes the file's `START_PRINT` skip its now-redundant chain) - so one preparation pass runs, at print temperature, with a guaranteed mesh.

Only the K1C entries use this. The K1, K1 Max, and K1 SE entries still map `bed_mesh` to the legacy `PREPARE` macro-param, deliberately: their stock firmware ships a different `START_PRINT` (the older K1 macro branches on `custom_macro.leveling_calibration` instead of `prepare`), so the same front-run sequence is unverified there. Convert them only after capturing a real print start from each model.

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

    HELIX_READY
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

    HELIX_READY
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

1. **Add HELIX_READY**: The most reliable option
2. **Check if your slicer sets layer info**: Look for `SET_PRINT_STATS_INFO` in your G-code
3. **Verify macro object subscriptions**: HelixScreen subscribes to `gcode_macro _HELIX_STATE`, `gcode_macro _START_PRINT`, and `gcode_macro START_PRINT`
4. **ForgeX users**: Detection should be instant via the Forge-X profile (`// State:` signals) and `START_PRINT.preparation_done` macro variable

## Files

| File | Purpose |
|------|---------|
| `assets/config/helix_macros.cfg` | The Klipper macros this doc describes: install it or copy the calls into your own macros |
| `moonraker-plugin/install.sh` | Installs and strips the phase-tracking marker blocks (the uninstall path above) |
| `src/printer/macro_manager.cpp` | Macro installation management behind the UI button |
