<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Creality K2 Series Support

HelixScreen has a cross-compilation target for the Creality K2 series of enclosed CoreXY printers. The K2 series runs Klipper with stock Moonraker, making it a natural fit for HelixScreen.

## Supported Models

All K2 models use Allwinner ARM Cortex-A7 dual-core processors running Tina Linux (OpenWrt-based).

| Model | Build Volume | Display | Chamber Heater | CFS | Status |
|-------|-------------|---------|----------------|-----|--------|
| K2 | 260 mm cubed | 4.3" 480x800 | No | Optional | Untested |
| K2 Pro | 300 mm cubed | 4.3" 480x800 | Yes (60C) | Optional | **Community-confirmed running** (2026-09) |
| K2 Plus | 350 mm cubed | 4.3" 480x800 | Yes (60C) | Yes (CFS) | **Hardware confirmed** |
| K2 SE | 220x215x245 mm | Unknown | No | Unknown | User-confirmed install (wget) |

Detection has an entry for the K2, K2 Pro and K2 Plus (plus the K2 Plus k2-improvements variant),
all sharing the `k2` preset. They are told apart by the model hostname or by the bed size the
firmware declares in `gcode_macro product_param` (250-270, 290-310 and 340-360 mm). The base K2
entry is the preset's `preset_default`, so a K2 preset install that identifies no K2 machine
persists "Creality K2". Its image is a copy of the K2 Plus photo until one of the base model exists.

## Hardware (Confirmed on K2 Plus — 2026-03-23)

| Spec | Value |
|------|-------|
| SoC | Allwinner T113 - `allwinner,t113_iarm,sun8iw20p1` (ARM Cortex-A7, dual-core, 57 BogoMIPS) |
| Board | OpenWrt `DISTRIB_TARGET=t113_i-CR0CN240110C10/generic`, `DISTRIB_ARCH=arm_cortex-a7_neon` |
| Display | 480x800 portrait (`U:480x800p-58`), 32bpp, stride 1920, fbdev (`/dev/fb0`); double-buffered → 480x1600 virtual fb. No DRM (`/dev/dri` absent) |
| Touch | Goodix `gt9xxnew_ts` on I2C (`Bus=0018`), sole input node `/dev/input/event0` |
| Rotation | 270° in software, from `assets/config/presets/k2.json` (`display.rotate: 270`, `rotation_probed: true`) |
| Stock UI | `/usr/bin/display-server` (must be stopped to use framebuffer) |
| RAM | 488 MB total |
| Storage | 27.5 GB on `/mnt/UDISK` |
| OS | OpenWrt 21.02-SNAPSHOT (Tina Linux), Linux 5.4.61 armv7l |
| Device libc | glibc 2.29 (`ld-linux-armhf.so.3`), libstdc++ 6.0.25. We ship a fully static musl binary, so it does not matter |
| Init System | procd (OpenWrt-style, NOT systemd) |
| MCU | GD32F303RET6 on `/dev/ttyS2` @ 230400 baud |
| Nozzle MCU | GD32F303CBT6 on `/dev/ttyS3` @ 230400 baud |
| Moonraker | Port 7125 (direct), port 4408 (nginx proxy) |
| Klipper UDS | `/tmp/klippy_uds` |
| SSH | `root` / `creality_2024` (enable via Settings menu) |
| Config path | `/mnt/UDISK/printer_data/config/` |
| Klipper path | `/usr/share/klipper/` |
| Logs path | `/mnt/UDISK/printer_data/logs/` |
| Gcode path | `/mnt/UDISK/printer_data/gcodes/` (also `/root/klipper/gcodes/`) |
| Web server | `web-server` on ports 80, 443, 9998, 9999 |
| ADB | `adbd` on port 5037 |
| WebRTC | `webrtc_local` on port 8000 (camera) |

### Notes

