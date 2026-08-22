# Print state: one axis, named capabilities

Status: **Phases 0a-4c complete** - merged in v0.99.115; only Phase 5 (the rename) remains open, see the Phase tracker
Branch: `fix/preparing-job-lifecycle` (Phase 0 only) → own branch for Phases 1-4

> **Resuming after a context break?** Read "Phase tracker" and "How to resume"
> at the bottom first. Every phase has explicit exit criteria; do not start one
> until its predecessor's criteria are met.

---

## The defect class

Code asks a **semantic** question - *"does a job own the toolhead right now?"* -
of a **raw wire value**: `helix::PrintJobState`, parsed straight from Moonraker's
`print_stats.state`. That value cannot express a job the app has committed to but
the printer has not reported yet, so every consumer keyed on it inherits the same
blind spot.

There are two types with nearly the same name, and the collision is a cause, not
a cosmetic problem:

| Type | What it is | Where |
|---|---|---|
| `helix::PrintJobState` | The wire. What `print_stats.state` said. | `include/printer_state.h` |
| `PrintState` | The derived UI lifecycle, including `Preparing`. | `include/print_lifecycle_state.h` |

Someone writing a toolhead guard reaches for the type called "print state" and
gets the wire. `print_occupies_toolhead(PrintJobState)` is a semantic question
with a wire parameter - the body is not wrong, it simply cannot see `Preparing`
and never could. **Adding a `has_preparing_job` bool parameter would patch one
call site while preserving the shape that generates the next one.** That idea was
considered and rejected.

## Evidence

Two censuses, 2026-08-19. Findings marked **(verified)** were re-checked by hand
rather than taken from the survey.

### The existing helper was never adopted

`print_occupies_toolhead` (`include/printer_state.h:113`) has **2 call sites**
(`ams_subscription_backend.cpp:318`, `ui_bypass_toggle_controller.cpp:28`), while
**~60 sites open-code `PRINTING || PAUSED` inline**. It was introduced as a single
source of truth and nothing migrated onto it. Worth remembering before we build
another one: *a helper nobody is forced onto does not become the answer.* That is
what Phase 4's gate is for.

**But adoption is only half of why it stalled, and the other half is the more
useful half.** It was introduced by `df22cf6f2` (2026-07-29, bundle JX2FVRB9) to
fix a real defect: a runout-paused user followed Klipper's own *"load it and
press RESUME"* prompt into a guaranteed refusal, because the AMS menu and
filament panel gated on load state while the backend refused on
`PRINTING || PAUSED`. One shared definition, so the affordance and the refusal
could not drift.

Nine days later `efab3a6b5` (2026-08-07) relaxed PAUSED *per backend* - allowed
unless the backend's macro homes itself. That answer needs a second input,
`filament_ops_self_home()`, and a function taking only `PrintJobState`
structurally cannot accept one. `print_blocks_filament_op()` was written and took
over the very sites `print_occupies_toolhead` had been created for, leaving it
with two residual callers and no constituency.

So **"does a print own the toolhead" has now proved unanswerable from the job
state alone twice** - once because the answer depends on a backend capability,
once because it depends on a window the wire cannot represent. Both times the fix
was an input the original signature could not take. Treat `job_holds_machine()`
as provisional on the same grounds: the rule that it is *a convenience over the
lifecycle, never a replacement for switching on it* is what stops a third round.

### Nine competing definitions of "is a print active"

Exactly one includes `Preparing`, and it has a single external consumer.

| # | Predicate | True for | Sees Preparing | Non-test call sites |
|---|---|---|---|---|
| 1 | `print_occupies_toolhead(PrintJobState)` | PRINTING, PAUSED | no | 2 |
| 2 | `status_indicates_active_print(json)` | PRINTING, PAUSED | no | 3 |
| 3 | `print_active` subject | PRINTING, PAUSED | no | 2 C++, **21 XML** |
| 4 | `is_active_print_state(PrintJobState)` | PRINTING, PAUSED | no | 3 |
| 5 | `PrintLifecycleState::is_active(PrintState)` | Printing, Paused, **Preparing** | **yes** | **1** (`ui_panel_print_status.cpp:3227`) |
| 6 | `PrintLifecycleState::want_viewer()` | everything but Idle | yes | 2 |
| 7 | `print_in_progress` subject | the preparing window, hand-maintained | **yes** | see below |
| 8 | `print_blocks_filament_op(printing, paused, self_homes)` | PRINTING; PAUSED only when self-homing | no | 4 |
| 9 | `is_blocking_operation_active()` | inverted: busy-but-not-printing | no | 2 |

Plus a fifth ad-hoc state set with no named predicate at
`spoolman_manager.cpp:99-100` (`PRINTING || COMPLETE || PAUSED`).

The one consumer of #5 is the panel's Tune/Timelapse gate
(`ui_panel_print_status.cpp:3227`, `PrintLifecycleState::is_active(state)`); the
class also uses it internally four times (`print_lifecycle_state.cpp:136`, `:144`,
`:153`, `:167`). So one site out of roughly sixty that ask "is a print happening"
asks it of the predicate that can actually answer.

Two others needed `Preparing` badly enough to hand-patch it inline rather than
reach for #5: `ui_emergency_stop.cpp:284-288` and `print_control_view.cpp:12-14`.
The second was written during this work - the correct predicate was in the header
the whole time and was not found. That is the argument for Phase 4's gate in one
sentence: a correct helper that nobody is *forced* onto gets reinvented at each
call site.

> **Correction, 2026-08-19.** An earlier draft of this plan said #5 had *zero*
> call sites and called it dead code. That was taken from a survey and not
> checked. It has one. The conclusion is unchanged but the number was wrong -
> verify a census claim before building an argument on it.

### The safety gap (verified)

21 XML bindings disable the controls that would fight a running job for the
toolhead:

| What | Count |
|---|---|
| Bed-levelling calibration - QGL, Z-Tilt, bed screws | 8 |
| Z calibration + Save Z-Offset | 4 |
| User macro buttons 1-4 | 8 |
| The home bypass tile | 1 |

> **Correction, 2026-08-19.** An earlier draft called these "jog, motion and
> extrude" controls. They are not - the jog arrows and extrude buttons carry no
> `print_active` binding at all. The conclusion is unchanged and if anything
> sharper: every one of these 21 emits G-code that homes, probes, or rewrites the
> Z offset, which is precisely what a host-side pre-print block is already doing.

All 21 use the identical form:

```
<bind_state_if_eq subject="print_active" state="disabled" ref_value="1"/>
```

| File | Lines |
|---|---|
| `ui_xml/controls_panel.xml` | 108, 336, 345, 355, 370, 489, 496, 507, 514 |
| `ui_xml/micro/controls_panel.xml` | 95, 282, 296, 415, 424, 431, 441, 447, 453 |
| `ui_xml/motion_panel.xml` | 143, 150 |
| `ui_xml/components/panel_widget_bypass.xml` | 15 |

