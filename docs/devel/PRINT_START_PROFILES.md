# Print Start Profiles (Developer Guide)

How the modular print start profile system works, how to add profiles for new printers, and full reference for all fields and phases.

**User-facing doc**: [PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md) (macros, slicer setup, troubleshooting)

---

## Architecture Overview

```
printer_database.json              assets/config/print_start_profiles/
  ┌────────────────────┐             ┌──────────────┐
  │ "ad5m" entry       │──refs────-> │ forge_x.json │  Sequential "// State:" signals
  │ "ad5m_pro" entry   │──refs────-> │ forge_x.json │  Same profile, same firmware
  │ "voron_24" entry   │──(none)──-> │ default.json │  Automatic fallback
  │ (unknown printer)  │──(none)──-> │ default.json │  Automatic fallback
  └────────────────────┘             └──────────────┘
                                           │
                   PrintStartCollector  <───┘
                     ├─ HELIX:PHASE:* signals (universal, always highest priority)
                     ├─ Profile signal formats (prefix + value lookup)
                     ├─ Profile regex patterns (response_patterns)
                     └─ Built-in fallback (if no JSON files found)
```

### Detection Priority Chain

Every G-code response line is checked in this order. First match wins.

| Priority | Source | Description |
|----------|--------|-------------|
| 1 | `HELIX:PHASE:*` | Universal override. Emitted by HelixScreen macros. Always checked, never profile-specific. |
| 2 | Profile signal formats | Exact prefix + value lookup (e.g., Forge-X `// State: HOMING...`). |
| 3 | PRINT_START marker | Regex: `PRINT_START\|START_PRINT\|_PRINT_START`. Sets INITIALIZING once per session. |
| 4 | Completion marker | Regex: `SET_PRINT_STATS_INFO\s+CURRENT_LAYER=\|LAYER:?\s*1\b\|...`. Sets COMPLETE. |
| 5 | Profile regex patterns | `response_patterns` from the loaded profile JSON. |
| 6 | Built-in fallback | Hardcoded patterns identical to `default.json`. Only used if no JSON loads at all. |

### Progress Modes

