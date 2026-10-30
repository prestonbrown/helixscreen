# Print Start Profiles (Developer Guide)

How HelixScreen turns "the printer is preparing a print" into a truthful status line, a progress bar, and an ETA - across firmwares that narrate everything, firmwares that narrate nothing, and firmwares in between: the observer pipeline and its signal sources, the evidence kinds, the full field and phase reference, and how to author a profile for a new printer.

Two sibling docs own the adjacent detail:

- [PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md) - user-facing setup: macros, slicer options, controllable pre-print operations, troubleshooting. That is the modder's half of the same system: this guide covers the profile side (what HelixScreen ships and how to extend it), that one covers making a user's existing macros legible to HelixScreen with no profile at all.
- [PREPRINT_PREDICTION.md](PREPRINT_PREDICTION.md) - the ETA engine: phase timing, thermal model, weighted history buckets.

---

## Architecture Overview

```
assets/config/printer_database.json        assets/config/print_start_profiles/
  ┌──────────────────────────────┐           ┌───────────────────┐
  │ "flashforge_adventurer_5m_   │──refs────> │ forge_x.json      │ Context-qualified phase_object
  │   forgex" entry              │           │                   │ + "// State:" signals
  │ "creality_k2_plus" entry     │──refs────> │ creality_k2.json  │ CFS tags + position + adaptive mesh
  │ "voron_24" entry (no field)  │──(none)──> │ default.json      │ Automatic fallback
  │ (unknown printer)            │──(none)──> │ default.json      │ Automatic fallback
  └──────────────────────────────┘           └───────────────────┘
                                                    │
                              PrintStartCollector <──┘
                                ├─ HELIX:PHASE:* signals (universal, always highest priority)
                                ├─ Profile signal formats (prefix + value lookup)
                                ├─ Profile regex patterns (response_patterns)
                                └─ Profile status-object evidence (phase_object, status_signals)
```

The active profile is loaded by `PrintStartProfile::load()` and drives `PrintStartCollector`. Any status objects it declares (`phase_object`, `status_signals`) are subscribed generically during discovery - `PrintStartProfile::required_status_objects()` feeds `MoonrakerDiscoverySequence::build_subscription_objects()` (`src/api/moonraker_discovery_sequence.cpp`), so a profile adding a status dependency needs no C++ change.

### The Pipeline

```
                     ┌────────────────────────────────────────────────┐
 arming (when to     │ MoonrakerManager::init_print_start_collector() │
 listen)             │ + PrintCollectorArming (print_stats edge)      │
                     └───────────────────────┬────────────────────────┘
                                             │ start()/stop()
                                             ▼
 signal sources ───────────────────▶ PrintStartCollector ──▶ PrinterState subjects
 (below, all gated                     (phase machine,         (phase, message,
  on active_)                           probe counters,        remaining, progress)
                                        position classifier,
                                        ETA/easing)
                                             │
                                             ▼
                                    print-status panel UI (XML-bound)
```

The collector is a `shared_ptr` owned by `MoonrakerManager`, recreated on every printer switch.

### Signal Sources

