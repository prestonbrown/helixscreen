# Lane identity: Moonraker is the truth (1.1)

| | |
|---|---|
| **Status** | Design for review |
| **Target** | release/1.1, then ported to main |
| **Spec** | [Lane Status Surface RFC 0001](https://github.com/prestonbrown/lane-status-spec) (draft 0.1): `lane_data` is the identity half, one writer per machine, tracker wins on identity, producer wins on presence |
| **Research** | Code read at land/beta18 `45e0c6567f`; AFC behaviour checked on the Voron (`AFC.py` deletes `lane_data` at boot, `AFC_lane.py#send_lane_data` republishes it) |

## Problem

HelixScreen keeps its own copy of what is in each lane and paints that copy over what the
printer reports. The copy lives in five places:

1. the in-memory lane source store (`lane_source_store.cpp`), where a user edit (`LocalUser`), a
   Spoolman lookup (`Spoolman`) and an old record (`Remembered`) all outrank the firmware's own
   reading (`lane_resolver.cpp#resolve`);
2. private Moonraker namespaces for AFC and Happy Hare (`helix-screen-afc-overrides`,
   `helix-screen-hh-overrides`), read once at start;
3. a local file cache with queued "held" writes (`filament_slot_overrides.json`);
4. ToolState's per-tool spool ids (`tool_spools.json` plus `helix-screen/tool_spool_assignments`),
   copied back onto lanes by `AmsState::sync_tool_spools`;
5. the bypass spool in `settings.json` (`filament/external_spool/*`).

Each copy is reconciled with the printer by rules that exist only for that purpose: eject
retention and its re-assert, echo suppression, outside-edit precedence, legacy authorship keys,
held-write merging, three re-read paths, tool changer re-layering. Every reconciliation gap is a
place where HelixScreen shows one thing and Mainsail, OrcaSlicer or a second HelixScreen shows
another. The Voron BoxTurtle eject (2026-11-06) is one of them.

## Principle

**A lane shows what Moonraker holds, and nothing else.** Moonraker holds it in two halves:

| Half | Where | HelixScreen's role |
|---|---|---|
| Live state: presence, active lane, errors | Klipper objects | Read only (subscription) |
| Identity: material, colour, spool, temps, brand, name, weights | Firmware fields where the firmware has them; otherwise Moonraker's `lane_data` record | Read; write only through the firmware's commands or, where HelixScreen is the machine's `lane_data` writer, the record |

Spoolman details (brand, name, weights, temps) for a linked spool are a lookup keyed by the id
the firmware or record holds, never a competing source: when the link changes, they change.

A user edit is a write. Until its echo arrives the lane shows it as pending; the echo then *is*
the value. A failed write is an error on screen, never a local copy.

## Who owns each lane, by backend

"Writer" is the RFC's `lane_data` writer for that machine. HelixScreen writes `lane_data` only
where it is the writer, and never overwrites a record whose `writer` names another live producer
(RFC S2).

| Backend | Identity the firmware holds | `lane_data` writer | HelixScreen writes an edit as |
|---|---|---|---|
| AFC | spool id, material, colour, weight, temps, vendor, name (`AFC_stepper`) | AFC | `SET_SPOOL_ID`, `SET_COLOR`, `SET_MATERIAL`, `SET_WEIGHT`, `SET_MAP`; nothing else |
| Happy Hare | spool id, material, colour, vendor (v4), name, temp (`mmu` gate map) | Happy Hare | `MMU_GATE_MAP` incl. `NAME=` (today missing) |
| CFS (stock) | material code, colour, tag spool id | HelixScreen | `BOX_MODIFY_TN_DATA` for colour/material (incl. on Clear); the rest in the record |
| CFS (Kalico fork) | material, colour, brand, name, spool id | HelixScreen | `_BOX_SLOT_SET` / `_BOX_SLOT_CLEAR` |
| AD5X IFS / Z-Mod | colour, type (`Adventurer5M.json`, `_IFS_VARS`, `IFS_SET_MATERIAL`) | HelixScreen | firmware for colour/type; the rest in the record |
| Z-Mod afclite (Creator 5) | colour, material (`CHANGE_ZCOLOR`) | afclite | firmware; already the model (`firmware_stores_color_and_material`) |
| QIDI Box | material, colour, vendor ids (`save_variables`) | HelixScreen | `SAVE_VARIABLE`; colour snapped to the palette before saving, and the snapped colour shown |
| Snapmaker U1 | type, vendor, subtype, colour (`print_task_config`), tag (read only) | HelixScreen | `SET_PRINT_FILAMENT_CONFIG`; spool id, name, weights in the record |
| ACE (ACEPRO) | colour, material, temps (hub) | HelixScreen | `ACE_SET_SLOT`; the rest in the record |
| ACE (other drivers), MultiACE | colour, material read only | HelixScreen (MultiACE: its engine) | the record (MultiACE: the engine's slot override) |
| OpenAMS | nothing | `[openams_spoolman]` when present, else HelixScreen | the record, when HelixScreen is the writer |
| Tool changer, MedusaHC, INDX | nothing (INDX: `T<n>.spool_id` macro variable) | HelixScreen (Mainsail's legacy `T<n>` records are replaceable) | the record `T<n>`; INDX also `SET_GCODE_VARIABLE MACRO=T<n> VARIABLE=spool_id` |

Firmware fields are never declared or stored by HelixScreen.

**Fields the firmware cannot hold, where another program writes `lane_data`.** AFC has no
command for brand or name (they arrive only through `SET_SPOOL_ID` from Spoolman), and it
deletes and republishes `lane_data` at every boot. Happy Hare before v4 has no vendor field.
For these two, HelixScreen keeps its existing namespace (`helix-screen-afc-overrides`,
`helix-screen-hh-overrides`), narrowed to the fields the firmware cannot hold: brand, name,
colour name, catalog pick, total weight. It is read by the same poll as `lane_data`, so every
HelixScreen on the printer agrees. Any spool id, material or colour in an old record is ignored
on read (the firmware's value wins), so beta entries survive the upgrade without a migration.
A linked spool's Spoolman details fill the same fields when the namespace has none.

## Records HelixScreen writes

Where HelixScreen is the writer, each record follows RFC 0001 §5 and REFERENCE.md Part B §7:
`lss_version`, `writer: "helixscreen"`, `lane` (string), `lane_name`, `loaded`, `material`,
`color`, temps as numbers, `vendor_name`, `name`, `spool_id`, `filament_id`, weights, and
HelixScreen-only keys prefixed `helix_`. Unknown is `null`, never a placeholder. One record per
lane, plus the bypass lane (below).

## Eject and swap

- Firmware that holds identity decides what an eject keeps: AFC `remember_spool`, Happy Hare's
  gate map, the ACE hub, CFS `tn_data`, Z-Mod's file. HelixScreen shows the result.
- AFC's "Keep Spool Info on Eject" switch is AFC's `remember_spool` (`AFC_SET_REMEMBER_SPOOL`),
  as shipped in beta.18.
- Where the record is the only identity (OpenAMS without its writer, tool changers, MedusaHC,
  INDX, ACE without ACEPRO), HelixScreen's "Keep Spool Info on Eject" setting decides whether
  the record keeps its identity when the lane empties. Either way the record's `loaded` becomes
  `false`. Kept identity with `loaded: false` departs from the RFC's "empty lane keeps its keys
  with null values" SHOULD; the record states the deviation through `loaded`.
- The setting is shown only on those backends.

## Reading: one path, polled

- One `refresh_lane_records()` replaces `request_resync`, `reread_lane_records` /
  `apply_reread_lane_records` and `read_unread_lane_records` / `adopt_unread_lane_records`.
- It runs at connect, after each of HelixScreen's own writes, and every 10 seconds (Moonraker
  sends no database-change notification). One `server.database.get_item` per namespace per poll
  (`lane_data`, plus the AFC/HH extras namespace on those backends).
- Records are matched by their `lane` field, not the key (RFC S3), and legacy records (no
  `writer`) are read.

## Spoolman

- The linked id comes from the firmware or the record. Spoolman details are fetched for that id
  and fill fields the firmware and record leave empty.
- Linking a spool writes the link where the lane's identity lives (`SET_SPOOL_ID`,
  `MMU_GATE_MAP SPOOLID=`, the record), not into ToolState.
- `fetch_linked_slot` and `fetch_linked_external_spool` become one fetcher: the bypass is a lane.

## Tools

ToolState stops holding spool identity. A tool's spool is the spool of the lane that feeds it.
`tool_spools.json`, `helix-screen/tool_spool_assignments`, the reverse sync in
`AmsState::sync_tool_spools` and `sync_external_identity` on every backend go. The
auto-assign-active-spool path (`spoolman_active_spool_sync.cpp#try_assign_active_spool_to_tool`)
writes the lane's link.

## Bypass spool

The bypass spool is a lane record (`lane_name: "bypass"`) on every backend that has a bypass,
written by HelixScreen and read back like any other record. `settings.json`
`filament/external_spool/*` and the in-memory dual store go.

## Other local state

- CFS `ams/bypass_declared`: read `box.enable` instead; the setting goes.
- Happy Hare `/hh_overrides/*` tuning values: read back from `mmu` where it reports them; the
  rest keyed per printer. (Tuning, not identity; included because it is the same class of bug.)
- `afc_message_dedup.json` and `user_filaments.json` stay local: they are UI state and catalog
  presets, not lane state.

## Offline

HelixScreen reaches the printer only through Moonraker, so Moonraker down is printer
disconnected, which the app already shows. There is no local lane state: the cache file and the
held-write queue go (this deletes the #2021 held-edit work). While connecting, lanes show the
live Klipper state and fill in identity when the records answer. Lane editing is disabled while
disconnected. A write that fails while connected is reported and not queued.

## Deleted

Targets from the research, read at `45e0c6567f`; line counts are estimates from function lengths.

| Area | What goes | ~Lines |
|---|---|---|
| Override store | legacy migrations (`try_migrate_legacy`, `try_migrate_lane_keys_to_tool_keys`; both shipped in v1.0.0, so every 1.0 install already ran them), local cache read and write, held writes (`merge_held_save`, `held_write_conflicts`, `persist/restore_held_locked`, `send_persisted_held_writes`), firmware fields in the AFC/HH namespaces | 700 |
| Lane model | `Remembered` and the persistent `LocalUser` rung; `outside_edit_wins`, `amend_authorship`, `declared_fields_on_load`, `wire_authored_by_helix`, `file_outside_edit_if_newer`, `helix_declared` / `helix_locked_*`; binding Rule 1 + Rule 2 become "the firmware's or record's id is the link" | 350 |
| Echo suppression | `lane_echo.cpp`, `own_write_echoes_` staging in AFC, HH, ACE, CFS, QIDI, IFS; `record_own_spool_write` / `own_write_expectation`; CFS `pushed_material_codes_`, `rfid_tracker_.expect_any_of`; firmware shadow copies (`lane_firmware_readings_`, `lane_firmware_spool_id_`, `gate_readings_`). Replaced by one pending-write overlay | 400 |
| Retention | `maybe_reassert_retained_spool_link`, unlink bookkeeping (`set_unlinked_spool`, `helix_unlinked_spool_id`), U1 `record_restores_locked`, `retract_unbacked_identity_locked` | 250 |
| ToolState identity | spool assignment, its file and key, reverse sync, `sync_external_identity` x N | 430 |
| Bypass spool | settings keys, dual store, `ams_state_external_spool.cpp` shrinks to the lane path | 240 |
| Duplicates | per-backend `persist_override`, `write_lane_locked`/`write_gate_locked`, `persist_external_identity_impl`, `persist_slot_weight` store half, `publish_external_spool_lane`, three `on_started` store loads, `clear_override_fields` vs `reset_lane_to_machine_readings`, the tool changer three-way re-layer: one implementation each in `AmsSubscriptionBackend` | 300 |
| Already dead | OpenAMS echo check, AFC `read_vendor` `vendor` branch, ACE SKU block, Snapmaker `request_resync` path, stale `lane_legacy_migration.h` comment | 30 |

**Target: about 2,000 production lines removed**, with the tests that pin deleted machinery
deleted beside them. The branch must end smaller than it started; a phase that grows the code
says why in its commit.

## Phases

Each phase is a branch off the previous one, lands its own tests, and leaves every backend
working. Tags run per phase; one review and one `land-gate` at the end.

1. **Lane model.** The resolver becomes firmware, then record, then Spoolman fill, plus the
   pending-write overlay. Delete `Remembered`, the persistent `LocalUser` rung and the
   binding/retention rules. Generic code only; backends keep their write paths.
2. **Record store.** `FilamentSlotOverrideStore` becomes a `lane_data` reader and writer:
   RFC-shaped records, the writer rule, one polled refresh, no cache, no held writes, no
   migrations; the AFC/HH namespaces narrowed to fields the firmware cannot hold.
3. **Backends, write-through.** Per backend (parallel agents, one worktree each): edits written
   where the table above says, firmware fields never stored, echo suppression replaced by the
   overlay, shared helpers moved into `AmsSubscriptionBackend`.
4. **Tools, bypass, settings.** ToolState identity, the bypass lane, `bypass_declared`,
   `hh_overrides`, the keep-spool setting's scope.
5. **Sweep.** Dead code the earlier phases exposed, docs (`FILAMENT_SLOT_METADATA.md`,
   `FILAMENT_MANAGEMENT.md`, the backend leaves, `docs/specs/filament_slots.md` aligned with the
   RFC), user docs.

## Tests

- **Divergence suite (new, one file per backend family):** for each row of the research
  tables, drive the backend with the firmware frames and `lane_data` record a real printer
  produces, then assert HelixScreen's lane equals what Moonraker holds. Cases: eject with and
  without retention, Mainsail unlink, Mainsail colour/material edit, a second client's record
  edit picked up by the poll, a failed write, a Spoolman link, a tool changer tool's spool.
- **Writer rule:** HelixScreen never writes `lane_data` on AFC or Happy Hare, and never
  overwrites a record another live writer owns.
- **Record shape:** records HelixScreen writes validate against RFC 0001 §5.
- **Deleted machinery:** its tests go with it; the test budget falls.
- **Hardware:** the Voron BoxTurtle (AFC) for the eject case; the mock for the rest; device
  checks per backend where a printer is reachable.

## Decisions (Preston, 2026-11-07)

- 1.1 scope, not 1.2; delete dead code, merge duplicates, simplify.
- Fields AFC/HH firmware cannot hold: keep and narrow their HelixScreen namespaces, read live.
- Second-client changes: poll every 10 s.
- Keep Spool Info on Eject: kept for backends whose only identity is the record.
- Offline: no local cache, no held-edit queue.
