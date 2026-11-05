# MedusaHC (Tool-Changer Add-On)

MedusaHC is a budget **hotend changer**: it swaps only the hot end (heater, thermistor,
fan), not a whole toolhead. It is not its own `AmsType` and has no backend of its own:
`AmsBackendToolChanger` drives it, and everything MedusaHC adds on top lives in one module.

- Hardware, config and controller: [Irbis3D/MedusaHC](https://github.com/Irbis3D/MedusaHC).
  The controller is the Klipper extra medusahc.py there, beside the dock-sensor extra
  pin_watch.py; the old macro controller is frozen on the repo's `legacy-macros` branch.

## Why it is not a backend

A stock install is `[medusahc]` + `[pin_watch io]` with `T<n>` macros running `SET`, and
no `[toolchanger]`. klipper-toolchanger is optional: pin_watch's `sync_toolchanger`
defaults to 0, and the `[toolchanger]` settings (`verify_tool_pickup: False` and the rest)
exist only in the README's optional minimal config, for other plugins that want the active
tool mirrored. Either way the machine is what `AmsBackendToolChanger` already models: a
parallel set of tools, one of them on the carriage. Adding a ninth `AmsType` would have
forked a working backend for no gain.

What a hotend changer genuinely adds is what klipper-toolchanger cannot answer, and that is
the whole content of `include/toolchanger_addon.h` + `src/printer/toolchanger_addon.cpp`.

## The add-ons

### 1. Dock sensors are the authority on which tool is mounted

Where klipper-toolchanger runs beside the controller, its minimal config sets
`verify_tool_pickup: False` and `require_tool_present: False`, so it never verifies a
pickup: `toolchanger.tool_number` is only what pin_watch last mirrored into it, and after a
failed or partial pickup it can name a tool that is not on the head.

The physical answer comes off dock sensors through MedusaHC's own pin_watch Klipper extra
(its pin_watch.py in the same repository). It is not a stock Klipper module - it is absent from all
136 files in Klipper3d/klipper's klippy/extras - so the object name is a real signature.
The controller agrees: its `_current_tool()` returns `pin_watch.current_tool`.

So once a dock sensor is present, `parse_toolchanger_state()` never takes the carriage from
`toolchanger.tool_number` at all: Moonraker sends it in deltas of its own, so it cannot be
outranked only within one frame. `apply_tool_sensor_locked()` alone sets `current_tool` and
`current_slot`. A dock reading is a **physical** dock index, so it maps to the slot of the same
index, never through the G-code tool map that `ASSIGN_TOOL` writes, and a reading past the
discovered tools is ignored (the last known tool stands). `toolchanger.status: ready` does not
clear a dock-sensor fault either: klipper-toolchanger knows nothing of the docks.

While the fault is latched, a tool change and an unmount are refused with "Dock sensors cannot
tell which tool is mounted": a swap plans its moves from what is on the carriage.

`current_tool == -2` is a third state, distinct from "no tool": the sensors cannot tell.
Reporting it as `-1` would invite a tool change against an unknown carriage state, so the
backend holds the last known tool and raises `AmsAction::ERROR` instead. A swap in flight
reads `-2` for a moment between the head sensor and the dock sensor changing, so an
unreported `-2` is a fault only outside one (`toolchanger_addon::sensor_error_is_fault()`);
a mount-to-feed run's Mount and Restore stages count as a swap. A `sensor_error` the
firmware itself reports is a fault during a swap, and at rest unless a definite
`current_tool` (-1 or a tool) sits beside it.

Upstream's [sensor-status.md](https://github.com/Irbis3D/MedusaHC/blob/main/docs/sensor-status.md) defines the two signals. `sensor_error` (with
`last_error` explaining it) latches when SET or DROP rejects an ambiguous starting state or
the sensors do not confirm the pick or drop; only the next SET or DROP the controller
accepts clears it, or a Klipper restart. Moving tools by hand does **not** clear it.
`current_tool` follows the dock pins live, and -2 alone means "undetermined", not an error,
during a swap or while someone rearranges hot ends. So the rule above, that a definite
`current_tool` at rest (or one arriving after the flag) means the docks agree and the flag
describes a past failure, is our choice, not upstream's: it lets the screen withdraw the
fault once the hot ends are seated, while the latch still names the failure.

The controller's `last_error` and `target_tool` name the failed operation: the fault dialog
reads "Tool change to Tool 3 (T2) failed: <last_error>" while the fault is up, and a failure
the screen is not showing as a fault (withdrawn, or left from before this connection)
toasts that sentence once, until the next operation clears it
(`AmsBackendToolChanger::controller_failure_text_locked()`). `target_tool` is the tool being
picked, or for a park the tool being parked, so a failed park reads "Parking Tool 2 (T1)
failed: <last_error>". An operation is a park when this screen dispatched one, or when its
first busy word is `dropping` (MHC_DROP never says `changing`; MHC_SET always opens on it). A `last_error` with `sensor_error` false has
no failed operation behind it: the controller sets it when its own startup fails
("Initialization failed; see klippy.log"), and it toasts once as "Tool changer error:
<last_error>", never as a tool change failure.

`operation` takes exactly `idle`, `changing`, `dropping` and `picking`. MHC_SET opens with
`changing` (homing and a Z hop before it reads the docks), then `dropping` when a tool is on
the head, then `picking`; that first frame lights the bar's first step. MHC_DROP goes
straight to `dropping`, with no `changing` frame. MHC_OPEN, MHC_CLOSE and MHC_CLEAN never
change `operation`. During a print, or a resume, priming and brush cleaning happen inside
`picking`, so that phase is long there.