- **No curl** — BusyBox wget only (no HTTPS support). Use `python3 urllib` for HTTP requests.
- **armv7l** — Dual-core Cortex-A7 (NOT Cortex-A53). Lower performance than K1 series.
- **480x800 display** — The panel is 480x800 portrait, same as all other K2 models. The controller behind it varies by variant (`lcm_id=gc9503cv_ue_480_800` on a K2 Plus, `st7701_9bit_mipi_tjc_480_800` on a K2 Pro) at identical geometry, depth and stride, so it changes nothing above the framebuffer. HelixScreen software-rotates portrait→landscape (applies to all K2). The 480x1600 seen in `/sys/class/graphics/fb0/virtual_size` is a double-buffered virtual framebuffer (two stacked 480x800 buffers), not a taller panel.
- **Python 3.9** — Available at `/usr/bin/python3`.
- **Moonraker's config is outside the file API** — stock firmware launches
  `moonraker.py -c /usr/share/moonraker/moonraker.conf`, while the file manager's only
  writable `config` root is `/mnt/UDISK/printer_data/config`. So
  `GET /server/files/config/moonraker.conf` is a 404 and **no HTTP call can edit
  Moonraker's configuration on this printer.** HelixScreen falls back to writing the file
  locally; it is the only supported printer that needs to. See
  [MOONRAKER_ARCHITECTURE.md § Locating Moonraker's Config File](../MOONRAKER_ARCHITECTURE.md#locating-moonrakers-config-file).
  A `moonraker.conf` **does** sometimes exist under the writable root — left by earlier
  HelixScreen releases — and it is a decoy: it shares the stock section set with the real
  config and Moonraker never reads it.

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

| Setting | Value |
|---------|-------|
| Architecture | armv7-a (hard-float, NEON VFPv4) |
| Toolchain | `arm-buildroot-linux-musleabihf-gcc` (Bootlin musl) |
| Linking | Fully static (musl) |
| Display backend | fbdev (`/dev/fb0`) |
| Input | evdev (auto-detected) |
| SSL | Disabled (Moonraker is local on port 4408) |
| Optimization | `-Os` with LTO (size-optimized) |
| Platform define | `HELIX_PLATFORM_K2` |

### CI/Release Status

The K2 target **is included** in the GitHub Actions release pipeline (`.github/workflows/release.yml`). Release artifacts are built automatically:

```bash
# Manual packaging
make package-k2
```

## Installation

### Prerequisites

- A Creality K2, K2 Pro, or K2 Plus printer
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

Deploy directory: `/mnt/UDISK/helixscreen` (override with `K2_DEPLOY_DIR`). SSH credentials: `root`/`creality_2024` (override with `K2_USER`/`K2_PASS`).

**Note**: The K2 uses BusyBox (OpenWrt), so deployment uses tar/ssh transfer instead of rsync.

### What Happens on Deploy

1. Stops any running HelixScreen processes
2. Deploys platform hooks (`assets/config/platform/hooks-k2.sh` → /mnt/UDISK/helixscreen/platform/hooks.sh)
3. Transfers binaries, assets, XML layouts, and config
4. Installs SysV init script at `/etc/init.d/S99helixscreen` for boot persistence
5. Installs the web-server carve-out at `/etc/init.d/helix-k2-webserver` (`config/k2-webserver.init`, a USE_PROCD starter). Once its instance is registered, procd's respawn keeps `web-server` alive; the hook's restore registers it at every HelixScreen start — the stock app's `stop` runs killall -9 over the stock set and takes any `web-server` down (ours included), while `disable` only removes the app's rc.d links. The carve-out's own rc.d boot entry is belt-and-braces. It serves the LAN web interface: ports 80/443 redirect to Fluidd on :4408, 9999 is the local status websocket (prestonbrown/helixscreen#1617)
6. Platform hooks stop the stock Creality UI (`display-server`, `Monitor`, etc.) via procd
7. Platform hooks start `wpa_supplicant` to replace the stock `wifi-server`
8. Starts HelixScreen on the framebuffer

### Reverting to Stock UI

To restore the stock Creality touchscreen:

```bash
ssh root@<printer-ip>
killall helix-screen helix-splash helix-watchdog 2>/dev/null
killall web-server 2>/dev/null            # Free port 80 for the stock instance
/etc/init.d/helix-k2-webserver disable    # Drop the carve-out's boot symlink (NOT the
                                          # /etc/rc.d/S99... spelling: rc.common derives
                                          # link names from basename $0, so that one
                                          # computes S99S99... and removes nothing)
/etc/init.d/app enable   # Re-enable stock UI on boot
/etc/init.d/app start    # Start stock UI now
```

### Display Backend

HelixScreen renders directly to `/dev/fb0`. The platform hooks stop the stock `display-server` to release the framebuffer. This is handled automatically by the deploy targets.

The K2 Plus panel is **480x800 portrait**; the framebuffer is double-buffered (480x1600 virtual). HelixScreen rotates it 270° to landscape and transforms touch coordinates to match. The rotation ships in `assets/config/presets/k2.json` with `rotation_probed: true`, so the interactive orientation probe never runs on a K2. `HELIX_DISPLAY_ROTATION` or `--rotate` override it.

### Touch Input

HelixScreen uses evdev and auto-detects the capacitive touch controller. Running as root (default) avoids permission issues on `/dev/input/event*`.

The K2 Plus reports a Goodix `gt9xxnew_ts` on `/dev/input/event0`, which is the only input node on the machine. **The name contains no "touch" substring**, so `grep -i touch /proc/bus/input/devices` returns nothing on a healthy K2 - match on `gt9`/`goodix` or just read the whole file.

Selection is scored, not name-matched: `src/api/input_device_scanner.cpp#"find_touch_device(const"` (shared by the fbdev and DRM backends) requires ABS capabilities, then adds points for a known name (`include/touch_calibration.h#is_known_touchscreen_name`), `INPUT_PROP_DIRECT`, and USB. Some K2 hardware revisions carry a `tlsc6x` controller instead. Its kernel name `tlsc6x_touch` matches the generic `touch` pattern, so it scores as a known panel (the Snapmaker U1 uses the same controller). No K2 with that variant has been observed yet.

## Spaghetti Detection

Installing HelixScreen stops the stock AI failure-detection loop: the launcher hook stops and disables `/etc/init.d/app` (the procd service whose `Monitor`/`master-server`/`app-server` children run Creality's detect loop), and the camera module hands `/dev/video0` to ustreamer. HelixScreen ships its own detector for that gap, so a K2 running HelixScreen is still watched.

`K2StockDetectionSource` (`src/printer/k2_stock_detection_source.cpp`) is capable only on a Creality K2 with `/usr/bin/detection` present and executable. The probe re-runs on every WebSocket connect (the printer type is read from the wizard's saved config, not cached at boot), so a first install that picks K2 in the wizard gets detection without a restart. During an active print it fetches a camera snapshot, runs the stock `/usr/bin/detection` binary on it, and parses the `label:`/`prob:` lines for the maximum spaghetti probability. Detection is edge-triggered: it fires once per spaghetti episode, stays quiet while it persists, and re-arms when the frame goes clean or a new print starts.

The interval and threshold are the printer's own, read from `/mnt/UDISK/creality/userdata/config/user_print_refer.json` (`ai_control.pastaTime` = poll period, clamped 5-600s; `ai_control.pastaTruth` = confidence threshold). Nothing writes that file.

The source only reports. Whether a detection pauses the print is decided above it, from the two settings in Settings > Safety & Alerts:

- **Spaghetti Detection** (on/off)
- **Pause on Detection** (pause the print, or only warn)

On the first start after install, both are seeded once from the printer's own stored choice in the same `ai_control` block (`switch` -> enabled, `pausePrint` -> pause). The copy happens that one time: later changes on Creality's side (the stock UI or a hand-edited `user_print_refer.json`) are not followed, and HelixScreen's toggles are authoritative from then on. HelixScreen never writes that file. A confirmed detection with pausing on sends `PAUSE`, shows the spaghetti modal (Resume / Abort / Reduce Sensitivity / Turn off detection); with pausing off it only warns. The Reduce Sensitivity button sends the stock `DEFECT_DETECTION_CONFIG NOODLE_SENSITIVITY=low` macro.

For desktop development, `HELIX_MOCK_DETECTION_CAPABLE=1` forces the capability probe true (mock printers are never a K2), so the Settings rows and the detection loop can be exercised in `--test` runs.

## CFS (Creality Filament System)

The CFS is a multi-material system on RS-485: four spools per unit, up to four units chained for
16 colours. All protocol logic lives in Creality's closed Cython module `box_wrapper.cpython-39.so`;
Klipper exposes it as the `box` object (plus `filament_rack` and `motor_control`). HelixScreen's
backend is described in [FILAMENT_BACKEND_CFS.md](../FILAMENT_BACKEND_CFS.md#cfs-creality-filament-system).

The firmware-side reference (box.cfg, the `box` schema variants and field encodings, the `BOX_*`
and `M8200` command surface, error codes, runout and auto-refill, and the community Kalico port)
is [CREALITY_CFS_K2_INTERNALS.md](../printer-research/CREALITY_CFS_K2_INTERNALS.md).

## Auto-Detection

HelixScreen auto-detects K2 printers using heuristics from `config/printer_database.json`:

| Heuristic | Confidence | Description |
|-----------|------------|-------------|
| Hostname `k2plus` / `k2pro` | 90 | Hostname contains "K2Plus" / "K2Pro" |
| Hostname `k2` | 85 | Hostname contains "k2" |
| `motor_control` object | 75 | K2-specific motor control module |
| `chamber_temp` sensor | 70 | Chamber temperature sensor |
| `heater_generic chamber_heater` | 70 | Active chamber heater |
| `fan_feedback` object | 65 | Creality fan-tachometer module — a captured real K1C reports it too, so it corroborates rather than distinguishes |
| `filament_rack` object | 65 | K2 filament rack module |
| Hostname `creality` | 60 | Hostname contains "creality" |
| `load_ai` object | 60 | AI print monitoring |
| `box` object | 45 | CFS box object — every CFS-equipped Creality carries it, so it corroborates |
| `build_volume_range` | 55 | Build volume within the K2 envelope |
| CoreXY kinematics | 40 | CoreXY motion system |

These identify the **printer**, not its firmware. A community Kalico port trips every one of them — `box`, `motor_control`, `fan_feedback` and the hostname are all still present — so model detection reports a stock K2 Plus. Anything that varies with firmware rather than hardware (the CFS box schema, the macro dialect) must therefore be detected from the payload or the macro list, never from `PrinterDetector`. See [Community Kalico port](../printer-research/CREALITY_CFS_K2_INTERNALS.md#community-kalico-port).

Note that `chamber_temp` is **not** universal on K2 hardware either: the Kalico port drops `temperature_sensor chamber_temp` and exposes chamber temperature through `heater_generic chamber_heater` instead.

## Known Limitations

### Display
- **480x800 portrait panel (double-buffered framebuffer → 480x1600 virtual)** — presented landscape by a 270° software rotation; same as all other K2 models.

### CFS
- **Closed-source protocol** — CFS communication relies on `box_wrapper.cpython-39.so` binary blob. Protocol has been reverse-engineered from strings but full reimplementation is not yet available.
- **Material database is cloud-fetched** — The material database at /mnt/UDISK/creality/userdata/box/material_database.json is downloaded from Creality's cloud. HelixScreen should include a fallback mapping for common material type codes.
- **Community Kalico ports require Box API v1 for control** — the flat status layout still parses without it, but load/unload/tool-change stay gated until `api_version == 1` identifies the supported command dialect. See [Community Kalico port](../printer-research/CREALITY_CFS_K2_INTERNALS.md#community-kalico-port).

### Platform
- **Low CPU** — Dual Cortex-A7 at ~57 BogoMIPS. Performance-sensitive features (bed mesh 3D, animations) may need throttling.
- **No curl** — BusyBox wget only, no HTTPS support.
- **Web-server carve-out** — `web-server` serves the LAN web interface (ports 80/443 redirect to Fluidd on :4408; 9998 is its HTTPS listener, 9999 the local status websocket) and must keep serving with HelixScreen installed. The platform hooks' `/etc/init.d/app` stop takes it down with the rest of the stock set (killall -9 in the stock stop_service; `disable` only removes rc.d links), so liveness is supervised: `config/k2-webserver.init` is a USE_PROCD starter whose registered instance procd respawns, and `platform_stop_competing_uis` (`hooks-k2.sh`) re-registers it at the end of every HelixScreen start — after every boot, service restart, and watchdog-driven start. The installer (`install_k2_webserver_backend`), `make deploy-k2`, and uninstall (as a `sysv-created` service) all carry the script; its own rc.d boot entry is belt-and-braces only (prestonbrown/helixscreen#1617, prestonbrown/helixscreen#1665).
- **WiFi managed by platform hooks** — The stock `wifi-server` is killed when HelixScreen takes over the display. Platform hooks (`hooks-k2.sh`) start `wpa_supplicant` directly using credentials at `/etc/wifi/wpa_supplicant/wpa_supplicant.conf`. WiFi configuration changes made via the stock UI are preserved.
- **Non-standard control socket** — `hooks-k2.sh` launches `wpa_supplicant` without `-O`, so the control socket lands at the `ctrl_interface=` from the stock conf — `/etc/wifi/wpa_supplicant/sockets/wlan0` on K2 — not the usual `/run/wpa_supplicant`. The WiFi backend searches that location (and auto-detects any `-O` path from the live process), so network discovery works without manual symlinks. Firmware that uses yet another path can be pointed at it via `HELIX_WPA_SOCKET_DIR`. Surfaced by a community **K2 Plus** report.

## Related Resources

- **[CrealityOfficial/K2_Series_Klipper](https://github.com/CrealityOfficial/K2_Series_Klipper)** — Creality's official (incomplete) Klipper fork
- **[Guilouz/Creality-K2Plus-Extracted-Firmwares](https://github.com/Guilouz/Creality-K2Plus-Extracted-Firmwares)** — Extracted stock firmware images
- **[ityshchenko/klipper-cfs](https://github.com/ityshchenko/klipper-cfs)** — Community open-source CFS module
- **[K2 Plus Research](../printer-research/CREALITY_K2_PLUS_RESEARCH.md)** — Detailed hardware and software research
- **[K1 vs K2 Community Comparison](../printer-research/CREALITY_K1_VS_K2_COMMUNITY.md)** — Analysis of community ecosystem differences
- **[Creality Wiki](https://wiki.creality.com/en/k2-flagship-series/k2-plus)** — Official K2 Plus documentation
- **[Creality Forum](https://forum.creality.com/c/flagship-series/creality-flagship-k2-plus/81)** — Official K2 Plus community forum
