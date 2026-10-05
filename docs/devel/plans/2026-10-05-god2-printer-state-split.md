# GOD-2: PrinterState forwarders and non-domain jobs

Audit finding GOD-2 (`2026-09-30-architecture-audit.html#GOD-2`). `printer_state.h` is 2,763 lines
declaring 284 methods; 196 are forwarders to domain sub-states (176 same-name, 20 renamed).
`printer_state.cpp` is 1,470 lines. PrinterState stays a Meyers singleton via
`get_printer_state()`. Delete this file when the tranche ships.

## Seams

- One non-const accessor per domain (`print_state()`, `temperature_state()`, `motion_state()`,
  ...), folding the three existing differently-shaped accessors into that naming. Same-name
  forwarders are deleted once callers insert `.X_state()`; the 20 renamed ones use a rewrite map
  (15 become `caps.subject(Capability::X)`).
- Stays on PrinterState: every setter that defers through `async_lifetime_` (the WebSocket thread
  calls these public wrappers; `set_*_internal` runs on the main thread), `update_from_status`,
  `set_hardware`, init/deinit, `get_subjects_lifetime`, and the cross-domain predicates
  (`is_blocking_operation_active`, `can_start_new_print`).
- Leaves PrinterState:
  - exclude_object parse to `PrinterExcludedObjectsState::update_from_status`;
  - Klippy freshness (parse, watermark, mutex, epoch) to
    `PrinterNetworkState::apply_webhooks`; `reset_klippy_state_freshness` keeps its synchronous
    locked semantics;
  - chamber resolution to a free `chamber::apply_resolution(...)`;
  - printer profile (type, z-offset strategy, external persistence, dynamic options, firmware
    option defaults) to a new `PrinterProfileState`;
  - Helix plugin status to `PrinterPluginStatusState`;
  - cross-singleton fan-out (LedController, sensor managers, TimelapseState, probe) to
    MoonrakerManager's dispatch beside `ToolState::update_from_status`.
- Dead: three friend declarations naming removed functions, ~49 methods with no src caller.

## Commits (pinning test first)

1. Delete dead friends, no-caller forwarders and the unused composite getter.
2. Per-domain accessors (pinned by pointer identity).
3. Rewrite callers one domain per commit (versions, hw_validation, calibration, excluded_objects,
   fan, network, capabilities, motion, temperature, print last: 274 src / 766 test sites). A
   Python script deletes the forwarders, compiles, and inserts `.X_state()` at each reported
   location; with no base class a deleted name cannot silently rebind.
4-9. Move exclude_object (pin: bbox, null current object, non-numeric point skipped); freshness
   (pin: an older epoch must not move the watermark); fan-out (pin: LED cache and sensor managers
   still see frames); chamber; plugin status; `PrinterProfileState` (pin: a persistence provider
   that latches then is refuted).

`make t F=` per commit; full-test-run, ASAN and `make mutate-diff` once at the end.

| | Now | After |
|---|---|---|
| printer_state.h | 2,763 lines | ~850 |
| methods | 284 | ~70 |
| printer_state.cpp | 1,470 lines | ~800 |

Call-site churn ~750 src sites (~140 files) and ~1,800 test sites (~150 files), all scripted.
The rebuild fan-in (~431 TUs) does not drop: the header still includes all 13 domain headers.

## Risks

- ESP32: printer_state and the domain .cpp files are in `app_srcs.txt`; `printer_profile_state.cpp`
  joins it (column 0, own line). Moved code stays exception/RTTI-free (`json_util::safe_*`).
- Test isolation: moved members keep being reset by the existing TestAccess helpers.
- `set_hardware` order is load-bearing (plr capability before status, dynamic options after
  discovery); extractions keep the call order exactly.
- ~290 files change: one domain per commit, announced to peers.
- Caller counts are regex-based, not compiler-verified.