| # | Source | Arrives on | Gate | Effect |
|---|--------|-----------|------|--------|
| 1 | `notify_gcode_response` lines | WS thread (client callback) | active + profile patterns | phase transitions, messages; `real_signal_seen_` latches and mutes proactive detection |
| 2 | `probe at X,Y is z=Z` lines (subset of #1) | WS thread | active, in/pre-mesh | mesh point counting (N/M); buffered until `kMeshProbeEntryThreshold` distinct points confirm a sweep, then credited; intercepted BEFORE pattern matching so they can never re-announce BED_MESH and reset the denominator |
| 3 | bed-mesh presence flap (`bed_mesh.probed_matrix` present→absent) | WS thread (`MoonrakerAPI` bed_mesh callback → `set_bed_mesh_presence_observer`) | active + phase==CLEANING | enters BED_MESH ("Bed Meshing...") + fetches the probe denominator; observer copied under a mutex (weakly-ordered targets read stale-null otherwise) |
| 4 | toolhead position subjects (`toolhead.position`, already subscribed) | main thread (queued status updates; 3 permanent `ObserverGuard`s) | active + profile `position_signals` | `PrintStartPositionClassifier`: centre Z-descents → "Probing Z...", ≥3-distinct-corner tour → "Checking Bed Mesh...", row march → BED_MESH entry |
| 5 | heater targets / temps / `homed_axes` / layers / progress (fallback observers) | main thread (subject observers) | active + `enable_fallbacks()` + NOT `real_signal_seen_` | proactive homing (partial `homed_axes`, heaters or not) and heating phases, adaptive timeouts, completion fallbacks |
| 6 | configured leveling points (`[quad_gantry_level] points`, `[z_tilt] points`, read by `PrinterDiscovery::parse_leveling_probe_points()` and copied in `start()`) matched against #2 and #4 | WS thread (probe lines), main thread (position) | active, any profile, points configured, before BED_MESH | a `probe at` line within 1.5mm of a configured point applies QGL ("Leveling Gantry...") or Z_TILT ("Z Tilt Adjust...") and never reaches the mesh counters; any other probe line is mesh data whatever the current phase. Without a probe line, Z dropping between samples at two distinct points applies the same phase (`include/leveling_probe_points.h`). Inference: never marks a real signal. With no points configured, every probe line while QGL/Z_TILT is current is taken as leveling |
| 7 | stationary purge: position + `live_extruder_velocity` subjects, filament-system action and loaded subjects | main thread (position/velocity/action observers, ETA tick) | active, not PURGING/COMPLETE | forward flow held unbroken for `kPurgeFlowSustainMs` with XY still against the stretch's first sample and Z never dropping (a blob purge or a chute) applies PURGING once the heaters are at target; a filament system mid load/unload/select/cut/tip, or reporting nothing loaded, vetoes it, and one reporting `AmsAction::PURGING` applies it outright (`include/print_start_purge_shape.h`) |
| 8 | `virtual_sdcard.file_position` frames (subscribed for every printer) | WS thread (status frames); the head read on the main thread, its answer on the transfer's thread | active + profile `scripted_start` | the start-block step the print stands on or has passed: Homing axes / Probing Z / Bed mesh / Cleaning Nozzle / Purging, as a live state (`include/start_block_steps.h`, `scripted_start` under Field Details) |

Sources 3 and 4 exist because Creality K1-class firmware forwards nothing to the console for ~3 minutes of Z-probing, corner validation, and mesh sweep - while sources that ARE not console output keep flowing. They only fill silence: a real console marker always outranks them, and their refinements are message-only unless the sweep march promotes BED_MESH (the same edge as the flap, so the two corroborate each other).

### Arming and Windows

`should_start_print_collector()` arms on a STANDBY→PRINTING transition with zero progress AND zero `print_duration` (joining a print mid-job must not raise the pre-print overlay). `PrintStartCollector::note_host_side_pre_start()` declares that the dispatch came from our own pre-start gcode block; the prediction history then re-filters to the host-pre-start bucket so its minutes of extra work never average into printer-edge prints (and vice versa). The user-visible side of the two arming paths - what each covers and when each opens - is in [PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md).

### The Three Kinds of Evidence

A print start is narrated by somebody. Which of the three evidence kinds a printer offers decides how its profile is written - this is the *why* behind the priority chain below:

| Kind | What it is | Schema fields | Trust |
|------|------------|---------------|-------|
| Narration | Console lines the firmware or macros print | `signal_formats`, `response_patterns` | What the printer SAYS - authoritative |
| Structured state | A status object carrying phase strings | `phase_object` + `state_patterns` | What the printer PUBLISHES - authoritative |
| Physical inference | Predicates over status frames | `status_signals` | What the printer IS DOING - confirmation for gaps nobody narrates, never a replacement for narration |

The line between the first two kinds and the third is enforced in code, not only in review. A narration match - console line or phase-object state - marks a real firmware signal, which tells the collector the printer is narrating its own sequence and switches off the proactive temperature detector in `check_fallback_completion()`. A `status_signals` match deliberately does not: `handle_status_signals()` passes `marks_real_signal=false` (`src/print/print_start_collector.cpp#handle_status_signals`), because a rule's predicates read the same heater and toolhead frames that detector reads, and counting one as narration would silence the detector with its own input - costing the HOMING and heating phases only it can supply on a printer that narrates nothing.

Phase semantics are engine-owned: the `PrintStartPhase` enum (`include/print_start_phase.h`) is the whole vocabulary, and profiles never invent phases - they only map evidence onto it.

Discipline for inference rules: an unambiguous physical fact (the head parked at a cutter's corner) may carry a high weight; an ambiguous one (a heater at target) is a tie-breaker at most - or stays out of the profile. A confident wrong phase is worse than an honest generic bar. `default.json` follows this rule: it infers only the two rising-edge heating facts and filament moving forward with the head moving once both heaters are at target (`motion_report.live_extruder_velocity` and `live_velocity` above 0 with `after_heat`, read as the purge or prime line), and deliberately omits anything more ambiguous. Extrusion with the head still is a load push or a stationary purge, which the engine's purge-shape check (Signal Sources row 7) tells apart.

### Detection Priority Chain

Every G-code response line is checked in this order. First match wins, and a matched line is consumed - it does not fall through to a later stage.

| Priority | Source | Description |
|----------|--------|-------------|
| 1 | `HELIX:PHASE:*` | Universal override. Emitted by HelixScreen macros. Always checked, never profile-specific. |
| 2 | Profile tag patterns | `tag_patterns`: lines a firmware prints to announce a step outright (the K2's `// num: N, velocity: V, percent F` purge progress and `[box]` load events). A match sets the phase directly; a `progress_group` makes the line's own number the progress. Only the profile that declares a pattern matches it, so another printer's coincidental output stays inert. |
| 3 | RESPOND completion | Regex matching `print` adjacent to `start`/`started`/`starting` in either word order. Many users end PRINT_START with `RESPOND MSG="Print started!"` - an authoritative COMPLETE. |
| 4 | Profile signal formats | Exact prefix + value lookup (e.g. Forge-X `// State: HOMING...`). |
| 5 | PRINT_START marker | Regex: `PRINT_START\|START_PRINT\|_PRINT_START` (case-insensitive). Sets INITIALIZING once per session. |
| 6 | Engine probe heuristics | `// Adapted probe count: N,M` and `probe at X,Y is z=Z` lines are consumed as mesh data - they update the probe counters and never reach the profile patterns. A `probe at` line at a configured leveling point is consumed as QGL/Z_TILT instead (Signal Sources row 6). |
| 7 | Profile regex patterns | `response_patterns` from the loaded profile JSON. |

Structured state and physical inference never pass through this chain: they arrive as status frames on `notify_status_update`, not as console lines, and are handled beside it (`handle_phase_object_status()`, `handle_status_signals()` in `src/print/print_start_collector.cpp`).

How evidence arbitrates once matched:

- **`signal_formats` matches always apply.** Each hit calls `update_phase()` directly - this is what lets a sequential profile re-announce a phase that recurs (Forge-X passes through CLEANING several times), with a monotonic-progress guard so the bar never regresses.
- **Pattern and status-signal matches apply only the first time a phase is detected** (`apply_profile_match()` checks the detected set). The exception is BED_MESH sub-phase relabeling: a new message while already in BED_MESH updates the label and restarts the probe counter.
- **Weights do not pick winners between live matches** - the first source to reach a phase claims it. A pattern's `weight` is the progress value in sequential mode and is unused in weighted mode, where the ETA engine sizes each phase; it is progress arithmetic, not arbitration.

### Progress Modes

| Mode | When to use | How it works |
|------|-------------|--------------|
| **`weighted`** | Unknown printers, generic macros | Each phase's share of the bar comes from the ETA engine's composite weights (thermal model for heating, prediction history for the rest, computed in `start()`), and a missing phase simply contributes nothing. `calculate_progress_locked()` credits completed phases their full share and the in-flight phase a partial one (heating by attainment, bed mesh by probe count, everything else by elapsed against its prediction), capped at 95% until COMPLETE. |
| **`sequential`** | Known firmware with deterministic output | Each signal maps to a specific 0-100% value. Progress jumps directly to that value. Smooth, predictable bar for printers we've profiled. |

---

## Evidence Rubric

Walk this whole list before writing or changing a profile, a collector rule, or a pre-print
inference. Console narration is only one source, and a phase that looks unnarrated is often
plainly visible in another one. For every row, record in the change what the source showed for
each stretch of the real start, including "nothing".

| Source | Where to read it | What it can settle |
|--------|------------------|--------------------|
| The G-code file itself | Moonraker file download, or `GCodeOpsDetector`'s pre-scan in the device log (`Found PRINT_START call at line`, `First extrusion at line`, the operation list) | The exact scripted order when start G-code is inlined (slicer-side start sequences like the U1's): homing, cleaning, mesh, the purge/prime line, with line numbers and byte offsets |
| File position | `virtual_sdcard.file_position` / `progress` | Which scripted line the printer is executing now, against the pre-scan. The printed file may be HelixScreen's modified copy (`.helix_temp/modified_*`), whose offsets differ from the original |
| Macro definitions | `configfile.config` (`gcode_macro *`), the vendor's klipper extras | What a `PRINT_START` or vendor macro really does, and in what order, when the file only calls it |
| Console narration | `server.gcode_store`, the device HelixScreen log (`-vvv`), klippy.log | Firmware/macro lines and action codes, with timestamps |
| Structured state | `display_status.message`, vendor status objects | Published phase strings (`phase_object`) |
| Toolhead position | `toolhead.position`, `motion_report.live_position`, `gcode_move` | Probing descents, corner tours, mesh row marches, parking at a purge bucket or cutter, first-layer height |
| Extrusion | `motion_report.live_extruder_velocity`, E in `live_position` | Purge and prime lines, flow calibration, filament loads: anything that extrudes |
| Heaters | every heater's `target`, `temperature`, `power` | Heating and soak stretches, reheat before priming, per-tool preheats on a toolchanger |
| Fans, tools, filament | fan objects, `toolhead.extruder`, AMS/feeder objects | Tool changes, feeds, cooling steps |
| Print state | `print_stats.state`, `print_duration`, `info.current_layer` | When the printer thinks printing began (often well before layer 1) and the real first-layer edge |
| Timing | timestamps across all of the above | Silent gaps and how long each stretch really lasts |

Rules that come out of the walk:

- **A declared signal outranks an inference.** When the printer or the file positively
  identifies a stretch, use that; infer only what no source identifies.
- **An inference must be checked against every silent gap in a real capture**, not just the one
  it was written for. A quiet-time or position rule that fits the prime line can fire just as
  well inside an earlier unnarrated calibration stretch.
- **Replay tests keep the real timing.** A replay that compresses a two-minute silent gap into
  one tick proves nothing about a rule that depends on silence or elapsed time. Take the gaps
  from the capture's timestamps.
- **No source is also a finding.** Write down which stretches nothing identifies; the bar's
  weighted progress covers those, and a confident wrong label is worse than a generic one.

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
- A single phase can be triggered multiple times (e.g., CLEANING appears 6 times in an older Forge-X's `// State:` lines). In sequential mode each hit updates the progress. In weighted mode only the first detection counts.

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

  // OPTIONAL: Lines that announce a step outright, checked right after
  // HELIX:PHASE and applied directly (no weight arbitration). Same entry
  // shape as response_patterns, plus progress_group (below).
  "tag_patterns": [],

  // OPTIONAL: This printer's prep chain goes silent (no gcode_response
  // markers) while it homes Z, validates the mesh at its corners, and
  // sweeps a calibration mesh - but toolhead.position keeps flowing.
  // The collector classifies that stream and refines the status line
  // through the silence: "Probing Z...", "Checking Bed Mesh...", and
  // sweep-march -> BED_MESH entry. Zones are anchored to the bed-mesh
  // probe area (mesh_min/mesh_max from the bed_mesh status object).
  "position_signals": false,

  // OPTIONAL: The slicer writes the start sequence into the file instead of
  // calling a PRINT_START macro that runs it, so the line at
  // virtual_sdcard.file_position is the step running now. The collector
  // reads the file's head and follows the position through its steps.
  "scripted_start": false,

  // OPTIONAL (with scripted_start): per start-block command word, text a
  // console line contains when that command printed it while running. Such a
  // line refines the step instead of being the previous command's last word.
  "start_block_owned_lines": { "BED_MESH_CALIBRATE": ["BED_PREHEATING"] },

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

      // Progress value (0-100) in sequential mode. Unused in weighted mode.
      "weight": 10
    }
  ],

  // OPTIONAL: A status object whose string field carries phase state -
  // for firmware or mods that publish an operation-context object
  // instead of (or alongside) narrating the console
  "phase_object": {
    "object": "operation_context",   // status object name; subscribed automatically
    "field": "current_state",        // string field inside it
    // OPTIONAL: a field naming the step the state belongs to - an array
    // (a stack, outermost first; its last element is used) or a string.
    // With it set, state_patterns match "<step>/<state>", e.g.
    // "kamp/LEVELING", instead of the bare state.
    "context_field": "context_types"
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
  ]
}
```

Malformed entries warn in the log and are skipped - one bad rule never takes down the profile, and the rest of the file still loads.

### Field Details

**`signal_formats`** - Best for firmware that outputs structured state lines (like Forge-X's `// State: HOMING...`). The prefix is matched with `string::find()`, not regex, so it works even if the line has other content before the prefix. The value after the prefix must match a mapping key **exactly** (case-sensitive, including trailing punctuation like `...`).

