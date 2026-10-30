# Filament Slot Metadata — Implementation Notes

HelixScreen-internal notes on how we persist per-slot filament overrides. For
the wire-format specification (what a third-party reader or writer needs to
know about the `lane_data` namespace), see
[`../specs/filament_slots.md`](../specs/filament_slots.md). This doc describes
the code that puts records there and reads them back, and assumes familiarity
with the public spec.

---

## 1. Overview

HelixScreen stores per-slot filament overrides — user-edited metadata (brand,
spool name, Spoolman binding, weights, color name) plus color/material — in
the Moonraker `lane_data` namespace following the AFC convention. This doc
describes the internal implementation: architecture, per-backend integration,
lifetime discipline, testing patterns, and the one-shot migration from our
pre-spec legacy namespaces.

The public format is covered by [`../specs/filament_slots.md`](../specs/filament_slots.md);
don't duplicate field semantics here.

All four HelixScreen-managed backends (IFS, Snapmaker, ACE, CFS) emit the
AFC-standard `lane` / `color` / `material` / `vendor` / `spool_id` /
`scan_time` / `bed_temp` / `nozzle_temp` fields (where a source value is
available) in addition to HelixScreen's extension fields. This makes every
slot edit in HelixScreen round-trip to OrcaSlicer 2.3.2+ with no extra
configuration on the slicer side — the convention is shared, not
HelixScreen-specific.

**Two-string material identity.** OrcaSlicer matches a lane to a preset by the
`material` string alone, and an unmatched string resolves to a Generic PLA
preset (wrong temperatures), not a near miss. So `to_lane_data_record()` emits
`material` as a **slicer-matchable** string produced by
`filament::orca_match_type()` (`src/printer/filament_variants.cpp`) — explicit
`orca_type_overrides` entry → exact `orca_library_types` entry →
`extract_base_material()` base polymer → the field is **omitted** when nothing
matches — and separately emits `helix_material`, the user's precise identity
(`ASA-GF`, `PLA Silk`), written unconditionally. `from_lane_data_record()`
prefers `helix_material`, so HelixScreen's own screen keeps the exact type even
though Orca sees the reduced `material`. The library-type set and override table
are generated into `assets/filaments.json` by `scripts/import_orca_filaments.py`
and pre-warmed on the main thread at startup (`filament::warm_orca_tables()`)
so the first lookup never parses the asset on a WebSocket background thread. See
[`FILAMENT_MANAGEMENT.md`](FILAMENT_MANAGEMENT.md) § "Two-string identity" for
the full resolution order and the OrcaSlicer fallback mechanism.

**What OrcaSlicer reads.** Orca's `MoonrakerPrinterAgent` (2.3.2 through
2.5.0-alpha) reads five fields per record: `lane` (a string, 0-based tool
index), `color`, `material`, `bed_temp` and `nozzle_temp`, and picks the first
visible preset whose `filament_type` equals `material`. Every other field is
HelixScreen's or AFC's own. The library-type table is regenerated with
`make regen-filaments ORCA_TAG=<tag>`, and a newer tag can add types the older
Orca releases in the field lack (2.5 adds `PLA Silk`, `PETG HF`, `PA6`,
`PE-CF`, `TPU-AMS`). Mapping a precise type onto one of those makes it match on
the new release and stop matching on the old ones, so `orca_type_overrides`
targets only types every supported Orca release carries.

---

## 2. Architecture

The persistence plumbing lives in three places:

| File | Role |
|------|------|
| `include/filament_slot_override.h` | `FilamentSlotOverride` plain struct — one record's worth of fields, plus `to_lane_data_record` / `from_lane_data_record` for the wire shape. |
| `include/filament_slot_override_store.h` | `FilamentSlotOverrideStore` class — per-backend instance that owns the MR-DB I/O, the local cache file, and the migration helper. |
| `src/printer/filament_slot_override_store.cpp` | Implementation: `load_blocking`, `save_async`, `clear_async`, `cache_path`, plus the `try_migrate_legacy` helper and the free `read_cache` / `write_cache_slot` functions. |

Each backend owns one `FilamentSlotOverrideStore` instance keyed by its
`backend_id`, plus the `overrides_` map that store was loaded into. The
`backend_id` isolates backends so they cannot stomp each other's records, and
so the local cache file can round-trip all of them without collision.

| Backend | `backend_id` | Namespace |
|---------|--------------|-----------|
| `AmsBackendAd5xIfs` | `ifs` | `lane_data` (shared) |
| `AmsBackendSnapmaker` | `snapmaker` | `lane_data` (shared) |
| `AmsBackendAce` | `ace` | `lane_data` (shared) |
| `AmsBackendCfs` | `cfs` | `lane_data` (shared) |
| `AmsBackendQidi` | `qidi` | `lane_data` (shared) |
| `AmsBackendToolChanger` | `toolchanger` | `lane_data` (shared) |
| `AmsBackendAfc` | `afc` | `helix-screen-afc-overrides` (private) |
| `AmsBackendHappyHare` | `happyhare` | `helix-screen-hh-overrides` (private) |

AFC and Happy Hare must name a private namespace: their own Klipper plugins own
`lane_data` and rewrite it on boot, so records left there do not survive and the
plugin's own load back as if the user had authored them.

