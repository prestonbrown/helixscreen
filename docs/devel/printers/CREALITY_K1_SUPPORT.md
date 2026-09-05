# Creality K1 Series Support

HelixScreen has a cross-compilation target for the Creality K1 series of high-speed CoreXY printers. The K1 series runs Klipper on an Ingenic MIPS processor. Moonraker is available via community firmware mods (Simple AF, Guilouz Helper Script).

## Supported Models

All K1 models use the Ingenic X2000E MIPS32r2 dual-core processor running Creality OS (Buildroot-based).

| Model | Build Volume | Display | Notable Features | Status |
|-------|-------------|---------|------------------|--------|
| K1 | 220 x 220 x 250 mm | 4.3" 480x800 | Standard | Untested |
| K1C | 220 x 220 x 250 mm | 4.3" 480x800 | Tri-metal "Unicorn" nozzle | **Tested** |
| K1 Max | 300 x 300 x 300 mm | 4.3" 480x800 | AI camera, LiDAR, ethernet | **Tested** |
| K1 SE | 220 x 220 x 250 mm | 4.3" 480x800 | Budget model | Untested |

## Hardware (Confirmed on K1C and K1 Max — 2026-03)

| Spec | Value |
|------|-------|
| SoC | Ingenic X2000E, MIPS32r2 dual-core @ 1.2 GHz |
| Co-processor | XBurst 0 @ 240 MHz (security/real-time) |
| RAM | 256 MB LPDDR2 (System-in-Package) |
| Storage | 8 GB eMMC |
| Display | 480x800 portrait, 32bpp ARGB8888, fbdev (`/dev/fb0`) |
| Stock UI | `/usr/bin/display-server` (must be stopped to use framebuffer) |
| OS | Creality OS (Buildroot 2020.02.1), Linux 5.4.x |
| glibc | 2.29 (important for ABI compatibility) |
| Python | 3.8.2 |
| Init System | SysV init (NOT systemd, NOT procd) |
| SSH | `root` / `creality_2023` (enable via Settings menu) |
| Config path | `/usr/data/printer_data/config/` |
| Klipper path | `/usr/share/klipper/` or `/usr/data/klipper/` |
| Logs path | `/usr/data/printer_data/logs/` |
| Gcode path | `/usr/data/printer_data/gcodes/` |

### Multi-MCU Architecture

| MCU | Chip | Serial Port | Purpose |
|-----|------|-------------|---------|
| mcu | GD32F303RET6 | `/dev/ttyS7` @ 230400 | Main motion control, steppers, bed heater |
| nozzle_mcu | GD32F303CBT6 | `/dev/ttyS1` @ 230400 | Hotend, LED, accelerometer (input shaper) |
| leveling_mcu | GD32E230F8P6 | `/dev/ttyS9` @ 230400 | Pressure sensor (PRTouch v2) |

### K1C vs K1 Max Differences

| Feature | K1C | K1 Max |
|---------|-----|--------|
| Build Volume | 220 x 220 x 250 mm | 300 x 300 x 300 mm |
| Rated Power | 350W | 1000W |
| AI Camera | Standard | Included |
| AI LiDAR | No | Yes |
| Ethernet | No | Yes |
| Chamber Heater | No | Yes (`heater_generic chamber_heater`) |

### Notes

- **256 MB RAM** — Memory-constrained. Moonraker + Klipper + HelixScreen leaves ~80-100 MB free. Creality warns about excessive memory usage.
- **MIPS32r2** — Unusual architecture for 3D printers. Requires MIPS cross-compilation toolchain.
- **480x800 display** — Portrait framebuffer, same as K2 series. Requires software rotation to landscape (800x480). Uses the `standard` breakpoint.
- **No stock Moonraker** — Must install community firmware (Simple AF or Guilouz) for Moonraker access.
- **K1C 2025 hardware revision** — Some units removed the root access option from the touchscreen menu.
- **Stock "unload" is not an unload** — `QUIT_MATERIAL` (what the firmware offers) purges ~100mm of filament FORWARD and retracts only ~62mm with anti-clog wiggles: net ~38mm deeper into the hotend. Creality expects you to cut the filament at the inlet and let it clear the residue (same philosophy as the Adventurer 5M). Install the HelixScreen macro pack (Settings → Advanced → HelixScreen Macros → Install) and `HELIX_UNLOAD_FILAMENT` takes the unload slot: heat to the firmware's default extruder temp, renew the melt (E10), then pull E-110 fully out of the direct-drive gears in two stages. On a CFS-equipped machine it handles the toolhead side; the box retract is driven by the filament system.

