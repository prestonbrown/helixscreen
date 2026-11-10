# Filament Slot Metadata: Implementation Notes

HelixScreen-internal notes on how a lane's record is read from and written to
Moonraker's database. For the wire format a third-party reader or writer needs
(the `lane_data` namespace), see [`../specs/filament_slots.md`](../specs/filament_slots.md).
This doc describes the code that puts records there and reads them back, and
assumes familiarity with the public spec.

---

## 1. Overview

A lane shows what Moonraker holds. HelixScreen keeps no local copy of any lane
record: there is no cache file, no queue of writes waiting for a connection and
no migration of older formats. Each backend reads its lane records from
Moonraker's database, files them on the lane model as `Record` observations, and
writes a user's edit back as a whole record. What the last read returned is all
the app knows about a lane's record.

Records follow RFC 0001 §5 (the Lane Status Surface): every RFC key is present,
an unknown value is `null` rather than a placeholder, and keys only HelixScreen
reads carry the `helix_` prefix. OrcaSlicer 2.3.2+ reads the same namespace for
"Sync filaments from Printer", so a slot edit reaches the slicer with no extra
configuration. The flow is one-directional: OrcaSlicer never writes `lane_data`.

**Two-string material identity.** OrcaSlicer matches a lane to a preset by the
`material` string alone, and an unmatched string resolves to a Generic PLA preset
(wrong temperatures), not a near miss. So `to_lane_data_record()`
(`src/printer/filament_slot_override_store.cpp#to_lane_data_record`) emits
`material` as a slicer-matchable string from `filament::orca_match_type()`
(`src/printer/filament_variants.cpp#orca_match_type`): explicit
`orca_type_overrides` entry, then exact `orca_library_types` entry, then the base
polymer with the type's one filler (`PA12-CF` to `PA-CF`), then the
`extract_base_material()` base polymer, and `null` when nothing matches. It writes
the precise identity (`ASA-GF`, `PLA Silk`) as `filament_type`.
`from_lane_data_record()` reads `filament_type` first and the legacy
`helix_material` after, so HelixScreen's own screen keeps the exact type even
though Orca sees the reduced `material`. The library-type set and override table
are generated into `assets/filaments.json` by `scripts/import_orca_filaments.py`
and pre-warmed on the main thread at startup (`filament::warm_orca_tables()`), so
the first lookup never parses the asset on a WebSocket background thread. See
[`FILAMENT_MANAGEMENT.md`](FILAMENT_MANAGEMENT.md) § "Two-string identity" for the
full resolution order.

**What OrcaSlicer reads.** Orca's `MoonrakerPrinterAgent` (2.3.2 through
2.5.0-alpha) reads five fields per record: `lane` (a string, 0-based tool index),
`color`, `material`, `bed_temp` and `nozzle_temp`, and picks the first visible
preset whose `filament_type` equals `material`. The library-type table is
regenerated with `make regen-filaments ORCA_TAG=<tag>`, and a newer tag can add
types older Orca releases lack (2.5 adds `PLA Silk`, `PETG HF`, `PA6`, `PE-CF`,
`TPU-AMS`). Mapping a precise type onto one of those makes it match on the new
release and stop matching on the old ones, so `orca_type_overrides` targets only
types every supported Orca release carries.

---

## 2. The record

`to_lane_data_record(slot, override, style)` writes one RFC 0001 §5 record:

| Key | Value |
|-----|-------|
| `lss_version` | `"0.1"` (`kLaneDataLssVersion`) |
| `writer` | `"helixscreen"` (`kLaneDataWriter`) |
| `lane` | the 0-based slot index as a string (Orca drops a numeric lane) |
| `lane_name` | the outer key (`laneN` or `T<n>`, below); `"bypass"` on the bypass spool's record |
| `loaded` | bool, or `null` |
| `color` | `"#RRGGBB"`, or `null` when `color_set` is false (pure black is a colour) |
| `material`, `filament_type` | the Orca match string and the precise type |
| `vendor_name`, `name` | brand and product name |
| `spool_id`, `spoolman_filament_id`, `filament_id` | numbers or `null`; `filament_id` is the linked spool's Orca id, else one the catalog records for the picked product |
| `bed_temp`, `nozzle_temp` | numbers resolved from the material at write time, or `null` |
| `remaining_weight_g`, `total_weight_g` | numbers or `null` |
| `scan_time` | `null` |
| `helix_color_name`, `helix_catalog_id`, `helix_product_name`, `helix_spoolman_vendor_id`, `helix_fingerprint`, `helix_unlinked_spool_id`, `helix_filament_id_source` | HelixScreen's own keys, omitted when empty |