**`response_patterns`** - Best for catching G-code commands and freeform console output. Patterns are compiled with `std::regex::icase`. Capture groups (`$1`, `$2`, etc.) in the message template are substituted with matched groups. Each pattern is checked via `std::regex_search` (partial match, not full line). When several patterns match the same line, the first one in file order wins.

**`hold_minutes_group`** - For text that announces a wait nobody will narrate, such as a heat soak printed before a silent `G4`. Set on a `response_patterns` or `state_patterns` entry, it names the capture group holding a number of minutes (`.` decimal, e.g. `10.0`); a group the pattern does not have is ignored with a warning, a zero or unparseable capture holds nothing, and a hold longer than a day is read as a day. Until the hold ends the printer counts as talking, so no timeout fires, and the held time is left out of the elapsed time the ceiling measures (the backstop leaves out at most one ceiling of it). A console line and its `display_status` copy announce one hold, not two. Declare it only for a wait of known length: a wait on a sensor (`M191`, `TEMPERATURE_WAIT`) has none.

**`tag_patterns`** - Same entry shape as `response_patterns`, for lines whose meaning is definitive: checked right after `HELIX:PHASE` (priority 2 above), ahead of RESPOND completion and the weighted patterns, and applied straight to the phase. With `progress_group` naming a capture group, the captured number is the step's progress: a value at or below 1.5 reads as a 0-1 fraction, anything larger as a percent, clamped to 0-100, and the message carries the percent after the pattern's own text.

