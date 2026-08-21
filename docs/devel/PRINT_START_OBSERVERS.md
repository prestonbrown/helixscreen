# Print-Start Observer System

How HelixScreen turns "the printer is preparing a print" into a truthful status
line, an ETA, and a progress bar - across firmwares that narrate everything,
firmwares that narrate nothing, and firmwares in between.

Three sibling docs own the adjacent detail:

- [PRINT_START_PROFILES.md](PRINT_START_PROFILES.md) - the JSON profile schema
  (patterns, weights, flags) and how to author one for a new printer.
- [PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md) - user-facing setup,
  pre-start gcode options, and the silent-phase signal descriptions.
- [PREPRINT_PREDICTION.md](PREPRINT_PREDICTION.md) - the ETA engine: phase
  timing, thermal model, weighted history buckets.

## The pipeline

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

The collector is a `shared_ptr` owned by `MoonrakerManager`, recreated on every
printer switch. Every background-thread entry point holds it via `weak_ptr`
(`s_collector`) and drops out if the collector was replaced.

## Signal sources

| # | Source | Arrives on | Gate | Effect |
|---|--------|-----------|------|--------|
| 1 | `notify_gcode_response` lines | WS thread (client callback) | active + profile patterns | phase transitions, messages; `real_signal_seen_` latches and mutes proactive detection |
| 2 | `probe at X,Y is z=Z` lines (subset of #1) | WS thread | active, in/pre-mesh | mesh point counting (N/M); intercepted BEFORE pattern matching so they can never re-announce BED_MESH and reset the denominator |
| 3 | bed-mesh presence flap (`bed_mesh.probed_matrix` present→absent) | WS thread (`MoonrakerAPI` bed_mesh callback → `set_bed_mesh_presence_observer`) | active + phase==CLEANING | enters BED_MESH ("Bed Leveling...") + fetches the probe denominator; observer copied under a mutex (weakly-ordered targets read stale-null otherwise) |
| 4 | toolhead position subjects (`toolhead.position`, already subscribed) | main thread (queued status updates; 3 permanent `ObserverGuard`s) | active + profile `position_signals` | `PrintStartPositionClassifier`: centre Z-descents → "Probing Z...", ≥3-distinct-corner tour → "Checking Bed Mesh...", row march → BED_MESH entry |
| 5 | heater targets / temps / layers / progress (fallback observers) | main thread (subject observers) | active + `enable_fallbacks()` + NOT `real_signal_seen_` | proactive heating phases, adaptive timeouts, completion fallbacks |

Sources 3 and 4 exist because Creality K1-class firmware forwards nothing to
the console for ~3 minutes of Z-probing, corner validation, and mesh sweep -
while sources that ARE not console output keep flowing. They only fill silence:
a real console marker always outranks them, and their refinements are
message-only unless the sweep march promotes BED_MESH (the same edge as the
flap, so the two corroborate each other).

## Arming and windows

`should_start_print_collector()` arms on a STANDBY→PRINTING transition with
zero progress AND zero `print_duration` (joining a print mid-job must not
raise the pre-print overlay). `PrintStartCollector::note_host_side_pre_start()`
declares that the dispatch came from our own pre-start gcode block; the
prediction history then re-filters to the host-pre-start bucket so its minutes
of extra work never average into printer-edge prints (and vice versa).

## ETA path (short version - see PREPRINT_PREDICTION.md)

`compute_predicted_weights()` blends the thermal model (heater targets learned
rates) with history-bucket per-phase durations. The monotonic countdown anchor
re-baselines whenever the weights' inputs change (heater targets newly set or
risen ≥15°C) - not merely when time passes - so a provisional 0°C-targets
estimate can't freeze the display when the real one arrives a second later.
Downward moves ease (capped per tick); remaining never rises between
input changes.

## Threading and lifetime rules

- Console lines (#1/#2) and the flap (#3) fire on the WS thread; the collector
  serializes everything through `state_mutex_`. `lv_tr` at that call site is
  the documented #1219 debt family - follow the existing `note_bed_mesh_presence`
  shape if you add a sibling.
- Position and fallback observers (#4/#5) fire on the main thread via queued
  subject sets. Their guards are permanent for the manager's lifetime and
  early-out on `!active_` before locking - the same shape as the heater-target
  fallback observers.
- `MoonrakerManager::shutdown()` RELEASES (not resets) every observer guard:
  subjects may already be deinitialized at that point (#579 family). Any new
  guard must be added there.
- The bed-mesh presence observer is copied under `bed_mesh_presence_mutex_`
  before invocation - see `fix(api): guard bed-mesh presence observer against
  cross-thread staleness` for the on-device race this closes.

## Where the behavior is pinned

| Behavior | Test |
|----------|------|
| K1C mesh sweep keeps its denominator (N/25, no sub-phase wipe) | `test_print_start_collector.cpp` - K1C replay fixture |
| Flap during CLEANING enters Bed Leveling | `test_print_start_collector.cpp` - flap test |
| ETA re-baselines on heater-target arrival and staged rise | `test_print_start_collector.cpp` - `[eta]` tests |
| Position chain wipe → centre → corners → sweep on real captures | `test_print_start_position_classifier.cpp` (corpus: `tests/fixtures/print_start_position_corpus.json`, extracted from the 2026-08-19 K1C klippy.log capture) |
| Position → message/phase integration, and the no-flag negative | `test_print_start_collector.cpp` - `[position]` integration tests |
| K1C first-print ETA defaults from measured durations | `test_printer_detector.cpp` - `print_start_default_phases` |
| Whole chain (wiring, observers, collector) against a real capture, no printer | `HELIX_MOCK_REPLAY=<script> --test` (see MOCK_ENVIRONMENT_VARIABLES.md; script from `scripts/extract_mock_replay.py`) |
