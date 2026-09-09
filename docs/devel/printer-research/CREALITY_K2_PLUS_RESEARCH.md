# Creality K2 Plus (and K2 Series) Research

**Date**: 2026-02-02 (hardware verified on a K2 Plus 2026-09-08)
**Status**: Hardware confirmed, build target shipping

## Executive Summary

The Creality K2 Plus is Creality's flagship CoreXY enclosed printer with 350mm³ build volume. It runs **Creality OS** (modified Klipper) and supports multi-material via **CFS (Creality Filament System)** - up to 16 colors with 4 daisy-chained units. **Moonraker is included in stock firmware**, on port 7125 directly and 4408 behind nginx. The K2 Plus runs an **Allwinner T113 (`sun8iw20p1`), dual-core ARM Cortex-A7** - not the Ingenic MIPS part used by the K1 series.

---

## 1. Hardware Specifications

### K2 Plus

| Specification | Details |
|---------------|---------|
| **Build Volume** | 350 x 350 x 350 mm |
| **Max Print Speed** | 600 mm/s |
| **Max Acceleration** | 30,000 mm/s² |
| **Nozzle Temperature** | Up to 350°C |
| **Bed Temperature** | Up to 120°C |
| **Chamber Temperature** | Up to 60°C (actively heated) |
| **Display** | 4.3" LCD touchscreen, 480 x 800 |
| **Storage** | 32 GB onboard |
| **Connectivity** | Dual-band WiFi, Ethernet |
| **Motion** | Step-servo motors, 32,768 microsteps/rev |
| **Weight** | 35 kg |
| **Price** | $1,499 (Combo with CFS) |