**`case_sensitive`** - `true` on any pattern entry compiles it case-sensitive; patterns are case-insensitive by default.

**`owns`** - A list of phase names on a `response_patterns` or `state_patterns` entry whose step emits another phase's signals on its own, such as the K2's `[NOZZLE_CLEAR] START`: its nozzle clear parks at the CFS extrude position and pushes filament to check flow, which reads as PURGING through the profile's own `BOX_GO_TO_EXTRUDE_POS` pattern, its purge-percent tag and the stationary-purge motion check. From that line until the step ends, a signal for an owned phase from any of those sources is the step's and changes nothing on display (`helix::OwningStep` in `include/print_start_collector.h`). The step's own phase, INITIALIZING and the heating phases leave it running, since a clean heats for itself; a signal for any other phase (the G28 after it, a mesh) ends it, and so does entering such a phase from any source. Unknown names, non-strings, the entry's own phase, IDLE and COMPLETE are ignored with a warning, and a non-array is ignored whole. Default: owns nothing.

**`counts_probes`** - `false` on a BED_MESH `response_patterns` or `state_patterns` entry whose probing only checks a stored mesh, such as Forge-X's `mesh_validation/CHECKING MESH` (five probes against a 25-point mesh). Its probe lines still count as the printer working, but the label carries no "(N/M)" and no point total is fetched, so it never reads "(5/25)". Default `true`.

**`narrates`** - `false` on a `response_patterns` or `state_patterns` entry whose text names a target rather than a step, such as a macro's `SET_DISPLAY_TEXT MSG="Bed: 90c"`. The match still sets the phase, but like a `status_signals` match it does not count as the printer narrating, so the collector's own homing and heating inference stays on. Default `true`.

**`phase_object` + `state_patterns`** - Best for a printer that publishes its phase as a status string rather than, or as well as, printing it to the console. Two shapes qualify. Firmware or a mod may expose an operation-context object with a state field, which Forge-X does. Stock Klipper always exposes `display_status.message`, which is where a `PRINT_START` macro's own `SET_DISPLAY_TEXT` / `M117` narration lands - and since Klipper does not echo the commands a macro runs, that field is often the only place a macro-driven start is legible at all, which is why the generic profile declares it.

The declared object is subscribed automatically during discovery. Klipper notifies on every field change in the object, so an unchanged state re-arrives regularly; only a NEW state applies, and the latch keeps the last MATCHED state - an unmapped state between two mapped ones is not a change of phase. The state string is matched by `state_patterns`, which share the exact `pattern`/`phase`/`message`/`weight` contract with `response_patterns` (first pattern in file order wins).

**`context_field`** qualifies the state with the step it belongs to. Firmware that nests steps can reuse one state string across them: on Forge-X 1.4.x `current_state` is the innermost context's state, so `LEVELING` set by the print context means "deciding how to level" while the same string inside `kamp` or `bed_level` means the mesh is running, and `FINISHING` ends a nested clean minutes before the print starts. With `context_field` declared the patterns match `<innermost step>/<state>`, which lets a profile anchor every pattern (`^kamp/LEVELING$`). Moonraker sends only the fields that changed, so the collector caches the last value of each field per run (cleared in `start()`) and re-matches the pair when either arrives. A null state - a step that has begun without naming one yet - replaces the outer step's state rather than pairing it with the new step. A context opened before the collector armed is never sent again until it changes, so `start()` queries the object once; that answer fills only fields no frame has delivered, is dropped if a later `start()` has begun another run, and never completes the pre-print: a file without an end macro leaves the last print's stack at `print/PRINTING` until the next start pushes its own.

A context-qualified state is the step running now, not a line that arrived, so it may move the display to any phase, an earlier one included (Forge-X parks and reheats the nozzle after its mesh). While the latest matched one names a pre-print phase, the adaptive and flat timeouts and the first-extrusion fallback wait for the printer to leave that step, or for its context stack to empty; the ceiling and backstop still apply. A bare state gets neither: `display_status.message` keeps the last `SET_DISPLAY_TEXT` long after its step ended. A malformed `context_field` (empty, not a string) drops the whole phase object, since its patterns would only ever miss bare states.

**A profile that declares a phase object but no `state_patterns` matches the state against its `response_patterns` instead** (`src/print/print_start_profile.cpp#try_match_state`). The two feeds carry one phase vocabulary, so a profile whose console and status wording overlap - the generic one, where both feeds say "Homing" and "Bed Mesh" - needs a single list and has no second copy to keep in step. Declaring `state_patterns` is how a profile whose two feeds need different text overrides that. A profile without `phase_object` ignores the frames entirely - the handler is a no-op, which is the fallback guarantee.