### 1a. And which tools are in their docks

The same sensors answer a second question the tool number cannot: what is in each dock.
`[medusahc]` publishes it as `sensors`: `e` for the toolhead, `t0`, `t1`, ... per dock,
`1` meaning occupied. `ToolReading::docks` carries it, indexed by tool number.

A dock reading vacant for a tool that is not on the head means that hot end has been taken
out of the machine, which is why `refresh_slot_statuses_locked()` can now stamp
`SlotStatus::EMPTY` at all. The carriage slot always wins over its own dock reading: the
mounted tool's dock reads vacant because that is where the tool came from.

`docks` is merged across frames, never replaced. Moonraker republishes only the fields that
CHANGED, so a frame naming two docks says nothing about the third - a per-frame overwrite
would blank a tool on every partial update. Empty `docks` means "this frame said nothing",
which is not the same answer as "every dock is vacant".

### 2. A filament feeder

Only the hot end travels, so the filament is held by a servo gripper on the frame
(`[servo my_servo]`) that has to be released around a swap. There is no analogue in
klipper-toolchanger, because a toolhead changer carries its own extruder.

Surfaced as two `DeviceAction`s (`open_feeder` / `close_feeder`) in the AMS Device
Operations overlay. They are gated by
`check_preconditions(/*requires_toolhead_motion=*/true)`: the gripper is the only thing
holding the filament, so opening it mid-print drops the strand and kills the job, and the
overlay itself has no print gate.

### 3. Nozzle cleaning

