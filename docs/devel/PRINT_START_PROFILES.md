# Print Start Profiles (Developer Guide)

How the modular print start profile system works, how to add profiles for new printers, and full reference for all fields and phases.

**User-facing doc**: [PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md) - macros, slicer setup, troubleshooting. That is the modder's half of the same system: this guide covers the profile side (what HelixScreen ships and how to extend it), that one covers making a user's existing macros legible to HelixScreen with no profile at all.

---

## Architecture Overview

```
assets/config/printer_database.json        assets/config/print_start_profiles/
  ┌──────────────────────────────┐           ┌───────────────────┐
  │ "flashforge_adventurer_5m_   │──refs────> │ forge_x.json      │ Sequential "// State:" signals
  │   forgex" entry              │           │                   │ + phase_object + status_signals
  │ "creality_k2_plus" entry     │──refs────> │ creality_k2.json  │ CFS tags + position + adaptive mesh
  │ "voron_24" entry (no field)  │──(none)──> │ default.json      │ Automatic fallback
  │ (unknown printer)            │──(none)──> │ default.json      │ Automatic fallback
  └──────────────────────────────┘           └───────────────────┘
                                                    │
                              PrintStartCollector <──┘
                                ├─ HELIX:PHASE:* signals (universal, always highest priority)
                                ├─ Profile signal formats (prefix + value lookup)
                                ├─ Profile regex patterns (response_patterns)
                                ├─ Profile status-object evidence (phase_object, status_signals)
                                └─ Built-in fallback (if no JSON files found)
```

The active profile is loaded by `PrintStartProfile::load()` and drives `PrintStartCollector`. Any status objects it declares (`phase_object`, `status_signals`) are subscribed generically during discovery - `PrintStartProfile::required_status_objects()` feeds `MoonrakerDiscoverySequence::build_subscription_objects()` (`src/api/moonraker_discovery_sequence.cpp`), so a profile adding a status dependency needs no C++ change.

### The Three Kinds of Evidence

A print start is narrated by somebody. Which of the three evidence kinds a printer offers decides how its profile is written - this is the *why* behind the priority chain below:

| Kind | What it is | Schema fields | Trust |
|------|------------|---------------|-------|
| Narration | Console lines the firmware or macros print | `signal_formats`, `response_patterns` | What the printer SAYS - authoritative |
| Structured state | A status object carrying phase strings | `phase_object` + `state_patterns` | What the printer PUBLISHES - authoritative |
| Physical inference | Predicates over status frames | `status_signals` | What the printer IS DOING - confirmation for gaps nobody narrates, never a replacement for narration |

The line between the first two kinds and the third is enforced in code, not only in review. A narration match - console line or phase-object state - marks a real firmware signal, which tells the collector the printer is narrating its own sequence and switches off the proactive temperature detector in `check_fallback_completion()`. A `status_signals` match deliberately does not: `handle_status_signals()` passes `marks_real_signal=false` (`src/print/print_start_collector.cpp#handle_status_signals`), because a rule's predicates read the same heater and toolhead frames that detector reads, and counting one as narration would silence the detector with its own input - costing the HOMING and heating phases only it can supply on a printer that narrates nothing.

Phase semantics are engine-owned: the `PrintStartPhase` enum (`include/print_start_phase.h`) is the whole vocabulary, and profiles never invent phases - they only map evidence onto it.

