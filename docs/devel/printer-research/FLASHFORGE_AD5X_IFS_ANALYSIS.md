# FlashForge AD5X — IFS System Deep Analysis

**Date**: 2026-03-09 (unload semantics re-verified against firmware source 2026-06-11)
**Status**: Real device data analyzed (from user dump); §5/§12 unload commands verified line-by-line against extracted ZMOD source
**Source**: Live AD5X running ZMOD, user-provided debug dump + Python source. Unload/eject path re-verified against the real **ZMOD v1.7.1** release payload (`ghzserg/zmod` → `AD5X-zmod-1.7.1.tgz` → `mod/.shell/zmod_ifs.py` + `translate/en/ad5x_display_off.cfg`).

---

## 1. Architecture Overview

The AD5X IFS (Intelligent Filament Switching) is a **separate MCU** communicating over **UART serial** (`/dev/ttyS4` at 115200 baud). It is NOT a Klipper MCU — it's a standalone board (FFP0202_IFS_Con_Board, STM32-based) that the `zmod_ifs.py` Klipper extra talks to via F-commands.

**Key insight**: There are NO dedicated Klipper objects for IFS state. The system is entirely macro-driven, with state stored in JSON files on disk and Klipper `save_variables`.

### Data Flow
```
IFS Board ←serial→ zmod_ifs.py ←gcode→ Klipper macros ←websocket→ Moonraker ←http→ HelixScreen
```

---

## 2. Serial Protocol (F-Commands)

The IFS board accepts commands and returns text responses. The `zmod_ifs.py` module polls `F13` every 200ms when idle.

| Command | Function | Response |
|---------|----------|----------|
| `F10 C{port} L{len} S{speed}` | Feed filament into port | `F10 ok. FFS channel {port} feeding.` |
| `F11 C{port} L{len} S{speed}` | Retract filament from port | `F11 ok. FFS channel {port} exiting.` |
<!-- gcode-layer note: F10/F11/F112 are surfaced at the gcode layer as IFS_F10/IFS_F11/IFS_F112 (registered in zmod_ifs.py). IFS_F11 defaults: PRUTOK=1 LEN=90 SPEED=1200 CHECK=0. CHECK=0 = no presence guard, no heating. -->
> **Surfaced at the gcode layer**: `F10`/`F11`/`F112` are registered as `IFS_F10`/`IFS_F11`/`IFS_F112`. `IFS_F11` defaults `PRUTOK=1 LEN=90 SPEED=1200 CHECK=0` — `CHECK=0` means no presence guard and no heating (an unconditional cold per-lane retract).
| `F13` | Status query | `FFS_state: N silk_state: N stall_state: N chan: N ffs_channels_insert: N` |
| `F15 C` | Driver reset | `F15 ok.` |
| `F18` | Release ALL clamps | `F18 ok` |
| `F23 C{port}` | Mark filament as inserted | `F23 ok. chan {port}.` |
| `F24 C{port}` | Clamp filament (grip) | `F24 ok. chan {port}.` |
| `F39 C{port}` | Release clamp for port | `F39 ok. FFS channel {port} release.` |
| `F112` | Force stop all movement | `F112 ok.` or `F112 ok. yes.` |

> **These F-commands ARE exposed as core ZMOD gcode commands.** They are registered in `zmod_ifs.py` via `gcode.register_command('IFS_F11', ...)` (and `IFS_F10`, `IFS_F112`, `IFS_F13`/`F15`/`F18`/`F23`/`F24`/`F39`) — verified in ZMOD AD5X v1.7.1, `mod/.shell/zmod_ifs.py:152`. They are **not** serial-only. `IFS_F11 PRUTOK={port} LEN={len} SPEED={speed}` is a thin wrapper over the raw serial command `F11 C{port} L{len} S{speed}`: its handler `cmd_IFS_F11` contains zero heating logic, and with the default `CHECK=0` it requires no lane presence/runout sensor to read filament — it waits only for a generic ready state. That makes `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` an **unconditional COLD per-lane retract**. Heating in the normal unload flow lives in the **Python** handler `cmd_IFS_REMOVE_CURRENT_PRUTOK` (`zmod_ifs.py:1144` — issues `M104 S{config['temp']}` + `TEMPERATURE_WAIT` only when `temp < config['temp']` and `BYPASS_TEMPERATURE_CHECK=0`), not in `IFS_F11`. The toolhead unload reaches that heating via `IFS_REMOVE_CURRENT_PRUTOK` (no args — it auto-detects the active channel from `FFMInfo.channel`). **See §12 for the verified per-command truth table — note that bare `IFS_REMOVE_PRUTOK` with no `PRUTOK=` is a no-op, NOT a toolhead unload.**

### F13 Status Response Fields

| Field | Type | Meaning |
|-------|------|---------|
| `FFS_state` | int | State machine value (see below) |
| `silk_state` | bitmask | Per-port filament presence (bit0=port1, bit1=port2, etc.) |
| `stall_state` | bitmask | Per-port motion stall detection |
| `chan` | int | Current active channel |
| `ffs_channels_insert` | bitmask | Which port just had filament physically inserted (triggers autoinsert) |