## Firmware Prerequisites

The K1 stock firmware does **not** include Moonraker. Users must install one of the following community mods:

### Simple AF (Recommended)

**Repository**: [pellcorp/creality](https://github.com/pellcorp/creality)

- Includes Klipper, Moonraker, and GuppyScreen
- Install location: `/usr/data/pellcorp/`
- HelixScreen replaces GuppyScreen as the touch UI

### Guilouz Helper Script

**Repository**: [Guilouz/Creality-K1-and-K1-Max](https://github.com/Guilouz/Creality-K1-and-K1-Max)

- Menu-based installer for Moonraker, Fluidd/Mainsail, GuppyScreen
- Install location: `/usr/data/helper-script/`
- Modular: users choose which components to install

### Guppy Mod (Advanced)

**Repository**: [ballaswag/creality_k1_klipper_mod](https://github.com/ballaswag/creality_k1_klipper_mod)

- Replaces entire Buildroot with modern version (2024.02)
- Updates Python 3.8.2 → 3.11.8, uses mainline Klipper
- **Caveat**: Breaks PRTouch — requires alternative probe

### Root Access

Available via the touchscreen:

1. Settings → Root Account Information
2. Accept disclaimer
3. Wait 30 seconds, confirm

SSH credentials: `root` / `creality_2023`

## Cross-Compilation

The K1 target uses MIPS32r2 with fully static musl linking. There is also an advanced dynamic-linking variant.

### Build via Docker (Recommended)

```bash
# Static build — single self-contained binary (recommended)
make k1-docker

# Dynamic build — links against K1's glibc 2.29 (advanced)
make k1-dynamic-docker
```

### Build Directly (Requires Toolchain)

```bash
make PLATFORM_TARGET=k1 -j           # Static (musl)
make PLATFORM_TARGET=k1-dynamic -j   # Dynamic (glibc)
```

### Build Configuration

#### Static Build (PLATFORM_TARGET=k1) — Recommended

| Setting | Value |
|---------|-------|
| Toolchain | Bootlin mips32el-musl-stable-2024.02-1 |
| Architecture | MIPS32r2 (little-endian, hard-float) |
| Linking | Fully static (musl) |
| Optimization | `-Os` with LTO (size-optimized) |
| Display backend | fbdev (`/dev/fb0`) |
| SSL | Enabled (static OpenSSL) |
| Platform define | `HELIX_PLATFORM_MIPS` |
| Output | `build/mips/bin/helix-screen` (~8-10 MB stripped) |

#### Dynamic Build (PLATFORM_TARGET=k1-dynamic) — Advanced

| Setting | Value |
|---------|-------|
| Toolchain | Custom crosstool-NG with NaN2008+FP64 ABI |
| Linking | Dynamic (glibc 2.29, must match K1 firmware) |
| Critical flags | `-mnan=2008 -mfp64` (NaN2008 ABI compatibility) |
| Dynamic linker | `/lib/ld-linux-mipsn8.so.1` |
| Output | `build/k1-dynamic/bin/helix-screen` (~3-5 MB stripped) |

The static build is recommended for its simplicity — no ABI compatibility concerns.

### CI/Release Status

The K1 target is included in the GitHub Actions release pipeline. Release artifacts are built automatically:

```bash
# Manual packaging
make package-k1
```

## Installation

### Prerequisites

- A Creality K1, K1C, K1 Max, or K1 SE printer
- **Root access enabled** (see Root Access section above)
- **Moonraker installed** via Simple AF, Guilouz, or Guppy Mod
- SSH access: `ssh root@<printer-ip>` (password: `creality_2023`)

### Quick Install (End Users)

```bash
# SSH into the printer, then:
wget -O - https://dl.helixscreen.org/install.sh | sh
```

Or with a local package:

```bash
scp helixscreen-k1.zip install.sh root@<ip>:/usr/data/
ssh root@<ip>
sh /usr/data/install.sh --local /usr/data/helixscreen-k1.zip
```

### Developer Deploy Targets

```bash
# Full deploy (binary + assets + config + platform hooks + init script)
make deploy-k1 K1_HOST=192.168.x.x

# Deploy and run in foreground with debug logging
make deploy-k1-fg K1_HOST=192.168.x.x

# Deploy binary only (fast iteration during development)
make deploy-k1-bin K1_HOST=192.168.x.x

# SSH into the printer
make k1-ssh K1_HOST=192.168.x.x

# Full build + deploy + run cycle
make k1-test K1_HOST=192.168.x.x
```

Deploy directory: `/usr/data/helixscreen` (override with `K1_DEPLOY_DIR`).

**Note**: The K1 uses BusyBox, so deployment uses tar/ssh transfer instead of rsync.

### What Happens on Deploy

1. Stops any running HelixScreen processes
2. Transfers binaries, assets, XML layouts, and config via tar/ssh
3. Installs SysV init script at `/etc/init.d/S99helixscreen` for boot persistence
4. Platform hooks stop the stock Creality UI (`display-server`, `Monitor`, etc.)
5. Platform hooks disable `/etc/init.d/S99start_app` to prevent stock UI on reboot
6. Platform hooks ensure SSH (dropbear) survives the stock UI shutdown
7. Starts HelixScreen on the framebuffer

### Reverting to Stock UI

```bash
ssh root@<printer-ip>
killall helix-screen helix-splash helix-watchdog 2>/dev/null
chmod +x /etc/init.d/S99start_app   # Re-enable stock UI
/etc/init.d/S99start_app start      # Start stock UI now
```

This lasts until the next boot. `config/helixscreen.init` calls `platform_stop_competing_uis` on every start, which re-applies `chmod a-x`, and `S99helixscreen` sorts ahead of `S99start_app` so it runs first. To make the revert stick, either uninstall HelixScreen (`install.sh --uninstall`, which re-enables the services the installer recorded) or disable the HelixScreen service before re-enabling `S99start_app`.

## Platform Hooks

Platform hooks are in `config/platform/hooks-k1.sh`. Key behaviors:

### Stock UI Shutdown

The stock K1 UI is managed by `/etc/init.d/S99start_app`, which launches `display-server`, `Monitor`, `master-server`, and other processes. The hooks:

1. Stop `S99start_app` via its init script
2. `chmod a-x` the init script to prevent reboot respawn (reversible)
3. Kill all lingering stock processes

**This takes the vendor's network stack down with the UI.** `master-server`, `app-server` and `web-server` are on the kill list, and they are what Creality Print and the Creality Cloud app talk to; port 80 closes with them. Only `display-server`, `Monitor` and `boot_display` actually contend for the framebuffer, so the current list is broader than the framebuffer problem requires. `hooks-k2.sh` makes the opposite call for the same reason and deliberately spares `web-server`, with a comment saying so. Keeping the K1 backend alive is tracked in [#1468](https://github.com/prestonbrown/helixscreen/issues/1468); it is not simply a matter of shortening the list, because the `chmod a-x` is what stops the services across a reboot.

### SSH Safety

**Critical**: `S99start_app` also manages dropbear (SSH) on stock firmware. Disabling it would kill SSH on next reboot. The hooks detect this and:

1. Check if dropbear is running after disabling S99start_app
2. If not, start dropbear directly
3. Create `/etc/init.d/S50dropbear` for future boot persistence

### Cache Directory

Sets `HELIX_CACHE_DIR=/usr/data/helixscreen/cache` since `/usr/data/` is the writable partition.

## Display Backend

HelixScreen renders directly to `/dev/fb0`. The framebuffer is 480x800 portrait (virtual size 480x1600 for double-buffering). Software rotation to landscape yields an effective resolution of 800x480.

## Touch Input

HelixScreen uses evdev and auto-detects the capacitive touch controller. Running as root (default) avoids permission issues on `/dev/input/event*`.

## Auto-Detection

HelixScreen auto-detects K1 printers using heuristics from `config/printer_database.json`:

| Heuristic | Confidence | Description |
|-----------|------------|-------------|
| Hostname contains `k1` | 80 | Unique to K1 series |
| Hostname contains `creality` | 60 | Brand indicator |
| Chamber fan object | 75 | K1-exclusive feature |
| Chamber temp sensor | 70 | Enclosed printer signature |
| CoreXY kinematics | 40 | Common in several printers |
| Build volume ~220mm | 50 | K1/K1C standard |
| Build volume ~300mm | 70 | K1 Max |
| AI camera object | 60 | K1 Max distinctive |

### Print Start Profile

All K1 models use the `creality_k1` print start profile:

```json
{
  "macro_name": "START_PRINT",
  "pre_start_gcode": "PRINT_PREPARED"
}
```

## Klipper Configuration

### Stock Objects

```ini
# Motion
[printer] kinematics: corexy, max_velocity: 1000, max_accel: 20000
[stepper_x], [stepper_y], [stepper_z]
[tmc2209 stepper_x], [tmc2209 stepper_y], [tmc2209 stepper_z]

# Heating
[extruder], [heater_bed]
[heater_generic chamber_heater]  # K1 Max only

# Temperature Sensors
[temperature_sensor mcu_temp], [temperature_sensor chamber_temp]

# Fans
[fan], [fan_generic fan0], [fan_generic fan1], [fan_generic fan2]
[heater_fan hotend_fan]

# Probing & Calibration
[prtouch_v2]   # Pressure-based leveling
[bed_mesh]
[adxl345]
[input_shaper]
```

Moonraker port: 7125 (direct). No multi-extruder or toolchanger support — K1 series are single-extruder only.

### CFS (optional upgrade)

K1, K1C, and K1 Max can run Creality's **official CFS upgrade**. When installed, the upgrade firmware (≥ v2.3.5.33) publishes a `box` Klipper object and a non-prefixed `BOX_*` macro set — different from the K2 stock firmware's `CR_BOX_*` primitives. HelixScreen detects the K1 dialect via `PrinterDetector::is_creality_k1()` at backend construction and emits the K1 macros (`BOX_EXTRUDE_MATERIAL TNN=…`, `BOX_EXTRUDER_EXTRUDE TNN=…`, `BOX_MATERIAL_FLUSH`
(bare — it takes `LEN`/`VELOCITY`/`TEMP`, never `TNN`), `BOX_CUT_MATERIAL`, `BOX_RETRUDE_MATERIAL`, `BOX_NOZZLE_CLEAN`, `BOX_GO_TO_EXTRUDE_POS`, `BOX_MOVE_TO_SAFE_POS`). See [FILAMENT_BACKEND_CFS.md § CFS](../FILAMENT_BACKEND_CFS.md#cfs-creality-filament-system) for the full dialect table.

Community open-source K1 firmwares (Guilouz, Simple AF, Guppy Mod) do not bundle the CFS macros — the upgrade firmware ships separately from Creality. Without the upgrade installed, no `box` object is published and the CFS backend stays disabled.

For the firmware side — what each `BOX_*` command actually does, the tn_data.json
persistence contract, and the deferred-failure/resume traps that make a successful-looking
gcode sequence do nothing — see
[CREALITY_CFS_INTERNALS.md](../CREALITY_CFS_INTERNALS.md). **Read it before changing anything
the CFS backend emits on K1.**

## Known Limitations

### Memory
- **256 MB RAM** — Most constrained platform. Moonraker + Klipper + HelixScreen is tight. Monitor memory usage carefully.

### Architecture
- **MIPS32r2** — Unusual ISA. Cross-compilation requires MIPS toolchain. No NEON SIMD (ARM-specific). MIPS SIMD (MSA) available but unused.
- **NaN2008 ABI** — The K1's glibc uses NaN2008 encoding. Dynamic builds must use `-mnan=2008 -mfp64` or linking will fail at runtime.

### Display
- **480x800 portrait** — Same panel as K2 series. Requires software rotation to 800x480 landscape.

### Firmware
- **Moonraker required** — Stock firmware has no Moonraker. Users must install community firmware first.
- **K1C 2025 revision** — Some newer K1C units removed the root access option from the settings menu. Community workarounds exist but are not widely adopted.

## Related Resources

- **[pellcorp/creality](https://github.com/pellcorp/creality)** — Simple AF firmware (recommended for HelixScreen)
- **[Guilouz/Creality-K1-and-K1-Max](https://github.com/Guilouz/Creality-K1-and-K1-Max)** — Helper Script & Wiki
- **[ballaswag/creality_k1_klipper_mod](https://github.com/ballaswag/creality_k1_klipper_mod)** — Guppy Mod
- **[ballaswag/guppyscreen](https://github.com/ballaswag/guppyscreen)** — GuppyScreen (LVGL touch UI, proves MIPS fbdev works)
- **[ballaswag/k1-discovery](https://github.com/ballaswag/k1-discovery)** — Hardware reverse engineering docs
- **[CrealityOfficial/K1_Series_Klipper](https://github.com/CrealityOfficial/K1_Series_Klipper)** — Official Klipper source
- **[K1 Series Research](../printer-research/CREALITY_K1_SERIES_RESEARCH.md)** — Detailed hardware and software research
- **[K1 vs K2 Community Comparison](../printer-research/CREALITY_K1_VS_K2_COMMUNITY.md)** — Ecosystem differences
- **[User Setup Guide](../../user/guide/creality-k1c-setup.md)** — End-user installation walkthrough