That last one matters: the home **bypass tile** binds `print_active`, and
`BypassToggleController::toggle()` guards on `print_occupies_toolhead()`. So the
bypass feature carries the blind spot in *both* its affordance and its handler -
during a host-side pre-print window the tile is enabled and the handler agrees to
run, driving filament through a toolhead that is homing or probing.

`print_active` is `PRINTING || PAUSED`. **During a host-side pre-print window
every one of these controls is enabled while the toolhead is homing and
probing.** Same hazard as the bypass toggle, 21 more sites. This is the reason
Phase 1 leads: the sweep is safety work, not tidying.

### A duplicate mechanism we created (verified)

`print_in_progress` (`printer_print_state.h:355`, `:602`, `:665`) already models
the preparing window - *"true during print preparation"*, per its own doc
comment. It is set imperatively from `PrintPreparationManager` and cleared by
hand on **ten-plus separate exit paths** (`ui_print_preparation_manager.cpp:630`,
`:639`, `:1350`, `:1358`, `:1410`, `:1419`, `:1493`, `:1559`, `:1610`, `:1669`,
and more).

The preparing-job work added `has_preparing_job()` + `preparing_epoch` without
finding it. Two mechanisms for one concept is the disease being treated. The new
one is the better shape - one owner, one exit point, `retire_preparing()` with a
reason - so the incumbent gets **derived from it**, not deleted outright (21 XML
bindings and several readers depend on a boolean subject existing).

## The design

**No new enum.** `PrintState` is already the derived model, and
`derive_print_state()` is already documented as "the single definition of this
mapping". The work is to finish that abstraction and give it a home outside a
panel - not to invent a parallel one.

Three parts:

1. **One axis.** `PrintState` becomes what everyone consults. `PrintJobState` is
   demoted to an implementation detail of `derive_print_state()` and of the sites
   that genuinely need wire semantics.

2. **Named capability predicates**, defined next to `derive_print_state()`:

   | Predicate | True for | Replaces |
   |---|---|---|
   | `job_holds_machine(lifecycle)` | Preparing, Printing, Paused | most `PRINTING \|\| PAUSED` guards |
   | `laying_material(lifecycle)` | Printing | progress/layer/ETA meaningfulness |
   | `job_ended(lifecycle)` | Complete, Cancelled, Error | terminal checks |
   | `machine_idle(lifecycle)` | Idle | calibration/firmware gates |

   The value is not brevity. `state == PRINTING || state == PAUSED` appears
   identically today for five unrelated reasons, and the call site does not say
   which. Naming the *why* is what makes the next lifecycle change safe.

3. **"Whose job" stays a separate axis.** `has_preparing_job()` answers which
   *mechanism* applies (retire vs `CANCEL_PRINT`), not what the user may do.
   Collapsing the two gives "Cancel is enabled and does the wrong thing" - learned
   the hard way in `print_control_view.cpp`.

### Do not over-collapse

`ams_subscription_backend.cpp:320-326` deliberately **allows** a filament op on a
PAUSED print **when the backend does NOT self-home** - because then no firmware
macro can hide a `G28`, and Layer 1
(`reject_homing_during_active_print`) still refuses any the app emits itself.
`print_blocks_filament_op()` encodes the same rule: `paused && backend_self_homes`
(`filament_op_slot_resolver.h:156`).

> **Get the direction right.** An earlier draft of this plan had it inverted -
> "relaxes PAUSED when the backend self-homes". That is exactly the dangerous
> reading: it would permit filament ops in the one case where a hidden G28 can
> fire mid-print. Verified against the source 2026-08-19.

So this site needs **both** axes: the lifecycle *and* a backend capability. It is
the concrete proof that `job_holds_machine()` cannot be the only predicate -
callers that must distinguish `Paused` have to keep being able to. The named
predicates are conveniences over the lifecycle, never a replacement for switching
on it.

### Sites that must KEEP raw `PrintJobState` (24)

A blind sweep breaks these. Confirmed during the preparing-job work:

- **The 7 derivation/parse sites** - `print_lifecycle_state.cpp:33-49`,
  `printer_print_state.cpp:1345-1356`, `:278-287`, `:445-449`,
  `printer_state.cpp:57-77`, `:82-92`, `printer_print_state.cpp:71`.
- **Terminal-outcome formatting** - the 9 sites in `print_completion.cpp`
  (`:205-215`, `:260`, `:309-315`, `:329-335`, `:370-373`) and the 4 progress and
  layer freeze guards in `printer_print_state.cpp` (`:704`, `:813`, `:888`,
  `:944`).
- **Telemetry's terminal classification** - `telemetry_manager.cpp:3114-3120`,
  `:3163`. And `:3039` in particular: it resets the max pre-print phase on
  "PRINTING from non-PAUSED". On the raw state that is `STANDBY -> PRINTING`, one
  reset at the real start. On the lifecycle it becomes
  `Idle -> Preparing -> Printing`, so the reset would fire at `Preparing ->
  Printing` and **wipe the data the tracker exists to collect**.
- **Navigation's activation edge** - `print_start_navigation.cpp:27`. On a
  lifecycle including host-side `Preparing` it would open the status panel for a
  job the printer has not accepted, duplicating `PrintStartController`'s
  optimistic push.
- **Reconciliation** - `printer_print_state.cpp:1278` (`reconcile_preparing()`
  requires an actually-running print) and the collector arming/teardown
  predicates in `moonraker_manager.h:176-178`, `:211-213`, `:222`.

**Every one of these needs a comment saying why it is on the wire**, or the next
sweep "finishes the job" and regresses it.

---

## Phase tracker

Update this table in the same commit that completes a phase. `Commit` is the
completing SHA.

| Phase | Scope | Sites | State | Commit |
|---|---|---|---|---|
| **0a** | `print_in_progress` derived from the preparing job; watchdog for a job that never confirms | 20 setters | **done** | `289d56856` |
| **0b** | Panel adopts the live phase so it agrees with the authority by construction | 1 + tests | **done** | `41392dfd2` |
| **1a** | `job_holds_machine` predicate + subject; the 21 XML bindings moved onto it | 21 XML | **done** | `152986987` |
| **1b** | The guards (11 migrate, 2 stay raw) | 13 sites + 4 helper call sites + 3 observers | **done** | `32e516e14` |
| **1-fix** | Eight findings from the full-branch review; K2 verification | 8 chunks | **done** | `bebb803a5` |
| **2a** | E-Stop visibility: two observers collapse into one on the lifecycle | 1 + 6 tests | **done** | `e163e42db` |
| **2b** | `locked_while_printing` covers the preparing window (predicate AND observer) | 3 + 3 tests | **done** | `734f7f4d1` |
| **2c** | Typed accessors + `check_print_state_cast.py`; 24 hand-casts removed | 24 + gate | **done** | `eb3f94048` |
| **2d** | job queue modal, PLR offer, power panel, keep-raw markers (AMS panel stays raw) | 4 + 6 markers | **done** | `1c8677d4e` |
| **3a** | The home print card + its idle-runout gate (the K2 finding) | 5 + 6 tests | **done** | `b10f464a3` |
| **3b** | Every remaining raw site carries a verified reason | 26 markers | **done** | `858d36329` |
| **4a** | Typed observer factories; the gate hole they close | 6 + factory | **done** | `26880e4c3` |
| **4c** | Delete the helper; `check_raw_print_job_state.py` at zero | 1 + gate + 81 markers | **done** | `155abc76b` |
| **5** | Rename `PrintState` -> `PrintLifecycle` | mechanical | not started | - |

