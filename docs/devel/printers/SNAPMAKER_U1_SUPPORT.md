<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Snapmaker U1 Support

HelixScreen supports the Snapmaker U1 toolchanger as an alternative touchscreen UI. The U1 runs Klipper with Moonraker on a Rockchip ARM64 SoC, and HelixScreen can replace the stock display interface when deployed via SSH.

## Hardware

| Spec | Value |
|------|-------|
| SoC | Rockchip RK3562 — quad Cortex-A53 @ 2GHz (aarch64) |
| GPU | Mali-G52 2EE (OpenGL ES 3.2) |
| RAM | 961MB |
| Display | 3.5" 480x320 32bpp capacitive touch, DRM/KMS (`/dev/dri/card0`, rockchipdrmfb) |
| Touch Controller | TLSC6x capacitive (`tlsc6x_touch` on `/dev/input/event0`) |
| Storage | 28GB eMMC (`/userdata` ext4 persistent, SquashFS rootfs read-only overlay) |
| Recovery | A/B firmware slots + Rockchip MaskRom (unbrickable) |
| Firmware | Klipper + Moonraker |
| OS | Debian Trixie (ARM64) |
| Drivers | TMC2240 steppers |
| Filament | 4-channel RFID reader (FM175xx), OpenSpool NTAG215/216 |
| Camera | MIPI CSI + USB (Rockchip MPP/VPU) |
| Toolheads | 4 independent heads (SnapSwap system) |
| Max Speed | 500mm/s |

### SnapSwap Toolchanger

The U1 is a 4-toolhead color printer. Each head has its own nozzle, extruder, heater, and filament sensor. Tool changes take approximately 5 seconds with no purging required.

