# AMS-11: splitting the AMS status god functions

Audit finding AMS-11 (`2026-09-30-architecture-audit.html#AMS-11`). Snapmaker `handle_status`
(`ams_backend_snapmaker.cpp`, 1,100 lines in one lock scope), Happy Hare `parse_mmu_state` (~900
lines), CFS `handle_status` (~680, already parse/apply split). All run on the main thread (the
subscription base defers every notify). Delete this file when the tranche ships.

## Seams

- Pure parse functions taking `const json&` and returning structs whose fields are
  `std::optional`, so an omitted field reads as nullopt and a Moonraker delta frame cannot reset
  it. Snapmaker: `snapmaker_status_parse.{h,cpp}` (extruders, active extruder, filament_detect,
  feed channels, print_task_config extending `read_print_preferences`, toolhead sensors, batch).
  Happy Hare: gate identity, telemetry, topology.
- Apply steps `apply_X_locked(const Delta&, FrameEffects&)`; `FrameEffects` replaces the loose
  frame locals; `converge_locked` takes the tail. `handle_status` becomes parse all, apply in
  today's order under the lock, dispatch effects after releasing it.
- Shared `ams::read_indexed<T>(json, key, n)` for the hand-rolled indexed-array loops (check
  json_util first).

## Behaviour to preserve

- Snapmaker order: extruders, active tool, feed (its error gate reads current slot/tool); RFID
  before feed (an insert rise in frame N is consumed by RFID in N+1); print_task_config identity
  after RFID; feed, batch, working slot; sensors, then converge.
- Happy Hare: capture the fault before `reason_for_pause` is rewritten; `gate_color_rgb`
  suppresses `gate_color`; the tail reads the previous frame's status before
  `refresh_gate_statuses_locked`; the fault edge last.
- CFS: seated stamp cleared first, applied last; the merge-patch accumulator resets on a flat
  frame; current slot after runout, runout episode after current slot.
- Absent `channel_state` means no change, absent `channel_error` means "ok".
- AmsState calls, queue_update, gcode and events stay after the backend mutex is released.

## Bug found

`parse_extruder_state` default-constructs its result and the caller replaces the stored state
wholesale, so a temperature-only extruder delta zeroes park_pin, active_pin and state. Fixed in its
own commit with a failing test first.

## Commits (pinning test first, `make mutate-diff` named)

1. Golden-sequence pin: one full Snapmaker frame then deltas (load, unload, RFID insert, batch
   fail, temperature-only extruder update); snapshot slots, action, working slot, effects.
2. Fix the extruder delta wipe (the one intended behaviour change).
3. `FrameEffects`. 4-9. One section each: extruders/active tool, filament_detect, feed channels,
   batch/working slot, print_task_config, toolhead sensors. 10. `converge_locked`.
11. Snapmaker colour parsing onto `read_lane_color` (behaviour change, separate).
12. Happy Hare: golden sequence, then identity, telemetry, topology parsers.
13. CFS: name the locked apply steps; parsers for its small sibling objects.

About +500 lines net (the audit expected no reduction; the win is testability).

## Risks

- Almost no real status captures exist: only `tests/fixtures/cfs_fork_four_box_L8MMBCCK.json`.
  Needed: JSONL recordings of notify_status_update from a U1 (load, unload, RFID insert, batch) and
  a Happy Hare box. Without them the goldens pin today's behaviour on synthetic frames only.
- Ordering regressions are the main risk; the golden sequences catch them.
- Snapmaker churns heavily; land steps one at a time.
- `docs/devel/FILAMENT_MANAGEMENT.md` still says notifies arrive on the WebSocket thread; fix it.