Total: **61 production edit points**, ~5 signature changes, ~30 test files.

---

### Phase 0 - make the preparing window have one owner

**On the current branch.** This is finishing the preparing-job work, not new
scope, and Phases 1-4 depend on the published subject being trustworthy.

Two jobs:

**0a. `print_in_progress` becomes derived.** `PrintPreparationManager` set it
true at two entry points and cleared it on **eighteen** exit paths; a missed path
left it stuck true and `can_start_new_print()` then refused every later print for
the rest of the session. It is now published from the preparing job -
`begin_preparing()` raises it, `retire_preparing()` lowers it whatever the exit
reason. All 20 manual calls deleted. The subject and accessor stay: readers and
XML depend on the boolean existing.

Two things found while doing it:

- **This closes a double-tap hole.** The old clear ran from a `wrapped_completion`
  that fired when the start RPC was *accepted*, which is before `print_stats`
  reports the job. In that gap `print_in_progress` was already false while the
  job state was still STANDBY, so `can_start_new_print()` returned true and a
  second tap could start another print. Deriving it holds the flag until the
  printer confirms.

- **The double-tap guard was in the wrong layer.** `start_print()` opened with
  *"reject if a print is already being started"*, reading the very flag the
  caller had just set - `PrintStartController::start_now()` arms the preparing
  job and then calls `start_print()` eleven lines later. Once the flag became
  derived, that guard rejected **every** print as a duplicate.
  `test_pre_start_timeout_gate.cpp:234` caught it. The guard is deleted: the
  controller already runs `can_start_new_print()` *before* arming, which is the
  correct place for it. Worth remembering as a general hazard of this refactor -
  **a guard that reads a flag its own caller sets is invisible until the flag
  changes owner.**

- **`PreparingExit::TimedOut` was dead.** It is handled everywhere - cooldown,
  notification, `decide_preparing_exit_action()` - but *nothing ever produced
  it*. That was survivable while the flag cleared on RPC-accept; once the flag is
  derived, a job the printer never acknowledges would latch it true forever.
  `begin_preparing()` now arms a one-shot watchdog (`PREPARING_WATCHDOG_MS`,
  1800s - above the K2's ~1140s worst case, matching `PrintStartCollector`'s own
  "definitely stuck" ceiling) and `retire_preparing()` disarms it. Cancelled in
  the destructor too, per CLAUDE.md threading rule 5. Test hook:
  `PrinterPrintStateTestAccess::fire_preparing_watchdog()`.

**0b. Make the panel ADOPT the published state instead of deriving its own.**

> **Scope corrected 2026-08-19.** This step was originally written as "delete the
> panel's private `PrintLifecycleState` and make the panel a reader". An
> investigation of what that class actually holds proved that wrong, and the
> deletion would have broken the Complete screen. Recorded here because the
> mistake is instructive: *"two state machines over the same inputs" was true of
> the enum and false of everything else in the object.*

`PrintLifecycleState` is not a duplicate state machine. It is a state machine
**plus a display-freeze latch store plus panel-local widget state**. Only the
enum is duplicated:

| Field | Status |
|---|---|
| `current_state_` | duplicated - this is the only thing `print_lifecycle` replaces |
| `elapsed_seconds_`, `remaining_seconds_`, `current_progress_`, `current_layer_`, `total_layers_` | **freeze latches, not mirrors.** Forced to their terminal values at Complete (`print_lifecycle_state.cpp:99-109`) and rejected once `outcome != NONE`. Moonraker zeroes the underlying subjects on STANDBY; these latches are why the Complete screen still reads `100% / 240/240 / 0s`. Deleting them deletes that. |
| `gcode_loaded_` | **no subject exists.** Written from four viewer-widget events (`:853`, `:1142`, `:1549`, `:1561`) no printer subject can see, and auto-cleared on the ->Idle edge. |
| `nozzle_*`, `bed_*`, `speed_percent_`, `flow_percent_` | pure unguarded mirrors - genuinely deletable |

`StateChangeResult` - seven booleans plus `old_state` - drives ~150 lines
(`:2696-2866`) including `runout_handler_->on_print_state_changed(old, new)`.

### The real defect: the two DO disagree, for the whole of PRINT_START

`PrintLifecycleState::on_job_state_changed()` hard-codes the phase
(`print_lifecycle_state.cpp:53-56`):

```cpp
PrintState new_state = derive_print_state(job_state, /*start_phase=*/0);
```

So when Moonraker reports `PRINTING` while a pre-print phase is still live - the
firmware-side case, the one `derive_print_state` exists for - the panel's copy
moves `Preparing -> Printing` while `print_lifecycle` correctly stays `Preparing`.
**They hold different states for the entire remainder of `PRINT_START`.**

### Revised scope: adopt, do not delete

The panel keeps the class, the latches, and `StateChangeResult`. What changes is
where the **enum** comes from: the panel stops deriving it and adopts the
published value. One input changes; the freeze semantics are untouched.

**Write the tests first.** There are currently **zero** panel-level tests
exercising `lifecycle_` - the 40 `[lifecycle]` cases all test the class in
isolation and would keep passing against a panel that had stopped using it. There
is no safety net at this seam, so it has to be built before the seam moves.

**Two behaviour changes to make deliberately, not by accident:**

- **Aborted preparation.** `on_start_phase_changed` returns a bare `bool` and
  produces no `StateChangeResult`, so `Preparing -> Idle` today runs none of the
  print-ended cleanup at `:2711-2751`. Adopting a published transition makes that
  a first-class edge.
- **In-`PRINT_START` display.** With the panel correctly staying `Preparing`, the
  gcode-load delay flips 500ms -> 5000ms (`:3717`) and the preprint observers keep
  owning the time display (`:1689`, `:3155`, `:3176`). That is the intended
  correction, but it is a change.

**Ordering hazard (needs a runtime check).** `observe_int_sync` snapshots the
value at notify time but runs the handler at drain time. Two lifecycle publishes
inside one `update_from_status` batch (e.g. `printer_print_state.cpp:436` then
`:466`) would collapse `print_lifecycle_prev`, so reconstructing `state_changed`
(`:3049`) from it is not safe without checking. Verify under
`HELIX_MOCK_AUTO_PRINT=1 --sim-speed 6 -vvv`.

Unused API found on the way: `get_state()` (`ui_panel_print_status.h:227`) and
`get_progress()` (`:247`) had no callers anywhere.

**`get_state()` is now the test seam** - `tests/unit/test_print_status_lifecycle_seam.cpp`
uses it to compare the panel's belief against the published subject, which is the
only way to observe the disagreement. Keep it. `get_progress()` is still unused;
delete it, or leave it and say why.