`AmsBackendQidi` runs a store in the shared `lane_data` namespace like the rest;
its `filament_slot{n}` / `color_slot{n}` / `vendor_slot{n}` save_variables are the
signal its fingerprint is built from, and the record carries the swap-detection
baseline across restarts (see `FILAMENT_BACKEND_QIDI_BOX.md` § Slot Metadata).

### Acquiring the store

`helix::ams::make_loaded_override_store()` builds the store and loads it in one
call, returning both halves in a `LoadedOverrideStore`:

```cpp
auto loaded = helix::ams::make_loaded_override_store(api_, "happyhare", get_type(),
                                                     backend_log_tag(), kOverrideNamespace);
{
    std::lock_guard<std::mutex> lock(mutex_);
    override_store_ = std::move(loaded.store);
    overrides_ = std::move(loaded.overrides);
}
```

A store whose load never ran is not a state any backend wants, and this is the
only way to get one, so that half-built version has no spelling. Call it with no
lock held - the round-trip blocks for up to 5s and the status subscription is
already live - and publish both fields under the lock afterwards.

`AmsBackendHappyHare` uses the helper. The other six still hand-write the
equivalent block in their own `on_started()` and are a straight follow-up
conversion; the helper's signature already covers every one of them (the only
variations are `backend_id`, the namespace, and whether the caller does extra
work with the store afterwards - AD5X IFS also reads the seated-lane scalar,
ToolChanger re-layers onto slots built before the load).

### Key methods

| Method | Threading | Purpose |
|--------|-----------|---------|
| `load_blocking()` | Called once from backend init, blocks the backend thread | Fetch `lane_data` from MR DB; on error/timeout, fall back to the local JSON cache. Returns `unordered_map<int, FilamentSlotOverride>`. Also triggers `try_migrate_legacy` if `lane_data` is empty and legacy namespaces have data, and runs the one-shot **material heal** (below). |
| `save_async(slot, override, cb)` | Main thread → HttpExecutor | POST the record to `lane_data/<key>` (see **Outer key style** below), refresh the local cache on success. Retries are the caller's responsibility. |
| `clear_async(slot, cb)` | Main thread → HttpExecutor | DELETE the slot's `lane_data` entry and drop it from the local cache. |

### Outer key style

The outer Moonraker DB key is **not** always `laneN`. `format_lane_key()` picks
between two shapes with **different bases**, and the store's style comes from
`lane_key_style_for(AmsType)` (`include/filament_slot_override_store.h`):

| Style | Key | Base | Used by |
|-------|-----|------|---------|
| `LaneKeyStyle::Lane` | `laneN` | **1-based** (`lane1` is slot 0) | every filament-switching system — AFC, Happy Hare, ACE, CFS, IFS |
| `LaneKeyStyle::Tool` | `T<n>` | **0-based** (`T0` is slot 0) | tool changers — Snapmaker, generic `TOOL_CHANGER` |

The inner `"lane"` field is 0-based in **both**, so the offset between the outer
key and the inner field is deliberate for `laneN` and absent for `T<n>`. Changing
one base without the other silently breaks interop with every other reader.

Tool-changer backends that predate the `T<n>` style carry a one-shot
`try_migrate_lane_keys_to_tool_keys()` at load: any `laneN` record HelixScreen
itself authored is rewritten as `T<n>` (or dropped when the canonical `T<n>`
already exists). Records it cannot prove it wrote are left alone.
| `cache_path()` | Pure | Returns `helix::get_user_config_dir() / "filament_slot_overrides.json"`. |

### Load behavior

`load_blocking` is a sync-over-async bridge. It fires a Moonraker DB request
and waits up to 5s (tunable via `load_timeout_`) on a `std::condition_variable`
backed by a `shared_ptr<SyncState>`. Moonraker's request tracker can still
fire the response callback up to ~60s later, which is why the rendezvous
state is kept alive via `shared_ptr` — a late callback harmlessly flips flags
on the still-living state. If the request completes with data, we take it.
If it errors or times out, we fall back to `read_cache()` for this backend's
entries. The cache is the offline-fallback view, never authoritative — a
successful MR fetch always supersedes it.

### Lifetime safety

MR DB callbacks can fire up to ~60 seconds after the initial call, long after
a store could be destroyed during teardown or reconfiguration. The store
avoids capturing `this` in HTTP callbacks and instead captures values plus a
`shared_ptr<SyncState>` for the sync-over-async bridge. The pattern:

```cpp
auto state = std::make_shared<SyncState>();
api_->db_get("lane_data", [state](const auto& result) {
    std::lock_guard<std::mutex> lk(state->m);
    state->result = result;
    state->done = true;
    state->cv.notify_one();
});
// block up to timeout on state->cv
```

The `state` shared_ptr keeps the rendezvous alive even if the store is
destroyed between the call and the late callback. This is an intentional
alternative to `AsyncLifetimeGuard` — the store doesn't own a UI and doesn't
need lifetime-gated UI updates; it just needs its one response slot to survive.

---

## 3. Per-backend integration

Each backend ties its `FilamentSlotOverrideStore` into its parse path and its
own hardware-event signal:

| Backend | Backend ID | Parse hook | Hardware-event signal | Override-exclusive fields |
|---------|------------|------------|-----------------------|---------------------------|
| `AmsBackendAd5xIfs` | `ifs` | `update_slot_from_state` → `apply_resolved_lane` | `Adventurer5M.json` color RGB change | brand, spool_name, spoolman filament/vendor ids, weights, color_name |
| `AmsBackendSnapmaker` | `snapmaker` | tail loop at end of `handle_status` | `filament_detect.info[ch].CARD_UID` byte-array → canonicalized string | spool_name, spoolman filament/vendor ids, remaining_weight_g |
| `AmsBackendMultiAce` | `snapmaker` (same backend id) | `rebuild_units_locked` per-bay loop, before the lane ingest | Heads: the base's `CARD_UID` path. Bays: the engine table's uid, else material+colour, through the same shared tracker; an entry stating no identity is no signal | spool_name, spoolman filament/vendor ids, remaining_weight_g |
| `AmsBackendAce` | `ace` | `parse_ace_object` per-slot loop | Status transition: EMPTY/UNKNOWN → present | brand, spool_name, spoolman filament/vendor ids, weights, color_name |
| `AmsBackendCfs` | `cfs` | `handle_status` tail loop | Composite `material_type\|color_value` fingerprint | spool_name, spoolman filament/vendor ids, remaining_weight_g |
| `AmsBackendToolChanger` | `toolchanger` | `handle_status` tail loop, `initialize_tools()` tail, and after the start-time load | **None** - see below | *every* field |
| `AmsBackendAfc` | `afc` | `apply_lane_status` and the `lane_data` query parse | AFC's own firmware clears | brand, color_name, spoolman filament/vendor ids |
| `AmsBackendHappyHare` | `happyhare` | `gate_spool_id` loop in `handle_status` | Gate-map spool id change | brand, spool_name, total_weight_g, color_name, spoolman filament/vendor ids |

"Override-exclusive fields" are the fields the user can edit on that backend
but the firmware never supplies — they always come from the override, never
fall through.

### Tool Changer is the odd one out

Every other backend layers overrides over something the machine reports, and
clears them when the hardware says the spool physically changed. klipper-toolchanger
does neither, and both halves of that are load-bearing:

- **The store is the sole source of filament identity, not a layer over one.**
  `parse_tool_state()` reads `mounted` and `active` and nothing else - no
  material, colour, brand or weight exists to fall through to. So "override-exclusive
  fields" is every field, and the merge is trivially "the override wins".
- **There is deliberately no hardware-event clearing.** Nothing on a tool changer
  can tell that a user swapped a spool - no RFID, no presence transition, no
  colour reading. That is a decision, not an omission: inventing a clear signal
  here would throw away user data on an event that does not mean what it would
  have to mean. The user-initiated `clear_slot_override()` is a different thing
  and is implemented: it drops the store record, the only record there is.
- **The wipe it fixes is `initialize_tools()`**, which resets every slot to
  `kAmsDefaultSlotColor` with the tool name as a placeholder `spool_name`, and
  runs on every `set_discovered_tools()`. Overrides are therefore re-layered at
  three points, not just the parse tail: that function's own tail, the parse
  tail, and immediately after the start-time load (because `set_discovered_tools()`
  runs before `start()`, so the slots predate the loaded overrides).
- **`T<n>` outer keys** (`lane_key_style_for`) are shared with Mainsail #2510's
  records rather than duplicating them.

Every lane-holding backend implements it: IFS, Snapmaker, ACE, CFS, AFC, Happy
Hare, the tool changer and the mock reset the lane to machine readings
(`reset_lane_to_machine_readings()`), erase the in-memory entry, reset the
override-exclusive fields on the live slot (brand, spool name, Spoolman ids,
weights, colour name, catalog pick) so the clear shows on the next
`get_slot_info()`, and fire `clear_async` against the backend's own private
namespace. The mock keeps no override records, so its implementation is the
lane reset and the slot-changed event alone. Colour and material are left
standing, because those come from the parse and the lane's firmware values
should surface, except on the tool changer, which has no parse underneath:
its clear blanks the whole slot, colour and material included, because its
store is the only record there is. `AmsBackendAfc` also empties them in
firmware with `SET_MATERIAL LANE=x MATERIAL=`, `SET_COLOR LANE=x COLOR=` (AFC
stores a bare `#`, which the parse reads as no colour) and
`SET_WEIGHT LANE=x WEIGHT=0`: the commit's empty `SET_SPOOL_ID` only runs
AFC's `clear_values()` when AFC has Spoolman configured and the lane does not
remember its spool, so an unlinked lane would otherwise keep them. The
parse reads that zero weight beside no material and no colour as unknown, not
an empty spool. One difference from AFC's own eject clear remains: AFC applies
its default material on a load only when the lane's colour is empty, and the
bare `#` is not, so the next untagged spool loads with no material until
something declares one.
`AmsBackendQidi` also implements it, with
a firmware half: the
local clear is the same erase/reset/`clear_async` against the shared
`lane_data` namespace, and the firmware half is three
`SAVE_VARIABLE VARIABLE={filament,color,vendor}_slot{n} VALUE=0` writes (row
ids start at 1, so 0 reads as no identity). `SAVE_VARIABLE` moves nothing, so
the writes go out mid-print too; the UI already refuses a clear on the lane
feeding the print. A
tagged spool re-populates the ids on its next insert, boot or RFID read. The
`lane_data` records
their Klipper plugins write are a separate thing and HelixScreen does not touch
them. For AFC that is not merely etiquette: AFC.py `delete_lane_data()`
wipes the whole namespace at the start of every PREP and refills it one lane at
a time, so the namespace is non-durable across reboots *and* transiently
incomplete during them. AFC/HH overrides go to a private namespace
(`kOverrideNamespace`) — see prestonbrown/helixscreen#1158.

