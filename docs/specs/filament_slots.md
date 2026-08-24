# Filament Slot Metadata — `lane_data` Convention

**Status**: Informational, v1.7 (2026-08). See [Changelog](#changelog).

This document describes HelixScreen's use of the `lane_data` Moonraker database
namespace to share per-slot filament metadata with OrcaSlicer and other tools.
It is published so third parties — firmware vendors, slicers, scales, spool
trackers — can interoperate with the same records without reverse-engineering
HelixScreen's source.

HelixScreen did **not** invent this convention. We document here what we write,
what we read, and the conservative rules we follow so adopters have a single
reference to implement against.

---

## 1. Overview

Moonraker exposes a per-instance JSON key/value store at
`/server/database/item`. The `lane_data` namespace, originally introduced by
[AFC (Armored Turtle Filament Changer)](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html),
holds one record per filament lane/slot. AFC writes these records from its
Klipper plugin; OrcaSlicer reads them (read-only — it never writes back) to
auto-populate filament presets when sending a print.

> **Happy Hare also writes `lane_data`.** Happy Hare's Moonraker component
> (`components/mmu_server.py`, `push_lane_data`) writes per-lane records into
> the `lane_data` namespace directly, using key names `vendor_name`, `name`,
> and `filament_id`. OrcaSlicer prefers this Moonraker `lane_data` source over
> the legacy live `mmu` Klipper status object (`GET
> /printer/objects/query?mmu`, fields
> `gate_status`/`gate_material`/`gate_color`/`gate_temperature`) when the
> namespace is populated. So Happy Hare is a `lane_data` writer alongside AFC
> and HelixScreen — its key convention (`vendor_name` / `name`) is the de-facto
> schema leader, since Orca follows it. HelixScreen emits those keys as aliases
> (see §3) for forward-compatibility.
>
> HelixScreen does **not** write `lane_data` records for AFC or Happy Hare
> backends — those plugins own their own lane records. "Clobber" understates it:
> AFC *empties the entire namespace* on every Klipper boot (`AFC.py`
> `delete_lane_data()`), then repopulates it one key at a time as each lane's
> PREP finishes, and full-POSTs a freshly-built dict on every lane event
> (`SET_COLOR`, `SET_MATERIAL`, `SET_MAP`, `set_spoolID`). Anything we wrote
> there would not survive a reboot, and would not even be reliably *readable*
> during the seconds-wide repopulation window. AFC/HH overrides therefore live
> in a private namespace; HelixScreen writes `lane_data` only for backends with
> no native writer (IFS, CFS, ACE, Snapmaker). See prestonbrown/helixscreen#1158.

HelixScreen participates as both reader and writer. When a user edits slot
metadata in HelixScreen's filament panel (brand, material, color, Spoolman
binding, weight), we persist that record to `lane_data` in the AFC-compatible
shape. OrcaSlicer 2.3.2+ reads it back on the next print and pre-selects the
correct filament preset — with no HelixScreen-specific integration on the
slicer side. Verified unchanged through OrcaSlicer 2.4.0-beta: the namespace
and the consumed fields (`lane`, `color`, `material`, `bed_temp`,
`nozzle_temp`) are identical to 2.3.2.

The convention is intentionally permissive:

- No schema enforcement. Readers parse what they understand, ignore the rest.
- No locking or transactional semantics. Last writer wins.
- `scan_time` (ISO-8601 UTC) is the advisory conflict-avoidance hint.

---

## 2. The `lane_data` convention

### Origin and consumers