The seam tests drive the panel through its **real observer path** - subject
writes plus a queue drain - rather than calling the private handlers, so they
exercise the ordering the refactor has to preserve. Drive points:
`update_from_status()` for the job state, `set_print_start_state()` for the
phase.

**Exit criteria**
- [x] `set_print_in_progress()` has no callers outside `PrinterPrintState`.
- [x] `print_in_progress` is true for exactly the interval
      `begin_preparing()` -> `retire_preparing()`, proven by a test per exit
      reason (Confirmed, Superseded, Failed, Cancelled, TimedOut).
- [x] A preparing job that never confirms is retired as `TimedOut` rather than
      latching the flag.
- [x] Panel-level tests exist for `lifecycle_` before the seam moves - was zero,
      now `tests/unit/test_print_status_lifecycle_seam.cpp`. **(0b)**
- [x] `PrintLifecycleState::on_job_state_changed()` no longer derives its own
      enum with a hard-coded `start_phase=0`. **(0b)**
- [x] The Complete-screen freeze still holds. **(0b)**
- [x] Full suite green (95/95 shards).

**Watch for:** a stuck-true `print_in_progress` is the failure mode being fixed,
so the test must assert the *false* edge on every exit reason, not just the happy
path.

---

### Phase 1 - safety guards, and the XML that matters

Highest value: closes the live hazard where 21 motion controls are enabled while
the toolhead moves during pre-print.

**1a. Add the lifecycle-derived subject** the XML needs. `print_active` cannot
gain `Preparing` without changing meaning for its C++ readers, so publish a new
boolean (working name `job_holds_machine`) from the lifecycle and move the 21
bindings onto it. Both subjects coexist until Phase 3 retires `print_active`.

XML sites: the 21 enumerated in "The safety gap" above. Note
`components/panel_widget_bypass.xml:15` is one of them, so Phase 1 fixes the
bypass tile's affordance and `ui_bypass_toggle_controller.cpp:28` fixes its
handler - both are needed, neither is sufficient alone.

**1b. The guards.** Surveyed site by site 2026-08-19; the paths and line
numbers in the first draft of this table were mostly wrong and three of the
sites turned out not to be `PRINTING || PAUSED` at all. Corrected list:

| Site | Gates | Action |
|---|---|---|
| `src/api/moonraker_gcode_guards.cpp:20` | Layer-1 refusal of app-emitted homing | **KEEP RAW** - see below |
| `src/printer/ams_subscription_backend.cpp:318,321` | AMS filament-op refusal | via `print_blocks_filament_op` |
| `src/ui/ui_bypass_toggle_controller.cpp:28` | Bypass toggle | `job_holds_machine` |
| `src/printer/printer_state.cpp:968` | `is_blocking_operation_active()` | **KEEP RAW** - see below |
| `src/ui/panel_widgets/tool_switcher_widget.cpp:588` | Tool-change refusal | via `print_blocks_filament_op` |
| `src/ui/panel_widgets/tool_switcher_widget.cpp:670` | Paused confirmation modal | `lifecycle == Paused` |
| `src/printer/ams_backend_ad5x_ifs.cpp:56` | AD5X runout gate - **PAUSED-only, not a "holds machine" question** | `lifecycle == Paused` |
| `src/system/post_op_cooldown_manager.cpp:73` | Post-op nozzle cooldown | `job_holds_machine` |
| `src/ui/ui_panel_filament.cpp:2599` | Cooldown scheduling after a filament op | `job_holds_machine` |
| `src/print/filament_sensor_manager.cpp:994` | Head-sensor-empty noise suppression | `job_holds_machine` |
| `src/system/update_checker.cpp:1245,2891` | Update download + notification | `job_holds_machine` |
| `src/system/upgrade_nudge.cpp:83` | Upgrade nudge - **PRINTING-only, no PAUSED arm** | `job_holds_machine` |
| `src/application/display_manager.cpp:1021` | Display sleep inhibit | `job_holds_machine` |

Three follow-on edits the survey turned up that are not in the census:

- `print_blocks_filament_op(bool printing, bool paused, bool self_homes)`
  (`include/filament_op_slot_resolver.h:156`) becomes
  `(PrintState lifecycle, bool self_homes)`. Four call sites, three of them
  outside the 15: `ui_ams_sidebar.cpp:1059`, `ui_ams_context_menu.cpp:218`,
  `ui_panel_filament.cpp:1876`.
- Two observers still watch `print_state_enum` and would not fire on the
  `Idle -> Preparing` edge, so the affordance would not grey even with the guard
  fixed: `tool_switcher_widget.cpp:125`, `ui_panel_filament.cpp:227`.

#### Two sites that must NOT be widened

**`moonraker_gcode_guards.cpp` - widening it breaks print start.**
`PrintPreparationManager` sends the user's configured pre-start block through
`api_->execute_gcode()` *inside* the preparing window
(`ui_print_preparation_manager.cpp:732`), and `is_homing_gcode()` matches any
line whose first token is `G28` (`include/gcode_homing.h:45`). On the K2 that
block is the forced bed mesh. Widening the guard to `job_holds_machine()` makes
it refuse the app's own pre-start G-code on every printer whose pre-start block
homes. This is the Phase 0a hazard exactly - **a guard that reads a flag its own
caller sets** - and it is the second time this refactor has produced one.

The window is not left unguarded: during a *firmware-side* `PRINT_START` the job
state is already `PRINTING`, so the guard fires. During a *host-side* block the
app is the only thing driving the toolhead, and the affordances that could send
a competing `G28` (all 21 XML bindings, the AMS ops, the bypass tile) are
disabled by the rest of Phase 1.

**`is_blocking_operation_active()` - it is inverted, and already correct.**
It returns true when `idle_timeout` says busy AND that busy-ness is *not*
explained by a print. During a host-side pre-print block `idle_timeout` reads
`Printing` (the host is running G-code) while `print_stats` reads `standby`, so
today the function correctly answers "blocked". Swapping the raw state for
`job_holds_machine()` would make it answer "not blocked" and *admit* jogs during
the bed mesh - the opposite of this phase's purpose. Leave it, and say why.

That is 25 keep-raw sites now, not 24.

**Exit criteria**
- [ ] All 21 XML bindings reference the lifecycle-derived subject.
- [ ] Motion/jog/extrude controls verified disabled during a host-side preparing
      window - by widget **state**, via `ctl ls`, not by `ctl click`
      (`ctl click` bypasses the indev layer and fires handlers on disabled
      widgets, so it cannot prove an affordance).
- [ ] Each guard has a test that fails if it is reverted to the raw state.
- [ ] Full suite green.

---

### Phase 2 - affordance and navigation

