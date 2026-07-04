<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Creality K2 Series Support

HelixScreen has a cross-compilation target for the Creality K2 series of enclosed CoreXY printers. The K2 series runs Klipper with stock Moonraker, making it a natural fit for HelixScreen.

## Supported Models

All K2 models use Allwinner ARM Cortex-A7 dual-core processors running Tina Linux (OpenWrt-based).

| Model   | Build Volume   | Display       | Chamber Heater | CFS         | Status                        |
| ------- | -------------- | ------------- | -------------- | ----------- | ----------------------------- |
| K2      | 260 mm cubed   | 4.3" 480x800  | No             | Optional    | Untested                      |
| K2 Pro  | 300 mm cubed   | 4.3" 480x800  | Yes (60C)      | Optional    | Untested                      |
| K2 Plus | 350 mm cubed   | 4.3" 480x800  | Yes (60C)      | Optional    | Untested                      |
| K2 Max  | 350 mm cubed   | 4.3" 480x1600 | Yes            | Yes (combo) | **Hardware confirmed**        |
| K2 SE   | 220x215x245 mm | Unknown       | No             | Unknown     | User-confirmed install (wget) |

## Hardware (Confirmed on K2 Max — 2026-03-23)

| Spec         | Value                                                            |
| ------------ | ---------------------------------------------------------------- |
| SoC          | Allwinner sun8iw20p1 (ARM Cortex-A7, dual-core, 57 BogoMIPS)     |
| Display      | 480x1600 portrait, 32bpp, fbdev (`/dev/fb0`)                     |
| Stock UI     | `/usr/bin/display-server` (must be stopped to use framebuffer)   |
| RAM          | 488 MB total                                                     |
| Storage      | 27.5 GB on `/mnt/UDISK`                                          |
| OS           | OpenWrt 21.02-SNAPSHOT, Linux 5.4.61 armv7l                      |
| Init System  | procd (OpenWrt-style, NOT systemd)                               |
| MCU          | GD32F303RET6 on `/dev/ttyS2` @ 230400 baud                       |
| Nozzle MCU   | GD32F303CBT6 on `/dev/ttyS3` @ 230400 baud                       |
| Moonraker    | Port 7125 (direct), port 4408 (nginx proxy)                      |
| Klipper UDS  | `/tmp/klippy_uds`                                                |
| SSH          | `root` / `creality_2024` (enable via Settings menu)              |
| Config path  | `/mnt/UDISK/printer_data/config/`                                |
| Klipper path | `/usr/share/klipper/`                                            |
| Logs path    | `/mnt/UDISK/printer_data/logs/`                                  |
| Gcode path   | `/mnt/UDISK/printer_data/gcodes/` (also `/root/klipper/gcodes/`) |
| Web server   | `web-server` on ports 80, 443, 9998, 9999                        |
| ADB          | `adbd` on port 5037                                              |
| WebRTC       | `webrtc_local` on port 8000 (camera)                             |

### Notes

- **No curl** — BusyBox wget only (no HTTPS support). Use `python3 urllib` for HTTP requests.
- **armv7l** — Dual-core Cortex-A7 (NOT Cortex-A53). Lower performance than K1 series.
- **480x1600 display** — The K2 Max has a taller display than other K2 models (480x800). Needs software rotation to landscape. The `lcm_id=gc9503cv_ue_480_800` in cmdline suggests the base panel is 480x800 but the K2 Max may use a different panel.
- **Python 3.9** — Available at `/usr/bin/python3`.

## Cross-Compilation

The K2 target uses Bootlin's armv7-eabihf musl toolchain with fully static linking. We target armv7 (32-bit) because Tina Linux uses 32-bit userland.

### Build via Docker (Recommended)

```bash
# Build the Docker toolchain and cross-compile (first time only — cached after)
make k2-docker
```

