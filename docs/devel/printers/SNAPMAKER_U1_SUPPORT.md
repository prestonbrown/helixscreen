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

The U1 does **not** use the standard [viesturz/klipper-toolchanger](https://github.com/viesturz/klipper-toolchanger) module. Instead, it uses native multi-extruder with custom Klipper extensions. Extruders are named `extruder`, `extruder1`, `extruder2`, `extruder3` with custom state fields (`park_pin`, `active_pin`, `activating_move`, `state`). HelixScreen has a dedicated `AmsBackendSnapmaker` that tracks tool state, RFID filament data, and supports tool switching via `T0`–`T3` gcodes.

## Firmware Requirements

HelixScreen needs exactly one thing from the firmware: **SSH access** (to deploy and to install its boot hook). Both stock and community firmware can provide it — HelixScreen does **not** require PAXX.

- **Stock Snapmaker firmware (1.2+)** ships the `dropbear` SSH server. It is disabled by default and gated behind the printer's root/developer-access mode: `/etc/init.d/S50dropbear` exits early unless `custom_misc vertype` reports `dbg`. Snapmaker added a user-facing **Root access** option in firmware **V1.2.0**; enabling it (or flashing debug mode via `custom_misc gen-debug`, which persists across upgrades) starts dropbear with the standard `root` / `lava` accounts, password `snapmaker`. That is the exact SSH path HelixScreen uses — **stock firmware is supported.**
- **PAXX Extended Firmware** is *repackaged stock firmware* plus an overlay of patches. It removes the dropbear debug-mode gate (SSH on by default) and bundles extras HelixScreen does not use — Tailscale, OctoEverywhere, WebRTC camera, a `/firmware-config/` web UI, RFID filament write-back, etc. It is the turnkey option if you'd rather not enable stock root access yourself, but it is **not** a HelixScreen requirement.

| Firmware | SSH | Status | Boot launcher HelixScreen hooks |
|----------|-----|--------|----------------------------------|
| Stock 1.2+ | dropbear, enabled via Root access / debug mode | **Supported** (autostart newly added, see caveat) | No display init script exists; HelixScreen hooks `S99input-event-daemon` (the only boot-glob launcher present on stock). |
| PAXX 1.2 / 1.3 | on by default | **Tested** | `/etc/init.d/S99screen` (launches stock UI `/usr/bin/gui`). |
| PAXX 1.4 | on by default | **Tested + hardware-validated** | `S99screen` removed; HelixScreen hooks `S99fb-http`. |

> **Stock-firmware caveat:** deploy, SSH, WiFi-credential reuse, and display takeover (`chmod -x /usr/bin/gui`) all rely on the *base rootfs*, which is byte-identical between stock and PAXX — so they work on stock. The stock **boot-time autostart** (the `S99input-event-daemon` hook) is newly added and **not yet verified on a stock device** (development hardware runs PAXX). The hook is additive and idempotent, so it is a no-op on PAXX; see [How HelixScreen takes over the display](#how-helixscreen-takes-over-the-display).

The only PAXX-specific endpoint HelixScreen ever calls is `/printer/filament_detect/set` (RFID write-back); on stock it returns 404 and HelixScreen degrades gracefully (filament edits persist in HelixScreen's own store instead of mirroring back to firmware).

PAXX source: [paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware).

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

The U1 target uses the same aarch64 cross-compiler as the Raspberry Pi, with fully static linking to avoid glibc version dependencies.

### Build via Docker (Recommended)

```bash
# Build the Docker toolchain (first time only — cached after)
make snapmaker-u1-docker
```

The Docker image (`docker/Dockerfile.snapmaker-u1`) is based on Debian Trixie with `crossbuild-essential-arm64`. It uses Debian's `aarch64-linux-gnu` toolchain with static linking for a self-contained binary.

### Build Directly (Requires Toolchain)

```bash
make PLATFORM_TARGET=snapmaker-u1 -j
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
| Optimization | `-Os` (size-optimized) |
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
2. The stock UI process (`gui`) is killed, and the installer disables its binary (`chmod -x /usr/bin/gui`) so no launcher can relaunch it. `lmd` (the camera/timelapse supervisor) is left running — killing it would break timelapse.
3. HelixScreen starts as DRM master with double-buffered page flipping
4. The DRM keepalive process exits once HelixScreen has the DRM device open
5. The first-run wizard appears (language selection, printer connection setup)

### Rollback (Restore Stock UI)

To restore the stock Snapmaker touchscreen UI, run the uninstaller — it re-enables the stock UI binary (`/usr/bin/gui`) and restores the stock `S99screen` launcher (firmware 1.3) or removes the HelixScreen-created one (firmware 1.4):

```bash
ssh root@<printer-ip> "curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall; reboot"
```

> **A bare `rm -rf /userdata/helixscreen` is no longer sufficient.** The installer disables the stock UI binary (`chmod -x /usr/bin/gui`) so neither firmware's launcher can start it; removing HelixScreen without re-enabling the binary leaves a black screen. If you can't run the uninstaller, revert manually:
>
> ```bash
> ssh root@<printer-ip> "killall helix-screen helix-watchdog 2>/dev/null; chmod +x /usr/bin/gui; rm -rf /userdata/helixscreen; reboot"
> ```

The stock UI binary lives on the read-only SquashFS rootfs and is only disabled, never deleted — it cannot be damaged by HelixScreen deployment.

## Reversible Deployment Strategy

HelixScreen can be deployed to the U1 without modifying the read-only base firmware — all changes live in the writable overlay and are fully reversible.

### Level 1: Manual SSH Deployment (Current, Fully Reversible)

This is the current deployment method used by `make deploy-snapmaker-u1`:

1. Install [PAXX Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware) for SSH access
2. Enable SSH via firmware config web UI
3. Deploy via `make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=<ip>`
4. The installer disables the stock UI binary (`chmod -x /usr/bin/gui`) and installs a HelixScreen launcher at `/etc/init.d/S99screen` (in the writable overlay); HelixScreen starts on `/dev/fb0`. `lmd` (camera/timelapse supervisor) keeps running.

**To revert**: run the uninstaller (re-enables `/usr/bin/gui` and restores the launcher), or manually `chmod +x /usr/bin/gui && rm -rf /userdata/helixscreen`, then reboot. The stock UI binary is on the read-only SquashFS rootfs and is only disabled, never deleted — but note that the init-script changes and the binary's exec bit **are** modified (in the reversible overlay), so a plain reboot alone will not bring the stock UI back.

### Level 2: SysV Init Override (Persistent, Reversible)

The U1 uses SysV init (not systemd). A persistent override would:

1. Create a HelixScreen init script in `/etc/init.d/` (writable overlay)
2. Optionally chmod -x the stock `S99screen` script (reversible since overlay)
3. HelixScreen starts on boot; stock UI stays dormant

**To revert**: Remove the init script override and reboot. Stock UI resumes from the read-only base.

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

**The solution**: The platform hooks (`config/platform/hooks-snapmaker-u1.sh`) spawn a background process that holds `/dev/dri/card0` open *before* killing gui. This prevents the kernel from tearing down the CRTC when gui exits. HelixScreen then opens the DRM device itself and becomes DRM master. The keepalive process detects that HelixScreen has the device open and exits — but the CRTC stays active because HelixScreen now holds the fd.

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

HelixScreen auto-detects the Snapmaker U1 using 17 heuristics from `config/printer_database.json`:

| Heuristic | Confidence | Description |
|-----------|------------|-------------|
| `fm175xx_reader` object | 99 | FM175xx RFID reader -- definitive U1 signature |
| `FILAMENT_DT_UPDATE` macro | 95 | RFID filament detection macro (extended firmware) |
| `FILAMENT_DT_QUERY` macro | 95 | RFID filament query macro (extended firmware) |
| Hostname `u1` | 90 | Hostname contains "u1" |
| Hostname `snapmaker` | 85 | Hostname contains "snapmaker" |
| `tmc2240` object | 60 | TMC2240 stepper driver presence |
| CoreXY kinematics | 40 | CoreXY motion system |
| Cartesian kinematics | 20 | Cartesian motion system |
| _(+9 additional heuristics)_ | various | Tool state, extruder naming, custom macros, motion parameters, etc. |

No manual printer configuration is needed in most cases. The FM175xx RFID reader is the strongest signal -- it is unique to the U1 and provides near-certain identification.

When the U1 is detected, the printer database record provides these metadata fields:

| Field | Value |
|-------|-------|
| `probe_type` | `eddy_current` |
| `toolhead_style` | `snapmaker_u1` |
| `preset` | `snapmaker_u1` |
| `z_offset_calibration_strategy` | `probe_calibrate` |

The `snapmaker_u1` preset causes the first-run wizard to auto-skip hardware steps that do not apply to the U1 (e.g., probe wiring, toolhead identification).

## Print Start Tracking

HelixScreen uses the `snapmaker_u1` print start profile (`config/print_start_profiles/snapmaker_u1.json`) to track progress through the startup sequence. The profile uses weighted progress mode with these phases:

1. Homing (10%)
2. Bed heating (20%)
3. Nozzle heating (20%)
4. Z tilt adjust (15%)
5. Bed mesh calibration (15%)
6. Nozzle cleaning (10%)
7. Purging (10%)

The progress bar updates as each phase completes, so you can see exactly where your printer is in its startup routine.

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
- **Two benign Moonraker warnings on buildroot** -- After install, Mainsail/Fluidd surface *"Unable to find DBus PolKit Interface"* (Moonraker's always-loaded `machine` component on a no-PolKit OS) and *"Unable to initialize System Update Provider for distribution: buildroot"* (Moonraker's `update_manager` initializes a system/OS package provider by default, which can't detect apt/PackageKit on buildroot). The PolKit one is not ours — it's inherent to Moonraker on buildroot and re-surfaces because our installer restarts Moonraker. The system-update one is triggered by us adding `[update_manager helixscreen]` (loading the component pulls in the default system provider); it can be silenced with a top-level `[update_manager]` / `enable_system_updates: False`, which doesn't affect HelixScreen's own one-click updater. Both are harmless. Same condition applies to K1/K2 (also buildroot).

## Future Work

### Remote screen / "gui" webcam streaming

**Implemented** as **fbdev + the "fb0 mailbox" mirror**, via the U1 platform hook's `start_remote_screen`/`stop_remote_screen` plus an in-app frame mirror (see the Known Issues entry above). The hook launches the firmware's `fb-http` (fbdev, reading `/dev/fb0`) and exports `HELIX_REMOTE_SCREEN_FB0=/dev/fb0`; HelixScreen then mirrors each rendered frame into `/dev/fb0` so the daemon's snapshot is the live UI. fb0 is a separate buffer decoupled from our DRM scanout (hardware-verified 2026-07-09: writing magenta to fb0 was served by the daemon while the panel stayed on HelixScreen, and helix survived the write). The mirror lives in `RemoteScreenManager`/`Fb0MailboxSink` (`src/application/remote_screen_*.cpp`), fed from the flush hook (`DisplayManager::install_color_transform_hook()`): open `/dev/fb0` once, mmap `MAP_SHARED|PROT_WRITE`, copy each dirty region as **BGRA**, stride 1920 (480×320×4) — a plain per-row memcpy (no byte-swap, no encode), on post-rotation/post-transform pixels. Every dirty rect is mirrored (no artificial fps cap; a static UI produces no writes). No in-process server, no encoder. It rides HelixScreen's existing per-boot start path, so it needs no separate init script (which would fall foul of the overlay boot-glob trap).

**Sink framework (extensible):** the mirror is one `RemoteScreenSink` behind `RemoteScreenManager`, so a future in-process C++ server (#1032) — which would serve snapshots directly and work on every platform, not just the U1 — drops in as an alternative sink without touching the flush-hook tap.

**Forward-looking — DRM capture:** `_remote_screen_backend_args` probes `fb-http` for a `--backend drm` flag and, if present, would drive a DRM capture path that reads `/dev/dri/card0` directly (no mirror needed). **No such `fb-http` exists in any public/shipping source as of 2026-07-09** (verified against the on-device build, `paxx12/screen-apps` `main`, and the pinned Extended-Firmware submodule commit — all fbdev-only), so this path is dormant; the fbdev+mirror path above is what runs today. If a DRM-capable `fb-http` ever ships, the probe lights it up and the mirror stays idle.

**Remote control — working (verified 2026-07-12, #1091).** `fb-http` also injects touch to `/dev/input/event0` via its `/touch` endpoint (nginx `/screen/touch`), and remote *control* works end-to-end for free. HelixScreen opens `event0` through LVGL's evdev driver (`display_backend_drm.cpp:489-492`) and does **not** `EVIOCGRAB` it, so it sees `fb-http`'s injected events. `fb-http` defaults its touch device to `/dev/input/event0` (no `--touch` flag needed) and scales browser clicks 1:1 (the TLSC6x reports `ABS_MT` in the same 480×320 space as the framebuffer). Verified on-device (v0.99.88, printer idle): synthetic taps at the Settings gear, Help & About row, and home-nav icon each landed on the intended element in the rendered snapshot — no offset, no inversion. The browser client (/usr/local/share/fb-http/html/index.html) maps clicks to native image coordinates via `getImageCoords` and POSTs `down`/`move`/`up`. PNG snapshot is poll-based, not a continuous MJPEG stream.

> **Rotation caveat.** This works because the U1 auto-probes `"rotate": 0` (no display rotation). fb-http serves the *rendered* (display-space) frame, and LVGL re-rotates every incoming `event0` point via `lv_display_rotate_point` (`lvgl/src/display/lv_display.c`) whenever a rotation is active. At `rotate == 0` that transform is identity, so injected coordinates pass straight through. **At a non-zero rotation, injected taps would be double-transformed (point-symmetric-inverted)** — fb-http can't know HelixScreen's rotation, and we don't own it. The rotation-robust path is the in-process touch injection tracked by **#1032** (inject past the evdev round-trip, after rotation), not the fb-http path. The U1's panel is fixed-mount and probes to 0 in practice, so this is a latent limitation, not an active bug.

### Extended Firmware Overlay

Package HelixScreen as an Extended Firmware overlay for one-click installation via paxx12's build system.

### RFID Filament UI

The `AmsBackendSnapmaker` backend parses RFID data from `filament_detect.info` when the RFID reader is enabled. With the RFID reader disabled (default on Extended Firmware via `disable-rfid-reader.cfg`), all RFID fields return `"NONE"` and `print_task_config` is the authoritative filament data source.

### Virtual Slot Mapping

The U1 supports an `extruder_map_table` with 32 virtual slots mapped to 4 physical extruders. This could enable more advanced filament management workflows.

## Moonraker Object Reference

The U1's Klipper exposes several custom objects. `AmsBackendSnapmaker` subscribes to and parses these during the Moonraker subscription phase.

### `print_task_config` — Authoritative filament data (HANDLED)

The primary source for filament info. Populated by the stock firmware's task manager regardless of RFID reader state.

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `filament_type` | `["PLA","PLA","PLA","PLA"]` | ✅ | Material per slot |
| `filament_sub_type` | `["SnapSpeed",...]` | ✅ | Appended to type (e.g., "PLA SnapSpeed") |
| `filament_vendor` | `["Snapmaker",...]` | ✅ | Brand per slot |
| `filament_color_rgba` | `["080A0DFF",...]` | ✅ | RGBA hex → RGB uint32 |
| `filament_exist` | `[true,true,true,true]` | ✅ | Slot presence |
| `filament_color` | `[4278716941,...]` | — | Redundant with `_rgba`, not parsed |
| `filament_official` | `[true,...]` | ❌ | Could show official/third-party badge |
| `filament_sku` | `[900001,...]` | ❌ | Snapmaker product SKU |
| `filament_soft` | `[false,...]` | ❌ | Soft filament flag (TPU etc.) |
| `filament_edit` | `[false,...]` | ❌ | Whether user has edited filament info |
| `extruder_map_table` | `[0,1,2,3,0,...(x32)]` | ❌ | Virtual→physical slot mapping for multi-material |
| `extruders_used` | `[false,...]` | ❌ | Which extruders are used in current print |
| `extruders_replenished` | `[0,1,2,3]` | ❌ | Auto-replenish mapping |
| `auto_replenish_filament` | `true` | ❌ | Auto-replenish enabled |
| `filament_entangle_detect` | `false` | ❌ | Entangle detection enabled |
| `filament_entangle_sen` | `"medium"` | ❌ | Sensitivity level |
| `flow_calibrate` | `false` | ❌ | Flow calibration pre-print option |
| `shaper_calibrate` | `false` | ❌ | Input shaper calibration pre-print option |
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

### `machine_state_manager` — Machine state (NOT HANDLED)

| Field | Example | Handled | Notes |
|-------|---------|---------|-------|
| `main_state` | `0` | ❌ | Top-level machine state (0=idle) |
| `action_code` | `0` | ❌ | Current action code |

Likely redundant with `print_stats.state` for most purposes.

### `defect_detection` — Print defect detection (NOT SUBSCRIBED)

AI-based print defect detection system. The status object itself is not subscribed — HelixScreen consumes detection through `print_stats.exception` instead: on a confirmed defect (custom `exception` sub-object, `code: 2` = spaghetti/noodle) the firmware pauses the print itself, and `U1StockSource` → `DetectionManager` (`include/detection_manager.h`) surfaces it as the response surface — spaghetti modal, defer-to-source policy, master toggle + per-source policy in Settings. Capability is gated by probing `printer.objects.list` for `defect_detection` after the WebSocket connects (probing at init would latch false forever).

| Field | Example | Notes |
|-------|---------|-------|
| `main_enable` | `true` | Master enable |
| `clean_bed.enable` | `true` | Dirty bed detection |
| `noodle.enable` | `true` | Spaghetti/noodle detection |
| `residue.enable` | `false` | Residue detection |
| `nozzle.enable` | `false` | Nozzle clog detection |

### `purifier` — Air purifier (NOT SUBSCRIBED)

Built-in HEPA/carbon air purifier. Not subscribed because we don't have UI for it yet.

| Field | Example | Notes |
|-------|---------|-------|
| `fan_speed` | `0.0` | Purifier fan speed (0-1) |
| `fan_rpm` | `0.0` | Actual RPM |
| `work_time` | `0` | Total runtime (seconds) |
| `power_detected` | `false` | External power connected |
| `delay_time` | `180` | Post-print run time (seconds) |

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
5. File issues at the HelixScreen GitHub repository

## Related Resources

- **[Extended Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware)** -- Adds SSH access and community features to the U1
- **[U1 Config Example](https://github.com/JNP-1/Snapmaker-U1-Config)** -- Community reverse-engineered Klipper configuration
- **[Snapmaker Forum](https://forum.snapmaker.com/c/snapmaker-products/87)** -- Official U1 discussion
- **[Toolchanger Research](../printer-research/SNAPMAKER_U1_RESEARCH.md)** -- Detailed analysis of U1's toolchanger implementation vs. standard Klipper toolchanger module
- **[Snapmaker/u1-klipper](https://github.com/Snapmaker/u1-klipper)** -- Open source Klipper fork
- **[Snapmaker/u1-moonraker](https://github.com/Snapmaker/u1-moonraker)** -- Open source Moonraker fork
- **[Snapmaker/u1-fluidd](https://github.com/Snapmaker/u1-fluidd)** -- Open source Fluidd fork
- **[paxx12/u1-firmware-tools](https://github.com/paxx12/u1-firmware-tools)** -- Firmware unpack/repack tools