| Mode | When to use | How it works |
|------|-------------|--------------|
| **`weighted`** | Unknown printers, generic macros | Sum weights of detected phases. Missing phases are fine (weight just isn't added). Progress = `sum(detected weights) / sum(all weights) * 95`. Capped at 95% until COMPLETE. |
| **`sequential`** | Known firmware with deterministic output | Each signal maps to a specific 0-100% value. Progress jumps directly to that value. Smooth, predictable bar for printers we've profiled. |

---

## All Phases

These are the `PrintStartPhase` enum values from `printer_state.h`. Use the **string name** (case-insensitive) in profile JSON files.

| Enum Value | Int | String Name | Typical Trigger | Default Weight |
|------------|-----|-------------|-----------------|----------------|
| `IDLE` | 0 | `IDLE` | Not in PRINT_START | - |
| `INITIALIZING` | 1 | `INITIALIZING` | PRINT_START detected | - |
| `HOMING` | 2 | `HOMING` | G28, Home All Axes | 10 |
| `HEATING_BED` | 3 | `HEATING_BED` | M190, M140 S>0 | 20 |
| `HEATING_NOZZLE` | 4 | `HEATING_NOZZLE` | M109, M104 S>0 | 20 |
| `QGL` | 5 | `QGL` | QUAD_GANTRY_LEVEL | 15 |
| `Z_TILT` | 6 | `Z_TILT` | Z_TILT_ADJUST | 15 |
| `BED_MESH` | 7 | `BED_MESH` | BED_MESH_CALIBRATE | 10 |
| `CLEANING` | 8 | `CLEANING` | CLEAN_NOZZLE, WIPE_NOZZLE | 5 |
| `PURGING` | 9 | `PURGING` | VORON_PURGE, LINE_PURGE | 5 |
| `COMPLETE` | 10 | `COMPLETE` | Layer 1 detected, HELIX:READY | - |

Notes:
- `IDLE` and `COMPLETE` are lifecycle states, not matchable phases in profiles.
- `INITIALIZING` is set automatically when PRINT_START is detected. You can also map signals to it for firmware that has distinct pre-homing steps.
- A single phase can be triggered multiple times (e.g., CLEANING appears 6 times in Forge-X). In sequential mode each hit updates the progress. In weighted mode only the first detection counts.

---

## Profile JSON Schema

Profiles live in `assets/config/print_start_profiles/{name}.json`.

```jsonc
{
  // REQUIRED: Human-readable name shown in logs
  "name": "My Printer Profile",

  // OPTIONAL: Description for documentation
  "description": "Profile for XYZ firmware on ABC printer",

  // OPTIONAL: "weighted" (default) or "sequential"
  "progress_mode": "weighted",

  // OPTIONAL: Opt in to Creality's tag-stream matchers (purge
  // "// num: N, velocity: V, percent F" lines, "[box]" CFS load events).
  // Without the flag those lines are inert on every profile.
  "cfs_signals": false,

  // OPTIONAL: This printer's prep chain goes silent (no gcode_response
  // markers) while it homes Z, validates the mesh at its corners, and
  // sweeps a calibration mesh - but toolhead.position keeps flowing.
  // The collector classifies that stream and refines the status line
  // through the silence: "Probing Z...", "Checking Bed Mesh...", and
  // sweep-march -> BED_MESH entry. Zones are anchored to the bed-mesh
  // probe area (mesh_min/mesh_max from the bed_mesh status object).
  "position_signals": false,

  // OPTIONAL: The bed-mesh sweep is trimmed to the object (KAMP-style),
  // so a configured probe_count overstates the sweep - the collector
  // skips the configfile denominator and counts live points instead.
  "adaptive_meshing": false,

  // OPTIONAL: Exact-match signal detection (for firmware with structured output)
  "signal_formats": [
    {
      // The exact prefix to search for in each G-code response line
      // Uses string find (not regex), so it matches anywhere in the line
      "prefix": "// State: ",

      // Map of exact values (after prefix) to phase info
      "mappings": {
        "HOMING...": {
          "phase": "HOMING",             // Phase name (case-insensitive)
          "message": "Homing axes...",    // Shown to user
          "progress": 10                  // 0-100, only used in sequential mode
        }
      }
    }
  ],

  // OPTIONAL: Regex pattern detection (for console output parsing)
  "response_patterns": [
    {
      // Regex pattern (case-insensitive). Supports capture groups.
      "pattern": "G28|Homing|Home All Axes",

      // Phase to set when matched
      "phase": "HOMING",

      // Message shown to user. Supports $1, $2, etc. for capture groups.
      "message": "Homing...",

      // Weight for weighted mode. In sequential mode this field is ignored.
      "weight": 10
    }
  ],

  // OPTIONAL: Override default weights for weighted progress calculation
  // Keys are phase names (case-insensitive), values are integer weights
  "phase_weights": {
    "HOMING": 10,
    "HEATING_BED": 20,
    "HEATING_NOZZLE": 20
  }
}
```

### Field Details

**`signal_formats`** — Best for firmware that outputs structured state lines (like Forge-X's `// State: HOMING...`). The prefix is matched with `string::find()`, not regex, so it works even if the line has other content before the prefix. The value after the prefix must match a mapping key **exactly** (case-sensitive, including trailing punctuation like `...`).

**`response_patterns`** — Best for catching G-code commands and freeform console output. Patterns are compiled with `std::regex::icase`. Capture groups (`$1`, `$2`, etc.) in the message template are substituted with matched groups. Each pattern is checked via `std::regex_search` (partial match, not full line).

**`phase_weights`** — Only meaningful in `weighted` mode. If omitted, phases matched by response_patterns use their individual `weight` field. If provided, this map is used by `calculate_progress_locked()` to sum detected phase weights.

**`message` strings are English translation tags** — they pass through `lv_tr()` at match time, so a loaded language pack resolves them like the built-in labels. An untranslated tag displays as-is.

**Non-console signals.** Two phase signals do not arrive through `notify_gcode_response`: a bed-mesh status clear while CLEANING enters BED_MESH ("Bed Leveling...", denominator fetched then), and `probe at X,Y is z=Z` lines are consumed as mesh points (never re-matched against `response_patterns`, so a BED_MESH pattern cannot re-announce the phase and reset the probe counters mid-sweep). See the "Silent-phase signals" section of [PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md); the full observer map - all five signal sources, threading, and the tests that pin them - is [PRINT_START_OBSERVERS.md](PRINT_START_OBSERVERS.md).

---

## HELIX:PHASE Signal Reference

These are **universal** and always active regardless of profile. They are the highest priority detection. Emitted by HelixScreen Klipper macros via `M118` or `RESPOND TYPE=command`.

Format: `HELIX:PHASE:{PHASE_NAME}`

| Signal | Maps to Phase |
|--------|---------------|
| `HELIX:PHASE:STARTING` or `HELIX:PHASE:START` | INITIALIZING |
| `HELIX:PHASE:HOMING` | HOMING |
| `HELIX:PHASE:HEATING_BED` or `HELIX:PHASE:BED_HEATING` | HEATING_BED |
| `HELIX:PHASE:HEATING_NOZZLE` or `HELIX:PHASE:NOZZLE_HEATING` or `HELIX:PHASE:HEATING_HOTEND` | HEATING_NOZZLE |
| `HELIX:PHASE:QGL` or `HELIX:PHASE:QUAD_GANTRY_LEVEL` | QGL |
| `HELIX:PHASE:Z_TILT` or `HELIX:PHASE:Z_TILT_ADJUST` | Z_TILT |
| `HELIX:PHASE:BED_MESH` or `HELIX:PHASE:BED_LEVELING` | BED_MESH |
| `HELIX:PHASE:CLEANING` or `HELIX:PHASE:NOZZLE_CLEAN` | CLEANING |
| `HELIX:PHASE:PURGING` or `HELIX:PHASE:PURGE` or `HELIX:PHASE:PRIMING` | PURGING |
| `HELIX:PHASE:COMPLETE` or `HELIX:PHASE:DONE` | COMPLETE |

---

## How to Add a Profile for a New Printer

### Step 1: Capture G-code output

Run a print on the target printer with debug logging:

```bash
./build/bin/helix-screen -vvv  # TRACE level shows all G-code responses
```

Or check Moonraker's console output / Mainsail console. Look for patterns during the PRINT_START sequence. Note:
- Does the firmware emit structured state lines? (signal format candidate)
- What G-code commands are visible in the console? (regex pattern candidate)
- Is the sequence deterministic? (sequential mode candidate)
- How long does each phase take roughly? (helps set weights)

### Step 2: Create the profile JSON

Create `assets/config/print_start_profiles/{name}.json`. Choose your approach:

**Structured output (sequential mode)** — If the firmware emits structured lines like `// State: HOMING...` or `[STATUS] Heating bed`, use `signal_formats` with `sequential` progress mode. Map each state to a progress percentage based on roughly how far through the prep sequence it occurs.

**Freeform output (weighted mode)** — If the firmware just outputs standard G-code commands and messages, use `response_patterns` with `weighted` mode. Assign weights based on how long each phase typically takes.

**Both** — You can combine signal formats and response patterns in the same profile. Signal formats are checked first (priority 2), response patterns later (priority 5).

### Step 3: Add to printer database

In `assets/config/printer_database.json`, add the `print_start_profile` field to the printer entry:

```json
{
  "id": "my_printer_id",
  "name": "My Printer Name",
  "print_start_profile": "my_profile_name",
  ...
}
```

The value must match the JSON filename without the `.json` extension.

If a printer has no `print_start_profile` field, or the profile fails to load, the system falls back to `default.json`, then to built-in hardcoded patterns (identical to `default.json`). This three-level fallback chain means nothing ever breaks.

### Step 4: Add to PrinterDetector (if new printer)

If this is a brand new printer type, you also need detection heuristics in `printer_database.json` so HelixScreen can identify the printer. See existing entries for the pattern (macro matches, sensor matches, object matches, etc.).

### Step 5: Test it

```bash
# Run profile-specific tests
./build/bin/helix-tests "[profile]"

# Run all print start tests
./build/bin/helix-tests "[print]"
```

Write tests in `tests/unit/test_print_start_profile.cpp` that:
1. Load your profile by name
2. Test every signal format mapping
3. Test response patterns with realistic console output
4. Test noise rejection (lines that should NOT match)

---

## Example: Hypothetical "ThermoBot" Printer

This is a fictional printer to demonstrate the format. It doesn't match any real firmware.

The ThermoBot firmware outputs lines like:
```
[TBOT] Phase: WARMING_UP
[TBOT] Phase: CALIBRATING
[TBOT] Phase: MESH_SCAN
[TBOT] Phase: READY_TO_PRINT
// Nozzle heating to 215C
// Bed stabilizing at 60C
```

Profile: assets/config/print_start_profiles/thermobot.json

```json
{
  "name": "ThermoBot FW",
  "description": "Hypothetical ThermoBot firmware with [TBOT] Phase: signals",
  "progress_mode": "sequential",

  "signal_formats": [
    {
      "prefix": "[TBOT] Phase: ",
      "mappings": {
        "WARMING_UP":      { "phase": "HEATING_BED",    "message": "Warming up...",          "progress": 10 },
        "CALIBRATING":     { "phase": "HOMING",         "message": "Calibrating axes...",    "progress": 30 },
        "MESH_SCAN":       { "phase": "BED_MESH",       "message": "Scanning bed mesh...",   "progress": 60 },
        "READY_TO_PRINT":  { "phase": "COMPLETE",        "message": "Starting print...",     "progress": 100 }
      }
    }
  ],

  "response_patterns": [
    {
      "pattern": "Nozzle heating to (\\d+)",
      "phase": "HEATING_NOZZLE",
      "message": "Heating nozzle to $1C...",
      "weight": 20
    },
    {
      "pattern": "Bed stabilizing at (\\d+)",
      "phase": "HEATING_BED",
      "message": "Bed stabilizing at $1C...",
      "weight": 20
    }
  ],

  "phase_weights": {
    "HEATING_BED": 20,
    "HOMING": 15,
    "HEATING_NOZZLE": 20,
    "BED_MESH": 30,
    "PURGING": 10
  }
}
```

Then in `printer_database.json`:
```json
{
  "id": "thermobot_x1",
  "name": "ThermoBot X1",
  "print_start_profile": "thermobot",
  ...
}
```

---

## Existing Profiles

| Profile | File | Mode | Printers | Key Feature |
|---------|------|------|----------|-------------|
| **Generic** | `default.json` | weighted | All unrecognized printers | 8 regex patterns covering common G-code commands |
| **Forge-X** | `forge_x.json` | sequential | FlashForge AD5M, AD5M Pro | 14 `// State:` signal mappings + 2 temperature regex patterns |
| **Built-in fallback** | (hardcoded) | weighted | Emergency fallback | Identical to `default.json`, compiled into binary |

---

## Fallback Completion Detection

For printers that don't emit any G-code layer markers (like Forge-X), the collector has additional fallback signals. These are enabled a few seconds after `start()` to give G-code response detection priority first.

| Fallback | Condition | When |
|----------|-----------|------|
| Layer count | `current_layer >= 1` | Most reliable when slicer outputs layer info |
| Progress + temps | `progress >= 2%` AND temps at target | File past preamble/macros |
| Timeout + temps | Elapsed > 45s AND temps >= 90% of target | Last resort |
| Macro variables | `_START_PRINT.print_started`, `START_PRINT.preparation_done`, `_HELIX_STATE.print_started` | Subscribed via Moonraker |

---

## Key Files

| File | Purpose |
|------|---------|
| `include/print_start_profile.h` | Profile class: structs, factory methods, matching API |
| `src/print/print_start_profile.cpp` | JSON parsing, signal/pattern matching, built-in fallback |
| `include/print_start_collector.h` | Collector: lifecycle, phase tracking, profile + predictor integration |
| `src/print/print_start_collector.cpp` | Detection engine: priority chain, progress calculation, ETA timer |
| `include/preprint_predictor.h` | Pure-logic ETA predictor using historical timing data |
| `src/print/preprint_predictor.cpp` | Config integration, caching for predictor |
| `include/printer_state.h` | `PrintStartPhase` enum, subject accessors |
| `include/printer_print_state.h` | Print domain: progress, layers, preprint ETA subjects |
| `src/application/moonraker_manager.cpp` | Wiring: profile loading, observer setup |
| `include/printer_detector.h` | `get_print_start_profile()` declaration |
| `src/printer/printer_detector.cpp` | Database lookup for profile name |
| `assets/config/print_start_profiles/*.json` | Profile definitions |
| `assets/config/printer_database.json` | Maps printer IDs to profile names |
| `tests/unit/test_print_start_profile.cpp` | Profile loading + matching tests |
| `tests/unit/test_print_start_collector.cpp` | Integration tests with collector |
| `tests/unit/test_preprint_predictor.cpp` | Predictor unit tests (weighting, FIFO, edge cases) |
| `docs/devel/PRINT_START_INTEGRATION.md` | User-facing setup guide |

---

## Thread Safety Notes

- `set_profile()` must be called **before** `start()`. It is rejected (with a warning) if the collector is active.
- `profile_` is a `shared_ptr` that is read-only after `start()`. No mutex needed for reads.
- `state_mutex_` protects `detected_phases_`, `current_phase_`, `print_start_detected_`, and `printing_state_start_`. All writes happen under the lock.
- `update_phase()` calls `state_.set_print_start_state()` **outside** the lock (it posts to the UI thread via `ui_queue_update()`).
- WebSocket callbacks (`on_gcode_response`) run on a background thread. `check_fallback_completion()` runs on the main thread.

---

## Pre-Print ETA Prediction

The `PreprintPredictor` uses historical timing data from previous prints to estimate how long the remaining preparation will take. It integrates with `PrintStartCollector` and is exposed as subjects on `PrinterPrintState`.

### How It Works

1. **During PRINT_START**, the collector records timestamps when each phase is entered
2. **On completion**, the per-phase durations are saved to config (`/print_start_history/entries`)
3. **On next print**, the predictor loads history and computes a weighted average across entries:
   - 1 entry: 100% weight
   - 2 entries: 60/40 (recent favored)
   - 3 entries: 50/30/20 (most recent favored)
4. **Real-time remaining** is calculated by subtracting completed phase time and elapsed time in current phase

### Key Design Decisions

- **Pure logic class**: `PreprintPredictor` has no LVGL or Config dependencies, making it fully unit-testable
- **FIFO with cap**: Keeps last 3 entries, rejects any entry over 15 minutes (anomaly protection)
- **Phase subset handling**: Redistributes weights when phases appear in only some entries
- **60-second cache**: `predicted_total_from_config()` caches parsed config to avoid repeated JSON parsing

### Files

| File | Purpose |
|------|---------|
| `include/preprint_predictor.h` | Pure prediction logic, weighted averages |
| `src/print/preprint_predictor.cpp` | Config integration, caching |
| `tests/unit/test_preprint_predictor.cpp` | 18 test cases covering all prediction paths |
| `include/printer_print_state.h` | Exposes `preprint_remaining_` and `preprint_elapsed_` subjects |

---

## Future Extensions

- **Probe progress**: During BED_MESH, count `// probe at X,Y` messages for "Probing 14/25" display
- **Temperature progress**: During HEATING phases, calculate % based on current vs target temp
- **User-editable profiles**: `config/print_start_profiles.d/` for user overrides (same pattern as `printer_database.d/`)
- **Voron profile**: Map Voron `status_*` LED macro calls to phases
- **Bambu/Prusa profiles**: For future printer support