`CLEAN` (a `[gcode_macro]` in `MHC_macros.cfg` wrapping the controller's `MHC_CLEAN`)
brushes the mounted tool's nozzle. `toolchanger_addon::clean_nozzle_gcode()` answers it
when the macro exists, and the backend offers **Clean nozzle** under Maintenance in Device
Operations. It moves the machine (`with_machine_motion()`), so it greys during a print and
`AmsBackendToolChanger::clean_nozzle()` refuses it under a print or another operation; it
is disabled with no tool on the carriage, where `MHC_CLEAN` only answers "no tool
installed".

## Two configurations

One controller, run with or without klipper-toolchanger.

| | Objects | Swap command |
|---|---|---|
| **Stock** | `[pin_watch io]`, `[medusahc]`, `gcode_macro T<n>` | `T<n>` (runs `SET T=<n>`, which runs `MHC_SET`), `DROP_TOOL` to park |
| **With klipper-toolchanger** (`sync_toolchanger: 1`) | the same, plus `[toolchanger]` + `[tool Tn]` | the same: `T<n>`, `DROP_TOOL` |

klipper-toolchanger never moves this hardware. It keeps an existing `[gcode_macro T<n>]`
rather than registering its own, its `UNSELECT_TOOL` runs only `dropoff_gcode` (empty in
the minimal config), and `ASSIGN_TOOL` never changes a `T<n>` macro. So the swap and the
park are the machine's own on both, and `toolchanger_addon::resolve_tool_commands()`
answers them whenever the MedusaHC row matches. `DROP_TOOL` is a `[gcode_macro]` in
`MHC_macros.cfg` (it also re-grips the filament); a config without it parks with
`MHC_DROP` then `MHC_CLOSE`. A plain klipper-toolchanger with no MedusaHC keeps
`SELECT_TOOL TOOL="tool Tn"` / `UNSELECT_TOOL`.

The status schema is the same on both: `operation`, `current_tool`, `target_tool`,
`last_error`, `feeder_open`, `layer`, `sensor_error`, `tool_count`, `sensors`. The feeder
runs on `MHC_OPEN` / `MHC_CLOSE` (native commands, absent from `printer.objects.list`), and
`OPEN` / `CLOSE` are the `[gcode_macro]` wrappers `MHC_macros.cfg` defines around them,
which HelixScreen sends.

### The phase word

`operation` (above) names the swap **direction**, which klipper-toolchanger's own
`status` does not: there a swap is one undifferentiated `changing`.
`ToolReading::phase_names_direction` records that a direction-naming word arrived, because
callers need it *before* a swap starts - the phase words themselves only appear once one is
already running.

Without klipper-toolchanger nothing but `operation` sets the action: `changing` and
`picking` are `SELECTING`, `dropping` is `UNLOADING`, `idle` ends the swap.

A swap command Klipper rejects with a gcode error (the controller's own "MedusaHC is busy", a
failed swap raised as an error) ends the optimistic `SELECTING` too. A reply timeout does not:
the macro may still be running. Nor does a rejection while the controller's last `operation`
word is busy: that is another client's swap, which keeps the action its word maps to until its
own `idle` frame ends it. That word counts only from a source that has said `idle` at least once
(Z-Mod's markers never do), and a Klipper restart or a lost connection forgets it.

**`feeder_open` is published on both configurations**, so the gripper phases are drivable on
every machine carrying `[medusahc]`.

The feeder buttons send the plain `OPEN` / `CLOSE` on every configuration: the stock
`MHC_macros.cfg` defines them as macros around `MHC_OPEN` / `MHC_CLOSE`, and they work on
a parked tool.

## Loading and unloading filament

MedusaHC has one extruder drive serving every tool, so filament moves only through the tool
on the carriage. `toolchanger_addon::feeds_through_mount()` (the provider row's
`feeds_through_mount`) says so, `AmsBackendToolChanger::set_discovery()` sets
`mounts_to_feed_`, and the backend answers `head_filament_ops()` Automatic in both
directions with `needs_mount_to_feed()` true. To every surface it is a plain Load and
Unload; the mounting happens inside the operation.

`include/mount_feed_sequence.h` builds the stages; `AmsBackendToolChanger::start_mount_feed()`
sends them one at a time, each on the previous one's ack, as ONE `LOADING` or `UNLOADING`
operation whose step bar is the stage list:

| Load | Gcode | Unload | Gcode |
|------|-------|--------|-------|
| Insert filament | an action prompt: "Insert filament into the hot end of Tool N while it is docked, then tap Continue", with Continue and Cancel. It comes first, while the tool is still in its dock: once the hot end is on the carriage, the extruder's gears stand between the operator and the nozzle. A tool already on the carriage gets "Insert filament into Tool N, then tap Continue", and a Load of several with the first one mounted names both cases in one sentence | Mount tool | the machine's own `T<n>`, the same swap the Tool buttons send; skipped when already mounted |
| Mount tool | same as Unload's; it closes the prompt | Heat nozzle | `SET_HEATER_TEMPERATURE HEATER=<tool's extruder> TARGET=<t>`, then `TEMPERATURE_WAIT SENSOR=<tool's extruder> MINIMUM=<t-2>` (`t` is the target slot's material temperature, else 220) |
| Heat nozzle | same as Unload's | Retract filament | `M83`, `G1 E-25 F1800`, `G1 E-150 F1200` (until the strand is fully out), `SET_HEATER_TEMPERATURE HEATER=<tool's extruder> TARGET=0` |
| Purge | `M83`, `G1 E5 F60` (slow draw), `G1 E50 F300` (tip to the nozzle), `G1 E<purge> F300` (the `purge_length` device action, 0-200mm, default 50, omitted at 0; persisted per printer as `toolchanger/purge_length_mm`), `G1 E-10 F1800`, `SET_HEATER_TEMPERATURE HEATER=<tool's extruder> TARGET=0` | Restore tool | remount the tool that was on the carriage (`T<n>`), or park (`DROP_TOOL`) if none was |
| Restore tool | same as Unload's | | |

A tool already on the carriage skips Mount and Restore: Insert, Heat, Purge. A completed
Unload leaves the strand in the tube, so it ends on a success toast naming every tool it
unloaded: "Unload complete. Pull the filament out of Tool 2, Tool 4." (`toast_unload_complete()`).

Nothing senses filament in a hot end, so the backend remembers what its own runs did:
`head_has_filament()` answers true for a head a completed Load fed, until a completed Unload of
it, and nullopt otherwise. The Unload picker reads such a docked tool as loaded rather than
ready to load (`BatchFilamentModal::row_at_toolhead()`). The record lives in memory only.

Neither direction sends `OPEN`: the feeder buttons stay a separate control.

Every temperature command names the tool's own heater by name (`extruder` / `extruder<n>`).
The swap does not run `ACTIVATE_EXTRUDER`, so Klipper's active extruder stays where it was
and a bare `M109` or `M104` would act on that one; a printer's own `M109` override may also
wait on a different heater than it set.

Each stage goes out through `ensure_homed_then(..., bypass_busy_gate=true)`, which only this
run passes: every other backend send keeps IMoonrakerAPI's busy gate. A swap leaves
`idle_timeout` reading `Printing` for a while, and the gate sends a script made only of
heater commands (a cleanup with nothing to restore) fire-and-forget then, dropping its
answer: the run would wait forever. No stage carries a bare `G0`/`G1`, so skipping the
gate's move refusal changes nothing for the run (the purge and retract open with `M83`).
A stage that times out (`kAmsOperationTimeoutMs`) or errors unwinds the run like any
failed stage.

`TEMPERATURE_WAIT` holds Klipper's gcode queue until the temperature is reached. A Cancel
during Heat takes effect only after the wait ends: its cleanup queues behind it. If
another client zeroes that heater mid-wait, the run's 300 s timeout fires and the run
unwinds, but Klipper itself stays in the wait until `M112` or a restart.

**The dock sensors confirm every swap.** A Mount or planned Restore that acks is not done
until the sensors place the expected tool on the carriage (`-1` after a park). The answer
can lead the frame that reports the finished swap, so the run waits up to
`kSwapConfirmGraceMs` for that frame. A reading still `-2`, a fault the controller reports,
or a different tool stops the run WITHOUT a restore - the carriage is unknown, and no swap
goes out against it - with heaters off by name and the toast "Load stopped. Dock sensors
cannot tell which tool is mounted". Outside a swap, any sensor fault under a live run stops
it the same way, and once the run is over the fault shows as the backend's `ERROR`. A
cleanup counts as a swap only when it actually puts the carriage back. The operator
prompt tolerates a transient unreported `-2` too, since pushing a strand into a docked hot
end can jostle a dock switch; one that outlasts the prompt fails the Mount's confirmation.

Every other way a run stops - a failed stage, Cancel, a lost connection - asks
`carriage_trusted_for_restore()` before sending the restore: with dock sensors, a swap in
flight or waiting on its confirmation, or a fault they report, leaves the carriage
unknown, and the cleanup turns the heaters off without it. A machine with no dock sensors
always restores.

The prompt is Klipper's own action prompt, raised with `RESPOND TYPE=command` lines, so it
shows on this screen and on Mainsail/Fluidd alike, and it needs `[respond]`. Its payload is
**English whatever this screen's language is** - the RESPOND lines are console text every
client shows verbatim, so no locale of ours may decide what Mainsail displays. Its buttons
answer with `RESPOND TYPE=command MSG=HELIX_MOUNT_FEED_CONTINUE` / `..._CANCEL`; the backend
watches the console for that exact echo (`mount_feed::parse_console()`). The sidebar's Cancel
(`AmsBackendToolChanger::cancel()`), the prompt's Cancel, and a prompt **dismissed** anywhere
(the screen's own dismiss sends `action:prompt_end`, whose echo is the only trace) all turn
the rest of the run into the cleanup: prompt closed and, once anything past the prompt has
gone out, the plan's heaters turned off BY NAME (`SET_HEATER_TEMPERATURE ... TARGET=0`,
never a blanket `M104 S0` - the active extruder before the first mount is the PREVIOUS
tool's, and its user-set target must survive) and the carriage restored. A stop at the
prompt itself, before anything moved or heated, only closes the prompt. A stage that fails does the same; Klipper's own error line reports
the failure. A cleanup that turns heaters off and cannot be sent at all ends the run and
says `Could not turn the heaters off`. A cleanup that only closes the prompt heated
nothing, so its failure ends the run with nothing further said: a printer without
`[respond]` refuses the prompt and its close alike, and the run's own "Load stopped."
already told the stop.

A stop that leaves the carriage where it found it - a failed swap, a lost connection,
a print taking over - adds "The carriage was not put back: check which tool is mounted."
to its toast.

**A Cancel during a swap waits for it.** With dock sensors, a Cancel while a Mount or Restore
is in flight or waiting on its confirmation does not unwind at once: Klipper finishes the swap
whatever the screen does, and a cleanup sent then would end the run while the carriage still
moves. The run stays busy (its action, step bar and no Cancel: `can_cancel_operation()` turns false,
and the sidebar and Device Operations follow AmsState's `ams_can_cancel` subject, since no action
edge comes; a second `cancel()` succeeds quietly) until the swap settles, then
turns the heaters off without a restore and toasts, as a warning, "Load stopped. The tool
change under way finished, and Tool 1 was not put back on the carriage." A swap that never
settles stops on the sensors' fault as usual. Without dock sensors the cleanup queues behind
the swap in Klipper and restores as before. Settings > AMS > Abort says "Aborting..." before it
calls `cancel()`, so a backend's own stop toast lands after it. Every sentence of a stop toast ends in a
period: "Load all stopped. Completed: Tool 1, Tool 3." (`toast_partial_run()`).

**A lost connection or a Klipper restart ends the run.** Neither can carry a cleanup: the
socket is gone, or Klippy is. Three signals reach the backend, whichever comes first:
the stage in flight failing with `CONNECTION_LOST` (`MoonrakerRequestTracker::cleanup_all()`
fails every pending request when the socket closes or Klippy disconnects),
`on_connection_lost()` (AmsState's connection observer, on the edge out of `CONNECTED`, for
a run waiting on the operator or the sensors with nothing in flight), and
`on_klipper_restarted()` (AmsState's klippy_state observer, on READY after any other
state). Each calls `stop_feed_run_unreachable()`: the run ends sending nothing, the action
goes IDLE, and the toast says "Load stopped. The connection to Klipper was lost." with the
carriage sentence and the completed tools.

The run's heaters are then OWED (`owed_heaters_off_`, the plan's heaters once anything past
the prompt went out, none before) and go out by name once Klippy is ready again: on the
first status frame after the reconnect (frames come only from a ready Klippy), or straight
from `on_klipper_restarted()`. A Klipper restart already zeroes every target, while a
dropped websocket leaves them all set; the failed request looks the same either way, so
the heaters-off always goes out, harmlessly after a restart. A heaters-off dropped by the
connection again stays owed, merged with whatever a later run came to owe meanwhile; a new
run sends any owed one before its own first stage. A print holding the printer (printing or
paused) when the debt comes due owns those heaters: the debt is dropped unsent, and a warning
names them ("Tool 4's nozzle is still heating."). After a lost connection the debt waits for a
frame carrying `print_stats.state`, and that frame's own state decides: PrinterState may apply
the same frame after the backend does, so its lifecycle can still describe the printer from
before the drop.
The carriage is never restored after a loss: nothing can vouch for it.

`abandon_feed_run_after_disconnect()` is the backend STOPPING under a live run
(`on_stopped()`, a printer switch or shutdown, with the connection possibly still up): it
sends the whole cleanup best effort and resets the run without waiting for an ack. It runs
without `mutex_` held, since the unwind takes it and emits; `on_stopping()` runs under it.

**A print ends the run.** Before each stage `send_feed_stage()` asks
`refuse_motion_if_printing()`: once a print is printing or paused, the run ends without
sending that stage or any cleanup - the print owns the heaters and the carriage - closes
the prompt if the operator's answer left it open, and toasts "... A print started." naming
the tools whose heaters its acked stages left set ("Tool 3's nozzle is still heating.",
`mount_feed::Run::heads_left_heating()`), so the operator can turn off any the print does not
use. The other side is `PrintStartController`: a start (and a reprint) is refused with "Wait
for the current filament operation to finish" while `AmsState::any_operation_running_here()`
(a backend's `running_step_operation()`: a run this screen sequences). A busy action alone,
such as another client's swap, does not refuse it: Klipper queues the print behind it.

**The wait has a ceiling.** Nobody inserting the filament for three minutes
(`mount_feed::kPromptTimeoutMs`) unwinds the run the same way and raises the error toast
"No filament was inserted in time": a run nobody is attending must not go on to mount and
heat a tool. A `LvglTimerGuard` fires `feed_wait_tick()` once a second while LVGL is up;
each tick hands `Run::advance_wait()` only the time since the previous tick, and the run
sums them. Unit tests drive `feed_wait_tick()` with an injected clock, or
`handle_wait_elapsed()` directly, through `ToolChangerTestAccess`, and the swap
confirmation's grace through `handle_swap_check_elapsed()` the same way.

**Load all / Unload all.** `max_heads_per_filament_op()` answers the tool count, so the
sidebar's picker accepts several heads on this backend. A Load of several asks **once**, up
front, exactly as a Load of one does ("Insert filament into the hot ends of Tools 1, 2, 4
while they are docked, as far as it goes, then tap Continue"), then works tool by tool
(Mount, Heat, Purge): each tool's opening stage heats the **next** one to its full material
temperature (`SET_HEATER_TEMPERATURE HEATER=extruder<n>`, the Klipper multi-extruder
naming - `extruder` for tool 0), so its own heat stage has nothing left to wait for. A preheat
never LOWERS a target: a next tool already set at least that hot is left alone
(`HeadRequest::heater_target_c`, read off the live extruder targets at dispatch). An Unload of several
runs the same sequence with no prompt. At the end the carriage is restored to the tool the
run found on it (or parked). A stop mid-sequence - cancel, timeout, failed stage - turns
every heater in the plan off, restores the carriage, and toasts the tools that did complete
(`mount_feed::Run::completed_heads()`).

**The printer's filament macros are never called.** The feed and retract stages always send
the built-in moves, and a batch of every tool is the same sequenced run as any other. A
`LOAD_FILAMENT` / `UNLOAD_FILAMENT` / `LOAD_ALL_FILAMENT` / `UNLOAD_ALL_FILAMENT` on a
MedusaHC printer is the owner's own customization (stock MedusaHC defines none), with no
shared contract for its parameters or what it does around the moves.

While a run is active its stage owns the published action and step: the swap frames its
Mount and Restore stages raise (`operation`, the gripper) do not end it. The step bar is
the stage list, laid out by `mount_feed::bar_rows()`: each tool's steps for a multi-tool run
("Tool 3 (2/3): Purge" while the second of heads {0, 2, 4} purges: the real tool, then its
place in the run), with the insert on the first tool only and the restore on the last
only, since one prompt and one restore cover the run. `get_operation_step_model()` builds
it from the live run and `Run::phase_index()` publishes the index into it. A batch from the
head picker names its operation to the sidebar, which rebuilds the bar once the run exists. A
sidebar opened mid-run asks `running_step_operation()` (load or unload, from the run) instead
of guessing from the action frames, where a load still heating reads like a swap. Each Heat
row's live readout follows that tool's own heater (`step_heater()`, `mount_feed::heater_name()`
of the row's head), not Klipper's active extruder: a MedusaHC without klipper-toolchanger never
runs `ACTIVATE_EXTRUDER`, so the active extruder stays `extruder` whatever is mounted. The
sidebar re-attaches those per-heater observers when the extruder set changes
(`extruder_version`), since a rediscovery re-creates the subjects. While
the run is cancellable (`can_cancel_operation()`: a run not already unwinding or waiting out a
swap, the same predicate `cancel()` acts on), the sidebar shows Cancel at the end of its status line in place of its action
buttons; it is the same abort as Settings > AMS > Abort. A backend whose `cancel()` can never
act (QIDI Box, Snapmaker U1, Multi-ACE) answers `can_cancel_operation()` false, so neither
surface offers one. The backend claims
`filament_ops_self_home`, so a load or unload is refused while a print is running or
paused, and `supports_auto_heat_on_load/unload`, so the UI does not preheat the active tool
before the backend mounts another.

`HELIX_MOCK_AMS=medusahc` runs all of it against mock hardware: the mock client echoes
`RESPOND` lines, so the prompt appears and its buttons drive the run. Every mock hot end
follows its own target (`SET_HEATER_TEMPERATURE HEATER=extruder<n>`), so the Heat readouts
move and a Resume onto a heated secondary tool goes through; the persona's configfile carries
upstream's example `RESUME`, which runs `MHC_RESUME`. Tests:
`tests/unit/test_toolchanger_mount_feed.cpp`. **Unverified on hardware.**

## Overriding the feeder macros

The command names changed upstream, and a machine mid-migration can have either set,
both, or legacy aliases repointed at something custom. So the macro each button sends is
user-selectable, not hardcoded.

`AmsBackendToolChanger::get_device_actions()` adds two `DROPDOWN` actions
(`feeder_open_macro` / `feeder_close_macro`) whose options come from
`feeder_macro_candidates()`: the macros the printer actually reports, filtered to `MHC_*`
plus anything containing `OPEN`, `CLOSE`, `FEEDER` or `GRIP`. A raw macro list runs to
hundreds of entries and is unusable as a picker; a free-text field cannot be typo-checked.

The first option is the `auto` sentinel, which restores whatever detection chose (`OPEN` /
`CLOSE`).

`Feeder` carries all of it - `open_gcode` (what is actually sent), `detected_open` (what
detection chose, kept so `auto` can be restored), `open_choice` (the stored pick) and
`macro_options`.

Persisted per-printer via `SettingsManager::get_feeder_open_macro()` /
`set_feeder_open_macro()` (and the `close` pair), under
`wizard::FEEDER_OPEN_MACRO` / `FEEDER_CLOSE_MACRO`. Per-printer rather than global because
two MedusaHC machines on one network can be at different points in the migration.

## The operation step bar

The sidebar's step bar used to render the legacy Heat/Feed/Purge stepper on every tool
changer, because `AmsBackendToolChanger` overrode neither `toolchange_phase_template()` nor
`get_operation_step_model()` and fell through to the coarse `AmsAction` fallback. On a
hotend changer none of those three steps happen: nothing heats, no filament feeds and
nothing purges. It swaps a hot end.

The steps a machine gets are now derived from what it actually reports, by
`tc_step_sequence()` in `ams_backend_toolchanger.cpp`:

| reports | swap | fresh load | unload |
|---|---|---|---|
| `operation` + `feeder_open` (Irbis3D) | Release / Dock / Pick up / Grip | Release / Pick up / Grip | Release / Dock / Grip |
| neither (plain klipper-toolchanger) | *no bar* | | |

Two rules produce all of it. The gripper brackets the operation, because a swap releases
the filament before anything moves and re-grips once the new hot end is seated. The middle
is the direction when the controller names it, and one undifferentiated "Change tool" when
it does not.

`tc_step_sequence()` is the single source for both the model and the phase-to-index
mapping, so a step added to it appears in the bar and becomes addressable at the same index
in one edit. That alone is not enough to keep the two in sync at runtime, though: the model
is built once, when the sidebar starts an operation, while the index is recomputed from
`tc_step_sequence()` again on every frame after that. Moonraker republishes only the fields
that changed, so the feeder/direction latches the sequence depends on can flip in between -
deriving both from the same function does not stop them from being derived from *different*
snapshots of it.

What actually prevents drift: `get_operation_step_model()` snapshots the latches it built the
model from, and `step_index_for_phase_locked()` resolves every frame's index against that same
snapshot rather than the live latches. Once a model exists, its index is pinned to the exact
sequence it was built from until the next model build refreshes the snapshot - the two cannot
drift apart in between.

**Mapping an idle frame.** Both ends of a swap read `idle`/`ready`, and the gripper is the
only thing separating them: open is the release that OPENS the operation, closed is the
grip that CLOSES it. A machine that does not report `feeder_open` cannot tell them apart
and gets no step for an idle frame rather than a guessed one.

That same ambiguity is why `apply_tool_sensor_locked()` does not report `AmsAction::IDLE`
for an idle-with-the-gripper-open frame while an operation is in flight. Doing so ended the
operation in the UI the instant it began - the bar was torn down on step 0 and the action
buttons came back mid-swap. It is gated on an operation actually running, because the user
can open the gripper by hand from the feeder panel while genuinely idle and that must not
read as busy.

**Suppression.** A machine with no phase source returns
`OperationStepModel{.suppressed = true}`, which is distinct from an empty model: empty means
"no model, use the legacy bar", suppressed means "no phases, and the legacy bar describes a
different kind of machine." The sidebar renders no step bar at all, and the status line
above it already names what the printer is doing. On MedusaHC that case never arises - both
controllers report phases - so in practice it is plain klipper-toolchanger, whose only
signal is `toolchanger.status == "changing"`.

**Which actions count.** The bar's visibility and its operation-start detection both ask
`IStepOperation::action_tracks_step_operation()`. They used to be two hardcoded action lists
that disagreed, and a tool changer fell through both: it reports `SELECTING` for a swap and
never heats, feeds or purges, so no start was detected, and once a bar did exist by accident
it vanished for the half of the swap spent in `SELECTING`. The default is the
filament-system vocabulary; `AmsBackendToolChanger` overrides it with
`SELECTING || UNLOADING`. Distinct from `ams_action_is_busy()`, the broader
"is anything happening" question the slot pulse asks.

## Detection

`toolchanger_addon::present(hw)` is
`has_pin_watch() && (has_tool_changer() || has_medusahc())`.

`pin_watch` is required, and alone it is just the extra; `[toolchanger]` alone is any of the
many klipper-toolchanger builds. `[medusahc]` + `[pin_watch io]` with no `[toolchanger]` is
the stock install and the main case; `[toolchanger]` beside pin_watch covers the optional
klipper-toolchanger setup and the frozen legacy-macros config. `[medusahc]` is matched
exactly, because `[medusahc_calibrate]` (the separate MedusaHC-Calibrate project) is another
object and not this one.

Without klipper-toolchanger there are **no `[tool N]` objects to name the tools**.
`PrinterDiscovery` enumerates the extruder heaters instead (one hot end per extruder is what
a hotend changer is) and keeps the G-code tool numbers as the names, `T0`..`T5`. Real
`tool N` objects win where they exist.

There is no `INITIALIZE_TOOLCHANGER` on a stock install either. `reset()` and `recover()`
send it only where klipper-toolchanger runs (`ToolCommands::initialize_available`); without
it they answer NOT_SUPPORTED, and `reset_available()` / `recover_available()` disable the
Device Operations Home and Recover buttons.

**It deliberately does not touch `has_mmu_` / `mmu_type_`.** Those pick which AMS backend
gets built. An unguarded write there replaces a working backend on any printer that
happens to run `pin_watch`, and the same applies to detecting on `T<n>` macros - every
multi-tool Klipper setup has those. `tests/unit/test_medusahc_detection.cpp` carries the
regression matrix (Snapmaker U1, Happy Hare, AD5X IFS, QIDI Box, plain IDEX), each of
which was measured resolving to MedusaHC when the signal was wired as an MMU type.

`PrinterDiscovery` records only the plain object facts (`has_pin_watch()`,
`pin_watch_object_name()`). Deciding that pin_watch + toolchanger *means* MedusaHC is the
add-on module's business - see the vendor-abstraction rule in the root `CLAUDE.md`.

## Tool mapping

A MedusaHC always takes `RemapStrategy::GcodeRewrite`: the job file is rewritten and
printed through the HelixPrint plugin. Its `T<n>` macros name the physical tool, and even
with klipper-toolchanger beside the controller `ASSIGN_TOOL` never moves a tool there (it
keeps the existing `T<n>` macro), so the `Native` route would print on the firmware's own
mapping. `AmsBackendToolChanger::get_remap_strategy()` asks `tool_commands_.present`, which
`resolve_tool_commands()` sets for every MedusaHC and for any changer without
klipper-toolchanger.

On the rewrite route the file is the only thing that moves, so a start macro that takes
per-tool values (`EXTRUDER_TEMP`, `EXTRUDER1_TEMP`, ...) keeps heating the tools the slicer
chose while the rewritten body selects others. The rewrite handles `INITIAL_TOOL=` and
`TOOL=`, and the Filament Mapping dialog warns about the rest while the picks move a tool; see
[the remapper](FILAMENT_MANAGEMENT.md#gcode-tool-remapper). The recommended slicer start
G-code for these machines is in the user guide,
[Printers that map filament by rewriting the file](../user/guide/filament.md#printers-that-map-filament-by-rewriting-the-file).

Any `HELIX_MOCK_AMS=medusahc*` variant runs the rewrite route end to end in `--test`; set
`helix_plugin_installed` to `1` with `ctl set` to get past the plugin gate.

## Tool offsets

The controller keeps the runtime offsets as variables on one macro,
`[gcode_macro _TOOL_OFFSET]` (`t<n>_off_x`, `t<n>_off_y`, `t<n>_off_z`; its `_macro_name()`
prefers the `_` name and accepts a plain `TOOL_OFFSET`), and the durable copy in
`save_variables` as `t<n>_gcode_{x,y,z}_offset`, which `_restore_saved_offsets()` loads
into the macro at startup. The `TOOL_OFFSET macro` row in `src/printer/tool_offsets.cpp`
resolves the macro the same way, reads and writes all three axes, and a save writes both
stores. klipper-toolchanger's own offset model is not the authority here, even where it
runs.

`_apply_offset()` sets X, Y and Z **absolutely** with `SET_GCODE_OFFSET` on every swap, so
a write to the macro changes nothing on the head until the next swap. After writing the
mounted tool's offset, HelixScreen sends `MHC_TOOL_OFFSET T=<n> MOVE=0`
(`tool_offsets::activate_mounted_offset_gcode()`), which applies it now without moving the
head. The mock's `MHC_TOOL_OFFSET` applies X, Y and Z the same way. That same absolute write also replaces any babystep on the next swap: the controller
treats the G-code offset as the tool's.

## Pause and resume after a failed swap

Upstream's [recovery guide](https://github.com/Irbis3D/MedusaHC/blob/main/docs/recovery.md)
describes it. A failed swap during a print runs `MHC_ERROR`, which sets
`_GLOBAL_STATE.error_state` and pauses the print, with no gcode error (the command handler
swallows `_OperationPaused`). The documented `PAUSE` macro saves the mounted tool as
`_GLOBAL_STATE.target_tool` on a manual pause and leaves the swap's own target after a
failure. The example `RESUME` (`macros_examples.cfg`), while paused, runs only
`MHC_RESUME T=<target_tool>`: after a failure, or when another tool is mounted, it homes XY,
closes the feeder and picks the target again (priming and brushing it inside `picking`), and
it refuses while that tool's nozzle is at or below 190C.

Wiring `RESUME` to `MHC_RESUME` is an install step the owner may skip, so the rule binds
only where it is done: `toolchanger_addon::resume_rule()` names the floor and the command
(190, `MHC_RESUME`), and `resume_rule_in_force()` asks `MacroParamCache::macro_runs()`
whether the printer's `RESUME` template, or a macro it calls, runs that command. It is
asked when needed, not at discovery, so it reads whatever configfile the cache last held.

Where it binds, the screen supports it with the least new UI. While a print is held, the
failure's dialog or toast adds "Seat the hot ends, heat Tool 3 (T2) above 190°C, then tap
Resume.", and `AmsBackendToolChanger::prepare_for_resume()` (the hook the Resume button
already runs) refuses with "Heat Tool 3 (T2) above 190°C before resuming" while the target's
own heater reads at or below the floor. Both key on the controller's test: its latched
failure (`sensor_error`, standing in for `error_state`, which the status does not publish),
or a mounted tool other than its `target_tool`. A manual pause with the target still mounted
resumes as before, and a printer whose `RESUME` does not run `MHC_RESUME` shows the failure
alone, with no temperature claim and no gate.

## Adding another hotend changer

Add one `Provider` row to the table in `toolchanger_addon.cpp`: a detection predicate,
which status objects to subscribe, and the feeder gcode. No call site changes.

## Printer database

Four entries in `assets/config/printer_database.json` - two frames, each with and without
the changer. Both frames share the same art (`duender.png`, `ducr10.png`), rendered from
the project images.

- `duender_medusahc` - `pin_watch` (92) and `medusahc` (92), hostname `medusa` (85),
  `toolchanger` corroborating at 20. `tool_count_4` and `kinematics_match corexy` are
  deliberately absent: they describe any 4-tool CoreXY toolchanger, and since
  `has_pattern()` is a case-insensitive substring match they would claim machines with no
  `pin_watch` at all.
- `duender` - hostname only. [Irbis3D/Duender](https://github.com/Irbis3D/Duender) is a
  mechanical Ender-3 to CoreXY conversion that ships **no Klipper config**, so a bare
  Duender has nothing in `printer.objects.list` distinguishing it from any other CoreXY.
  A `corexy` heuristic here would compete with the Voron and RatRig entries on every one
  of their machines. The wizard's manual-pick path covers it instead.
- `ducr10_medusahc` - the same MedusaHC signals on the DuCR10 frame (Andizzle's "DUO CR10"
  remix of the Duender, [Printables 1263532](https://www.printables.com/model/1263532-duender-remix-duo-cr10-corexy-conversion-gridfinit)).
- `ducr10` - hostname only, same reasoning as `duender`.

The bare-frame hostname deliberately does not appear on either MedusaHC entry, or the bare
entry could never outscore it (scoring is highest-match + 3 per extra match). The `ducr10`
hostname is the one exception: it sits at 40 on `ducr10_medusahc`, low enough to lose to
the bare `ducr10` entry (80) on a changer-less machine but still worth its +3 bonus on top
of a MedusaHC object match.

**The two MedusaHC entries cannot be told apart by hardware.** Same extra, same objects,
same schema - only the hostname says which frame it is, so each entry carries a
`hostname_exclude` for the other's name. A MedusaHC machine named neither is a genuine
tie, and whichever entry the detector reaches first wins; both name a MedusaHC CoreXY, so
the miss is the frame picture, not the capabilities.

## Known gaps

Not yet reconciled, and worth knowing before extending this:

(Per-tool spool metadata used to be listed here. It is now persisted - see
[FILAMENT_SLOT_METADATA.md](FILAMENT_SLOT_METADATA.md#tool-changer-is-the-odd-one-out).)

- **`layer` and the prime flags** - per-tool priming scheduled against the layer number.
  The layer command is `_LAYER_SET L=` (wrapping `MHC_LAYER_SET`), the first-use flags are
  `_PRIME_FLAGS_SET` and `PRIME_FLAGS_CLEAR`. No analogue in the toolchanger model, unused
  today. The old aliases `DROP`, `TOOL_OFFSET_T`, `LAYER_SET` and `PRIME_FLAGS_SET` are gone
  upstream; nothing here sends them.
- **No schema version.** `get_status()` still carries no version field, so a renamed key
  reads as an absent one. Prefer widening the provider table over hardcoding against any
  single revision of the controller.

## What upstream has committed to (Sergei / Irbis3D, 2026-08-24)

Answers to the questions this document raised. Treat as intent, not contract.

- **`[pin_watch io]` is staying.** It remains the primary source for sensor states and for
  which tool is physically installed, so detection requires it.
- **`operation`, `current_tool`, `sensor_error`, `tool_count` are stable** and will not be
  renamed without notice. There is no schema version field today; he intends to add one,
  plus compatibility flags, so external interfaces can adapt.
- **The `sensors` dict is per-dock, and we should be using it.** `e` is the toolhead
  sensor, `t0`, `t1`, ... the docks. `1` means occupied: `e: 1` is a tool on the head,
  `tN: 1` is tool N seated in its dock. Example: `{"e":1,"t5":1,"t4":1,"t3":0,"t2":1,
  "t1":1,"t0":1}` is T3 on the head, everything else docked. That is enough to show real
  per-dock state instead of only the mounted tool number.
- **`verify_tool_pickup: False` is intentional.** klipper-toolchanger is there for offset
  calibration and for syncing the active tool into Mainsail/Fluidd; MedusaHC performs the
  swap and the sensors through pin_watch are the physical truth. Read
  `medusahc.current_tool` and `medusahc.sensors`, never `toolchanger.tool_number` - which
  is what `apply_tool_sensor_locked()` already does.
- **The klipper-toolchanger dependency is meant to go away** eventually, so MedusaHC runs
  standalone. No major behaviour changes are planned in the meantime, and he will give
  advance notice.

Since then (Irbis3D/MedusaHC, 2026-08-30 onward): the Python controller is the default
and described as proven reliable, the klipper-toolchanger dependency is dropped (it is now
optional), and `get_status()` still has no schema version field.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the
shared architecture and [FILAMENT_BACKEND_TOOLCHANGER.md](FILAMENT_BACKEND_TOOLCHANGER.md)
for the backend that drives it.
