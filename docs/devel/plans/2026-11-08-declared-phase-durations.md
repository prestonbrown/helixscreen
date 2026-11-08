# Declared phase durations for the pre-print countdown (#1902)

Status: design, awaiting Preston's answers to the open questions at the end. Ships on main (1.2).
Deleted in the change that ships the last phase (`docs/CLAUDE.md`).

This is a plan, not code. It states interfaces, tests and constraints; bodies are the
implementer's. Every header it defines gets a `scripts/syntax_check.py` pass before its phase
starts.

## Why

An adaptive `PRINT_START` decides at render time how long it will soak, heat the chamber or mesh,
from the print's own parameters. The countdown prices those phases from history and the thermal
rate model, which describe past prints with other targets, so the estimate is wrong from the
first second and corrects itself only as phases end. The macro often knows the exact figure (a
soak is `G4 P600000`) and has no way to say it. The issue asks for a way to say it once, at the
top of the macro, and for the countdown to trust it.

## What exists today

| Piece | Where | What it does now |
|---|---|---|
| Phase enum and names | `include/print_start_phase.h#print_start_phase_name`, `#print_start_phase_from_name`, `#kPrintStartPhaseAliases` | `SOAKING` and `HEATING_CHAMBER` already exist, with aliases `HEAT_SOAK`, `SOAK`, `CHAMBER_HEATING`, `HEAT_CHAMBER`. Names, not numbers, leave the process |
| Signal parse | `src/print/print_start_collector.cpp#PrintStartCollector::check_helix_phase_signal` | finds `HELIX:PHASE:` anywhere in a console line, cuts the name at the first space or quote, so anything after the name is ignored |
| Line routing | `src/print/print_start_collector.cpp#PrintStartCollector::on_gcode_response` | HELIX:PHASE first, then profile tags, RESPOND completion, profile signal formats, then `check_phase_patterns` (profile `response_patterns`) |
| Estimate | `src/print/preprint_predictor.cpp#PreprintPredictor::estimate` | durations = thermal heating + history phases (homing floored) with `PreprintPhaseOverrides` applied; total = history wall clock adjusted for skipped/added phases, with a residual split (`PreprintEntry::heating_seconds`) so a start pays its own heating plus the learned constant remainder |
| Weights | `src/print/print_start_collector.cpp#PrintStartCollector::compute_predicted_weights` | called at start, on `note_phase_overrides`, and when a heater target appears or rises 15°C; normalises durations into `predicted_phase_weights_`, total into `predicted_total_seconds_` |
| Countdown | `src/print/print_start_collector.cpp#PrintStartCollector::update_eta_display` | per-phase remaining (completed 0, current `max(0, dur - elapsed)`, future full); then the monotonic clamp, the under-two-minutes bias, and the downward rate limit. An overrunning phase contributes 0, so the number holds at the sum of the phases still ahead |
| Anchor release | `src/print/print_start_collector.cpp#PrintStartCollector::note_phase_overrides` | sets `last_remaining_ = 0` before recomputing, which lets one upward correction through the monotonic clamp |
| Holds | `src/print/print_start_collector.cpp#PrintStartCollector::note_signal` | a profile match with `hold_seconds` declares silent work; the quiet gate and the ceiling leave held time out |
| Timeouts | `src/print/print_start_collector.cpp#PrintStartCollector::check_fallback_completion` | deadline = `predicted_total * kAdaptiveTimeoutMargin` (1.5) gated by `kPreprintQuietTimeout` (90 s) and temps at target; ceiling `kAbsoluteMaxTimeout` (1800 s) stretched by the prediction |
| Checklist | `src/print/print_start_collector.cpp#PrintStartCollector::checklist_input` | `predicted_seconds` from `predictor_.predicted_phases()`; expected order from `start_block_` or `macro_expected_phases_` |
| Macro analysis | `include/print_start_analyzer.h#PrintStartAnalysis`, reaching the collector through `src/ui/print/ui_print_preparation_manager.cpp#PrintPreparationManager::set_macro_analysis` | operations per macro chain; only prints started from the detail view get one |
| Macro pack | `assets/config/helix_macros.cfg` (v2.1.0) | eleven hidden signal macros, each `SET_GCODE_VARIABLE` on `_HELIX_STATE` plus `RESPOND MSG="HELIX:..."`. No `_HELIX_PHASE_SOAKING` or `_HELIX_PHASE_HEATING_CHAMBER` |
| Version rungs | `src/printer/macro_manager.cpp#parse_installed_version` | `HELIX_UNLOAD_FILAMENT` = 2.1.0 via `has_helix_macro` (names starting `HELIX_` only) |
| Visible rewrite (main) | `src/printer/macro_manager.cpp#MacroManager::with_visible_signal_names` | on a visible-name printer erases the `_` from every `_HELIX_PHASE_` prefix, so a new `_HELIX_PHASE_SOAKING` would ship there as `HELIX_PHASE_SOAKING` |
| Hidden-name migration (branch `docs/plan-hidden-macros`, #2006) | its plan `docs/devel/plans/2026-11-07-hidden-signal-macro-migration.md`, `include/signal_macro_names.h#kVisibleNames` | deletes the rewrite, forwards the eleven legacy visible names to the hidden ones, and pins that every `[gcode_macro _HELIX_*]` in the pack except `_HELIX_STATE` is `_` + a `kVisibleNames` entry |
| Heat-soak option (branch `feat/preprint-heat-soak`, #1972) | `src/print/print_start_phase_label.cpp#print_start_phase_options`, `src/printer/printer_profile_state.cpp#PrinterProfileState::apply_dynamic_options` | maps `kHeatSoakOptionId` to `SOAKING` in the phase overrides; runs the printer's soak macro as pre-start gcode, before `PRINT_START` |
| Firmware | `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` | collector, predictor and analyzer are in the ESP32 build; `macro_manager.cpp` is not |

Measured: a plan line such as `echo: HELIX:PLAN SOAKING=600 BED_MESH=90 QGL=40 HOMING=30`
matches `response_patterns` in `default.json`, `creator5.json`, `qidi.json`, `ad5m.json` and both
Kobra profiles (`soaking`, `QGL`, `Homing`, `bed.?mesh`). Any code path that lets the line reach
`check_phase_patterns` jumps the phase.

## Decisions

### D1. One new signal macro, `_HELIX_PLAN`, hidden-only

```
[gcode_macro _HELIX_PLAN]
description: HelixScreen pre-print plan: expected phase durations in seconds
gcode:
    RESPOND MSG="HELIX:PLAN{% for k, v in params.items() %} {k}={v|string|replace('"', '')}{% endfor %}"
```

Called as `_HELIX_PLAN SOAKING=600 BED_MESH=90`. The macro forwards its parameters and validates
nothing; the app owns every rule (D4). It writes no `_HELIX_STATE` variable: nothing reads one.

It follows #2006's naming: a leading underscore so Fluidd and Mainsail leave it out of their
lists, and **no visible spelling and no forwarder**, because no printer has ever called
`HELIX_PLAN`. The same holds for the two new phase signals (D6). #2006's pin test therefore gains
a second list, `kHiddenOnlyNames`, which its "every `_HELIX_*` section is a listed name" case
accepts beside `kVisibleNames`; `hidden_loaded` and the alias logic keep meaning the eleven.

**Ordering constraint.** This lands after #2006 Phase 1 (which deletes
`with_visible_signal_names`). Landed first, the rewrite would rename `_HELIX_PHASE_SOAKING` to
`HELIX_PHASE_SOAKING` on every visible-name printer, and the docs would name a macro those
printers do not have. If #2006 slips, the rewrite must skip the new names instead.

### D2. Wire format: per-phase durations, repeatable, merged

```
HELIX:PLAN <NAME>=<SECONDS> [<NAME>=<SECONDS> ...]
```

- **Per phase only.** No `TOTAL=` and no global `REMAINING=`. A total cannot be split across
  phases, so overrun per phase, the bar's shape and the history split all lose it; the sum of the
  declared phases is the total the macro knows. (Q2 asks whether a total is still wanted.)
- **Repeatable.** Each accepted line merges into the run's plan, a later value replacing an
  earlier one for the same phase. Klipper renders a macro's Jinja before it runs, so a plan at the
  top of `PRINT_START` cannot see state the macro creates; a later `_HELIX_PLAN` from a sub-macro
  (rendered when it is called) can, e.g. a chamber wait sized from the chamber's live temperature.
- **What a value means depends on where the run is.** For a phase not yet started, its full
  duration. For the **current** phase, the seconds left from now (stored as elapsed + value), so
  `_HELIX_PLAN SOAKING=600` immediately before `G4 P600000` is right whether it comes before or
  after `_HELIX_PHASE_SOAKING`. For a phase already finished, ignored with a debug line.
- **Zero means "will not run"** and acts as a skip override for that phase (Q1: the issue
  proposes rejecting zero). It lets `HELIX_BED_MESH_IF_NEEDED`-style macros say "no mesh this
  time" without the history's mesh share staying in the estimate.
- **Not exhaustive.** A phase the plan leaves out keeps history, overrides and defaults. Authors
  declare what they know.

### D3. How declared durations combine with the predictor

Precedence per phase: **declared, then phase overrides, then history / thermal model, then
defaults.** A declared value beats a #1972 override in both directions: the plan is what the
macro is about to do with this print's parameters, the override is what the user asked for
before the macro rendered. Declared heating (`HEATING_BED`, `HEATING_NOZZLE`, `HEATING_CHAMBER`)
replaces the thermal price for that heater and is what the entry records as priced heating.

Total, the hard part. The history wall clock already contains the declared phases' time from past
runs, attributed or not; adding the declared seconds on top double counts, and subtracting a
learned per-phase mean under-counts whenever older runs never signalled that phase (the issue's
own case: a silent `G4` soak lands in no phase). So planned runs are a **separate population**,
as `PreprintWindow` already separates host pre-start runs:

- An entry recorded while a plan was in force stores the plan (`PreprintEntry::declared`).
- With a plan this start: the total comes from planned entries only, as a residual. For each
  such entry, residual = wall clock − its priced heating − for each phase it declared, the
  recorded duration of that phase (the declared value when the phase was never signalled). Total
  = weighted mean residual + this start's heating + this start's declared non-heating seconds.
  Recorded, not declared, durations are subtracted so a declared phase that overran does not leak
  its overrun into the residual.
- With a plan and no planned entries yet: the no-history formula, the sum of the durations
  (declared, plus per-phase history means and thermal heating for the rest). It loses unmapped
  macro time on the first planned print or two, which is the price of never double counting.
- Without a plan: unchanged. Planned entries are ordinary runs and stay in that population.

The plan is known only once `PRINT_START` runs, so the print details estimate before a print
(`PreprintPredictor::estimate` with no plan) is unchanged.

### D4. Parsing: strict, bounded, app-side

A pure function, no LVGL, no regex, no exceptions (firmware is `-fno-exceptions` and C++17):

- **Anchored.** The line, after leading whitespace and one optional `echo:` or `//` prefix and
  whitespace, must start with `HELIX:PLAN` followed by whitespace or end of line. A line merely
  containing the text (an `M117`, an error echo) is not a plan. Stricter than `HELIX:PHASE:`, which
  matches anywhere; this one carries numbers that move a deadline.
- **Always consumed.** `on_gcode_response` checks it before `check_helix_phase_signal`, and a plan
  line returns early whether or not any token was valid, so it never reaches the profile
  patterns (see "Measured" above).
- **Bounded.** A line over `kMaxPlanLineBytes` (512) or with more than `kMaxPlanTokens` (32)
  tokens is rejected whole, with one warning.
- **Tokens.** `NAME=VALUE`, split on spaces and tabs. NAME is upper-cased and resolved through
  `print_start_phase_from_name` (canonical names and aliases); `IDLE`, `INITIALIZING` and
  `COMPLETE` are refused. VALUE is digits with at most one `.` (Jinja renders `600.0`), checked by
  character before `text_io::parse_leading<float>`, rounded to whole seconds, finite, 0 to
  `kMaxDeclaredSeconds`. Anything else drops that token only, and the line logs one warning naming
  the dropped tokens (capped at the first few).
- **Cap.** `kMaxDeclaredSeconds` = 21600 (6 h) per phase. The issue asks for no cap, but a
  declared duration feeds the adaptive deadline and a hold (D5), and an absurd value from a typo
  (`G4 P600000` copied as `SOAKING=600000`) would hold Preparing open for a week on a printer that
  has gone silent. Six hours is past any real soak. (Q3.)
- **Duplicate names** in one line: last wins. A line with no valid token changes nothing.
- **Only while a pre-print is active**, between arming and COMPLETE. Outside one (a user typing
  `_HELIX_PLAN` at the console, a plan from a print that ended) it is consumed and logged at debug.
- **Who can send it.** Anyone with console access can RESPOND anything, as with `HELIX:READY`
  today. The worst a forged plan can do is a wrong countdown and, within the cap, a later
  timeout. It runs no command and writes nothing but this run's history entry.

### D5. The collector: plan state, deferred first estimate, holds, overtime

- **State.** `declared_` (phase -> seconds, as the predictor takes it) and a `plan_seen_` flag,
  both cleared in `reset_run_locked`. Each accepted line merges, applies D2's current-phase rule,
  releases the monotonic anchor once (`last_remaining_ = 0`, the `note_phase_overrides` precedent)
  and recomputes weights. A plan that raises the total therefore shows the higher number at once;
  every later tick is guarded as today.
- **Deferred first estimate.** `PrintStartAnalysis` gains `declares_plan`: some macro in
  `macro_chain` calls `_HELIX_PLAN` (case-insensitive, word-bounded, the analyzer's existing
  operation scan; no arguments read, no Jinja evaluated). With the flag set, `start()` leaves the
  weights empty, so the countdown shows nothing rather than a history guess, until the first of:
  a plan line, a phase signal or phase change, or `kPlanWaitSeconds` (10 s) after arming. A plan
  that arrives after that still recomputes as above. `start_block_` is untouched. Prints started
  outside the detail view have no analysis and never wait.
- **Holds.** When the current phase has a declared duration (on entry, or when a plan names the
  current phase), the collector calls `note_signal(remaining declared seconds)`. A silent 10-minute
  `G4` then cannot trip the 90 s quiet gate, and the ceiling leaves the held time out, exactly as
  a profile's `hold_seconds` does today.
- **Checklist.** `checklist_input()` fills `predicted_seconds` from the declared value where there
  is one; declared phases missing from the expected order are inserted at their enum position
  (the enum order is the sequence order).
- **Overtime balance.** A pure function computes the signed balance the issue describes:
  Σ over finished phases (actual − expected) + max(0, current elapsed − current expected), where
  expected is the duration the weights held for that phase. It is logged at each phase change and
  stored nowhere. The countdown needs no change to "hold" while a phase overruns: the overrunning
  phase already contributes 0 and the clamp forbids a rise, so the number sits at the phases still
  ahead and the bar's current-phase fraction is capped. Whether the screen says it is running
  over is Q4; until it is answered nothing on screen changes, so no `ctl geom` measurement was
  taken. If Preston wants it, the implementer measures the preparing card's time label at
  800x480 and the smallest breakpoint before drawing it.
- **History.** The finished run's entry records `declared` when `plan_seen_`. A run that ends
  by fallback timeout records nothing, as today.

### D6. Two new phase signals

`_HELIX_PHASE_SOAKING` and `_HELIX_PHASE_HEATING_CHAMBER`, same shape as the existing eight
(`SET_GCODE_VARIABLE ... phase`, `RESPOND MSG="HELIX:PHASE:<NAME>"`). The issue asks for soaking;
chamber heating is the other half of the adaptive heating it describes, and the plan accepts both
names, so a macro should be able to signal both. Hidden-only (D1). Pack v2.2.0, with a rung in
`parse_installed_version` on `has_macro("_HELIX_PLAN")` (the rung must use `has_macro`, since
`has_helix_macro` only sees names starting `HELIX_`). `HELIX_START_PRINT` does not call
`_HELIX_PLAN`: it knows no durations.

### D7. ESP32 cost

The parser, the predictor change and the collector change all compile into the firmware; the pack
does not. Budget: the parser is a linear scan over a `string_view` with no regex and no
allocation beyond the result map (at most 14 nodes), so well under 1 KB of flash; the
predictor's population filter and residual are one extra lambda over `weighted_entry_mean`; the
collector adds one map and a flag per run. History entries grow by an optional small object,
within the existing `kMaxStoredEntries` (15) cap. No new static buffers (large statics land in
internal `.bss` on ESP32). Measure with the firmware size report in Phase 2 rather than trust this.

## Interfaces

New `include/print_start_plan.h`, `src/print/print_start_plan.cpp`, namespace `helix`,
`<map>`, `<string>`, `<string_view>`, `<vector>`, `<optional>` only:

```cpp
/// phase_enum -> declared seconds; 0 means the phase will not run
using PreprintDeclaredDurations = std::map<int, int>;

inline constexpr size_t kMaxPlanLineBytes = 512;
inline constexpr size_t kMaxPlanTokens = 32;
inline constexpr int kMaxDeclaredSeconds = 21600;

struct PrintStartPlanLine {
    PreprintDeclaredDurations durations; ///< valid tokens, last duplicate wins
    std::vector<std::string> dropped;    ///< rejected tokens, at most 4 kept for the warning
    bool rejected_whole = false;         ///< over a bound; durations empty
};

/// nullopt when the line is not a plan line (D4 anchoring); a plan line always yields a value
[[nodiscard]] std::optional<PrintStartPlanLine> parse_print_start_plan(std::string_view line);

/// Merge @p incoming into @p plan per D2: finished phases ignored, the current phase's value
/// becomes @p current_elapsed + value, anything else replaces
void merge_declared(PreprintDeclaredDurations& plan, const PreprintDeclaredDurations& incoming,
                    int current_phase, int current_elapsed_seconds,
                    const std::vector<int>& finished_phases);

/// Signed overtime: finished phases' (actual - expected) plus the current phase's overrun
[[nodiscard]] int overtime_balance_seconds(const std::map<int, int>& expected,
                                           const std::map<int, int>& actual_finished,
                                           int current_phase, int current_elapsed_seconds);
```

`include/preprint_predictor.h`:

```cpp
struct PreprintEntry {
    // ... existing fields
    /// The plan this start declared; nullopt for a start without one
    std::optional<PreprintDeclaredDurations> declared;
};

[[nodiscard]] PreprintEstimate estimate(const PreprintHeating& heating,
                                        const PreprintPhaseOverrides& overrides = {},
                                        const PreprintDeclaredDurations& declared = {}) const;
```

History JSON: each entry gains an optional `"declared": {"SOAKING": 600, "BED_MESH": 90}`, keyed
by phase name like `"phases"`; `load_entries_from_config` drops unknown names and invalid
values with the parser's rules, `entries_to_json` writes it back.

`include/print_start_analyzer.h#PrintStartAnalysis` gains `bool declares_plan = false;`.

`PrintStartCollector` gains private `check_helix_plan_signal(const std::string& line)`,
`declared_`, `plan_seen_`, `plan_wait_until_`, and `kPlanWaitSeconds`; `compute_predicted_weights`
passes `declared_` to `estimate`.

## Phases

While building, each phase runs only its own tags once after the change, with the new test seen
red against a hand break. The batch gets one review, one screenshot only if Q4 changes the
screen, one `make land-gate`, and `mutate-diff` after landing (CLAUDE.md § Verification Cadence).

### Phase 1: parser and predictor (pure)

`print_start_plan.h/.cpp`, the predictor's `declared` parameter, population split and residual,
the history field. Tags `[print_start_plan]`, `[print][predictor]`.

| Case | Expect |
|---|---|
| `echo: HELIX:PLAN SOAKING=600 BED_MESH=90` | {SOAKING:600, BED_MESH:90} |
| `// HELIX:PLAN soak=600`, alias and lower case | {SOAKING:600} |
| `SOAKING=600.0`, `SOAKING=599.6` | 600 |
| `SOAKING=0` | {SOAKING:0} |
| `SOAKING=-5`, `=abc`, `=1e9`, `=600x`, `=`, `=21601`, `= 600` | token dropped, listed in `dropped` |
| `IDLE=5`, `COMPLETE=5`, `INITIALIZING=5`, `FOO=5` | dropped |
| `SOAKING=600 SOAKING=300` | 300 |
| 513-byte line, 33 tokens | `rejected_whole`, empty durations |
| `M117 HELIX:PLAN SOAKING=600`, `HELIX:PLANX SOAKING=1`, `HELIX:PHASE:SOAKING` | nullopt |
| `HELIX:PLAN` alone | a value with empty durations |
| `merge_declared`: finished phase named | unchanged |
| `merge_declared`: current phase 120 s in, value 600 | stored 720 |
| `overtime_balance_seconds`: one phase 60 over, one 20 under, current 30 over | 70 |
| `estimate` no history, SOAKING=600 declared | durations[SOAKING]=600, total = sum |
| `estimate` declared HEATING_BED=300 | replaces the thermal bed price; counted once in the total |
| `estimate` declared SOAKING=0 | SOAKING absent from durations, as a skip override |
| `estimate` declared 600 vs an override skipping SOAKING | declared wins |
| `estimate` with plan, history of unplanned entries only (soak inside the wall clock, unsignalled) | total = sum of durations, never wall clock + 600 |
| `estimate` with plan, planned entries recorded SOAKING 600, wall 1200, heating 300 | residual 300; declared SOAKING=900 now gives 300 + heating now + 900 |
| `estimate` planned entry whose declared phase was never signalled | its declared value is subtracted |
| `estimate` planned entry whose soak overran (declared 600, recorded 700) | residual subtracts 700 |
| `estimate` no plan, mixed planned and unplanned entries | identical to today's result on the same entries |
| History JSON round trip with `declared`; an entry with a bad name and a negative value | round trips; bad pairs dropped on load |

Break checks: let the residual use all entries and the unplanned-history case goes red; drop the
current-phase rule in `merge_declared` and the 720 case goes red.

### Phase 2: collector and analyzer

Tags `[print][collector]` (new sub-tag `[plan]`), `[print_start]`.

| Case | Expect |
|---|---|
| Plan line during a pre-print | weights recomputed; `predicted_total_seconds_` reflects it |
| Plan line whose total is higher than the shown remaining | the next tick shows the higher number (anchor released once) |
| Plan line with profile `default.json` loaded | phase unchanged (line never reaches `response_patterns`) |
| Plan line with every token invalid | consumed, no recompute, one warning |
| Plan line while idle or after COMPLETE | consumed, no state change |
| Second plan line | merges; the current-phase value counts from now |
| `declares_plan` set, no plan within 10 s | weights computed at 10 s |
| `declares_plan` set, plan at 2 s | first weights include the plan; nothing shown before |
| `declares_plan` set, phase signal before any plan | weights computed at the signal |
| Current phase declared 600, console silent 300 s, temps at target | no fallback completion (hold) |
| Declared SOAKING, 900 s silent, cap respected | hold ends at 600 s; normal timeouts resume |
| `checklist_input` with a declared phase | its `predicted_seconds` is the declared value |
| Run finishes with a plan | history entry carries `declared` |
| Run finishes by fallback timeout | no entry (unchanged) |
| Analyzer: chain body calls `_HELIX_PLAN SOAKING=...` | `declares_plan` true |
| Analyzer: `_HELIX_PLANX`, `MY_HELIX_PLAN`, a comment line | false |

Break checks: move the plan check after `check_phase_patterns` and the profile case goes red;
skip the hold and the silent-soak case goes red.

### Phase 3: macro pack, rung, docs

| Case | Expect |
|---|---|
| Pack defines `_HELIX_PLAN`, `_HELIX_PHASE_SOAKING`, `_HELIX_PHASE_HEATING_CHAMBER`; header v2.2.0 | `[config][content]` |
| #2006 pin test with `kHiddenOnlyNames` | passes; a new `_HELIX_*` section in neither list goes red |
| `macro_content_for` on a printer with the alias block | no `HELIX_PLAN` or `HELIX_PHASE_SOAKING` forwarder |
| `parse_installed_version` with `_HELIX_PLAN` loaded | 2.2.0; an older pack reads OUTDATED |
| Pack `_HELIX_PLAN` rendering (bats or the pack test's Jinja fixture if one exists) | `SOAKING=600 BED_MESH=90` emits the anchored line; a value with `"` loses the quote |

Docs, in the same change:

- `docs/devel/PRINT_START_INTEGRATION.md`: a new § "Declaring phase durations" after
  "Option 1b: With Phase Tracking": the call, the rules of D2 in plain terms, the cap, that a
  declared phase should also be signalled, the worked example below; the two new signals in
  § "Phase Tracking (Optional)"; a troubleshooting line ("countdown ignores my plan": look for
  `HELIX:PLAN` warnings in the log).
- `docs/devel/PREPRINT_PREDICTION.md`: § "Declared Durations" (precedence, the planned
  population and residual, why not a total) and the history JSON schema.
- `assets/config/helix_macros.cfg` USAGE header; `CHANGELOG.md`.

Worked example for the doc:

```
[gcode_macro PRINT_START]
gcode:
    {% set bed = params.BED|int %}
    {% set soak = 600 if bed >= 100 else 0 %}
    {% set mesh = params.MESH|default(1)|int %}
    _HELIX_PLAN SOAKING={soak} BED_MESH={90 if mesh else 0}
    _HELIX_PHASE_HOMING
    G28
    _HELIX_PHASE_HEATING_BED
    M190 S{bed}
    {% if soak > 0 %}
    _HELIX_PHASE_SOAKING
    G4 P{soak * 1000}
    {% endif %}
    {% if mesh %}
    _HELIX_PHASE_BED_MESH
    BED_MESH_CALIBRATE
    {% endif %}
    _HELIX_READY
```

## Risks

- **Older app instances.** A second screen running a release without this change sees the plan
  line reach its profile patterns and may jump to SOAKING or HOMING for a moment; the next real
  signal corrects it. The pack ships with the app, so this needs a shared printer with mixed
  versions.
- **App restart mid-pre-print.** RESPOND lines are not replayed; a reconnecting app has no plan
  and falls back to history. Carrying the plan in `_HELIX_STATE` would survive a reconnect at the
  cost of Jinja building a Python literal; left out of v1 (Q5).
- **A pack older than the user's macro.** `_HELIX_PLAN` answers `Unknown command`, which Klipper
  reports without stopping the print (the same assumption #2006's D6 makes); the countdown is as
  today.

## Open questions for Preston

1. **Zero.** Should `PHASE=0` mean "this phase will not run" (D2), or be rejected as the issue
   proposes? Zero lets an adaptive macro withdraw a mesh or soak the history expects.
2. **Total.** Per-phase only (D2), or also a `TOTAL=` for macros that know only the sum?
3. **Cap.** A 6-hour per-phase cap (D4) against the issue's "no upper cap". The cap protects the
   timeouts from a typo; is 6 h right?
4. **Running over on screen.** The countdown already holds while a phase overruns. Should the
   time label also say it is running over (the issue's request), and if so, in words or as a
   `+N min` suffix? Needs a `ctl geom` pass before anyone draws it.
5. **Reconnect.** Is losing the plan when the app restarts mid-pre-print acceptable for v1, or
   should `_HELIX_PLAN` also store it in `_HELIX_STATE` so the app can read it back?
6. **Chamber signal.** Add `_HELIX_PHASE_HEATING_CHAMBER` with `_HELIX_PHASE_SOAKING` (D6), or
   soaking only as the issue asks?