---

## 4. Local cache

- **File**: `helix::get_user_config_dir() / "filament_slot_overrides.json"`
- **Format**: top-level `"version": 1`, then keys by backend_id, each holding
  a `"slots"` object keyed by stringified slot index:

```json
{
  "version": 1,
  "ifs": {
    "slots": {
      "0": { "brand": "Polymaker", "color_name": "Orange", ... },
      "2": { "brand": "Hatchbox", ... }
    }
  },
  "cfs": {
    "slots": {
      "0": { "spool_name": "...", "remaining_weight_g": 850.0 }
    }
  }
}
```

- **Write**: atomic — write to `.tmp` sibling, then `std::filesystem::rename`
  into place. Any failure along the way leaves the previous file untouched.
- **Read**: only when the MR DB fetch fails in `load_blocking`. Never
  authoritative — it exists so a first-boot-after-reconnect on a flaky
  network doesn't lose the user's view of recent edits.
- **Scope**: per-user, persists across app restarts. Not synced between
  HelixScreen instances — MR DB is the shared source of truth.

---

## 5. Merge policy

The merge rule (firmware vs stored record) is documented in
[`../specs/filament_slots.md`](../specs/filament_slots.md#5-merge-policy).
Implementation-side a stored record is not merged into a `SlotInfo` at all. It
is translated into per-source lane records, and the resolver ranks those
against what firmware states on the current frame.

`ingest_legacy_records()`
(`src/printer/lane_legacy_migration.cpp#ingest_legacy_records`) runs once per backend
from its init path, immediately after `load_blocking()`. It hands each parsed
record to `sources_from_record()`
(`src/printer/lane_translation.cpp#sources_from_record`), which splits one
record into the several sources it may legitimately speak for, and files each
through that source's own funnel: the `LocalUser` part through
`commit_slot_edit()`, everything else through `ingest()`. The split is by what
the record says about authorship, not by whether a field holds a value. The
first row below settles a linked record on its own; the rest are how an
unlinked one is split:

| What the record holds | Filed as |
|-----------------------|----------|
| a `spool_id` above zero | `Spoolman` for the whole identity, except a colour the record's `helix_declared` names, which is `LocalUser`: the colour ladder puts a person above the server. A `helix_locked_*` key on a linked record is not read, since a release 1.0 writer set it on links and meter flushes alike |
| a field named in the record's `helix_declared` set, colour and material included | `LocalUser`. A declaration stands over a value the record carries, for every field alike: a clear is not a declaration but "whatever the machine reports", so a field the record holds nothing in is never declared, and a name for it in the set (a record written by a build that recorded clears) reads as no declaration |
| on a record with no `spool_id`, a colour or material its `helix_declared` does not name, beside a `helix_locked_*` key present and true **on the wire** | `LocalUser`: how a record written before the set could name colour and material is read. Absent or false is `Remembered` |
| `catalog_id` / `product_name` | `LocalUser` regardless of what the record declares: firmware has no concept of a catalog product, so a value there can only be a pick |
| anything else the record carries | `Remembered`, which the resolver ranks **below** the current firmware frame |
| `remaining_weight_g` / `total_weight_g` | `Metered`, always: a weight is a measurement whoever wrote it |

A lane's stored record is amended from each changed Spoolman filing, through
`AmsBackend::persist_external_identity()`
(`include/ams_backend.h#AmsBackend/persist_external_identity`). It takes the
identity the server states, leaves a declared colour, the weights, the catalog
pick and the temperatures alone, claims no authorship, and writes nothing to
firmware. A lane with no record gets none: a filing is not an edit.

A record written before `helix_declared` existed carries no set, and its brand,
spool name and vendor id count as declared only beside a colour or material
declaration the same record carries. That declaration is the evidence a person
edited the record at all, since the auto-mirror declares neither and can
populate none of those three.

Each backend's parse then ends with `apply_resolved_lane()`
(`include/ams_backend.h#AmsBackend/apply_resolved_lane`), which lays
`resolve()`'s answer onto the `SlotInfo` the parse just built. Only fields some
source actually observed are written, so a field no source spoke to keeps the
backend's own value, and the fields the resolver does not own (tool mapping,
extruder name, endless-spool group, error, environment, remaining length, temps,
indices) are left exactly as the backend set them.

On the two backends whose `SlotInfo` persists across frames (AD5X IFS, Happy
Hare), `prepare_lane_repaint_locked()`
(`include/ams_subscription_backend.h#AmsSubscriptionBackend/prepare_lane_repaint_locked`)
runs before that paint and blanks the identity fields only the lane's records
state (`clear_lane_only_identity()`, `src/printer/lane_apply.cpp`), so a
dropped record stops showing rather than living on in the struct; a field the
slot's override record carries takes that record's value instead, and colour,
material and weights stay with the firmware-truth caches. A backend that
rebuilds its struct from each frame needs none of this.

