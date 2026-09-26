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

## Operations

Every action uses the command the manager advertises in `commands`. **A missing command
disables only its own action**; status and the other actions keep working.

| Action | Command (v1) | Without the command |
|--------|--------------|---------------------|
| Load slot | `OPENAMS_LOAD GROUP=<group> SLOT=<id>` | Load and tool change refuse with NOT_SUPPORTED |
| Unload | `OPENAMS_UNLOAD` | `can_unload_from_toolhead()` answers false, so Unload is not offered |
| Cancel | `OAMSM_LOAD_FILAMENT_CANCEL` | refuses with NOT_SUPPORTED; Abort is disabled |
| Reset / recover | `OAMSM_CLEAR_ERRORS` | refuses with NOT_SUPPORTED |

The manager advertises load and unload only once the user has merged the updated
`oams_macros.cfg` macros, so an install with the new manager and old macros shows status
and can reset, but cannot load. While `ready` is false, every action is refused.

- **Tool change** resolves group `T<n>` to a slot: the member already loaded, otherwise
  the first member with a spool ready. A group with no ready member is refused before
  anything is sent, rather than failing in `OAMSM_VALIDATE_LOAD`.
- **Load and unload** stay pending until Moonraker completes the macro call. Completion
  emits `EVENT_LOAD_COMPLETE` or `EVENT_UNLOAD_COMPLETE`; failure unwinds the pending
  action and keeps the reason in `operation_detail` until the next action or
  `clear_fault()`. The macro's own error reaches the user through the G-code error
  stream, so the dispatch passes `caller_surfaces_errors=false` and only unwinds. Both
  callbacks hop to the main thread through the lifetime token.
- **Unload with several lanes loaded** is refused: the v1 command names no lane.
- **Cancel** reaches only a load the manager is running on its own, such as a runout
  reload. `OAMSM_LOAD_FILAMENT_CANCEL` is ordinary G-code: while a load started from the
  screen runs, `OPENAMS_LOAD` holds Klipper's G-code queue, and the cancel could not run
  until nothing was left to cancel. That case is refused as busy, and the backend's own
  record of the load is left alone. `can_cancel_operation()` answers false in that case,
  so Device Operations shows Abort disabled rather than offering one that fails. The
  manager's `openams/cancel_load` webhook can interrupt a load, but Moonraker does not
  expose it.
- **Homing** is the macro's job, so none is added (`skip_homing`).

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
| Environment sensors | No |

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