Discipline for inference rules: an unambiguous physical fact (the head parked at a cutter's corner) may carry a high weight; an ambiguous one (a heater at target) is a tie-breaker at most - or stays out of the profile. A confident wrong phase is worse than an honest generic bar. `default.json` follows this rule: it infers only the two rising-edge heating facts and deliberately omits anything more ambiguous.

### Detection Priority Chain

Every G-code response line is checked in this order. First match wins, and a matched line is consumed - it does not fall through to a later stage.

| Priority | Source | Description |
|----------|--------|-------------|
| 1 | `HELIX:PHASE:*` | Universal override. Emitted by HelixScreen macros. Always checked, never profile-specific. |
| 2 | Profile CFS tag stream | Creality tag lines (`// num: N, velocity: V, percent F` purge progress, `[box]` CFS load events). Only checked when the profile sets `cfs_signals: true` - the vocabulary is vendor-specific and must not fire on another printer's coincidental output. |
| 3 | RESPOND completion | Regex matching `print` adjacent to `start`/`started`/`starting` in either word order. Many users end PRINT_START with `RESPOND MSG="Print started!"` - an authoritative COMPLETE. |
| 4 | Profile signal formats | Exact prefix + value lookup (e.g. Forge-X `// State: HOMING...`). |
| 5 | PRINT_START marker | Regex: `PRINT_START\|START_PRINT\|_PRINT_START` (case-insensitive). Sets INITIALIZING once per session. |
| 6 | Engine probe heuristics | `// Adapted probe count: N,M` and `probe at X,Y is z=Z` lines are consumed as mesh data - they update the probe counters and never reach the profile patterns. |
| 7 | Profile regex patterns | `response_patterns` from the loaded profile JSON. |
| 8 | Built-in fallback | Not a stage of its own. When `default.json` cannot be read or parsed, `load_default()` returns `make_builtin_default()`, and that compiled-in profile becomes the one this chain consults. |

Structured state and physical inference never pass through this chain: they arrive as status frames on `notify_status_update`, not as console lines, and are handled beside it (`handle_phase_object_status()`, `handle_status_signals()` in `src/print/print_start_collector.cpp`).

How evidence arbitrates once matched:

- **`signal_formats` matches always apply.** Each hit calls `update_phase()` directly - this is what lets a sequential profile re-announce a phase that recurs (Forge-X passes through CLEANING several times), with a monotonic-progress guard so the bar never regresses.
- **Pattern and status-signal matches apply only the first time a phase is detected** (`apply_profile_match()` checks the detected set). The exception is BED_MESH sub-phase relabeling: a new message while already in BED_MESH updates the label and restarts the probe counter.
- **Weights do not pick winners between live matches** - the first source to reach a phase claims it. Weights size each phase's share of the progress bar (and double as the progress value in sequential mode); they are progress arithmetic, not arbitration.

### Progress Modes

| Mode | When to use | How it works |
|------|-------------|--------------|
| **`weighted`** | Unknown printers, generic macros | Each phase's weight is its share of the bar, and a missing phase simply contributes nothing. Once the ETA engine has composite weights, `calculate_progress_locked()` credits completed phases their full share and the in-flight phase a partial one (heating by attainment, bed mesh by probe count, everything else by elapsed against its prediction); before that it falls back to the plain sum of detected profile weights. Either way the result is capped at 95% until COMPLETE. |
| **`sequential`** | Known firmware with deterministic output | Each signal maps to a specific 0-100% value. Progress jumps directly to that value. Smooth, predictable bar for printers we've profiled. |

---

## All Phases

These are the `PrintStartPhase` enum values from `include/print_start_phase.h`. Use the **string name** (case-insensitive) in profile JSON files. The ints are an implementation detail - they order the sequence and nothing else - so profiles name phases and never number them.

| Enum Value | Int | String Name | Typical Trigger | Default Weight |
|------------|-----|-------------|-----------------|----------------|
| `IDLE` | 0 | `IDLE` | Not in PRINT_START | - |
| `INITIALIZING` | 1 | `INITIALIZING` | PRINT_START detected | - |
| `HOMING` | 2 | `HOMING` | G28, Home All Axes | 10 |
| `HEATING_BED` | 3 | `HEATING_BED` | M190, M140 S>0 | 20 |
| `SOAKING` | 4 | `SOAKING` | Heat-soak dwell, chamber temperature wait | - |
| `HEATING_NOZZLE` | 5 | `HEATING_NOZZLE` | M109, M104 S>0 | 20 |
| `QGL` | 6 | `QGL` | QUAD_GANTRY_LEVEL | 15 |
| `Z_TILT` | 7 | `Z_TILT` | Z_TILT_ADJUST | 15 |
| `BED_MESH` | 8 | `BED_MESH` | BED_MESH_CALIBRATE | 10 |
| `CLEANING` | 9 | `CLEANING` | CLEAN_NOZZLE, WIPE_NOZZLE | 5 |
| `PURGING` | 10 | `PURGING` | VORON_PURGE, LINE_PURGE | 5 |
| `COMPLETE` | 11 | `COMPLETE` | Layer 1 detected, HELIX:READY | - |

Aliases accepted alongside the canonical names, for macros already emitting them (`kPrintStartPhaseAliases`): `START`/`STARTING` (INITIALIZING), `DONE` (COMPLETE), `BED_HEATING` (HEATING_BED), `HEAT_SOAK`/`SOAK` (SOAKING), `NOZZLE_HEATING`/`HEATING_HOTEND` (HEATING_NOZZLE), `QUAD_GANTRY_LEVEL` (QGL), `Z_TILT_ADJUST` (Z_TILT), `BED_LEVELING` (BED_MESH), `NOZZLE_CLEAN` (CLEANING), `PURGE`/`PRIMING` (PURGING).

Notes:
- `IDLE` and `COMPLETE` are lifecycle states, not matchable phases in profiles.
- `INITIALIZING` is set automatically when PRINT_START is detected. You can also map signals to it for firmware that has distinct pre-homing steps.
- A single phase can be triggered multiple times (e.g., CLEANING appears 6 times in Forge-X). In sequential mode each hit updates the progress. In weighted mode only the first detection counts.

---

## Profile JSON Schema

Profiles live in `assets/config/print_start_profiles/{name}.json`. Every key except `name` is optional; a profile with only `response_patterns` is the common case.

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

  // OPTIONAL: A status object whose string field carries phase state -
  // for firmware or mods that publish an operation-context object
  // instead of (or alongside) narrating the console
  "phase_object": {
    "object": "operation_context",   // status object name; subscribed automatically
    "field": "current_state"         // string field inside it
  },

  // OPTIONAL: Regex patterns matched against the phase object's field
  // value. Same entry shape as response_patterns (pattern / phase /
  // message / weight) - regex over a published state, not a console line.
  // Omit it and the state is matched against response_patterns instead.
  "state_patterns": [
    {
      "pattern": "LEVELING",
      "phase": "BED_MESH",
      "message": "Bed Mesh...",
      "weight": 25
    }
  ],

  // OPTIONAL: Edge-triggered physical predicates over status frames -
  // evidence for windows nobody narrates (a filament-change move, a
  // heater coming up from cold)
  "status_signals": [
    {
      "name": "at_cutter",              // rule id, used in logs
      "object": "toolhead",             // status object; subscribed automatically
      "when": [                         // AND-list of predicates over one object
        {"field": "position", "index": 0, "op": "lt", "value": 0.0},
        {"field": "position", "index": 1, "op": "lt", "value": 0.0}
        // RHS alternative: "ref_field": "<sibling field>" + optional "offset",
        // e.g. temperature more than 2 below target
      ],
      "phase": "PURGING",
      "message": "Changing filament...",
      "weight": 30
    }
  ],

  // OPTIONAL: Time-based phase advancement for firmwares that run
  // cleaning/purge as silent macros - nothing narrates between
  // heat-complete and first layer. Entries fire in order, N seconds
  // after both heaters reach target. No shipped profile uses it yet.
  "silent_progression": [
    {"phase": "PURGING", "message": "Purging...", "after_temps_ready_seconds": 8}
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

Malformed entries warn in the log and are skipped - one bad rule never takes down the profile, and the rest of the file still loads.

### Field Details

**`signal_formats`** - Best for firmware that outputs structured state lines (like Forge-X's `// State: HOMING...`). The prefix is matched with `string::find()`, not regex, so it works even if the line has other content before the prefix. The value after the prefix must match a mapping key **exactly** (case-sensitive, including trailing punctuation like `...`).

**`response_patterns`** - Best for catching G-code commands and freeform console output. Patterns are compiled with `std::regex::icase`. Capture groups (`$1`, `$2`, etc.) in the message template are substituted with matched groups. Each pattern is checked via `std::regex_search` (partial match, not full line). When several patterns match the same line, the first one in file order wins.

**`phase_object` + `state_patterns`** - Best for a printer that publishes its phase as a status string rather than, or as well as, printing it to the console. Two shapes qualify. Firmware or a mod may expose an operation-context object with a state field, which Forge-X does. Stock Klipper always exposes `display_status.message`, which is where a `PRINT_START` macro's own `SET_DISPLAY_TEXT` / `M117` narration lands - and since Klipper does not echo the commands a macro runs, that field is often the only place a macro-driven start is legible at all, which is why the generic profile declares it.

The declared object is subscribed automatically during discovery. Klipper notifies on every field change in the object, so an unchanged state re-arrives regularly; only a NEW state applies, and the latch keeps the last MATCHED state - an unmapped state between two mapped ones is not a change of phase. The state string is matched by `state_patterns`, which share the exact `pattern`/`phase`/`message`/`weight` contract with `response_patterns` (first pattern in file order wins).

**A profile that declares a phase object but no `state_patterns` matches the state against its `response_patterns` instead** (`src/print/print_start_profile.cpp#try_match_state`). The two feeds carry one phase vocabulary, so a profile whose console and status wording overlap - the generic one, where both feeds say "Homing" and "Bed Mesh" - needs a single list and has no second copy to keep in step. Declaring `state_patterns` is how a profile whose two feeds need different text overrides that. A profile without `phase_object` ignores the frames entirely - the handler is a no-op, which is the fallback guarantee.

**`status_signals`** - Edge-triggered physical predicates, for windows nobody narrates (a filament-change move, a heater coming up from cold). `when` is an AND-list over one object's status fields: `field` is a dot-path into the object, `index` optionally selects an array element, `op` is one of `eq ne gt lt near` (`tolerance` is read by `near`). The right-hand side is either a literal `value` or a sibling field of the same object (`ref_field`, plus optional `offset`) - which is how "temperature is more than 2 below target" is expressed. A rule fires on the false->true transition and re-arms when the predicate drops: physical states hold for windows while the frame keeps re-delivering them, console lines are events, and the engine treats them accordingly. A malformed rule is skipped **whole** - an AND that silently dropped one condition would widen the match. As with `phase_object`, a profile with no `status_signals` block never evaluates a frame.

**`phase_weights`** - Only meaningful in `weighted` mode. If omitted, phases matched by response_patterns use their individual `weight` field. If provided, this map is used by `calculate_progress_locked()` to sum detected phase weights.

**`adaptive_meshing` / `position_signals` / `cfs_signals`** - Toggles for the engine's built-in heuristic families (adaptive bed-mesh probe counting, nozzle-position inference through a silent prep window, Creality CFS tag lines). Only enable what you have verified on the hardware.

**`message` strings are English translation tags** - they pass through `lv_tr()` at match time (`src/print/print_start_profile.cpp#match_pattern_list` for pattern and state hits, `#evaluate_status_signal` for inference rules), so a loaded language pack resolves them like the built-in labels. An untranslated tag displays as-is.

**The extractor never reads profile JSON.** `scripts/translations/extractor.py` globs `*.cpp`, `*.h` and `*.xml` only, so a message string that exists nowhere but a profile file is not a key in any language pack and ships English in every locale. The generic profile's strings escape this because `make_builtin_default()` spells the same wording in C++ under `lv_tr()`, where the extractor sees it. So: reuse the wording from an existing profile where the phase is the same. That keeps the panel's message from varying by printer for the same event, and it is also what keeps a new profile translated. Wording that genuinely has no precedent has to reach the extractor from a `.cpp`, `.h` or `.xml` file as well, or it stays English.

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

Write the profile against what the machine actually says, not what its docs claim. Every step below assumes you have a real capture in hand before you write a line of JSON.

### Step 1: Capture a real start

Run a print on the target printer and save the full console output - from `START_PRINT` to first extrusion:

```bash
./build/bin/helix-screen -vvv  # TRACE level shows all G-code responses
```

Or check Moonraker's console / gcode store, or klipper's `printer.log`. Then read the capture and ask, in this order (strongest evidence first):

- Does the firmware or a mod publish a status object carrying state, or does the macro write its progress to `display_status.message` with `SET_DISPLAY_TEXT` / `M117`? (`phase_object` candidate - and on a macro-driven Klipper printer the console will be nearly empty, so check this first)
- Do the macros print structured state lines? (`signal_formats` candidate)
- What G-code commands and messages are visible in the console? (`response_patterns` candidate)
- Are there gaps nobody narrates - a filament-change window, a silent mesh sweep? (`status_signals` candidate; inference is the last resort, not the first)
- Is the sequence deterministic? (sequential mode candidate)
- How long does each phase take roughly? (helps set weights)

### Step 2: Create the profile JSON

Create `assets/config/print_start_profiles/{name}.json`. Choose your approach from what the capture shows:

**Structured output (sequential mode)** - If the firmware emits structured lines like `// State: HOMING...` or `[STATUS] Heating bed`, use `signal_formats` with `sequential` progress mode. Map each state to a progress percentage based on roughly how far through the prep sequence it occurs.

**Published state object** - If a mod publishes an operation-context object, or the macro narrates through `display_status.message`, declare `phase_object`. Map each published state to a phase with `state_patterns`, or omit `state_patterns` and let the state fall through to `response_patterns` when both feeds use the same words.

**Freeform output (weighted mode)** - If the firmware just outputs standard G-code commands and messages, use `response_patterns` with `weighted` mode. Assign weights based on how long each phase typically takes.

**Gaps** - Windows the capture shows but nobody narrates get `status_signals` predicates over position/temperature facts. Keep the trust table above: inference confirms, it does not replace narration.

**All of the above combine** - signal formats, response patterns, phase object, and status signals can coexist in one profile. On a console line, signal formats are checked first (priority 4), response patterns last (priority 7); status evidence arrives on the status stream beside the chain.

Whatever you choose, reuse message strings from an existing profile where the phase is the same - the panel's wording should not vary by printer for the same event.

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

If a printer has no `print_start_profile` field, or the profile fails to load, the system falls back to `default.json`, then to the compiled-in profile `make_builtin_default()` builds. This three-level fallback chain means nothing ever breaks. The compiled-in copy is hand-maintained, and the `[parity]` test described under "Existing Profiles" below is the only thing holding it level with the JSON.

### Step 4: Add to PrinterDetector (if new printer)

If this is a brand new printer type, you also need detection heuristics in `printer_database.json` so HelixScreen can identify the printer. See existing entries for the pattern (macro matches, sensor matches, object matches, etc.).

### Step 5: Pin it with tests, then prove they can fail

```bash
# Run profile-specific tests
./build/bin/helix-tests "[profile]"

# Run all print start tests
./build/bin/helix-tests "[print]"
```

Write tests in `tests/unit/test_print_start_profile.cpp` that:
1. Load your profile by name
2. Test every signal format mapping and state pattern
3. Test response patterns with realistic console output - the captured lines, the honest ones
4. Test noise rejection (lines that should NOT match, including captured lines that must not announce a phase)

For a profile authored from a capture, also add a regression file like `tests/unit/test_print_start_profile_k2.cpp` - the whole captured narration, pinned line by line. And for object/signal rules, add collector-level cases in `tests/unit/test_print_start_collector.cpp`: feed a status frame, assert the phase, and assert the frame is IGNORED for a profile without the declaration - that is the fallback guarantee.

Then prove the tests can fail: break a mapping, watch the suite go red, restore (see `tests/CLAUDE.md` - a green suite is not evidence). Commit profile + database entry + tests together.

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
        "READY_TO_PRINT":  { "phase": "COMPLETE",       "message": "Starting print...",      "progress": 100 }
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

## Debugging a Profile

Run the app with `-vv`: `PrintStartProfile` logs every signal-format match (`Signal match: ... -> phase=`) and the collector logs phase transitions. The two status feeds log at the same level, each naming what it matched - `Phase-object state '<state>' -> phase N` and `Status signal '<rule name>' held -> phase N` - so a silent feed is distinguishable from one that fires and is ignored. `-vvv` adds the raw G-code response lines and the pattern-level matches (`Pattern match: ...`). A phase that never arrives is usually a pattern that does not match the real spelling - diff your regex against the captured line, not against memory, and remember that on a macro-driven Klipper printer the line you want may only ever appear in `display_status.message`.

---

## Existing Profiles

| Profile | File | Mode | Printers | Key Feature |
|---------|------|------|----------|-------------|
| **Generic** | `default.json` | weighted | All unrecognized printers (70+ database entries carry no profile field) | 9 regex patterns, serving both the console and `display_status.message` via `phase_object`, + 2 rising-edge heating `status_signals`; conservative by design |
| **Forge-X** | `forge_x.json` | sequential | FlashForge AD5M / AD5M Pro on Forge-X | 14 `// State:` signal mappings + 2 temperature regex patterns + `phase_object` (10 state patterns) + 2 `status_signals` (at_cutter, at_chute) |
| **AD5M stock** | `ad5m.json` | weighted | FlashForge Adventurer 5M, AD5M Pro (stock firmware) | 1 signal format + 7 regex patterns |
| **Creality K1 family** | `creality_k1.json` | weighted | K1, K1C, K1 Max (+ CFS variants) | `position_signals` for the silent prep window |
| **Creality K2** | `creality_k2.json` | weighted | K2 Plus, K2 Pro | `cfs_signals` + `position_signals` + `adaptive_meshing` - all three heuristic families |
| **QIDI** | `qidi.json` | weighted | Q1 Pro, X-Max 3, Plus 4, Max 4, ... | 9 regex patterns |
| **Anycubic Kobra** | `anycubic_kobra.json`, `anycubic_kobra_s1.json` | weighted | Kobra 2 Pro / 3 family, Kobra S1 (+ Max) | 6 regex patterns each |
| **Artillery M1** | `artillery_m1.json` | sequential | Artillery M1 Pro | 1 signal format + 4 regex patterns |
| **Snapmaker U1** | `snapmaker_u1.json` | weighted | Snapmaker U1 | 2 signal formats + `adaptive_meshing`; patterns captured live, none invented for silent steps |
| **Built-in fallback** | `make_builtin_default()` | weighted | Emergency fallback when `default.json` is unreadable | Same decisions as `default.json`, compiled into the binary and pinned there by the `[parity]` test below |

The generic profile exists twice, once as JSON and once as C++, so something has to hold the
two copies together: `PrintStartProfile: the built-in fallback matches the shipped
default.json` in `tests/unit/test_print_start_profile.cpp`, tag `[profile][print][parity]`.
It compares **decisions, not regex text** - each copy may spell a pattern its own way, and
what is pinned is where a given string lands. The test walks a corpus of both feeds'
vocabulary (command echoes like `M190 S60`, macro prose like `Heating Bed: 100c`, and lines
neither may claim such as `BED_MESH_CLEAR`) through `try_match_pattern()` and
`try_match_state()` on both profiles and requires the same phase and message from each; it
also pins the phase object and its subscription list, the heater inference rules field by
field, and the phase weights. Editing `default.json` without editing
`make_builtin_default()` turns it red, and that is the whole mechanism - there is no code
path that derives one from the other.

---

## Fallback Completion Detection

For printers that don't emit any G-code layer markers (like Forge-X), the system has additional fallback completion signals, armed a few seconds after `start()` - later than the G-code response path, so console detection always gets first crack.

| Fallback | Condition | When |
|----------|-----------|------|
| Layer edge | `print_stats.info.current_layer` 0 -> 1 (or the counter advancing) while >= 1 | Authoritative when the printer reports layers; the gate rejects a stale value carried over from the previous print |
| First extrusion | `print_stats.print_duration > 0` | Printers that never report a layer field |
| Adaptive timeout + temps | Elapsed past the predicted total (x1.5 margin) AND temps >= 90% of target AND quiet for 90s | Predictions available |
| Predicted ceiling | Elapsed past `max(predicted x2.5, 1800s)` AND quiet for 90s, no temp gate | Predictions available |
| Flat timeout + temps | Elapsed > 300s, same temp and quiet gates | No prediction data |
| Absolute ceiling | 1800s regardless of chatter or temperature | No prediction data - stuck detection, the one timeout with no quiet requirement |
| Macro variables | `_START_PRINT.print_started`, `START_PRINT.preparation_done`, `_HELIX_STATE.print_started` | Subscribed via Moonraker |

Timeouts are deliberately reluctant: active mesh probing suppresses them, and a pre-print that is still narrating itself is never timed out on the clock alone (only the absolute ceiling ignores that).

---

## Key Files

| File | Purpose |
|------|---------|
| `include/print_start_profile.h` | Profile class: structs, factory methods, matching API |
| `src/print/print_start_profile.cpp` | JSON parsing, signal/pattern/state/signal matching, built-in fallback |
| `include/print_start_collector.h` | Collector: lifecycle, phase tracking, profile + predictor integration |
| `src/print/print_start_collector.cpp` | Detection engine: priority chain, status-frame handlers, progress calculation, ETA timer |
| `include/preprint_predictor.h` | Pure-logic ETA predictor using historical timing data |
| `src/print/preprint_predictor.cpp` | Config integration, caching for predictor |
| `include/print_start_phase.h` | `PrintStartPhase` enum, canonical names, alias table |
| `include/printer_state.h` | Subject accessors |
| `include/printer_print_state.h` | Print domain: progress, layers, preprint ETA subjects |
| `src/application/moonraker_manager.cpp` | Wiring: profile loading, observer setup |
| `src/api/moonraker_discovery_sequence.cpp` | Subscribes the profile's declared status objects during discovery |
| `include/printer_detector.h` | `get_print_start_profile()` declaration |
| `src/printer/printer_detector.cpp` | Database lookup for profile name |
| `assets/config/print_start_profiles/*.json` | Profile definitions |
| `assets/config/printer_database.json` | Maps printer IDs to profile names |
| `tests/unit/test_print_start_profile.cpp` | Profile loading + matching tests (table tests, `[profile][print]`), including the built-in fallback parity test (`[parity]`) |
| `tests/unit/test_print_start_profile_k2.cpp` | Captured-lines regression: the K2 narration pinned line by line |
| `tests/unit/test_print_start_collector.cpp` | Integration tests with collector |
| `tests/unit/test_preprint_predictor.cpp` | Predictor unit tests (weighting, FIFO, edge cases) |
| `docs/devel/PRINT_START_INTEGRATION.md` | User-facing setup guide |
| `docs/devel/PRINT_START_OBSERVERS.md` | The whole pre-print observer system: five signal sources, threading, tests |

---

## Thread Safety Notes

- `set_profile()` must be called **before** `start()`. It is rejected (with a warning) if the collector is active.
- `profile_` is a `shared_ptr` that is read-only after `start()`. No mutex needed for reads.
- `state_mutex_` protects `detected_phases_`, `current_phase_`, `print_start_detected_`, and `printing_state_start_`. All writes happen under the lock.
- `update_phase()` calls `state_.set_print_start_state()` **outside** the lock (it posts to the UI thread via `ui_queue_update()`).
- WebSocket callbacks run on a background thread - both `on_gcode_response` and the status-frame handlers (`handle_phase_object_status()`, `handle_status_signals()`) - so a profile match never touches LVGL directly. `check_fallback_completion()` runs on the main thread from the ETA timer.

---

## Pre-Print ETA Prediction

The `PreprintPredictor` uses historical timing data from previous prints to estimate how long the remaining preparation will take. It integrates with `PrintStartCollector` and is exposed as subjects on `PrinterPrintState`. This is the summary; the engine is documented in full in [PREPRINT_PREDICTION.md](PREPRINT_PREDICTION.md).

### How It Works

1. **During PRINT_START**, the collector records timestamps when each phase is entered
2. **On completion**, `save_prediction_entry()` writes one entry to config (`/print_start_history/entries`): the per-phase durations, minus the two heating phases, plus the wall clock of the whole measured window as `total`
3. **On next print**, the predictor loads the entries matching this print's measurement window and temperature bucket, and averages them with an exponential time decay - weight `exp(0.23 * i)` for entry `i`, oldest first, normalized to sum to 1 - so the newest entry counts most and any entry count works
4. **Real-time remaining** is computed by the collector rather than the predictor alone: `compute_predicted_weights()` combines the predictor's per-phase shape with the thermal model's heating estimate, and `update_eta_display()` counts the result down every 5 seconds

### Key Design Decisions

- **Pure logic class**: `PreprintPredictor` has no LVGL or Config dependencies, making it fully unit-testable
- **FIFO with cap**: keeps the newest `MAX_ENTRIES` (10) entries of the bucket it loaded
- **No total-duration cap**: outliers are rejected per phase by median absolute deviation, so one anomalous probe sequence does not discard the homing data recorded beside it
- **Phase subset handling**: Redistributes weights when phases appear in only some entries
- **60-second cache**: `predicted_total_from_config()` caches parsed config to avoid repeated JSON parsing

### Files

| File | Purpose |
|------|---------|
| `include/preprint_predictor.h` | Pure prediction logic, weighted averages |
| `src/print/preprint_predictor.cpp` | Config integration, caching |
| `tests/unit/test_preprint_predictor.cpp` | Prediction unit tests: weighting, FIFO, window and temp-bucket filtering, remaining-time edge cases |
| `include/printer_print_state.h` | Exposes `preprint_remaining_` and `preprint_elapsed_` subjects |

---

## Future Extensions

- **User-editable profiles**: `config/print_start_profiles.d/` for user overrides (same pattern as `printer_database.d/`)
- **Temporal predicates**: windowed conditions for `status_signals` (Z oscillation -> BED_MESH probing, sustained zero velocity with hot heaters -> soak) - needs an `over_ms` condition type
- **Voron profile**: Map Voron `status_*` LED macro calls to phases
- **Bambu/Prusa profiles**: For future printer support