> **Re-surveyed 2026-08-19 against the post-merge tree. Three claims below were
> wrong and are corrected here; the original text follows for context.**
>
> | Site | Verdict |
> |---|---|
> | `print_control_buttons.cpp` | **Already migrated.** Its two raw reads are correct BY DESIGN - `printer_has_the_job` must exclude Preparing, or Pause re-enables and Stop sends `CANCEL_PRINT` to a printer holding no job. Wants a `RAW_PRINT_STATE_OK` marker, not a change. |
> | `ui_panel_power.cpp:233` | Move, but note it has **no observer at all** - the lock is snapshotted into `DeviceRow::locked` at row build. Migrating the read alone changes nothing live. |
> | `power_device_state.cpp:156,211,236` | **Highest value.** Also **re-point the observer at `:88`** from `print_state_enum` to `print_lifecycle`, or the new predicate never evaluates during a host-side block. (That observer also carries no lifetime token - separate issue.) |
> | `ui_emergency_stop.cpp:284-287` | **Lowest risk**, behaviour-neutral: the hand-OR is already `job_holds_machine` spelled out. Lets **two** ObserverGuards (`:188` enum + `:197` phase) collapse into one on the lifecycle. |
> | `ui_job_queue_modal.cpp:388` | Real bug: the guard reads the wire, so a queue tap during a host-side block deletes the entry and then fails the start. Use `can_start_new_print()`, which also covers the `print_in_progress` axis. |
> | `ui_panel_ams.cpp:268` | **The plan was wrong to list this as moving to an "active" predicate.** The comment at `:246-251` requires an edge into PRINTING *specifically* - a fault pauses the print, so `PAUSED -> PRINTING` IS the signal (#1185). **Settled 2026-08-19: it stays RAW.** Even `== PrintState::Printing` is wrong, because `print_lifecycle` holds `Preparing` for the whole of a firmware-side `PRINT_START` - so reading it would move the dismissal from the START of `PRINT_START` to its END, silently. The trigger asks a *value* question about what the printer reports, which is what `RAW_PRINT_STATE_OK` is for. Pinned by a keep-raw case in `tests/unit/test_ams_error_modal_autodismiss.cpp` that goes red if the observer is migrated. |
> | `ui_plr_offer_controller.cpp:86` | Move. Offering power-loss recovery on top of an in-flight start is a modal ambush. (`:161` is keep-raw - it mirrors a Klipper condition on `standby`.) |
> | `print_start_navigation.cpp` | **Keep raw - re-verified.** The duplication is real, but the optimistic push lives in `ui_panel_print_select.cpp:2595,2907`, not `PrintStartController` as this plan said. |
> | `printer_print_state.cpp:1240-1252` | **No longer a Phase 2 site.** `can_start_new_print()`'s `is_print_in_progress()` early-return already covers the whole Preparing window. A migration there is cosmetic. |
>
> **The exit criterion below is unachievable as written.** "`is_active_print_state()`
> deleted or re-typed" cannot happen: `ui_plr_offer_controller` must move and
> `print_start_navigation` must not, and they share the helper. Split them - leave
> the helper for navigation, and have the PLR controller read the lifecycle direct.
>
> **Testing gap:** sites 2, 3, 4, 5 and 7 have NO test asserting Preparing-window
> behaviour. `power_device_state` has no coverage of `effective == 2` at all.


12 affordance sites (`print_control_view.cpp` - already partly done -
`print_control_buttons.cpp:128,146`, `ui_ams_context_menu.cpp:219`,
`ui_ams_sidebar.cpp:1060`, `ui_panel_filament.cpp:1876`, `ui_panel_power.cpp:233`,
`power_device_state.cpp:156,211,236`, `ui_emergency_stop.cpp:284`,
`ui_job_queue_modal.cpp:388`, `printer_print_state.cpp:1239-1245`) and 6
navigation sites.

`ui_emergency_stop.cpp:284-288` already hand-ORs the pre-print phase - replace
that with the predicate rather than leaving a second spelling.

**Navigation needs care:** `print_start_navigation.cpp:27` is one of the 24 sites
that must stay on the wire. Only `ui_plr_offer_controller.cpp:86` and
`ui_panel_ams.cpp:268` move.

**Exit criteria**
- [x] No hand-rolled `|| start_phase != 0` compositions remain. (2a collapsed the
      E-Stop pair; nothing else composes the phase by hand.)
- [x] ~~`is_active_print_state()` deleted or re-typed to `PrintState`.~~ Replaced,
      as the correction box above says it had to be: the helper is now **private to
      navigation** - after 2d its only callers are inside
      `print_start_navigation.cpp` itself, and it carries a `RAW_PRINT_STATE_OK`
      saying why navigation must not see `Preparing`.
- [x] Every keep-raw site reached in Phase 2 carries a `RAW_PRINT_STATE_OK`
      marker: `print_control_view` (x2), `print_start_navigation`,
      `can_start_new_print`, `ui_panel_ams`, `ui_plr_offer_controller`.
- [x] Full suite green.

---

### Phase 3 - display and bookkeeping

> **The premise of this phase was wrong, and the census is the correction.**
> It was written as "11 display sites and 15 bookkeeping sites" to migrate. Read
> site by site on 2026-08-19, **exactly one file needed migrating** - the home
> print-status widget - and it held two real defects. Every other remaining site
> is legitimately on the wire, several for reasons the plan did not anticipate:
>
> | Site | Why it stays raw - none of these were predicted |
> |---|---|
> | `ui_panel_print_select.cpp:2382` | Reads `print_filename`, which `reset_for_new_print()` **deliberately does not clear**, so during a preparing window it still holds the PREVIOUS job. Widening it badges the wrong file rather than none. Badging the committed file means reading `preparing_job()` - a feature, not a migration. |
> | `led_auto_state.cpp:128` | The returned strings are **LED theme keys** in the JSON themes. There is no `preparing` key, and a pre-print block already falls through to `heating`, which is what the machine is doing. |
> | `ams_state.cpp:2547` | Wants a `Preparing` label, but **no such translation key exists** and this is the lowest-priority fallback in the chain. Deferred rather than adding a key for a line the AmsAction string usually wins. |
> | `ams_state.cpp:1474` | Arms a runout EDGE that must be witnessed while material moves. Preparing would arm it for a latch raised before anything moved. |
> | `ui_panel_print_status.cpp:1829` | Scopes the runout badge to the running file's tools; during Preparing `get_tools_used()` still describes the previous job. |
> | `gcode_error_router.cpp:364` | Feeds resume/retry affordances. A failure inside a host-side block is `PrintPreparationManager`'s to report. |
>
> The lesson is the one this plan keeps re-learning: **a census taken from a grep
> is a list of candidates, not a list of sites.** Two earlier corrections in this
> document say the same thing about Phase 1's and Phase 2's tables.

**3a - the home print card.** `PrintStatusWidget` observed `print_state_enum`, so
`is_active_` (which picks the `view_subject_` variant) read idle for the whole of
a host-side pre-print block - the defect seen on the K2 and recorded below under
"found on the K2, not fixed". Tapping the card went to the file browser instead
of the status overlay. The same wire read also let the **idle runout modal** fire
on top of a committed start, burning the one-shot post-unload grace on the way
past. Both now ask the lifecycle.

Also added: `observe_print_lifecycle()` beside `observe_print_state()` in
`observer_factory.h`. The existing factory hard-casts to `PrintJobState` while
accepting any subject, which is trap #5 in reusable form - handing it
`print_lifecycle` compiles and answers a different question. Both now name the
subject they are for.

**3b - reasons, not just markers.** Every file that still names
`PrintJobState::PRINTING` or `PAUSED` carries a `RAW_PRINT_STATE_OK` explaining
why, written from reading the site. Derivation functions carry one whole-function
note rather than per-arm markers.

**`print_active` is NOT deleted, and should not be.** It has **zero XML bindings**
(Phase 1a moved all 21) and exactly two C++ readers, both inside
`PrinterPrintState`, and both want wire semantics on purpose:
`update_print_show_progress()` (`:973`) and the stale-phase guard at `:1099`,
whose own comment says *"print_active is legitimately 0 for the whole host-side
pre-start block"*. Deleting it would mean re-deriving the same value at both
sites. Documented as wire-semantics-on-purpose, which is what the exit criterion
allowed for.

**Exit criteria**
- [x] `print_active` deleted, or documented as wire-semantics-on-purpose.
- [x] Every remaining raw-state site carries a comment saying why.
- [x] Full suite green.

---

### Phase 4 - delete the helper, add the gate

Delete `print_occupies_toolhead(PrintJobState)`.

Add `scripts/check_raw_print_job_state.py` on the established pattern: regex over
`PrintJobState::(PRINTING|PAUSED|...)`, `get_print_job_state()`, and
`lv_subject_get_int(...get_print_state_enum_subject())` - the last is the back
door a `get_print_job_state()` deprecation would miss. Per-line opt-out
`// RAW_PRINT_STATE_OK: <reason>`, file allowlist for the derivation sites,
`--max-allowed N` ratchet wired in `scripts/quality-checks.sh`.

Cost: ~250-350 lines of Python, ~20 lines in `quality-checks.sh`, ~11 bats
meta-tests on the `tests/shell/test_l081_gate.bats` pattern. **No changes needed
to the pre-commit hook or GitHub Actions** - both run `quality-checks.sh`
wholesale.

**4a first, because the gate could not have held without it.** Twenty sites
subscribed to a print-state subject in six different spellings, and twelve of
them cast the observer lambda's `int` by hand - with the `get_*_subject()` call
that decides which enum is correct sitting up to twelve lines away.
`check_print_state_cast.py` could not see any of them: it only matched
`static_cast<Enum>(lv_subject_get_int(..))`. So it reported a clean zero while
seven sites did exactly what it existed to forbid. Six moved onto
`observe_print_state()` / `get_print_lifecycle()`; the pattern now also matches
the plain-int form.

`observe_print_state_immediate()` exists because **typing and dispatch mode are
orthogonal**, and a factory family covering only one dispatch mode is a trap:
swapping `observe_int_immediate()` for the typed factory silently deferred
AbortManager's cancel detection onto the UpdateQueue. Two tests caught it. Worth
remembering - "same subject, same handler, better typing" is not automatically
behaviour-neutral.

**Considered and deliberately NOT done: an `EdgeTracker<T>`.** Six sites track a
previous print state with three different first-tick conventions, which looked
like textbook drift. It is not: **four of the six are correct, and two of those
encode requirements a generic tracker cannot express** - `print_start_navigation`
re-seeds *and* does a level check because a restored power-loss job must still
navigate (#1099); `print_collector_arming` seeds STANDBY *and* carries
`is_initial_transition()` to tell "joined mid-print" from "user reprint". The
other two (`telemetry_manager`, `ams_backend_ad5x_ifs`) can false-edge on the
first tick, and both consequences are benign. A shared tracker would have sat
*underneath* the four correct sites without removing their flags - a seventh
mechanism, not a consolidation. The knowledge was kept instead: each of those
sites now carries a FIRST-TICK note in its marker.

**Two markers written in Phase 3b were invisible to the gate** - `RAW_PRINT_STATE_OK,`
and `RAW_PRINT_STATE_OK (whole function):` rather than the literal
`RAW_PRINT_STATE_OK:`. They read as annotated and were not. All 81 markers are
now the same token, which is the only reason the zero is trustworthy. If the
opt-out ever gains variants, the gate must be taught them in the same commit.

**Exit criteria**
- [x] Gate fails on a newly introduced raw comparison outside the allowlist.
      Proven twice, not assumed: it fails on a canary file, and on the
      pre-refactor version of `spoolman_manager.cpp` retrieved from git.
- [x] Baseline set to the surviving count and recorded here: **0**, over 81
      annotated sites in 30 files. Wired into `scripts/quality-checks.sh`, so
      the pre-commit and pre-push hooks both run it.
- [x] `print_occupies_toolhead()` deleted (zero callers since `32e516e14`).

**Not done, deliberately:** the ~11 bats meta-tests this phase originally
specified. The property that matters - the gate can fail - was proven directly
by the two canaries above. The bats suite is polish; write it if the gate ever
grows conditional logic worth pinning.

---

### Phase 5 - rename

> **Demoted 2026-08-19, and no longer a safety item.** This phase existed largely
> because `PrintJobState` and `PrintState` are confusable, and the confusion had
> teeth: the two enums do NOT share numbering past index 0, so
> `static_cast<PrintState>(lv_subject_get_int(<the WRONG subject>))` compiles,
> runs, and answers a different question. That mistake shipped into two commits on
> this work before tests caught it.
>
> `eb3f94048` fixed it properly instead: `get_print_lifecycle()` now sits beside
> `get_print_job_state()`, each owning its subject/enum pairing, all 24 hand-casts
> are gone, and `check_print_state_cast.py` holds the count at zero. A rename would
> have made the mistake *read* wrong; removing the cast makes it unsayable.
>
> The rename is still worth doing for clarity. It is no longer urgent.


`PrintState` -> `PrintLifecycle`, mechanically, once call sites are stable. Last
because it touches everything and must not be interleaved with behaviour changes.

---

## Risks

- **Every phase changes behaviour at every site it touches, by design** - the
  sites start seeing `Preparing`. Each phase needs its own test pass; one green
  run at the end proves nothing about which phase broke what.
- **The 24 keep-raw sites are load-bearing.** Telemetry and navigation were both
  caught mid-refactor during the preparing-job work; they look identical to the
  sites that should move.
- **`upgrade_nudge.cpp:82` is PRINTING-only** while its neighbours are
  `PRINTING || PAUSED`. **Decided 2026-08-19: normalise it, deliberately.** Its
  own comment says *"Don't nudge mid-print - the printer is the priority, not our
  prompts"*, and a paused print is mid-print; the neighbouring guard in
  `update_checker.cpp:1246` refuses update downloads on `PRINTING || PAUSED` for
  the same reason. The narrow spelling reads as an oversight, not a design. Phase
  1 moves it to `job_holds_machine()`, which also suppresses the nudge during
  `Preparing` - a user who just committed to a print is the last person who wants
  an upgrade prompt. This makes the nudge strictly rarer, which is the safe
  direction, but it IS a behaviour change and the commit must say so.
- **Test count.** ~30 test files touch `PrintJobState`; three are pure
  predicate-definition tests that get rewritten outright
  (`test_print_start_navigation.cpp`, `test_print_active.cpp`,
  `test_print_control_view.cpp`).

## Not verified on hardware

Neither is a correctness gap - both are behaviour that has only ever been
exercised by unit tests. Recorded here because they are the difference between
"green" and "known to work on a printer", and they outlive any one session.

- **The Cancel/Pause affordance change.** The K2 hardware run predates
  `34cd93e93`, so the change that enables Cancel during a host-side pre-print and
  disables Pause during a firmware-side one has never been touched by a finger.
  Verify by reading the widget's `disabled` state flag (`ctl ls`), **not** by
  `ctl click` - that bypasses the indev layer and fires handlers on disabled
  widgets, which is how a previous "verification" of this exact path was wrong.
- **CB1/Voron, the no-pre-start-block case.** The 750ms debounce is supposed to
  stop an overlay flash on a printer whose preparing window is sub-second. Never
  run there.

## Found on the K2 during Phase 1 verification, not fixed

Both are real, both were seen on hardware, neither blocked the merge.

- **Cancelling during a host-side pre-start block does not abort the G-code
  already sent.** The app retires its claim correctly and the UI moves on, but
  the printer keeps executing the block: the mesh finishes, and the app's own
  `TURN_OFF_HEATERS` queues *behind* it (observed as two
  `printer.gcode.script` RPCs timing out at 60s, with the bed still commanded to
  105C for four minutes after the cancel). Inherent - the app cannot retract a
  block it has handed over - but this branch is what makes Cancel-during-preparing
  reachable, so it is newly exposed. A real fix means emitting an abort for the
  block, which is its own design question.

- ~~**The home print-status widget shows `print_card_idle` during Preparing.**~~
  **Fixed in Phase 3a (`b10f464a3`).** The guess was right - same defect class,
  same wire read. Fixing it turned up a second one in the same file: the idle
  runout modal could fire on top of a committed start, and burned the one-shot
  post-unload grace on its way past.

## Found while doing Phase 2d - FIXED on main by `13db7c92e` / `faadde444`

**`PowerPanel::populate_device_chips()` defers a rebuild that reads
`chip_container_` raw.** `lifetime_.defer` guards against `this` dying; nothing
nulls `chip_container_` when the panel's widget tree is deleted, and the
deferred body's `if (chip_container_)` then passes on a dangling pointer. Found
because the Phase 2d power-panel test segfaulted in
`create_led_chip -> lv_obj_add_style -> lv_obj_get_screen` when it drained the
queue after deleting the tree. The test now drains first and says why. Unrelated
to print state, so it was handed to a parallel session rather than widening this
branch; `13db7c92e` makes PowerPanel drop its cached widget pointers when the
tree dies, and `faadde444` moves the row deletion out of the UpdateQueue batch.
Both are on main and merged in here.

## Out of scope, tracked separately

**Klippy readiness** is the same defect at larger scale - 3 parse sites, 7
disagreeing predicates, 18 raw consumers across 13 layers, and the only model
that handles "the app knows something the wire doesn't" (`expected_restart`
suppressing a transient SHUTDOWN) is trapped inside
`PrinterStatusIcon::compute_state()`. Consequence: `MoonrakerAPI::execute_gcode`
tells the user *"Klipper is halted - restart firmware to continue"* during a
`SAVE_CONFIG` restart the app itself initiated. Own branch, after this one.

Also found, not scheduled: `MoonrakerMotionAPI::execute_gcode` gates on klippy
alone with no connection term (`moonraker_motion_api.cpp:416`), 200 lines from
the gate fixed to consult both (bundle XRK8KPTF); `is_printer_ready()` is dead
code doing an RPC round trip for data already in a subject; temperature
"ready to proceed" has 6 implementations with 3 thresholds; AMS "is filament
loaded" has two definitions in one file that disagree exactly where
`ams_backend_snapmaker.cpp:1591-1598` works to distinguish them.

## How to resume

1. Read the phase tracker. Find the first phase not marked done.
2. Re-read that phase's exit criteria - they are the definition of done.
3. `git log --oneline` on this branch; the plan's `Commit` column should match.
4. If a phase is half-finished, the surviving raw-state count is the progress
   metric: `grep -rn "PrintJobState::\(PRINTING\|PAUSED\)" src/ include/ | wc -l`.
   Record it here when you stop.

**Progress metric log** (append on every stop):

| Date | Phase | Raw-state sites remaining | Note |
|---|---|---|---|
| 2026-08-19 | 0 | 91 | Plan written. Baseline for the resume command below. |
| 2026-08-19 | 0a | 91 | `289d56856`. Phase 0a touches the preparing window, not the raw-state count, so the metric is unchanged by design. Suite 95/95. |
| 2026-08-19 | 0b | 91 | `41392dfd2`. Also count-neutral - 0b changes which inputs an existing derivation gets, not how many sites read the wire. Suite 95/95. **Phase 0 complete.** |
| 2026-08-19 | - | 91 | `d606bd823`. Re-merged main (10 commits, no conflicts). Suite 95/95, and the **full ungated** quality sweep passes (36 gates) - worth re-running before any push, because per-commit gates only ever run `--staged-only` and skip anything you did not stage. |
| 2026-08-19 | 4 | 63 | `26880e4c3` + `155abc76b`. **Phase 4 complete; only the cosmetic Phase 5 remains.** Gate at zero over 81 annotated sites, wired into `quality-checks.sh`. `print_occupies_toolhead()` deleted. The count does not move because Phase 4 was never about removing wire reads - it was about making the surviving ones say why, and making the enum/subject mismatch unsayable. Suite 96/96. |
| 2026-08-19 | 3 | 63 | `b10f464a3` + `858d36329`. **Phase 3 complete**, but not as written - see the correction box in Phase 3. One file migrated (the home print card + its idle-runout gate, both real, both K2-relevant); everything else was verified keep-raw and now says why. `observe_print_lifecycle()` added beside `observe_print_state()`: the old factory hard-cast to PrintJobState while accepting any subject, which is the enum-mismatch trap in reusable form. `print_active` documented rather than deleted - zero XML bindings, two internal readers that both want wire semantics. |
| 2026-08-19 | 2d | 66 | `1c8677d4e`. **Phase 2 complete.** Three real defects, each written test-first and red before the fix: a queue tap during a pre-print block deleted the job then failed the start; PLR offered recovery on top of a committed start; a `locked_while_printing` PSU stayed togglable while the toolhead homed. `ui_panel_ams` was investigated and deliberately NOT migrated - see the correction box in Phase 2. Six `RAW_PRINT_STATE_OK` markers added. Suite green; the AMS keep-raw pin was mutation-verified. |
| 2026-08-19 | 2c | 68 | `eb3f94048` on `fix/print-state-phase2`. Phase 2a-2c done. Remaining 2d: `ui_job_queue_modal` (real bug - a queue tap during a pre-print block deletes the entry, then the start fails; use `can_start_new_print()`), `ui_plr_offer_controller:86`, `ui_panel_ams:268` (**value comparison only** - see the correction box in Phase 2), `ui_panel_power:233` (near-no-op; the live path is `power_device_state`), and RAW_PRINT_STATE_OK markers on `print_control_buttons`, `print_start_navigation`, `can_start_new_print`. |
| 2026-08-19 | merged | 72 | `776a6afe1` on main. Phase 1 verified on the K2: job_holds_machine=1 while print_active=0 during a host-side block, controls flip enabled->disabled, Pause refused / Cancel offered, cancel reports as cancelled with no spurious completion, no latch, collector stopped, second start works. A full-branch review first found 1 Blocker + 1 Blocker-coverage + 6 Major, all fixed in 8 chunks - see "What the review caught" below. |
| 2026-08-19 | 1b | 72 | `32e516e14`. 91 -> 72: the first real drop. `print_occupies_toolhead()` is now **zero-caller** (Phase 4 deletes it). Cost the census did not predict: ~90 assertions across 7 test files failed because every fixture drove `print_state_enum` directly - now routed through `tests/test_helpers/print_state_test_drivers.h`. 96/96 shards, ungated sweep green. |
| 2026-08-19 | 1a | 91 | `152986987`. Subject + 21 XML bindings + 13 tests. Count-neutral by design: 1a adds a derived subject and moves XML, it does not remove a C++ wire read. 96/96 shards, ungated sweep green. |
| 2026-08-19 | - | 91 | `6945d4e98`. Re-merged main again (12 commits, no conflicts, translations auto-merged). Suite 95/95, ungated sweep green. Site counts re-verified unchanged: 21 bindings, 91 raw-state sites. |

## Before you touch anything: state of the branch

**Phase 0 + 1 are MERGED to main** (`776a6afe1`) and verified on the K2. **Phases 2, 3
and 4 are COMPLETE** on branch `fix/print-state-phase2`, in the same worktree
(`.worktrees/preprint-arm-on-initiation`) - the old branch was deleted after the
merge and the worktree reused, so the build cache is warm. Not yet merged.

Raw-state metric: **63** (was 91 at the start of Phase 1), and every one of those
now carries a `RAW_PRINT_STATE_OK` reason.

Next up is **Phase 3** (display + bookkeeping, 11 + 15 sites; retire
`print_active`), then **Phase 4** (the ratcheting gate - the helper deletion it
also lists is already achieved, `print_occupies_toolhead` has had zero callers
since `32e516e14`).

Everything below this line predates the merge and is kept for the reasoning, not
as current instructions.


Branch `fix/preparing-job-lifecycle`, worktree
`.worktrees/preprint-arm-on-initiation`. Green at three layers as of
`6945d4e98`: build, suite 95/95, and the full ungated quality sweep.

**Phase 0 is not separately mergeable.** `begin_preparing`, `retire_preparing`,
`PreparingExit` and `derive_print_state` do not exist on main - they are all from
the preparing-job work earlier on this branch. Neither 0a nor 0b can be
cherry-picked. The mergeable unit is the whole branch.

**Main moves fast; re-merging is a standing step, not a one-off.** Three merges
in one day, and the last batch landed work adjacent to Phase 1. Re-merge and
re-run before assuming any site list here is current, then re-check the two
counts in the progress log - they are cheap and they are the tripwire.

**Already checked, do not redo:** `90089b714 fix(ams): the bypass subject reports
what the toggle acts on` repointed `ams_bypass_active` at the backend's
`is_bypass_active()`. That is about *which bypass value* the subject reports and
is orthogonal to the Phase 1 finding. `ui_bypass_toggle_controller.cpp:28` still
reads `print_occupies_toolhead(PrintJobState)` and still cannot see the preparing
window.

**Landing checklist.** The pre-push hook now runs the **full ungated** sweep
(`scripts/quality-checks.sh`), while every commit only ran `--staged-only`, which
skips gates that inspect nothing you staged. Run the full sweep yourself before
pushing rather than discovering it at push time. Note also that a green
pre-commit hook is **not** evidence the generated artifacts are in sync - only
`make test-run` catches a stale `theme_token_table.cpp` after a `ui_xml` change.

**Build cost.** Touching `printer_print_state.h` or `print_lifecycle_state.h`
rebuilds essentially everything; that was 40+ minutes on a contended box. Batch a
phase's changes into one build instead of iterating, and check `uptime` before
assuming a slow build is stuck - this machine runs many parallel sessions.


### Testing this area: three artifacts that impersonate bugs

Every one of these produced a failure indistinguishable from the defect being
hunted. Budget for them.

1. **`ctl click` fires handlers on DISABLED widgets.** It calls
   `lv_obj_send_event()` directly, bypassing the indev layer, so it proves the
   handler runs and says nothing about whether a finger can reach it. Check the
   widget's `disabled` state flag instead.
2. **A dead fixture's `PrinterState` leaves subject names resolving to freed
   storage**, and the damage lands in an unrelated destructor long after your own
   assertions pass. Fixed upstream by `c7cc96670`, but the shape recurs.
3. **One `UpdateQueue::drain()` is not enough.** The panel's observers are
   `observe_int_sync`; a handler running during a drain queues more work that is
   still pending when `drain()` returns, leaving the panel exactly one transition
   behind. Drain until quiescent.

4. **A test that writes `print_state_enum` by hand stops driving anything once
   the consumer moves to `print_lifecycle`.** The subject still changes, the
   observer is on a different subject, and the panel simply never re-gates - so
   the assertion fails as if the production guard were missing. Verified during
   Phase 1b: production has exactly ONE writer of `print_state_enum_`
   (`printer_print_state.cpp:436`) and `publish_lifecycle_state()` is the next
   statement, so the two cannot desync outside a test. Drive
   `update_from_status()` instead, and raise the phase with
   `set_print_start_state()` when you want `Preparing`. Expect this in every
   later phase: it is the single most likely way a green suite turns red for a
   reason that is not a bug.

5. **`PrintJobState` and `PrintState` do NOT share numbering, and a wrong cast
   is silent.** `PrintJobState` is STANDBY=0, PRINTING=1, PAUSED=2, COMPLETE=3,
   CANCELLED=4, ERROR=5. `PrintState` is Idle=0, Preparing=1, Printing=2,
   Paused=3, Complete=4, Cancelled=5, Error=6 - offset by one from PRINTING on.
   Changing a comparison to `PrintState::X` while leaving the read on
   `get_print_state_enum_subject()` compiles, runs, and answers a *different
   question*: it was hit in Phase 1b on `ams_backend_ad5x_ifs.cpp`, where
   `print_is_paused()` would have returned true for a COMPLETE job and false for
   a real pause, inverting the runout detector. **Whenever you change the cast,
   change the subject in the same edit**, and grep
   `get_print_state_enum_subject` afterwards for readers whose comparison type no
   longer matches. This is the single strongest argument for Phase 5's rename.

**The discriminator is whether the failure moves when you change production
code.** In 0b the firmware-side assertion moved and the other two did not - that
was the signal, and it was available two wrong hypotheses before it was used.

**Note on units.** The census counts **88 distinct decision sites**; the resume
command counts **91 matching lines**. They are different measures and both are
right - a `switch` arm and a two-line condition are one site but two lines. Track
the grep number here, because that is the one a future session can reproduce in a
second without redoing the census.
