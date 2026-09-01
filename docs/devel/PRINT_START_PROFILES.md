# Print Start Profiles

How HelixScreen knows what a printer is doing before the first layer, and
how to write a profile for a printer we are adding. For making a user's
existing macros legible to HelixScreen, see
[PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md) - that is the
modder's half of the same system.

## The model: three kinds of evidence, one referee

A print start is narrated by somebody. Which of three evidence kinds a
printer offers decides how its profile is written:

| Kind | What it is | Trust |
|------|------------|-------|
| Narration | Console lines the firmware or macros print (`response_patterns`) | What the printer SAYS - authoritative |
| Structured state | A status object carrying phase strings (`phase_object` + `state_patterns`) | What the printer PUBLISHES - authoritative |
| Physical inference | Predicates over status frames (`status_signals`) | What the printer IS DOING - confirmation, never overrides later narration |

All three feed one pipeline in `PrintStartCollector`
(`src/print/print_start_collector.cpp`) and arbitrate through per-match
`weight`s: the strongest evidence for the current moment wins, and a
later higher-weight match supersedes an earlier one. Phase semantics
(the `PrintStartPhase` enum, `include/printer_state.h`) are engine-owned;
profiles never invent phases.

Discipline for inference rules: an unambiguous physical fact (the head
parked at a cutter's corner) may carry a high weight; an ambiguous one
(a heater at target) is a tie-breaker at most - or stays out of the
profile. A confident wrong phase is worse than an honest generic bar.

## Where profiles live and how one is chosen

- JSON files: `assets/config/print_start_profiles/<name>.json`
  (`default`, `forge_x`, `ad5m`, `creality_k1`, `creality_k2`, ...).
- A printer entry in `assets/config/printer_database.json` names its
  profile with `"print_start_profile": "<name>"`; printers without the
  field get `default`.
- The active profile is loaded by `PrintStartProfile::load()` and drives
  `PrintStartCollector`; any status objects it declares are subscribed
  generically during discovery
  (`MoonrakerDiscoverySequence::build_subscription_objects()`).

## Schema

```json
{
  "name": "Human Name",
  "description": "One line.",
  "progress_mode": "weighted",
  "adaptive_meshing": true,
  "position_signals": true,
  "cfs_signals": true,
  "signal_formats": [],
  "response_patterns": [
    {"pattern": "M190|Heating bed", "phase": "HEATING_BED",
     "message": "Heating Bed...", "weight": 20}
  ],
  "phase_weights": {"HOMING": 5, "BED_MESH": 25},
  "phase_object": {"object": "operation_context", "field": "current_state"},
  "state_patterns": [
    {"pattern": "LEVELING", "phase": "BED_MESH",
     "message": "Bed Mesh...", "weight": 25}
  ],
  "status_signals": [
    {"name": "at_cutter", "object": "toolhead",
     "when": [{"field": "position", "index": 0, "op": "lt", "value": 0.0}],
     "phase": "PURGING", "message": "Changing filament...", "weight": 30}
  ]
}
```

Every key except `name` is optional; a profile with only
`response_patterns` is the common case. Malformed entries warn in the
log and are skipped - one bad rule never takes down the profile.

- `response_patterns[].pattern` - regex over a console line
  (`notify_gcode_response`). `message` may contain `$1` capture groups.
- `phase_weights` - per-phase fallback weights for progress estimation;
  `progress_mode` picks the estimation strategy.
- `adaptive_meshing`, `position_signals`, `cfs_signals` - toggles for the
  engine's built-in heuristic families (bed-mesh probe counting,
  nozzle-position inference, Creality CFS signals). Only enable what you
  have verified on the hardware.
- `phase_object` - a status object whose string field carries phase
  state (e.g. a mod publishing an operation-context object). The state
  string is matched by `state_patterns`, which share the
  pattern/phase/message/weight contract; an unchanged state never
  re-fires.
- `status_signals` - edge-triggered physical predicates. `when` is an
  AND-list over one object's status fields (`field` dot-path, optional
  `index` for arrays, `op` one of `eq ne gt lt near`, `tolerance` for
  `near`). The right-hand side is either a literal `value` or a sibling
  field of the same object (`ref_field`, plus optional `offset`) - which
  is how "temperature is more than 2 below target" is expressed. A rule
  fires on the false->true transition and re-arms when the predicate
  drops - physical states hold for windows, console lines are events,
  and the engine treats them accordingly. A malformed rule is skipped
  whole: an AND that silently dropped one condition would widen the
  match.

## Making a new one (the K2 method)

1. **Capture a real start.** Run a print start on the hardware and save
   the full console output (`~/.helixscreen` logs, or klipper's
   printer.log) from `START_PRINT` to first extrusion. The profile is
   written against what the machine actually says, not what its docs
   claim.
2. **Choose evidence by what the capture shows.**
   - Macros print state lines? -> `response_patterns` on those strings.
   - Firmware/mod publishes a status object with state? ->
     `phase_object` + `state_patterns` (no regex at all).
   - Neither, or gaps (a filament-change window nobody narrates)? ->
     `status_signals` on position/temperature facts. Keep the trust
     table above; inference is the last resort, not the first.
3. **Write the JSON**, reusing message strings from an existing profile
   where the phase is the same - the panel's wording should not vary by
   printer for the same event.
4. **Wire it**: add the JSON, set `"print_start_profile"` in the
   printer's database entry.
5. **Pin it with tests**, in the established pattern:
   - Table tests in `tests/unit/test_print_start_profile.cpp`
     (`[profile][print]`): every pattern matches its captured line, and
     negative cases do not.
   - A regression file like `tests/unit/test_print_start_profile_k2.cpp`
     for the captured narration lines - the honest ones, including lines
     that must NOT announce a phase.
   - Collector-level cases in `tests/unit/test_print_start_collector.cpp`
     for object/signal rules (feed a status frame, assert the phase, and
     assert the frame is IGNORED for a profile without the declaration -
     that is the fallback guarantee).
6. **Prove the tests can fail** (break a mapping, watch red, restore) -
   see `tests/CLAUDE.md` - then commit profile + database entry + tests
   together.

## Debugging a profile

Run the app with `-vv`: `PrintStartProfile` logs every signal match
(`Signal match: ... -> phase=`) and the collector logs phase
transitions. A phase that never arrives is usually a pattern that does
not match the real console spelling - diff your regex against the
captured line, not against memory.