Beside that ranking sit the two cross-field rules that can invalidate a lane's
declared identity outright. Both are `classify_binding()`
(`src/printer/lane_binding.cpp#classify_binding`), a pure function over the lane's
sources and one `BindingReading`; `reconcile_binding()` applies its verdict by
dropping the records that declared the broken binding (`Spoolman`, `LocalUser`
and `Remembered`, leaving `Sensed`, `VendorCache` and `Metered` alone), and the
backend clears its persisted copy with `clear_persisted_override()` so the
record does not come back at the next start.

- **Rule 1 — external re-bind.** Firmware reports a positive `spoolman_id`
  different from the one the lane's declaring sources name → verdict `Rebound`.
  Not gated by any capability or setting; can fire on any backend whose
  firmware reports a positive spool id (AFC, Happy Hare, and flat-schema CFS,
  which parses a per-slot id). Our own in-flight writes are exempt: backends
  call `AmsBackend::record_own_spool_write()` at the write site, and
  `AmsBackend::reconcile_lane_binding()` feeds
  `AmsBackend::own_write_expectation()` into `BindingReading`'s own-write pair:
  the id firmware last reported before the write and the just-written id do not
  fire; a third id fires and consumes the expectation.
- **Rule 2 — eject.** `AmsBackend::printer_reports_spool_ids()` (true only
  on AFC and Happy Hare) arms **only** this rule: there a firmware id ≤ 0
  while the lane still declares a positive id is the plugin's own eject
  signal, and it clears only when the `ams/keep_spool_info_on_eject`
  setting is off (default on — "Keep Spool Info on Eject" toggle in the AMS
  Management overlay, shown only where the capability is true). Caveat: with
  AFC's own retention on (`remember_spool = true` everywhere) firmware never
  reports the eject zero, so this rule cannot fire and the toggle is a no-op —
  `printer_retains_spool_info()` detects that shape and the overlay disables
  the toggle with a note instead.

The id `classify_binding()` compares against must be firmware's own standing
word for the lane, never a `SlotInfo` field: `apply_resolved_lane()` writes the
lane's resolved `spoolman_id` back into that struct, so a caller reading it
would compare a record against itself and no binding could ever look broken.

**Exactly one reader may consume an own-write expectation.**
`own_write_expectation()` is single-shot: the frame that matches the written id
erases the entry. It has to be consumed at the site that sees firmware's own
spool id and nowhere else, or the expectation ends an id early and the next
frame reads our own in-flight write as somebody else's re-bind.
`reconcile_lane_binding()` is its only caller, so that holds by construction
rather than by discipline.

What counts as a value the record "carries" at all, per field type:

| Field type | Treated as "nothing here" |
|------------|---------------------------|
| `std::string` | empty string |
| `int` (ids) | 0 |
| `float` (weights) | negative |
| `uint32_t` (`color_rgb`) | `color_set` false, or the value `kAmsDefaultSlotColor` (`0x808080`), which is where a cleared slot and a colourless record both land |

`color_set` is why colour needs two tests rather than one: pure black is a real
filament colour, so `color_rgb == 0` cannot mean "unset". `is_declarable_color()`
(`src/printer/lane_translation.cpp#is_declarable_color`) is the second, and it is
deliberately a struct-side answer only. A producer writing `#808080` on a *wire*
is stating a grey, which `read_lane_color()` reads as an observation. When adding
a field to the struct, pick a "nothing here" value a user could never legitimately
enter for it.

### Tag readings and provenance

A backend with a tag reader (an RFID chip, or an inline colour scanner) answers
`AmsBackend::tag_reading(slot)` with what the tag reported for the spool now in the
slot, as a `helix::ams::TagReading` (`include/ams_tag_reading.h`): material, brand,
product, colour with any multi-colour, nozzle min/max, bed temperature and total weight,
each optional, unset where the tag said nothing. Every other backend answers `nullopt`.
The reading is the tag's own, kept while a user's edit overrides it, which the lane's
VendorCache record alone cannot do on a backend whose write-back replaces the box's
record.

`AmsBackend::tag_provenance(slot)` compares `get_slot_info()` against the reading: a field
is from the tag when the tag states it and the slot shows that value (text
case-insensitive; colour needs the multi-colour to match too). A user's override that
differs reads as not from the tag, and `helix::ams::reset_to_tag(slot, reading)` is the
edit that restores it, which also clears the colour name and a catalog pick the tag's
identity no longer matches. Generic code calls only these three; the per-backend rule for
what a tag reading is lives in each leaf doc:

| Backend | Reading | How it is kept |
|---|---|---|
| Snapmaker U1 | `filament_detect.info` with a real `CARD_UID` | the firmware keeps it (edits go to `print_task_config`) |
| CFS | the box's latched record plus the catalog temps for its code; a Spoolman id from the stock record's serial | `fold_tag_frame`; the id is re-read each frame |
| QIDI Box | the saved RFID ids resolved through the Box tables, plus the profile temps | `fold_tag_frame` |
| ACE | a frame whose `rfid` says identified | per bay, until a read finds no tag or the bay empties |
| AFC | the lane's `td1_color` scan | `SlotInfo::scanned_color_rgb` |