### FFS State Machine Values

States are offset by 11 per channel: `base + (channel-1) * 11`

| Value | Meaning |
|-------|---------|
| 3 | Polling/querying |
| 5 | **Ready** (idle) |
| 7, 18, 29, 40 | Clamped (per channel) |
| 11, 22, 33, 44 | **Loading** (per channel) |
| 12, 23, 34, 45 | Releasing clamp (per channel) |
| 15, 26, 37, 48 | **Unloading** (per channel) |
| 127 | Driver error |

---

## 3. Klipper Objects Available via WebSocket

### Standard Filament Sensors (subscribable)

Per-port presence sensors:
- `filament_switch_sensor _ifs_port_sensor_1` through `_4`
- `zmod_ifs_switch_sensor _ifs_port_sensor_1` through `_4` (zmod wrappers, same data)

Per-port motion sensors:
- `filament_motion_sensor _ifs_motion_sensor_1` through `_4`
- `zmod_ifs_motion_sensor _ifs_motion_sensor_1` through `_4`

Toolhead sensors:
- `filament_switch_sensor head_switch_sensor` — filament in extruder
- `zmod_ifs_switch_sensor head_switch_sensor`
- `filament_motion_sensor ifs_motion_sensor` — main motion sensor

### Other Notable Objects
- `temperature_sensor weightValue` — load cell (filament weight, type `temperature_load`)
- `temperature_sensor head` — extruder head temp sensor (separate from heater)
- `save_variables` — persistent variables (colors, types, tool mapping)

### Tool Macros
- `gcode_macro T0` through `T15` (16 defined, only 4 physical ports)
- `gcode_macro SET_EXTRUDER_SLOT` — sets active slot: `_SET_EXTRUDER_SLOT SLOT={slot}`

---

## 4. State Storage

### JSON Config Files (on device filesystem)

| File | Contents |
|------|----------|
| `/usr/prog/config/Adventurer5M.json` | `FFMInfo.channel` (active port), `FFMInfo.ffmType{1-4}` (material), `FFMInfo.ffmColor{1-4}` (hex color) |
| `/usr/data/config/mod_data/file.json` | Tool-to-port mapping array (e.g., `[1, 2, 3, 4]` maps T0→port1) |
| `/usr/data/config/mod_data/filament.json` | Per-material filament profiles (speeds, lengths, temps) |

### Klipper save_variables (`/usr/data/config/mod_data/variables.cfg`)

Key IFS-related variables from a real device:

```ini
allowed_tool_count = 4
less_waste_colors = ['000000', 'FFFFFF', '8000FF', '004000']
less_waste_types = ['PLA', 'PLA', 'PLA', 'PLA']
less_waste_tools = [1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]
less_waste_current_tool = -1
less_waste_e_feedrate = 810
less_waste_e_feedrates = [130, 130, 810, 130, ...]
less_waste_external = 0
less_waste_extruder_port = 0
less_waste_extruder_temp = 220
autoinsert = 1
head_switch_sensor = 0
ifs_motion_sensor = 0
```

**Note**: `less_waste_colors` are hex strings WITHOUT `#` prefix. `less_waste_current_tool = -1` means no tool active.

---

## 5. Key G-Code Commands

### Tool Change Flow
1. `A_CHANGE_FILAMENT CHANNEL={n}` — orchestrates full tool change (save position, retract old, load new, purge, restore)
2. `END_CHANGE_FILAMENT` — restores temperature, fan speed, position after change
3. `INSERT_PRUTOK_IFS PRUTOK={n}` — load filament from port N (toolhead load; looks up temp from config)
4. `REMOVE_PRUTOK_IFS PRUTOK={n}` — `cmd_REMOVE_PRUTOK_IFS` (`zmod_ifs.py:682`) → `_REMOVE_PRUTOK_IFS` macro. Verified sequence: **`_G28` (homes only if `homed_axes` lacks `xyz` — see §12)** → `_GOTO_TRASH` → `IFS_REMOVE_CURRENT_PRUTOK TEMP={current extruder target}` → cold-jog of lane N (`IFS_F24`/`IFS_F11`/`IFS_F39 PRUTOK=N`). The **toolhead** part (`IFS_REMOVE_CURRENT_PRUTOK`) acts on the **currently active channel from `FFMInfo.channel`, NOT on N** — it heats the current channel to its config temp (if the extruder sensor reads filament present and the nozzle is below that temp) and unloads it; only the trailing cold jog uses N. So `PRUTOK=3` with a *different* slot loaded heats + unloads the loaded slot, ignoring 3. ⚠ Our earlier note claimed it errors `No filament N in IFS`. **That string is not on this path.** It exists (`print_result()` on `RET_SILK`, `zmod_ifs.py:789`), but `print_result()` is only ever reached from the *load* paths (`cmd_IFS_F10:916-918`, `cmd_INSERT_PRUTOK_IFS:856`) - none of `cmd_REMOVE_PRUTOK_IFS`, `cmd_IFS_REMOVE_PRUTOK`, `cmd_IFS_REMOVE_CURRENT_PRUTOK` or the cold jogs `IFS_F24`/`IFS_F11`/`IFS_F39` call it. The real error the unload chain raises is `"Failed to extract filament from extruder"` (`zmod_ifs.py:1140`), when the extruder sensor is still tripped after the retract. Do **not** treat it as "unload filament from port N independently." For a cold per-lane retract of an idle lane, use `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` (see §2).
5. `IFS_REMOVE_PRUTOK` — ⚠ **NOT a "retract currently-loaded" command.** `cmd_IFS_REMOVE_PRUTOK` (`zmod_ifs.py:1104`) reads `PRUTOK` (**default 0**) and **returns immediately when `prutok == 0`** (line 1113). Sent bare with no `PRUTOK=` it is a **guaranteed no-op** — no heat, no home, no retract. It is meant to be called internally with an explicit `PRUTOK=N` (it is, from `IFS_REMOVE_CURRENT_PRUTOK` at line 1163: `IFS_REMOVE_PRUTOK PRUTOK={current} FORCE=0`). To unload whatever is at the toolhead, call **`IFS_REMOVE_CURRENT_PRUTOK`** (no args) — see §12.
   - **`IFS_REMOVE_CURRENT_PRUTOK`** (`zmod_ifs.py:1144`) is the correct "unload the toolhead" entry: returns early if the extruder sensor reads no filament (`get_extruder_sensor()` on `temperature_sensor filamentValue`, present ≥ 0.72); otherwise reads the active channel, heats to its config temp if cold, and unloads it. **Does not home itself** (caller must home).