The Docker image (`docker/Dockerfile.k2`) downloads [Bootlin's armv7-eabihf musl toolchain](https://toolchains.bootlin.com/) (stable-2024.02-1).

### Build Directly (Requires Toolchain)

```bash
make PLATFORM_TARGET=k2 -j
```

### Build Configuration

| Setting         | Value                                               |
| --------------- | --------------------------------------------------- |
| Architecture    | armv7-a (hard-float, NEON VFPv4)                    |
| Toolchain       | `arm-buildroot-linux-musleabihf-gcc` (Bootlin musl) |
| Linking         | Fully static (musl)                                 |
| Display backend | fbdev (`/dev/fb0`)                                  |
| Input           | evdev (auto-detected)                               |
| SSL             | Disabled (Moonraker is local on port 4408)          |
| Optimization    | `-Os` with LTO (size-optimized)                     |
| Platform define | `HELIX_PLATFORM_K2`                                 |

### CI/Release Status

The K2 target **is included** in the GitHub Actions release pipeline (`.github/workflows/release.yml`). Release artifacts are built automatically:

```bash
# Manual packaging
make package-k2
```

## Installation

### Prerequisites

- A Creality K2, K2 Pro, K2 Plus, or K2 Max printer
- **Stock firmware with root access** — no custom firmware (Guilouz, etc.) required
- Root access enabled: Settings > "Root account information" > acknowledge disclaimer > wait 30 seconds > press "Ok"
- SSH access: `ssh root@<printer-ip>` (password: `creality_2024`)
- Find your printer's IP: Settings > Network on the printer touchscreen

**Important:** K2 hostname does NOT resolve via mDNS — always use the IP address.

### Quick Install

```bash
# 1. Build the K2 binary (Docker — works on any host OS)
make k2-docker

# 2. Deploy and run in foreground (first time — watch the output)
make deploy-k2-fg K2_HOST=192.168.x.x

# 3. For production: deploy in background
make deploy-k2 K2_HOST=192.168.x.x
```

### All Deploy Targets

```bash
# Full deploy (binary + assets + config + platform hooks)
make deploy-k2 K2_HOST=192.168.x.x

# Deploy and run in foreground with debug logging
make deploy-k2-fg K2_HOST=192.168.x.x

# Deploy binary only (fast iteration during development)
make deploy-k2-bin K2_HOST=192.168.x.x

# SSH into the printer
make k2-ssh K2_HOST=192.168.x.x

# Full build + deploy + run cycle
make k2-test K2_HOST=192.168.x.x
```

Deploy directory: `/opt/helixscreen` (override with `K2_DEPLOY_DIR`). SSH credentials: `root`/`creality_2024` (override with `K2_USER`/`K2_PASS`).

**Note**: The K2 uses BusyBox (OpenWrt), so deployment uses tar/ssh transfer instead of rsync.

### What Happens on Deploy

1. Stops any running HelixScreen processes
2. Deploys platform hooks (`config/platform/hooks-k2.sh` → `/opt/helixscreen/platform/hooks.sh`)
3. Transfers binaries, assets, XML layouts, and config
4. Installs SysV init script at `/etc/init.d/S99helixscreen` for boot persistence
5. Ensures `/opt/helixscreen` symlink points to `/mnt/UDISK/helixscreen`
6. Platform hooks stop the stock Creality UI (`display-server`, `Monitor`, etc.) via procd
7. Platform hooks start `wpa_supplicant` to replace the stock `wifi-server`
8. Starts HelixScreen on the framebuffer

### Reverting to Stock UI

To restore the stock Creality touchscreen:

```bash
ssh root@<printer-ip>
killall helix-screen helix-splash helix-watchdog 2>/dev/null
/etc/init.d/app enable   # Re-enable stock UI on boot
/etc/init.d/app start    # Start stock UI now
```

### Display Backend

HelixScreen renders directly to `/dev/fb0`. The platform hooks stop the stock `display-server` to release the framebuffer. This is handled automatically by the deploy targets.

The K2 Max framebuffer is **480x1600 portrait** — HelixScreen will need software rotation to landscape mode, plus touch coordinate transform.

### Touch Input

HelixScreen uses evdev and auto-detects the capacitive touch controller. Running as root (default) avoids permission issues on `/dev/input/event*`.

## CFS (Creality Filament System) — Full Protocol Reference

The CFS is a multi-material filament management system using RS-485 serial communication. Each CFS unit holds 4 spools; up to 4 units can be daisy-chained for 16-color printing.

### Architecture

The CFS is implemented as Klipper modules, but the core logic is in **closed-source Cython `.so` blobs**:

| Module                            | Source                   | Function                                                              |
| --------------------------------- | ------------------------ | --------------------------------------------------------------------- |
| `box.py`                          | 3-line shim              | Loads `MultiColorMeterialBoxWrapper` from `box_wrapper.cpython-39.so` |
| `box_wrapper.cpython-39.so`       | **Binary blob** (Cython) | All CFS protocol, RFID, motor control, filament state                 |
| `auto_addr.py`                    | 3-line shim              | Loads `AutoAddrWrapper` from `auto_addr_wrapper.cpython-39.so`        |
| `auto_addr_wrapper.cpython-39.so` | **Binary blob**          | RS-485 device discovery and address assignment                        |
| `filament_rack`                   | Klipper module           | External filament rack sensor                                         |

### Klipper Configuration

From the active `box.cfg` on the K2 Max:

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

| Detail          | Value                                                                                |
| --------------- | ------------------------------------------------------------------------------------ |
| Bus             | `/dev/ttyS5` at 230400 baud                                                          |
| Frame format    | `0xF7 \| addr \| length \| status \| function_code \| data[] \| CRC8`                |
| Slave addresses | `0x01-0x04` (individual CFS units), `0xFE` (broadcast boxes), `0xFF` (all devices)   |
| Commands        | Connect, RFID read, motor control, extrude/retract, version/SN query, sensor queries |

### Moonraker Object: `box`

The `[box]` Klipper module exposes full CFS state via Moonraker's `printer.objects.query`. This is the primary interface for HelixScreen integration.

**Query**: `GET /printer/objects/query?box`

#### Top-Level Fields

| Field            | Type   | Description                                               |
| ---------------- | ------ | --------------------------------------------------------- |
| `state`          | string | Connection state: `"connect"`, `"None"`                   |
| `filament`       | int    | Filament loaded flag (1 = loaded)                         |
| `enable`         | int    | CFS enabled for printing                                  |
| `auto_refill`    | int    | Auto-refill (backup spool) enabled                        |
| `filament_useup` | int    | Filament use-up tracking enabled                          |
| `map`            | dict   | Tool-to-slot mapping: `{"T1A": "T1A", "T1B": "T1B", ...}` |
| `same_material`  | array  | Groups of slots with matching material for auto-refill    |

#### Per-Unit Fields (`T1`, `T2`, `T3`, `T4`)

Each CFS unit (T1=unit 1, T2=unit 2, etc.) has:

| Field               | Type     | Example                                     | Description                                                                                                           |
| ------------------- | -------- | ------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| `state`             | string   | `"connect"` / `"None"`                      | Unit connection state                                                                                                 |
| `filament`          | string   | `"None"`                                    | Currently loaded filament                                                                                             |
| `temperature`       | string   | `"27"`                                      | Internal temperature (C)                                                                                              |
| `dry_and_humidity`  | string   | `"48"`                                      | Relative humidity (%)                                                                                                 |
| `filament_detected` | string   | `"None"`                                    | Filament detection state                                                                                              |
| `measuring_wheel`   | string   | `"None"`                                    | Measuring wheel state                                                                                                 |
| `version`           | string   | `"1.1.3"`                                   | Firmware version                                                                                                      |
| `sn`                | string   | `"10000882925..."`                          | Serial number                                                                                                         |
| `mode`              | string   | `"0"`                                       | Operating mode                                                                                                        |
| `vender`            | array[4] | hex strings / `"none"` / `"unknown"`        | Raw RFID vendor data per slot, `"none"` if no filament presented, `"unknown"` if the filament doesn't have a RFID tag |
| `remain_len`        | array[4] | `["35","57","52","52"]`                     | Remaining filament length (meters) per slot                                                                           |
| `color_value`       | array[4] | `["0000000","0FFFFFF","00A2989","0C12E1F"]` | Filament color hex per slot                                                                                           |
| `material_type`     | array[4] | `["101001","101001","101001","101001"]`     | Material type code per slot                                                                                           |
| `uuid`              | array    | ints                                        | RFID UUID bytes                                                                                                       |
| `change_color_num`  | array[4] | `["-1","-1","-1","-1"]`                     | Color change count per slot                                                                                           |

#### Slot Addressing

Slots use a `T{unit}{letter}` naming convention:

- **Unit**: 1-4 (CFS unit number)
- **Letter**: A-D (slot within unit)
- Example: `T1A` = Unit 1, Slot A; `T3C` = Unit 3, Slot C

The `map` field maps virtual tool names to physical slots (usually 1:1 unless remapped).

#### Material Type Codes

The `material_type` field uses a format: `1XXXXX` where `XXXXX` is the material database ID.

| Code     | Material              |
| -------- | --------------------- |
| `101001` | Creality Hyper PLA    |
| `102001` | Creality Hyper PLA-CF |
| `106002` | Creality Hyper PETG   |
| `103001` | Creality Hyper ABS    |
| `100001` | Generic PLA           |
| `100003` | Generic PETG          |
| `100004` | Generic ABS           |
| `100005` | Generic TPU           |

The full material database is at `/mnt/UDISK/creality/userdata/box/material_database.json` (77 materials, fetched from Creality cloud). Material entries include brand, name, `meterialType` (sic — Creality typo), density, diameter, temp range.

#### Color Values

Colors are 7-character hex strings with a leading `0`: `"0RRGGBB"`. Examples:

- `"0000000"` = black
- `"0FFFFFF"` = white
- `"00A2989"` = teal
- `"0C12E1F"` = orange-red

#### Disconnected Units

Units that are not connected report all fields as `"None"` or `"-1"`.

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

| Object                                   | Description                                             |
| ---------------------------------------- | ------------------------------------------------------- |
| `fan_feedback`                           | Fan speeds: fan0-fan4                                   |
| `filament_switch_sensor filament_sensor` | Filament runout sensor state                            |
| `load_ai`                                | AI print quality monitoring (waste detection)           |
| `heater_generic chamber_heater`          | Chamber heater control                                  |
| `temperature_fan chamber_fan`            | Chamber cooling fan (carries the M141 maintain ceiling) |
| `temperature_sensor chamber_temp`        | Chamber temperature sensor                              |
| `motor_control`                          | Motor ready state, cutter position                      |
| `belt_mdl mdlx` / `belt_mdl mdly`        | Belt tension measurement                                |
| `prtouch_v3`                             | Pressure-based Z probe                                  |
| `z_align` / `z_tilt`                     | Z axis alignment                                        |
| `custom_macro`                           | Custom macro management                                 |
| `fan_feedback`                           | RPM feedback for all fans                               |

#### Chamber Heating (M141)

The K2 chamber is controlled by the `M141` macro, which coordinates two objects: `heater_generic chamber_heater` and `temperature_fan chamber_fan`. HelixScreen routes chamber sets through `M141 S{temp}` (rather than a raw `SET_HEATER_TEMPERATURE`), and the macro's behavior depends on the setpoint:

- `M141 S0` → **Off** (heater target 0; cooling fan reset to its configured resting target, e.g. 35°C)
- `M141 S{≤40}` → **Maintaining** — holds a cooling ceiling via `temperature_fan chamber_fan`; the heater target stays 0
- `M141 S{>40}` → **Heating** — sets the `heater_generic chamber_heater` target

Because the heater target reads 0 while maintaining, HelixScreen synthesizes a canonical display target and mode rather than displaying the raw heater target. See [MULTI_EXTRUDER_TEMPERATURE.md § Chamber Heating (M141)](../MULTI_EXTRUDER_TEMPERATURE.md#chamber-heating-m141) for the subject/binding details.

### GCode Commands (from `box_wrapper.so` decompilation)

#### Filament Operations

| Command                                 | Description                         |
| --------------------------------------- | ----------------------------------- |
| `BOX_EXTRUDE_MATERIAL TNN=T1A`          | Load filament from specified slot   |
| `BOX_RETRUDE_MATERIAL`                  | Unload current filament back to CFS |
| `BOX_RETRUDE_MATERIAL_WITH_TNN TNN=T1A` | Unload specific slot                |
| `BOX_EXTRUDER_EXTRUDE TNN=T1A`          | Feed filament to extruder           |
| `BOX_MATERIAL_FLUSH`                    | Purge/flush filament                |
| `BOX_MATERIAL_CHANGE_FLUSH`             | Color-change flush sequence         |
| `BOX_EXTRUSION_ALL_MATERIALS`           | Prime all materials                 |

#### Tool Change (M8200 macro)

| Subcommand     | Description                            |
| -------------- | -------------------------------------- |
| `M8200 P`      | Pre-operation (prepare for change)     |
| `M8200 C`      | Cut filament                           |
| `M8200 R`      | Retract to CFS box                     |
| `M8200 L I{n}` | Load slot n (0-15, auto-mapped to TnX) |
| `M8200 W`      | Waste detection                        |
| `M8200 F`      | Flush/purge                            |
| `M8200 O`      | End operation                          |

#### Query/Status

| Command                               | Description                                                          |
| ------------------------------------- | -------------------------------------------------------------------- |
| `BOX_GET_BOX_STATE`                   | Query overall CFS state                                              |
| `BOX_GET_RFID ADDR={n} NUM={n}`       | Read RFID data for specific slot                                     |
| `BOX_GET_REMAIN_LEN ADDR={n} NUM={n}` | Query remaining filament length                                      |
| `BOX_GET_FILAMENT_SENSOR_STATE`       | Query filament sensor states                                         |
| `BOX_GET_HARDWARE_STATUS`             | Full hardware status (RFID cards, humidity, eeprom, measuring wheel) |
| `BOX_GET_BUFFER_STATE`                | Buffer tube state                                                    |
| `BOX_GET_VERSION_SN`                  | Firmware version and serial number                                   |

#### Control

| Command                                           | Description                                 |
| ------------------------------------------------- | ------------------------------------------- |
| `BOX_ENABLE_CFS_PRINT ENABLE={0\|1}`              | Enable/disable CFS for printing             |
| `BOX_ENABLE_AUTO_REFILL`                          | Toggle auto-refill (backup spool switching) |
| `BOX_SET_BOX_MODE`                                | Set CFS operating mode                      |
| `BOX_SET_TEMP`                                    | Set extrusion temperature                   |
| `BOX_SET_PRE_LOADING ADDR={n} NUM={n} ACTION=RUN` | Pre-load filament                           |
| `BOX_START_PRINT`                                 | Signal print start to CFS                   |
| `BOX_END_PRINT`                                   | Signal print end to CFS                     |
| `BOX_ERROR_CLEAR`                                 | Clear CFS error state                       |
| `BOX_ERROR_RESUME_PROCESS`                        | Resume after error                          |

#### Physical Operations

| Command                         | Description                   |
| ------------------------------- | ----------------------------- |
| `BOX_GO_TO_EXTRUDE_POS` (M1500) | Move to purge station         |
| `BOX_MOVE_TO_SAFE_POS` (M1499)  | Park at safe position         |
| `BOX_NOZZLE_CLEAN` (M1501)      | Wipe nozzle on silicone strip |
| `BOX_CUT_MATERIAL` (M1502)      | Activate filament cutter      |
| `BOX_MOVE_TO_CUT`               | Move to cut position          |

#### M8200 — Slicer-Facing CFS Interface

**Use M8200 for manual load/unload operations.** Creality's `BOX_LOAD_MATERIAL` macro is buggy — it omits `CR_BOX_PRE_OPT` which is required before `CR_BOX_EXTRUDE`, causing `key60: Internal error` shutdowns.

| Command            | Effect                                   | Underlying               |
| ------------------ | ---------------------------------------- | ------------------------ |
| `M8200 P`          | Prepare CFS for material change          | `CR_BOX_PRE_OPT`         |
| `M8200 L I={slot}` | Load filament from slot (0-indexed)      | `CR_BOX_EXTRUDE TNN=...` |
| `M8200 C`          | Cut filament                             | `CR_BOX_CUT`             |
| `M8200 R`          | Retract filament (optional `E={length}`) | `CR_BOX_RETRUDE`         |
| `M8200 W`          | Waste purge                              | `CR_BOX_WASTE`           |
| `M8200 F`          | Flush (uses last TNN from L command)     | `CR_BOX_FLUSH`           |
| `M8200 O`          | End material change operation            | `CR_BOX_END_OPT`         |

**Load sequence:** `M8200 P` → `M8200 L I=2` → `M8200 F` → `M8200 O`
**Unload sequence:** `M8200 P` → `M8200 C` → `M8200 R` → `M8200 O`

**Prerequisites:** Printer must be homed (`G28`). CFS does not require nozzle heating for feed/retract — heating is only needed for purging at the nozzle.

**Stock UI note:** Creality's display-server communicates with the CFS **directly over RS-485** (`/dev/ttyS5` at 230400 baud), bypassing Klipper entirely for load/unload. The GCode macros are primarily for automated print-time use.

#### Macro Sequences (from box.cfg — DO NOT use for manual load/unload)

| Macro                       | Sequence                                      | Notes                                         |
| --------------------------- | --------------------------------------------- | --------------------------------------------- |
| `BOX_LOAD_MATERIAL TNN=T1A` | Heat → Cut → Retract → Extrude → Flush → Park | **BUG: Missing CR_BOX_PRE_OPT → key60 crash** |
| `BOX_QUIT_MATERIAL`         | Heat → Cut → Retract → Park                   | Same issue                                    |
| `BOX_INFO_REFRESH`          | Pre-load → Get RFID → Get Remain Len          | Safe to use, [usage](#refresh-spool-info)     |

#### Refresh Spool Info

```gcode
; ADDR is the CFS addr (minimal 1, maximal 4)
; NUM is the operation bitflag (see below)
BOX_INFO_REFRESH ADDR={} NUM={}
```

| Spool | Flag     |
| ----- | -------- |
| A     | `0b0001` |
| B     | `0b0010` |
| C     | `0b0100` |
| D     | `0b1000` |

### Error Codes

CFS errors are reported as JSON with `key8xx` codes:

| Code              | Error                                           |
| ----------------- | ----------------------------------------------- |
| `key831`          | RS-485 communication timeout                    |
| `key834`          | Parameter error                                 |
| `key835`-`key838` | Extrusion blockages (connections, sensor, gear) |
| `key840`          | Box state error                                 |
| `key841`          | Cut sensor not detected                         |
| `key843`          | RFID read error                                 |
| `key844`          | Pneumatic joint abnormal                        |
| `key845`          | Nozzle blocked                                  |
| `key847`          | Empty printing, material enwind                 |
| `key848`          | Material break at connections                   |
| `key849`-`key851` | Retraction errors                               |
| `key853`          | Humidity sensor error                           |
| `key855`          | Cut position error                              |
| `key856`          | No cutter detected                              |
| `key857`          | Motor load error                                |
| `key858`          | EEPROM error                                    |
| `key859`          | Measuring wheel error                           |
| `key860`          | Buffer error                                    |
| `key861`          | Left RFID card error                            |
| `key862`          | Right RFID card error                           |
| `key863`-`key865` | Retraction/extrusion completion errors          |

### Internal Classes (from Cython decompilation)

| Class                          | Purpose                                                             |
| ------------------------------ | ------------------------------------------------------------------- |
| `MultiColorMeterialBoxWrapper` | Main Klipper module — GCode registration, `get_status()`, lifecycle |
| `BoxState`                     | Tnn_map, Tnn_content, connection tracking, slot state               |
| `BoxAction`                    | RS-485 commands, RFID reads, motor control, sensor queries          |
| `BoxSave`                      | Persistence: resume_tnn, error state, save/restore across restarts  |
| `BoxCfg`                       | Configuration from box.cfg (positions, velocities, temps)           |
| `ParseData`                    | Binary protocol parser: RFID, remain_len, measuring_wheel, CRC8     |
| `CutSensor`                    | Cutter hall sensor monitoring                                       |

### HelixScreen Integration Plan

The CFS exposes state through the standard Moonraker object query interface, similar to AFC and Happy Hare. Integration approach:

1. **Add CFS as a filament backend in `AmsState`** — query `box` object, map T1-T4 units with A-D slots
2. **Map data fields**:
   - `color_value` → slot color (strip leading `0`, parse as hex)
   - `material_type` → lookup in material database (strip leading `1`, match ID)
   - `remain_len` → remaining filament display
   - `temperature` / `dry_and_humidity` → per-unit environmental monitoring
   - `state` → connection status
3. **Commands**: Use `M8200` for load/unload (NOT `BOX_LOAD_MATERIAL` — see M8200 section above)
4. **Auto-detection**: Add `box` to Moonraker object heuristics in `printer_database.json`

### Community Resources

- **[ityshchenko/klipper-cfs](https://github.com/ityshchenko/klipper-cfs)** — Community open-source CFS Klipper module (early stage, protocol documentation)
- **[CrealityOfficial/K2_Series_Klipper](https://github.com/CrealityOfficial/K2_Series_Klipper)** — Creality's official (incomplete) Klipper fork with binary blobs

## Auto-Detection

HelixScreen auto-detects K2 printers using heuristics from `config/printer_database.json`:

| Heuristic              | Confidence | Description                       |
| ---------------------- | ---------- | --------------------------------- |
| Hostname `k2`          | 85         | Hostname contains "k2"            |
| `box` object           | 90         | CFS module present (K2-specific)  |
| `motor_control` object | 75         | K2-specific motor control module  |
| `fan_feedback` object  | 70         | K2-specific fan RPM feedback      |
| `load_ai` object       | 65         | AI print monitoring (K2-specific) |
| `chamber_temp` sensor  | 70         | Chamber temperature sensor        |
| Hostname `creality`    | 60         | Hostname contains "creality"      |
| CoreXY kinematics      | 40         | CoreXY motion system              |

## Known Limitations

### Display

- **480x1600 portrait framebuffer (K2 Max)** — needs software rotation to landscape. Other K2 models may be 480x800.

### CFS

- **Closed-source protocol** — CFS communication relies on `box_wrapper.cpython-39.so` binary blob. Protocol has been reverse-engineered from strings but full reimplementation is not yet available.
- **Material database is cloud-fetched** — The material database at `/mnt/UDISK/creality/userdata/box/material_database.json` is downloaded from Creality's cloud. HelixScreen should include a fallback mapping for common material type codes.

### Platform

- **Low CPU** — Dual Cortex-A7 at ~57 BogoMIPS. Performance-sensitive features (bed mesh 3D, animations) may need throttling.
- **No curl** — BusyBox wget only, no HTTPS support.
- **WiFi managed by platform hooks** — The stock `wifi-server` is killed when HelixScreen takes over the display. Platform hooks (`hooks-k2.sh`) start `wpa_supplicant` directly using credentials at `/etc/wifi/wpa_supplicant/wpa_supplicant.conf`. WiFi configuration changes made via the stock UI are preserved.
- **Non-standard control socket** — `hooks-k2.sh` launches `wpa_supplicant` without `-O`, so the control socket lands at the `ctrl_interface=` from the stock conf — `/etc/wifi/wpa_supplicant/sockets/wlan0` on K2 — not the usual `/run/wpa_supplicant`. The WiFi backend searches that location (and auto-detects any `-O` path from the live process), so network discovery works without manual symlinks. Firmware that uses yet another path can be pointed at it via `HELIX_WPA_SOCKET_DIR`. Surfaced by a community **K2 Plus** report.

## Related Resources

- **[CrealityOfficial/K2_Series_Klipper](https://github.com/CrealityOfficial/K2_Series_Klipper)** — Creality's official (incomplete) Klipper fork
- **[Guilouz/Creality-K2Plus-Extracted-Firmwares](https://github.com/Guilouz/Creality-K2Plus-Extracted-Firmwares)** — Extracted stock firmware images
- **[ityshchenko/klipper-cfs](https://github.com/ityshchenko/klipper-cfs)** — Community open-source CFS module
- **[prestonbrown/helixscreen/pull/1077](https://github.com/prestonbrown/helixscreen/pull/1077#issuecomment-4864952310)** — Usage of `BOX_INFO_REFRESH` macro
- **[K2 Plus Research](printer-research/CREALITY_K2_PLUS_RESEARCH.md)** — Detailed hardware and software research
- **[K1 vs K2 Community Comparison](printer-research/CREALITY_K1_VS_K2_COMMUNITY.md)** — Analysis of community ecosystem differences
- **[Creality Wiki](https://wiki.creality.com/en/k2-flagship-series/k2-plus)** — Official K2 Plus documentation
- **[Creality Forum](https://forum.creality.com/c/flagship-series/creality-flagship-k2-plus/81)** — Official K2 Plus community forum
