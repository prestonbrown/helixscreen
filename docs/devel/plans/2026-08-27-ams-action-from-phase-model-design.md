# Derive AmsAction from the backend's phase model

**Status:** steps 1-3 on main (the phase vocabulary, `OperationStep` `coarse` projection, and the U1 / AD5X IFS / tool changer migration; see "Step 3: done"). Steps 4 and 5 are open.
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

`system_info_.action = ` / `set_action(` call sites, 91 in the backends (2026-09-27), plus
three in `ams_subscription_backend.cpp` and `AmsState::set_action()`:

| backend | sites | has a phase model today |
|---|---|---|
| `ams_backend_mock.cpp` | 25 | no |
| `ams_backend_ad5x_ifs.cpp` | 22 | yes |
| `ams_backend_toolchanger.cpp` | 10 | yes |
| `ams_backend_cfs.cpp` | 9 | internal only (see below) |
| `ams_backend_ace.cpp` | 9 | no |
| `ams_backend_afc.cpp` | 6 | narration template |
| `ams_backend_openams.cpp` | 5 | no |
| `ams_backend_snapmaker.cpp` | 4 | yes |
| `ams_backend_happy_hare.cpp` | 1 | narration template |
| `ams_backend_qidi.cpp` | 1 | no |

Regenerate with:

```bash
grep -rc "system_info_.action = \|set_action(" src/printer/ams_backend_*.cpp
```

The UI reads more of the enum than the list above says. Counting `AmsAction::` in
`src/ui/`: ERROR 15, IDLE 12, HEATING 6, LOADING 4, UNLOADING 4, CUTTING 3, FORMING_TIP 3,
PURGING 2, SELECTING 1, RESETTING 1, PAUSED 1, CHECKING 1. Most of the tail is two places:
the device operations overlay's status text (`action_to_string`, one string per value) and
the legacy step bar (`AmsOperationSidebar::get_step_index_for_action`, which maps HEATING,
CUTTING, FORMING_TIP and PURGING to steps for backends with no model). HEATING also lights
the path canvas heat glow (`ui_panel_ams.cpp`). Shrinking the enum means retiring those
first.

## Step 1: the phase vocabulary

`OperationStep::coarse` says what a step looks like to a consumer of the coarse action:
the kind of work for a step that does some (Heat -> HEATING, Cut -> CUTTING, Feed ->
LOADING, Retract -> UNLOADING, Purge -> PURGING), and the operation's direction for a step
that only positions the machine. `OperationStepModel::action_at(index)` reads it by
**position**, the same index the step bar highlights: nothing reads `phase_id`, and for
every phased backend today it equals the position.

