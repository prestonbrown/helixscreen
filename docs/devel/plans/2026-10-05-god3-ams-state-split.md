# GOD-3: AmsState split and the recursive mutex

Audit finding GOD-3 (`2026-09-30-architecture-audit.html#GOD-3`). `src/printer/ams_state.cpp` is
3,701 lines behind one recursive mutex. Delete this file when the tranche ships.

## Bugs to ship first (separate fix branch)

1. `is_filament_operation_active()` reads the `ams_action_` subject from the WebSocket thread
   (FilamentSensorManager::update_from_status) while the main thread writes it.
2. FilamentSensorManager takes a raw pointer from `get_backend()` and uses it after the lock
   drops; `clear_backends()` on reconnect can free it in that window (use-after-free).

## Off-main callers today

- `moonraker_discovery_sequence.cpp` calls `any_filament_batch_in_flight` on the WS thread.
- `FilamentSensorManager::update_from_status` (WS thread): `is_filament_operation_active`,
  `post_unload_runout_grace_armed`, `get_backend()->get_type()`.
- Snapmaker's status handler calls `mark_slot_unloaded`.
- The toolchanger backend, holding its own mutex, calls the lock-free
  `get_ams_operation_phase_subject` and `get_active_step_operation`.

Everything else is main thread (discovery is `queue_update`d, `on_backend_event` only queues).

## Plain mutex by shrinking what it guards

- `AmsBackendRegistry` owns backends, sinks and the gcode callback behind a `std::mutex`; add/clear
  are main-only (asserted). Off-main readers: `any_filament_batch_in_flight()` and a new
  `primary_type()` replacing `get_backend()->get_type()`.
- `RunoutGrace` (grace flag, timestamp, last unload time) gets its own leaf mutex.
- `set_action` mirrors into `std::atomic<AmsAction>`, so `is_filament_operation_active` stops
  reading a subject.
- Everything else (projection, getters, clog, dryer, narration, external spool) drops the lock and
  asserts `is_main_thread()`.
- Lock order: Registry -> AmsBackend::mutex_, only in the two off-main queries. Projection holds
  no AmsState lock while calling into backends or FilamentSensorManager. No cycle.

## Seams (AmsState stays the facade; 80 src files and ~930 test sites unchanged)

`AmsSubjects` (main-only storage and getters), `AmsBackendRegistry`, `AmsProjection`
(`sync_from_backend` split into named steps), tool mapping as free functions over a const
registry, `ExternalSpoolStore`, clog and dryer as projection helper files.

## Commits (pinning test first)

1. Fold the color/status/fill/material getter overloads onto `backend_slot_subject()`.
2. Atomic action mirror; FSM uses `primary_type()`. Pin: a thread hammering both while main
   cycles `set_action` and `clear_backends`/`add_backend` (red under TSAN today).
3. Extract `RunoutGrace`.
4. Extract `AmsBackendRegistry` (still recursive); pin the `clear_backends` order.
5. Drop the lock from main-only methods; assert main thread.
6. Registry to `std::mutex` (a missed recursion hangs loudly).
7. Pure moves: external spool, tool mapping, subjects, clog, dryer.
8. Split `sync_from_backend` into named steps.

About 9 commits, ~-150 net lines.

## Verification

- After 2, 5, 6: `scripts/zeus-run.sh tsan-app` with mock AMS at `--sim-speed 6`, looping
  load/unload and reconnect. Unit tests under TSAN need a `tsan` job added to zeus-run.sh, or
  `make test-tsan` by hand in the helix-tsan container on `[ams]`, `[filament_sensor]`,
  `[spoolman]`.
- After 4 and 7: `zeus-run.sh asan '[ams]'`.

## Risks

- A hidden off-main caller of a main-only method races silently in release builds; asserts catch
  it in tests, tsan-app is the backstop.
- Primary (12) vs secondary (7) slot arrays: unifying them changes the XML contract; out of scope.
- A future backend calling the registry under its own lock would create a cycle; comment the
  registry mutex.
- Unverified: consumption sink and FilamentConsumptionTracker threads; ToolState locking under
  `sync_from_backend`.