`helix::ams::fold_tag_frame(kept, raw, filed)` is the rule for a backend whose user edit
is written back into the record the tag reader fills: a field the frame states and the
own-write echo guard files replaces the kept value, a field the guard withholds is our own
write coming back and keeps it, and a field the frame no longer states is dropped. Two
limits follow from the box holding one record: after a restart the guard is not armed, so
what the box reports (by then the user's values) becomes the reading; and a value typed on
the printer's own screen is indistinguishable from a tag read.

A user override survives the tag reader's next poll everywhere through the lane resolve:
the declaring LocalUser record outranks VendorCache. The multi-colour is the one tag field
no lane record carries, so a backend lays it onto the slot only while the lane declares no
colour, and `SlotInfo::assign_filament_fields` carries it through an edit.

### Tag-declared Spoolman links

Some tags name a Spoolman spool. `TagReading::spoolman_id` carries it, and only a backend's
own decoder fills it: each tag format has its own values that name no spool (a CFS stock
tag's factory serial `000001`, a blank), and the decoder drops them, so a set id is always
one to look up. It is a binding, not a property the slot shows, so it is no `TagField`,
provenance and `reset_to_tag()` ignore it, and `fold_tag_frame()` leaves it to the backend.
`AmsBackend::tags_name_spoolman_spools()` says whether a backend's tags can carry one; it
gates the setting's row and nothing else.

The policy is backend-neutral and lives in one place: `helix::ams::TagSpoolLinkTracker`
(`include/tag_spool_link.h`), a pure per-lane tracker, driven by
`AmsState::sync_tag_spool_link` on every primary-backend sync. It acts on transitions of
the id, never on a frame that repeats it:

| Frame | Effect |
|---|---|
| lane reads empty | forget the lane's tag: whatever is seated next is a new insert |
| occupied, no id (unread, untagged, a default serial) | nothing |
| first id this session | link only a lane linked to nothing, so a link made before a restart stands; a spool Spoolman does not have is logged, not toasted |
| same id as last seen | nothing, so a spool picked while that tag stays seated is kept |
| a different id, or an id after empty | link the lane to it |

A wanted link waits across frames while Spoolman is unreachable and is dropped when the
tag changes or the lane empties. When Spoolman is available AmsState looks the spool up
(`get_spoolman_spool`, completion deferred to the main thread through `async_lifetime_`),
re-checks that the lane still wants it, and links through `AmsState::link_spool_to_slot`,
the same commit a QR scan uses: `apply_spool_to_slot()` over the slot, then
`commit_slot_edit()`, so the identity cache, the active spool on a loaded lane, the
backend write and persistence all behave as for a pick. A spool Spoolman does not have
gets one warning toast. A lookup that gets no answer is logged and offered again after
`TagSpoolLinkTracker::kLookupRetryFrames` frames, so a slow Spoolman is retried without a
lookup per status frame. The
per-printer setting `ams/tag_links_spoolman` (default on) settles every want without a
lookup, so turning it on never links a spool already seated.

The tracker lives for the session. The last id a lane showed is not persisted, so a
restart cannot tell a spool that sat in the lane from one put in while HelixScreen was off,
and the first frame only links a lane linked to nothing. That has two visible effects: a
lane a person unlinked by hand while its tag stayed seated is linked again after a restart,
and turning the setting on and restarting links spools already seated in unlinked lanes.
Persisting the id would need a new field in the slot override record, which exists only for
a lane with an edit (an unlinked lane and a lane whose tag names a missing spool usually have
none), plus its own wire key and load path in every backend that binds the store; the
session-only rule is the smaller cost. A lane whose tag names a spool Spoolman lacks warns
only on an insert this session, so a restart does not repeat the toast.

A lane in an absent unit (`AmsSystemInfo::slot_absent()`) is skipped: its bays are
placeholders that read empty, and a box dropping off the bus is not a removal. A lookup's
answer is dropped when the lane's link changed while it ran, which is a pick that stands.

This is deliberately not `classify_binding()`'s Rule 1. Rule 1 compares firmware's id
against the lane's declaration on every frame, so a tag id fed to it would undo a pick made
while the tag stays seated.

---

## 6. Clear semantics

Clear Spool erases everything HelixScreen and the printer's firmware remember
about that slot; what's left afterwards is only what the hardware can
physically read right now. How close each backend gets to that bar is set by
what its firmware can be told to forget:

- **AD5X IFS, AFC, CFS on Kalico, Tool Changer and Happy Hare** reach it:
  the first four through write paths that cover the firmware-held fields (and
  the tool changer's store is the only record there is, so dropping it erases
  everything), Happy Hare through one
  `MMU_GATE_MAP GATE=n MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1` that empties
  the firmware gate map. In Spoolman pull mode the gate map is Spoolman's, so
  Happy Hare refuses the write honestly (a partial failure naming Spoolman
  as the owner) instead of pretending.
- **QIDI Box reaches it too**: the box's `SAVE_VARIABLE`s are the record,
  and the clear writes `VALUE=0` to `filament_slot{n}` / `color_slot{n}` /
  `vendor_slot{n}` (row ids start at 1, so 0 reads as no identity, and vendor
  0 is Generic), mid-print too because `SAVE_VARIABLE` moves nothing. A
  tagged spool re-populates
  its ids on the next insert, boot or RFID read (the hardware reading what
  is physically there).
- **ACE, Snapmaker and stock CFS cannot** (read-only API, no empty spelling
  for a slot value, and the tag is re-read on the next probe), so their
  tag-derived and firmware-held values survive a clear.

The gesture also refuses while its lane feeds an active print
(`clear_spool_blocked_by_print()` in `include/filament_op_slot_resolver.h`):
the job holds the machine (`job_holds_machine()`) and the lane is the one at
the toolhead (`AmsBackend::slot_is_actively_loaded()`), so the material and
colour the print's own surfaces are displaying survive until the job ends.
The context menu greys the button with the reason (`ams_slot_can_clear` /
`ams_slot_clear_hint`); the dispatch guard in `ams_dispatch_backend_action()`
is the authority for a caller holding a menu rendered before the print
started. Other lanes stay clearable mid-print, and a free machine clears any
lane, loaded or not.

Four distinct clear paths, handled separately:

- **User-initiated clear.** The AMS context menu's "Clear Spool" gesture
  (`MenuAction::CLEAR_SPOOL` in `src/ui/ui_ams_detail.cpp`) is a commit first
  and a clear second, and the order is load-bearing. `AmsState::commit_slot_edit()`
  carries the arms a backend clear has no way to reach: the Spoolman server
  active-spool unlink, the identity-cache invalidation and the ToolState clear
  (bundle F2LNLQCC: clearing only the backend left the server asserting the
  spool again after a restart). The commit hands the backend a slot with
  nothing on it, and Happy Hare's backend recognizes exactly that shape
  (`is_full_clear()`), answering with one gate-map wipe
  (`MMU_GATE_MAP GATE=n MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1`;
  see [the Happy Hare backend doc](FILAMENT_BACKEND_HAPPY_HARE.md#clear-spool));
  in Spoolman pull mode it refuses that write honestly, reporting partial
  failure with Spoolman named as the owner, and a print running on the gate
  gets a backend-level refusal there for the same reason the dispatch guard
  above refuses. Only on the commit's success does the gesture call
  `AmsBackend::clear_slot_override(slot_index)`, which drops the lane's
  standing user declarations and the persisted override record: the half an
  edit statement cannot express, because on an unlinked lane a colour pick, a
  typed weight and a colour name never engage as clears
  (prestonbrown/helixscreen#1661). The tool changer clears its store and
  nothing else; that store is the only record there is, so its clear has no
  firmware half.
- **Hardware-event clear.** Each backend watches its own signal (see the
  integration table) and auto-clears when the signal transitions to
  "different spool". The baseline is recorded on first observation after
  startup and NEVER triggers a clear on its own — otherwise every app launch
  would wipe overrides. Backends on the shared fingerprint tracker seed that
  baseline from the record's persisted fingerprint instead ("Swap
  fingerprints" below), so a swap that happened while HelixScreen was off is
  caught on the first frame after start.
- **Binding-rule clears (re-bind / eject).** The two cross-field rules
  `classify_binding()` decides (§5) drop the lane's declaring sources and the
  backend's persisted record with them: an external re-bind (firmware reports a
  different positive spool id), or, only where `printer_reports_spool_ids()` is
  true and the setting is off, a firmware eject signal.
- **Self-wipe prevention.** When the user edits the color on IFS, the IFS
  backend pre-updates `last_firmware_color_` to the user's new RGB before
  pushing the override. Without this, the next `Adventurer5M.json` read
  would see a "color change" (because firmware still reports the original
  color briefly) and clear the override the user just saved. Snapmaker and
  CFS don't need this pre-update: `CARD_UID` and the composite fingerprint
  aren't user-editable.

### Swap fingerprints: persistence across restarts

CFS (stock and flat schema), Snapmaker and QIDI Box run their hardware-event
clear through the shared `SlotFingerprintTracker`
(`include/filament_slot_override_store.h`). Each observed identity string
(stock CFS material-code|color composite, the flat schema's
material/brand/name/color composite, Snapmaker's `CARD_UID`, QIDI Box's
filament|color|vendor row-id composite) classifies as
`NoSignal`, `Baseline`, `Unchanged`, `OwnWriteEcho` or `Changed`, and only
`Changed` clears the override. Before pushing a user edit the backend arms
`expect_any_of()` with the values the write should echo back. Expectations
accumulate per slot, so two edits inside one poll window each match their own
echo, while a change no write asked for consumes every pending expectation
and still reports `Changed`. A failed dispatch drops them
(`forget_expected()`): a write that never reached the firmware cannot blind
the slot.

A baseline that lives only in memory misses a swap made while HelixScreen was
off, so `bind_fingerprint_persistence()` — each of those backends calls it
right after the store loads — ties the tracker to the records:

- **Seeding.** Every loaded record with a non-empty `fingerprint` seeds the
  baseline, so the first observation after start compares against the last
  fingerprint seen before shutdown: different means `Changed` (clears, same
  as a live swap), same means `Unchanged` (the override survives the
  restart).
- **Recording.** The baseline sink writes the current fingerprint onto the
  record on `Baseline`, `Unchanged` and `OwnWriteEcho`, never `Changed`. A
  change clears the record, so the new spool's fingerprint is picked up by
  the first stable observation of the override that follows it.
- **On-disk key.** The fingerprint rides the record as `helix_fingerprint`
  in the shared `lane_data` namespace and as bare `fingerprint` in the local
  cache (§4); the wire format is
  [`../specs/filament_slots.md`](../specs/filament_slots.md). Records
  without the key seed nothing and behave as before: first observation is
  `Baseline`, no clear.

---

## 7. Testing patterns

Tests live in `tests/unit/test_filament_slot_override*.cpp` and in the
per-backend `test_ams_backend_*.cpp` files. Shared patterns:

- **Friend-class test access** (per lesson L065): `Ad5xIfsTestAccess`,
  `SnapmakerTestAccess`, `AceTestAccess`, `CfsTestAccess`. These friend the
  production class and expose private hooks for seeding overrides, injecting
  a store, or driving the parse path without going through a live MR API.
- **`TmpCacheDir` RAII helper**. Overrides `HELIX_USER_CONFIG_DIR` to a
  temp directory for the test's lifetime and rm's it on teardown. Keeps
  cache writes from touching the developer's real `~/.helixscreen/`.
- **Mock hooks for MR DB** (on `MoonrakerClientMock`, which serves
  `server.database.*`): `mock_db_set`, `mock_db_get`, and the generic
  per-method `fail_next`, `defer_next` + `fire_deferred` /
  `fire_deferred_error`, and `call_count`. The deferred hooks are how we exercise the "callback fires after the store is
  destroyed" lifetime regression — seed a deferred call, destroy the store,
  fire the callback, assert nothing crashes.
- **Tags**:
  - `[slow]` on any test that uses `MoonrakerAPIMock` (L052 — prevents
    parallel shard hangs when LVGL-backed subjects get torn down out of
    order).
  - `[filament_slot_override]` covers every store-related test across
    files for easy targeted runs.

---

## 8. Migration

One-shot migration from the pre-spec legacy namespaces
`helix-screen:ace_slot_overrides` and `helix-screen:cfs_slot_overrides`
into `lane_data/laneN` entries. Lives in `try_migrate_legacy` in
`src/printer/filament_slot_override_store.cpp`.

Trigger conditions (all must hold):

- `lane_data` namespace for this backend is empty
- Legacy namespace for this backend has data
- Backend is ACE or CFS (IFS and Snapmaker never used the legacy namespaces,
  so their migration path short-circuits)

The migration is idempotent: once records exist under `lane_data`, the
"`lane_data` empty" guard fails and the legacy path is skipped. After a
successful migration, the legacy MR DB entry is best-effort deleted via
`database_delete_item`, and the pre-Task-6 per-backend JSON cache file
(ace_slot_overrides.json / cfs_slot_overrides.json) is also removed from
the user config dir. Delete failures are logged at warn but do not break the
migrated result — a lingering legacy blob is harmless because the idempotence
guard would short-circuit on the next startup anyway. Migration also deletes
the legacy entry in the all-malformed-entries case so subsequent startups
don't re-scan unsalvageable data on every boot.

Migration runs inline inside `load_blocking`, before the method returns, so
the backend sees the migrated records immediately without a second round-trip.

### Material heal

Records written before the two-string split (see § 1) carry a `material` that
may not be slicer-matchable and no `helix_material`. `load_blocking` runs a
one-shot heal over the fetched `lane_data` to repair them without the user
re-editing every slot:

- **Only HelixScreen-authored records** are touched, proven by a `helix_locked_*`
  key. AFC, Happy Hare, and Mainsail share this namespace; their records are not
  ours to rewrite (same ownership rule the anomaly scanner follows).
- The record is **mutated in place** — `helix_material` set to the precise
  identity, `material` set to `orca_match_type()` of it (or erased if nothing
  matches) — so `scan_time` and any co-author's fields survive. The POST targets
  the **same key it read**, so on a `T<n>` tool-changer backend the stale `laneN`
  record is healed before the key migration moves it, and the migration carries
  the healed body.
- **Gated on `orca_tables_available()`.** A missing or stale
  `assets/filaments.json` would make `orca_match_type()` return empty for every
  input and strip `material` from every lane in one pass, so the heal skips
  entirely rather than heal against an empty table.
- **Re-runs on drift.** The gate is "resolves to a different match string than it
  carries," not "already has `helix_material`", so a later library regeneration
  that drops a type we previously matched is repaired on the next boot. It
  converges because `orca_match_type(material) == material` is a fixed point on
  its own output.

---

## 9. References

- **Public spec**: [`../specs/filament_slots.md`](../specs/filament_slots.md) — wire format, field semantics, third-party adoption guidance.
- **Related dev doc**: [`FILAMENT_MANAGEMENT.md`](FILAMENT_MANAGEMENT.md) — backend architecture, UI panels, per-backend implementation details.
- **Source anchors**:
  - Struct: `include/filament_slot_override.h` (`FilamentSlotOverride`)
  - Store: `include/filament_slot_override_store.h` + `src/printer/filament_slot_override_store.cpp` — `load_blocking` (line 565), `save_async` (line 663), `clear_async` (line 717), `cache_path` (line 345), `try_migrate_legacy` (line 385), `read_cache` (line 243), `write_cache_slot` (line 150)
  - Record to lane sources: `src/printer/lane_translation.cpp` (`sources_from_record`), `src/printer/lane_legacy_migration.cpp` (`ingest_legacy_records`)
  - Lane sources to `SlotInfo`: `src/printer/lane_resolver.cpp` (`resolve`), `src/printer/lane_apply.cpp` (`apply_resolved`), `src/printer/ams_backend.cpp` (`AmsBackend::apply_resolved_lane`)
  - Binding rules: `src/printer/lane_binding.cpp` (`classify_binding`, `reconcile_binding`)
  - Tag-declared Spoolman links: `src/printer/tag_spool_link.cpp` (`TagSpoolLinkTracker`), driven by `src/printer/ams_state.cpp#sync_tag_spool_link`