**`status_signals`** - Edge-triggered physical predicates, for windows nobody narrates (a filament-change move, a heater coming up from cold). `when` is an AND-list over one object's status fields: `field` is a dot-path into the object, `index` optionally selects an array element, `op` is one of `eq ne gt lt near` (`tolerance` is read by `near`). The right-hand side is either a literal `value` or a sibling field of the same object (`ref_field`, plus optional `offset`) - which is how "temperature is more than 2 below target" is expressed. A rule fires on the false->true transition and re-arms when the predicate drops: physical states hold for windows while the frame keeps re-delivering them, console lines are events, and the engine treats them accordingly. A malformed rule is skipped **whole** - an AND that silently dropped one condition would widen the match. `after_heat: true` holds a rule back until every heater with a target is within 2C of it (the collector's at-target band, read at its fallback tick), for evidence that also appears earlier in a start: the generic profile's extrusion rule uses it, since a bucket purge before the mesh or a tool load also moves filament. As with `phase_object`, a profile with no `status_signals` block never evaluates a frame.

**`adaptive_meshing` / `position_signals`** - Toggles for the engine's built-in heuristic families (adaptive bed-mesh probe counting, nozzle-position inference through a silent prep window). Only enable what you have verified on the hardware.

**`scripted_start`** - For a printer whose slicer profile writes the whole start sequence into the file (the Snapmaker U1), so the commands that do the work never pass through a macro and say nothing on the console. The first `virtual_sdcard.file_position` frame of a run reads the print file's first `kStartBlockHeadBytes` (200 KB) with a ranged read and scans it with `GCodeOpsDetector` keeping every occurrence (`include/start_block_steps.h#scan_start_block`). Each line that runs a step becomes one, at its byte offset, up to the first layer marker:

| Line | Phase | Label |
|------|-------|-------|
| `G28` naming Z and no other axis | HOMING | Probing Z... |
| any other `G28` or homing macro | HOMING | Homing axes... |
| a QGL / Z tilt command | QGL / Z_TILT | the phase's own label |
| `BED_MESH_CALIBRATE`, `G29` or a mesh macro (not `BED_MESH_PROFILE`, `_CLEAR` or `_OFFSET`) | BED_MESH | Bed mesh... |
| a nozzle-clean macro (`CLEAN_NOZZLE` and the other `NOZZLE_CLEAN` keywords) | CLEANING | Cleaning Nozzle... |
| a purge or prime macro, and the first `G1` with a positive E | PURGING | Purging... |

Only the command word counts, so a keyword in a comment or a later token runs nothing. Klipper moves `file_position` past a line only once its command returns, so the position is the start of the line running now, and `StartBlockFollower` shows the last step at or before it. Homing, leveling and meshing block until done, so they show only when the position stands on their line; a position that jumped past one never ran it (the U1's `print_task_config` skips a disabled mesh that way). A clean or purge is queued moves that run while the position stands on a later line, so passing it is enough. A step applies as a live state (`apply_profile_match()` with `live_state`), so it may move the display back to an earlier phase, a Z touch after a clean; a heater wait the position stands on still labels itself from that line (`heater_waited_on()`). The console reaches us before the batched status frame naming the line, so a step arriving within `kStartBlockConsoleGrace` (1 s) of a console line that changed the display needs that line placed. If the profile's `start_block_owned_lines` says the step's command prints it (the U1's mesh prints its soak and scan codes), the line refines the step and the step is dropped. Otherwise the line was the previous command's last word: the step is held and shows once the grace has passed (the next position frame or fallback tick), unless the console changes the display again first. A console line that changes nothing (a repeated action code) places nothing. A step for a run that has since restarted, or after COMPLETE, is dropped, and stop() or a new run cancels a head read in flight. The first frame's `print_stats.filename` names the file to read, the filename subject only when frames have not carried one; the subscribed position also serves the heater-wait line read (`request_wait_line()`), which queries `virtual_sdcard` only before the first frame carries a position. `virtual_sdcard.file_position` is subscribed for every printer; it moves exactly when `progress` does, so it adds no frames.

**`message` strings are English translation tags** - they pass through `lv_tr()` at match time (`src/print/print_start_profile.cpp#match_pattern_list` for pattern and state hits, `#evaluate_status_signal` for inference rules), so a loaded language pack resolves them like the built-in labels. An untranslated tag displays as-is.

**The extractor never reads profile JSON.** `scripts/translations/extractor.py` globs `*.cpp`, `*.h` and `*.xml` only, so a message string that exists nowhere but a profile file is not a key in any language pack and ships English in every locale. So: reuse the wording from an existing profile where the phase is the same. That keeps the panel's message from varying by printer for the same event, and it is also what keeps a new profile translated. Wording that genuinely has no precedent has to reach the extractor from a `.cpp`, `.h` or `.xml` file as well, or it stays English.