**Note on underscore variants**: `_INSERT_PRUTOK_IFS`, `_REMOVE_PRUTOK_IFS`, `_IFS_REMOVE_PRUTOK` are internal **gcode macros** that expect explicit `TEMP`/length params (default `TEMP` fallback: 220). The no-underscore public names (`INSERT_PRUTOK_IFS`, `REMOVE_PRUTOK_IFS`, `IFS_REMOVE_PRUTOK`, `IFS_REMOVE_CURRENT_PRUTOK`) are **Python commands** registered in `zmod_ifs.py` that look up per-lane config and fill those params in. Always use the no-underscore versions — but mind that bare `IFS_REMOVE_PRUTOK` no-ops without `PRUTOK=N` (above).
6. `SET_EXTRUDER_SLOT SLOT={n}` → `_SET_EXTRUDER_SLOT SLOT={n}` — tell firmware which slot is active
7. `SET_CURRENT_PRUTOK` — detect and set active filament based on sensor state

### Status/Control
- `IFS_STATUS` — returns JSON: `{"State": N, "Port1": bool, "Port2": bool, "Port3": bool, "Port4": bool, "Silk": N, "Chan": N, "Insert": N, "NeedInsert": bool, "Stall": bool, "stall_state": N}`
- `IFS_UNLOCK` → `IFS_F18` — release all clamps
- `_IFS_ON` / `_IFS_OFF` — enable/disable IFS system
- `COLOR` — update color display
- `LOAD_MATERIAL` — interactive filament load (4-stage: SELECT → HEATUP → ACTION → END)

### Filament Load/Unload (Manual)
- `LOAD_FILAMENT` — manual load
- `UNLOAD_FILAMENT` — manual unload
- `PURGE_FILAMENT` — manual purge

---

## 6. Python Module: `zmod_ifs.py`

### Key Class: `zmod_ifs`
- Opens `/dev/ttyS4` serial port in a background thread (`_sensor_reader`)
- Polls `F13` every 200ms when idle
- Command/response uses incrementing ID system (`F10#42` → response matched by ID)
- Thread-safe via `_command_lock` and `_ret_command_lock`
- Auto-detects IFS availability (empty response = IFS offline → `_IFS_OFF`)
- Auto-insert: when `NeedInsert` becomes true (filament physically pushed into port), triggers `_IFS_AUTOINSERT`

### Key Class: `IfsData`
- Thread-safe state container updated from F13 responses
- `get_values()` returns full state dict (used by `IFS_STATUS` command)
- `get_port(n)` → bool for filament presence per port
- `get_stall(n)` → bool for motion stall per port

### Filament Config System
- Default temps: PLA=220, PLA-CF=220, SILK=230, TPU=230, ABS=250, PETG=250, PETG-CF=250
- Per-material profiles in `filament.json` with configurable speeds, tube lengths, purge amounts
- `DEFAULT_FILAMENT_SETTINGS` dict defines all tuneable parameters

---

## 7. Detection Strategy for HelixScreen

### How to Detect AD5X IFS

**Primary**: Look for `zmod_ifs_switch_sensor` OR `zmod_ifs_motion_sensor` in `printer.objects.list`

**Secondary confirmation**: Check for `gcode_macro SET_EXTRUDER_SLOT` and `gcode_macro _IFS_AUTOINSERT`