| Project | Role | Reference |
|---------|------|-----------|
| AFC | Originator, writes from Klipper plugin | [AFC docs](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html) |
| OrcaSlicer 2.3.2+ | Reader only — never writes back (filament preset auto-sync) | [MoonrakerPrinterAgent.cpp, PR #12086](https://github.com/OrcaSlicer/OrcaSlicer/pull/12086) |
| HelixScreen | Reader + writer (this document) | `src/printer/filament_slot_override_store.cpp` |

Happy Hare also writes this namespace — its Moonraker component
(`components/mmu_server.py`, `push_lane_data`) emits per-lane records keyed with
`vendor_name` / `name` / `filament_id`, and OrcaSlicer prefers that Moonraker
source over the live `mmu` Klipper object when it is populated. See the note in
§1; HelixScreen mirrors Happy Hare's key names as aliases (§3).

### Moonraker endpoint

Fetch the whole namespace:

```
GET /server/database/item?namespace=lane_data
```

Response shape:

```json
{
  "result": {
    "namespace": "lane_data",
    "value": {
      "lane1": { "lane": "0", "color": "#FF5500", "material": "PLA", ... },
      "lane2": { "lane": "1", "color": "#10A0E0", "material": "PETG", ... }
    }
  }
}
```

Per-key get / post / delete:

```
GET    /server/database/item?namespace=lane_data&key=lane1
POST   /server/database/item      # body: { namespace, key, value }
DELETE /server/database/item      # body: { namespace, key }
```

### Top-level shape

One JSON object keyed by a per-slot identifier. Two key styles are in use
(§4): `laneN` (1-based, filament systems) and `T<n>` (0-based, tool changers).
Each value is an object conforming to the record shape in Section 3.

**The outer key is opaque; the inner `lane` field is authoritative.** Readers
recover a slot's index from the record's `lane` field (§3), never by parsing
the outer key. This is how OrcaSlicer reads the namespace
(`MoonrakerPrinterAgent.cpp:780` iterates values and never inspects the key),
and it is what makes the two key styles interoperable.

Non-record siblings may exist in the namespace (`seated`, other tools' config).
Readers must skip any value that is not a well-formed record — i.e. not an
object, or missing the `lane` field — rather than erroring. HelixScreen is
**key-agnostic**: it ingests any record with a parseable `lane` field
regardless of its outer key, so it reads records written by AFC, Happy Hare,
Mainsail, or anyone else. See §8 for the full reader/writer contract.

---

## 3. HelixScreen's emitted record

A full HelixScreen-emitted record looks like this:

```json
{
  "lane": "0",
  "color": "#1A1A1A",
  "material": "ASA",
  "helix_material": "ASA-GF",
  "vendor": "Polymaker",
  "vendor_name": "Polymaker",
  "spool_id": 42,
  "scan_time": "2026-04-18T12:34:56Z",
  "bed_temp": 90,
  "nozzle_temp": 250,

  "spool_name": "PolyLite ASA-GF Black",
  "name": "PolyLite ASA-GF Black",
  "spoolman_vendor_id": 7,
  "remaining_weight_g": 850.0,
  "total_weight_g": 1000.0,
  "color_name": "Black"
}
```

The top group is AFC-standard. The bottom group is HelixScreen's extensions.
Note the deliberate `material` / `helix_material` pair: `material` is `"ASA"`
(the string OrcaSlicer can match to a preset), while `helix_material` is the
precise `"ASA-GF"` identity HelixScreen shows on-device. See the field
reference below.

### Field reference

#### AFC-standard fields

| Field | Type | Required | Format / units | Semantics | Source |
|-------|------|----------|----------------|-----------|--------|
| `lane` | string | yes | stringified integer, 0-based | Tool / slot index as interpreted by the slicer. Matches OrcaSlicer's tool-index convention. See §4 for the intentional off-by-one versus the outer DB key. | HelixScreen writes slot index as string. |
| `color` | string | optional | `#RRGGBB` hex | Slot color. Leading `#` is conventional; HelixScreen's parser also accepts `0x`-prefixed forms on read. Emitted only when the override carries a non-zero RGB. | User-edited, or firmware-reported on backends where the user has no override. |
| `material` | string | optional | short code (`PLA`, `PETG`, `ABS`, `TPU`, …) | The **slicer-matchable** material string. OrcaSlicer matches a lane to a preset by this value alone, so a writer should emit a string the slicer's library actually carries. HelixScreen derives it from the user's precise type (see `helix_material`): explicit override → exact library type → base polymer, and **omits the field entirely** when nothing safely matches, rather than emit a string the slicer would resolve to a wrong (Generic PLA) preset. Readers should still treat unknown values as opaque strings — do NOT silently map them. | Derived by HelixScreen (`orca_match_type()`), or user-edited on writers without a match table. |
| `vendor` | string | optional | free-form | Brand / manufacturer. Readers match case-insensitively when pairing with their own filament databases. | User-edited. |
| `vendor_name` | string | optional | free-form | Alias of `vendor`, mirroring Happy Hare's key convention (`push_lane_data` in `components/mmu_server.py`). Emitted with the same value as `vendor` for forward-compat: as OrcaSlicer moves toward vendor-aware preset matching, `vendor_name` is the key it is most likely to consume. Zero-cost today — Orca ignores unknown keys. HelixScreen's reader accepts either key. | Same as `vendor`. |
| `spool_id` | integer | optional | positive integer | Spoolman spool ID for the physical spool currently loaded. Omitted when zero. | User-selected from Spoolman, if configured. |
| `scan_time` | string | optional | ISO-8601 UTC, second precision (`YYYY-MM-DDTHH:MM:SSZ`) | Last time this record was written or scanned. Advisory only — used for conflict avoidance, not for mutual exclusion. Sub-second fractions are truncated. | `std::chrono::system_clock::now()` at save time. |
| `bed_temp` | integer | optional | Celsius | Recommended bed temperature. | User entry, bound Spoolman spool's filament profile, or HelixScreen's internal material DB (in that priority order). |
| `nozzle_temp` | integer | optional | Celsius | Recommended nozzle temperature. When derived from a Spoolman spool's min/max range, the midpoint is emitted. | Same priority order as `bed_temp`. |

OrcaSlicer (2.3.2 through 2.4.0-beta, verified) only consumes `lane`, `color`,
`material`, `bed_temp`, and `nozzle_temp`. All other fields are additive and
must be silently ignored by compliant readers.

#### HelixScreen extension fields

These are additive and namespaced into the same record object. Other tools
reading `lane_data` should ignore any they don't understand (the "safe_json_*"
discipline that OrcaSlicer's `MoonrakerPrinterAgent` uses internally — never
throw on unknown keys).

| Field | Type | Required | Format / units | Semantics | Source |
|-------|------|----------|----------------|-----------|--------|
| `helix_material` | string | optional | free-form material name | The **precise** material identity the user chose (`ASA-GF`, `PLA Silk`, `PPS-CF`), which may be more specific than the slicer-matchable `material`. HelixScreen's reader prefers this over `material`, so the on-device display keeps the exact type even when `material` was reduced (or omitted) for slicer matching. OrcaSlicer and other readers ignore it. Emitted unconditionally when HelixScreen authors the record. | HelixScreen (`to_lane_data_record()`). |
| `spool_name` | string | optional | free-form | Human-readable name for the spool (e.g. `"PolyLite ASA-GF Black"`). Distinct from `vendor` + `material` because users often want a friendlier label. | User-edited, or auto-filled from Spoolman. |
| `name` | string | optional | free-form | Alias of `spool_name`, mirroring Happy Hare's key convention (`push_lane_data` in `components/mmu_server.py`). Emitted with the same value as `spool_name` for forward-compat. HelixScreen's reader accepts either key. | Same as `spool_name`. |
| `spoolman_vendor_id` | integer | optional | positive integer | Spoolman vendor ID, paired with `spool_id` for full Spoolman round-tripping. Omitted when zero. | From Spoolman when a spool is selected. |
| `remaining_weight_g` | float | optional | grams | Remaining filament weight. Negative = unset / unknown. | Spoolman, or user-entered. |
| `total_weight_g` | float | optional | grams | Full-spool nominal weight. Negative = unset / unknown. | Spoolman, or user-entered. |
| `color_name` | string | optional | free-form | Human-readable color label (e.g. `"Orange"`), distinct from the `color` hex value. Some user workflows care about the marketing name as well as the RGB. | User-edited, or auto-filled from Spoolman. |

Fields are emitted only when present. Empty strings, zero, and negative floats
are treated as "not set" and omitted from the written record — reducing noise
and making future schema evolution easier.

---

## 4. Key mapping and indexing

The outer DB key and the inner `lane` field are two distinct indices for the
same slot. The **inner field is always 0-based and authoritative**; the outer
key's base depends on the writer's key style.

| Key style | Outer DB key | Outer base | Inner `lane` field | Example for slot 0 | Written by |
|-----------|--------------|------------|--------------------|--------------------|------------|
| `laneN` | `"lane" + (i+1)` | 1-based | `std::to_string(i)` (0-based string) | key `"lane1"`, field `"0"` | HelixScreen (filament systems), Happy Hare, AFC before its virtual-tools firmware |
| `T<n>` | `"T" + i` | 0-based | `std::to_string(i)` (0-based string) | key `"T0"`, field `"0"` | HelixScreen (tool changers), Mainsail #2510, AFC since its virtual-tools firmware |

The `laneN` style matches AFC's on-disk layout (AFC labels its lanes `lane1`,
`lane2`, … in its own config — that config naming is unaffected by AFC's move
to `T<n>` *keys*, which is a `lane_data` change only). The `T<n>` style matches the tool-index naming
Mainsail and OrcaSlicer use (`T0`, `T1`, …). **Both styles carry the identical
0-based stringified inner `lane` field** — the only difference is the outer key
and its base, which readers treat as opaque.

