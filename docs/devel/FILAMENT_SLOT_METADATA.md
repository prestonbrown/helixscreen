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

---

## 2. Architecture

The persistence plumbing lives in three places:

| File | Role |
|------|------|
| `include/filament_slot_override.h` | `FilamentSlotOverride` plain struct — one record's worth of fields, plus `to_lane_data_record` / `from_lane_data_record` for the wire shape. |
| `include/filament_slot_override_store.h` | `FilamentSlotOverrideStore` class — per-backend instance that owns the MR-DB I/O, the local cache file, and the migration helper. |
| `src/printer/filament_slot_override_store.cpp` | Implementation: `load_blocking`, `save_async`, `clear_async`, `cache_path`, plus the `try_migrate_legacy` helper and the free `read_cache` / `write_cache_slot` functions. |

Each backend (IFS, Snapmaker, ACE, CFS) owns one `FilamentSlotOverrideStore`
instance keyed by its `backend_id` (`"ifs"`, `"snapmaker"`, `"ace"`, `"cfs"`).
The store isolates backends so they cannot stomp each other's records, and so
the local cache file can round-trip all four without collision.

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
| `AmsBackendAd5xIfs` | `ifs` | `update_slot_from_state` → `apply_overrides` | `Adventurer5M.json` color RGB change | brand, spool_name, spoolman_id, spoolman_vendor_id, weights, color_name |
| `AmsBackendSnapmaker` | `snapmaker` | tail loop at end of `handle_status_update` | `filament_detect.info[ch].CARD_UID` byte-array → canonicalized string | spool_name, spoolman_id, spoolman_vendor_id, remaining_weight_g |
| `AmsBackendAce` | `ace` | `parse_ace_object` per-slot loop | Status transition: EMPTY/UNKNOWN → present | brand, spool_name, spoolman_id, spoolman_vendor_id, weights, color_name |
| `AmsBackendCfs` | `cfs` | `handle_status_update` tail loop | Composite `material_type\|color_value` fingerprint | spool_name, spoolman_id, spoolman_vendor_id, remaining_weight_g |

"Override-exclusive fields" are the fields the user can edit on that backend
but the firmware never supplies — they always come from the override, never
fall through.

AFC, Happy Hare, Tool Changer, and Mock inherit the no-op
`clear_slot_override` default from `AmsBackend`. They manage their own
override semantics independently — AFC and Happy Hare write `lane_data`
directly from their Klipper plugins, and HelixScreen does not touch those
records. For AFC that is not merely etiquette: AFC.py `delete_lane_data()`
wipes the whole namespace at the start of every PREP and refills it one lane at
a time, so the namespace is non-durable across reboots *and* transiently
incomplete during them. AFC/HH overrides go to a private namespace
(`OVERRIDE_NAMESPACE`) — see prestonbrown/helixscreen#1158.

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

The merge rule (firmware vs override) is documented in
[`../specs/filament_slots.md`](../specs/filament_slots.md#5-merge-policy).
Implementation-side there is exactly **one** implementation:
`helix::ams::merge_override()` (`include/filament_slot_override_store.h` +
`src/printer/filament_slot_override_store.cpp`). All six backends'
`apply_overrides` delegate to it (CFS keeps only its presence-promotion tail
locally) — the per-backend hand-rolled field-walk loops are gone. It carries
spec §5 (override wins field-by-field, sentinels fall through) plus the two
cross-field rules that can drop the **whole record** instead of merging:

- **Rule 1 — external re-bind.** Firmware reports a positive `spoolman_id`
  different from the override's → the whole record drops, firmware truth
  paints. Not gated by any capability or setting; can fire on any backend
  whose firmware reports a positive spool id (AFC, Happy Hare, and
  flat-schema CFS, which parses a per-slot id). Our own in-flight writes are
  exempt: backends record them via `AmsBackend::record_own_spool_write()` and
  feed `AmsBackend::own_write_expectation()` into `MergeOptions`'
  `suppress_rebind_firmware_{old,new}_id` — the id firmware last reported
  before the write and the just-written id don't fire; a third id fires and
  consumes the expectation.
- **Rule 2 — eject.** `AmsBackend::printer_reports_spool_ids()` (true only
  on AFC and Happy Hare) arms **only** this rule: there a firmware id ≤ 0
  while the override holds a positive id is the plugin's own eject signal,
  and it clears the record only when the `ams/keep_spool_info_on_eject`
  setting is off (default on — "Keep Spool Info on Eject" toggle in the AMS
  Management overlay, shown only where the capability is true). Caveat: with
  AFC's own retention on (`remember_spool = true` everywhere) firmware never
  reports the eject zero, so this rule cannot fire and the toggle is a no-op —
  `printer_retains_spool_info()` detects that shape and the overlay disables
  the toggle with a note instead.

Sentinel values that fall through to firmware:

| Field type | "Unset" sentinel |
|------------|------------------|
| `std::string` | empty string |
| `int` (ids) | 0 |
| `float` (weights) | -1.0 |
| `uint32_t` (color_rgb) | 0 |

The sentinels match what `FilamentSlotOverride` emits as "missing" on
serialization — ensuring a round-trip through JSON preserves the
override-vs-firmware distinction. When adding new fields to the struct, pick
a sentinel that a user could never legitimately enter for that field.

---

## 6. Clear semantics

Four distinct clear paths, handled separately:

- **User-initiated clear.** The edit modal's "Clear metadata" button calls
  `AmsBackend::clear_slot_override(slot_index)`. This is the public API
  contract; IFS/Snapmaker/ACE/CFS override it to DELETE their store entry.
- **Hardware-event clear.** Each backend watches its own signal (see the
  integration table) and auto-clears when the signal transitions to
  "different spool". The baseline is recorded on first observation after
  startup and NEVER triggers a clear on its own — otherwise every app launch
  would wipe overrides.
- **Merge-rule clears (re-bind / eject).** The two cross-field rules in
  `merge_override()` (§5) can drop the whole record during `apply_overrides`:
  an external re-bind (firmware reports a different positive spool id), or —
  only where `printer_reports_spool_ids()` is true and the setting is off —
  a firmware eject signal.
- **Self-wipe prevention.** When the user edits the color on IFS, the IFS
  backend pre-updates `last_firmware_color_` to the user's new RGB before
  pushing the override. Without this, the next `Adventurer5M.json` read
  would see a "color change" (because firmware still reports the original
  color briefly) and clear the override the user just saved. Snapmaker and
  CFS don't need this pre-update: `CARD_UID` and the composite fingerprint
  aren't user-editable.

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
- **Mock hooks for MR DB**: `mock_set_db_value`, `mock_get_db_value`,
  `mock_reject_next_db_{post,delete,get}`,
  `mock_defer_next_db_{post,delete,get}` + `fire_deferred_*`. The deferred
  variants are how we exercise the "callback fires after the store is
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
  - Per-backend `apply_overrides` helpers: `src/printer/ams_backend_ad5x_ifs.cpp`, `src/printer/ams_backend_snapmaker.cpp`, `src/printer/ams_backend_ace.cpp`, `src/printer/ams_backend_cfs.cpp`
