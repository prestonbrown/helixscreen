# Filament Slot Metadata — `lane_data` Convention

**Status**: Informational, v2.0 (2026-11). See [Changelog](#changelog).

This document describes how HelixScreen reads and writes the `lane_data`
Moonraker database namespace, the per-lane filament records OrcaSlicer and other
tools share. It is published so third parties (firmware vendors, slicers,
scales, spool trackers) can interoperate with the same records without
reverse-engineering HelixScreen's source.

HelixScreen did **not** invent this convention. Since v2.0 this document follows
**RFC 0001, Lane Status Surface** (the `lane_data` half, its §5, and the record
fields of its reference, Part B §7) and records where HelixScreen stands on each
of its rules.

---

## 1. Overview

Moonraker exposes a per-instance JSON key/value store at
`/server/database/item`. The `lane_data` namespace, originally introduced by
[AFC (Armored Turtle Filament Changer)](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html),
holds one record per filament lane. OrcaSlicer reads it (it never writes back)
to pick filament presets when sending a print.

RFC 0001 splits a lane's state in two:

- **Live state**: what the firmware reports in Klipper status objects (presence,
  the active lane, errors, and on most filament systems the spool's identity).
- **`lane_data`**: the identity half, one record per lane in the Moonraker
  database, which is what a slicer and a second screen read.

Three rules from RFC 0001 §5 shape everything below:

- **One writer per machine.** A producer sets `writer` and never overwrites a
  record another live producer wrote. A record with no `writer` is legacy and
  may be replaced.
- **One record per lane**, kept in step with the loaded spool. An unknown value
  is `null`, never a placeholder.
- **Ignore the key.** Keys differ between writers (§4); a reader uses the
  record's `lane` field.

### Where HelixScreen writes and where it only reads

HelixScreen keeps no lane state of its own: a lane shows what the firmware and
Moonraker hold, and an edit is written there before the screen treats it as
saved.

| Printer | `lane_data` writer | What HelixScreen does |
|---------|--------------------|-----------------------|
| AFC | AFC's plugin | Reads only. Edits go to AFC as G-code (`SET_SPOOL_ID`, `SET_COLOR`, `SET_MATERIAL`, `SET_WEIGHT`, `SET_MAP`), and AFC republishes its records. Fields AFC cannot hold (brand, name, colour name, catalog pick, total weight) live in HelixScreen's own namespace, `helix-screen-afc-overrides`. The external spool's lane (§4) is HelixScreen's |
| Happy Hare | Happy Hare's Moonraker component | Reads only. Edits go to the gate map with `MMU_GATE_MAP`. Fields the gate map cannot hold live in `helix-screen-hh-overrides` |
| OpenAMS with `[openams_spoolman]` | The component (a namespace Moonraker protects from clients) | Reads only. Edits go through the component's `server.openams_spoolman.edit` endpoint, and each bay's spool comes from the component's own status, since its records are one per tool, not per bay |
| CFS, AD5X IFS, QIDI Box, Snapmaker U1, ACE, tool changers, OpenAMS without the component | HelixScreen | Writes the record, after writing what the firmware holds (colour, material and the like) with the printer's own commands |

The two HelixScreen namespaces for AFC and Happy Hare hold records of the same
shape (§3), narrowed to the fields their firmware cannot hold; a spool id,
material or colour found there is ignored, because the firmware's value wins.

The convention is permissive in what it carries:

- No schema enforcement. Readers parse what they understand and ignore the rest.
- No locking. The writer rule (§5) is what keeps two writers apart.
- `scan_time` (ISO-8601 UTC) records the last write.

---

## 2. The `lane_data` convention

### Origin and consumers

| Project | Role | Reference |
|---------|------|-----------|
| AFC | Originator, writes from its Klipper plugin | [AFC docs](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html) |
| Happy Hare | Writes from its Moonraker component (components/mmu_server.py, `push_lane_data`), with the `vendor_name` / `name` keys RFC 0001 adopted | Happy Hare source |
| OrcaSlicer 2.3.2+ | Reader only (filament preset sync) | [MoonrakerPrinterAgent.cpp, PR #12086](https://github.com/OrcaSlicer/OrcaSlicer/pull/12086) |
| HelixScreen | Reader everywhere; writer where §1 says | `src/printer/filament_slot_override_store.cpp` |

OrcaSlicer (2.3.2 through 2.4.0-beta, verified) reads `lane`, `color`,
`material`, `bed_temp` and `nozzle_temp`; builds carrying
[PR #14423](https://github.com/OrcaSlicer/OrcaSlicer/pull/14423) also read a
string `filament_id` and select the preset carrying it.

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

One JSON object keyed by a per-lane identifier. **The key is advisory; the
record's `lane` field is authoritative** (RFC 0001 S3). Keys vary in the wild
(RFC 0001 reference V-S3, §4 below), so a reader recovers a lane's index from
`lane` and never by parsing the key. This is how OrcaSlicer reads the namespace:
its MoonrakerPrinterAgent.cpp iterates the values and never inspects a key.

Non-record siblings may sit in the namespace (`seated`, another tool's
`source`). A reader skips any value that is not an object or has no parseable
`lane`, rather than erroring. HelixScreen reads any record with a parseable
`lane` whatever its key.

---

## 3. HelixScreen's record

A record HelixScreen writes follows RFC 0001 §5 and the field table of its
reference, Part B §7:

```json
{
  "lss_version": "0.1",
  "writer": "helixscreen",
  "lane": "0",
  "lane_name": "lane1",
  "loaded": true,
  "color": "#1A1A1A",
  "material": "ASA",
  "filament_type": "ASA-GF",
  "vendor_name": "Polymaker",
  "name": "PolyLite ASA-GF",
  "spool_id": 42,
  "spoolman_filament_id": 55,
  "filament_id": null,
  "bed_temp": 90,
  "nozzle_temp": 250,
  "remaining_weight_g": 850.0,
  "total_weight_g": 1000.0,
  "scan_time": "2026-11-07T12:34:56Z",
  "helix_color_name": "Black"
}
```

Every key in the table below is written on every record; a value HelixScreen
does not know is `null` (RFC 0001 V-S2), never `""`, `0` or a placeholder.
`helix_`-prefixed keys (RFC 0001 V-S1) are written only when they hold a value.

### Field reference

| Field | Type | Meaning |
|-------|------|---------|
| `lss_version` | string | `"0.1"`, the RFC 0001 record version. |
| `writer` | string | `"helixscreen"`. See §5 for what HelixScreen does with another writer's name. |
| `lane` | string | The 0-based lane index, as a JSON string (OrcaSlicer drops a numeric `lane`). Negative values are never written and are rejected on read. |
| `lane_name` | string | The record's key, `laneN` or `T<n>` (§4). |
| `loaded` | bool or null | Whether a spool is in the lane; `null` when the lane has never reported. |
| `color` | string or null | `#RRGGBB`. Pure black `#000000` is a colour, not an unset one. |
| `material` | string or null | The **slicer-matchable** type. OrcaSlicer matches a lane to a preset by this string alone and falls back to a Generic PLA preset on a miss, so HelixScreen writes the exact library type or its base polymer, and `null` when nothing matches safely. |
| `filament_type` | string or null | The precise type the user chose (`ASA-GF`, `PLA Silk`), which `material` may have reduced for matching. |
| `vendor_name` | string or null | Manufacturer. |
| `name` | string or null | Product name. |
| `spool_id` | int or null | Spoolman spool id. |
| `spoolman_filament_id` | int or null | The Spoolman filament record behind `spool_id`. |
| `filament_id` | string or null | OrcaSlicer's filament id. On a lane linked to a Spoolman spool whose filament carries the `orca_filament_id` extra (RFC 0001 §6), HelixScreen writes that id, since the tracker wins on identity, and marks it with `helix_filament_id_source: "spoolman"`. Otherwise it writes the id its catalog records for the user's catalog pick when no other product carries the same id. It never guesses one from a brand or name, and never writes a Bambu `GF*` code or a numeric value. |
| `bed_temp`, `nozzle_temp` | int or null | Celsius. From the user's entry, the linked spool's filament, or HelixScreen's material table, in that order; a range is written as its midpoint. |
| `remaining_weight_g`, `total_weight_g` | number or null | Grams. |
| `scan_time` | string | ISO-8601 UTC, second precision, stamped on every write. |
| `helix_color_name` | string | The colour's name (`"Black"`). |
| `helix_catalog_id`, `helix_product_name` | string | The HelixScreen catalog product the user picked. |
| `helix_spoolman_vendor_id` | int | The Spoolman vendor id. |
| `helix_filament_id_source` | string | `"spoolman"` when `filament_id` came from the linked spool (above). |
| `helix_fingerprint` | string | The spool identity HelixScreen's swap detection last observed (§6). Not filament metadata; carry it through unchanged on a rewrite. |
| `helix_external` | bool | `true` on the external spool's lane (§4). |

### Reading records HelixScreen did not write

HelixScreen reads any record, its own older ones included, and accepts the
spellings writers have used: `filament_type`, then `helix_material`, then
`material` for the type; `vendor_name` or `vendor`; `name` or `spool_name`;
`helix_color_name` or `color_name`; `spoolman_filament_id` or
`helix_spoolman_filament_id`, and a **numeric** `filament_id`, which is Happy
Hare's way of writing Spoolman's filament id. A string `filament_id` is
another tool's Orca id and is read back only from HelixScreen's own record with
`helix_filament_id_source: "spoolman"`.

---

## 4. Key mapping and indexing

The outer DB key and the inner `lane` field are two distinct indices for the
same slot. The **inner field is always 0-based and authoritative**; the outer
key's base depends on the writer's key style.

| Key style | Outer DB key | Outer base | Inner `lane` field | Example for slot 0 | Written by |
|-----------|--------------|------------|--------------------|--------------------|------------|
| `laneN` | `"lane" + (i+1)` | 1-based | `std::to_string(i)` (0-based string) | key `"lane1"`, field `"0"` | HelixScreen (filament systems), AFC before its virtual-tools firmware |
| `laneN` (0-based) | `"lane" + i` | 0-based | `str(gate)` (0-based string) | key `"lane0"`, field `"0"` | Happy Hare (components/mmu_server.py `push_lane_data`) |
| `T<n>` | `"T" + i` | 0-based | `std::to_string(i)` (0-based string) | key `"T0"`, field `"0"` | HelixScreen (tool changers), Mainsail #2510, AFC since its virtual-tools firmware |

The `laneN` style matches AFC's on-disk layout (AFC labels its lanes `lane1`,
`lane2`, … in its own config — that config naming is unaffected by AFC's move
to `T<n>` *keys*, which is a `lane_data` change only). The `T<n>` style matches the tool-index naming
Mainsail and OrcaSlicer use (`T0`, `T1`, …). **Both styles carry the identical
0-based stringified inner `lane` field** — the only difference is the outer key
and its base, which readers treat as opaque. Happy Hare's `lane1` is gate 1,
HelixScreen's `lane1` is slot 0; the two never share a printer's namespace,
because HelixScreen writes no records for a Happy Hare backend (§1).

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
OrcaSlicer's MoonrakerPrinterAgent.cpp:796.

---

## 5. The writer rule and what a lane shows

### Writing

- HelixScreen writes `lane_data` only on the printers §1 names it the writer
  for, and only after the firmware fields of an edit have gone to the firmware.
- **It never overwrites a record whose `writer` names another producer**
  (RFC 0001 S2). An edit aimed at such a lane is refused, and the screen says
  so; nothing is queued to retry. A record with no `writer`, which includes
  every record older tools and older HelixScreen builds wrote, is legacy and
  HelixScreen may replace it.
- A record is written whole, one key per lane, so edits on different lanes
  never race. A write that fails is reported where the edit was made and is not
  retried later; HelixScreen keeps no local copy to retry from.

### Reading

HelixScreen reads each namespace it uses (`lane_data`, plus
`helix-screen-afc-overrides` or `helix-screen-hh-overrides` on those printers)
when it connects, after each of its own writes, and every 10 seconds, since
Moonraker sends no notification when a database key changes. So a second
screen, Mainsail, a scale or a slicer that rewrites a record is seen within
about 10 seconds.

### What a lane shows

For each field, a lane shows the first of these that has a value:

1. an edit made on this screen whose write has not been confirmed yet;
2. what the firmware reports;
3. the `lane_data` record (or the AFC / Happy Hare namespace);
4. Spoolman's details for the spool the lane is linked to.

The linked spool is the one the edit, the firmware or the record names, in that
order. Spoolman's details count only for that spool: a Spoolman record for any
other id fills nothing. Weights follow the same order except that Spoolman's
remaining weight ranks above the firmware's and the record's, and HelixScreen's
own consumption meter fills a weight nothing else states.

An edit stays "unconfirmed" until a firmware report or a record read states the
same value, until its write fails, or for 30 seconds, whichever comes first. In
the last two cases the lane goes back to what the printer holds. A field the
edit clears shows what the printer holds at once.

A tool reading these records does not need to follow this order; it is
documented so third parties know why HelixScreen shows what it shows.

---

## 6. Hardware-event clearing

HelixScreen clears what it wrote about a lane when the hardware shows that the
*physical* spool in it has changed, so details of a spool that is gone do not
survive a swap nobody re-entered.

### The insert rule

One rule decides what a spool going into a slot does to the slot's record
(`prestonbrown/helixscreen#1710`). It is judged on **evidence**: what the
hardware physically read off the spool now in the slot, compared against what
it read off the one before.

| Evidence | What counts |
|----------|-------------|
| Tag UID | A per-spool tag identifier nobody can set through the UI (Snapmaker `CARD_UID`). A reader that has finished with a spool and found no tag counts too: it says the spool is untagged. |
| Material and colour | Values decoded from the spool's tag in this insert. A value the firmware remembers across inserts (a colour set on the printer's own menu, a saved slot table) is **not** a reading of the new spool. |
| Binding | A spool id the firmware names for the lane (AFC, Happy Hare, CFS flat schema). |

| Verdict | When | What happens |
|---------|------|--------------|
| Different spool | The tag UIDs differ, or a finished read finds a tag where the previous spool had none or none where it had one; or, with no UID on either side, any of material or colour read on both sides differs; or the firmware names a different spool id | HelixScreen drops what described the previous spool: an unconfirmed edit, the Spoolman details, the metered weight and the record it wrote. The firmware's own reading stays and paints the lane. Nothing is written to firmware, which already holds the new spool's reading. |
| Same spool | The tag UIDs match; or, with no UID on either side (or a read not yet finished), material **and** colour both match | Everything is kept, silently. Two spools of one material and colour are interchangeable; a change of manufacturer between them is the user's to correct. |
| No evidence | Anything less, including every untagged spool | Everything is kept, and HelixScreen shows a non-blocking "same spool?" notice whose Clear button runs Clear Spool. The notice is not shown for a slot with no details to clear. |

Materials compare without case or surrounding whitespace; colours compare as
exact RGB. A family match (`PLA` against `PLA-CF`) is a different spool. A
backend that reads no colour reports none, never its no-colour sentinel as an
RGB, and a multi-colour spool contributes its primary colour, the one the slot
paints. The verdict does not depend on
the print state: when the hardware says the slot holds a different spool,
keeping the old details would describe a spool that is gone, so a runout
reload mid-print clears too. The "same spool?" notice alone is withheld on
the slot feeding the print, because the Clear Spool it offers is refused
there.

Continuous swap detection is the same rule applied without an insert edge.
Backends whose hardware re-reads a tag while the slot stays occupied compare
each reading against the stored one (the fingerprint below) and treat a change
exactly as a different-spool verdict, so the fingerprint must be built from
the same evidence fields: a fingerprint that includes a field the rule
ignores would call a same-spool insert a swap.

### Per backend, under the insert rule

| Backend | Tag UID | Material and colour read off the spool | Binding | Verdict for an untagged insert |
|---------|---------|----------------------------------------|---------|-------------------------------|
| Snapmaker U1 | `CARD_UID` | From the tag | - | No evidence |
| CFS | - | `material_type` and `color_value` from the RFID lookup, when a tag was read | Flat schema only | No evidence |
| QIDI Box | - | The filament and colour table ids from the tag; the vendor id is not evidence | - | No evidence |
| ACE | - | From the tag, for tagged spools only | - | No evidence |
| AD5X IFS | - | None: the IFS colour and type are firmware memory, set on the printer's menu | - | No evidence |
| AFC | - | None | Per-lane `spool_id` | No evidence, unless the plugin names a different spool |
| Happy Hare | - | None: the gate map is user-maintained | Per-gate `spool_id` | No evidence, unless the MMU names a different spool |
| OpenAMS | - | None: the manager reports only `ready` and `loaded` | - | No evidence |
| Tool changer | - | - | - | No insert signal; the rule never runs |

Two rows carry caveats the table cannot hold. Every AD5X IFS insert is No
evidence; one its port sensor sees raises the notice, and one inferred only
from `Adventurer5M.json` (no port sensor) raises none. Stock CFS waits up to 3
frames for the RFID probe before judging an insert. The
wait does not count down while the box is busy and the probe is deferred; it
restarts when the probe runs. A finished read (`vender` past `"unknown"`) that
restates the pulled spool's material and colour is the same spool, and stays
silent.

Clearing is a `DELETE` on the slot's `lane_data` key. The first observation
after startup establishes the baseline fingerprint and is NOT treated as a
swap; otherwise every app launch would wipe overrides. For backends whose
fingerprint is hardware-read (Snapmaker `CARD_UID`, CFS and QIDI composites),
the baseline travels with the record as `helix_fingerprint` (§3), so the first
observation after a restart is a comparison against the stored value: a match
is the same spool, a mismatch is a swap made while HelixScreen was not running
and clears the record. Records without the key, including everything written
before it existed, keep the first-observation-is-a-baseline rule.

Where the firmware holds the spool's identity, it decides what an eject keeps
(AFC's per-lane `remember_spool`, Happy Hare's gate map, the CFS and ACE
units' own tables); HelixScreen shows the result. A lane with
`remember_spool` off that empties drops HelixScreen's AFC namespace record too.
A firmware-reported *different* positive `spool_id` is the firmware's link and
the lane follows it on any backend.

Third-party writers do **not** need to implement this behavior. It is
documented so readers understand why a record HelixScreen wrote may disappear.

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

Follow RFC 0001 §5. A minimal record, as `POST /server/database/item`:

```json
{
  "namespace": "lane_data",
  "key": "lane1",
  "value": {
    "lss_version": "0.1",
    "writer": "my-tool",
    "lane": "0",
    "lane_name": "lane1",
    "loaded": true,
    "color": "#1A1A1A",
    "material": "ASA",
    "vendor_name": "Polymaker",
    "name": "PolyLite ASA",
    "spool_id": 42,
    "bed_temp": 90,
    "nozzle_temp": 250,
    "scan_time": "2026-11-07T12:34:56Z"
  }
}
```

Guidelines:

- **Set `writer`, and respect another one.** Do not overwrite a record whose
  `writer` names another live producer; a record with none is legacy and may be
  replaced. HelixScreen follows the same rule, so a record you stamp is never
  overwritten by it.
- **Always emit `lane` as a string.** OrcaSlicer requires it and drops a
  numeric one.
- **Emit a slicer-matchable `material`.** An unmatched string resolves to a
  Generic PLA preset (wrong temperatures). Put the precise type in
  `filament_type` and write `material` as `null` rather than an unmatchable
  string.
- **Write `null` for unknown values**, never `""`, `0` or `"NONE"`.
- **Use `vendor_name` and `name`**, Happy Hare's keys, which RFC 0001 adopted.
- **Stamp `scan_time`** on every write, ISO-8601 UTC with second precision.
- **Keep one key per lane.** OrcaSlicer has no deduplication, so the same
  `lane` under two keys shows as two trays (§8). Use `laneN` (1-based key) for
  filament systems or `T<n>` (0-based key) for tool changers; the `lane` field
  is 0-based either way (§4).
- **Prefix your own keys** with your tool's name (RFC 0001 V-S1), and carry a
  record's `helix_`-prefixed keys through unchanged when you rewrite it.

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
| **AFC** (AFCProject) | `T<n>` since the virtual-tools firmware (DEV 2026-08, Klipper-Add-On #832); `laneN` before | AFC_lane.py `send_lane_data` writes one record **per T(n) mapping**, so a multi-mapped lane appears once per tool. Inner `lane` field is the tool number string. See below. |
| **Happy Hare** | `laneN`, 0-based (`lane0` for gate 0) | components/mmu_server.py `push_lane_data`; also emits `vendor_name` / `name` / `filament_id` inner fields, `filament_id` being Spoolman's numeric filament id. |
| **Mainsail #2510** | `T<n>` | Writes `lane_data` records for plain Spoolman + tool changer setups, keyed by tool (`T0`, `T1`, …). This is why HelixScreen tool changers converge on `T<n>`. |

#### Shipped: AFC moved from `laneN` to `T<n>` (virtual-tools firmware)

**Status: on upstream `DEV` as of 2026-08-16** (Klipper-Add-On #832, which
also became the 1.3 release line). Verified in AFC's own source: AFC_lane.py
`send_lane_data` now iterates `_mapped_keys()` and POSTs one record per `T(n)`
currently mapped to the lane, keyed by that `T(n)` — a lane mapped to
`T16,T17` produces two records with identical contents. A lane-name key cannot
express that, which is why the key changed. The inner `lane` field changed
meaning with it: it now carries the **tool number** string
(`key.replace("T", "")`), not a lane index. Stale `T(n)` keys are removed
incrementally when no lane claims them anymore (`_sent_lane_data_keys`), in
addition to the boot-time full-namespace wipe (AFC.py `delete_lane_data`).

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
   keys through the tool mapping the firmware reports in its status objects,
   and parks (replays later) any payload that arrives before a mapping does.
   Its record reader (`from_lane_data_record`) is key-agnostic, and HelixScreen
   writes no lane records on AFC printers except the external spool's (§4).
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
| **OrcaSlicer** | `MoonrakerPrinterAgent::fetch_moonraker_filament_data`, in OrcaSlicer's own MoonrakerPrinterAgent.cpp (not in this repo). **Key-opaque**: iterates `result.value.items()` (line 780) and never reads the outer key — the slot index comes from the inner `lane` field (line 786). The inner `lane` **must be a JSON string**: `safe_json_string()` (line 661) is `is_string()`-guarded with no coercion, so an integer `lane` is silently dropped (line 796). **No deduplication**: `trays.push_back()` (line 813) is unconditional and the grid bind (line 494) is first-match-wins over nlohmann's alphabetically-sorted keys. Color parsing (`normalize_color_value`, line 691) strips a leading `#`, so `#RRGGBB` is fine. Orca does not read Spoolman or the `gcode_macro` namespace. |
| **HelixScreen** | `reload_async` in `filament_slot_override_store.cpp`, at connect, after each of its own writes and every 10 seconds. **Key-agnostic**: reads any record whose inner `lane` parses, regardless of outer key. **Canonical-preferring on duplicates**: when two keys describe the same lane, keeps the record whose key is canonical for this backend's own style. This is order-independent and agrees with Orca's alphabetical first-wins in every case that can occur. |

### The collision rule

Because Orca keys off the **inner** `lane` field and does **not** dedup, two
records that share the same inner `lane` under two different outer keys (e.g. a
stale `lane1` and a fresh `T0`, both `"lane": "0"`) appear in Orca as **two
trays for one slot**. `"T0"` sorts before `"lane1"`, so Orca's first-match-wins
would show the `T0` record — but the duplicate is still visually present.

The fix is not on the reader side (readers cannot know two keys mean one slot):
**writers must converge on one key style per slot.** HelixScreen does this by
writing `T<n>` on tool changers (matching Mainsail). A third party that keeps rewriting a
different key for the same slot will produce a permanent duplicate that no
reader can resolve.

---

## 9. Reference implementations

| Project | File | Role |
|---------|------|------|
| HelixScreen | `src/printer/filament_slot_override_store.cpp` | Reader, and writer where §1 says. `to_lane_data_record` / `from_lane_data_record` for the record shape; `reload_async` / `save_async` / `clear_async` for namespace I/O. |
| OrcaSlicer 2.3.2–2.4.0-beta | OrcaSlicer's own src/slic3r/Utils/MoonrakerPrinterAgent.cpp:727-822 (`fetch_moonraker_filament_data`) | Reader (filament preset auto-sync on print send). Canonical reference for AFC-standard field semantics. Unchanged across this range. |
| AFC (Armored Turtle) | Klipper plugin | Native writer. See [AFC docs](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html). |
| Happy Hare | components/mmu_server.py (`push_lane_data`) | Native writer. Writes `lane_data` records keyed with `vendor_name` / `name` / `filament_id`. OrcaSlicer prefers this Moonraker source over the live `mmu` Klipper object (`fetch_hh_filament_info`, in OrcaSlicer's own MoonrakerPrinterAgent.cpp:825-950) when the namespace is populated. HelixScreen mirrors HH's `vendor_name` / `name` key convention as aliases. |

---

## Changelog

- **v2.0 (2026-11)**: Aligned with RFC 0001, Lane Status Surface. §1: the
  writer per printer, and the printers where HelixScreen only reads (AFC, Happy
  Hare, OpenAMS with `[openams_spoolman]`). §3: HelixScreen's records carry
  `lss_version`, `writer`, `lane_name` and `loaded`, the precise type moves
  from `helix_material` to `filament_type`, the Spoolman filament id to
  `spoolman_filament_id`, and the colour name and catalog keys take the
  `helix_` prefix; unknown values are `null`. The legacy `vendor`,
  `spool_name`, `helix_material`, `color_name`, `helix_locked_*` and
  `helix_declared` keys are no longer written (the first four are still read).
  §5: the authorship merge is replaced by the RFC writer rule and a fixed order
  (unconfirmed edit, firmware, record, Spoolman for the linked spool), read at
  connect, after each write and every 10 seconds. §6: an eject on firmware that
  holds identity follows the firmware. §7: writer guidance follows RFC 0001.
- **v1.17 (2026-10-27)**: §3: new optional field `filament_id`, OrcaSlicer's
  preset id for the slot's catalog product, which OrcaSlicer builds carrying
  PR #14423 use to select the exact preset. An id more than one catalog product
  carries is never written. §2/§4/§8: Happy Hare's `laneN` keys
  are 0-based (`lane0` is gate 0), not 1-based.
- **v1.16 (2026-10-22)**: §7: an outside writer writes `vendor_name` and
  `name`, never the legacy `vendor` / `spool_name` that mark a record as
  HelixScreen's own (§5); the write example now uses `vendor_name`. The wire
  format is unchanged.
- **v1.15 (2026-09-27)**: §5: a record AFC's plugin wrote (`td`, no
  `vendor_name`) is a firmware reading and never promotes, whether or not the
  plugin build writes `extruder_index`. §6: stock CFS judges a finished read
  that restates the pulled spool as the same spool, and holds its insert wait
  while a busy box defers the probe (`prestonbrown/helixscreen#1710`).
- **v1.14 (2026-09-25)**: §5 amendment: newest edit wins whoever made it
  (prestonbrown/helixscreen#1632). A shared-namespace record carrying none of
  our authorship marks (`helix_` keys, or the legacy `vendor` / `spool_name`
  spellings) is another tool's replacement of ours and files as the lane's
  statement, unstamped outright or stamped only over an older statement,
  instead of as a memory below whatever a person said. A `scan_time` in the
  JavaScript or Python spelling (fractional seconds, numeric offset) orders
  the same as a `Z`-suffixed one. §6: Snapmaker U1, ACE and AFC moved onto
  the insert rule (prestonbrown/helixscreen#1710).
- **v1.13 (2026-09-24)**: §6 states one insert rule for every backend: a
  spool going into a slot is judged on what the hardware read off it (tag UID,
  or material and colour decoded from the tag, or a firmware-named spool id).
  A different spool drops HelixScreen's record of the old one; the same spool
  keeps it; no evidence keeps it and asks the user
  (`prestonbrown/helixscreen#1710`). The wire format is unchanged.
- **v1.12 (2026-09-23)**: New optional extension key `helix_spoolman_filament_id` (§3): the
  Spoolman filament definition ID behind the `spool_id`, persisted with the binding so the
  definition survives HelixScreen restarts and every poll of a box that replaces its parsed
  slots wholesale (`prestonbrown/helixscreen#1632`). Filed with the spool id and dropped with
  it. The `helix_` prefix is load-bearing: Happy Hare writes its own unprefixed
  `filament_id` inner field, a different value with the same name, which HelixScreen does
  not read. Omitted when zero; records written by earlier versions and by other tools are
  unaffected.
- **v1.11 (2026-09-22)**: A name in `helix_declared` stands only over a value
  the record carries (§3, §5). Clearing a field is not a declaration of
  emptiness: HelixScreen no longer names a field in the set when a user clears
  it, and a stored record naming a field it holds no value for - written by an
  older build - now reads as claiming nothing for that field, so firmware's
  value reaches the lane again (`prestonbrown/helixscreen#1661`).
- **v1.10 (2026-09-22)**: New optional extension key `helix_fingerprint` (§3):
  the slot identity HelixScreen's hardware-swap detection last observed,
  persisted so a restart can compare rather than treat every first observation
  as a baseline (§6). Omitted when empty; absence keeps the old
  first-observation rule, so records written by earlier versions and by other
  tools are unaffected.
- **v1.9 (2026-09-15)**: `helix_declared` names `color_rgb` and `material` as
  well, and `helix_locked_color` / `helix_locked_material` are written from it.
  A reader takes a lock key only for a field `helix_declared` does not name, on
  a record with no `spool_id`, where a true key beside a value is the user's
  declaration (§3, §5).
- **v1.8 (2026-09-14)**: Documented the three authorship keys HelixScreen
  writes into every record it authors and which this document had never
  described: `helix_locked_color`, `helix_locked_material` and `helix_declared`
  (§3), each always emitted so that `false` and the empty array stay
  distinguishable from an absent key, with a note asking other writers to
  preserve them across a rewrite (§3, §7). §5 restated accordingly: the merge
  is ranked by what a record claims about authorship, not by whether a field
  holds a value. Also corrected the `color` row, which said the field was
  emitted only for a non-zero RGB. `#000000` is a real filament colour and is
  emitted like any other.
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
  against AFC's upstream source (AFC_lane.py `send_lane_data` /
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
  (its own MoonrakerPrinterAgent.cpp:780), so the outer key is opaque; the real
  hazard is the same inner `lane` under two outer keys (Orca does not dedup).
  HelixScreen now writes `T<n>` on tool changers (converging with Mainsail
  #2510) and migrates its own stale `laneN` records to `T<n>` on load.
- **v1.3 (2026-06-18)**: Corrected the Happy Hare description — HH's Moonraker
  component (components/mmu_server.py, `push_lane_data`) writes `lane_data`
  records directly (keys `vendor_name` / `name` / `filament_id`), and
  OrcaSlicer prefers that Moonraker source over the live `mmu` Klipper object.
  HH is now documented as a `lane_data` writer alongside AFC and HelixScreen,
  not an out-of-scope separate path. HelixScreen now emits `vendor_name` (alias
  of `vendor`) and `name` (alias of `spool_name`) to mirror HH's key
  convention for forward-compat (Orca ignores unknown keys), and its reader
  accepts either spelling. HelixScreen still does **not** write `lane_data` for
  AFC / Happy Hare backends — those plugins own their own records.
- **v1.2 (2026-06-09)**: Verified against OrcaSlicer's own 2.4.0-beta source
  (MoonrakerPrinterAgent.cpp): the `lane_data` namespace and the five consumed
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
