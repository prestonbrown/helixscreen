# Snapmaker U1 Support

**Last updated**: 2026-03-31

## Hardware Profile

| Spec | Value |
|------|-------|
| SoC | **Rockchip RK3562** — quad Cortex-A53 @ 2GHz, Mali-G52 2EE GPU, 1 TOPS NPU |
| Display | **UNCERTAIN** — see [Display Resolution Discrepancy](#display-resolution-discrepancy) |
| Touch | `/dev/input/event0` |
| Firmware | Klipper + Moonraker (modified forks, open source as of 2026-03-30) |
| Stock UI | `/usr/bin/unisrv` (Universal Service) — NOT Qt, NOT Flutter, NOT open source |
| Drivers | TMC2240 |
| Filament | RFID (FM175xx, OpenSpool NTAG215/216) |
| Camera | MIPI CSI + USB, Rockchip MPP/VPU |
| OS | Debian Trixie ARM64, SquashFS rootfs (read-only) |
| SSH | `root@<ip>` or `lava@<ip>` (password: `snapmaker`) via extended firmware |

### Display Resolution Discrepancy

> **NEEDS HARDWARE VERIFICATION**: The actual display resolution is unconfirmed.

| Source | Resolution | Notes |
|--------|-----------|-------|
| Our build target | 480x320 | What we compile for today |
| Snapmaker official specs | 800x480 | Marketing materials |
| fb-http touch defaults | 1024x600 | Extended firmware's remote screen tool |

The real resolution can only be confirmed on hardware via `fbset` or `FBIOGET_VSCREENINFO` ioctl on `/dev/fb0`. Until verified, the 480x320 build target may be wrong.

---

## Open Source Release (2026-03-30)

Snapmaker released GPL code on March 30, 2026:

| Repository | Modification Level | Key Changes |
|------------|-------------------|-------------|
| [Snapmaker/u1-klipper](https://github.com/Snapmaker/u1-klipper) | ~20% modified | Multi-toolhead, eddy-current probing, RFID, power-loss recovery |
| [Snapmaker/u1-moonraker](https://github.com/Snapmaker/u1-moonraker) | ~15% modified | Snapmaker Cloud integration, 3MF support |
| [Snapmaker/u1-fluidd](https://github.com/Snapmaker/u1-fluidd) | Stock | Basically Fluidd v1.36.2, unmodified |

**NOT released**: The proprietary touchscreen UI (`/usr/bin/unisrv`). This remains closed source.

### Toolchanger Approach (from Klipper source)

The U1 does **NOT** use the [viesturz/klipper-toolchanger](https://github.com/viesturz/klipper-toolchanger) module. Instead:

- Native multi-extruder: `[extruder]`, `[extruder1]`, `[extruder2]`, `[extruder3]`
- Custom macros in `lava/` directory for tool parking/switching
- T0-T3 gcode commands → `ACTIVATE_EXTRUDER` internally
- No `[toolchanger]` or `[tool T*]` config sections

---

## Filesystem Layout

| Path | Type | Contents |
|------|------|----------|
| `/` | SquashFS (read-only) | Root filesystem |
| `/home/lava/` | Writable | User data, Klipper configs |
| `/home/lava/printer_data/config/` | Writable | Klipper configuration files |
| `/oem/` | Writable | OEM partition |
| `/userdata/` | Writable | User data partition |

### Init Scripts (from extended firmware analysis)

| Script | Service |
|--------|---------|
| `S50dropbear` | SSH (added by extended firmware) |
| `S60klipper` | Klipper |
| `S61moonraker` | Moonraker |
| `S90lmd` | Camera service (`/usr/bin/lmd`) |

> **NEEDS HARDWARE VERIFICATION**: The systemd service name for `unisrv` (stock UI) is unknown. Requires SSH access to enumerate.

---

## A/B Firmware Slots

The U1 uses Rockchip's standard A/B partition scheme:

- Switch slot: `updateEngine --misc=other --reboot`
- MaskRom recovery available for unbricking
- Flashing stock firmware via USB reverts all modifications

**Reversibility**: A/B slots + MaskRom = effectively unbrickable.

---

## Extended Firmware (paxx12)

[paxx12/SnapmakerU1-Extended-Firmware](https://github.com/paxx12/SnapmakerU1-Extended-Firmware) — Docker-based overlay build system that patches the stock SquashFS image.

### What It Adds

- SSH access (root/lava, password "snapmaker")
- v4l2-mpp camera support
- fb-http remote screen viewer
- Fluidd / Mainsail web UIs
- AFC-Lite filament changer support
- OpenRFID
- Tailscale VPN

### Installation

1. Copy `.bin` file to USB drive
2. Flash via touchscreen Settings menu
3. Stock firmware can be restored the same way (full reversibility)

---

## Current Status

### Implemented (committed on `main`)
- ARM64 cross-compilation target: `make PLATFORM_TARGET=snapmaker-u1` / `make snapmaker-u1-docker`
- Docker toolchain: `docker/Dockerfile.snapmaker-u1` (Debian Trixie, static linking)
- Printer database entry with RFID reader detection heuristics
- Print start profile: `config/print_start_profiles/snapmaker_u1.json` (weighted/heuristic)
- Platform hooks: `config/platform/hooks-snapmaker-u1.sh`
- Deployment targets: `deploy-snapmaker-u1`, `deploy-snapmaker-u1-fg`, `deploy-snapmaker-u1-bin`

### NOT in CI/release pipeline
The snapmaker-u1 target is deliberately excluded from `release-all` and `package-all`.
It will not build in GitHub Actions until a workflow job is explicitly added to `.github/workflows/release.yml`.

### 480x320 Display (may be wrong resolution)
See the 480x320 Display Considerations section of
[SNAPMAKER_U1_SUPPORT.md](../printers/SNAPMAKER_U1_SUPPORT.md) for a summary of what works
and what needs fixing at this resolution. If the actual display is 800x480 or 1024x600,
those notes may not apply and a new breakpoint/layout target would be needed.

---

## Build & Deploy

```bash
# Build via Docker (recommended)
make snapmaker-u1-docker

# Deploy to U1
make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=192.168.1.xxx

# Deploy binary only (fast iteration)
make deploy-snapmaker-u1-bin SNAPMAKER_U1_HOST=192.168.1.xxx

# SSH
make snapmaker-u1-ssh SNAPMAKER_U1_HOST=192.168.1.xxx
```

Default SSH user is `lava` (override with `SNAPMAKER_U1_USER`).
Default deploy dir is `/opt/helixscreen` (override with `SNAPMAKER_U1_DEPLOY_DIR`).

---

## Future Work

### Display Resolution Verification (Blocker)
**Priority**: High. Our build target assumes 480x320 but this is likely wrong.
Need someone with hardware to run `fbset` or query `FBIOGET_VSCREENINFO`.

### RFID Filament Integration (Nice-to-Have)
The U1 has a 4-channel RFID reader (FM175xx) with OpenSpool support. Could implement
`AmsBackendRfid` following the existing AMS backend pattern.

Klipper commands:
- `FILAMENT_DT_UPDATE CHANNEL=<n>` — Read tag
- `FILAMENT_DT_QUERY CHANNEL=<n>` — Query cached data
- OpenSpool JSON format with material type, color, temp ranges

### Analyze Moonraker Fork API Surface
Now that source is available, examine `u1-moonraker` to determine the exact API surface
for Snapmaker Cloud integration and 3MF support. May expose additional objects useful
for HelixScreen.

### Extended Firmware Overlay Packaging
Could package HelixScreen as an extended firmware overlay for cleaner user installation
(vs manual SSH deployment).

---

## References

- **Snapmaker source code**: [u1-klipper](https://github.com/Snapmaker/u1-klipper) | [u1-moonraker](https://github.com/Snapmaker/u1-moonraker) | [u1-fluidd](https://github.com/Snapmaker/u1-fluidd)
- **Extended Firmware**: [paxx12/SnapmakerU1-Extended-Firmware](https://github.com/paxx12/SnapmakerU1-Extended-Firmware)
- **Community Config**: [JNP-1/Snapmaker-U1-Config](https://github.com/JNP-1/Snapmaker-U1-Config)
- **Forum**: [Snapmaker U1 Toolchanger Category](https://forum.snapmaker.com/c/snapmaker-products/87)
- **Discord**: Snapmaker Discord `#u1-printer` channel
- **Detailed research**: [SNAPMAKER_U1_RESEARCH.md](SNAPMAKER_U1_RESEARCH.md) (toolchanger detection, Moonraker API comparison)