### Processor
**CONFIRMED (on device 2026-03-23, K2 Plus hostname K2Plus-50C1)**: The K2 uses a DIFFERENT SoC than the K1 series.
- **Linux SoC**: Allwinner T113 (sun8iw20p1), dual-core ARM Cortex-A7, armv7l (32-bit only) (NOT Ingenic MIPS like K1)
- **Evidence**: Tina Linux (Allwinner's distro), entware armv7sf installer; `/proc/cpuinfo` reports sun8iw20p1 / Cortex-A7. linux-sunxi.org lists sun8iw20 = T113.
- **RAM**: ~488 MB total (confirmed on device)
- **Storage**: 32 GB
- **Motion MCU**: GD32F303RET6 (ARM Cortex-M3) - same as K1

### K2 Series Variants

| Model | Build Volume | Chamber Heater | Price |
|-------|-------------|----------------|-------|
| **K2** | 260³ mm | No | $549-699 |
| **K2 Pro** | 300³ mm | Yes (60°C) | $849-1,049 |
| **K2 Plus** | 350³ mm | Yes (60°C) | $1,199-1,499 |
| **K2 SE** | 220x215x245 mm | No | Lower cost |

---

## 2. Stock Firmware

### Operating System
- **Tina Linux 21.02-SNAPSHOT** (OpenWrt-based; `DISTRIB_ID='OpenWrt'`)
- **Kernel**: Linux 5.4.61 armv7l
- **Klipper**: Custom fork with proprietary extensions
- **Python**: 3.9

### Firmware Distribution
- Updates as `.img` files
- CFS firmware as `.bin` files
- OTA via Creality Cloud
- Extracted at [Guilouz/Creality-K2Plus-Extracted-Firmwares](https://github.com/Guilouz/Creality-K2Plus-Extracted-Firmwares)

---

## 3. Multi-Material System (CFS)

The **Creality Filament System** is Creality's answer to Bambu's AMS.

### CFS Specifications
| Feature | Value |
|---------|-------|
| **Spool Capacity** | 4 per unit |
| **Max Units** | 4 (daisy-chained) |
| **Max Colors** | 16 |
| **Communication** | RS-485 protocol |
| **Features** | Humidity/temp monitoring, RFID, auto-backup |

### CFS Communication
- RS-485 serial via dedicated cables
- Proprietary Python wrappers (compiled `.so` blobs):
  - `filament_rack_wrapper.cpython-39.so`
  - `serial_485_wrapper.cpython-39.so`
  - `box_wrapper.cpython-39.so`

### CFS G-code Macros
```
BOX_GO_TO_EXTRUDE_POS
BOX_NOZZLE_CLEAN
BOX_MOVE_TO_SAFE_POS
BOX_CUT_MATERIAL
T0, T1, T2, T3 (tool change)
```

---

## 4. Moonraker Availability

### Stock: YES (unlike K1 series)

| Service | Port |
|---------|------|
| Fluidd | 4408 |
| Mainsail | 4409 |
| Moonraker API | 4408 |

### Limitations
- Some features restricted in stock
- Full functionality after rooting
- CFS mapping unavailable via native OctoPrint/Klipper file transfer

---

## 5. Custom Firmware Options

### Official Open Source
**Repository**: [CrealityOfficial/K2_Series_Klipper](https://github.com/CrealityOfficial/K2_Series_Klipper)

Community criticism:
- No build documentation
- Outdated Tina Linux base (5 years old)
- CFS modules only as compiled blobs
- No integration instructions

### Community Projects

| Project | Status | Description |
|---------|--------|-------------|
| [k2-improvements](https://github.com/jamincollins/k2-improvements) | **ARCHIVED Aug 2025** | Full Klipper venv, Entware, Cartographer (148 stars, 42 forks) |
| [K2Plus-entware](https://github.com/vsevolod-volkov/K2Plus-entware) | Active? | Basic entware installer |
| [Fluidd-K2](https://github.com/BusPirateV5/Fluidd-K2) | Unknown | Customized Fluidd, WebRTC camera |
| [Mainsail-K2](https://github.com/Guilouz/Mainsail-K2) | Unknown | Lightweight Mainsail build |
| [k2_powerups](https://github.com/minimal3dp/k2_powerups) | Unknown | Improved leveling/start procedures |

**Note**: Guilouz Helper Script will NOT support K2 Plus - developer returned the printer (May 2025). GuppyScreen also does not support K2. See [K1 vs K2 Community Comparison](CREALITY_K1_VS_K2_COMMUNITY.md) for full analysis.

---

## 6. Klipper Configuration Structure

### Location
`/usr/share/klipper/config/`:
- `printer.cfg` - Main config
- `gcode_macro.cfg` - G-code macros

### Key Variables
```ini
z_safe_g28: 0.0
max_x_position: 350
max_y_position: 352
max_z_position: 320
```

### Notable Features
- `Qmode` - Quiet mode (2500mm/s² accel, 150mm/s velocity)
- Chamber heater control (`M141`, `M191`)
- Input shaper calibration
- `BOX_*` macros for CFS

### Closed-Source Components
- `box_wrapper.cpython-39.so`
- `filament_rack_wrapper.cpython-39.so`
- `serial_485_wrapper.cpython-39.so`
- Master server/display application

---

## 7. Display Interface

### Hardware
- **Size**: 4.3 inches
- **Resolution**: 480 x 800 pixels
- **Type**: LCD touchscreen
- **Interface**: `/dev/fb0`

- **Touch**: Goodix `gt9xxnew_ts`, capacitive, `/dev/input/event0`

Full measured geometry, rotation and DPI are in [Section 12](#12-display-details).

### Software
- Stock UI is `/usr/bin/display-server`, LVGL-based like the K1's
- Direct framebuffer rendering, no DRM/KMS
- No X11/Wayland

---

## 8. Root Access

### Enabling Root
1. Settings > "Root account information"
2. Read disclaimer, check acknowledgment
3. Wait 30 seconds, press "Ok"
4. SSH credentials displayed

### Credentials
- **Username**: `root`
- **Password**: `creality_2024`

```bash
ssh root@<printer-ip>
```

---

## 9. HelixScreen Compatibility Assessment

### Favorable Factors
1. **Moonraker included** - stock firmware, port 7125 direct and 4408 behind nginx
2. **Root access available** - SSH with default credentials
3. **Linux framebuffer** - direct `/dev/fb0` access, released cleanly when `display-server` stops
4. **LVGL precedent** - the stock UI is LVGL too

### Challenges

| Challenge | Status |
|-----------|--------|
| 480x800 portrait panel | Resolved - rotated 270 degrees in software, shipped in the `k2` preset |
| Dual-core Cortex-A7 headroom | Live constraint - animations are disabled in the preset, and `hooks-k2.sh` allows 120s for Moonraker to answer |
| CFS integration | Partial - status and control work; the protocol is reverse-engineered from `box_wrapper.cpython-39.so` rather than reimplemented |
| Boot animation overlay | Resolved - `boot-play` composites a video layer above the framebuffer, so the hooks kill it explicitly |
| Declining community | Unchanged - key maintainers (Guilouz, jamincollins) left, but stock Moonraker means we depend on them less |

### Implementation Path

Steps 1 to 5 are done: the SoC is confirmed T113, the armv7 musl toolchain is in
`docker/Dockerfile.k2`, fbdev and evdev drive the panel and touch, Moonraker is wired on
7125, and CFS runs through the `box` object and `M8200`.

Remaining work is variant breadth (K2, K2 Pro, K2 SE) and a native CFS implementation - see
[Section 14](#14-helixscreen-build-target).

---

## 10. Community Resources

### Official
- **Forum**: [forum.creality.com/c/flagship-series/creality-flagship-k2-plus/81](https://forum.creality.com/c/flagship-series/creality-flagship-k2-plus/81)
- **Wiki**: [wiki.creality.com/en/k2-flagship-series/k2-plus](https://wiki.creality.com/en/k2-flagship-series/k2-plus)

### Discord
- [discord.com/invite/creality](https://discord.com/invite/creality)

### GitHub
| Repository | Purpose |
|------------|---------|
| [CrealityOfficial/K2_Series_Klipper](https://github.com/CrealityOfficial/K2_Series_Klipper) | Official Klipper fork |
| [Guilouz/Creality-K2Plus-Extracted-Firmwares](https://github.com/Guilouz/Creality-K2Plus-Extracted-Firmwares) | Extracted firmware |
| [ballaswag/guppyscreen](https://github.com/ballaswag/guppyscreen) | GuppyScreen (K2 not yet supported) |

---

## 11. Known Filesystem Paths

| What | Path |
|------|------|
| Klipper | `/usr/share/klipper/` |
| Klipper config | `/mnt/UDISK/printer_data/config/printer.cfg` |
| G-code macros | `/usr/share/klipper/config/gcode_macro.cfg` |
| Moonraker | Stock, port **7125** direct / **4408** via nginx |
| Stock UI | `/usr/bin/display-server` |
| Init system | **procd** (OpenWrt-style, NOT SysV or systemd) |
| Service startup | `/etc/init.d/app` (starts display-server, master-server, app-server, etc.) |
| SSH credentials | `root` / `creality_2024` |

---

## 12. Display Details

Verified on a K2 Plus (2026-09-08, read-only SSH), board `CR0CN240110C10`, firmware 1.1.4.11.

| Attribute | Value |
|-----------|-------|
| Size | 4.3 inches |
| Panel resolution | 480 x 800 native portrait |
| Panel controller | `lcm_id=gc9503cv_ue_480_800` in `/proc/cmdline`. Varies by variant - a K2 Pro reports `st7701_9bit_mipi_tjc_480_800` at identical geometry |
| Framebuffer mode | `U:480x800p-58` (`/sys/class/graphics/fb0/modes`) |
| Virtual framebuffer | `480,1600` - two stacked 480x800 buffers for page flipping, not a taller panel |
| Framebuffer depth / stride | 32 bpp, 1920 bytes (480 x 4) |
| Presented orientation | Landscape, by software rotation 270 degrees |
| DPI | 218.2, from `src/application/display_metrics.cpp#kKnownPanels` (`{"k2", {480, 800, 108.6}}`) |
| Touch controller | Goodix `gt9xxnew_ts`, I2C (`Bus=0018`), sole node `/dev/input/event0` |
| Touch modules on disk | `gt9xxnew_ts.ko`, `tlsc6x.ko` - two variants for different hardware revisions |
| Framebuffer device | `/dev/fb0`; no DRM (`/dev/dri` absent) |
| Backlight | No `/sys/class/backlight` class; `platform_enable_backlight()` in `hooks-k2.sh` is a no-op |
| G2D accelerator | `g2d_sunxi` loaded at boot (Allwinner hardware 2D, supports rotation) |

Rotation is shipped, not probed. `assets/config/presets/k2.json` sets `display.rotate: 270`
with `rotation_probed: true`, so the interactive orientation probe in
`src/application/application.cpp#run_rotation_probe_and_layout` never runs on a K2. The same
preset sets `animations_enabled: false` for the dual-core A7. Override at runtime with
`HELIX_DISPLAY_ROTATION` or `--rotate`.

The touch device name contains no "touch" substring, so
`grep -i touch /proc/bus/input/devices` returns nothing on a perfectly healthy K2. Match on
the handler node or the `gt9`/`goodix` prefix instead. Both of those are in the scoring list
in `include/touch_calibration.h#is_known_touchscreen_name`; **`tlsc` is not**, so a board
carrying the `tlsc6x` variant scores lower and leans on its `INPUT_PROP_DIRECT` and ABS
capability bits to be selected. No K2 with that variant has been observed yet.

---

## 13. Hardware Profile (answered) and Bring-Up Probe for Other K2 Variants

The questions this section used to pose are answered for the K2 Plus. The commands are kept
because they are what a contributor with a different K2 variant should run and report.

| # | Probe | K2 Plus answer |
|---|-------|----------------|
| 1 | `cat /sys/class/graphics/fb0/virtual_size` | `480,1600` (portrait panel, double-buffered) |
| 2 | `uname -m` | `armv7l` |
| 3 | `tr -d '\0' < /proc/device-tree/compatible` | `allwinner,t113_iarm,sun8iw20p1` |
| 4 | `ls /lib/ld-*.so* /lib/libc.so.6` | glibc 2.29, `ld-linux-armhf.so.3`; libstdc++ 6.0.25 |
| 5 | `df -h` | 27.5 GB writable on `/mnt/UDISK` |
| 6 | stop `display-server`, write to `/dev/fb0` | Releases the framebuffer cleanly |
| 7 | `cat /proc/bus/input/devices` | `gt9xxnew_ts` on `event0`, the only input node |
| 8 | Moonraker reachable | Yes, 7125 direct / 4408 via nginx |

Full probe to hand to someone with a different K2. Note the K2 has **no curl** (BusyBox wget,
no HTTPS, and recent builds drop wget too), so HTTP goes through `python3 urllib`:

```bash
# Identity - the board string is what distinguishes variants
tr -d '\0' < /proc/device-tree/compatible; echo
cat /etc/openwrt_release            # DISTRIB_TARGET carries the board ID
hostname; uname -a

# Display - panel geometry, and whether the driver already rotated it
for f in modes virtual_size bits_per_pixel stride rotate; do
  printf '%-16s ' "$f"; cat /sys/class/graphics/fb0/$f
done
cat /proc/cmdline                   # lcm_id= panel string, panel_orientation= if present
ls /dev/fb* /dev/dri 2>&1           # DRM present, or fbdev only?

# Touch - do NOT grep for "touch", the Goodix node is not named that
cat /proc/bus/input/devices
cat /sys/class/input/event0/device/properties     # 0x1 = INPUT_PROP_DIRECT
cat /sys/class/input/event0/device/capabilities/abs

# Toolchain fit
ls -l /lib/ld-*.so* /lib/libc.so.6 /lib/libstdc++.so.6*

# Resources, layout, and which process owns the screen
free -m; df -h; ls /mnt/UDISK
ps w | grep -vE '\[.*\]'
ls /etc/init.d/

# Klipper / Moonraker shape - this is what the printer database entry keys on
cat /mnt/UDISK/printer_data/config/printer.cfg
python3 -c "import urllib.request as u;print(u.urlopen('http://127.0.0.1:7125/printer/objects/list').read().decode()[:2000])"
```

### K2 Pro

Reported by a community member (2026-09-08), collected on their machine rather than ours:

| Fact | K2 Pro | K2 Plus |
|------|--------|---------|
| Device tree | `allwinner,t113_iarm,sun8iw20p1` | `allwinner,t113_iarm,sun8iw20p1` |
| `uname -m` | `armv7l` | `armv7l` |
| CPU | 2 x Cortex-A7 rev 5, 57.14 BogoMIPS | 2 x Cortex-A7 rev 5, 57.14 BogoMIPS |
| `DISTRIB_ARCH` | `arm_cortex-a7_neon` | `arm_cortex-a7_neon` |
| `DISTRIB_TAINTS` | `no-all glibc busybox` | `no-all glibc busybox` |
| libc | glibc 2.29, `ld-linux-armhf.so.3` | glibc 2.29, `ld-linux-armhf.so.3` |
| Tina / OpenWrt | `TINA_VERSION=5.0`, OpenWrt 21.02-SNAPSHOT | `TINA_VERSION=5.0`, OpenWrt 21.02-SNAPSHOT |
| Kernel | 5.4.61 `#1`, built 2026-04-23 | 5.4.61 `#56`, built 2025-12-17 |
| **Board** | `t113_i-`**`CR0CN200400C10`**`/generic` | `t113_i-`**`CR0CN240110C10`**`/generic` |
| Firmware build | `tina.112052.20260506.071020` | `tina.wuhui.20251217.103029` |
| Hostname | `K2Pro` | `K2Plus-50C1` |
| `fb0/modes` | `U:480x800p-58` | `U:480x800p-58` |
| `fb0/virtual_size` | `480,1600` | `480,1600` |
| `fb0` depth / stride / rotate | 32 bpp, 1920, 0 | 32 bpp, 1920, 0 |
| DRM | absent (`/dev/dri`) | absent (`/dev/dri`) |
| **Panel controller** | **`st7701_9bit_mipi_tjc_480_800`** | **`gc9503cv_ue_480_800`** |
| DRAM | `androidboot.dramsize=512` | 488 MB usable |
| Touch device | `gt9xxnew_ts`, `Bus=0018`, `dead:beef:28bb` | `gt9xxnew_ts`, `Bus=0018`, `dead:beef:28bb` |
| Touch `properties` / `abs` | `2` / `2650000 0` | `2` / `2650000 0` |
| Input nodes | `event0` only | `event0` only |
| libc / libstdc++ | glibc 2.29 / 6.0.25 | glibc 2.29 / 6.0.25 |
| RAM | 488 MB, no swap | 488 MB, no swap |
| `/mnt/UDISK` | 27.5 GB | 27.5 GB |
| Overlay (`/dev/mmcblk0p10`) | 240 MB | 240 MB |

The board ID in `DISTRIB_TARGET` is the field that actually separates the two, and it is a
better variant discriminator than the hostname: everything else in the identity block is
byte-identical apart from build stamps.

The build-stamp gap is **not** established as a model difference. Our K2 Plus reference unit
runs firmware **1.1.4.11** and has never been updated, so the roughly five months between the
two images is at least partly our own version skew. Nothing here separates a Pro from a Plus
on the strength of a date. Settling it needs a Plus on current firmware, and the macro set is
where any real difference would show, not the hardware.

Reading the firmware version on a K2 (there is no `/etc/ota_info`, unlike the K1 family):

```bash
sh /etc/ota_bin/get_ota_board_name.sh        # CR0CN240110C10 on a K2 Plus
sh /etc/ota_bin/get_ota_current_version.sh   # fw_printenv version, e.g. 1.1.4.11
```

The version lives in the U-Boot environment. The update *check* lived in Creality's daemon
stack, which HelixScreen stops, so an installed machine cannot tell you whether a newer image
exists.

**The panel controller differs and the framebuffer contract does not.** A K2 Pro drives a
Sitronix ST7701 where the Plus drives a GC9503CV, but both present 480x800 at 32 bpp with a
1920-byte stride, a double-height virtual buffer and no DRM. Everything HelixScreen touches is
on the framebuffer side of that line, so the `k2` target and its 270-degree rotation apply
unchanged. Anyone porting a *kernel* between the two variants does have to care, because the
panel driver and DTS are where the difference lives.

The touch IC *is* shared, even though the display IC is not. Both variants report the same
Goodix `gt9xxnew_ts` down to the placeholder `dead:beef:28bb` bus IDs and the same capability
bitmaps. `tlsc6x` ships in the rootfs as an alternate module but has not been seen on either
machine.

Both report `properties = 2`, which is `INPUT_PROP_DIRECT` (bit 1), and an `abs` bitmap of
`2650000 0` - `ABS_MT_TOUCH_MAJOR`, `ABS_MT_WIDTH_MAJOR`, `ABS_MT_POSITION_X/Y` and
`ABS_MT_TRACKING_ID` with **no legacy `ABS_X`/`ABS_Y`**. These are MT-only panels, which is the
case `src/api/input_device_scanner.cpp#get_input_touch_capabilities` exists to handle.

**HelixScreen runs on a K2 Pro.** The reporting machine has it installed and running from
`/opt/helixscreen` with `--rotate=270`, alongside our `ustreamer`, our `wpa_supplicant` and an
`S99helixscreen` init script, with the stock `display-server` stopped. The `k2` target needed
no modification. That is the K2 Pro's hardware validation.

The stock config set is the same on both variants: `printer.cfg`, `box.cfg`,
`gcode_macro.cfg`, `motor_control.cfg`, `printer_params.cfg` and `sensorless.cfg` under
`/mnt/UDISK/printer_data/config/`. The Pro we reached carries `box.cfg`, so that machine has a
CFS attached and the `box` object our database entry expects is present.

`printer.cfg` confirms the size difference and little else:

| Field | K2 Pro | K2 Plus |
|-------|--------|---------|
| `bed_mesh` `mesh_max` | `295,295` | `345,345` |
| `stepper_x` / `_y` / `_z` `position_max` | 302 / 332 / 303 | 352.5 / 400 / 360 |
| `max_accel` | 20000 | 30000 |
| kinematics, `max_velocity`, `probe_count`, `nozzle_diameter` | corexy, 800, 9x9, 0.4 | same |
| Stock includes | `sensorless`, `gcode_macro`, `printer_params`, `box`, `motor_control` | same |

Both `build_volume_range` heuristics in the database are therefore confirmed against real
hardware: the Plus's 340-360 band contains 345, the Pro's 290-310 contains 295. The heuristic
reads `bed_mesh`, so `mesh_max` is the field that decides it, not the stepper limits - and on
both machines the Y stepper travels well past the bed to reach the nozzle-clean position.

`START_PRINT`, `PRINT_PREPARED` and `PRINT_PREPARE_CLEAR` are all present on the Pro, which is
what the entry's `pre_print_options` drive.

Every object the `creality_k2_pro` entry scores on is present: `box`, `motor_control`,
`fan_feedback`, `load_ai`, `filament_rack`, `heater_generic chamber_heater`,
`temperature_sensor chamber_temp` and corexy kinematics, alongside `prtouch_v3`, `z_align`,
`belt_mdl mdlx`/`mdly` and `exclude_object`. Detection needs nothing added.

**`objects/list` only ever names `gcode_macro` objects, so a command implemented by a klipper
extra is absent from it whether or not it exists.** `BOX_NOZZLE_CLEAN` is missing from both a
K2 Plus and a K2 Pro for that reason, while still appearing in the gcode response stream as
`[GCODE]BOX_NOZZLE_CLEAN`. Use `/printer/gcode/help` to ask whether a command is registered;
`objects/list` cannot answer it.

`LOAD_AI_RUN` is genuinely absent - it appears in neither list on a K2 Plus - so the
`ai_detect` option's `requires_macro` gate hides a toggle whose command does not exist. That is
the gate working, not a defect, and it matches the AI stack being inert under HelixScreen
anyway.

Note that the one machine we have reached adds community packages (k2-KAMP and a `custom/`
folder), so its macro set does not establish what a stock K2 Pro ships, and its
`print_start_default_phases` cannot serve as a stock baseline.

Detection already covers the model. `creality_k2_pro` in `assets/config/printer_database.json`
carries a `k2pro` hostname heuristic at confidence 90 plus a 290-310 mm build-volume range.
A stock Pro reports its hostname as `K2Pro`, which matches that heuristic and misses the K2
Plus's `k2plus`, so the model resolves without further work. Its `print_start_default_phases` are copied
from the K2 Plus and have never been measured on a Pro, which is worth re-timing once one is
running.

---

## 14. HelixScreen Build Target

### Status: shipping, verified on K2 Plus hardware

`PLATFORM_TARGET=k2` builds a fully static binary with Bootlin's armv7-eabihf **musl**
toolchain. The K2 root filesystem is **glibc 2.29**; static linking is what makes the device
libc irrelevant.

| Component | Details |
|-----------|---------|
| `PLATFORM_TARGET=k2` in `mk/cross.mk` | armv7-a hard-float, neon-vfpv4, musl static, fbdev/evdev |
| `docker/Dockerfile.k2` | Bootlin `armv7-eabihf--musl--stable-2024.02-1`, static OpenSSL in the sysroot |
| Deploy targets | `deploy-k2`, `deploy-k2-fg`, `deploy-k2-bin`, `k2-test`, `k2-ssh` (tar over ssh; BusyBox has no rsync) |
| Release packaging | `release-k2`, `package-k2`; camera via `ustreamer-k2` |
| Update checker platform key | `HELIX_PLATFORM_K2` -> `"k2"` |
| Display | fbdev on `/dev/fb0`, 480x800 portrait, rotated 270 degrees to landscape |
| Touch input | evdev auto-detection, Goodix `gt9xxnew_ts` |
| Boot persistence | `/etc/init.d/S99helixscreen` plus `config/helixscreen-k2-procd-shim.sh` |
| Install root | `/opt/helixscreen` -> `/mnt/UDISK/helixscreen` |

`K2_HOST` is mandatory for every deploy verb: the K2 has no mDNS, so the hostname does not
resolve.

procd only runs an init script that carries both `#!/bin/sh /etc/rc.common` and a `DEPEND`
line. A bare SysV script is skipped silently and the device sits on the Creality logo, which
is why the shim exists.

### Build & Deploy

```bash
# Build
make k2-docker

# Deploy (SSH: root / creality_2024)
make deploy-k2 K2_HOST=192.168.1.100
make deploy-k2-fg K2_HOST=192.168.1.100   # foreground with debug output
make k2-ssh K2_HOST=192.168.1.100          # SSH into the printer
```

### Early assumptions, and how they resolved

| Assumption | Resolution |
|-----------|------------|
| Ingenic X2000E MIPS, as on the K1 | **Allwinner T113 / sun8iw20p1**, ARM Cortex-A7 dual-core |
| ARM variant aarch64 | **armv7l**, 32-bit userland. The armv7 toolchain is correct |
| musl userland | Device is **glibc 2.29**. Irrelevant: the binary is fully static |
| Framebuffer already landscape | **No.** 480x800 portrait; rotated 270 degrees in software |
| Deploy dir `/opt/helixscreen` | Correct, as a symlink to `/mnt/UDISK/helixscreen` |
| Stock UI is `display-server` | Correct; `hooks-k2.sh` also stops `Monitor`, `master-server`, `app-server`, `audio-server`, `wifi-server`, `upgrade-server` and the `boot-play` animation, and deliberately leaves `web-server` running |
| BusyBox, no rsync | Correct; deploy uses tar over ssh |

### Still open

| Component | Notes |
|-----------|-------|
| CFS reimplementation | Protocol reverse-engineered from `box_wrapper.cpython-39.so`; see the CFS reference in [CREALITY_K2_SUPPORT.md](../printers/CREALITY_K2_SUPPORT.md) |
| K2, K2 Pro, K2 SE validation | Database entries exist for K2 Plus and K2 Pro; only the K2 Plus has been run by us. A K2 SE is a K1-family MIPS board despite the name - see the routing trap in `src/printer/ams_backend_cfs.cpp` |

---

## Conclusion

The K2 Plus is architecturally easier to target than the K1 (ARM rather than MIPS) and ships
stock Moonraker, so no community firmware is required. The target is built, deployed and
running on real hardware.

What remains is breadth rather than depth: the other K2 variants share the SoC, panel and
touch controller by every signal collected so far, and need a contributor with the hardware to
run the probe in Section 13 and report the board string and `printer.cfg`.

See also: [K1 vs K2 Community Comparison](CREALITY_K1_VS_K2_COMMUNITY.md)