**NOT detectable via**: Standard AFC/Happy Hare/toolchanger object prefixes (none present)

### How to Get State

**Option A (Preferred)**: Subscribe to `save_variables` → watch `less_waste_colors`, `less_waste_types`, `less_waste_current_tool`, `less_waste_tools`

**Option A2 (Native ZMOD)**: Download `Adventurer5M.json` via Moonraker file API (`config` root). Parse `FFMInfo.ffmColor1-4` and `FFMInfo.ffmType1-4`. Re-read on port sensor changes or when `RUN_ZCOLOR`/`CHANGE_ZCOLOR` appears in gcode response stream.

**Option B**: Execute `IFS_STATUS` G-code command and parse JSON response for real-time hardware state (port presence, stall, active channel)

**Option C**: Subscribe to individual `filament_switch_sensor _ifs_port_sensor_{1-4}` for per-port presence

### How to Control

All operations via G-code commands:
- Tool change: `A_CHANGE_FILAMENT CHANNEL={n}`
- Load: `INSERT_PRUTOK_IFS PRUTOK={n}` (toolhead load from port N)
- Unload (toolhead, currently-loaded filament): **`IFS_REMOVE_CURRENT_PRUTOK`** (no args — auto-detects active channel, heats if cold, unloads). ⚠ Do **not** send bare `IFS_REMOVE_PRUTOK` — it no-ops without `PRUTOK=N`. `REMOVE_PRUTOK_IFS PRUTOK={n}` also performs a toolhead unload of the *current* channel (plus a cold jog of lane N) and homes via `_G28` (conditional on `homed_axes` — §12); `PRUTOK=N` does **not** select an independent port for the toolhead part.
- Unlock: `IFS_UNLOCK`

> **Cold per-lane reverse-jog IS available at the gcode layer** via `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` (core ZMOD — no heat, no presence guard; see §2). The heated *toolhead* unload is `IFS_REMOVE_CURRENT_PRUTOK` (no args) or `REMOVE_PRUTOK_IFS PRUTOK=N` — **not** bare `IFS_REMOVE_PRUTOK`, which no-ops without `PRUTOK=N`. See §12 for the verified truth table.

---

## 8. Backend Implementation Notes

### Topology
**Linear** — 4 independent lanes → single hub at toolhead (no hub splitter like AFC). Each lane has its own motor, clamp, and presence sensor.

### Slot Count
Fixed at 4 (from `allowed_tool_count` variable, but hardware is always 4 ports).

### Color/Material Info
Available from `save_variables`:
- Colors: `less_waste_colors` (list of hex strings, no `#` prefix)
- Materials: `less_waste_types` (list of material name strings)
- Tool mapping: `less_waste_tools` (index = tool number, value = physical port)
- Active: `less_waste_current_tool` (-1 = none)

### Caveats
1. **No Moonraker database storage** — state is in Klipper `save_variables` and device JSON files
2. **Colors are hex WITHOUT #** — need to prepend for our UI: `"8000FF"` → `0x8000FF`
3. **Tool numbering**: T0-T15 are macros but only 4 physical ports (1-4). The `less_waste_tools` array maps logical tools to physical ports.
4. **IFS availability is dynamic** — the serial connection can drop. `zmod_ifs.ifs` bool tracks availability. When unavailable, all commands call `_IFS_OFF`.
5. **Autoinsert**: Physical insertion of filament into a port auto-triggers loading sequence (configurable via `autoinsert` variable).

---

## 9. Macro Packages: bambufy vs lessWaste

Two IFS macro packages exist for ZMOD. Both use the same `save_variables` schema, and both
include automatic backup/failover (`variable_backup`). Stock zMod also has its own switchover
**before any plugin is loaded** — `ANALOG_PRUTOK` (`zmod_ifs.py:cmd_ANALOG_PRUTOK`), wired
to `head_switch_sensor`'s `runout_gcode` in `ad5x_display_off.cfg:39-44`. zmod's user-facing
name for it is **"Infinite Spool Mode"**. The match rule is identical across all three paths
(stock zMod, bambufy, lessWaste): exact material AND exact colour AND port-present.

### Stock zMod (no plugin)
- `ANALOG_PRUTOK` runs always-on; no toggle. Confirmed from zmod 1.7.1 source and on-device
  by raza616.

