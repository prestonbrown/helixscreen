# Derive AmsAction from the backend's phase model

**Status:** not started. Target branch: `main`.
**Precursor:** `f6e866600` (`fix(toolchanger): show a step bar that matches a hotend changer`), on main.

## Why

An AMS backend describes its current operation **twice**, and the two descriptions are
maintained independently:

1. `system_info_.action` (`AmsAction`) - a coarse shared enum, assigned by hand.
2. `OperationStepModel` + `system_info_.operation_phase` - the rich per-backend phase list,
   declared by the backends that have one.

Nothing keeps them in agreement. Every bug fixed in `f6e866600` was that disagreement:

- The MedusaHC fork reports its phase as `changing`, which matched no branch in
  `apply_tool_sensor_locked()`, so the action was never updated during a swap while the
  phase model was tracking it correctly.
- The step bar's visibility and its operation-start detection were two hardcoded
  `AmsAction` lists that disagreed with each other about `SELECTING` and `PURGING`. A tool
  changer fell through both.

`f6e866600` fixed the symptoms by giving the question a single override point
(`AmsBackend::action_tracks_step_operation()`). That is a smaller step, not the
destination: the duplication is still there, at 79 assignment sites.

## The shape to aim for

The phase model becomes the single source of truth. `AmsAction` survives only as a
**derived projection** for the handful of consumers that need a coarse answer.

Each `OperationStep` gains its coarse projection, so a backend declares the mapping once
alongside the phase it belongs to, instead of restating it at every assignment site:

```
struct OperationStep {
    std::string label;
    int phase_id = -1;
    bool optional = false;      // declared today, read by nothing - fix or delete
    bool live_temp = false;
    AmsAction coarse;           // NEW: what this phase looks like to generic consumers
};
```

Then:

- `system_info_.action` is computed from the current phase, not assigned.
- `ams_action_is_busy()` becomes "the current phase is not the idle one".
- `AmsBackend::action_tracks_step_operation()` **is deleted**. "Should the bar follow this"
  becomes "the backend has phases and is in one." Deleting the abstraction added in
  `f6e866600` rather than extending it is the signal this is the right shape.

A backend with no phase model keeps assigning `action` directly during migration, so the
two systems can coexist while backends move over one at a time.

## Why this is safe to scope down

The shared enum has almost no readers left. Every place the UI tests a specific
`AmsAction` value, after `f6e866600`:

```
ERROR x4 · IDLE x2 · UNLOADING x1 · LOADING x1 · HEATING x1
```

Twelve enum values; the UI meaningfully distinguishes three. Everything else it needs
already comes from the phase model. So the target is *fewer* coarse states, not more.

## Inventory: what has to move

`system_info_.action = ` / `set_action(` call sites, 79 total:

| backend | sites | has a phase model today |
|---|---|---|
| `ams_backend_mock.cpp` | 26 | no |
| `ams_backend_ad5x_ifs.cpp` | 15 | yes |
| `ams_backend_toolchanger.cpp` | 9 | yes (added in `f6e866600`) |
| `ams_backend_cfs.cpp` | 9 | no |
| `ams_backend_ace.cpp` | 9 | no |
| `ams_backend_afc.cpp` | 6 | narration template |
| `ams_backend_snapmaker.cpp` | 4 | yes |
| `ams_backend_happy_hare.cpp` | 1 | narration template |

Regenerate with:

```bash
grep -rc "system_info_.action = \|set_action(" src/printer/ams_backend_*.cpp
```

## Suggested sequence

1. **Write the phase vocabulary down first, for all eight backends, before touching any
   code.** This is where the risk lives, not in the mechanical edit. A backend whose phases
   are wrong will look fine in tests and wrong on a printer.
2. Add `coarse` to `OperationStep` and a derived-action path, with the old assignment still
   winning. No behavior change; both systems live side by side.
3. Migrate the three backends that already declare a phase model (Snapmaker, AD5X IFS, tool
   changer). Verify each in `--test`.
4. Give the remaining backends a phase model, or leave them on direct assignment
   permanently if their firmware genuinely reports no phases.
5. Delete `action_tracks_step_operation()` and the direct assignments that are now dead.

Steps 1 and 2 are worth doing even if the rest stalls.

## Open item carried over: the PURGING behavior change

`f6e866600` unified two action lists that had drifted apart. They disagreed about
**`PURGING`** as well as `SELECTING`, and unifying meant picking one:

| | old `show_progress` (sidebar) | old `is_active_action` (detection) | new shared default |
|---|---|---|---|
| `PURGING` | yes | **no** | **yes** |
| `SELECTING` | no | no | no (tool changer overrides) |

So an externally-started operation whose **first** action is `PURGING` now counts as an
operation start, where previously it did not. Consequence: the step bar is created and
shown for that transition.

This is the more consistent answer and the disagreement looked accidental rather than
designed, but it is a real behavior change for filament systems that was inherited rather
than intended. The full suite is green, which means no test covered it, not that no user
notices it.

**Decide deliberately during this work** rather than leaving it as an accident of a
refactor: either keep it (a purge is an operation, showing the bar is right) or make
`PURGING` a non-start action explicitly, with a comment saying why. Both are defensible;
what is not defensible is it staying an unexamined side effect.

Relevant if a report arrives about a step bar appearing during an AFC or Happy Hare purge.

## Open item carried over: the MedusaHC fork mock is asserted, not observed

`HELIX_MOCK_AMS=medusahc-fork` reproduces topi314's status schema, read from
`scripts/medusahc.py` in that repo. The unit tests pin that shape, and the mock is
self-consistent with our reading of it.

**No frame from a real machine has ever gone through it.** A debug bundle from a machine
running that fork is the cheap way to confirm the mock matches reality rather than matching
our interpretation. Until then, treat fork behavior as unverified against hardware.

## Branch strategy

Branch off `main`, not off `feature/medusahc-mock-and-steps`.

`main` is the trunk and this work is 1.1-shaped, so it originates there and needs no
porting. It is not a candidate for `release/1.0`, which takes only what the 1.0 fleet
needs and receives it by cherry-pick. See `BRANCHING.md`.

```bash
scripts/setup-worktree.sh feature/ams-action-from-phases
```

## Files this will touch

| path | why |
|---|---|
| `include/ams_backend.h` | `OperationStep::coarse`, delete `action_tracks_step_operation()` |
| `include/ams_types.h` | `ams_action_is_busy()` / `ams_action_is_filament_operation()` become derived or go away |
| `include/ams_step_operation.h` | `detect_step_operation()` loses its passed-in predicate |
| `src/printer/ams_backend_*.cpp` | the 79 sites |
| `src/ui/ui_ams_sidebar.cpp` | asks the phase model instead of the action |
| `src/ui/ui_ams_slot.cpp` | pulse asks the phase model |
| `tests/unit/test_ams_step_operation.cpp` | signature change, 19 call sites |

## Related reading

- `docs/devel/FILAMENT_BACKEND_MEDUSAHC.md` - the two shipping configurations, the
  `state`-vs-`operation` trap, and the step bar section
- `docs/devel/FILAMENT_BACKEND_TOOLCHANGER.md` - step bar suppression
- `docs/devel/FILAMENT_MANAGEMENT.md` - backend overview