`from_lane_data_record()` reads the RFC spellings first and the legacy ones
(`vendor`, `spool_name`, `helix_material`, a numeric `lane`) after, so a record
written by an older HelixScreen, AFC, Happy Hare or Mainsail still reads. A
record with no valid `lane` field does not parse, and costs its slot only.

### Outer key style

The outer Moonraker DB key is not always `laneN`. `format_lane_key()` picks
between two shapes with different bases, and the store's style comes from
`lane_key_style_for(AmsType)` (`include/filament_slot_override_store.h#lane_key_style_for`):

| Style | Key | Base | Used by |
|-------|-----|------|---------|
| `LaneKeyStyle::Lane` | `laneN` | 1-based (`lane1` is slot 0) | every filament-switching system: AFC, Happy Hare, ACE, CFS, IFS, QIDI, OpenAMS |
| `LaneKeyStyle::Tool` | `T<n>` | 0-based (`T0` is slot 0) | tool changers: Snapmaker, generic `TOOL_CHANGER` |

The inner `lane` field is 0-based in both, so the offset between the outer key
and the inner field is deliberate for `laneN` and absent for `T<n>`. Records are
matched to slots by the inner `lane` field, never the key (RFC 0001 §3):
`parse_namespace_document()` (`src/printer/filament_slot_override_store.cpp#parse_namespace_document`)
parses a whole namespace, and when two records name one slot the one under the
store's canonical key wins. Non-object siblings (an AD5X `seated` scalar an older build wrote) are
skipped.

---

## 3. The store

| File | Role |
|------|------|
| `include/filament_slot_override.h` | `FilamentSlotOverride`, one record's fields, and `user_override_from_slot_info()` / `stage_user_override()`, which turn an edited `SlotInfo` into the record to write |
| `include/filament_slot_override_store.h` | `FilamentSlotOverrideStore`, the wire shape (`to_lane_data_record`, `from_lane_data_record`, `extras_only`, `parse_namespace_document`), the shared persist tail and the fingerprint tracker |
| `src/printer/filament_slot_override_store.cpp` | the implementation |
| `include/ams_subscription_backend.h` | `open_lane_record_store()`, `refresh_lane_records()` and the persist helpers every backend calls |

`FilamentSlotOverrideStore` (`include/filament_slot_override_store.h#FilamentSlotOverrideStore`)
reads and writes one backend's records:

| Method | Purpose |
|--------|---------|
| `reload_async(cb, on_failure)` | One `server.database.get_item` for the whole namespace. `cb` receives the parsed records (narrowed by `extras_only()` on an extras namespace); a namespace nothing has written yet is an empty read. A failed read calls `on_failure`. Callbacks capture nothing of the store |
| `adopt_read(records)` | Remember each slot's `writer` from the last read, for the writer rule. Main thread |
| `save_async(slot, record, cb)` / `clear_async(slot, cb)` | Write or delete one slot's record. Refused with an error under the writer rule. On success the store calls the owner's `set_on_written()` callback, which re-reads |
| `save_key_async` / `clear_key_async` / `load_key_async` | HelixScreen's own plain values, in the `helix-screen` namespace rather than among the lane records (whose every value OrcaSlicer reads as a record): the CFS bypass declaration (`cfs_bypass_declared`) |
| `save_seated_slot_async` / `clear_seated_slot_async` / `load_seated_slot_blocking` | AD5X IFS's seated lane (`<backend>_seated` through the key calls above): the 0-based lane loaded to the toolhead; a `seated` scalar left in `lane_data` moves there on load |

**The writer rule** (RFC 0001 §2). A record whose `writer` names another producer
is never overwritten or deleted: `writer_refusal()` checks the writer the last
read saw for that slot, and `save_async` / `clear_async` answer an error instead
of sending. A record with no `writer` is legacy and replaceable.
`written_by_another_producer()` is the same test on a wire record.