**Non-console signals.** Two phase signals do not arrive through `notify_gcode_response`: a bed-mesh status clear while CLEANING enters BED_MESH ("Bed Leveling...", denominator fetched then), and `probe at X,Y is z=Z` lines are consumed as mesh points (never re-matched against `response_patterns`, so a BED_MESH pattern cannot re-announce the phase and reset the probe counters mid-sweep). Both are rows in the [Signal Sources](#signal-sources) table above, which also maps the toolhead-position stream and the fallback observers.

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

Or check Moonraker's console / gcode store, or klipper's `printer.log`. The console is only one
of the sources: walk the whole [Evidence Rubric](#evidence-rubric) for the same start, with
timestamps. Then read the capture and ask, in this order (strongest evidence first):

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

Two more fields give a printer's first print a measured estimate instead of a generic one:

```json
{
  "print_start_default_phases": { "HOMING": 25, "SOAKING": 60, "BED_MESH": 2, "PURGING": 15 },
  "thermal_rates": { "heater_bed": 6.0, "extruder": 0.4 }
}
```

`print_start_default_phases` is seconds per phase the prediction history keeps a duration for (HOMING, SOAKING, QGL, Z_TILT, BED_MESH, CLEANING, PURGING); any other name is ignored with a warning. Its HOMING value is also the homing time the print details estimate shows, never below 20s. The printer's own phase timings replace these from the next completed print on. `thermal_rates` is seconds per degree C per heater (`extruder` or `heater_bed`; any other name is ignored with a warning), used by `ThermalRateManager::apply_archetype_defaults()` in place of its guess from the bed size. The rate a print saves is its whole measured climb, seconds over degrees, with a hold between two climbs (a probing temperature, then the print temperature) left out, blended 70/30 with the saved rate loaded at startup. A completed pre-print, a timeout completion included, saves the rates it measured, but the app loads saved rates only at startup (`Application` calls `ThermalRateManager::load_from_config()`), so until the next restart the database rates stay in use.

If a printer has no `print_start_profile` field, or the profile fails to load, the system falls back to `default.json`. If that is unreadable too (a broken install), `load_default()` returns an empty profile: no text matching, but HELIX:PHASE, RESPOND, the macro ready-flags, heater inference and the timeouts still run. The ESP32 image ships `default.json` alone (`scripts/esp32_stage_assets.py#stage_config`), so every named profile falls back to it there.

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
  ]
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
| **Forge-X** | `forge_x.json` | weighted | FlashForge AD5M / AD5M Pro on Forge-X | `phase_object` on `operation_context` qualified by `context_types` (16 anchored `<context>/<STATE>` patterns; only `print/PRINTING` completes) for 1.4.x, 14 `// State:` signal mappings for older versions, 3 response patterns (priming line, clean moves, bed wait). No `status_signals`: the AD5M has no cutter or chute, and its bed runs -110..110, so a negative-corner rule fires on the mesh's first point |
| **AD5M stock** | `ad5m.json` | weighted | FlashForge Adventurer 5M, AD5M Pro (stock firmware) | 1 signal format + 7 regex patterns |
| **Creality K1 family** | `creality_k1.json` | weighted | K1, K1C, K1 Max (+ CFS variants) | `position_signals` for the silent prep window |
| **Creality K2** | `creality_k2.json` | weighted | K2 Plus, K2 Pro `tag_patterns` (purge percent, `[box]` load events) + `position_signals` + `adaptive_meshing` |
| **QIDI** | `qidi.json` | weighted | Q1 Pro, X-Max 3, Plus 4, Max 4, ... | 9 regex patterns |
| **Anycubic Kobra** | `anycubic_kobra.json`, `anycubic_kobra_s1.json` | weighted | Kobra 2 Pro / 3 family, Kobra S1 (+ Max) | 6 regex patterns each |
| **Artillery M1** | `artillery_m1.json` | sequential | Artillery M1 Pro | 1 signal format + 4 regex patterns |
| **Snapmaker U1** | `snapmaker_u1.json` | weighted | Snapmaker U1 | 2 signal formats (action codes, including the bed soak and mesh codes) + 2 response patterns (homing, the per-tool Z touch) + `adaptive_meshing` + `scripted_start` for the silent cleans and prime line; patterns captured live, none invented for silent steps |
| **COSMOS** | `cosmos_cc1.json` | weighted | Elegoo Centauri Carbon on OpenCentauri COSMOS | 7 response patterns read on the console and through `phase_object` (display_status); the heat soak declares `hold_minutes_group`; the skew check completes the pre-print |

---

## Fallback Completion Detection

For printers that don't emit any G-code layer markers (like Forge-X), the system has additional fallback completion signals, armed a few seconds after `start()` - later than the G-code response path, so console detection always gets first crack.

| Fallback | Condition | When |
|----------|-----------|------|
| Layer edge | `print_stats.info.current_layer` 0 -> 1 (or the counter advancing) while >= 1 | Authoritative when the printer reports layers; the gate rejects a stale value carried over from the previous print |
| First extrusion | `print_stats.print_duration > 0` and no context-qualified phase-object step under way (a priming line extrudes before the print) | Printers that never report a layer field |
| Adaptive timeout + temps | Elapsed past the predicted total (x1.5 margin) AND both heaters at target (within 2°C, and not cooling from more than 5°C above it) AND 90s without pre-print activity AND no context-qualified phase-object step under way | Predictions available |
| Flat timeout + temps | Elapsed > 300s, same temp, activity and step gates | No prediction data |
| Ceiling | Elapsed past `max(predicted x2.5, 1800s)` AND 90s without a matched line or probe line, no temp gate | Always - stuck detection for a heater that never settles |
| Backstop | Twice the ceiling, regardless of chatter or temperature | Always - the one timeout with no quiet requirement |
| Macro variables | `_START_PRINT.print_started`, `START_PRINT.preparation_done`, `_HELIX_STATE.print_started` | Subscribed via Moonraker |
| Start macro returned | `virtual_sdcard.file_position` past 0 after this run showed 0 (read once at `start()`, then followed in frames) | Firmware that runs its own start macro before streaming the file, named in `include/start_macro_injection.h` (OpenCreator on the Creator 5). The collector starts before the new file loads, so a first reading at the last job's end arms nothing until the position shows 0; a resume never shows 0 and is left to the other signals |

Timeouts are deliberately reluctant: active mesh probing suppresses every one but the backstop, and a pre-print that is still narrating itself is never timed out on the clock alone (only the backstop ignores that). Activity for the two deadline timeouts is a matched line (a signal format, a pattern or a phase-object state), a probe line, a `status_signals` rule firing, or a heater reading a degree above its highest yet under its current target; a heater swinging back up to an earlier reading is not a climb. A silent cool-down (M109 to a probing temperature) outlasts the 90s quiet gate, which is why a heater above its target does not count as at target, and a context-qualified step holds the deadline timeouts outright. The ceiling counts only matched lines and probe lines: a rule or a heater reads the same frames a stuck heater produces, so neither holds it open.

---

## Key Files

| File | Purpose |
|------|---------|
| `include/print_start_profile.h` | Profile class: structs, factory methods, matching API |
| `src/print/print_start_profile.cpp` | JSON parsing, signal/pattern/state/signal matching |
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
| `tests/unit/test_print_start_profile.cpp` | Profile loading + matching tests (table tests, `[profile][print]`) |
| `tests/unit/test_print_start_profile_k2.cpp` | Captured-lines regression: the K2 narration pinned line by line |
| `tests/unit/test_print_start_collector.cpp` | Integration tests with collector |
| `tests/unit/test_preprint_predictor.cpp` | Predictor unit tests (weighting, FIFO, edge cases) |
| `docs/devel/PRINT_START_INTEGRATION.md` | User-facing setup guide |

---

## Thread Safety Notes

- `set_profile()` must be called **before** `start()`. It is rejected (with a warning) if the collector is active.
- The collector is a `shared_ptr` owned by `MoonrakerManager`, recreated on every printer switch. Every background-thread entry point holds it via `weak_ptr` (`s_collector`) and drops out if the collector was replaced.
- `profile_` is a `shared_ptr` that is read-only after `start()`. No mutex needed for reads.
- `state_mutex_` protects `detected_phases_`, `current_phase_`, `print_start_detected_`, and `printing_state_start_`. Console lines, probe lines, and the bed-mesh flap fire on the WS thread; the collector serializes everything through `state_mutex_`. `lv_tr` at that call site is the documented #1219 debt family - follow the existing `note_bed_mesh_presence` shape if you add a sibling.
- `update_phase()` calls `state_.set_print_start_state()` **outside** the lock (it posts to the UI thread via `ui_queue_update()`).
- WebSocket callbacks run on a background thread - both `on_gcode_response` and the status-frame handlers (`handle_phase_object_status()`, `handle_status_signals()`) - so a profile match never touches LVGL directly. `check_fallback_completion()` runs on the main thread from the ETA timer.
- Position and fallback observers fire on the main thread via queued subject sets. Their guards are permanent for the manager's lifetime and early-out on `!active_` before locking - the same shape as the heater-target fallback observers.
- `MoonrakerManager::shutdown()` RELEASES (not resets) every observer guard: subjects may already be deinitialized at that point (#579 family). Any new guard must be added there.
- The bed-mesh presence observer is copied under `bed_mesh_presence_mutex_` before invocation - weakly-ordered targets would otherwise read a stale null.

---

## Where the Behavior Is Pinned

| Behavior | Test |
|----------|------|
| K1C mesh sweep keeps its denominator (N/25, no sub-phase wipe) | `test_print_start_collector.cpp` - K1C replay fixture |
| Flap during CLEANING enters Bed Meshing | `test_print_start_collector.cpp` - flap test |
| ETA re-baselines on heater-target arrival and staged rise | `test_print_start_collector.cpp` - `[eta]` tests |
| A heating phase completes at its target, not when the chain's marker passes (concurrent-heat firmware) | `test_print_start_collector.cpp` - "Remaining keeps unfinished heating work" |
| Timeouts wait for both heaters at target (2°C) and count a climbing heater or a status-signal rule as activity (a new high under the current target, not a swing back up); the ceiling ignores temps, heaters and rules but waits out narration; the backstop ignores everything | `test_print_start_collector.cpp` - "Timeout fallback waits for the heaters to reach their targets", "A heater still closing on its target counts as activity", "A heater cycling under its target is not climbing", "A heater sampled during the last print is not climbing on the next", "The ceiling ends a pre-print whose heater never settles", "The ceiling waits for a narrating printer to go quiet", "A printer that never stops narrating ends at the backstop", "The ceiling stretches for a long prediction", "A status-signal rule is not a line the ceiling waits out", "A status-signal rule holds the timeout like a climbing heater" |
| A pattern's declared hold (a COSMOS heat soak's silent G4) counts as the printer talking until it ends and is left out of the ceiling; its display copy holds once; the backstop leaves out at most one ceiling of held time | `test_print_start_collector.cpp` - "A declared hold counts as the printer talking until it ends", "A declared hold is left out of the time the ceiling measures", "A printer that keeps announcing holds still ends at the backstop", "a long COSMOS heat soak holds the pre-print open"; `test_print_start_profile.cpp` - "a pattern's hold comes from the capture group it names"; `test_print_start_profile_cosmos.cpp` |
| A database entry's `thermal_rates` replace the bed-size guess (unknown heater names skipped); a timeout completion saves the rates it measured but not its phase timings; homing takes the predictor's floor in both the collector and the print details estimate | `test_thermal_rate_model.cpp` - "ThermalRateManager takes measured rates from the printer database"; `test_printer_detector.cpp` - "pre-print lookups share one entry, and rates name known heaters"; `test_print_start_collector.cpp` - "A timeout completion keeps the heating rates it measured", real COSMOS replay's database-rates section; `test_preprint_predictor.cpp` - "homing estimate never drops below the floor"; `test_print_preparation_manager.cpp` - "estimate takes its homing time from the predictor" |
| A silent macro (G28, M190, M109, purge, no console narration) shows Homing, Heating Bed, Heating Nozzle and Purging; the countdown follows the bed's measured rate (weights recompute once, when a heater gets its first measured rate); a non-heating phase never seen and ordered before the one on display owes no time (only without history); a start that only heated still saves its total and rates | `test_print_start_collector.cpp` - "silent Voron V0 start shows its steps and learns its length", "a start that only heats saves its length and heating rate", "an already-homed printer does not show Homing" |
| A pre-print saves its whole measured climb as the heating rate, holds between climbs left out | `test_thermal_rate_model.cpp` - "ThermalRateModel leaves a hold between two climbs out of the rate it keeps", "ThermalRateModel keeps the rate of the whole climb, not its last approach"; `test_print_start_collector.cpp` - "a COSMOS pre-print learns the heating rates it took" |
| A bed mesh with no probing under way gives way to the nozzle heating toward a target set after the mesh began; a recent probe line or an unchanged target keeps the mesh | `test_print_start_collector.cpp` - "A bed mesh with no probing under way gives way to the nozzle heat after it", "a quiet COSMOS park and purge shows the nozzle heating" |
| A real COSMOS pre-print completes at its skew check, not on a timeout | `test_print_start_collector.cpp` - "real COSMOS pre-print completes at its last step" (replay of the 2026-09-14 CC1 klippy.log) |
| Entering a phase releases the monotonic anchor (no frozen countdown through a long mesh) | `test_print_start_collector.cpp` - "Entering a phase releases the monotonic countdown anchor" |
| Sweep-march promotion credits the buffered pre-mesh probes (count matches the physical taps) | `test_print_start_collector.cpp` - "Buffered pre-mesh probes are credited" |
| Position chain wipe → centre → corners → sweep on real captures | `test_print_start_position_classifier.cpp` (corpus: `tests/fixtures/print_start_position_corpus.json`, extracted from the 2026-08-19 K1C klippy.log capture) |
| Position → message/phase integration, and the no-flag negative | `test_print_start_collector.cpp` - `[position]` integration tests |
| K1C first-print ETA defaults from measured durations | `test_printer_detector.cpp` - `print_start_default_phases` |
| A real Forge-X 1.4.2 pre-print completes at `print/PRINTING`, not at the clean's FINISHING or a timeout through the silent cool-down; the mesh starts at `kamp/LEVELING`, not the print's LEVELING, its denominator is 25 and the two cleaning probes are not its points | `test_print_start_collector.cpp` - "real Forge-X 1.4.2 pre-print completes at PRINTING" (replay of `tests/fixtures/forgex_142_console.txt` and `forgex_142_heaters.txt` with reconstructed `operation_context` frames) |
| Context-qualified phase object: delta frames re-match the cached pair, a null state is not the outer step's, the cache is per run, the start-up query fills only unseen fields, a live state may step back, a live step holds the deadline timeouts and a bare state does not; a heater cooling toward its target is not at target; a signal-format line is activity | `test_print_start_collector.cpp` - `[forge_x]`, "A bare phase-object state does not hold the timeouts", "A heater cooling toward its target is not at target", "A signal-format line counts as activity for the timeouts"; `test_print_start_profile.cpp` - "a context field qualifies the state the patterns match", "a malformed context field drops the phase object", "forge_x state patterns map the mod's state strings" |
| K2 patterns match the real narration; `BED_MESH_CLEAR` deliberately NOT a mesh pattern | `test_print_start_profile_k2.cpp` (captures: 2026-08-18 K2 Plus klippy.log) |
| A Klipper shutdown or error stops the collector even while print_stats still reports a job (the U1 reports `paused` after a verify_heater shutdown) | `test_moonraker_manager.cpp` - "should_stop_collector_on_klippy_state stops on shutdown and error" (the decision; the observer wiring in `MoonrakerManager::init_print_start_collector` is not constructible in a unit test) |
| While a heater wait reports (the once-a-second `B:.. /.. T0:.. /..` lines of M109/M190/TEMPERATURE_WAIT) after a real signal, the label shows the heater the waiting command names: the line at `virtual_sdcard.file_position`, queried each fallback tick (and on a wait's first report) and read again whenever the position moves, never asked when the transfers API has no ranged reads or after a failed read in the same wait, since one wait can hand to the next with no gap in the reports. M109 is the nozzle, cool-downs included, M190 the bed, M191 or a `TEMPERATURE_WAIT` on the chamber heater SOAKING (only with a chamber heater configured), a `TEMPERATURE_WAIT` on `heater_bed` or an extruder that heater (`PrintStartCollector::heater_waited_on`). Any other command, a failed read, or no answer yet falls back to the heater short of its target, nozzle first, then the bed, then a chamber heater once both are at target (SOAKING, "Heating chamber..."). A phase signal ends the wait; otherwise the first tick after the reports stop restores the displaced phase, message and sequential progress. Stray lines, a heater merely warming behind a phase, and firmware HEATING_* phases are left alone; reports count as activity for the timeouts | `test_print_start_collector.cpp` - `[heater_wait]` tests, including "a bed wait after plate detect shows Heating Bed until it ends", "a wait shows the nozzle while it is short, then the bed", "a long chamber wait holds the pre-print past its deadline", "A phase signal right after a wait's last report keeps its phase", "A chamber wait does not relabel a heating phase the firmware announced", "A heater wait gives a sequential profile's phase, message and progress back", "real heater waits show the bed heating between firmware phases" (replay of `tests/fixtures/u1_heater_wait_console.txt`, the 2026-09-23 U1 gcode_store), "each real wait shows the heater its command waits on" (replay of `tests/fixtures/u1_tool_probe_clean_console.txt` against `u1_tool_probe_clean_start.gcode`) and "a heater wait shows the heater its command names, else the guess" |
| On a `scripted_start` profile, the first `virtual_sdcard.file_position` frame reads the file's head once; each step of its start block shows from the frame that reaches its line (a homing, leveling or mesh line only while the position stands on it, a clean or purge once passed), at most once, with the step's label; a step within 1 s of a console line that changed the display is left to it; a profile without the flag reads nothing; each phase's history entry is its time summed over every visit | `test_start_block_steps.cpp` (`[start_block]`: the real U1 block's steps at their bytes, what a line runs, the follower) and `test_print_start_collector.cpp` - "the print position shows the cleans, Z probes and prime line" (replay of `tests/fixtures/u1_bmcu_console.txt` with `u1_bmcu_positions.txt` against `u1_bmcu_start.gcode`, the 2026-10-17 U1 print, durations included), "a firmware code on a step's line is not overwritten by the step", "A profile without a scripted start does not read the print's head" |
| Whole chain (wiring, observers, collector) against a real capture, no printer | `HELIX_MOCK_REPLAY=<script> --test` (see MOCK_ENVIRONMENT_VARIABLES.md; script from `scripts/extract_mock_replay.py`) |

---

## Pre-Print ETA Prediction

The ETA during preparation - `PreprintPredictor` history buckets, the thermal model, weighted averaging, and the live countdown - is documented in full in [PREPRINT_PREDICTION.md](PREPRINT_PREDICTION.md).

---

## Future Extensions

- **User-editable profiles**: `config/print_start_profiles.d/` for user overrides (same pattern as `printer_database.d/`)
- **Temporal predicates**: windowed conditions for `status_signals` (Z oscillation -> BED_MESH probing, sustained zero velocity with hot heaters -> soak) - needs an `over_ms` condition type
- **Voron profile**: Map Voron `status_*` LED macro calls to phases
- **Bambu/Prusa profiles**: For future printer support