### bambufy (Original)
- **Repo**: [function3d/bambufy](https://github.com/function3d/bambufy)
- IFS macro package, 4 tools (T0-T3)
- **Backup/failover**: `variable_backup` (**default on** = `1`); `_RUNOUT_HEAD` performs the
  slot swap. Same type+colour+present match as stock zMod.
- Overrides stock `head_switch_sensor` runout_gcode with its own `_RUNOUT_HEAD` (the only
  sensible design — running both paths in parallel would double-handle every runout).
- `PAUSE REASON` values emitted: `jam`, `broken`, `runout`, `empty`, `backup`, `nobackup`,
  `loading` (`nobackup` is bambufy-only — `bambufy.cfg:149`, on a backup-enabled runout with
  no same-type+colour match).

### lessWaste (Enhanced Fork)
- **Repo**: [Hrybmo/lessWaste](https://github.com/Hrybmo/lesswaste)
- Based on bambufy V1.2.10, adds significant features:
  - **16 virtual tools** (T0-T15) mapped to 4 physical ports via `variable_tools`
  - **Backup/failover**: `variable_backup` (**default off** = `0`); same `_RUNOUT_HEAD` shape
    as bambufy. There is **no** `variable_backup_filament_spent` in source
    (`lesswaste_src.cfg` 1995 lines, zero matches) — "consumed" slots are inferred from
    `filament_detected == false` on the port sensor, not tracked in a variable.
  - **Virtual channel mode**: `variable_is_virtual_mode` — allows more slicer tools than physical slots
  - **Purge control**: in-tower (`_NOPOOP`) or out-the-back, configurable flush volumes
  - **Same-filament purge skip**: `variable_same_filament_purge`
  - **Per-tool feedrates**: `variable_e_feedrates` array
  - **Auto-recovery**: `_CHECK_FILAMENT` macro detects which port is loaded after unexpected state
  - **PAUSE REASON values**: `jam`, `broken`, `runout`, `empty`, `backup`, `loading`
  - **Start UI**: Dialog-based tool-to-port assignment before printing (`_IFS_COLORS`)
  - **KAMP**: Adaptive bed mesh at print start (`variable_kamp`)
  - **IFS unlock after boot**: `variable_ifs_unlock_after_boot` for stock screen glitch workaround

### lessWaste _IFS_VARS (Complete Variable List)

```ini
variable_filament_unload_before_cutting: 24
variable_filament_drop_length: 35
variable_filament_unload_after_cutting: 2
variable_filament_unload_speed: 1500
variable_nozzle_cleaning_length: 70
variable_filament_load_speed: 900
variable_filament_home_speed: 700
variable_filament_insert_speed: 2800
variable_filament_tube_length: 1000
variable_filament_catch_length: 5
variable_filament_pressure_length: 1
variable_filament_autoinsert_full_length: 550
variable_tools: [1,2,3,4,5,5,5,5,5,5,5,5,5,5,5,5]  # index=tool, value=port
variable_external: 0
variable_extruder_port: -1
variable_current_tool: -1
variable_extruder_temp: 0
variable_extruder_fan: 0
variable_bed_temp: 0
variable_e_feedrate: 130
variable_e_feedrates: []
variable_consume: 0
variable_kamp: 0
variable_line_purge: 0
variable_backup: 0
variable_types: ['PLA','PLA','PLA','PLA', ...]  # 16 entries
variable_colors: ['000000','000000','000000','000000', ...]  # 16 entries
variable_start: 0
variable_sbros_trash_speed: 4000
variable_info_dialog: 1
variable_same_filament_purge: 1
variable_ifs_unlock_after_boot: 0
```

### Zmod Slot Renumbering Issue

Zmod has an option to rename slots from 0-indexed (0,1,2,3) to 1-indexed (1,2,3,4). When enabled, the slicer sends T1-T4 instead of T0-T3, causing the `_T` macro to look up `ifs.tools[1]` through `ifs.tools[4]` instead of `ifs.tools[0]` through `ifs.tools[3]`. This is a slicer↔macro configuration mismatch.

**HelixScreen is not affected** — we read the `less_waste_tools` mapping array directly, which is always consistent regardless of slot naming. The off-by-one only matters between the slicer's `T[next_extruder]` output and the Klipper `_T` macro.

---

## 10. Open Questions

1. ~~What does the Moonraker object state actually look like for `zmod_ifs_*` objects?~~ (User's Moonraker API was behind broken reverse proxy — couldn't get state dump)
2. Does `zmod_ifs` expose any Klipper object status attributes (like `get_status()` in the Python module)? Need to check if it implements that method.
3. ~~What does `zmod_color` expose?~~ **Answered**: Source at `github.com/ghzserg/zmod_ff5x` branch `1.6`, file `.shell/zmod_color.py`. Re-reads `Adventurer5M.json` on every G-code command (no cache). When display is OFF (HelixScreen case), reads/writes file directly. Colors use `#` prefix in the file.
4. How does the `file.json` tool mapping interact with multi-color prints? (Slicer outputs T0/T1/etc., mapping resolves to physical ports)

---

## 11. Files Reference

### On Device
| Path | Contents |
|------|----------|
| `/opt/config/mod/.shell/zmod_ifs.py` | IFS Klipper module (symlinked into klippy/extras/) |
| `/opt/config/mod/.shell/zmod_ifs_motion_sensor.py` | Motion sensor wrapper |
| `/opt/config/mod/.shell/zmod_ifs_switch_sensor.py` | Switch sensor wrapper |
| `/opt/config/mod/.shell/zmod_color.py` | Color management module |
| `/usr/prog/config/Adventurer5M.json` | Active channel, filament types/colors |
| `/usr/data/config/mod_data/file.json` | Tool→port mapping |
| `/usr/data/config/mod_data/filament.json` | Per-material profiles |
| `/usr/data/config/mod_data/variables.cfg` | Klipper save_variables |

### Config Structure (ZMOD)
| Path | Purpose |
|------|---------|
| `/opt/config/printer.cfg` | Main config (includes base + mod) |
| `/opt/config/printer.base.cfg` | MCU, steppers, bed, kinematics |
| `/opt/config/mod/ad5x.cfg` | AD5X-specific: sensors, IFS config, save_variables |
| `/opt/config/mod/display_off.cfg` | IFS + sensor macros (tool change, load, unload) |
| `/opt/config/mod/ad5x_config_off.cfg` | SAVE_ZMOD_DATA macro, IFS color management |
| `/opt/config/mod/base_mod.cfg` | PAUSE/RESUME/CANCEL overrides, START_PRINT/END_PRINT |
| `/opt/config/mod/client.cfg` | Client variable macros |
| `/opt/config/mod/motion_sensor.cfg` | Runout detection macros |

---

## 12. Unload Semantics — VERIFIED against ZMOD v1.7.1 source + on-device cfg (2026-06-12)

Re-verified line-by-line against the extracted firmware (`mod/.shell/zmod_ifs.py` + `translate/en/ad5x_display_off.cfg`) **and against raza616's actual on-device config** (bundle `7AC4SDEX`, v0.99.76). This supersedes the earlier empirical-only notes. The `_`-prefixed names are gcode macros; the no-underscore names are Python commands in `zmod_ifs.py`.

> ⚠ **The 2026-06-11 fix (send bare `IFS_REMOVE_CURRENT_PRUTOK`) was incomplete** — corrected 2026-06-12. It missed that the Python command *itself* early-returns on an empty extruder sensor, and that the firmware's own working button is the `_IFS_REMOVE_CURRENT_PRUTOK` **macro** (with `NEED_TRASH=1 BYPASS_TEMPERATURE_CHECK=1`), not the bare command. See the corrected dispatch below.

### Per-command truth table (what each command our backend can send actually does)

| Command (as sent by HelixScreen) | Homes? | Heats? | Acts on | Net effect |
|---|---|---|---|---|
| **`IFS_REMOVE_PRUTOK`** (bare, no `PRUTOK=`) | no | no | nothing | **NO-OP.** `cmd_IFS_REMOVE_PRUTOK` defaults `PRUTOK=0` and `return`s at `:1113` on `prutok == 0`. Does nothing at all. |
| **`IFS_REMOVE_CURRENT_PRUTOK`** (bare Python cmd, no args) | no (caller homes) | yes *only if extruder loaded* | the **active** channel (`FFMInfo.channel`) | **Early-returns (NO-OP) if the extruder sensor reads empty (`:1149`).** Else, with the bare call (`TEMP=0`, `BYPASS=0`, `NEED_TRASH=0`): `M104 S{config.temp}` + `TEMPERATURE_WAIT` (`:1159`), then `IFS_REMOVE_PRUTOK PRUTOK={active} FORCE=0 NEED_TRASH=0` — **retracts but skips the trash drop and leaves the nozzle hot.** |
| **`_IFS_REMOVE_CURRENT_PRUTOK`** (the firmware's "Remove from extruder" **macro**) | **if unhomed** (`_G28`) | yes *only if extruder loaded* (same empty-sensor early-return) | the **active** channel | Firmware's own UI button. Self-homes, calls `IFS_REMOVE_CURRENT_PRUTOK NEED_TRASH=1 BYPASS_TEMPERATURE_CHECK=1` (trash drop, no pre-heat wait), then `SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0` + `COLOR`. **This is what HelixScreen dispatches for a loaded toolhead.** |
| **`REMOVE_PRUTOK_IFS PRUTOK=N`** | **if unhomed** (`_G28`) | yes (via `IFS_REMOVE_CURRENT_PRUTOK`, same rule as above) | **active** channel for the toolhead unload + cold-jogs **lane N** | Homes, unloads the *currently-loaded* filament (ignores N for the heated part), then cold-jogs lane N (`IFS_F24`/`IFS_F11`/`IFS_F39`). |
| **`INSERT_PRUTOK_IFS PRUTOK=N`** | **if unhomed** (`_G28`) | yes (`M104` + `TEMPERATURE_WAIT` to lane N's configured temp) | **lane N**, after an implicit unload of whatever is seated | `cmd_INSERT_PRUTOK_IFS` (`zmod_ifs.py:732`) fills the lane's config in and runs `_INSERT_PRUTOK_IFS`: `_G28` -> `IFS_REMOVE_CURRENT_PRUTOK TEMP={current}` -> `_GOTO_TRASH` -> heat -> `IFS_F24`/`IFS_F10` feed -> purge/wipe -> `IFS_F23` -> `_SET_EXTRUDER_SLOT` + `SET_CURRENT_PRUTOK`. **This is what HelixScreen dispatches for a load.** |
| **`IFS_F11 PRUTOK=N … CHECK=0`** | no | no | **lane N** only | Unconditional cold per-lane retract (no presence guard). Used by `eject_lane()`. |

#### ⚠ `_G28` is a CONDITIONAL home, not an unconditional one

Every "homes" cell above routes through the `_G28` macro, whose entire body is:

```jinja
[gcode_macro _G28]
gcode:
    {% if "xyz" not in printer.toolhead.homed_axes %}
        _HOME
    {% endif %}
```

(`mod/_mod/translate/*/base.cfg:88`, byte-identical in all 12 language copies of ZMOD 1.7.1.)

So a firmware macro homes an **unhomed** toolhead and does nothing on a homed one. Two consequences worth stating, because reading `_G28` as a plain home has already produced one wrong bug report (prestonbrown/helixscreen#1248, "load double-homes"):

1. **`ensure_homed_then()` + a self-homing macro does not home twice.** Our `G28` (which ZMOD's own `G28` override at `base.cfg:1949` turns into `_HOME` when sent with no axis params) leaves `homed_axes == "xyz"`, and the macro's `_G28` then falls through. One home total.
2. **The homed-axes guard is not a safety guarantee.** `homed_axes` is cleared by a Klipper error, an `M84`, or a cold resume, so the buried `_G28` can still fire when a job owns the toolhead. That, plus the unguarded `_GOTO_TRASH` / `_SBROS_TRASH` / `_CLEAR_REZINA` moves in the same macros, is why `filament_ops_self_home()` refuses filament ops while PAUSED as well as PRINTING (bundle `XWPBR2DX`, commit `329e731e9`).

`get_extruder_sensor()` reads the toolhead filament ADC (`temperature_sensor filamentValue`, present ≥ 0.72). `get_current_channel_from_config()` reads `FFMInfo.channel` from `Adventurer5M.json` (0 = none).

### ⚠ HelixScreen bug this exposes (root cause of "homes and then nothing happens")

The symptom has had **two distinct firmware no-op preconditions**, both producing "homes (our G28) then nothing":

1. **Until 2026-06-11:** `unload_filament()` dispatched **bare `IFS_REMOVE_PRUTOK`** via `ensure_homed_then()`. That command no-ops on its `PRUTOK=0` default (`:1113`). The 2026-06-10 phase-tracker work (`4ebfd4135`/`2a7cff44e`) only added UI progress synthesis and did not change the dispatch.
2. **The 2026-06-11 fix (bare `IFS_REMOVE_CURRENT_PRUTOK`) did not resolve it** — bundle `7AC4SDEX` (v0.99.76) shows `head_switch_sensor` **empty** while `ifs_motion_sensor` reads present: filament is in the lane, not the toolhead. `cmd_IFS_REMOVE_CURRENT_PRUTOK` early-returns on the empty extruder sensor (`:1149`), so after our G28 it still did nothing. We swapped one no-op precondition (PRUTOK=0) for another (empty extruder sensor).

- **✅ Fixed 2026-06-12:** `unload_filament()` now **branches on the toolhead sensor (`head_filament_`)**:
  - **Toolhead loaded** → dispatch the firmware's own macro **`_IFS_REMOVE_CURRENT_PRUTOK`** raw via `execute_gcode()` (the macro self-homes — we must NOT wrap it in `ensure_homed_then()`, which would home twice). This matches the "Remove from extruder" button observed working on raza616's device, and gets the trash drop + post-unload nozzle cool-down the bare command skipped.
  - **Toolhead empty** → route to **`eject_lane()`** (cold `IFS_F11 PRUTOK=N CHECK=0`) to pull the filament back from the lane, instead of homing into a guaranteed no-op.
  - Regression-guarded by `tests/unit/test_ams_backend_ad5x_ifs.cpp`: "unload_filament dispatches the firmware `_IFS_REMOVE_CURRENT_PRUTOK` macro" and "unload_filament with empty toolhead routes to cold lane eject (7AC4SDEX)".
- **The "heats even for filament not loaded" complaint** was the old `REMOVE_PRUTOK_IFS PRUTOK=N` path (its `IFS_REMOVE_CURRENT_PRUTOK` step heats+unloads the *currently active* channel regardless of N). Neither current path can heat an empty toolhead — the loaded-head macro early-returns on an empty extruder sensor, and the empty-head branch is a cold lane eject.

### `SAVE_ZMOD_DATA REMOVE_FILAMENT=…` (raza616's config question)

`REMOVE_FILAMENT` is **persisted config, not a heat toggle.** `SAVE_ZMOD_DATA` only does `SAVE_VARIABLE VARIABLE=remove_filament VALUE={0|1}` (`ad5x_config_native.cfg:106`). Setting `REMOVE_FILAMENT=0` will **not** stop the heat-before-unload — that heating is in `cmd_IFS_REMOVE_CURRENT_PRUTOK`, gated only by `temp < config.temp` and `BYPASS_TEMPERATURE_CHECK`.

### Caveat — ZMOD version + head-sensor dependency
The `prutok == 0 → return` and empty-extruder-sensor early-returns are confirmed in **v1.7.1** and in raza616's on-device cfg. Older ZMOD may differ; the fix is correct on current firmware regardless. The 2026-06-12 routing depends on our `head_filament_` tracking the same physical state the firmware reads via `get_extruder_sensor()`, and that is where it is weakest.

**`head_filament_` is not the switch sensor.** `parse_head_sensor()` is called from *both* sensor branches of `handle_status_update()`: the switch branch (`filament_switch_sensor head_switch_sensor` / `zmod_ifs_switch_sensor head_switch_sensor`) and the motion branch (`filament_motion_sensor ifs_motion_sensor` / `zmod_ifs_motion_sensor ifs_motion_sensor`). The motion sensor is device-confirmed to read `filament_detected=false` on a lane that is loaded but idle, which the class-header NOTE and the `head_filament_` member comment in `include/ams_backend_ad5x_ifs.h` both spell out. So "a head-sensor false-negative" is not a hypothetical here; it is the ordinary reading whenever the last frame to write `head_filament_` came from the motion sensor. The consequence is unchanged - firmware's toolhead unload would itself no-op in that same state, so cold eject remains the best available action - but the empty-head branch is taken far more often than "false negative" implies.

Everywhere an *authoritative* empty head is required, the code uses the switch pair instead: `head_switch_seen_ && !head_switch_present_`, spelled `head_empty_authoritative` at the two `apply_zcolor_result` seated-channel gates, and fed to the #1250 unattended-runout detector by `note_head_switch_reading_locked()`, which only the switch branch calls.

**The unload router now uses the switch pair too.** `head_empty_for_unload_routing_locked()` is the single predicate behind `do_unload_filament()`'s eject-vs-cut branch, its context-menu mirror `slot_unloads_to_toolhead()`, and `eject_lane()`'s seated-lane refusal:

```
head_switch_seen_ ? !head_switch_present_ : !head_filament_
```

Positive switch evidence is required to claim "empty", because the errors are not symmetric. A false *empty* sends seated, un-cut filament to the cold `IFS_F11` retract and grinds it (raza616 #981; `5HR3HHS6` is the same hazard reached via the dropped active pointer). A false *loaded* only reaches `_IFS_REMOVE_CURRENT_PRUTOK`, which early-returns on the extruder sensor — "homes and nothing happens", annoying and harmless. `head_filament_`'s known failure mode (the motion-sensor false negative) produces the dangerous direction, so it can no longer claim "empty" by itself. On motion-only firmware `head_switch_seen_` stays false and the fallback is exactly the historical `!head_filament_` — no silent behaviour change on a device that never publishes a switch. Bundle `7AC4SDEX` (switch empty, motion present) still routes to the cold eject, now for a stronger reason: switch-absent is authoritative regardless of what motion says.

All three sites share the one predicate by necessity: the router hands an empty-head unload to `eject_lane()`, so a refusal keyed on a different notion of "empty" would bounce the very call the router just made ("Unload from toolhead first" in answer to an unload). `can_unload_from_toolhead()` deliberately stays on `head_filament_` — it decides only whether the Unload affordance is *offered*, and its dangerous direction is the opposite one (a false "empty" hides the #995 recovery affordance while filament is physically seated).

> **Still a proxy, not the firmware's gate.** `cmd_IFS_REMOVE_CURRENT_PRUTOK` early-returns on `get_extruder_sensor()` (`zmod_ifs.py:1149`), which is neither sensor: it reads the `temperature_sensor filamentValue` ADC, `result = value >= 0.72` when `value > 0.3`, and `True` otherwise — a missing reading counts as loaded (`zmod_ifs.py:353-361`). HelixScreen does not subscribe to `filamentValue` anywhere. Subscribing to it is the proper fix and would make the predicate exact; it needs a real AD5X to confirm the object is published, and we have neither hardware nor an `ad5x` mock profile. **Needs raza616 field re-test** (we ship AD5X blind — no test device).

### Other consequences (unchanged, still valid)
- A cold per-lane reverse-jog **IS available at the gcode layer** via `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` — a thin wrapper over raw serial `F11 C{port}…` with zero heating logic.
- Keep the currently-loaded slot unloadable even after a runout clears the head sensor (#995).
- A true cold per-lane eject is **not firmware-blocked**: call `IFS_F11` directly to recover a snapped chunk stuck in an idle lane (#996). No hot nozzle involved.

### Provenance
Extracted firmware kept out-of-tree at `../zmod` (cloned from `github.com/ghzserg/zmod`, release asset `AD5X-zmod-1.7.1.tgz`). Key files: `mod/.shell/zmod_ifs.py` (`cmd_REMOVE_PRUTOK_IFS:682`, `cmd_IFS_REMOVE_PRUTOK:1104`, `cmd_IFS_REMOVE_CURRENT_PRUTOK:1144`, `cmd_IFS_F11:939`), `mod/_mod/translate/en/ad5x_display_off.cfg` (`_REMOVE_PRUTOK_IFS:502`, `_IFS_REMOVE_PRUTOK:562`, `_IFS_REMOVE_CURRENT_PRUTOK:609`).