Each backend owns one store, keyed by its `backend_id`:

| Backend | `backend_id` | Namespace |
|---------|--------------|-----------|
| `AmsBackendAd5xIfs` | `ifs` | `lane_data` |
| `AmsBackendSnapmaker` (and `AmsBackendMultiAce`) | `snapmaker` | `lane_data` |
| `AmsBackendAce` | `ace` | `lane_data` |
| `AmsBackendCfs` | `cfs` | `lane_data` |
| `AmsBackendQidi` | `qidi` | `lane_data` |
| `AmsBackendToolChanger` | `toolchanger` | `lane_data` |
| `AmsBackendOpenAms` | `openams` | `lane_data` |
| `AmsBackendAfc` | `afc` | `helix-screen-afc-overrides` (extras) |
| `AmsBackendHappyHare` | `happyhare` | `helix-screen-hh-overrides` (extras) |

### Extras namespaces: AFC and Happy Hare

AFC and Happy Hare own `lane_data`: their Klipper plugins write it themselves (AFC
wipes the whole namespace at the start of every PREP and refills it a lane at a
time), so HelixScreen never writes there on those printers and keeps its own
values in a private namespace (`kOverrideNamespace`, prestonbrown/helixscreen#1158).
A store on any namespace other than `lane_data` holds extras only
(`holds_extras_only()`): `extras_only()` (`src/printer/filament_slot_override_store.cpp#extras_only`)
keeps what the firmware cannot hold, which is brand, name, colour name, catalog
pick and total weight, plus HelixScreen's bookkeeping. A spool id, material or
colour in an extras record is ignored on read and never written, because the
firmware's value is the lane's: AFC and Happy Hare state those through their own
status, filed as `Firmware`.

---

## 4. Reading: `refresh_lane_records()`

`AmsSubscriptionBackend::refresh_lane_records()`
(`src/printer/ams_subscription_backend.cpp#refresh_lane_records`) is the one read
path. It runs:

- at connect: `open_lane_record_store()` (`src/printer/ams_subscription_backend.cpp#open_lane_record_store`)
  builds the store from a backend's `on_started()` and reads at once;
- after each write of ours: the store's `set_on_written()` callback defers a
  refresh, so the lane shows the echo;
- every `kLaneRecordPollPeriod` (10 s): Moonraker sends no database-change
  notification, so a record another client rewrote shows on the next poll. The
  timer is an `LvglTimerGuard` (`lane_record_poll_`) created in `start()` and reset
  in `stop()` and the destructor; the poll first withdraws expired Pending edits
  (`poll_lane_records()`), then refreshes;
- on a user resync (`request_resync()`), a Klipper restart, and from a backend
  whose parse saw a lane restated (`schedule_lane_refresh()`, safe from any thread).

One database get per refresh. A read already in flight takes the request with it,
and one more read follows when it lands (`refresh_in_flight_` / `refresh_again_`).
The reply is deferred to the main thread through the backend's lifetime token.

`adopt_lane_records()` (`src/printer/ams_subscription_backend.cpp#adopt_lane_records`)
then, under `mutex_`, hands the read to the store (`adopt_read()`), replaces
`overrides_`, and calls `on_lane_records_read_locked()`, the hook for a backend
that keeps state derived from the records (CFS, Snapmaker and QIDI seed their
fingerprint baselines there). Outside the lock it files every record whole as its
lane's `Record` observation (`record_observation()`, `src/printer/lane_translation.cpp#record_observation`),
drops the `Record` of a slot whose record vanished, and repaints every slot and
emits `EVENT_STATE_CHANGED` when the records changed since the last read, a
Pending edit was echoed, or this is the store's first read (sources filed before
it, such as a Spoolman fetch, were never painted). A read for a store a reconnect
has since replaced is dropped. The bypass spool's record (below) is not a slot's
record: the primary backend (index 0) files it on the bypass lane instead.

A failed read is logged at debug and changes nothing; the next poll retries.

### The bypass spool's record

The bypass (external) spool is a lane record like any other, told apart by
`lane_name: "bypass"` rather than by its index (`helix_external: true` on a
1.1 beta record reads the same way). There is no other store: no settings key,
no in-memory copy.