Why a tool changer uses `T<n>`: Mainsail's Spoolman + toolchanger integration
(PR #2510) writes its records keyed `T<n>`. A HelixScreen tool changer that
also wrote `laneN` for the same slot would produce **two records with the same
inner `lane`** — which OrcaSlicer ingests as duplicate trays (it does not
dedup; see §8). Converging on the `T<n>` key makes the two writers overwrite
one shared key instead of colliding.

### The external / bypass spool lane (`i == num_gates`)

HelixScreen publishes the external (bypass) spool as the lane **one past the
last physical slot** — inner `lane` field `"N"`, outer key in the writer's
normal style for that index — so a slicer renders it as one more tray beyond
the physical bays (T4 beside T0–T3, `lane5` beside `lane1`–`lane4`). This is
what makes "print this single-tool file from the external spool" a *selectable*
choice in OrcaSlicer rather than an error.

Rules:

- **Written by HelixScreen only.** No filament-system plugin publishes its own
  extern entry (verified in AFC's `AFC_lane.send_lane_data` — mapped lanes
  only — and Happy Hare's `mmu_server.py push_lane_data` — gates only).
- **Ephemerality is expected and self-healing.** AFC deletes the whole
  namespace at boot (`AFC.py delete_lane_data()`) and Happy Hare's boot-time
  cleanup deletes any record with `lane >= num_gates`. HelixScreen does NOT
  fight either writer: it re-publishes on its triggers (bypass engage,
  external-spool identity change), accepting that the entry disappears until
  then after a firmware-plugin restart.
- **Identity-gated.** A record with no Spoolman id, no material, and no picked
  color (default-gray) clears the lane instead of publishing a phantom tray;
  pure black is a real pick.
- Backends with no bypass capability (`supports_bypass` false) never publish
  one — there is no external feed to select.

HelixScreen's writer produces the key via
`format_lane_key(i, style)` (`"T"+i` for Tool, `"lane"+(i+1)` for Lane) and the
inner field via `std::to_string(i)`. Readers recover the 0-based slot index
from the inner `lane` field and use it as the canonical identifier; the outer
key is opaque.

Negative values in the inner `lane` field are rejected on read. This matches
`MoonrakerPrinterAgent.cpp:796`.

---

## 5. Merge policy

HelixScreen combines three sources of slot metadata:

1. **Firmware-reported state** — what the printer's AMS/IFS/CFS/AFC plugin
   thinks is loaded (read from Klipper objects or vendor REST APIs).
2. **Override records** — the `lane_data` entries written by HelixScreen or
   any other well-behaved writer.
3. **User edits in progress** — in-memory, not yet persisted.

The merge rule is **override-wins, field-by-field**:

- If an override record contains a non-empty / non-zero / non-negative field,
  it replaces the firmware-reported value for that field in the UI.
- If a field is missing, empty, zero, or negative in the override, the
  firmware value falls through.
- User edits are committed to the override record atomically on save; there is
  no partial-edit state on disk.

**Amendment (v1.7):** when firmware itself reports a per-lane `spool_id` that
differs from a reader's stored override, the firmware value is authoritative —
HelixScreen drops its whole override record for that lane rather than shadowing
the external write. An override survives only an absent/zero firmware id
(ejection), which HelixScreen makes user-configurable per system (§6).

A tool reading these records does not need to replicate HelixScreen's merge
rule — it is documented here so third parties understand why we emit only the
fields we do, and why we omit defaulted fields rather than writing zeros.

---

## 6. Hardware-event clearing

HelixScreen automatically clears its own override records when a backend
detects that the *physical* spool in a slot has changed. This prevents stale
"user said this was orange PLA" metadata from surviving a spool swap the user
never re-entered in the UI.

Per backend:

| Backend | Signal | Rationale |
|---------|--------|-----------|
| AD5X IFS | `Adventurer5M.json` color transition to a materially different RGB | No RFID on IFS; color is the only spool-identity signal firmware exposes. |
| Snapmaker U1 | `CARD_UID` change on the RFID tag | Snapmaker tags every spool with a unique UID — the cleanest available fingerprint. |
| CFS | Per-slot composite `material_type|color_value` fingerprint change | CFS rewrites both fields from a server-side RFID lookup, so their concatenation is a stable spool fingerprint. |
| ACE | Slot status transition `EMPTY → present` | ACE has no RFID or stable tag; the only reliable "different spool" proxy is an empty-to-loaded transition. |
| AFC | Per-lane `spool_id` reported by the AFC plugin: a *different positive* id, or `0`/absent | The plugin owns the lane↔spool binding and reports the loaded spool's id. A different positive id is a re-bind, cleared via the §5 amendment; `0`/absent is the plugin's own eject signal. |
| Happy Hare | Per-gate `spool_id` (SPOOLID) reported by the MMU: same split as AFC | Same authority argument as AFC — the MMU reports the bound spool's id per gate. |

Clearing is a `DELETE` on the slot's `lane_data` key. The first observation
after startup establishes the baseline fingerprint and is NOT treated as a
swap — otherwise every app launch would wipe overrides.

Where firmware reports a spool id while a spool is loaded (AFC, Happy Hare),
the eject half of that signal is user-configurable in HelixScreen: the
"Keep Spool Info on Eject" setting (default on) retains the record across an
eject when enabled and clears it when disabled. The re-bind half is not
configurable — a firmware-reported *different positive* `spool_id` clears via
the §5 amendment on **any** backend whose firmware reports one, including CFS
firmware variants that publish a per-slot `spool_id` (flat schema).

Third-party writers do **not** need to implement this behavior. It is
documented here for transparency so readers understand why HelixScreen-authored
records may disappear between sessions.

---

## 7. Third-party adoption

### If you want to read records

Fetch the namespace in a single GET:

```
GET /server/database/item?namespace=lane_data
```

Iterate the returned object. For each value:

1. Confirm it's a JSON object.
2. Read `lane` (string or integer). Reject negative values.
3. Pull whichever fields you need (`color`, `material`, `vendor`,
   `spool_id`, `bed_temp`, `nozzle_temp`, etc.).
4. Silently ignore any keys you don't recognize — the namespace is shared
   and future schema extensions are expected.

The `scan_time` field is your conflict-avoidance signal: if you cache records
locally, compare `scan_time` to your cached copy before overwriting.

### If you want to write records

Write AFC-shaped records using `POST /server/database/item`:

```json
{
  "namespace": "lane_data",
  "key": "lane1",
  "value": {
    "lane": "0",
    "color": "#1A1A1A",
    "material": "ASA",
    "helix_material": "ASA-GF",
    "vendor": "Polymaker",
    "spool_id": 42,
    "scan_time": "2026-04-18T12:34:56Z",
    "bed_temp": 90,
    "nozzle_temp": 250
  }
}
```

Guidelines:

- **Always emit** `lane`. OrcaSlicer requires it.
- **Emit a slicer-matchable `material`.** OrcaSlicer picks a preset by the
  `material` string alone; an unmatched string resolves to a Generic PLA preset
  (wrong temperatures), not a near miss. Emit a value the slicer's library
  carries, and omit the field rather than emit an unmatchable one. If you also
  track a more precise type, carry it in `helix_material` — readers that
  understand it can show the exact identity while the slicer still matches on
  `material`.
- **Stamp** `scan_time` on every write. Use ISO-8601 UTC with second
  precision; other readers will rely on it.
- **Pick one key style and stay consistent within a slot.** Use `laneN`
  (1-based outer key) for filament systems or `T<n>` (0-based outer key) for
  tool changers; either way the inner `lane` field is 0-based (§4). The outer
  key is opaque to readers — OrcaSlicer never inspects it
  (`MoonrakerPrinterAgent.cpp:780`), so a "wrong" outer key does **not** desync
  a reader. What *does* cause trouble is writing the **same inner `lane` under
  two different outer keys**: OrcaSlicer has no dedup and shows both as separate
  trays (§8). Converging with other writers on one key style for a given slot
  avoids that.
- **Add extension fields freely.** Other tools will ignore them. If your
  extension becomes broadly useful, open a documentation PR here or in the
  AFC docs so the convention grows deliberately.
- **Best-effort only.** No transactions, no locking. If two writers race,
  last write wins. Use `scan_time` to avoid clobbering fresher data when
  you can.

### Conflict avoidance

The convention is cooperative, not transactional. The sharpest tool available
is `scan_time`:

```
if (remote.scan_time > my_cache.scan_time) {
    // Remote has fresher data — merge or skip.
} else {
    // Safe to overwrite.
}
```

Clock skew between the printer and your writer can defeat this; treat
`scan_time` as advisory.

---

## 8. Interoperating readers and writers

`lane_data` is a **shared, cooperative namespace**: several tools write to it
and several read from it, with no central coordinator. This section documents
the three-way contract as ground truth — every claim below was verified against
the tools' source, **not** their PR or release descriptions (a PR broadening a
TypeScript type is not evidence the wire format changed). When in doubt,
re-verify against the cited source lines rather than this table.

### Writers

| Writer | Key style | Notes |
|--------|-----------|-------|
| **HelixScreen** | `T<n>` on tool changers, `laneN` otherwise | `format_lane_key(i, style)` in `filament_slot_override_store.cpp`. Style is derived from the AMS type (`lane_key_style_for`), not hardcoded per backend. |
| **AFC** (AFCProject) | `T<n>` since the virtual-tools firmware (DEV 2026-08, Klipper-Add-On #832); `laneN` before | `AFC_lane.py` `send_lane_data` writes one record **per T(n) mapping**, so a multi-mapped lane appears once per tool. Inner `lane` field is the tool number string. See below. |
| **Happy Hare** | `laneN` | `components/mmu_server.py` `push_lane_data`; also emits `vendor_name` / `name` / `filament_id` inner fields. |
| **Mainsail #2510** | `T<n>` | Writes `lane_data` records for plain Spoolman + tool changer setups, keyed by tool (`T0`, `T1`, …). This is why HelixScreen tool changers converge on `T<n>`. |

#### Shipped: AFC moved from `laneN` to `T<n>` (virtual-tools firmware)

**Status: on upstream `DEV` as of 2026-08-16** (Klipper-Add-On #832, which
also became the 1.3 release line). Verified in source: `AFC_lane.py`
`send_lane_data` now iterates `_mapped_keys()` and POSTs one record per `T(n)`
currently mapped to the lane, keyed by that `T(n)` — a lane mapped to
`T16,T17` produces two records with identical contents. A lane-name key cannot
express that, which is why the key changed. The inner `lane` field changed
meaning with it: it now carries the **tool number** string
(`key.replace("T", "")`), not a lane index. Stale `T(n)` keys are removed
incrementally when no lane claims them anymore (`_sent_lane_data_keys`), in
addition to the boot-time full-namespace wipe (`AFC.py` `delete_lane_data`).

Same firmware, same records: #808 adds `vendor_name`, `name`, and
`initial_weight`, using Happy Hare's spelling for the first two. That closes
the gap where Spoolman knows a lane's brand but `lane_data` only carried its
material.

Consequences, in order of importance:

1. **Readers that join on the inner `lane` field are unaffected on 1:1
   mappings** — tool *n*'s record says `"lane": "<n>"`, which is where a
   sequential mapping put it anyway. Upstream verified OrcaSlicer's filament
   sync still works against this shape. On remapped or virtual-tool setups the
   tray grid follows the *tool* numbering, which is the correct behavior for a
   slicer-side view.
2. **A reader must not treat a `T(n)` outer key as a lane name.** A reader
   that bootstraps its slot model from the namespace's keys would create
   "slots" named after tools. HelixScreen's live-data reader resolves `T(n)`
   keys through the tool mapping the firmware reports in its status objects —
   and parks (replays later) any payload that arrives before a mapping does,
   because the namespace query is one-shot. Its override-store reader
   (`from_lane_data_record`) was already key-agnostic and needed no change;
   HelixScreen does not author records for AFC printers either way.
3. **The upgrade window is self-clearing.** The boot-time
   `delete_lane_data()` removes every key in the namespace before
   republishing, so stale `laneN` records cannot linger alongside new `T<n>`
   ones and produce the duplicate-tray collision described in §8.
4. **Key-space overlap with tool-changer writers.** `laneN` and `T<n>`
   previously kept AFC and the tool-changer writers (Mainsail, HelixScreen) in
   disjoint key spaces. With AFC on `T<n>`, an AFC lane and a tool-changer slot
   with the same index become the *same key*, and AFC's boot-time delete will
   remove a foreign `T<n>` record. Unlikely to bite in practice (an AFC user is
   not also running a tool-changer writer on the same printer) but it is a
   failure mode that did not exist before.

### Readers

| Reader | Behavior (verified against source) |
|--------|-----------------------------------|
| **OrcaSlicer** | `MoonrakerPrinterAgent::fetch_moonraker_filament_data`. **Key-opaque**: iterates `result.value.items()` (`:780`) and never reads the outer key — the slot index comes from the inner `lane` field (`:786`). The inner `lane` **must be a JSON string**: `safe_json_string()` (`:661`) is `is_string()`-guarded with no coercion, so an integer `lane` is silently dropped (`:796`). **No deduplication**: `trays.push_back()` (`:813`) is unconditional and the grid bind (`:494`) is first-match-wins over nlohmann's alphabetically-sorted keys. Color parsing (`normalize_color_value`, `:691`) strips a leading `#`, so `#RRGGBB` is fine. Orca does not read Spoolman or the `gcode_macro` namespace. |
| **HelixScreen** | `load_blocking` in `filament_slot_override_store.cpp`. **Key-agnostic**: ingests any record whose inner `lane` parses, regardless of outer key. **Canonical-preferring on duplicates**: when two keys describe the same slot, keeps the record whose key is canonical for this backend's own style (first canonical wins; a canonical beats a non-canonical; otherwise the incumbent stays). This is order-independent and agrees with Orca's alphabetical first-wins in every case that can occur. Tool-changer backends additionally migrate their own stale `laneN` records to `T<n>` on load (dropping, not overwriting, a `laneN` when the canonical `T<n>` already exists). |

### The collision rule

Because Orca keys off the **inner** `lane` field and does **not** dedup, two
records that share the same inner `lane` under two different outer keys (e.g. a
stale `lane1` and a fresh `T0`, both `"lane": "0"`) appear in Orca as **two
trays for one slot**. `"T0"` sorts before `"lane1"`, so Orca's first-match-wins
would show the `T0` record — but the duplicate is still visually present.

The fix is not on the reader side (readers cannot know two keys mean one slot):
**writers must converge on one key style per slot.** HelixScreen does this by
writing `T<n>` on tool changers (matching Mainsail) and migrating away any
stale `laneN` it previously wrote. A third party that keeps rewriting a
different key for the same slot will produce a permanent duplicate that no
reader can resolve.

---

## 9. Reference implementations

| Project | File | Role |
|---------|------|------|
| HelixScreen | `src/printer/filament_slot_override_store.cpp` | Reader + writer. `to_lane_data_record` / `from_lane_data_record` for record shape; `load_blocking` / `save_async` / `clear_async` for namespace I/O. |
| OrcaSlicer 2.3.2–2.4.0-beta | `src/slic3r/Utils/MoonrakerPrinterAgent.cpp:727-822` (`fetch_moonraker_filament_data`) | Reader (filament preset auto-sync on print send). Canonical reference for AFC-standard field semantics. Unchanged across this range. |
| AFC (Armored Turtle) | Klipper plugin | Native writer. See [AFC docs](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html). |
| Happy Hare | `components/mmu_server.py` (`push_lane_data`) | Native writer. Writes `lane_data` records keyed with `vendor_name` / `name` / `filament_id`. OrcaSlicer prefers this Moonraker source over the live `mmu` Klipper object (`fetch_hh_filament_info`, `MoonrakerPrinterAgent.cpp:825-950`) when the namespace is populated. HelixScreen mirrors HH's `vendor_name` / `name` key convention as aliases. |

---

## Changelog

- **v1.7 (2026-08-17)**: §5 amendment — firmware-authoritative re-bind: when
  firmware reports a per-lane `spool_id` that differs from a stored override,
  HelixScreen drops its whole override record for that lane instead of
  shadowing the external write. The rule fires on any backend whose firmware
  reports a positive id (AFC, Happy Hare, and flat-schema CFS firmware that
  publishes a per-slot `spool_id`). §6 gained the AFC and Happy Hare rows —
  their firmware-reported spool id doubles as the hardware-event signal
  (different positive id → §5 re-bind clear; `0`/absent → eject, there
  user-configurable via HelixScreen's "Keep Spool Info on Eject", default on).
- **v1.7 (2026-08-18)**: Documented the **external / bypass spool lane**
  (§4): HelixScreen publishes the extern spool as the lane one past the last
  physical slot (`lane == num_gates`) on backends whose bypass it controls
  (CFS, AD5X IFS via their mirror stores; AFC and Happy Hare via dedicated
  shared-namespace stores). No third-party plugin publishes its own extern
  entry, and both AFC's boot-time namespace delete and Happy Hare's boot-time
  `lane >= num_gates` cleanup are documented as expected, self-healing
  ephemerality. No wire-format change.
- **v1.6 (2026-08-16)**: AFC's virtual-tools firmware (Klipper-Add-On #832, on
  `DEV`, 1.3 release line) switched its `lane_data` keys from `laneN` to
  `T<n>`, one record per mapped tool, with the inner `lane` field now the tool
  number string. The "announced" subsection became "shipped", re-verified
  against the upstream source (`AFC_lane.py` `send_lane_data` /
  `clear_lane_data` / `_mapped_keys`). Also records #808: AFC now publishes
  `vendor_name`, `name`, and `initial_weight` in the same records.
- **v1.5 (2026-07-20)**: Split filament material identity into two fields.
  `material` is now the **slicer-matchable** string (OrcaSlicer matches a lane
  to a preset by it alone; an unmatched value resolves to a Generic PLA preset,
  not a near miss), and HelixScreen derives it from the user's precise type —
  emitting a reduced string or omitting the field rather than a string the
  slicer would mismatch. The new `helix_material` extension field carries the
  **precise** identity (e.g. `ASA-GF`); HelixScreen's reader prefers it, so the
  on-device display keeps the exact type while the slicer still matches on
  `material`. HelixScreen also one-shot **heals** its own pre-existing records
  to this two-field form on load. Also bumped the status header (it lagged the
  changelog at v1.3).
- **v1.4 (2026-07-16)**: Documented the `T<n>` tool-changer key style alongside
  `laneN`, made the top-level shape (§2) and key mapping (§4) key-agnostic, and
  added §8 "Interoperating readers and writers" (the three-way writer/reader
  contract, verified against source). Corrected the previously-wrong normative
  claim that "breaking the 1-based key / 0-based field correspondence silently
  desyncs every other reader" — OrcaSlicer never reads the outer key
  (`MoonrakerPrinterAgent.cpp:780`), so the outer key is opaque; the real
  hazard is the same inner `lane` under two outer keys (Orca does not dedup).
  HelixScreen now writes `T<n>` on tool changers (converging with Mainsail
  #2510) and migrates its own stale `laneN` records to `T<n>` on load.
- **v1.3 (2026-06-18)**: Corrected the Happy Hare description — HH's Moonraker
  component (`components/mmu_server.py`, `push_lane_data`) writes `lane_data`
  records directly (keys `vendor_name` / `name` / `filament_id`), and
  OrcaSlicer prefers that Moonraker source over the live `mmu` Klipper object.
  HH is now documented as a `lane_data` writer alongside AFC and HelixScreen,
  not an out-of-scope separate path. HelixScreen now emits `vendor_name` (alias
  of `vendor`) and `name` (alias of `spool_name`) to mirror HH's key
  convention for forward-compat (Orca ignores unknown keys), and its reader
  accepts either spelling. HelixScreen still does **not** write `lane_data` for
  AFC / Happy Hare backends — those plugins own their own records.
- **v1.2 (2026-06-09)**: Verified against the OrcaSlicer 2.4.0-beta source
  (`MoonrakerPrinterAgent.cpp`): the `lane_data` namespace and the five consumed
  fields are unchanged from 2.3.2, and OrcaSlicer reads the namespace read-only
  (never writes back). Corrected the Happy Hare entries — HH lanes reach
  OrcaSlicer through Orca's live `mmu`-object reader (2.4.0+), not `lane_data`.
- **v1.1 (2026-04-28)**: HelixScreen now emits `bed_temp` and `nozzle_temp`
  on every save. Source priority: explicit user entry > bound Spoolman
  spool's filament profile > internal material database default (looked up
  by `material` name at write time). For `nozzle_temp` derived from a
  Spoolman min/max range, the midpoint is emitted as a single integer to
  match the AFC wire format.
- **v1 (2026-04)**: HelixScreen's initial adoption of the `lane_data`
  convention. AFC-standard fields (`lane`, `color`, `material`, `vendor`,
  `spool_id`, `scan_time`, `bed_temp`, `nozzle_temp`) plus HelixScreen
  extensions (`spool_name`, `spoolman_vendor_id`, `remaining_weight_g`,
  `total_weight_g`, `color_name`). `bed_temp` / `nozzle_temp` documented
  but not yet emitted (lifted in v1.1).