A projection needs the operation as well as the index, because each `StepOperationType`
has its own model (the U1's index 3 is Feed on a load and Retract on an unload).
`AmsState::get_active_step_operation()` is the one the bar is rendering. Index -1 names no
step, so IDLE, ERROR and PAUSED stay outside the phase model and keep being assigned.

| backend | phases (position: label -> projection) | where the action comes from |
|---|---|---|
| Snapmaker U1 | load: 0 Home -> LOADING, 1 Select -> LOADING, 2 Heat -> HEATING, 3 Feed -> LOADING, 4 Purge -> PURGING. unload: 0 Home -> UNLOADING, 1 Select -> UNLOADING, 2 Heat -> HEATING, 3 Retract -> UNLOADING. Preload and manual feed reuse the load model. | `classify_channel_state()`: one action per direction for the whole operation |
| AD5X IFS | unload: 0 Heat -> HEATING, 1 Cut -> CUTTING, 2 Retract -> UNLOADING. load and swap: 0 Heat -> HEATING, 1 Feed -> LOADING, 2 Purge -> PURGING | `apply_phase_action_locked()` while its tracker runs; hand assignment otherwise |
| Tool changer | Release -> SELECTING, Dock -> UNLOADING, Pick -> SELECTING, Change -> SELECTING, Grip -> SELECTING (sequence depends on op and on which signals the controller reports) | the `operation` / `state` word per frame |
| CFS | none published. Its phase tracker synthesizes CUTTING -> UNLOADING -> LOADING -> PURGING into the action, and the bar comes from the legacy action switch. A declared Cut / Retract / Feed / Purge model would project exactly what it assigns today. | `apply_synthesized_action_locked()` + hand assignment |
| AFC | narration: heat -> HEATING, cut -> CUTTING, unload -> UNLOADING, feed -> LOADING, poop / brush / kick -> PURGING, load -> LOADING (proposed; `ToolchangePhase` has no `coarse` yet) | AFC's status string via `ams_action_from_string()`; the phase comes separately from console `//` lines, so the two can disagree |
| Happy Hare | narration: heat, form_tip, cut, unload, select, feed, purge, load | `mmu.action` via `ams_action_from_string()`. The phase is looked up FROM the action (`sync_narration_step`), so deriving the action from the phase runs backwards here: stays on direct assignment |
| ACE | none: firmware reports a status word only | `ams_action_from_string()` |
| OpenAMS | none: `ready` / `loaded` only | pending and reported actions |
| QIDI Box | none: a `tool_change` variable and blocked slots | LOADING / IDLE / ERROR from save_variables |
| Mock | none of its own; simulates every value including CHECKING, PAUSED, RESETTING | 25 hand sites |

### Where the projection and the assignment disagree today

Step 2 declares the projections for the three phased backends and logs at debug (`[AmsSidebar]
Step N projects X but the backend assigned Y`) wherever they differ. Found by reading:

- **Snapmaker, Heat (both directions):** assigned LOADING / UNLOADING, projects HEATING.
  Migrating lights the heat glow and "Heating..." on the U1, and gives
  `detect_step_operation()` a LOADING -> HEATING -> LOADING sequence it has never seen
  there. Check the bar in `--test` before migrating.
- **Snapmaker, Purge:** assigned LOADING, projects PURGING. This is the PURGING open item
  below.
- **Tool changer, closing Grip frame:** the action goes IDLE on the same frame the phase
  lands on Grip, so for one frame Grip projects SELECTING against IDLE. Derived, the swap
  would read busy until the next frame parks the phase at -1.
- **Tool changer, opening Release frame on our own dispatch:** the action is the sidebar's
  optimistic HEATING marker, projection SELECTING.
- **AD5X:** none while its tracker runs. Its tool change, externally started IFS activity
  and zmod change assign LOADING / UNLOADING / SELECTING with the phase at -1, so nothing
  projects there. The phase also lags the action at dispatch: HEATING is set at once, the
  phase only on the first extruder frame.
- **AFC:** unmeasured. Narration and the status string are independent sources; declaring
  `coarse` on `ToolchangePhase` is what would make the sidebar log measure it.

Two constraints for the migration:

- `AmsBackendToolChanger::get_operation_step_model()` is not a pure query: it snapshots the
  latches its later phase resolution depends on. A derivation must not call it; read the
  model the sidebar already built, or split the snapshot out first.
- Projecting the U1's Home / Select as SELECTING instead of the direction would hide its
  bar through them: the U1 uses the default `action_tracks_step_operation()`, which leaves
  SELECTING out.

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

Steps 1 and 2 are worth doing even if the rest stalls. Both are on
`feature/ams-action-from-phases`: step 1 is the vocabulary above, step 2 is `coarse`,
`action_at()` and the sidebar's debug line.

## Step 3: done

The U1, AD5X IFS and tool changer publish their current step's projection. The shared
rule is `AmsBackend::project_action()`: while the backend's own assignment says an
operation runs, the published action is the step's projection; IDLE, ERROR and PAUSED
always stand. `AmsSubscriptionBackend::published_action_locked()` applies it for both
`get_current_action()` and a backend's `get_system_info()`, fed by the
`step_action_locked()` hook.

- **U1:** one table (`u1_step_action()`) feeds the step model and the published action.
- **AD5X IFS:** the tracker already assigned per phase; its action now comes from the same
  table (`kIfsUnloadPhases` / `kIfsLoadPhases`) the step model reads. No behaviour change.
- **Tool changer:** `resolve_step_locked()` returns the step's projection with its index.
  An unmount's Release step now reads SELECTING instead of the dispatch's UNLOADING.

What the migration taught, which changes steps 4 and 5:

- **The hand assignment is not dead on the U1 or the tool changer, so step 5 cannot
  delete it.** Both parsers read `system_info_.action` back as operation state: the U1
  closes an operation by matching LOADING / UNLOADING, and the tool changer's
  `was_mid_operation` is what tells a closing grip from a resting gripper. The projection
  is applied where the action is published, not written into that state. Step 5 should
  rename the field to what it is (the backend's operation state) rather than delete it.
- **The closing Grip frame reads IDLE, not SELECTING.** It is the frame that ends the
  swap, and projecting it would leave the machine busy until another frame parks the
  phase; Moonraker republishes only what changed, so no such frame is guaranteed. The
  earlier "take the projection" decision assumed a following frame.
- **The sidebar's disagreement log now fires by design** on that closing grip. It
  measured step 2 and should go in step 5.
- **The sidebar's post-load cooldown edge takes PURGING -> IDLE as well as
  LOADING -> IDLE**, since a load on a purging backend ends in its purge.
- **Runout suppression narrows on the U1.** `AmsState::is_filament_operation_active()`
  suppresses only LOADING, UNLOADING and SELECTING, so a sensor edge during the U1's Heat
  or Purge step is no longer suppressed. That is how the AD5X already behaves and matches
  that function's rule (a stationary step's sensor change is a real fault); worth
  confirming on the U1 rig.
- **The U1 cannot be verified under `--test`.** `HELIX_MOCK_AMS=snapmaker` builds
  `AmsBackendMock`, which has no step model, and no mock printer advertises the U1's
  `filament_feed` objects for `--real-ams`. Its migration is unit-tested only; the rig
  should show "Heating..." and the heat glow on the Heat step, and "Purging" on Purge.

## Decided: the U1 shows Heating while it heats

**Decision (Preston, 2026-09-27):** the Snapmaker U1's Heat step projects `HEATING` in
both directions, and migrating the U1 takes the visible change: "Heating..." and the heat
glow for that step where the hand assignment reports LOADING or UNLOADING today. The
one-frame tool changer disagreements (closing Grip, opening Release) take the projection
as they fall out.

## Decided: PURGING starts an operation

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

**Decision (Preston, 2026-09-27): keep it.** A purge is an operation, so an
externally-started operation whose first action is `PURGING` shows the step bar. The
reason is stated at `include/ams_types.h#ams_action_is_filament_operation`, and the
migration projects Purge steps to `PURGING` on that basis.

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