- **Where it lives.** The primary backend holds it when it answers
  `AmsBackend::holds_bypass_record()` (every `AmsSubscriptionBackend`): in its
  own lane-record store at `bypass_lane_index_locked()`, one past its lanes by
  default (CFS: the Fork's external slot, or 16 on stock firmware). On AFC and
  Happy Hare that store is the extras namespace, so HelixScreen still never
  writes `lane_data` there; `extras_only()` keeps a bypass record whole, since
  no firmware holds the bypass spool. With no such backend (no filament system,
  or the mock), AmsState keeps it in `lane_data` at lane 0 (`lane1`) and reads
  it itself: at connect, after each write, and every 10 seconds.
- **Reading.** Either read path files the record as the `Record` source on
  `kBypassLaneId` (`file_bypass_record()`).
  `AmsState::raw_external_spool_info()` is that record with any pending edit
  laid over it; `get_external_spool_info()` adds the Spoolman details and the
  meter, which rank above the record's weights.
- **Writing.** `AmsState::set_external_spool_info()` files the edit as the
  bypass lane's Pending source and writes the whole record
  (`external_lane_record()`); the read that follows echoes it, and a failed
  write withdraws it. `AmsSubscriptionBackend::publish_external_spool_lane()`
  is the one writer on the backend side: it deletes a record of ours left at an
  earlier index and never overwrites a slot's record at the index.

### Editing waits for the first read

`AmsBackend::lane_records_ready()` (`include/ams_backend.h#AmsBackend/lane_records_ready`)
is false on a subscription backend from the moment its store opens until the
first read lands; a failed read leaves it false. A backend that never opened a
store is always ready. An edit made before the read would overwrite records nobody
has seen, and nothing is held to send later, so editing waits:

- `AmsState` publishes `ams_lane_records_ready` (`get_ams_lane_records_ready_subject()`),
  1 once every backend is ready over a live connection, from each
  `sync_from_backend()` and on the connection-lost edge.
- The context menu's **Edit Spool** button binds its disabled state to it
  (`ui_xml/ams_context_menu.xml`), and both the menu and the spool editor show an
  inline "Loading lane details…" line while it is 0.
- The editor's Save is disabled by an `action_button_disabled_cond` in
  `ui_xml/ams_edit_overlay.xml` over the editor's own dirty gate
  (`ams_edit_save_disabled`) and the ready subject. It is a view-time `cond`
  because both subjects come from `init_subjects()`, after the component
  registers (`docs/devel/LVGL9_XML_GUIDE.md` § "Which phase registers the subject").
- `AmsState::commit_slot_edit()` (`src/printer/ams_state.cpp#commit_slot_edit`)
  refuses an edit that reaches it another way: with no printer connection it
  answers `AmsErrorHelper::not_connected()`, and before the read a
  `NOT_CONNECTED` error saying the lane details are still loading.

---

## 5. Writing: Pending edits

`AmsBackend::commit_user_edit()` (`src/printer/ams_backend.cpp#commit_user_edit`)
is every editor's commit. It files the edit on the lane as `Pending`
(`file_pending()`, `include/lane_source_store.h#file_pending`) before
`apply_user_edit()` writes it, so the lane shows the edit at once. The backend's
`apply_user_edit()` stages the record (`persist_user_record_locked()`, which runs
`stage_user_override()`) and saves it, and writes firmware where the machine
holds the field.

`user_edit_observation()` (`src/printer/lane_translation.cpp#user_edit_observation`)
builds the Pending observation from the two editor snapshots: only fields the
edit moved are engaged, so values seeded into the editor from firmware are not
restated (#965). `spoolman_id` is engaged exactly when the binding changed. On a
Spoolman-linked lane the spool's own identity is not the person's to move
(`keep_spool_owned_identity()`); a changed binding first drops what described the
previous spool (`drop_previous_spool_declarations()`).

A Pending value is withdrawn by whichever comes first:

| Event | Where |
|-------|-------|
| **Echo.** A `Firmware` or `Record` reading states the same value | `ingest()` strips the echoed fields (`strip_echoed_fields()`, `include/lane_source_store.h#strip_echoed_fields`). Weights compare within 0.1 g, colours on their RGB. A cleared Pending field is echoed by a `Record` that leaves the field unset, since a record states "none" as `null`; firmware silent on a field is no echo, because most firmware never holds it |
| **Write failure.** The record write fails, or the firmware refuses the command | `save_override_async()` withdraws the lane's Pending and logs at warn; AFC withdraws on a refused `SET_COLOR` / `SET_MATERIAL`. No toast: the spool editor, which waits on the lane (`await_pending()`), stays open with "Not saved: <reason>. Try again." under Save and keeps the user's values. The store's `set_on_write_failed()` hook repaints the lane |
| **Refusal.** `apply_user_edit()` fails outright | `commit_user_edit()` withdraws at once and returns the error; a partially applied edit keeps what reached firmware |
| **Timeout.** Neither echo nor failure within `kPendingWriteTimeout` (30 s) | `expire_pending()`, run by the poll and lazily on read, withdraws it with a warn log; the lane shows what Moonraker holds. No popup |

**Clears.** A cleared text field or zeroed id travels engaged-empty, and filing it
withdraws the field's earlier Pending value instead of standing as an empty one:
the clear hands the field back to what Moonraker holds at once
(prestonbrown/helixscreen#1661). Two clears stay engaged: an unlink
(`spoolman_id` 0), which must displace the link Moonraker still holds until the
write lands, and a colour name cleared beside a new colour, which must displace a
name describing the old colour. An edit that only clears leaves an empty Pending record: it
shows nothing, but stands for the write, so a waiter still hears the outcome.
Only a `Record` reading (the read after the write) answers it; firmware frames
arrive whether or not the write landed.

**Offline.** An edit while disconnected is refused before anything is filed
(§4). A write that fails is reported through the log and the lane falls back; it
is never queued.

### The shared persist tail

| Helper | Use |
|--------|-----|
| `save_override_async(store, slot, record, tag, noun, lane)` | Save, and on failure withdraw `lane`'s Pending |
| `persist_staged_override(...)` | Snapshot `overrides_[slot]` under the backend mutex and save it |
| `AmsSubscriptionBackend::persist_override(slot, noun)` | The same from a backend, taking `mutex_` |
| `AmsSubscriptionBackend::persist_user_record_locked(slot, info[, material])` | Stage an edited `SlotInfo` as the slot's record and save it |
| `persist_override_weight()` | Put a metered weight on the record |
| `persist_override_external_identity()` | Amend a linked slot's record with what Spoolman now states (colour only when the record has none) |
| `mirror_firmware_to_lane_data(..., MirrorPolicy)` | Mirror firmware-detected colour and material into the record on backends whose firmware does not publish `lane_data` (IFS and Snapmaker `OverwriteAlways`, CFS `FillUnsetOnly`) |
| `clear_persisted_override()` | Erase a slot's record in memory and on the printer |

---

## 6. Per-backend integration

| Backend | Parse hook | Hardware-event signal | Record-only fields |
|---------|------------|-----------------------|--------------------|
| `AmsBackendAd5xIfs` | `update_slot_from_state` then `apply_resolved_lane` | `Adventurer5M.json` colour or type change on a present lane (`check_external_color_change`) runs the mirror | brand, spool name, Spoolman ids, weights, colour name |
| `AmsBackendSnapmaker` | tail loop of `handle_status` | `filament_detect.info[ch].CARD_UID`, canonicalized | spool name, Spoolman ids, remaining weight |
| `AmsBackendMultiAce` | `rebuild_units_locked` per-bay loop | heads: the base's `CARD_UID` path; bays: the engine table's uid, else material plus colour, through the same tracker | spool name, Spoolman ids, remaining weight |
| `AmsBackendAce` | `parse_ace_object` per-slot loop | status EMPTY/UNKNOWN to present | brand, spool name, Spoolman ids, weights, colour name |
| `AmsBackendCfs` | `handle_status` tail loop | composite `material_type\|color_value` fingerprint | spool name, Spoolman ids, remaining weight |
| `AmsBackendToolChanger` | `handle_status` tail loop and `initialize_tools()` tail | none (below) | every field |
| `AmsBackendAfc` | `parse_afc_stepper` | AFC's own firmware clears | brand, name, colour name, catalog pick, total weight (extras) |
| `AmsBackendHappyHare` | `gate_spool_id` loop in `handle_status` | the gate's spool id moving drops the departed spool's weights (`retire_departed_weights_locked()`); the frame's repaint drops its record-only fields | brand, name, colour name, catalog pick, total weight (extras) |

"Record-only fields" are the fields a user can edit on that backend but the
firmware never supplies: they come from the `Record`, a Pending edit or Spoolman,
never from firmware.

### Tool Changer is the odd one out

Every other backend has something the machine reports. klipper-toolchanger does not:

- **The record is the sole source of filament identity.** `parse_tool_state()`
  reads `mounted` and `active` and nothing else, so every identity field comes
  from the record.
- **There is no hardware-event clearing.** Nothing on a tool changer can tell
  that a user swapped a spool: no RFID, no presence transition, no colour reading.
  Inventing a clear signal would throw away user data on an event that does not
  mean what it would have to mean. The user's Clear Spool drops the record, the
  only record there is.
- **`initialize_tools()`** resets every slot to `kAmsDefaultSlotColor` with the
  tool name as a placeholder `spool_name` on every `set_discovered_tools()`, so it
  ends by laying the resolved lane back on (`apply_resolved_lane`), and the store's
  first read repaints every slot.
- **`T<n>` outer keys** are shared with Mainsail #2510's records rather than
  duplicating them.

---

## 7. Tag readings

A backend with a tag reader (an RFID chip, or an inline colour scanner) answers
`AmsBackend::tag_reading(slot)` with what the tag reported for the spool now in the
slot, as a `helix::ams::TagReading` (`include/ams_tag_reading.h`): material, brand,
product, colour with any multi-colour, nozzle min/max, bed temperature and total
weight, each optional. Every other backend answers `nullopt`. The reading is the
tag's own, kept while a user's edit overrides it.

`AmsBackend::tag_provenance(slot)` compares `get_slot_info()` against the reading:
a field is from the tag when the tag states it and the slot shows that value (text
case-insensitive; colour needs the multi-colour to match too).
`helix::ams::reset_to_tag(slot, reading)` is the edit that restores it, which also
clears the colour name and a catalog pick the tag's identity no longer matches.

| Backend | Reading | How it is kept |
|---|---|---|
| Snapmaker U1 | `filament_detect.info` with a real `CARD_UID` | the firmware keeps it (edits go to `print_task_config`) |
| CFS | the box's latched record plus the catalog temps for its code; a Spoolman id from the stock record's serial | `keep_tag_reading()`; the id is re-read each frame |
| QIDI Box | the saved RFID ids resolved through the Box tables, plus the profile temps | `keep_tag_reading()` |
| ACE | a frame whose `rfid` says identified | per bay, until a read finds no tag or the bay empties |
| AFC | the lane's `td1_color` scan | `SlotInfo::scanned_color_rgb` |

`helix::ams::keep_tag_reading()` (`src/printer/ams_tag_reading.cpp#keep_tag_reading`)
keeps what one frame of a box's tag reader states as that slot's reading, with
the nozzle range the backend's tables give the material; the slot's reading is
erased when the frame states nothing. A value typed on the printer's own screen
is indistinguishable from a tag read.

A user override survives the tag reader's next poll through the lane resolve: a
Pending edit outranks `Firmware`, and once echoed the edit is what firmware or the
record states. The multi-colour is the one tag field no lane observation carries,
so a backend lays it onto the slot only while the lane has no colour of its own,
and `SlotInfo::assign_filament_fields` carries it through an edit.

### Tag-declared Spoolman links

Some tags name a Spoolman spool. `TagReading::spoolman_id` carries it, and only a
backend's own decoder fills it: each tag format has values that name no spool (a
CFS stock tag's factory serial `000001`, a blank), and the decoder drops them, so a
set id is always one to look up. It is a binding, not a property the slot shows,
so provenance and `reset_to_tag()` ignore it. `AmsBackend::tags_name_spoolman_spools()`
says whether a backend's tags can carry one; it gates the setting's row and
nothing else.

The policy lives in `helix::ams::TagSpoolLinkTracker` (`include/tag_spool_link.h`),
a pure per-lane tracker driven by `AmsState::sync_tag_spool_link` on every
primary-backend sync. It acts on transitions of the id, never on a frame that
repeats it:

| Frame | Effect |
|---|---|
| lane reads empty | forget the lane's tag: whatever is seated next is a new insert |
| occupied, no id (unread, untagged, a default serial) | nothing |
| first id this session | link only a lane linked to nothing, so a link made before a restart stands; a spool Spoolman does not have is logged, not toasted |
| same id as last seen | nothing, so a spool picked while that tag stays seated is kept |
| a different id, or an id after empty | link the lane to it |

A wanted link waits across frames while Spoolman is unreachable and is dropped
when the tag changes or the lane empties. When Spoolman is available AmsState
looks the spool up (`get_spoolman_spool`, completion deferred to the main thread
through `async_lifetime_`), re-checks that the lane still wants it, and links
through `AmsState::link_spool_to_slot`, the same commit a QR scan uses. A spool
Spoolman does not have gets one warning toast. A lookup that gets no answer is
retried after `TagSpoolLinkTracker::kLookupRetryFrames` frames. The per-printer
setting `ams/tag_links_spoolman` (default on) settles every want without a lookup,
so turning it on never links a spool already seated.

The tracker lives for the session, so a restart cannot tell a spool that sat in
the lane from one put in while HelixScreen was off, and the first frame only links
a lane linked to nothing. A lane in an absent unit (`AmsSystemInfo::slot_absent()`)
is skipped: a box dropping off the bus is not a removal.

---

## 8. Clear semantics

Clear Spool erases what HelixScreen and the printer's firmware remember about that
slot; what is left afterwards is what the hardware can read right now. How close
each backend gets is set by what its firmware can be told to forget:

- **AD5X IFS, AFC, CFS on Kalico, Tool Changer and Happy Hare** reach it: the
  first four through write paths that cover the firmware-held fields (the tool
  changer's record is the only record there is), Happy Hare through one
  `MMU_GATE_MAP GATE=n MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1` that empties the
  gate map. In Spoolman pull mode the gate map is Spoolman's, so Happy Hare refuses
  the write (a partial failure naming Spoolman as the owner).
- **QIDI Box** writes `VALUE=0` to `filament_slot{n}` / `color_slot{n}` /
  `vendor_slot{n}` (row ids start at 1, so 0 reads as no identity), mid-print too,
  because `SAVE_VARIABLE` moves nothing. A tagged spool re-populates its ids on the
  next insert, boot or RFID read.
- **ACE, Snapmaker and stock CFS cannot** (read-only API, no empty spelling for a
  slot value, and the tag is re-read on the next probe), so their tag-derived and
  firmware-held values survive a clear.

The gesture refuses while its lane feeds an active print
(`clear_spool_blocked_by_print()` in `include/filament_op_slot_resolver.h`): the
job holds the machine and the lane is the one at the toolhead. The context menu
greys the button with the reason (`ams_slot_can_clear` / `ams_slot_clear_hint`);
the dispatch guard in `ams_dispatch_backend_action()` is the authority for a menu
rendered before the print started.

The paths that clear:

- **User-initiated clear.** The context menu's Clear Spool
  (`MenuAction::CLEAR_SPOOL` in `src/ui/filament/ui_ams_detail.cpp`) is a commit first and a
  clear second. `AmsState::commit_slot_edit()` carries the Spoolman active-spool
  unlink, the identity-cache invalidation and the ToolState clear, and hands the
  backend a slot with nothing on it; Happy Hare recognizes that shape
  (`is_full_clear()`) and answers with the gate-map wipe above. Only on the commit's
  success does the gesture call `AmsBackend::clear_slot_override(slot)`, which resets
  the lane to machine readings (`reset_lane_to_machine_readings()`: drops `Record`,
  `Pending`, `Spoolman` and `Metered`), erases the slot's record and fires
  `clear_async`. Colour and material are left to the parse, except on the tool
  changer, whose clear blanks the whole slot. AFC also empties them in firmware
  (`SET_MATERIAL LANE=x MATERIAL=`, `SET_COLOR LANE=x COLOR=`, `SET_WEIGHT LANE=x WEIGHT=0`).
- **Hardware-event clear.** Each backend watches its own signal (§6) and clears
  when it transitions to a different spool. The baseline is recorded on first
  observation after startup and never triggers a clear on its own. Backends on the
  fingerprint tracker seed that baseline from the record (below), so a swap made
  while HelixScreen was off is caught on the first frame after start.
- **Spoolman link moved by firmware.** On AFC and Happy Hare the firmware states
  the spool id, filed as `Firmware`, so the lane's link is whatever firmware says
  now; Spoolman's details count only for that id (`linked_spool_id()`,
  `src/printer/lane_resolver.cpp#linked_spool_id`), and `SpoolmanManager` drops a
  fetched record for any other id.

### Swap fingerprints across restarts

CFS (stock and flat schema), Snapmaker and QIDI Box run their hardware-event clear
through `SlotFingerprintTracker` (`include/filament_slot_override_store.h#SlotFingerprintTracker`).
Each observed identity string classifies as `NoSignal`, `Baseline`, `Unchanged`,
`OwnWriteEcho` or `Changed`, and only `Changed` clears the record. Before pushing a
user edit the backend arms `expect_any_of()` with the values the write should echo
back; a failed dispatch drops them (`forget_expected()`).

`bind_fingerprint_persistence()` ties the tracker to the records, called from each
of those backends' `on_lane_records_read_locked()`:

- **Seeding.** Every record with a `helix_fingerprint` seeds the baseline, so the
  first observation after start compares against the last fingerprint seen before
  shutdown.
- **Recording.** The baseline sink writes the current fingerprint onto the record
  on `Baseline`, `Unchanged` and `OwnWriteEcho`, never `Changed`.
- Records without the key seed nothing: first observation is `Baseline`, no clear.

---

## 9. Testing patterns

Tests live in `tests/unit/test_filament_slot_override_store.cpp`,
`tests/unit/test_lane_record_refresh.cpp`, `tests/unit/test_lane_pending.cpp` and
the per-backend `test_ams_backend_*.cpp` files.

- **Friend-class test access**: `Ad5xIfsTestAccess`, `SnapmakerTestAccess`,
  `AceTestAccess`, `CfsTestAccess`, `ToolChangerTestAccess` and
  `FilamentSlotOverrideStoreTestAccess` reach private hooks (`call_on_started`,
  `refresh_lane_records`) without a live Moonraker. Their `call_on_started` drains
  `UpdateQueue`, so the first read has landed when it returns.
- **Mock hooks for the database** on `MoonrakerClientMock`: `mock_db_set`,
  `mock_db_get`, and the generic `fail_next`, `defer_next` + `fire_deferred` /
  `fire_deferred_error`, and `call_count`. `fail_next("server.database.get_item")`
  fails a read; `fail_next("server.database.post_item")` fails a write.
- **Drain until quiet.** A write's success queues a read and the read's answer
  queues its filing, so a case drains `UpdateQueue` several times before asserting
  (`settle()` in `test_lane_record_refresh.cpp`).
- **Tags**: `[filament_slot_override]` covers the store; `[refresh]`, `[pending]`
  and `[ready]` the read path, the Pending overlay and the editing gate.

---

## 10. References

- **Public spec**: [`../specs/filament_slots.md`](../specs/filament_slots.md): wire format, field semantics, third-party adoption guidance.
- **Related dev doc**: [`FILAMENT_MANAGEMENT.md`](FILAMENT_MANAGEMENT.md): backend architecture, UI panels, per-backend details.
- **Lane model**: [`architecture/07-filament-ams.md`](architecture/07-filament-ams.md) § "Lane identity by source".
- **Source anchors**:
  - Record: `include/filament_slot_override.h#FilamentSlotOverride`
  - Store: `src/printer/filament_slot_override_store.cpp#to_lane_data_record`, `src/printer/filament_slot_override_store.cpp#from_lane_data_record`, `src/printer/filament_slot_override_store.cpp#extras_only`
  - Read path: `src/printer/ams_subscription_backend.cpp#refresh_lane_records`, `src/printer/ams_subscription_backend.cpp#adopt_lane_records`
  - Pending: `include/lane_source_store.h#file_pending`, `include/lane_source_store.h#strip_echoed_fields`, `include/lane_source_store.h#expire_pending`
  - Lane sources to `SlotInfo`: `src/printer/lane_resolver.cpp#resolve`, `include/ams_backend.h#AmsBackend/apply_resolved_lane`
  - Tag-declared Spoolman links: `src/printer/tag_spool_link.cpp`, driven by `src/printer/ams_state.cpp#sync_tag_spool_link`