The U1 does **not** use the standard [viesturz/klipper-toolchanger](https://github.com/viesturz/klipper-toolchanger) module. Instead, it uses native multi-extruder with custom Klipper extensions. Extruders are named `extruder`, `extruder1`, `extruder2`, `extruder3` with custom state fields (`park_pin`, `active_pin`, `activating_move`, `state`) and the fitted nozzle (`nozzle_diameter`, `nozzle_volume_type`, which the firmware stores outside `printer.cfg` and changes through the `printer.control.nozzle_properties` webhook). HelixScreen has a dedicated `AmsBackendSnapmaker` that tracks tool state, RFID filament data, and supports tool switching via `T0`–`T3` gcodes, and lets each head's nozzle be set from its Toolhead settings section ([FILAMENT_BACKEND_SNAPMAKER_U1.md](../FILAMENT_BACKEND_SNAPMAKER_U1.md#changing-a-heads-nozzle)).

## Firmware Requirements

HelixScreen needs exactly one thing from the firmware: **SSH access** (to deploy and to install its boot hook). Both stock and community firmware can provide it — HelixScreen does **not** require PAXX.

- **Stock Snapmaker firmware (1.2+)** ships the `dropbear` SSH server. It is disabled by default and gated behind the printer's root/developer-access mode: `/etc/init.d/S50dropbear` exits early unless `custom_misc vertype` reports `dbg`. Snapmaker added a user-facing **Root access** option in firmware **V1.2.0**; enabling it (or flashing debug mode via `custom_misc gen-debug`, which persists across upgrades) starts dropbear with the standard `root` / `lava` accounts, password `snapmaker`. That is the exact SSH path HelixScreen uses — **stock firmware is supported.** On stock firmware the `S50dropbear` vertype gate means the init script alone never starts SSH on a release build: when **Root access** is on, the stock GUI starts dropbear with `S50dropbear start --force` on every boot. With the stock GUI disabled, nothing would, so HelixScreen's platform hook does it instead (`assets/config/platform/hooks-snapmaker-u1.sh#ensure_root_ssh`, which reads the same `open_root` setting from `<printer_data>/config/gui_config.json`).
- **PAXX Extended Firmware** is *repackaged stock firmware* plus an overlay of patches. It removes the dropbear debug-mode gate (SSH on by default) and bundles extras HelixScreen does not use — Tailscale, OctoEverywhere, WebRTC camera, a `/firmware-config/` web UI, RFID filament write-back, etc. It is the turnkey option if you'd rather not enable stock root access yourself, but it is **not** a HelixScreen requirement.

| Firmware | SSH | Status | Boot launcher HelixScreen hooks |
|----------|-----|--------|----------------------------------|
| Stock 1.2+ | dropbear, enabled via Root access / debug mode | **Supported** (autostart newly added, see caveat) | No display init script exists; HelixScreen hooks `S99input-event-daemon` (the only boot-glob launcher present on stock). |
| PAXX 1.2 / 1.3 | on by default | **Tested** | `/etc/init.d/S99screen` (launches stock UI `/usr/bin/gui`). |
| PAXX 1.4 | on by default | **Tested + hardware-validated** | `S99screen` removed; HelixScreen hooks `S99fb-http`. |

> **Stock-firmware caveat:** deploy, SSH, WiFi-credential reuse, and display takeover (`chmod -x /usr/bin/gui`) all rely on the *base rootfs*, which is byte-identical between stock and PAXX — so they work on stock. The stock **boot-time autostart** (the `S99input-event-daemon` hook) is newly added and **not yet verified on a stock device** (development hardware runs PAXX). The hook is additive and idempotent, so it is a no-op on PAXX; see [How HelixScreen takes over the display](#how-helixscreen-takes-over-the-display).

The only PAXX-specific endpoint HelixScreen ever calls is `/printer/filament_detect/set` (RFID write-back); on stock it returns 404 and HelixScreen degrades gracefully (filament edits persist in HelixScreen's own store instead of mirroring back to firmware).

PAXX source: [paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware).

### Filament feeder presets and the preload_length cap

PAXX's Filament Feeders setting (Firmware Config, Snapmaker Components) can select a BIQU
PopStation mini preset, which links
`tweaks/klipper/filament_feeders_popstation_mini.cfg` into `extended/klipper/` and raises
`preload_length` for `[filament_feed left]` and `[filament_feed right]` (stock 950 mm). Stock
filament_feed.py caps the option at 1500 mm. The
`39-feature-filament-feeders` overlay patches that cap to 2500 mm
([01-filament-feed-preload-length-max.patch](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware/blob/main/overlays/firmware-extended/39-feature-filament-feeders/patches/home/lava/klipper/01-filament-feed-preload-length-max.patch)), and the preset's own header says it requires that
patch ([docs/filament_feeders.md](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware/blob/main/docs/filament_feeders.md)).
A preset value above 1500 on a build without the patch makes Klipper refuse to start with
`Option 'preload_length' in section 'filament_feed left' must have maximum of 1500.0`. The
preset in the firmware repository sets 2000 mm, following BIQU's setup guide.

HelixScreen shows a Klipper error that is already present when it starts, so this config
error appears in the recovery dialog at startup rather than as a silent disconnect
(`tests/unit/test_klippy_fault_at_startup.cpp` uses this exact message). Re-select the Default
feeder option, or update to firmware that carries the patch.

### How HelixScreen takes over the display

The U1 root filesystem is a read-only SquashFS with a writable OverlayFS upper on `/oem`. `/etc/init.d/S01aoverlayfs` wipes that upper on every boot **unless `/oem/.debug` exists**, so the installer touches `/oem/.debug` to make its changes persist. (This overlay/`.debug` mechanism is byte-identical across firmware 1.2/1.3/1.4.)

To own the display and auto-start at boot, the installer:

1. **Hooks a boot-time launcher** — installs a HelixScreen delegate into whichever boot-glob init scripts ship in the firmware's read-only SquashFS, each preserved as `*.stock` for the helix-not-installed fallback and for uninstall:
   - **PAXX 1.2 / 1.3** → `/etc/init.d/S99screen` (the stock display launcher); delegate is *helix-instead-of-stock-UI*.
   - **PAXX 1.4** → `/etc/init.d/S99fb-http` (`S99screen` was removed); delegate is *helix-instead-of-stock-UI*.
   - **Stock firmware** → `/etc/init.d/S99input-event-daemon`. Stock ships **no display launcher** in `/etc/init.d` (the stock UI is started by a supervisor binary), so there is nothing UI-shaped to hook. `S99input-event-daemon` is the one squashfs-resident, late-running script present on stock; the delegate *preserves* its function (runs the saved `.stock` to keep `input-event-daemon` going) and **then** starts HelixScreen. Because `helixscreen.init start` is idempotent, this same hook is harmless on PAXX (where an earlier launcher already started HelixScreen).
2. **Disables the stock UI binary itself** with `chmod -x /usr/bin/gui` (launcher-independent belt-and-suspenders so nothing can re-grab the framebuffer/DRM, regardless of which launcher or supervisor starts it). The uninstaller restores the exec bit.

Both changes live in the persistent overlay upper and survive reboot via `/oem/.debug`. A **firmware upgrade** re-flashes the rootfs and removes `/oem/.debug`, so HelixScreen must be reinstalled after any firmware update (stock or PAXX).

> **CRITICAL — the boot-glob trap that dictates which script we hook.** busybox `init` runs `/etc/init.d/rcS` **from the read-only SquashFS** and expands its `for i in /etc/init.d/S??*` boot loop **once**, *before* `S01aoverlayfs` does its `pivot_root` onto the `.debug` overlay. So the *list of script names* is frozen from the SquashFS; a script created **only** in the overlay upper (not present in the SquashFS) is **not** in that frozen glob → it **never runs at boot** (only at *shutdown*, via `rcK`, once the overlay is active — a boot/shutdown asymmetry). This is why an installer-created `/etc/init.d/S99screen` does not autostart on PAXX 1.4 *or* stock (neither ships `S99screen` in its SquashFS). The fix is to delegate from a script that **does** ship in the SquashFS and runs **after** `S01aoverlayfs` (so rcS executes its *overlay copy*, post-pivot): `S99fb-http` on PAXX 1.4, `S99input-event-daemon` on stock. Each hook is conditional (`[ -f ]`) and idempotent, so installing all of them is safe across every firmware variant.

## Cross-Compilation

The U1 target uses the same aarch64 cross-compiler as the Raspberry Pi. libstdc++ and libgcc are linked statically; libc and libdrm come from the printer.

### Build via Docker (Recommended)

```bash
# Build the Docker toolchain (first time only — cached after)
make snapmaker-u1-docker
```

The Docker image (`docker/Dockerfile.snapmaker-u1`) is based on Debian Trixie with `crossbuild-essential-arm64`. It uses Debian's `aarch64-linux-gnu` toolchain.

### Build Directly (Requires Toolchain)

```bash
make snapmaker-u1
```

### Build Configuration

| Setting | Value |
|---------|-------|
| Architecture | aarch64 (ARMv8-A) |
| Toolchain | `aarch64-linux-gnu-gcc` (Debian cross) |
| Linking | Hybrid (static libstdc++/libgcc, dynamic libc/libdrm) |
| Display backend | DRM/KMS (`/dev/dri/card0`, double-buffered page flipping) |
| Input | evdev (auto-detected) |
| SSL | Enabled |
| Optimization | `-Os -flto` (size-optimized) |
| Platform define | `HELIX_PLATFORM_SNAPMAKER_U1` |

### CI/Release Status

The snapmaker-u1 target is included in `release-all`, `package-all`, and the GitHub Actions release workflow. Binaries are built on every tagged release.

Manual packaging is also available:

```bash
make package-snapmaker-u1
```

## Installation

### Prerequisites

1. **Snapmaker U1** on the network (Ethernet or WiFi)
2. **SSH access** — via either firmware path:
   - **Stock firmware (1.2+):** enable the printer's **Root access** option (added in V1.2.0), or flash persistent debug mode (`custom_misc gen-debug`). This starts `dropbear`.
   - **PAXX Extended Firmware:** SSH is on by default. Download from [paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware), flash via USB (FAT32, `.bin` in root). To toggle SSH explicitly: `curl -X POST http://<printer-ip>/firmware-config/api/settings/ssh/true`.
3. **SSH access verified** — connect as root (works on both):
   ```bash
   ssh root@<printer-ip>   # password: snapmaker
   ```

### One-Line Install (Recommended)

For end users with Extended Firmware already installed, the easiest path is the hosted installer:

```bash
# Install on U1 (requires extended firmware and SSH access)
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

The installer auto-detects the U1 platform, downloads the correct aarch64 binary from the release CDN, deploys platform hooks, and starts HelixScreen. Re-run to upgrade.

### Build

```bash
# Build the Docker toolchain and cross-compile (first time builds the toolchain image)
make snapmaker-u1-docker
```

Output: `build/snapmaker-u1/bin/helix-screen` (~13MB stripped aarch64 binary)

### Deploy

```bash
# Full deploy (binary + assets + platform hooks) — stops stock UI, starts HelixScreen
make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=<printer-ip>

# Deploy and run in foreground with debug logging (recommended for first run)
make deploy-snapmaker-u1-fg SNAPMAKER_U1_HOST=<printer-ip>

# Deploy binary only (fast iteration during development)
make deploy-snapmaker-u1-bin SNAPMAKER_U1_HOST=<printer-ip>

# SSH into the printer
make snapmaker-u1-ssh SNAPMAKER_U1_HOST=<printer-ip>
```

Default SSH user is `root` (override with `SNAPMAKER_U1_USER`). Default deploy directory is `/userdata/helixscreen` (override with `SNAPMAKER_U1_DEPLOY_DIR`).

The deploy target automatically:
- Copies the binary, assets, and platform hooks to `/userdata/helixscreen/`
- Deploys the init script (`helixscreen.init`) and DRM keepalive hooks
- Starts HelixScreen via the init script (which sources the hooks)

### What Happens on Deploy

1. DRM keepalive: a background process opens `/dev/dri/card0` to prevent CRTC teardown
2. The stock UI process (`gui`) is killed, and the installer disables its binary (`chmod -x /usr/bin/gui`) so no launcher can relaunch it. `lmd` is left running: it supervises `gui`, `unisrv` (camera, MQTT and timelapse) and `flow_calc_server` (flow calibration), so killing it would take those down too. With `gui` not executable, `lmd` logs `execv(/usr/bin/gui) failed: Permission denied` and retries it every few seconds for as long as the printer is up; the retries are harmless noise.
3. HelixScreen starts as DRM master with double-buffered page flipping
4. The DRM keepalive process exits once HelixScreen has the DRM device open
5. The first-run wizard appears (language selection, printer connection setup)

### Rollback (Restore Stock UI)

To restore the stock Snapmaker touchscreen UI, run the uninstaller — it re-enables the stock UI binary (`/usr/bin/gui`) and restores the stock `S99screen` launcher (firmware 1.3) or removes the HelixScreen-created one (firmware 1.4):

```bash
ssh root@<printer-ip> "curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --uninstall; reboot"
```

> **A bare `rm -rf /userdata/helixscreen` is not enough.** The installer disables the stock UI binary (`chmod -x /usr/bin/gui`) so neither firmware's launcher can start it; removing HelixScreen without re-enabling the binary leaves a black screen. If you can't run the uninstaller, revert manually:
>
> ```bash
> ssh root@<printer-ip> "killall helix-screen helix-watchdog 2>/dev/null; chmod +x /usr/bin/gui; rm -rf /userdata/helixscreen; reboot"
> ```

The stock UI binary lives on the read-only SquashFS rootfs and is only disabled, never deleted — it cannot be damaged by HelixScreen deployment.

## Reversible Deployment Strategy

HelixScreen can be deployed to the U1 without modifying the read-only base firmware — all changes live in the writable overlay and are fully reversible.

### Level 1: Manual SSH Deployment (Current, Fully Reversible)

This is the current deployment method used by `make deploy-snapmaker-u1`:

1. Enable SSH: the **Root access** option on stock firmware 1.2+, or [PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) (SSH on by default). See [Firmware Requirements](#firmware-requirements)
2. Deploy via `make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=<ip>`
3. The installer disables the stock UI binary (`chmod -x /usr/bin/gui`) and hooks the firmware's own boot launcher (`S99screen`, `S99fb-http` or `S99input-event-daemon`, preserved as `*.stock`; see [How HelixScreen takes over the display](#how-helixscreen-takes-over-the-display)); HelixScreen starts on `/dev/fb0`. `lmd` (supervisor of `gui`, `unisrv` and `flow_calc_server`) keeps running.

**To revert**: run the uninstaller (re-enables `/usr/bin/gui` and restores the launcher), or manually `chmod +x /usr/bin/gui && rm -rf /userdata/helixscreen`, then reboot. The stock UI binary is on the read-only SquashFS rootfs and is only disabled, never deleted — but note that the init-script changes and the binary's exec bit **are** modified (in the reversible overlay), so a plain reboot alone will not bring the stock UI back.

### Level 2: SysV Init Override (Persistent, Reversible)

Level 1 is already persistent: the boot-launcher hook it installs lives in the `/oem/.debug` overlay and survives reboot. A standalone HelixScreen init script in `/etc/init.d/` does **not** work on its own, because the boot glob is frozen from the SquashFS before the overlay mounts (see the boot-glob trap under [How HelixScreen takes over the display](#how-helixscreen-takes-over-the-display)), which is why the installer delegates from a script the SquashFS already ships.

**To revert**: run the uninstaller, which restores each hooked launcher from its `*.stock` copy and the exec bit on `/usr/bin/gui`, then reboot.

### Level 3: Extended Firmware Overlay (Cleanest, Reversible)

Package HelixScreen as an overlay in paxx12's Extended Firmware build system:

1. Add a HelixScreen overlay that deploys the binary and init script
2. Build a custom firmware .bin with the overlay included
3. Flash via USB like any firmware update

**To revert**: Flash stock firmware (or Extended Firmware without the HelixScreen overlay) via USB. A/B firmware slots ensure the previous firmware is preserved.

### Safety Guarantees

| Risk | Mitigation |
|------|-----------|
| Bricked device | Impossible — Rockchip MaskRom mode provides hardware-level recovery |
| Lost stock UI | Stock UI lives on read-only SquashFS — cannot be accidentally deleted |
| Klipper/Moonraker disrupted | HelixScreen only replaces the display UI; Klipper (S60klipper) and Moonraker (S61moonraker) are independent services |
| Can't revert | Multiple revert paths: reboot, kill process, remove override, reflash firmware |
| Firmware slot corruption | A/B slots — switch with `updateEngine --misc=other --reboot` |

### Display Backend — DRM/KMS with CRTC Keepalive

HelixScreen uses the DRM backend for double-buffered page flipping on `/dev/dri/card0` (rockchipdrmfb). The 480x320 MCU panel runs on a DPI/RGB parallel interface via the Rockchip VOP2 display controller.

**The CRTC keepalive problem**: The stock UI (`/usr/bin/gui`) holds DRM master. When gui exits, the kernel's VOP2 driver calls `vop2_crtc_atomic_disable`, permanently disabling the display until reboot. The MCU panel driver only creates modes during the initial boot sequence — once the CRTC is disabled, there's no way to re-enable it.

**The solution**: The platform hooks (`assets/config/platform/hooks-snapmaker-u1.sh`) spawn a background process that holds `/dev/dri/card0` open *before* killing gui. This prevents the kernel from tearing down the CRTC when gui exits. HelixScreen then opens the DRM device itself and becomes DRM master. The keepalive process detects that HelixScreen has the device open and exits — but the CRTC stays active because HelixScreen now holds the fd.

**Critical implementation notes**:
- The keepalive MUST be a background subshell (`(exec 3>/dev/dri/card0; ...) &`), not a shell fd (`exec 7>`). Shell fds die when the init script exits, but background processes survive.
- The keepalive polls `/proc/*/fd` until it sees `helix-screen` with `/dev/dri/card0` open, then exits.
- `HELIX_DRM_DEVICE=/dev/dri/card0` is set in `platform_pre_start()` to skip auto-detection.
- No libinput is needed — touch input uses evdev directly.

**Filesystem note**: `/opt/` is an overlay filesystem wiped on reboot. All persistent data lives on `/userdata/` (ext4, 28GB). `/home/lava/` is also part of the overlay and is NOT persistent.

### Touch Input

Touch input is provided by a TLSC6x capacitive controller (`tlsc6x_touch`) on `/dev/input/event0`. HelixScreen auto-detects this device and uses multitouch (MT) axis ranges (0-480, 0-320). No touch calibration is required — the capacitive controller is factory-calibrated.

### Backlight

HelixScreen auto-detects the sysfs backlight device (`/sys/class/backlight/backlight`, max brightness 255) for sleep/wake control.

## Auto-Detection

HelixScreen auto-detects the Snapmaker U1 using 21 heuristics from `assets/config/printer_database.json`:

| Heuristic | Confidence | Description |
|-----------|------------|-------------|
| `fm175xx_reader` object | 99 | FM175xx RFID reader -- definitive U1 signature |
| `homing_precise_corexy` object | 99 | Snapmaker's homing module |
| `FILAMENT_DT_UPDATE` / `FILAMENT_DT_QUERY` macros | 95 | RFID filament detection macros |
| `extruder_offset_calibration`, `filament_entangle_detect` objects | 95 | Snapmaker modules |
| Hostname `u1` | 90 | Hostname contains "u1" |
| `machine_state_manager` object, `FEEDING_RUNOUT_EVENT_HANDLE` / `EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL` macros | 90 | Snapmaker modules and macros |
| Hostname `snapmaker` | 85 | Hostname contains "snapmaker" |
| `defect_detection`, `purifier` objects | 80-85 | Snapmaker modules |
| Four tool heaters | 75 | `tool_count` |
| `e1_fan` / `e2_fan` / `e3_fan` | 70 | Per-head fans |
| `tmc2240` object | 60 | TMC2240 stepper driver presence |
| Build volume | 50 | Bed mesh range |
| CoreXY kinematics | 30 | CoreXY motion system |
| aarch64 CPU | 10 | Host architecture |

No manual printer configuration is needed in most cases. The FM175xx RFID reader is the strongest signal -- it is unique to the U1 and provides near-certain identification.

When the U1 is detected, the printer database record provides these metadata fields:

| Field | Value |
|-------|-------|
| `probe_type` | `eddy_current` |
| `toolhead_style` | `snapmaker_u1` |
| `preset` | `snapmaker_u1` |
| `z_offset_calibration_strategy` | `firmware_managed` |
| `z_offset_persists` | `false` |
| `tool_change_resets_z_offset` | `true` |

The `snapmaker_u1` preset causes the first-run wizard to auto-skip hardware steps that do not apply to the U1 (e.g., probe wiring, toolhead identification).

**Z-offset adjustments are temporary.** HelixScreen offers no Save Z Offset step on
the U1 (`firmware_managed`), and the firmware does not persist a `SET_GCODE_OFFSET`
adjustment either. Each head keeps its own stored offset (`extruder.gcode_offset`,
set by the firmware's own `PROBE_XYZ_OFFSET_CALIBRATE SAVE_INFO=1`), and activating a
head re-applies that offset with an absolute `SET_GCODE_OFFSET X= Y= Z=`
(the firmware's `active_gcode_offset()` in its klippy kinematics/extruder.py, called
from its virtual_sdcard.py on each tool change). A babystep therefore lasts until the next tool change or the end of the
print, and Print Tune says so (`ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE`).

`Z_OFFSET_APPLY_PROBE` is no way out either (measured on the device): it writes the
first head's coil section and folds the active head's per-head offset into it, so it is
not a save of the babystep and HelixScreen never sends it here.

## Print Start Tracking

HelixScreen uses the `snapmaker_u1` print start profile (`assets/config/print_start_profiles/snapmaker_u1.json`) in weighted progress mode, so each phase's share of the bar comes from the ETA engine rather than the profile. The U1's PRINT_START is inlined into the slicer gcode, and the profile reads the firmware's own action codes (`// Success: Set action code <CODE>` and `// Success: Changed main state to PRINTING with action <CODE>`) plus two console lines:

| Signal | Phase | Label |
|--------|-------|-------|
| `// trigger_mcu_pos: {` (G28 X/Y) | HOMING | Homing axes... |
| `// z offset: ` (the G28 Z touch, after every tool pick and before the mesh) | HOMING | Probing Z... |
| `PRINT_SWITCH_CHECKING` | INITIALIZING | Checking extruders... |
| `PRINT_AUTO_FEEDING` / `PRINT_REPLENISHING` | INITIALIZING | Loading filament... / Replenishing filament... |
| `PRINT_BED_DETECTING`, `DETECT_PLATE` | BED_MESH | Inspecting bed... / Detecting plate... |
| `BED_PREHEATING` (the firmware's bed soak) | SOAKING | Heat Soaking... |
| `BED_PRESCANNING` (four corners) | BED_MESH | Validating bed mesh... |
| `BED_LEVELING` | BED_MESH | Bed mesh... (its message change restarts the probe count) |
| `PRINT_PREEXTRUDING` | PURGING | Priming... |
| `PRINT_RESUMING` | INITIALIZING | Resuming print... |

`// z_mesh_complete:` also comes from G28 Z, so it maps to nothing. Heating has no pattern: while an M109/M190/TEMPERATURE_WAIT reports, the collector reads the command at `virtual_sdcard.file_position` and names the heater it waits on (`PRINT_START_PROFILES.md`, the heater-wait row of the test table). The first model layer, not an action code, ends the pre-print.

### The print position fills the silent steps

The nozzle cleans and the prime line say nothing on the console, so the profile sets `scripted_start` and the collector follows `virtual_sdcard.file_position` through the start block the slicer wrote (`PRINT_START_PROFILES.md`, `scripted_start`). Each `*_CLEAN_NOZZLE*` line shows "Cleaning Nozzle...", each `G28 Z` "Probing Z...", `BED_MESH_CALIBRATE` "Bed mesh..." and the first extruding `G1` "Purging...". The action codes above still name the steps inside a line (the soak, the corners, the mesh, the plate), and arrive before the frame naming the line. The profile's `start_block_owned_lines` says which command prints which, taken from the console captures: `BED_MESH_CALIBRATE` prints BED_PREHEATING, BED_PRESCANNING, BED_LEVELING and a `// z offset:` touch, and `G28` prints `// trigger_mcu_pos:` and `// z offset:`. The clean macros print no code at all, so none is listed for them.

What one print showed, read-only from the printer at 1 Hz on 2026-10-17 (`tests/fixtures/u1_bmcu_*`, one head, reported as T1). The position is where the print stood; the clean macros queue their moves and return, so the position passes their line at once and stands on the next while they run.

| s | Position (byte, line) | Console | Shown |
|---|---|---|---|
| 0 | 12961 `G28 X Y` | `trigger_mcu_pos` | HOMING, Homing axes... |
| 26 | 13011 `DEFECT_DETECTION_DETECT_BED` | `PRINT_BED_DETECTING`, five samples | BED_MESH, Inspecting bed... |
| 87 | 13339 `SM_PRINT_AUTO_FEED EXTRUDER=1` | `PRINT_SWITCH_CHECKING`, `PRINT_AUTO_FEEDING`; an M109 to 280 inside the feed | INITIALIZING, Loading filament..., the nozzle wait at 94-115 s |
| 144 | 13743 `M109 T0 S170` | wait reports, nozzle cooling 256 -> 172 | HEATING_NOZZLE |
| 187 | 13790 (past 13756 `ROUGHLY_CLEAN_NOZZLE_WITH_DISCARD`) | none | CLEANING |
| 190 | 13824 `G28 Z I140 J140` | `// z offset:`, probes at 140,140 | HOMING, Probing Z... |
| 209 | 13879 `DETECT_BED_PLATE` | `DETECT_PLATE` | BED_MESH, Detecting plate... |
| 224 | 13992 `M109 S210` | wait reports | HEATING_NOZZLE |
| 234-249 | 14023-14197 (past 14002 `ROUGHLY_CLEAN_NOZZLE`, then on 14057 `FINELY_CLEAN_NOZZLE_STAGE_1` and 14197 `_STAGE_2`) | the stage probes at y 276.5 | CLEANING |
| 249 | 14271 `M109 S170` | wait reports, cooling 194 -> 170 | HEATING_NOZZLE |
| 264 | 14281 `M190 S100` | wait reports, bed 90 -> 99 | HEATING_BED (from the next fallback tick, 270 s) |
| 319 | 14316 `G28 Z` | `// z offset:`, probes at the mesh's corner | HOMING, Probing Z... |
| 340 | 14818 `M109 S260` (the position never stood on 14559 `BED_MESH_CALIBRATE`) | `print_task_config configuration does not do auto-leveling` | HEATING_NOZZLE |
| 360 | 14840 `G1 X110 E15 F360` | none | PURGING |
| 376 | 16178, the first layer | | |

That print ran no mesh, so it cannot answer whether an `ADAPTIVE=0` mesh's point count can be read ahead of time: there was no `BED_PRESCANNING`, `BED_LEVELING` or `PRINT_PREEXTRUDING`, and no probe table. The start block names the grid itself (`PROBE_COUNT=4,4 ADAPTIVE=0`), so a print with auto-leveling on would settle it. The `BED_PREHEATING` soak runs inside the mesh line, which is what keeps "Heat Soaking..." up when the position reaches it.

Each phase's history entry is the time it showed, summed over its visits (`PREPRINT_PREDICTION.md`, Phase Timing Tracking): that print records HOMING 67 s, BED_MESH 74 s, CLEANING 18 s and PURGING 17 s. INITIALIZING (the feed) and the heater waits are not recorded as phases.

## Pre-Print Flow Calibration (Pressure Advance)

The U1 auto-calibrates pressure advance per extruder at the start of every print (the "Purging" phase above). Snapmaker calls this **flow calibration**, but the value it computes and applies is the Klipper pressure-advance K.

**The sensor is an eddy-current inductance coil, not a load cell.** Each toolhead has one `[inductance_coil extruder{N}]` (pins `e0:PA0`…`e3:PA0`) — the same sensor genus the U1 uses as its Z-probe (probe_inductance_coil.py, carries a `z_offset`). It outputs a 1–2 MHz frequency that shifts with tiny displacement of a metal target, reacting to extrusion back-pressure. There is no strain-gauge/load-cell in this path.

**Mechanism** (on-device klippy/extras/flow_calibrator.py, backed by the native `klippy/extras/flow_calc_server` binary over FIFO pipes `/tmp/flow_calculator_{req,resp}`):

- The machine start gcode calls `SM_PRINT_FLOW_CALIBRATE EXTRUDER=N` once per active extruder, right after auto-feed and before the bed mesh.
- For each candidate K, `_measure_k()` purges filament in a repeating slow/fast pattern (defaults **0.8 mm/s slow, 8 mm/s fast, 200 mm/s² accel, 14 loops**) while recording (a) coil frequency samples and (b) trapq acceleration timing. The native server returns a signed **`area`** — the flow mismatch between the fast and slow phases. At the correct K, fast/slow stay pressure-matched and `area → 0`.
- It root-finds the zero-crossing (default `LINEAR_FITTING`: `np.polyfit` through ~5 `(k, area)` points; alt `DICHOTOMY` bisects on the sign of `area`), applies the result live via `_set_pressure_advance`, persists it to flow_calibrator.json, and records it into the virtual_sdcard for power-loss recovery (`record_pl_print_pressure_advance`, ties into [PLR](#print-start-tracking)).
- It marks the extruder calibrated so it won't re-run mid-print, and aborts cleanly (falling back to the filament default K) on filament runout, tangle, cancel, or out-of-range.

**Gotcha — OrcaSlicer's manual PA overrides the measurement.** Whether the calibrated K actually reaches the print is decided entirely by the Orca filament profile's **"Enable pressure advance"** toggle:

| Orca `enable_pressure_advance` | What prints | Why |
|---|---|---|
| **OFF** | The coil-measured K | Orca emits no `SET_PRESSURE_ADVANCE` — the value applied by `SM_PRINT_FLOW_CALIBRATE` stands for the whole job (all tools). |
| **ON** | Orca's static K (e.g. 0.02) | Orca injects `SET_PRESSURE_ADVANCE ADVANCE=<val>; Override pressure advance value` after calibration (and once per tool change on multi-color), clobbering the measurement. The calibration still runs and still saves to disk, but is discarded for the print. |

Verified against two real slices: a single-color file with `enable_pressure_advance = 1` had exactly one `SET_PRESSURE_ADVANCE` (after the four calibrations); a 4-color file with `enable_pressure_advance = 0` had **zero** across 13k lines. So counter-intuitively, to *use* the sensor you *disable* PA in Orca — enabling it means "I'm supplying my own value, ignore the sensor." (Not yet confirmed which state is Snapmaker's shipped Orca-profile default.)

**HelixScreen relevance:** the routine is gated on the print task's `flow_calibrate` flag (disable-able per job), and emits `SET_MAIN_STATE MAIN_STATE=FLOW_CALIBRATION` plus per-extruder `{EXTRUDER}_FLOW_CALIBRATING` action codes — both surfaceable in the UI during the pre-print phase.

**Running it outside a print.** `SM_PRINT_FLOW_CALIBRATE` is the print-job wrapper and returns without a word when no print task is running. The routine itself is `FLOW_CALIBRATE`, registered by the same Python extra (listed in `/printer/gcode/help`, never a `gcode_macro`). `FLOW_CALIBRATE TEMP=<c>` calibrates the **mounted** extruder: it homes X/Y if needed, moves to the discard position, heats to `TEMP` (clamped to the heater's limits), purges through about five candidates and applies and saves the result. The flow calibrator has no status object, so the capability is recognised by `filament_parameters`, the Snapmaker extra it cannot run without. Its console lines:

| Line | Meaning |
|---|---|
| `measure k: 0.02000` | one per candidate |
| `Got pressure advance: 0.0412` | the result |
| `flow k is out of range, use default value:0.02` | failed; the default K was applied |
| `abort calibration: <reason>` | failed |

K is set through the extruder stepper directly, so no `SET_PRESSURE_ADVANCE` echo reaches the console. HelixScreen drives this from its Pressure Advance screen: its `SNAPMAKER_U1` provider row in `src/printer/pa_calibration.cpp` holds the command and patterns, and `src/printer/pa_calibration.cpp#procedure_for` turns it into a run.

## Error codes

The firmware reports every fault it raises through `exception_manager` as
`LLLL-IIII-NNNN-CCCC`: level, module id, index, code. HelixScreen decodes that token
(`include/snapmaker_exceptions.h#decode_exception_code`) wherever it arrives: a console line, a
raise notification, the standing list in `exception_manager.exceptions`, a `{"coded": ...}`
envelope, or `print_stats.exception` on the pause it caused.

**One table words them all.** `src/printer/snapmaker_exceptions.cpp` holds a row per code (or per
run of codes with one meaning) with a short summary, the cause and the fix, following the map
below. A summary with `{}` names the head the index points at ("Toolhead 1 could not load its
filament"). The rows feed:

- console lines and raise notifications, through the backend
  (`src/printer/ams_backend_snapmaker.cpp#classify_coded_fault`): the summary is the dialog's or
  toast's title, the cause and the fix its text. A feeder failure (525) that pauses a print is a
  dialog with Open Filament, Cancel Print and Close;
- the pause's stop reason on the print status tile, through the pause descriptor
  (`src/printer/pause_descriptor.cpp#read`), which a cancel keeps;
- the fault-code classifier's fallback (`src/printer/firmware_fault_codes.cpp`), which reads the
  cause when no filament backend answers.

A code with no row keeps the firmware's own text: 522-0 (every uncoded command error), 523-9000,
530-9 (the calibration steps raise it with the failing step's own reason, a heater that will not
heat among them) and 531-23 carry the real reason in their message, and the rest are codes the source never raises.
Causes marked unverified below have no row's certainty behind them; their wording stays generic.

### Code format and raise paths

A fault is `LLLL-IIII-NNNN-CCCC`: level, module id, index, code, four digits each (`<klipper>/klippy/coded_exception.py`, `CodedException.structured_code`). Level 1 is informational, 2 asks for a pause, 3 asks for a cancel or stop. The id is the module (`<klipper>/klippy/exception_manager.py`, `ExceptionList`). The index is usually the toolhead (0-based, so index 0 is "Toolhead 1" in the UI), but some codes use an MCU, an axis or an endstop index instead; each row says which.

Codes reach `exception_manager.raise_exception_async` (and from there Moonraker's `raise_exception` remote method) three ways:

1. **Keyword form.** `gcmd.error(message, action=..., id=..., index=..., code=..., level=...)`, since `gcode.CommandError` is a `CodedException`. A top-level G-code error goes through `<klipper>/klippy/klippy.py` `raise_coded_exception`. **Any plain `gcmd.error("text")` with no keywords is still reported, as `0003-0522-0000-0000`** (the class defaults: id 522, index 0, code 0, level 3, action cancel).
2. **JSON in the message text.** `'{"coded": "0003-0522-%04d-0012", "msg": "...", "action": "pause", "oneshot": 0}'`. `raise_coded_exception`, `invoke_shutdown` and `virtual_sdcard.work_handler` parse it with `extract_encoded_message`; the `coded` string overrides id/index/code/level, and `action` overrides the action.
3. **Direct calls.** `exception_manager.raise_exception_async(id=, index=, code=, level=)` and `printer.raise_structured_code_exception("LLLL-IIII-NNNN-CCCC", msg)` (`<klipper>/klippy/klippy.py`). These only report; whatever module raises them does its own pausing.

**What actually happens to a print** is decided by the `action`, not the level, when the error comes out of a G-code line run from a print file (`virtual_sdcard.work_handler`): `pause` runs `PAUSE ON_ERROR=1` (532-0000-0004 runs plain `PAUSE`), `pause_runout` runs `PAUSE IS_RUNOUT=1`, `none` lets the print continue, and anything else (the default `cancel`) ends the print. That is why a few level-1 codes below still end a print. Shutdown paths (`invoke_shutdown`) halt the printer whatever the level says; the user must restart firmware.

Several formats use `%4d` rather than `%04d`, so the raw message text can carry space-padded fields such as `"0002-0523-   1-0018"`. Python's `int()` accepts that, so the reported integers are correct, but anything that parses the raw `coded` string out of a message must tolerate the spaces.

Grip-state letters used below (from `<klipper>/klippy/extruder.py` `get_extruder_activate_status`): each head reports three sensors, `[parked, picked, grab_valid]`, as T/F. `FFF` = the head is seen nowhere (detached), `FFT` = only the pogo-pin contact is seen (pogo pins not connected), `TTx` = it reads as docked and on the carriage at once (conflicting sensors).

### 522 MOTION / system

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 522-0 | 0 | 3, cancel | Any G-code command failing with a plain error that has no code of its own | A command failed and the firmware gave no specific reason code. Read the message text for the real reason. | Read the message shown with the error; retry the action. If it repeats, restart the printer and send the log to support. | coded_exception.py defaults, applied by klippy.py raise_coded_exception to every uncoded gcmd.error |
| 522-2 | 0 | 3, firmware shutdown | Printer entered shutdown for a reason with no more specific code (includes "MCU shutdown" for most controller faults, "spontaneous restart", internal errors in ready callbacks) | The printer stopped itself for safety. The message says which board or reason. | Restart firmware from the screen. If it repeats, power-cycle the printer and check the cables to the toolheads. | klippy.py invoke_shutdown (default code) |
| 522-3 | 0 | 3, printer will not start | Reading the printer configuration at startup | A setting in the printer's configuration file is invalid or missing. | Undo any recent config edits, or restore the default config, then restart firmware. | klippy.py _connect (config_error) |
| 522-4 | 0 | 3, printer will not start | Setting up pins at startup | The configuration assigns a hardware pin that is invalid or used twice. | Undo recent config edits or restore the default config, then restart firmware. | klippy.py _connect (pins.error) |
| 522-5 | 0 | 3, printer will not start | Talking to a controller board at startup | A board is running firmware that does not match the printer software (protocol mismatch). | Run the official firmware update again so every board is updated, then restart. | klippy.py _connect (msgproto.error) |
| 522-6 | MCU: 0 host, 1 main board, 2-5 toolhead boards e0-e3, 100 no toolhead boards found (USB hub OK), 101 USB hub not detected, 255 unknown | 3, printer will not start | Connecting to the controller boards at startup | The printer could not connect to one of its boards. Index 100 means none of the toolhead boards answered; 101 means the internal USB hub was not seen. | Power-cycle the printer. If it repeats, check that every toolhead is seated and its cable is connected; for 101 the internal USB hub or its cable is suspect, contact support. | klippy.py _connect (mcu.error) |
| 522-7 | 0 | 3, printer will not start | Startup | Unexpected internal error during startup. | Restart firmware; if it repeats, power-cycle and send the log to support. | klippy.py _connect (generic Exception) |
| 522-8 | MCU: 0 host, 1 main board, 2-5 toolhead boards e0-e3, 100 all toolhead boards lost at once, 255 unknown | 3, firmware shutdown | Running; a board stopped answering | Lost communication with a board. Index 100 means all four toolhead boards dropped at the same time (a shared cable or the USB hub). | Power-cycle the printer. Check the toolhead cable for the named board; for 100 check the shared toolhead harness and hub. | mcu.py MCU.check_active |
| 522-9 | 0 | 3 | Setting up the accelerometer (input shaping or vibration checks) | The accelerometer did not accept a setting; usually a loose cable or bad sensor. | Restart and retry. If it repeats, check the accelerometer cable and contact support. | extras/sc7a20.py set_reg; extras/lis2dw.py set_reg |
| 522-10 | 0 | 3 | Starting accelerometer measurements | The accelerometer reported the wrong chip id; usually a loose cable or bad sensor. | Same as 522-9. | extras/sc7a20.py _start_measurements; extras/lis2dw.py _start_measurements |
| 522-11 | 0 | 3 | Polling a stepper motor driver | A motor driver reported a fault (overheat, short, or reset). The message names the motor. | Power off, let the printer cool, check that nothing blocks that axis, restart. If it repeats, contact support. | extras/tmc.py _query_register |
| 522-12 | axis: 0 X, 1 Y, 2 Z | 3, cancel | Planning a move | A move was sent before that axis was homed. | Home all axes, then retry. | toolhead.py Move.move_error |
| 522-13 | axis: 0 X, 1 Y, 2 Z | 3, cancel | Planning a move | A move went outside the printer's travel limits on that axis. The file may be sliced for a different printer or have a bad offset. | Re-slice for the U1 profile and check that the model fits on the plate; re-home and retry. | toolhead.py Move.move_error |
| 522-14 | 0 | 3, firmware shutdown | A controller reported "ADC out of range" for a sensor the firmware could not identify | A temperature reading went outside its valid range on an unknown sensor. | Restart firmware; check temperature sensor cables. Contact support if it repeats. | extras/error_mcu.py _check_mcu_shutdown |
| 522-15 | 0 | 3, firmware shutdown | Heater watchdog, for a heater that is neither a nozzle nor the bed | A heater did not warm up at the expected rate. | Restart firmware; check that heater's wiring. Contact support if it repeats. | extras/verify_heater.py heater_fault |
| 522-16 | MCU: 0 host, 1 main board, 2-5 toolhead boards e0-e3 | 3, firmware shutdown | A board shut down with "Timer too close" | The board fell behind on its timing, usually because the host was overloaded or the connection hiccuped. | Restart firmware. If it repeats during prints, avoid heavy extra load (camera streaming, large uploads) and contact support. | klippy.py invoke_shutdown |
| 522-17 | power_loss_check: 0. serialhdl: 0 main board, 1-4 toolhead boards e0-e3, 255 unknown (note: a different board numbering from 522-6/8/16) | 3, firmware shutdown (power_loss_check) or report only (serialhdl) | Power-loss detection | Mains power dropped or dipped. | Check the power cable and outlet. After power returns, use power-loss recovery to resume the print if offered. | extras/power_loss_check.py handle_report_power_loss_status; serialhdl.py SerialReader.handle_output |
| 522-18 | 0 | 3, firmware shutdown | Shutdown requested over the API | Something (an app or the screen) asked the printer to shut down. | Restart firmware. | klippy.py invoke_shutdown (shutdown_codes table) |
| 522-19 | 0 | 3, firmware shutdown | M112 emergency stop | An emergency stop was issued. | Make sure the printer is safe, then restart firmware. | klippy.py invoke_shutdown (shutdown_codes table) |

### 523 TOOLHEAD

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 523-0 | toolhead | 2, pause (runout pause). Kept active (oneshot 0) until filament is detected again | Printing, or starting flow calibration, or a runout check command | The toolhead's filament sensor sees no filament: the spool ran out or the filament broke before the head. When the print pauses, the firmware first tries auto-replenish from another spool; the code is raised only if that did not happen. | Load filament into that toolhead (or replace the spool), then resume. If filament is present, check for a break between the spool and the head. | extras/filament_switch_sensor.py RunoutHelper._runout_event_handler, cmd_CHECK_FILAMENT_RUNOUT; extras/flow_calibrator.py cmd_FLOW_CALIBRATE (before start, and on abort, where it is not reported to Moonraker: proactive_report=0) |
| 523-1 | toolhead | 3, cancel | Extruding | The nozzle is below the minimum temperature allowed for extruding. | Heat the toolhead to the filament's temperature, or check that it is heating at all (see 523-3). | kinematics/extruder.py PrinterExtruder.check_move |
| 523-2 | toolhead | 3, firmware shutdown (toolhead power is cut) | Reading the nozzle temperature | The nozzle temperature reading is out of range: the thermistor is disconnected, shorted or damaged. | Power off, re-seat that toolhead on its dock and carriage, check its connector. If it repeats, the hotend thermistor needs replacement. | extras/error_mcu.py _check_mcu_shutdown |
| 523-3 | toolhead | 3, firmware shutdown | Heater watchdog | The nozzle did not heat up at the expected rate: heater or thermistor fault, or a loose hotend. | Restart, re-seat the toolhead and check the hotend is fully inserted. If it repeats, check the heater and thermistor. | extras/verify_heater.py heater_fault |
| 523-4 | toolhead | 3, cancel | Extrude-only move | An extrude or retract move was longer than the firmware allows; usually a bad G-code command or macro. | Re-slice, or check the macro or command you sent. | kinematics/extruder.py PrinterExtruder.check_move |
| 523-5 | toolhead | 3, cancel | Printing move | The file asks for more extrusion than the firmware allows (too wide or too fast a line for the nozzle). | Re-slice with the correct nozzle size and profile for the U1. | kinematics/extruder.py PrinterExtruder.check_move |
| 523-6 | always 0 (the head number is in the message text, not the index) | 1, info (but a command error, so a print would end: action defaults to cancel) | Setting a nozzle temperature by toolhead number | A temperature was set for a toolhead number that does not exist. | Re-slice for the U1 profile with the right number of toolheads. | kinematics/extruder.py PrinterExtruder._set_extruder_temp |
| 523-7 to 523-17 | toolhead being switched to | 2, pause | Toolhead change, before picking: checking which head is on the carriage | More than one toolhead reads as picked up at once. The code says which: 7 heads 1+2, 8 heads 1+3, 9 heads 1+4, 10 heads 2+3, 11 heads 2+4, 12 heads 3+4, 13 heads 1+2+3, 14 heads 1+2+4, 15 heads 1+3+4, 16 heads 2+3+4, 17 all four (UI numbering). Usually a dock or carriage sensor fault. | Make sure every toolhead not on the carriage is fully seated in its dock, then home and resume. If it repeats, the dock or carriage sensors need checking. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER (code = combination + 6) |
| 523-18 | toolhead being switched to | 2, pause | Toolhead change, before picking | A toolhead is in an unknown position (not docked, not on the carriage) and the firmware will not switch. | Check every toolhead: seat any loose one fully in its dock, then home and resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-19 | toolhead being parked | 2, pause | Toolhead change, before parking the current head | The head on the carriage does not read as properly picked up, so the firmware will not park it. | Check that the current toolhead is firmly attached to the carriage; re-seat it, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-20 | toolhead being parked | 2, pause | Toolhead change, after parking | After parking, the head does not read as docked. | Check that the toolhead sits fully in its dock and nothing blocks the dock. Re-seat it, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-21 | toolhead being picked | 2, pause | Toolhead change, before picking | The head to be picked up does not read as docked. | Check that toolhead is in its dock; re-seat it, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-22 | toolhead being picked | 2, pause | Toolhead change, after picking | A different toolhead than the one requested reads as being on the carriage. | Check which head is on the carriage; dock all heads properly, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-23 | toolhead being picked | 2, pause | Toolhead change, after picking | The pick failed: every toolhead still reads as docked. | Check that the carriage and the head's latch are clear of debris; re-seat the head, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-24 to 523-34 | toolhead being picked | 2, pause | Toolhead change, after picking | More than one head reads as picked up after the pick. Same combinations as 7-17 (24 heads 1+2 ... 34 all four). | As for 523-7. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER (code = combination + 23) |
| 523-35 | toolhead being picked | 2, pause | Toolhead change, after picking | The pick failed with a toolhead in an unknown position (no more specific code applied). | Check all toolheads are docked or firmly on the carriage, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-36 | toolhead | 3, cancel | Setting a nozzle temperature | The requested nozzle temperature is outside the allowed range. | Check the temperature in the slicer filament profile. | extras/heaters.py Heater.set_temp |
| 523-37 | 0 | 1, info (kept until reset) | Periodic check while not printing | Toolhead switch count since the last maintenance passed the recommended threshold. | Do the toolhead maintenance Snapmaker recommends (clean docks and pogo pins), then reset the maintenance counter. | kinematics/extruder.py ExtruderSwitchRecorder._on_save_timer |
| 523-38 | toolhead | 2, pause | Printing or flow calibration: comparing extruder movement with the feeder wheel movement | The filament is tangled or stuck between the spool and the head: the extruder is pulling but the feeder wheels are not turning. | Check the spool for tangles or a stuck spool, clear the filament path, then resume. | extras/filament_entangle_detect.py _check_entangle_event; extras/flow_calibrator.py cmd_FLOW_CALIBRATE (not reported to Moonraker there) |
| 523-39 | toolhead | 2, pause | Print start or flow calibration, checking the head's filament info | No filament type is set for a toolhead the print uses. | Set the filament type for that toolhead on the screen or in the app, then resume. | extras/print_task_config.py cmd_INNER_CHECK_AND_RELOAD_FILAMENT_INFO; extras/flow_calibrator.py cmd_FLOW_CALIBRATE |
| 523-40 | the detached toolhead | 2, pause | Toolhead change, before picking | A toolhead reads as detached: not in its dock and not on the carriage (FFF). | Find that toolhead, put it back in its dock fully, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-41 | the affected toolhead | 2, pause | Toolhead change, before picking | A toolhead reads as both docked and picked up at once (sensor conflict). | Re-seat that toolhead; if it repeats, its dock or carriage sensor needs checking. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-42 | toolhead being parked | 2, pause | Toolhead change, after parking | The head did not go back into its dock (still reads as on the carriage, FTx). | Check the dock for debris or a bent latch; guide the head into the dock, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-43 | toolhead being parked | 2, pause | Toolhead change, after parking | The head reads as detached after parking (FFx): it fell off or is not in the dock. | Find the toolhead, seat it in its dock, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-44 | toolhead being parked | 2, pause | Toolhead change, after parking | The head reads as both docked and picked up at once (TTx). | Re-seat the head; if it repeats, check its sensors. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-45 | the affected toolhead | 2, pause | Toolhead change, after picking | That head's pogo pins are not making contact (FFT). | Clean the pogo pins on the head and carriage, re-seat the head, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-46 | the affected toolhead | 2, pause | Toolhead change, after picking | That head reads as detached (FFF). | Find the toolhead, seat it properly, home, resume. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-47 | the affected toolhead | 2, pause | Toolhead change, after picking | That head reads as both docked and picked up (TTx). | Re-seat the head; if it repeats, check its sensors. | kinematics/extruder.py _cmd_SWITCH_EXTRUDER |
| 523-48 | always 0 (per-head counts are in the message) | 1, info | End of a print: the head fan had read 0 RPM while on, two or more times in total | The firmware lost contact with a toolhead's fan during the print, which it treats as a pogo-pin disconnection. | Clean the pogo pins on the toolheads and carriage, then retry. | kinematics/extruder.py PrinterExtruder._handle_stop_print_job |
| 523-9000 | toolhead | 2, pause | Flow (pressure advance) calibration | Flow calibration failed for some other reason; the message carries the underlying error. | Read the message, fix that cause (often a probe or homing problem), and retry, or turn off flow calibration for this print. | extras/flow_calibrator.py cmd_FLOW_CALIBRATE |
| 523-9001 | toolhead | 3, cancel | Flow calibration | Flow calibration is not allowed for this filament with a 0.2 mm nozzle (PA, PC, carbon or glass fiber filled, and other listed types). | Turn off flow calibration for this print, or use a larger nozzle for that filament. | extras/flow_calibrator.py cmd_FLOW_CALIBRATE (rule in filament_parameters.py is_allow_to_print) |

### 524 CAMERA

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 524-0 | 0 | 2, pause | Starting timelapse recording at print start | The camera service did not confirm that timelapse recording started. | Check that the camera works (open its live view), restart the printer if it does not, or turn off timelapse for this print, then resume. | extras/timelapse.py cmd_TIMELAPSE_START |

### 525 FEEDING

Index = toolhead the feeder channel serves (`filament_ch[ch]`). The "inlet sensor" is the feeder's port sensor (`_port[ch]`); the "head sensor" is that toolhead's filament motion sensor (`filament_motion_sensor eN_filament`). Preload failures are informational; load, unload and hand-feed failures come out of `FEED_AUTO` / `FEED_MANUAL` as pause errors. **When a load or unload fails while homing, switching toolheads, moving or heating, the 525 code is not used:** the original error is re-raised (`_is_keep_raw_error_info`), so the user sees that error's own code (528 homing, 523 toolhead switch, 523/526 heater).

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 525-0 | toolhead | 2 reported, but action none (does not pause or cancel) | Turning automatic feeding on or off for a channel | The auto-feed setting could not be applied. | Retry the setting; restart the printer if it keeps failing. | extras/filament_feed.py cmd_FEED_AUTO |
| 525-10 | toolhead | 1, info | Preloading filament into the feeder | Preload failed for a reason with no specific code. | Pull the filament out, trim the tip straight, and insert it again. | extras/filament_feed.py _do_feed (preload) |
| 525-11 | toolhead | 1, info | Preloading | The feeder motor is not turning at the expected speed (stalled or blocked). | Check the feeder for jammed filament or debris; reinsert the filament. If it repeats, contact support. | extras/filament_feed.py _do_feed (preload) |
| 525-12 | toolhead | 1, info | Preloading | The motor turns but the feeder wheels do not: the filament is slipping or not gripped. | Trim the tip straight and push the filament in firmly until it is gripped. | extras/filament_feed.py _do_feed (preload) |
| 525-13 | toolhead | 1, info | Preloading | The inlet sensor sees no filament (missing at start or pulled out during preload). | Insert the filament fully into the feeder inlet. | extras/filament_feed.py _do_feed (preload) |
| 525-14 | toolhead | 1, info | Preloading | The filament did not reach its preload position in time. | Check the tube between the feeder and the toolhead for kinks or blockage; reinsert. | extras/filament_feed.py _do_feed (preload) |
| 525-15 | toolhead | 1, info | Preloading | The toolhead's filament sensor already sees filament, so old filament is still in the path. | Unload the old filament from that toolhead first, then preload. | extras/filament_feed.py _do_feed (preload) |
| 525-30 | toolhead | 2, pause | Loading filament to the toolhead | Loading failed for a reason with no more specific code. | Retry the load. If it repeats, unload, trim the tip and load again. | extras/filament_feed.py _do_feed (load); raised by cmd_FEED_AUTO |
| 525-31 | toolhead | 2, pause | Loading, pushing filament toward the toolhead | The feeder motor stopped turning. | Check the feeder and tube for a jam; clear it and retry. | same |
| 525-32 | toolhead | 2, pause | Loading, pushing filament toward the toolhead | The feeder wheels stopped turning: filament stuck or slipping. | Check for a tangle on the spool or a blocked tube; trim and reload. | same |
| 525-33 | toolhead | 2, pause | Loading | The inlet sensor lost the filament (it was never there, ran out, or was pulled back). | Insert filament into the feeder inlet, then retry. | same |
| 525-34 | toolhead | 2, pause | Loading, pushing filament toward the toolhead | The filament did not reach the toolhead's filament sensor in time. | Check the tube between the feeder and the toolhead for kinks or blockage; retry. | same |
| 525-35 | toolhead | 2, pause | Loading, pushing filament toward the toolhead | The feeder pushed more than the maximum length and the toolhead sensor never saw filament (broken filament, a disconnected tube, or a faulty head sensor). | Check that the tube is connected at both ends and the filament is not broken inside it; retry. | same |
| 525-50 | toolhead | 2, pause | Loading, after the extruder gripped: purging (flushing) the new filament | The purge step after extruding failed. | Check the purge area and nozzle wiper are clear, clean the nozzle, retry the load. | same (failure in the flushing step; code set before the extrusion step and kept when the flush fails) |
| 525-51 | toolhead | 2, pause | Loading, trying to get the toolhead extruder to grip the filament | The extruder never gripped the filament: no movement seen on the feeder wheels or nozzle sensor after all retries (or the extrude step itself raised an error). | Cut the filament tip straight, check the nozzle is not clogged and the toolhead is hot, then retry. | same |
| 525-70 | toolhead | 2, pause | Unloading filament | Unloading failed (state mismatch, unload script error, or bad parameter). | Retry the unload; if the filament is stuck, heat the toolhead and pull it out by hand. | extras/filament_feed.py _do_feed (unload); raised by cmd_FEED_AUTO |
| 525-90 | toolhead | 2, pause | Hand feeding (manual load) | A manual feed step failed. | Retry the manual load from the start. | extras/filament_feed.py _do_feed (manual feed); raised by cmd_FEED_MANUAL |

### 526 HEATER_BED

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 526-0 | 0 | 3, firmware shutdown | Reading the bed temperature | The bed temperature reading is out of range: bed thermistor disconnected or damaged. | Power off and check the bed cable connection; contact support if it repeats. | extras/error_mcu.py _check_mcu_shutdown |
| 526-1 | 0 | 3, firmware shutdown | Heater watchdog | The bed did not heat at the expected rate. | Restart; check the bed heater cable. Contact support if it repeats. | extras/verify_heater.py heater_fault |
| 526-2 | 0 | 3, cancel | Setting the bed temperature | The requested bed temperature is outside the allowed range. | Check the bed temperature in the slicer profile. | extras/heaters.py Heater.set_temp |

### 527 CAVITY

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 527-0 | 0 | 3, firmware shutdown | Reading the chamber (cavity) temperature sensor | The chamber temperature reading is out of range: sensor disconnected or damaged. | Power off, check the chamber sensor cable; contact support if it repeats. | extras/error_mcu.py _check_mcu_shutdown |

### 528 HOMING

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 528-0 | 0 | 2, pause | Homing Z | Z homing was asked for before X and Y were homed. | Home all axes. | extras/homing_xyz_override.py _z_probe_pre_process, cmd_G28_BASE |
| 528-1 | 0 | 2, pause | Homing Z, checking which toolhead is on the carriage | The toolhead docked/picked state is wrong, so Z homing was stopped. | Make sure exactly one toolhead is on the carriage and the rest are docked, then home. | extras/homing_xyz_override.py _z_probe_pre_process |
| 528-2 | endstop: 0 X, 1 Y, 2 Z, 3 probe, 255 other | 3, cancel | Homing move | An error happened during the homing move (the message holds the underlying error). | Check that the axis moves freely, then home again. | extras/homing.py HomingMove.homing_move |
| 528-3 | endstop: 0 X, 1 Y, 2 Z, 3 probe, 255 other | 3, cancel | Waiting for an endstop or the probe | An error happened while waiting for the endstop or probe to trigger. | Home again; if it repeats, restart the printer. | extras/homing.py HomingMove.homing_move |
| 528-4 | endstop: 0 X, 1 Y, 2 Z, 3 probe, 255 other | 3, cancel | Homing or probing | The endstop or probe never triggered across the full travel. For the probe (index 3), the nozzle never touched or sensed the bed. | Check that the build plate is in place and nothing blocks the axis; clean the nozzle tip; home again. | extras/homing.py HomingMove.homing_move |
| 528-5 | 0 | 3, cancel | Second homing pass | The endstop was still triggered after backing off. | Check that the axis is not blocked and the endstop is not stuck; home again. | extras/homing.py Homing.home_rails |
| 528-6 | 0 | 3, cancel | Repeated homing passes | Homing results varied more than allowed between passes. | Check the belts and rails for debris or looseness; home again. | extras/homing.py Homing.home_rails |
| 528-7 | 0 | 3 | Homing | Homing failed because the printer shut down during it. | Fix the shutdown reason shown with it, restart firmware. | extras/homing.py PrinterHoming.manual_home, cmd_G28 |
| 528-8 | 0 | 3 | Probing | Probing failed because the printer shut down during it. | Fix the shutdown reason, restart firmware. | extras/homing.py PrinterHoming.probing_move, probing_coil_move |
| 528-9 | 0 | 3 | Probing | The probe was already triggered before the move started. | Clean the nozzle and the plate, check nothing is touching the nozzle, retry. | extras/homing.py PrinterHoming.probing_move |
| 528-10 | 0 | 2, pause | Precise XY homing | X and Y must be homed before precise homing. | Home all axes. | extras/homing_precise_corexy.py check_xy_is_homing |
| 528-11 | 0 | 2, pause (retried automatically first) | Precise XY homing | Precise homing computed a position too far from the expected one. | Home again; check belts and the calibration target for debris. If it repeats, run the home-origin calibration. | extras/homing_precise_corexy.py precise_corexy_coord |
| 528-12 | 0 | 2, pause | Precise XY homing | The diagonal probing used for precise homing failed. | Clean the nozzle and calibration target, home again. | extras/homing_precise_corexy.py precise_corexy_coord |
| 528-13 | the detached toolhead | 2, pause | Homing Z | A toolhead reads as detached (FFF). | Seat that toolhead in its dock, then home. | extras/homing_xyz_override.py _z_probe_pre_process |
| 528-14 | the affected toolhead | 2, pause | Homing Z | A toolhead reads as both docked and picked up (TTx). | Re-seat that toolhead, then home. | extras/homing_xyz_override.py _z_probe_pre_process |

### 529 GCODE

These come from G-code parsing; during a print they end the print unless noted.

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 529-0 | 0 | 3, cancel | Reading a command's parameters | A command is missing a required value. | Re-slice the file, or fix the macro or command you sent. | gcode.py GCodeCommand.get |
| 529-1 | 0 | 3, cancel | Reading a command's parameters | A command value could not be read as a number. | Same as 529-0. | gcode.py GCodeCommand.get |
| 529-2 | 0 | 3, cancel | Reading a command's parameters | A value is below its allowed minimum. | Same as 529-0. | gcode.py GCodeCommand.get |
| 529-3 | 0 | 3, cancel | Reading a command's parameters | A value is above its allowed maximum. | Same as 529-0. | gcode.py GCodeCommand.get |
| 529-4 | 0 | 3, cancel | Reading a command's parameters | A value must be above a limit and is not. | Same as 529-0. | gcode.py GCodeCommand.get |
| 529-5 | 0 | 3, cancel | Reading a command's parameters | A value must be below a limit and is not. | Same as 529-0. | gcode.py GCodeCommand.get |
| 529-6 | 0 | 3, printer will not start | Loading the configuration | Two config sections define the same command. | Remove the duplicate macro from your config, restart firmware. | gcode.py GCodeDispatch.register_command |
| 529-7 | 0 | 3, printer will not start | Loading the configuration | Two config sections register the same multi-target command in conflicting ways. | Remove the duplicate config section, restart firmware. | gcode.py GCodeDispatch.register_mux_command |
| 529-8 | 0 | 3, cancel | Parsing a command | A command line is malformed. | Re-slice, or fix the command or macro. | gcode.py GCodeDispatch._get_extended_params |
| 529-9 | 0 | 1 reported, but a print would still end (action defaults to cancel) | Running a command that targets a named device | The command names a device (sensor, toolhead) that does not exist. | Fix the device name in the command or macro. | gcode.py GCodeDispatch._cmd_mux |
| 529-10 | 0 | 3, cancel | Arc move (G2/G3) | Arc moves in relative mode are not supported. | Turn off arc fitting in the slicer, or use absolute mode. | extras/gcode_arcs.py ArcSupport._cmd_inner |
| 529-11 | 0 | 3, cancel | Arc move | Arc moves with a radius (R) are not supported. | Turn off arc fitting in the slicer. | extras/gcode_arcs.py ArcSupport._cmd_inner |
| 529-12 | 0 | 3, cancel | Arc move | An arc move is missing its center values. | Turn off arc fitting in the slicer, or re-slice. | extras/gcode_arcs.py ArcSupport._cmd_inner |
| 529-13 | 0 | 3, cancel | Move (G0/G1) | A move has an invalid speed (zero or negative). | Re-slice; check the speed in custom G-code. | extras/gcode_move.py GCodeMove.cmd_G1 |
| 529-14 | 0 | 3, cancel | Move (G0/G1) | A move command could not be read. | Re-slice; check custom G-code. | extras/gcode_move.py GCodeMove.cmd_G1 |
| 529-15 | 0 | 3, cancel | G20 | Inch units are not supported. | Slice in millimeters. | extras/gcode_move.py GCodeMove.cmd_G20 |

### 530 PROBE_OR_CALIBRATION

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 530-0 | toolhead | 3, cancel | Checking the nozzle sensor (inductance coil) before probing or offset calibration | The toolhead's nozzle sensor gives no reading at all. | Re-seat the toolhead and clean its pogo pins; restart. If it repeats, the sensor needs service. | extras/probe_inductance_coil.py _probe, cmd_PROBE_XYZ_OFFSET_CALIBRATE |
| 530-1 | toolhead | 3, cancel | Same as 530-0 | The nozzle sensor reading is outside its normal range. | Clean the nozzle tip and the plate, re-seat the toolhead, retry. | same |
| 530-2 | 0 | 3, cancel | Probing | Probing was asked for before the printer was homed. | Home all axes, then retry. | extras/probe_inductance_coil.py _probe, cmd_PROBE_XYZ_OFFSET_CALIBRATE |
| 530-3 | 0 | 3, cancel | XYZ offset calibration | Invalid number of probe points requested (command error). | Start the calibration again from the screen. | extras/probe_inductance_coil.py cmd_PROBE_XYZ_OFFSET_CALIBRATE |
| 530-4 | 0 | 3, cancel | Probing, checking toolhead state | The toolhead docked/picked state is wrong, so probing stopped. | Make sure one toolhead is on the carriage and the rest are docked, home, retry. | extras/probe_inductance_coil.py _probe |
| 530-5 | 0 | 3, cancel | Probing | The toolhead on the carriage is not the one the firmware expected. | Home again so the firmware re-detects the toolhead, then retry. | extras/probe_inductance_coil.py _probe |
| 530-6 | 0 | 3, cancel | Probing | The probe in use does not belong to the active toolhead. | Home again; restart the printer if it repeats. | extras/probe_inductance_coil.py _probe |
| 530-7 | 0 | 3, cancel | Probing | Unsupported probe axis (command error). | Unverified for users; the source shows only a bad probe-axis parameter. Retry from the screen. | extras/probe_inductance_coil.py _probe |
| 530-8 | probe axis: 0 X, 1 Y, 2 Z (not a toolhead) | 3, cancel | Probing several times | Repeated probe readings disagreed more than allowed. | Clean the nozzle tip and the plate, make sure the plate sits flat, retry. | extras/probe_inductance_coil.py run_probe |
| 530-9 | 0 | 3, cancel | XYZ offset calibration steps | A calibration step was run while the printer was not in offset-calibration mode; the offset calibration also reports other step failures (a heater not heating) under this code, so the message is the reason. | Start the offset calibration again from the screen. | extras/probe_inductance_coil.py _verify_calibration_state |
| 530-10 | 0 | 3, cancel | Plate detection during calibration | The build plate was expected to be on the bed but reads as removed. | Put the build plate back on, aligned, and retry. | extras/probe_inductance_coil.py cmd_DETECT_PLATE |
| 530-11 | 0 | 3, cancel | Plate detection during calibration | The build plate was expected to be removed but is still on the bed. | Remove the build plate when asked, then continue. | extras/probe_inductance_coil.py cmd_DETECT_PLATE |
| 530-13 | 0 | 3, cancel | Probing | No toolhead is on the carriage; all are docked. | Home again so a toolhead is picked; retry. | extras/probe_inductance_coil.py _probe |
| 530-14 | the detached toolhead | 3, cancel | Probing | A toolhead reads as detached (FFF). | Seat that toolhead in its dock, home, retry. | extras/probe_inductance_coil.py _probe |
| 530-15 | the affected toolhead | 3, cancel | Probing | That toolhead's pogo pins are not making contact (FFT). | Clean the pogo pins on the head and carriage, re-seat, retry. | extras/probe_inductance_coil.py _probe |
| 530-16 | the affected toolhead | 3, cancel | Probing | A toolhead reads as both docked and picked up (TTx). | Re-seat that toolhead, retry. | extras/probe_inductance_coil.py _probe |
| 530-17 | 0 | 3, cancel | Bed contact probe | The probe never touched the plate: the PEI plate is not placed correctly. | Seat the PEI plate flat and aligned on the bed, retry. | extras/probe_inductance_coil.py cmd_PROBE_BED_CONTACT |
| 530-18 | 0 | 3 | Saving toolhead offset calibration results (factory mode only) | The measured offset between toolheads is out of the allowed range. | Re-seat all toolheads and rerun the offset calibration. Contact support if it repeats. | extras/probe_inductance_coil.py cmd_EXTRUDER_OFFSET_ACTION_SAVE_RESULT |
| 530-19 | 0 | 3, cancel | Park (dock) position calibration | No dock position is configured for that toolhead. | Unverified for users; points to a missing factory calibration. Contact support. | kinematics/extruder.py cmd_ENTER_PARK_POINT_MANUAL_CALIBRATION, cmd_MOVE_TO_PARK_CALIBRATION_POINT, cmd_VERIFY_PARK_POSITION |
| 530-20 | 0 | 3, cancel | Park position calibration | All toolheads must be docked first. | Dock every toolhead (home the printer), then retry. | kinematics/extruder.py cmd_MOVE_TO_PARK_CALIBRATION_POINT, cmd_VERIFY_PARK_POSITION |
| 530-21 | toolhead | 3, cancel | Verifying the dock position: test pick | Picking the toolhead failed during dock-position verification. | Adjust the dock position again and rerun the verification. | kinematics/extruder.py cmd_VERIFY_PARK_POSITION |
| 530-22 | toolhead | 3, cancel | Verifying the dock position: test park | Parking the toolhead failed during verification. | Same as 530-21. | kinematics/extruder.py cmd_VERIFY_PARK_POSITION |
| 530-23 | 0 | 3, cancel | Changing or verifying a dock position | The new dock position is too far from the current one. | Make a smaller adjustment, or confirm with the force option if you are sure. | kinematics/extruder.py cmd_SET_EXTRUDER_PARK_POSITION, cmd_MOVE_TO_PARK_CALIBRATION_POINT, cmd_VERIFY_PARK_POSITION |
| 530-24 | 0 | 3, cancel | Home-origin calibration (diagonal probing) | A calibration probe point failed. | Clean the nozzle and calibration target, rerun the calibration. | extras/homing_precise_corexy.py perform_multi_point_diagonal_probing |
| 530-25 | 0 | 3, cancel | Home-origin calibration | A calibration point landed in the wrong place. | Check belts and rails for debris or looseness, rerun. | same |
| 530-26 | 0 | 3, cancel | Home-origin calibration | Points stayed unstable after all retries. | Same as 530-25. | same |
| 530-27 | 0 | 3, cancel (retried first by the advanced homing command) | Precise homing | The home position is unstable. | Rerun the home-origin calibration; check belts. | extras/homing_precise_corexy.py precise_corexy_coord |
| 530-28 | 0 | 3, cancel (retried first) | Precise homing validation | The validation point does not match. | Same as 530-27. | extras/homing_precise_corexy.py precise_corexy_coord |
| 530-29 | 0 | 1, info, kept until cleared | After a bed scan | The bed is too far out of level for automatic compensation; manual leveling is needed. | Level the bed with the manual leveling procedure, then clear the warning. | extras/bed_mesh.py check_manual_leveling_needed |

### 531 PRINT_FILE

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 531-0 | 0 | 1, info (action none) | Listing print files | The file list could not be read from storage. | Restart the printer; if it repeats, check the storage for space. | extras/virtual_sdcard.py get_file_list |
| 531-1 | 0 | 1, info (action none) | Starting, resuming or seeking a file print | A print is already running. | Wait for it to finish, or cancel it first. | extras/virtual_sdcard.py do_resume, cmd_SDCARD_PRINT_FILE, cmd_SDCARD_PRINT_FILE_WITH_PARAMETERS, cmd_M23, cmd_M26 |
| 531-2 | 0 | 1, info | Writing to the print storage | Writing files this way is not supported. | Upload files from the slicer or app instead. | extras/virtual_sdcard.py cmd_error |
| 531-3 | 0 | 1, info | Reset file command | That command cannot run from inside a print file. | Remove it from the G-code file. | extras/virtual_sdcard.py cmd_SDCARD_RESET_FILE |
| 531-4 | 0 | 1, info | Opening a print file | The file could not be opened (missing, renamed, or corrupt). | Re-upload the file and start it again. | extras/virtual_sdcard.py _load_file |
| 531-5 | 0 | 1, info | Power-loss recovery | Recovery is not allowed in the current state, or restoring the print failed (the message says which). | Return to the idle screen and try recovery again; if it fails, start the print fresh. | extras/virtual_sdcard.py cmd_SDCARD_PRINT_PL_RESTORE, restore_print |
| 531-6 | 0 | 1, info | Clearing power-loss data | Power-loss data cannot be deleted while printing. | Wait until the print ends. | extras/virtual_sdcard.py cmd_SDCARD_PRINT_PL_CLEAR_ENV |
| 531-10 | 0 | 3, print stops | Reading the print file during a print | The file could not be read part way through. | Re-upload the file; if it repeats, the storage may be failing, contact support. | extras/virtual_sdcard.py work_handler |
| 531-14 | 0 | 3, cancel (print does not start) | Checking print settings at start | The file was sliced for a different nozzle size than the one installed on a toolhead it uses. | Re-slice with the installed nozzle size, or fit the matching nozzle. | extras/print_task_config.py cmd_SET_PRINT_TASK_PARAMETERS |
| 531-15 | 0 | 1, info | Changing the toolhead mapping | Toolhead mapping cannot change during a print. | Change it before starting the print. | extras/print_task_config.py cmd_SET_PRINT_EXTRUDER_MAP |
| 531-16 | 0 | 1 from preferences / used-toolhead changes; 3 from task parameters | Changing print settings | Those print settings cannot change during a print. | Change them before starting the print. | extras/print_task_config.py cmd_SET_PRINT_PREFERENCES, cmd_SET_PRINT_USED_EXTRUDERS, cmd_SET_PRINT_TASK_PARAMETERS |
| 531-17 | 0 | 3, cancel (print does not start) | Reading print settings at start | The print settings sent with the file could not be applied (generic). | Re-slice and send the print again. | extras/print_task_config.py cmd_SET_PRINT_TASK_PARAMETERS |
| 531-18 | 0 | 3, cancel (print does not start) | Checking print settings at start | Flow calibration is turned on for a filament and nozzle pair that does not allow it (0.2 mm nozzle with PA, PC, fiber-filled and similar). | Turn off flow calibration for this print, or use a larger nozzle. | extras/print_task_config.py cmd_SET_PRINT_TASK_PARAMETERS |
| 531-19 | 0 | 3, cancel (print does not start) | Checking print settings at start | The file was sliced for a different nozzle type (standard or high-flow) than the one installed. | Re-slice with the installed nozzle type, or set the toolhead's nozzle type correctly on the screen. | extras/print_task_config.py cmd_SET_PRINT_TASK_PARAMETERS |
| 531-21 | 0 | 3, print does not start | Pre-start check | Filament auto-feeding is still in progress. | Wait for feeding to finish, then start the print. | extras/virtual_sdcard.py cmd_SDCARD_PRINT_FILE, cmd_SDCARD_PRINT_FILE_WITH_PARAMETERS |
| 531-23 | 0 | 3, print does not start | Pre-start check | The pre-print check failed; the message carries the underlying reason. | Read the message and fix that cause, then start again. | same |

### 532 DEFECT_DETECTION

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 532-1 | 0 | 2, pause | Checking the bed before or during a print | Something is on the build plate (dirty or occupied bed). | Clear and clean the build plate, then resume. | extras/defect_detection.py defect_event_handler (via cmd_DEFECT_DETECTION_DETECT_BED) |
| 532-2 | 0 | 2, pause | Camera monitoring during a print | "Spaghetti": the print has come loose or is extruding into the air. | Check the print. Resume only if it looks fine; otherwise cancel, clean the plate and reprint. | extras/defect_detection.py defect_event_handler |
| 532-3 | 0 | 2, pause | Camera monitoring during a print | Leftover material detected where it should not be (residue). | Check the bed and print; remove debris, then resume. | extras/defect_detection.py defect_event_handler |
| 532-4 | 0 | 2, pause (plain PAUSE, not an error pause) | Camera check of the nozzle | Filament is stuck on the nozzle (dirty nozzle). | Clean the nozzle, then resume. | extras/defect_detection.py defect_event_handler (via cmd_DEFECT_DETECTION_DETECT_NOZZLE); special-cased in virtual_sdcard.work_handler |

### 533 PURIFIER

| Code (id-code) | Index means | Level / effect | What the firmware was doing | Cause in plain words for a printer user | Fix for the user | Source (file, function) |
|---|---|---|---|---|---|---|
| 533-0 | 0 | 2, pause | Chamber preheat or hot-chamber mode | The chamber's inner circulation fan is switched on but reads 0 RPM (stuck or unplugged). | Check the fan for obstructions and its cable, then resume. | extras/purifier.py _handle_hot_chamber_mode, _handle_preheat_chamber_mode |
| 533-1 | 0 | 2, pause | Chamber cooling mode | The chamber stayed too hot and did not cool in time. | Open the door or lid to let it cool, check the exhaust fan works, then resume. | extras/purifier.py _trigger_cooling_alarm |

### 2052 SYSTEM (moonraker)

No raise site in this source copy. Moonraker `<moonraker>/moonraker/components/exception_manager.py` declares `MUDOLE_ID_SYSTEM = 2052` and exposes `/server/exception/raise`, `/server/exception/clear` and `/server/exception/query` (not over HTTP), so system codes come from an outside client (the screen app or the MQTT side), not from Klipper or Moonraker code in this copy. Its constants `CODE_MOTION_COMMON = 0`, `CODE_MOTION_DISCONNECTED = 1`, `CODE_MOTION_SHUTDOWN = 2` are used only in commented-out code.

### Open questions in the map

- **2052 SYSTEM:** never raised in this copy; the codes come from an external client through Moonraker's `/server/exception/raise`. The source that sends them is not in this tree.
- **526 and 527 are raised**, but only from `<klipper>/klippy/error_mcu.py` (ADC out of range), `<klipper>/klippy/verify_heater.py` and `<klipper>/klippy/heaters.py`. 527 has a single code (sensor out of range); nothing raises a chamber over-temperature as 527 (that is 533-1).
- **Codes that never occur:** 522-1 (the Moonraker "disconnected" constant, commented out), 530-12, 531-7 to 9, 11 to 13, 20, 22, 532-0 (the handler's default; every caller passes events 1 to 4), 523-6 from the switch logic (it would need an impossible multi-pick combination; 523-6 in practice is only "toolhead not configured").
- **523-6's index is always 0**, even for toolheads 2 to 4 (`_set_extruder_temp` hardcodes it). The head is only in the message text.
- **522-17 board numbering differs by source:** `<klipper>/klippy/serialhdl.py` uses 0 = main board, 1-4 = e0-e3, while `<klipper>/klippy/mcu.py`/`<klipper>/klippy/klippy.py` (522-6, 8, 16) use 0 = host, 1 = main board, 2-5 = e0-e3. A UI that names the board from the index must branch on the code.
- **Malformed fallback code:** `<klipper>/klippy/klippy.py` `invoke_shutdown`, when a shutdown message is JSON without a `coded` key, builds `f"{level:04d}-{id:04d}-{code:04d}-{level:04d}"` (level-id-code-level). No caller in this tree sends such JSON, so the path appears unused, but if it fires the reported code is wrong.
- **Space-padded fields:** `%4d` formats in `<klipper>/klippy/extruder.py` (523-18 to 47, 523-7 to 17), `<klipper>/klippy/homing.py` (528-2 to 4), `<klipper>/klippy/homing_xyz_override.py` (528-13/14), `<klipper>/klippy/probe_inductance_coil.py` (530-0/1, 8, 14 to 16), `<klipper>/klippy/extruder.py` (530-21/22) and `<klipper>/klippy/serialhdl.py` (522-17) put spaces in the raw message string. The reported integers are fine.
- **Level versus effect:** level 1 codes raised as command errors without `"action": "none"` (523-6, 529-9) still end a running print, because the action defaults to cancel. The 531 informational codes with `"action": "none"` do not.
- **Feeder errors that hide behind other codes:** a load/unload failure during homing, toolhead switching, moving or heating re-raises the original error instead of a 525 code, so a UI wanting "load failed" wording must also expect 528, 523 and heater codes during a feed.
- **MCU firmware:** the MCU only sends shutdown reason strings. The host maps "Timer too close" to 522-16 and "ADC out of range" to 523-2 / 526-0 / 527-0 / 522-14; every other MCU shutdown reason ("MCU shutdown" with any other reason, spontaneous restart) becomes 522-2, with the reason only in the message text. `<klipper>/klippy/error_mcu.py` adds help text, not codes.
- **Uncoded errors:** every plain `gcmd.error("...")` across the codebase (many in filament_feed, extruder, print_task_config) reports as 522-0. Errors raised with `action='pause'` but no code (for example `<klipper>/klippy/extruder.py` "Extruder malfunction after parking ..." with a None state, `cmd_SWITCH_EXTRUDER` retry exhaustion when the message is not JSON) report 522-0 at level 3 yet pause the print.
- **Unverified meanings:** 530-7 (only a bad parameter is visible), 530-19 (a missing dock position calibration; whether users can ever hit it is unclear), 525-30 (a generic "load failed" with no specific trigger in the source beyond unclassified exceptions such as the drive step), 532-1 bed check details (the source runs probe moves; the camera's part is in the remote analysis service, not here).

## 480x320 Display Considerations

The U1's 480x320 display uses the TINY layout preset. This is the smallest resolution HelixScreen supports, and several UI panels have known layout issues at this size. Key issues:

- **Navbar icons clipped** at screen edges
- **Controls panel** labels overlapping, z-offset value wrapping
- **Print select list view** fundamentally broken at this size
- **Numeric keypad overlay** too tall, bottom rows cut off
- **Filament panel** cards pushed off-screen

These are resolution-specific issues, not Snapmaker-specific. Any 480x320 device benefits from the same fixes.

## Known Limitations

- **480x320 UI needs work** -- Multiple panels have layout issues at this resolution (see above).
- **SSH access required** -- deployment needs SSH. Stock firmware (1.2+) provides it via the **Root access** option / debug mode; the community [Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) enables it by default. Either works — PAXX is **not** required. (Stock boot-time autostart is newly added and pending verification on stock hardware; see [Firmware Requirements](#firmware-requirements).)
- **Auto-start requires `/oem/.debug`** -- The overlay filesystem is wiped on boot unless `/oem/.debug` exists. This flag must be created once during installation to persist the boot-launcher hook (see [How HelixScreen takes over the display](#how-helixscreen-takes-over-the-display)).
- **WiFi management** -- Stopping `unisrv` (stock UI) does not affect WiFi — the U1 uses standard `wpa_supplicant` managed by the OS. HelixScreen has its own WiFi manager with `wpa_supplicant` support.
- **Remote screen ("gui" webcam)** -- Wired up via the U1 platform hook (`assets/config/platform/hooks-snapmaker-u1.sh`): `start_remote_screen` launches the firmware's `fb-http` tool on every HelixScreen start (so it survives the overlay boot-glob trap for free — the hook already runs on every boot), gated on the PAXX `web remote_screen` toggle. **The working path is fbdev + the in-app fb0 mailbox mirror.** Every shipping `fb-http` (on-device build, mainline `paxx12/screen-apps`, and the exact commit the Extended-Firmware submodule pins) reads `/dev/fb0` and exposes only `--port/--bind/--fb/--touch/--html-dir`; **no `fb-http` with a DRM backend exists in any public/shipping source** (verified 2026-07-09). HelixScreen renders into its own DRM dumb buffer and never touches `/dev/fb0`, so on its own the fbdev daemon would serve a stale frame — the hook therefore exports `HELIX_REMOTE_SCREEN_FB0=/dev/fb0` on the fbdev path, and HelixScreen mirrors each rendered frame into `/dev/fb0` (`RemoteScreenManager`/`Fb0MailboxSink`, tapped from the flush hook), so `fb-http`'s snapshot is the live UI. Hardware-verified 2026-07-09: fb0 is decoupled from the DRM scanout (writing to it does not disturb the panel), BGRA maps straight through (plain memcpy), and the served snapshot matched what was written. The hook still **probes** the tool (`_remote_screen_backend_args`) and would prefer a `--backend drm` capture path *if* a DRM-capable `fb-http` ever ships (no mirror needed then) — but that is dormant/forward-looking today. nginx `/screen/` route (`/etc/nginx/fluidd.d/remote-screen.conf`) and the `[webcam gui]` Moonraker slot are provided by the firmware — but the `[webcam gui]` section ships **commented out** in `extended/moonraker/04_remote_screen.cfg`. Setting `web remote_screen` in `extended2.cfg` alone only gates fb-http startup; the firmware-config web UI toggle (/usr/local/share/firmware-config/functions/12_settings_remote_screen.yaml) is what uncomments `[webcam gui]`, sets `enabled: true`, and restarts `S99fb-http` (= HelixScreen, via the hijacked delegate) plus Moonraker. Enabling by hand requires replicating all of that (`extended-config.py add/uncomment/update` + both restarts).
- **Two benign Moonraker warnings on buildroot** -- After install, Mainsail/Fluidd surface *"Unable to find DBus PolKit Interface"* (Moonraker's always-loaded `machine` component on a no-PolKit OS) and *"Unable to initialize System Update Provider for distribution: buildroot"* (Moonraker's `update_manager` initializes a system/OS package provider by default, which can't detect apt/PackageKit on buildroot). The PolKit one is not ours — it's inherent to Moonraker on buildroot and re-surfaces because our installer restarts Moonraker. The system-update one appears whenever Moonraker loads the `update_manager` component without `enable_system_updates: False`. Stock U1 Moonraker loads no `update_manager` at all (`/machine/update/status` returns 404), so nothing triggers it until the installer runs. The U1's Moonraker still ships the old zip updater module (no `asset_name` support, #993), so `configure_moonraker_updates` never writes `[update_manager helixscreen]` there: it removes a stale one if present, then `disable_system_updates_on_buildroot` adds a bare `[update_manager]` with `enable_system_updates: False` (stock: creates the section; where one exists, merges the key into it; a PAXX rig ends up with exactly that and no helixscreen section). In-app updates are HelixScreen's own updater. Both are harmless. Same condition applies to K1/K2 (also buildroot).

## Future Work

### Remote screen / "gui" webcam streaming

**Implemented** as **fbdev + the "fb0 mailbox" mirror**, via the U1 platform hook's `start_remote_screen`/`stop_remote_screen` plus an in-app frame mirror (see the Known Issues entry above). The hook launches the firmware's `fb-http` (fbdev, reading `/dev/fb0`) and exports `HELIX_REMOTE_SCREEN_FB0=/dev/fb0`; HelixScreen then mirrors each rendered frame into `/dev/fb0` so the daemon's snapshot is the live UI. fb0 is a separate buffer decoupled from our DRM scanout (hardware-verified 2026-07-09: writing magenta to fb0 was served by the daemon while the panel stayed on HelixScreen, and helix survived the write). The mirror lives in `RemoteScreenManager`/`Fb0MailboxSink` (`src/application/remote_screen_*.cpp`), fed from the flush hook (`DisplayManager::install_color_transform_hook()`): open `/dev/fb0` once, mmap `MAP_SHARED|PROT_WRITE`, copy each dirty region as **BGRA**, stride 1920 (480×320×4) — a plain per-row memcpy (no byte-swap, no encode), on post-rotation/post-transform pixels. Every dirty rect is mirrored (no artificial fps cap; a static UI produces no writes). No in-process server, no encoder. It rides HelixScreen's existing per-boot start path, so it needs no separate init script (which would fall foul of the overlay boot-glob trap).

**Sink framework (extensible):** the mirror is one `RemoteScreenSink` behind `RemoteScreenManager`, so a future in-process C++ server (#1032) — which would serve snapshots directly and work on every platform, not just the U1 — drops in as an alternative sink without touching the flush-hook tap.

**Forward-looking — DRM capture:** `_remote_screen_backend_args` probes `fb-http` for a `--backend drm` flag and, if present, would drive a DRM capture path that reads `/dev/dri/card0` directly (no mirror needed). **No such `fb-http` exists in any public/shipping source as of 2026-07-09** (verified against the on-device build, `paxx12/screen-apps` `main`, and the pinned Extended-Firmware submodule commit — all fbdev-only), so this path is dormant; the fbdev+mirror path above is what runs today. If a DRM-capable `fb-http` ever ships, the probe lights it up and the mirror stays idle.

**Remote control — working (verified 2026-07-12, #1091).** `fb-http` also injects touch to `/dev/input/event0` via its `/touch` endpoint (nginx `/screen/touch`), and remote *control* works end-to-end for free. HelixScreen opens `event0` through LVGL's evdev driver (`src/platform/display/display_backend_drm.cpp#create_input_pointer`) and does **not** `EVIOCGRAB` it, so it sees `fb-http`'s injected events. `fb-http` defaults its touch device to `/dev/input/event0` (no `--touch` flag needed) and scales browser clicks 1:1 (the TLSC6x reports `ABS_MT` in the same 480×320 space as the framebuffer). Verified on-device (v0.99.88, printer idle): synthetic taps at the Settings gear, Help & About row, and home-nav icon each landed on the intended element in the rendered snapshot — no offset, no inversion. The browser client (/usr/local/share/fb-http/html/index.html) maps clicks to native image coordinates via `getImageCoords` and POSTs `down`/`move`/`up`. PNG snapshot is poll-based, not a continuous MJPEG stream.

> **Rotation caveat.** This works because the U1 auto-probes `"rotate": 0` (no display rotation). fb-http serves the *rendered* (display-space) frame, and LVGL re-rotates every incoming `event0` point via `lv_display_rotate_point` (`lib/lvgl/src/display/lv_display.c`) whenever a rotation is active. At `rotate == 0` that transform is identity, so injected coordinates pass straight through. **At a non-zero rotation, injected taps would be double-transformed (point-symmetric-inverted)** — fb-http can't know HelixScreen's rotation, and we don't own it. The rotation-robust path is the in-process touch injection tracked by **#1032** (inject past the evdev round-trip, after rotation), not the fb-http path. The U1's panel is fixed-mount and probes to 0 in practice, so this is a latent limitation, not an active bug.

### Extended Firmware Overlay

Package HelixScreen as an Extended Firmware overlay for one-click installation via paxx12's build system.

### RFID Filament UI

The `AmsBackendSnapmaker` backend parses RFID data from `filament_detect.info` when the RFID reader is enabled. With the RFID reader disabled (default on Extended Firmware via `disable-rfid-reader.cfg`), all RFID fields return `"NONE"` and `print_task_config` is the authoritative filament data source.

### Virtual Toolhead Mapping

The U1 maps 32 logical tools onto 4 physical heads through `extruder_map_table`, and that table
— not the physical toolhead layout — is what decides which head prints a given `Tn`. HelixScreen
both writes it (the pre-print `SET_PRINT_EXTRUDER_MAP` sequence, one line per used tool) and
reads it back (`AmsBackendSnapmaker::get_tool_mapping()`), so the live preview colours each
tool by the toolhead that will actually print it instead of inferring a mapping from the slicer
palette. Two properties make the read safe:

- **Write every used entry, not just the remaps.** `SET_PRINT_EXTRUDER_MAP` sets one entry and
  resets nothing, so emitting only genuine remaps left the rest holding the *previous* print's
  values — a job that remapped `T0`→head 2 made the next job print `T0` from head 2 as well.
- **Only read it while a task is configured.** Idle, the table holds a default identity
  `[0,1,2,3,…]` that is indistinguishable from "this print needs no remap" and is wrong for any
  file whose tools do not line up with the toolheads. `extruders_used` being all-false is the
  firmware's own "no task" signal and gates the read.

Full command and data-model reference: [FILAMENT_BACKEND_SNAPMAKER_U1.md](../FILAMENT_BACKEND_SNAPMAKER_U1.md) § "Firmware API: `print_task_config`".

## Moonraker Object Reference

The U1's Klipper exposes several custom objects. `AmsBackendSnapmaker` subscribes to and parses these during the Moonraker subscription phase.

### `print_task_config` — Authoritative filament data (HANDLED)

The primary source for filament info. Populated by the stock firmware's task manager regardless of RFID reader state.

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `filament_type` | `["PLA","PLA","PLA","PLA"]` | ✅ | Material per feeder |
| `filament_sub_type` | `["SnapSpeed",...]` | ✅ | Appended to type (e.g., "PLA SnapSpeed") |
| `filament_vendor` | `["Snapmaker",...]` | ✅ | Brand per feeder |
| `filament_color_rgba` | `["080A0DFF",...]` | ✅ | RGBA hex → RGB uint32 |
| `filament_exist` | `[true,true,true,true]` | ✅ | Feeder presence |
| `filament_color` | `[4278716941,...]` | — | Redundant with `_rgba`, not parsed |
| `filament_official` | `[true,...]` | ❌ | Could show official/third-party badge |
| `filament_sku` | `[900001,...]` | ❌ | Snapmaker product SKU |
| `filament_soft` | `[false,...]` | ❌ | Soft filament flag (TPU etc.) |
| `filament_edit` | `[false,...]` | ❌ | Whether user has edited filament info |
| `extruder_map_table` | `[0,1,2,3,0,...(x32)]` | ✅ | **Logical tool → physical head routing for the running print.** Published by `AmsBackendSnapmaker::get_tool_mapping()`; the live preview colours each tool by the toolhead that actually prints it. Only read while `extruders_used` says a task is configured — idle it holds a default identity that would read as "no remap". |
| `extruders_used` | `[false,...]` | ✅ | Which heads the current task uses. Gates whether `extruder_map_table` may be read at all (all-false = no task configured). |
| `extruders_replenished` | `[0,1,2,3]` | ❌ | Auto-replenish mapping |
| `auto_replenish_filament` | `true` | ❌ | Auto-replenish enabled |
| `filament_entangle_detect` | `false` | ❌ | Entangle detection enabled |
| `filament_entangle_sen` | `"medium"` | ❌ | Sensitivity level |
| `flow_calibrate` | `false` | ❌ | Flow calibration pre-print option |
| `shaper_calibrate` | `false` | ❌ | Stored by the firmware but read by nothing on the printer. Not offered as a pre-print option, never sent (`FILAMENT_BACKEND_SNAPMAKER_U1.md`, `SET_PRINT_PREFERENCES`) |
| `time_lapse_camera` | `false` | ❌ | Timelapse pre-print option |
| `auto_bed_leveling` | `false` | ❌ | ABL pre-print option |

### `filament_detect` — RFID tag data (HANDLED)

Per-channel RFID tag reads. All fields return `"NONE"`/`0` when the RFID reader is disabled.

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `state` | `[1,1,1,1]` | ✅ | 1=filament present, 0=empty |
| `info[].MAIN_TYPE` | `"NONE"` or `"PLA"` | ✅ | Skipped when `"NONE"` |
| `info[].SUB_TYPE` | `"NONE"` or `"Silk"` | ✅ | Skipped when `"NONE"` |
| `info[].MANUFACTURER` | `"NONE"` or `"Snapmaker"` | ✅ | |
| `info[].VENDOR` | `"NONE"` | ✅ | Fallback for brand |
| `info[].ARGB_COLOR` | `4294967295` | ✅ | ARGB → RGB mask |
| `info[].HOTEND_MIN/MAX_TEMP` | `0` | ✅ | |
| `info[].BED_TEMP` | `0` | ✅ | |
| `info[].WEIGHT` | `0` | ✅ | |
| `info[].DIAMETER` | `0` | ❌ | Filament diameter (1.75mm) |
| `info[].LENGTH` | `0` | ❌ | Spool length |
| `info[].DRYING_TEMP/TIME` | `0` | ❌ | Recommended drying params |
| `info[].OFFICIAL` | `false` | ❌ | Official Snapmaker filament |
| `info[].CARD_UID` | `0` | ❌ | RFID tag unique ID |
| `info[].SKU` | `0` | ❌ | Product SKU |
| `config.startup_stay` | `false` | ❌ | Unknown purpose |

### `filament_feed left` / `filament_feed right` — Feed module state (HANDLED)

Per-extruder filament feed state. Left module serves extruder0/1, right serves extruder2/3.

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `extruderN.filament_detected` | `true` | ✅ | Filament presence |
| `extruderN.channel_state` | `"load_finish"` | ✅ | Load/unload progress |
| `extruderN.channel_error` | `"ok"` | ✅ | Error detection |
| `extruderN.channel_error_state` | `"none"` | ❌ | Error sub-state |
| `extruderN.channel_action_state` | `"load_finish"` | ❌ | Redundant with channel_state |
| `extruderN.module_exist` | `true` | ❌ | Feed module physically present |
| `extruderN.disable_auto` | `false` | ❌ | Auto-feed disabled |

Known `channel_state` values: `"idle"`, `"preloading"`, `"loading"`, `"load_finish"`, `"unloading"`

### `machine_state_manager` — Machine state

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `main_state` | `0` | ✅ | Screws-tilt state (8): gated exit, connect-time reconcile |
| `action_code` | `0` | ❌ | Current action code |

Subscribed only when `mmu_type()` reports SnapSwap; the screws-tilt code reads it via `printer.objects.query` regardless.

### `defect_detection` — Print defect detection (NOT SUBSCRIBED)

AI-based print defect detection system. The status object itself is not subscribed (it is queried once per connect for the bed check's state); HelixScreen consumes detection through `print_stats.exception` instead: on a confirmed defect (custom `exception` sub-object, `code: 2` = spaghetti/noodle) the firmware pauses the print itself, and `U1StockSource` reports it to `DetectionManager` (`include/detection_manager.h`) with `already_paused` set. Sources only report: `DetectionManager::response_for()` classifies the event from the settings in Settings > Safety & Alerts (**Spaghetti Detection**, exposed to XML as the `detection_available` subject). **Pause on Detection** is hidden on a U1 (the `detection_pause_applicable` subject is 0): the firmware pauses by itself and no HelixScreen setting can prevent that, so the row only exists for sources whose pause HelixScreen sends. Every confirmed U1 detection therefore shows the response modal (Resume / Abort / Tune / Turn off detection) even under a warn-only policy: a print the firmware already paused cannot be left on a vanishing toast. The first capable start seeds the Spaghetti Detection settings with the defaults (detection on) and they are HelixScreen's from then on. The switch reaches the printer: `DetectionManager::set_detection_enabled()` (the settings toggle and the modal's Turn off detection) always sends `DEFECT_DETECTION_CONFIG NOODLE_ENABLE=0|1` through `U1StockSource::apply_detection_enabled()`, so with detection off the firmware stops pausing prints rather than pausing them silently. After each capability probe, `DetectionManager::reconcile_detection_enabled()` reads `noodle.enable` from a `printer.objects.query` and sends only when it differs from the setting: each send rewrites the module's config file, and enabling a feature mid-print turns the cavity LED on. Capability is gated by probing `printer.objects.list` for `defect_detection` after the WebSocket connects (probing at init would latch false forever).

**Bed clear check (code 1).** Before a print the module runs the camera's `item` detection over the bed (`DEFECT_DETECTION_DETECT_BED`); when it scores above its threshold it raises exception 532 code 1 "detected dirty bed", pauses, and deactivates `virtual_sdcard`, so the print cannot resume. False positives happen on a clear bed. `U1StockSource` reports it as a `DirtyBed` event and `present_detection()` shows the bed check modal (Restart print / Cancel print / Turn off bed check) whatever the Spaghetti Detection settings say, since the print needs a decision either way. Restart takes the same path as the "Print Was Terminated" restart (`helix::ui::restart_print_from_beginning()`): `SDCARD_RESET_FILE` + `CANCEL_PRINT_BASE`, then the file again. The source also answers `explains_current_pause()` for any 532 defect pause, which keeps the runout guidance dialog and the "Filament Runout" pause reason away from it (`include/non_filament_pause.h`). The check is switched with `DEFECT_DETECTION_CONFIG CLEAN_BED_ENABLE=0|1`, which the firmware persists in its own config file; its `get_status()` returns that same stored config, so the **Bed Clear Check** row in Settings > Safety & Alerts reads `clean_bed.enable` from a `printer.objects.query` after each capability probe instead of assuming a default. The `defect_detection/config` webhook only writes: it returns nothing to read back.

| Field | Example | Notes |
|-------|---------|-------|
| `main_enable` | `true` | Master enable |
| `clean_bed.enable` | `true` | Dirty bed detection |
| `noodle.enable` | `true` | Spaghetti/noodle detection |
| `residue.enable` | `false` | Residue detection |
| `nozzle.enable` | `false` | Nozzle clog detection |

### `purifier` — the Top Cover's air purifier

The object exists on every U1; it means something only while the Top Cover accessory is
fitted, which `power_detected` reports (an ADC on the cover connector, debounced, hot-pluggable).
Read by `helix::enclosure_top` (the printer picture) and `helix::chamber_air` (mode row, fan
dials, Filter meter, notices); see `docs/devel/CHAMBER_HEATER.md` § Chamber Air Modes.

Firmware 2.x fields (`<klipper>/klippy/extras/purifier.py`):

| Field | Notes |
|-------|-------|
| `power_detected` | Top Cover fitted |
| `mode` | 0 idle, 1 cool chamber (exhaust), 2 preheat, 3 hot chamber (recirculate) |
| `desired_chamber_temp`, `critical_chamber_temp` | the cool-mode ceiling and its alarm |
| `exhaust_fan`, `inner_fan` | `{speed 0..1, delay s, speed_threshold}` |
| `inner_fan_work_time` | seconds the inner (filter) fan has run in modes 2-3, kept in the firmware's purifier.json |
| `inner_fan_rpm` | tachometer, changes every tick, never subscribed |

Older layouts exist: 1.3 has no `mode` and keeps the work time under `inner_fan.work`; an earlier
one had flat `fan_speed` / `work_time`. Each reading keys on its own field being present.
Commands: `SET_PURIFIER_MODE MODE= [FAN_SPEED=] [DESIRE_TEMP=] [ALARM_TEMP=] [DELAY_OFF=]`,
`SET_PURIFIER FAN=exhaust|inner [SPEED=] [DELAY_OFF=] [WORK=]`, `WAIT_CHAMBER_TEMP`. With the cover
off every non-zero speed or mode is refused and only logged, and removal forces both fans off and
the mode to idle. Extended Firmware overlay 41 republishes the fans as `fan_generic exhaust_fan`
and `fan_generic circulation_fan`.

What the stock screen (`/usr/bin/gui`, 2.0.0.205) does, and HelixScreen matches: a Top Cover card
with the current mode and two fan sliders (a 10 % floor on the fan the mode drives, a confirmation
below its threshold, a refusal for the fan the mode keeps off); the filter's run hours with a
reset; once per install, a "new top cover detected" guide followed by an offer to recalibrate
vibration compensation (`SHAPER_CALIBRATE`); a warning every time the cover comes off; and a
confirmation before resuming in cool mode with the chamber above its alarm. It sends no chamber
command at print start: `SET_PURIFIER_MODE` comes from the sliced gcode.

### `filament_entangle_detect` — Tangle detection (NOT SUBSCRIBED)

Per-extruder filament tangle detection. Not subscribed.

| Field | Example | Notes |
|-------|---------|-------|
| `detect_factor` | `1.0` | Tangle confidence (0-1, lower=tangled) |

## Verified Hardware

HelixScreen has been tested on a Snapmaker U1 with Extended Firmware. Confirmed working:

- DRM display at 480x320 via rockchipdrmfb with double-buffered page flipping
- DRM CRTC keepalive works — gui killed cleanly, no SIGSTOP hack needed
- Touch input via TLSC6x capacitive controller (no calibration needed)
- Backlight control via sysfs
- Stock UI stops and restarts cleanly via init script hooks
- SSH session survives stopping gui (WiFi unaffected)
- First-run wizard displays correctly at TINY breakpoint
- Memory monitor reports 961MB total with appropriate thresholds
- Persistent deployment on `/userdata/` survives reboots

## Community Testing

We welcome additional testers with Snapmaker U1 hardware — **especially anyone running STOCK firmware** (with Root access enabled), since stock boot-time autostart is implemented but not yet verified on a stock device:

1. Enable SSH — stock firmware: turn on the **Root access** option in printer settings; or install the [Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) (SSH on by default, then `curl -X POST http://<ip>/firmware-config/api/settings/ssh/true` if needed)
2. Install via one-liner (easiest): `curl -sSL https://releases.helixscreen.org/install.sh | sh`
3. **Stock testers:** after install, **reboot** and confirm HelixScreen comes back up on its own (this exercises the `S99input-event-daemon` boot hook) — please report success/failure.
   - Or build from source: `make snapmaker-u1-docker` then `make deploy-snapmaker-u1-fg SNAPMAKER_U1_HOST=<ip>`
4. Report: Does the wizard appear? Does touch work? Can you connect to Moonraker? Do tool changes work?
5. Report in a [new issue](https://github.com/prestonbrown/helixscreen/issues/new), stating the firmware (stock or PAXX) and its version

## Related Resources

- **[Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware)** -- Adds SSH access and community features to the U1
- **[U1 Config Example](https://github.com/JNP-1/Snapmaker-U1-Config)** -- Community reverse-engineered Klipper configuration
- **[Snapmaker Forum](https://forum.snapmaker.com/c/snapmaker-products/87)** -- Official U1 discussion
- **[LAN Client Authorization](../LAN_CLIENT_AUTHORIZATION.md)** -- How Snapmaker Orca and the Snapmaker App pair with the printer, and why the prompt is HelixScreen's job once it replaces the stock screen
- **[U1 Research](../printer-research/SNAPMAKER_U1_RESEARCH.md)** -- Toolhead layout, published firmware sources, on-device layout, PAXX extended firmware
- **[Snapmaker/u1-klipper](https://github.com/Snapmaker/u1-klipper)** -- Open source Klipper fork
- **[Snapmaker/u1-moonraker](https://github.com/Snapmaker/u1-moonraker)** -- Open source Moonraker fork
- **[Snapmaker/u1-fluidd](https://github.com/Snapmaker/u1-fluidd)** -- Open source Fluidd fork
- **[paxx12/u1-firmware-tools](https://github.com/paxx12/u1-firmware-tools)** -- Firmware unpack/repack tools
