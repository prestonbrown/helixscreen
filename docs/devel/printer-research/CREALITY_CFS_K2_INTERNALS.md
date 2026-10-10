# Creality CFS Internals (K2 series)

Reverse-engineering reference for the CFS as it runs on the K2 series: the `box_wrapper.cpython-39.so`
module (Python 3.9, ARM), its Klipper configuration, the `box` Moonraker object and its schema
variants, the command surface, error codes and runout behaviour, plus the community Kalico port
that replaces the module.

This is the *firmware side*. For how HelixScreen talks to it, see
[FILAMENT_BACKEND_CFS.md](../FILAMENT_BACKEND_CFS.md#cfs-creality-filament-system); for the
platform, see [../printers/CREALITY_K2_SUPPORT.md](../printers/CREALITY_K2_SUPPORT.md). The K1
family runs a different generation of the module with its own command set:
[CREALITY_CFS_K1_INTERNALS.md](CREALITY_CFS_K1_INTERNALS.md). Nothing here may be assumed to
hold for K1.


The CFS is a multi-material filament management system using RS-485 serial communication. Each CFS unit holds 4 spools; up to 4 units can be daisy-chained for 16-color printing.

### Architecture

The CFS is implemented as Klipper modules, but the core logic is in **closed-source Cython `.so` blobs**:

| Module | Source | Function |
|--------|--------|----------|
| box.py | 3-line shim | Loads `MultiColorMeterialBoxWrapper` from `box_wrapper.cpython-39.so` |
| `box_wrapper.cpython-39.so` | **Binary blob** (Cython) | All CFS protocol, RFID, motor control, filament state |
| auto_addr.py | 3-line shim | Loads `AutoAddrWrapper` from `auto_addr_wrapper.cpython-39.so` |
| `auto_addr_wrapper.cpython-39.so` | **Binary blob** | RS-485 device discovery and address assignment |
| `filament_rack` | Klipper module | External filament rack sensor |

### Klipper Configuration

From the active `box.cfg` on the K2 Plus:

```ini
[serial_485 serial485]
serial: /dev/ttyS5
baud: 230400

[auto_addr]

[filament_rack]
not_pin: !PA5

[box]
bus: serial485
filament_sensor: filament_sensor
Tn_extrude_temp: 220        # Extrusion temperature
Tn_extrude: 140             # Extrusion length
Tn_extrude_velocity: 360    # Extrusion speed
Tn_retrude: -10             # Retraction after cut
Tn_retrude_velocity: 600    # Retraction speed
buffer_empty_len: 30        # Buffer tube reserve length
has_extrude_pos: 1          # Has dedicated purge station
extrude_pos_x: 133          # Purge station X
extrude_pos_y: 378          # Purge station Y
safe_pos_x: 225             # Safe park X
safe_pos_y: 345             # Safe park Y
# ... cut positions, clean positions, etc.
```

### RS-485 Protocol

| Detail | Value |
|--------|-------|
| Bus | `/dev/ttyS5` at 230400 baud |
| Frame format | `0xF7 \| addr \| length \| status \| function_code \| data[] \| CRC8` |
| Slave addresses | `0x01-0x04` (individual CFS units), `0xFE` (broadcast boxes), `0xFF` (all devices) |
| Commands | Connect, RFID read, motor control, extrude/retract, version/SN query, sensor queries |

### Moonraker Object: `box`

The `[box]` Klipper module exposes full CFS state via Moonraker's `printer.objects.query`. This is the primary interface for HelixScreen integration.

**Query**: `GET /printer/objects/query?box`

#### Box schema variants

There are **two** incompatible `box` shapes in the wild, with zero key overlap:

| Schema | Shipped by | Shape |
|--------|-----------|-------|
| **Stock** | Creality's own `[box]` module (K1 and K2) | Per-unit `T1`–`T4` objects, each holding four parallel arrays; top-level `filament` / `map` / `same_material` / `auto_refill` |
| **Flat** | Community Kalico ports carrying a reimplemented box.py | A single `slots[]` array of self-describing objects; top-level `loaded_slot` / `slot_filament_mask` / `load_path` / `materials` |

`AmsBackendCfs::detect_schema()` picks between them **from the payload** — a `T{n}` key means Stock, otherwise a `slots` array means Flat, and anything ambiguous defaults to Stock. It deliberately does **not** consult `PrinterDetector`: the affected printers report as stock K2 Plus hardware by every model signal, so model detection cannot see the firmware swap.

Everything from here to "Disconnected Units" describes the **stock** schema. The flat schema is documented under [Community Kalico port](#community-kalico-port) below.

#### Top-Level Fields

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | Connection state: `"connect"`, `"None"` |
| `filament` | int | Filament loaded flag (1 = loaded) |
| `enable` | int | CFS enabled for printing |
| `auto_refill` | int | Auto-refill (backup spool) enabled |
| `filament_useup` | int | **Sticky latch, not an enable flag.** 1 = the box has reported its spool used up. Set by `BoxAction.send_data` on that report; cleared to 0 **only** by `BoxAction.extruder_extrude` on a successful extrude. It is *not* print-scoped — nothing resets it when a job ends, so it can read 1 indefinitely on an idle machine (confirmed live on a K2 Plus: `filament_useup: 1` with `print_stats.state: standby`). Treat a *transition* to 1 as the signal; the level alone means nothing. See [Runout and auto-refill](#runout-and-auto-refill). |
| `map` | dict | Tool-to-slot mapping: `{"T1A": "T1A", "T1B": "T1B", ...}` |
| `same_material` | array | Auto-refill equivalence groups. Each entry is `[material_type, color_value, [slot_ids], material_name]` — e.g. `[["101001", "01A1A1A", ["T1C", "T1D"], "PLA"]]` (live K2 Plus). Membership requires **exact string equality on BOTH `material_type` AND `color_value`**; slots whose either field is a sentinel (`-1`, `none`, `unknown`, `""`) are excluded. On the machine above, T1B carried the same `color_value` `01A1A1A` but `material_type: unknown`, and was left out of the group for that reason alone. |

#### Per-Unit Fields (`T1`, `T2`, `T3`, `T4`)

Each CFS unit (T1=unit 1, T2=unit 2, etc.) has:

| Field | Type | Example | Description |
|-------|------|---------|-------------|
| `state` | string | `"connect"` / `"None"` | Unit connection state |
| `filament` | string | `"None"` | Currently loaded filament |
| `temperature` | string | `"27"` | Internal temperature (C) |
| `dry_and_humidity` | string | `"48"` | Relative humidity (%) |
| `filament_detected` | string | `"None"` | Filament detection state |
| `measuring_wheel` | string | `"None"` | Measuring wheel state |
| `version` | string | `"1.1.3"` | Firmware version |
| `sn` | string | `"10000882925..."` | Serial number |
| `mode` | string | `"0"` | Operating mode |
| `vender` | array[4] | hex strings / `"none"` / `"unknown"` | Raw RFID vendor data per slot: `"none"` if no filament is presented, `"unknown"` if the filament has no RFID tag |
| `remain_len` | array[4] | `["35","57","52","52"]` | Remaining filament length (meters) per slot |
| `color_value` | array[4] | `["0000000","0FFFFFF","00A2989","0C12E1F"]` | Filament color hex per slot |
| `material_type` | array[4] | `["101001","101001","101001","101001"]` | Material type code per slot |
| `uuid` | array | ints | RFID UUID bytes |
| `change_color_num` | array[4] | `["-1","-1","-1","-1"]` | Color change count per slot |

#### Slot Addressing

Slots use a `T{unit}{letter}` naming convention:
- **Unit**: 1-4 (CFS unit number)
- **Letter**: A-D (slot within unit)
- Example: `T1A` = Unit 1, Slot A; `T3C` = Unit 3, Slot C

The `map` field maps virtual tool names to physical slots (usually 1:1 unless remapped).

#### Material Type Codes

The `material_type` field uses a format: `1XXXXX` where `XXXXX` is the material database ID.

| Code | Material |
|------|----------|
| `101001` | Creality Hyper PLA |
| `102001` | Creality Hyper PLA-CF |
| `106002` | Creality Hyper PETG |
| `103001` | Creality Hyper ABS |
| `100001` | Generic PLA |
| `100003` | Generic PETG |
| `100004` | Generic ABS |
| `100005` | Generic TPU |

The full material database is at /mnt/UDISK/creality/userdata/box/material_database.json (77 materials, fetched from Creality cloud). Material entries include brand, name, `meterialType` (sic — Creality typo), density, diameter, temp range.

#### Color Values

Colors are 7-character hex strings with a leading `0`: `"0RRGGBB"`. Examples:
- `"0000000"` = black
- `"0FFFFFF"` = white
- `"00A2989"` = teal
- `"0C12E1F"` = orange-red

#### Disconnected Units

Units that are not connected report all fields as `"None"` or `"-1"`.

### Community Kalico port

Some K2 Plus owners run a community Kalico (Danger-Klipper) port instead of Creality's firmware — [`Jacob10383/kalico`](https://github.com/Jacob10383/kalico), a fork of `KalicoCrew/kalico`, with Creality's closed CFS module replaced by a clean-room box.py. Moonraker reports the replacement modules as **untracked** files (box.py, box_addr.py, box_catalog.py, box_change.py, box_protocol.py): they are dropped in by the port's installer and are not committed to the kalico fork — searching GitHub for them finds nothing. They are, however, **downloadable**: see [Getting the module source](#getting-the-module-source). First seen in debug bundle QJKZEMTS on v0.99.106.

**Identifying it.** The printer looks like stock K2 Plus hardware in every model signal, so identify it from the firmware:

| Signal | Where | Value |
|--------|-------|-------|
| Klipper repo | `moonraker.log` git repo block | `Jacob10383/kalico`, branch `main` |
| Klipper version | `printer.klipper_version` | CalVer, e.g. `v2026.08.00-0-g64af1fb2` (stock is `v0.12.x`) |
| Box schema | `printer.box` | has `slots[]`, no `T1` |
| Config | `printer.cfg` | `[box]` with `box_count = N` |

Stock CFS macros are **entirely absent**: zero `CR_BOX_*`, zero `BOX_MODIFY_TN_DATA`, zero `M8200`.

#### Getting the module source

The port ships from `https://firmware.jacobean.xyz`, and every artifact is content-addressed, so the box modules can be read **without flashing anything and without pulling the 300 MB rootfs**. Do this before reviewing any Fork change — the command surface is not guessable and the modules are the only authority.

```bash
curl -sS -o install.py https://firmware.jacobean.xyz/install.py   # READ it, never run it
grep -E '^(FIRMWARE_VERSION|HELIX_VERSION)' install.py           # pinned release + HelixScreen build
curl -sS https://firmware.jacobean.xyz/<FIRMWARE_VERSION>/index  # rootfs/kernel/swap/bootstrap/extras digests
```

The index's `extras.manifest_sha256` points at a manifest listing every Klipper module with its own `sha256`. Objects live at `/<FIRMWARE_VERSION>/o/<sha256>`, so fetching one module is:

```bash
curl -sS -o box.py https://firmware.jacobean.xyz/<FIRMWARE_VERSION>/o/<box.py sha256 from the manifest>
shasum -a 256 box.py   # must match the manifest digest
```

install.py is a **kernel and rootfs flasher**. Downloading and reading it is safe; executing it is not. Nothing in this workflow needs it to run.

Re-verified 2026-08-07 against `FIRMWARE_VERSION = 6.18`: box.py 2776 lines, sha256 `4cf85e6d…2d28695a`, now declaring `API_VERSION = 1` and `LEGACY_WIDGET_VERSION = 2`, with `_BOX_SLOT_CLEAR` registered in `_register_commands`. The payload emits **both** `api_version` and `fluidd_widget_version`, so a module carrying the new field is still readable by a HelixScreen that keys on the old one.

> **`FIRMWARE_VERSION` does not identify the module.** The 2026-08-06 fetch of the *same* `6.18` pin returned a 2754-line box.py with `WIDGET_VERSION = 2` and no `_BOX_SLOT_CLEAR`. The artifacts were republished in place without a version bump, so two printers can both report `6.18` and run different command surfaces. Trust the manifest digest, not the version string — and re-fetch before relying on any earlier reading here.

`_register_commands` in box.py plus `_register_t_commands` (the per-slot `T<n>` handlers, registered only after bus enumeration) are the full command surface.

Treat the module as an **interface reference only**: command names, parameter names, JSON field names. Do not port its implementation.

#### Flat schema fields

Top-level:

| Field | Type | Description |
|-------|------|-------------|
| `slots` | array | Self-describing slot objects (see below) — includes one `external: true` entry for the external spool holder |
| `loaded_slot` | int | Index into `slots[]`, `-1` when nothing is loaded |
| `loaded_mask` | int | Bitmask of loaded slots |
| `slot_filament_mask` | int | Bitmask of slots holding filament (`15` = all four bays) |
| `temp_c` / `humidity_pct` | number | Unit environment — JSON **numbers**, not the stock schema's strings |
| `state` / `state_code` | string / int | e.g. `"IDLE"` / `0` |
| `status` / `status_code` | string / int | e.g. `"OK"` / `0` |
| `runout` | null \| object | The runout-swap plan, not a runout event: `{"loaded_slot": N, "chain": [...]}` whenever a bay is loaded (or the runout origin while one is active), `null` otherwise. `chain` is every other present slot with identical `material` and `color`, in slot order, as `slots[].index` values; `runout_recovery` swaps to `chain[0]`. HelixScreen reports `loaded_slot -> chain[0]` as the endless-spool backup edge, and an empty chain as OnWithoutBackup (#1464) |
| `runout_swap_enabled` | bool | Endless-spool equivalent |
| `materials` | dict | `{"PLA": {"target_temp": 220}, ...}` — per-material recommended temps, keyed by the same string each slot reports. Stock has no equivalent. |
| `load_path` | object | Shared feed path: `encoder`, `buffer`, `printhead_sensor`, `clog_detection` |
| `data_ready` / `driver_ready` | bool | Module readiness |
| `api_version` | int | Box command/status contract version; HelixScreen supports version 1. This is the dialect signal. |
| `fluidd_widget_version` | int | The module's `LEGACY_WIDGET_VERSION`, still emitted alongside `api_version` for the port's own Fluidd widget. Not a command-set version — it stayed at 2 across a release that added commands. |

Per-slot (`slots[i]`):

| Field | Type | Notes |
|-------|------|-------|
| `index` | int | Firmware slot number, `(box address - 1) * 4 + local`. HelixScreen uses it as the bay's global index, so per-bay state stays on the physical bay when a box drops out; an address below the highest present one becomes an absent placeholder unit (`AmsUnit::absent`) that is neither counted nor commanded. |
| `name` | string | Spool name, e.g. `"2026_PETG"` |
| `material` | string | Plain material name — **no code table**, unlike stock's `101001` |
| `color` | string | Conventional `"#RRGGBB"` — **not** stock's leading-zero `"0RRGGBB"` |
| `brand` | string | `"None"` is the absent-brand sentinel |
| `present` / `loaded` | bool | Drive `SlotStatus` directly |
| `external` | bool | `true` marks the external spool holder, excluded from the unit's bays |
| `rfid_percent` / `rfid_reserve` | null \| … | Present but no per-slot UID — see gaps below |
| `spoolman_id` | null \| int | Null until linked |

#### Command dialect

`CfsMacroVariant::Fork` — a **third** dialect, independent of the schema axis. The commands are high-level and self-contained: box.py owns the whole feed/purge/park sequence, so there is no envelope for HelixScreen to assemble and every operation is a single line.

Signatures below are read from the module's own `_register_commands` / `cmd_*` handlers, not inferred:

| Command | Signature | Notes |
|---------|-----------|-------|
| `BOX_LOAD` | `SLOT=<0..15>` | Low-level feed primitive. HelixScreen does not call it. |
| `BOX_UNLOAD` | `[MANUAL=0\|1]` | **Rejects `SLOT`** outright: "BOX_UNLOAD no longer accepts SLOT". |
| `T<n>` | `[FLUSH=0\|1]`, default 1 | Load/tool change. HelixScreen emits bare `T<n>`; the registered command owns the change engine (cut → retract → load → flush). |
| `_BOX_SLOT_SET` | `SLOT=<n> MATERIAL="<str>" COLOR="#RRGGBB" BRAND="..." NAME="..." SPOOLMAN_ID=<id\|-1>` | SLOT, MATERIAL, and COLOR are **required**. The COLOR quotes are required because Kalico treats a bare `#` as a comment. Helix always sends the optional fields; `-1` clears the Spoolman link. Material is uppercased by the module. |
| `_BOX_SLOT_CLEAR` | `SLOT=<n>` | Removes the persisted Box profile for the slot. |
| `_BOX_MATERIAL_SET` | `MATERIAL=<str> TARGET_TEMP=<170..350>` | Edits the `materials` table. |
| `_BOX_SET_RUNOUT_SWAP` | `ENABLE=0\|1` | Endless-spool equivalent. |
| `BOX_CUT`, `NOZZLE_CLEAN`, `BOX_GO_TO_WASTEBIN`, `BOX_RUNOUT_CHECK`, `BOX_DEBUG`, `BOX_BUFFER_RETRACT` | no parameters | |

> **There is no `BOX_CHANGE`.** The name appears only in user-written alias macros (e.g. `CFS_CHANGE` → `BOX_CHANGE {rawparams}`) that this firmware does not define — those aliases are dead. Tool change is `T<n>`.

`state` and `status` arrive as **strings** already (`IDLE`/`PRELOAD`/`PRINT`/`RELOAD`/`ERROR`/`TEST`, and 22 status names including `RUNOUT`, `SLOT_EMPTY`, `FEED_TIMEOUT`, `ODOMETER_STALLED`, `BUFFER_REFILL_STALLED`). Use those; do not attempt to decode `state_code`/`status_code` numerically.

#### Detection

Two independent signals, both from the payload:

- **Schema** → `detect_schema()`: a `T{n}` key means Stock; otherwise a `slots` array means Flat.
- **Dialect** → `detect_fork_dialect()`: `api_version == 1` explicitly selects this Box command set; do not infer commands from the `slots[]` layout.

The dialect signal deliberately is not `has_macro("BOX_LOAD")`: these commands are registered in Python via `gcode.register_command`, so they are **not** gcode_macros and never appear in `printer.objects.list`. A flat payload *without* a supported `api_version` parses fine but keeps the stock dialect and stays gated — a different flat-schema firmware must not inherit this one's command set.

#### Current support status

**Fork command paths enabled.** Slot display, materials, colors, environment and path sensors parse; load / unload / tool-change / slot-metadata writes all emit verified commands. `reject_if_flat_schema()` gates only a flat box whose API version we cannot identify.

Remaining gaps, degraded rather than broken:

| Stock feature | Flat behavior |
|---------------|---------------|
| RFID fingerprinting (`build_cfs_slot_uid`) for hardware-change detection | No per-slot UID in the schema; baseline/clear logic no-ops |
| `set_tool_mapping` via TNN + `box.map` | No equivalent — the module maps tools to slots 1:1 |
| Bypass / external spool load | **Implemented.** The `external: true` entry is observable and `T<external>` is a registered command (verified from the port's own box.py — re-fetched 2026-08-17, sha256 `a5b4d19e…9eeb6f23`, 2892 lines, `API_VERSION = 1`): the change engine runs the same attended flow for the holder as for a bay (heat → wastebin → wait `EXTERNAL_WAIT = 30 s` for insertion → feed 30 mm → flush), `BOX_UNLOAD`'s external branch ejects it, and mid-print `T<external>` pauses for attended loading. `supports_bypass` turns on for the Fork dialect when the payload carries the entry; `loaded_slot` naming it maps to the -2 sentinel. The entry is still skipped when building `unit.slots`, so it never renders as a fifth bay. See `FILAMENT_BACKEND_CFS.md` § "CFS" |

Note `push_slot_identity_to_firmware` is **not** a gap: its Fork counterpart is `_BOX_SLOT_SET`, which writes color, brand, name, and Spoolman link together. Because it requires a material, the backend reads the current slot profile to build the write. The explicit Clear Spool action emits `_BOX_SLOT_CLEAR`.

Parse: `AmsBackendCfs::parse_flat_box_status()`. Builders: `load_gcode` / `unload_gcode` / `swap_gcode` / `slot_set_gcode`. Tests: `tests/unit/test_ams_cfs_flat_schema.cpp` (`[flat]`, `[fork]`), built on the real QJKZEMTS payload.

### Moonraker Object: `filament_rack`

External filament rack (non-CFS) state:

```json
{
  "vender": "-1",
  "color_value": "-1",
  "material_type": "-1",
  "remain_material_color": null,
  "remain_material_type": null,
  "remain_material_velocity": 360
}
```

### Moonraker Object: `motor_control`

```json
{
  "motor_ready": true,
  "is_homing": false,
  "cut": { "state": true, "pos_x": -7.7 }
}
```

### Other Notable Moonraker Objects

| Object | Description |
|--------|-------------|
| `fan_feedback` | Fan speeds: fan0-fan4 |
| `filament_switch_sensor filament_sensor` | Filament runout sensor state |
| `load_ai` | AI print quality monitoring (waste detection) |
| `heater_generic chamber_heater` | Chamber heater control |
| `temperature_fan chamber_fan` | Chamber cooling fan (carries the M141 maintain ceiling) |
| `temperature_sensor chamber_temp` | Chamber temperature sensor |
| `motor_control` | Motor ready state, cutter position |
| `belt_mdl mdlx` / `belt_mdl mdly` | Belt tension measurement |
| `prtouch_v3` | Pressure-based Z probe |
| `z_align` / `z_tilt` | Z axis alignment |
| `custom_macro` | Custom macro management |
| `fan_feedback` | RPM feedback for all fans |

#### Chamber Heating (M141)

The K2 chamber is controlled by the `M141` macro, which coordinates two objects: `heater_generic chamber_heater` and `temperature_fan chamber_fan`. HelixScreen routes chamber sets through `M141 S{temp}` (rather than a raw `SET_HEATER_TEMPERATURE`), and the macro's behavior depends on the setpoint:

- `M141 S0` → **Off** (heater target 0; cooling fan reset to its configured resting target, e.g. 35°C)
- `M141 S{≤40}` → **Maintaining** — holds a cooling ceiling via `temperature_fan chamber_fan`; the heater target stays 0
- `M141 S{>40}` → **Heating** — sets the `heater_generic chamber_heater` target

Because the heater target reads 0 while maintaining, HelixScreen synthesizes a canonical display target and mode rather than displaying the raw heater target. See [MULTI_EXTRUDER_TEMPERATURE.md § Chamber Heating (M141)](../MULTI_EXTRUDER_TEMPERATURE.md#chamber-heating-m141) for the subject/binding details.

### GCode Commands (from `box_wrapper.so` decompilation)

These are the **stock K2** commands (`CfsMacroVariant::K2`). Two other dialects exist: the K1 official CFS upgrade's non-prefixed `BOX_*_MATERIAL` set (`CfsMacroVariant::K1`, see `CREALITY_K1_SUPPORT.md`), and the community Kalico port's high-level `T<n>` / `BOX_UNLOAD` set (`CfsMacroVariant::Fork`, see [Community Kalico port](#community-kalico-port)). Dialect and box schema vary **independently** — do not infer one from the other.

#### Filament Operations
| Command | Description |
|---------|-------------|
| `BOX_EXTRUDE_MATERIAL TNN=T1A` | Load filament from specified slot |
| `BOX_RETRUDE_MATERIAL` | Unload current filament back to CFS |
| `BOX_RETRUDE_MATERIAL_WITH_TNN TNN=T1A` | Unload specific slot |
| `BOX_EXTRUDER_EXTRUDE TNN=T1A` | Feed filament to extruder |
| `BOX_MATERIAL_FLUSH` | Purge/flush filament |
| `BOX_MATERIAL_CHANGE_FLUSH` | Color-change flush sequence |
| `BOX_EXTRUSION_ALL_MATERIALS` | Prime all materials |

#### Tool Change (M8200 macro)
| Subcommand | Description |
|------------|-------------|
| `M8200 P` | Pre-operation (prepare for change) |
| `M8200 C` | Cut filament |
| `M8200 R` | Retract to CFS box |
| `M8200 L I{n}` | Load slot n (0-15, auto-mapped to TnX) |
| `M8200 W` | Waste detection |
| `M8200 F` | Flush/purge |
| `M8200 O` | End operation |

#### Query/Status
| Command | Description |
|---------|-------------|
| `BOX_GET_BOX_STATE` | Query overall CFS state |
| `BOX_GET_RFID ADDR={n} NUM={n}` | Read RFID data for specific slot |
| `BOX_GET_REMAIN_LEN ADDR={n} NUM={n}` | Query remaining filament length |
| `BOX_GET_FILAMENT_SENSOR_STATE` | Query filament sensor states |
| `BOX_GET_HARDWARE_STATUS` | Full hardware status (RFID cards, humidity, eeprom, measuring wheel) |
| `BOX_GET_BUFFER_STATE` | Buffer tube state |
| `BOX_GET_VERSION_SN` | Firmware version and serial number |

#### Control
| Command | Description |
|---------|-------------|
| `BOX_ENABLE_CFS_PRINT ENABLE={0\|1}` | Enable/disable CFS for printing |
| `BOX_ENABLE_AUTO_REFILL ENABLE={0\|1}` | **Setter, not a toggle.** The handler reads an int via `gcmd.get_int`, so it sets auto-refill on or off rather than flipping it. **Parameter spelling RECOVERED** (2026-08-17): Creality's own master-server sends `BOX_ENABLE_AUTO_REFILL ENABLE=1` / `ENABLE=0` — present in the string tables of both OTA images (K1 `CR4CU220812S11_ota_img_V2.3.5.34.img`, K2 Plus `CR0CN240110C10_ota_img_V1.1.4.11.img`). HelixScreen's `toggle_auto_refill` device action sends the switch's value as the explicit argument (with no value, the inverse of the cached flag). |
| `BOX_SET_BOX_MODE` | Set CFS operating mode |
| `BOX_SET_TEMP` | Set extrusion temperature |
| `BOX_SET_PRE_LOADING ADDR={n} NUM={n} ACTION=RUN` | Pre-load filament |
| `BOX_START_PRINT` | Signal print start to CFS |
| `BOX_END_PRINT` | Signal print end to CFS |
| `BOX_ERROR_CLEAR` | Clear CFS error state |
| `BOX_ERROR_RESUME_PROCESS` | Resume after error |

#### Physical Operations
| Command | Description |
|---------|-------------|
| `BOX_GO_TO_EXTRUDE_POS` (M1500) | Move to purge station |
| `BOX_MOVE_TO_SAFE_POS` (M1499) | Park at safe position |
| `BOX_NOZZLE_CLEAN` (M1501) | Wipe nozzle on silicone strip |
| `BOX_CUT_MATERIAL` (M1502) | Activate filament cutter |
| `BOX_MOVE_TO_CUT` | Move to cut position |

#### M8200 — Slicer-Facing CFS Interface

**Use M8200 for manual load/unload operations.** Creality's `BOX_LOAD_MATERIAL` macro is buggy — it omits `CR_BOX_PRE_OPT` which is required before `CR_BOX_EXTRUDE`, causing `key60: Internal error` shutdowns.

| Command | Effect | Underlying |
|---------|--------|-----------|
| `M8200 P` | Prepare CFS for material change | `CR_BOX_PRE_OPT` |
| `M8200 L I={slot}` | Load filament from slot (0-indexed) | `CR_BOX_EXTRUDE TNN=...` |
| `M8200 C` | Cut filament | `CR_BOX_CUT` |
| `M8200 R` | Retract filament (optional `E={length}`) | `CR_BOX_RETRUDE` |
| `M8200 W` | Waste purge | `CR_BOX_WASTE` |
| `M8200 F` | Flush (uses last TNN from L command) | `CR_BOX_FLUSH` |
| `M8200 O` | End material change operation | `CR_BOX_END_OPT` |

**Load sequence:** `M8200 P` → `M8200 L I=2` → `M8200 F` → `M8200 O`
**Unload sequence:** `M8200 P` → `M8200 C` → `M8200 R` → `M8200 O`

**`M8200 C` does not position the toolhead.** `CR_BOX_CUT` strikes in X from wherever the head
stands; Creality's tool change moves to `[box]` `pre_cut_pos` (`G0 X10 Y200` on a K2 Pro)
before it. Sent from the purge chute it can drive the head into the frame. HelixScreen cuts
with `BOX_CUT_MATERIAL`, which positions itself.

**Prerequisites:** Printer must be homed (`G28`). CFS does not require nozzle heating for feed/retract — heating is only needed for purging at the nozzle.

**Stock UI note:** Creality's display-server communicates with the CFS **directly over RS-485** (`/dev/ttyS5` at 230400 baud), bypassing Klipper entirely for load/unload. The GCode macros are primarily for automated print-time use.

#### Macro Sequences (from box.cfg — DO NOT use for manual load/unload)
| Macro | Sequence | Notes |
|-------|----------|-------|
| `BOX_LOAD_MATERIAL TNN=T1A` | Heat → Cut → Retract → Extrude → Flush → Park | **BUG: Missing CR_BOX_PRE_OPT → key60 crash** |
| `BOX_QUIT_MATERIAL` | Heat → Cut → Retract → Park | Same issue |
| `BOX_INFO_REFRESH` | Pre-load → Get RFID → Get Remain Len | Safe to use, [usage](#refresh-spool-info) |

#### The external-spool pair: `LOAD_MATERIAL` / `QUIT_MATERIAL`

Separate from the `BOX_`-prefixed bay macros above, and easy to miss because the names differ
by a prefix. These are Creality's **non-CFS spool-holder** operations, provided by the
`[filament_rack]` module (`filament_rack_wrapper.cpython-39.so`, loaded on a stock K2 Plus —
`filament_rack` appears in `printer.objects/list`). They are what a bypass / external spool
load and unload should use, and HelixScreen's stock-dialect bypass paths do:

| Macro | Sequence | Feed gated on |
|-------|----------|---------------|
| `LOAD_MATERIAL` | `BOX_GO_TO_EXTRUDE_POS` → `FILAMENT_RACK_SAVE_FAN` → `FILAMENT_RACK_PRE_FLUSH` → `FILAMENT_RACK_SET_TEMP` → `FILAMENT_RACK_FLUSH` → `FILAMENT_RACK_RESTORE_FAN` → `SET_COOL_TEMP` → park | toolhead switch |
| `QUIT_MATERIAL` | `BOX_GO_TO_EXTRUDE_POS` → `FILAMENT_RACK_SET_TEMP` → `BOX_MOVE_TO_CUT` → `G0 E-10 F360` → `SET_COOL_TEMP` → park | toolhead switch |

Both gate their extruder work on `filament_switch_sensor filament_sensor`, so the vendor's
intended workflow is "the user feeds to the sensor and the macro takes it from there". A
consequence worth knowing: with nothing at the sensor, `LOAD_MATERIAL` moves, cools and parks
while reporting success, having loaded nothing.

**Only `LOAD_MATERIAL` is self-sufficient.** Measured on a K2 Plus 2026-08-19 by watching the
extruder axis: `LOAD_MATERIAL` moved **+370 mm** (a complete load and purge), while
`QUIT_MATERIAL` moved **-13.99 mm** and left the filament still gripped by the extruder gears.
The reason is in `[box]` itself — `tn_extrude = 140` against `tn_retrude = -10` — because a bay
unload only needs the extruder to break its grip and the box's feeder reels the rest, and a
bypass spool has no feeder. HelixScreen therefore appends its own 80 mm retract to
`QUIT_MATERIAL` and sends `LOAD_MATERIAL` bare. Full rationale, and why an auto-detected
`QUIT_MATERIAL` must not outrank the CFS backend, in
[`FILAMENT_MANAGEMENT.md` § CFS bypass: why the two vendor macros are not symmetric](../FILAMENT_MANAGEMENT.md#cfs-bypass-why-the-two-vendor-macros-are-not-symmetric).

#### Refresh Spool Info

```gcode
; ADDR is the CFS addr (minimum 1, maximum 4)
; NUM is the operation bitflag (see below)
BOX_INFO_REFRESH ADDR={} NUM={}
```

`NUM` is a per-slot bitflag; combine flags to refresh multiple slots at once (e.g. `NUM=15` refreshes all four).

| Spool | Flag |
|-------|------|
| A | `0b0001` |
| B | `0b0010` |
| C | `0b0100` |
| D | `0b1000` |

### Error Codes

CFS errors are reported as JSON with `key8xx` codes:

| Code | Error |
|------|-------|
| `key831` | RS-485 communication timeout |
| `key834` | Parameter error |
| `key835`-`key838` | Extrusion blockages (connections, sensor, gear) |
| `key839` | Extrusion abnormal (confirmed present in the binary; condition not determined) |
| `key840` | Box state error |
| `key841` | Cut sensor not detected |
| `key843` | RFID read error |
| `key844` | Pneumatic joint abnormal |
| `key845` | Nozzle blocked |
| `key846` | Confirmed present in the binary; condition not determined |
| `key847` | Empty printing, material enwind — **provenance uncertain**, see caveat below |
| `key848` | Material break at connections |
| `key849`-`key851` | Retraction errors |
| `key852` | Confirmed present in the binary; condition not determined |
| `key853` | Humidity sensor error |
| `key855` | Cut position error |
| `key856` | No cutter detected — **provenance uncertain** |
| `key857` | Motor load error — **provenance uncertain** |
| `key858` | EEPROM error — **provenance uncertain** |
| `key859` | Measuring wheel error — **provenance uncertain** |
| `key860` | Buffer error — **provenance uncertain** |
| `key861` | Left RFID card error — **provenance uncertain** |
| `key862` | Right RFID card error — **provenance uncertain** |
| `key863`-`key865` | Retraction/extrusion completion errors — **provenance uncertain** |

> **Provenance caveat.** The codes themselves are certain — every `keyNNN` string above was recovered from `box_wrapper.cpython-39.so`. What could **not** be determined from the stripped binary is *which condition raises which code* for `key847` and for the `key856`-`key865` block: Cython folded the raise sites into generated dispatch code with no recoverable mapping back to the calling method. Those descriptions are inference from Creality's user-facing error text and neighbouring codes, not from the raise site. Do not build behaviour that depends on a specific one of them meaning a specific thing until it has been observed on hardware. `key839`, `key846` and `key852` are listed because the strings exist; nothing at all is known about their conditions.

### Runout and auto-refill

Recovered from the stripped `box_wrapper.cpython-39.so` on a live K2 Plus, plus the plain-Python Klipper sources on the device. The config quotes below are from that machine's own `printer.cfg`, as dumped into `klippy.log`.

**The firmware always pauses first. There is no pause-free hot swap.** The toolhead switch is a stock Klipper `filament_switch_sensor` with `pause_on_runout` set, so `runout_helper` issues the pause before anything CFS-specific runs:

```ini
[filament_switch_sensor filament_sensor]
pause_on_runout = true
switch_pin = ^!nozzle_mcu:PA11
runout_gcode =
	{% if printer.extruder.can_extrude|lower == 'true' %}
	G91
	G0 E30 F600
	G90
	{% endif %}
	BOX_CHECK_MATERIAL_REFILL
```

filament_switch_sensor.py calls `pause_resume.send_pause_command()`, waits out `PAUSE_DELAY`, and only then runs `runout_gcode`: a 30 mm purge (skipped when the hotend is too cold to extrude — note the jinja guard, which the 30 mm push is entirely inside) followed by `BOX_CHECK_MATERIAL_REFILL`. The whole path is gated on the job being in the printing state at the moment the sensor edge is noted, i.e. before the pause. **Auto-refill therefore always operates on an already-paused print.**

**Three outcomes, all of which leave the print paused:**

| `auto_refill` | Matching slot in `same_material`? | What happens |
|---------------|-----------------------------------|--------------|
| on | yes | Cut → retrude → extrude → flush from the matching slot, then resume |
| on | no | `respond_info("... no identical supplies ...")`, no swap, stays paused |
| off | — | `respond_info("... disable material automatic refill ...")`, no swap, stays paused |

The two give-up strings are what HelixScreen keys its CFS runout modal off (`AmsBackendCfs::classify_error`). They arrive as `respond_info` output — `// `-prefixed lines on the gcode-response channel, **not** `!!` — which is why that override inverts the usual `is_bang_line()` gate. They are untranslated English literals from one Creality build, so the matcher takes a distinctive fragment (`identical suppl`, `automatic refill` + `disab`) rather than the whole sentence, and falls back to "line mentions refill **and** `filament_useup` is set **and** the job is paused" if the wording changes entirely.

> **Not verified by us:** the exact on-the-wire text of either message. `box_wrapper.cpython-39.so` is stripped, the strings were read out of it rather than out of source, and no runout occurred in the six days of `klippy.log` available from the live machine, so neither has been seen on the wire. If a K2 user reports the modal not appearing, the first thing to check is the literal wording in their `moonraker.log` gcode-response stream.

**A second runout cannot re-trigger.** `check_material_refill` runs `SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=0` on **every** path, match or no match. The sensor stays disabled for the remainder of the job unless something re-enables it. (HelixScreen does not currently offer a re-enable button; a wrongly-timed re-enable would trip immediately if filament is not back at the gate.)

**Resuming.** `BOX_ERROR_RESUME_PROCESS` early-returns unless box `enable != 0` **and** `print_stats.state == 'paused'`. It is reached from a plain `RESUME` via `RESUME_EXTERNAL_PROCESS` (`gcode_macro.cfg`), and it only does the box half of the recovery — it does not un-pause the job. **`RESUME` is the correct user-facing command**; sending `BOX_ERROR_RESUME_PROCESS` directly leaves the print paused. (`AmsBackendCfs::recover_gcode()` still returns the bare command for the programmatic `recover()` API; the recovery *button* sends `RESUME`.)

**The generic Klipper runout warning is suppressed.** Creality edited filament_switch_sensor.py so the usual `key358` runout warning is not emitted whenever the box is enabled — the box's own error path is meant to be the only voice. That is why a CFS runout produces no `!!` line at all and the `respond_info` messages are the only signal available. (Read from the device's own filament_switch_sensor.py; not independently re-confirmed since — the six days of `klippy.log` available from the live K2 Plus contain no `key358` at all, which is consistent with the edit but proves nothing on its own, as no runout occurred in that window either.)

### Internal Classes (from Cython decompilation)

| Class | Purpose |
|-------|---------|
| `MultiColorMeterialBoxWrapper` | Main Klipper module — GCode registration, `get_status()`, lifecycle |
| `BoxState` | Tnn_map, Tnn_content, connection tracking, slot state |
| `BoxAction` | RS-485 commands, RFID reads, motor control, sensor queries |
| `BoxSave` | Persistence: resume_tnn, error state, save/restore across restarts |
| `BoxCfg` | Configuration from box.cfg (positions, velocities, temps) |
| `ParseData` | Binary protocol parser: RFID, remain_len, measuring_wheel, CRC8 |
| `CutSensor` | Cutter hall sensor monitoring |

### Community Resources

- **[ityshchenko/klipper-cfs](https://github.com/ityshchenko/klipper-cfs)** — Community open-source CFS Klipper module (early stage, protocol documentation)
- **[CrealityOfficial/K2_Series_Klipper](https://github.com/CrealityOfficial/K2_Series_Klipper)** — Creality's official (incomplete) Klipper fork with binary blobs
