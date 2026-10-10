# OpenAMS Filament Backend

`AmsBackendOpenAms` drives [klipper_openams](https://github.com/OpenAMSOrg/klipper_openams)
through the versioned status contract its `oams_manager` publishes
([UI_API.md](https://github.com/OpenAMSOrg/klipper_openams/blob/master/docs/UI_API.md)). It reads no AFC object and sends no AFC command. AFC's own OpenAMS
support is a separate, fully supported path: `AFC_OpenAMS` units belong to the AFC backend
([FILAMENT_BACKEND_AFC.md](FILAMENT_BACKEND_AFC.md)), which does not use `oams_manager`.

The contract's constants and its "supported" test live in one place,
`include/openams_api.h`, shared by discovery and the backend, so the printer is claimed
exactly when the backend can read it.

## Discovery and precedence

`printer.objects.list` carries names, not status, and a manager that predates the API still
registers `oams_manager` (it publishes only `current_group`). So the name decides nothing
on its own:

1. `PrinterDiscovery::parse_objects()` records that `oams_manager` exists and claims
   nothing.
2. `claim_status_query()` then names `oams_manager.api_version` and `schema`, but only
   when no other filament system claimed the printer. **Any other MMU wins**, AFC
   included, whatever the object-list order, so a leftover `[oams_manager]` section
   never takes over a working AFC setup. A settled claim is an MMU claim like AFC's, so
   it also outranks the multi-extruder and toolchanger backends on the same printer.
3. `MoonrakerDiscoverySequence` sends that query before the early hardware callback and
   hands the reply to `settle_status_claims()`, which claims `AmsType::OPENAMS` only for
   `api_version == 1` with schema `openams.manager`. A missing or newer version, or a
   failed query, leaves the printer unclaimed, as if OpenAMS were absent. The query
   reaches Klippy ahead of every later discovery request, so its reply also precedes the
   subscription.

Printers with no `oams_manager` take the unchanged path: the callback fires at once.

The subscription, from `AmsBackendOpenAms::required_status_objects()`, asks for
`api_version schema ready commands lanes units groups`. Those
arrays arrive whole on every change, so topology and state never land half-applied.

## Status model

- `lanes[]`: independently operated filament paths (one FPS each). A lane's `state`
  (`loading`, `unloading`, `loaded`, ...) drives `AmsAction`; its `current_slot` marks
  the loaded slot.
- `units[]`: feeders on a lane. `topology` (`hub`, `linear`, `parallel`, `mixed`)
  shapes the drawing; rendering never branches on `kind`. An unfamiliar topology draws
  as a hub, because slots are addressed by id and still load and unload correctly.
- `units[].slots[]`: manager slot ids, unique in the snapshot, with `ready` and
  `loaded`. Commands address these ids, never array positions.
- `groups[]`: the slots that can satisfy a tool or material request. A `T<n>` group is
  tool n.

The backend flattens every unit's slots into HelixScreen's global slot index and keeps
each slot's manager id and group for dispatch. Moonraker sends only changed fields, so
updates merge over the last snapshot. With several lanes loaded, no single current slot
or tool is reported: independent lanes are not folded into one value.

A status without a supported API (for example, after a downgrade) presents nothing: no
units, no action and no error.

## Filament pressure sensor

Each lane may carry `pressure` and `set_point` (klipper_openams
[UI_API.md](https://github.com/OpenAMSOrg/klipper_openams/blob/master/docs/UI_API.md)).
`pressure` is the lane's FPS reading, 0.0 to 1.0. `set_point` is the reading the
feeding unit's hub motor regulates to: below it the extruder is pulling harder
than the hub feeds (filament pulling tight), above it the hub is overfeeding
(filament loose).

The backend puts the reading on every unit of that lane as its `BufferHealth`
(`fps_value`, `fps_set_point`, `fps_reported`). It writes no `sync_feedback_bias`:
that field is Happy Hare's. Everything draws through `helix::buffer_reading()`
(`include/buffer_reading.h`), so:

- `BufferHealth::fps_to_bias()` maps the pressure onto the -1..+1 bias around the
  set point, and the one colour rule (grey below 0.3, amber, red from 0.7) applies;
- the Filament Buffer widget, the loaded-spool card and the Buffer Status modal read
  the lane feeding the current slot, else the first unit with a sensor (with several
  lanes loaded there is no current slot); the path canvas draws the unit's own lane;
- the path canvas draws the buffer box labelled **FPS** because the buffer reports
  pressure (`ui_ams_detail.cpp`), tinted live; any buffer that reports pressure gets
  the same label;
- tapping it opens the Buffer Status modal: the slider, "FPS 62%" and "target 50%",
  the lean in words and the last minute as a trace line.

OpenAMS has a buffer reading and no clog detection: klipper_openams publishes none, so
the Clog Detection widget has nothing to show. See
[Filament buffer reading](FILAMENT_MANAGEMENT.md#filament-buffer-reading).

A lane with no `set_point` cannot be placed either side of its target, so it
gets the pressure reading as text alone ("Pressure: N%"), with no slider, trace or tint. A manager that
publishes no `pressure` gets no buffer box.

## Operations

Every action uses the command the manager advertises in `commands`. **A missing command
disables only its own action**; status and the other actions keep working.

| Action | Command (v1) | Without the command |
|--------|--------------|---------------------|
| Load slot | `<commands.load> GROUP=<group> SLOT=<id>` | Load and tool change refuse with NOT_SUPPORTED |
| Unload | `<commands.unload> FPS=<lane id>` | `can_unload_from_toolhead()` answers false, so Unload is not offered |
| Cancel | `OAMSM_LOAD_FILAMENT_CANCEL` | refuses with NOT_SUPPORTED; Abort is disabled |
| Reset / recover | `OAMSM_CLEAR_ERRORS` | refuses with NOT_SUPPORTED |

The command names are the manager's to choose: klipper_openams advertises
`OPENAMS_LOAD` / `OPENAMS_UNLOAD` (its macros accept and ignore `FPS`), the openams plugin
advertises `OAMSM_LOAD_TO_TOOLHEAD` / `OAMSM_UNLOAD_FROM_TOOLHEAD`. The parameters are the
same for both: `SLOT` is the global slot id and `FPS` is `lanes[].id`. A manager that
advertises no load or unload command shows status and can reset, but cannot load or
unload. While `ready` is false, every action is refused.

- **Tool change** resolves group `T<n>` to a slot: the member already loaded, otherwise
  the first member with a spool ready. A group with no ready member is refused before
  anything is sent, rather than failing in `OAMSM_VALIDATE_LOAD`.
- **Load and unload** stay pending until Moonraker completes the macro call. Completion
  emits `EVENT_LOAD_COMPLETE` or `EVENT_UNLOAD_COMPLETE`; failure unwinds the pending
  action and keeps the reason in `operation_detail` until the next action or
  `clear_fault()`. The macro's own error reaches the user through the G-code error
  stream, so the dispatch passes `caller_surfaces_errors=false` and only unwinds. Both
  callbacks hop to the main thread through the lifetime token.
- **Unload** names the lane the loaded slot sits on (`FPS=<lane id>`), so it empties the
  lane the user acted on even with several lanes loaded. A caller naming no loaded slot
  gets the current slot, else the one lane that is loaded.
- **Load over a loaded lane** is a swap. The manager's load does not clear a lane that
  already holds another slot, so the backend sends `<unload> FPS=<lane>` and the load as
  one two-line script, and the action reads as one LOADING operation. A load into an empty
  lane, or of the slot already loaded, is the single load line. `needs_unload_before_load()`
  answers per lane for the same rule, so slots on other lanes never plan an unload.
- **Cancel** reaches only a load the manager is running on its own, such as a runout
  reload. `OAMSM_LOAD_FILAMENT_CANCEL` is ordinary G-code: while a load started from the
  screen runs, `OPENAMS_LOAD` holds Klipper's G-code queue, and the cancel could not run
  until nothing was left to cancel. That case is refused as busy, and the backend's own
  record of the load is left alone. `can_cancel_operation()` answers false in that case,
  so Device Operations shows Abort disabled rather than offering one that fails. The
  manager's `openams/cancel_load` webhook can interrupt a load, but Moonraker does not
  expose it.
- **Homing** is the macro's job, so none is added (`skip_homing`).

## Unit faults

The openams plugin publishes `oams_manager.devices.<unit>.faults[]` (severity, code, text,
bay, actions); klipper_openams publishes no `devices`, so it never raises one. The
subscription asks for `devices` and ignores it when absent. A `stop` or `pause` fault
puts the system in `AmsAction::ERROR`, marks the unit's slots (the one bay for a bay
fault) with a `SlotError`, and fills `operation_detail`, so the overview shows the unit's
error dot and the sidebar text. `current_error()` (channel B of the AMS error model)
turns it into the recovery dialog with one **Reset** action. Codes read in plain
words (`motor_drive_fault`, `motion_timeout`); any other code reads as the unit's own text.

Reset, the recovery dialog and `clear_fault()` send one script: `OAMS_CLEAR_FAULT
OAMS=<unit id>` for each unit that has a fault advertising `clear_fault`, in unit order,
then `commands.reset`. A firmware fault is not cleared by `OAMSM_CLEAR_ERRORS`, and while
it is latched the unit refuses all motion. With no unit fault the script is `commands.reset`
alone, as before.

## Shared hub

`units[].lane` is the unit's hub: units naming the same lane feed one hub, one FPS and one
toolhead. The backend copies it to `AmsUnit::hub_id`, and `compute_system_tool_layout()`
folds units with the same non-empty `hub_id` onto one nozzle, so the overview draws the
overview model: unit columns merging into one **Hub**, the lane's **FPS** box
(`ui_system_path_canvas_set_buffer()` / `set_unit_hub()`) and one toolhead per lane. Groups
`T<n>` are filament groups and never toolheads. Units on different lanes are different hubs on
different toolheads, so two lanes draw two chains. The openams plugin's `lanes_by_fps[<lane>].extruder`
becomes each slot's `extruder_name`, so the toolhead is badged by its extruder; without it
the badge is the toolhead's position. Only `units[].lane`, `groups[].lane` and `lanes[]` are
needed for the drawing, so klipper_openams (which publishes nothing else) draws the same.

The unit detail inside the overview is hub-only by design: it draws the slots down to the
hub, and the trunk below (FPS, nozzle) is the overview's.

## Slot identity

OpenAMS reports no colour, material or spool identity, so identity is HelixScreen's:

- a `FilamentSlotOverrideStore` on the shared `lane_data` namespace (`laneN`, 1-based),
  loaded at start with legacy records ingested;
- `apply_user_edit()`, `persist_slot_weight()`, `persist_external_identity_impl()` and
  `clear_slot_override()` keep the stored record in step, so edits, Spoolman links and
  metered weights survive a restart, and "clear slot metadata" empties both the record
  and the live slot;
- `firmware_publishes_lane_identity()` is false and `lane_record_store()` names the
  store, so a resync re-reads what other `lane_data` writers changed. A repaint between
  frames takes identity from the lane alone: `overrides_` is what the store persists and
  a resync does not refresh it, so nothing is decided from it;
- a spool link written after start is picked up from `notify_openams_spoolman_status` (sent
  by the openams `[openams_spoolman]` Moonraker component whenever it updates
  `lane_data`): the store re-reads the namespace without blocking, the records are filed
  onto the lanes (every source except a person's own edit, which stands) and the slots are
  repainted. A manager without the component never sends it;
- a spool inserted into a slot last seen empty is, under the insert rule
  (`docs/specs/filament_slots.md` §6), always "no evidence": the record stays and
  the "same spool?" notice offers Clear. The first frame after start is a baseline, and
  a frame from an offline unit or a manager that is not `ready` neither raises the notice
  nor moves the baseline, since those bays have not been read.

## Capabilities

| Capability | Behaviour |
|------------|-----------|
| Per-slot loaded authority | Yes, from `slots[].loaded` |
| Tool mapping | Read-only `T<n>` groups; `owns_tool_mapping_table()` is true, edits are NOT_SUPPORTED |
| Bypass | No |
| Endless spool | Not exposed |
| Runout surface | No error hook, so the generic runout modal and toast remain (`runtime_config.cpp`) |
| Environment sensors | Per unit from `devices.<unit>.environment` (`temp_c`, `rh_pct`), into `AmsUnit::environment`; openams only |
| Dryer | Per unit, see below; openams only |
| Filament pressure | Per lane, from `lanes[].pressure` and `set_point`; drawn as the FPS box with bias tint and the buffer slider |

## Dryer

A unit offers drying when `devices.<unit>.capabilities.dryer` is true and
`supported_actions` holds both `dryer_start` and `dryer_stop`; `get_dryer_info(unit)` then
reports the unit's range (`dryer_target_min_c` / `dryer_target_max_c`), whether it is running
(any `dryer.state` other than off, idle or fault), its target, the time left, the chamber
temperature (`telemetry.dryer.chamber_c`) and the fan. The generic environment indicator and
overlay drive it per unit. Start clamps the target to the unit's range and the duration to
1 s - 7 days and sends `OAMS_DRYER_START OAMS=<units[].id> TARGET=<C> DURATION=<s>`; stop sends
`OAMS_DRYER_STOP OAMS=<idx>`. A unit with `dryer_requires_unloaded` (the AMS 2 Pro) refuses
to start while any of its bays is loaded. klipper_openams publishes no `devices`, so it shows
no climate readout and no dryer.

## Tests

`tests/unit/test_ams_backend_openams.cpp` (`[openams]`) covers the discovery claim (a
supported API claims the printer; a pre-API manager, unknown version or failed query does
not; AFC keeps it in either object order), the snapshot model, partial updates, and the
public `load_filament()` / `change_tool()` / `unload_filament()` entry points. It checks
completion and error callbacks, each missing command, `ready=false`, a multi-slot group,
cancel in each state, weight persistence with slot-metadata clearing, and the
insert notice. `tests/unit/test_discovery_klippy_gate.cpp` drives the real discovery
sequence through the claim query, and `tests/unit/test_moonraker_subscription_fields.cpp`
pins the subscribed fields.
