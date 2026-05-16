<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# QIDI Printer Support

HelixScreen supports the entire QIDI 3-series and 4-series in two distinct modes. Which mode you use depends on whether you want HelixScreen running on the printer's built-in screen, or on a separate touchscreen device (Raspberry Pi, repurposed tablet, etc.) controlling the printer over the network.

## Two Operating Modes

**Remote control (network client) -- works on every supported QIDI**

If your QIDI is running Klipper + Moonraker -- whether that's stock firmware, [FreeDi](https://github.com/Phil1988/FreeDi), [OpenQIDI](https://openqidi.com/), or another community stack -- HelixScreen can control it remotely from a separate touchscreen device. The printer's local display (TJC HMI or otherwise) is irrelevant in this mode; HelixScreen talks to Moonraker over WebSocket on port 7125 the same way Mainsail or Fluidd does.

All six QIDI models in the printer database (X-Max 3, X-Plus 3, X-Smart 3, Q1 Pro, Plus 4, Q2) are auto-detected as remote targets. Just point HelixScreen at the printer's hostname or IP and the correct profile loads automatically.

**On-device install (replace the built-in UI)**

This mode requires a Linux framebuffer display. Most older QIDI models use TJC HMI displays (a Chinese Nextion clone) connected over serial UART -- standalone MCU-driven screens that HelixScreen cannot drive without a physical screen swap. FreeDi targets that serial display from the mainboard side with its own firmware; HelixScreen can't.

The newer 4-series models (Q2, Plus 4, Max 4) ship with framebuffer displays and can run HelixScreen directly on the printer.

## Display Compatibility

QIDI uses two fundamentally different display architectures:

- **TJC HMI (serial)** -- A standalone microcontroller-driven display connected to the mainboard via serial UART. These are flashed with `.tft` firmware files via microSD card. HelixScreen **cannot** drive these displays. FreeDi targets this display type.
- **Linux framebuffer** -- A display driven directly by the Linux SoC via fbdev or DRM. HelixScreen **can** run on these.

## Models

QIDI uses two generations of mainboard:

- **Older models (X-Max 3, X-Plus 3, Q1 Pro, X-Smart 3):** MKSPI boards with Rockchip RK3328, ARM Cortex-A53 (aarch64), 1 GB RAM. These all use TJC HMI serial displays.
- **Newer models (Q2, Plus 4, Max 4):** New-generation boards with quad-core ARM Cortex-A35 (aarch64), ~498 MB RAM. These use Linux framebuffer displays driven by the SoC.

"On-device" below means whether HelixScreen can replace the printer's built-in display. **All six models work as remote targets regardless of this column.**

| Model | Display Type | Resolution | On-Device Install? | Notes |
|-------|-------------|------------|--------------------|-------|
| Q2 | Linux framebuffer (4.3" IPS capacitive) | 480x272 | **Yes** (confirmed) | Goodix touch controller. User-confirmed working install. WiFi requires wpa_supplicant backend (see below). |
| Plus 4 | Linux framebuffer (5" capacitive) | 800x480 | **Likely yes** (untested) | Same new-gen board architecture as Q2. Uses TJC HMI on stock firmware but has Linux framebuffer capability with FreeDi/OpenQIDI. |
| Max 4 | Linux framebuffer (5" capacitive) | 800x480 | **Likely yes** (untested) | Same display and board architecture as Plus 4. |
| X-Max 3 | TJC HMI (serial) | 800x480 | **No** | Requires screen replacement (HDMI/DSI touchscreen). Auto-detected as remote target. |
| X-Plus 3 | TJC HMI (serial) | 800x480 | **No** | Requires screen replacement. Same display firmware as X-Max 3. Auto-detected as remote target. |
| Q1 Pro | TJC HMI (serial) | 480x272 | **No** | Requires screen replacement. TJC model TJC4827X243_011. Auto-detected as remote target. |
| X-Smart 3 | TJC HMI (serial) | 480x272 | **No** | Requires screen replacement. Smallest of the 3-series (175x180x170, passive enclosure, no active chamber heater). Auto-detected as remote target. |

## Remote Control (Network Client)

No QIDI-side install is needed. Run HelixScreen on a Raspberry Pi, repurposed Android tablet, or any other supported device, and add the QIDI printer by hostname or IP. Auto-detection identifies the model from Klipper objects, macros, hostname, and build volume. The right print start profile and capabilities load automatically.

This works on **stock firmware** (Q2, Plus 4, Max 4 all run standard Moonraker) and on **community stacks** like [FreeDi](https://github.com/Phil1988/FreeDi), [OpenQIDI](https://openqidi.com/), or [53Aries/Q2-Firmware](https://github.com/53Aries/Q2-Firmware) -- anything that exposes Moonraker on port 7125.

For the older 3-series (X-Max 3, X-Plus 3, X-Smart 3, Q1 Pro), FreeDi is the easy path to a clean Klipper + Moonraker + Mainsail stack. FreeDi's own `FreeDiLCD` keeps the printer's local TJC display alive; HelixScreen runs separately on your touchscreen device and controls the printer over the network.

## On-Device Installation

This section is for replacing the printer's built-in display with HelixScreen running directly on the printer. Only supported on Linux-framebuffer models (see table above).

### Prerequisites

- A QIDI printer with a Linux framebuffer display (Q2, Plus 4, Max 4)
- SSH access to the printer
- Stock firmware works directly. Community firmware like [53Aries/Q2-Firmware](https://github.com/53Aries/Q2-Firmware) or [OpenQIDI](https://openqidi.com/) may also be used.

### One-Line Installer (Recommended)

The standard HelixScreen installer works on QIDI hardware out of the box. SSH into the printer and run:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer auto-detects QIDI-class SBCs (hostname `linaro-alip` + `/home/mks` on stock Q2; the same Pi/aarch64 binary covers Plus 4 and Max 4) and sets up the systemd service, launcher, and config under the correct user.

### Manual Deployment (Alternative)

QIDI's aarch64 processors are the same architecture as the Raspberry Pi 4 and Pi 5, so the standard Pi build of HelixScreen also runs if you'd rather drop the binary in by hand:

```bash
# Build on a build server (or use a pre-built release)
make pi-docker

# Copy the binary to your QIDI printer
scp build-pi/bin/helix-screen root@<qidi-ip>:/usr/local/bin/

# SSH into the printer and run
ssh root@<qidi-ip>
helix-screen
```

For verbose output during first-time setup, add `-vv` for DEBUG-level logging:

```bash
helix-screen -vv
```

### Display Backend

HelixScreen auto-detects the best available display backend in this order: DRM, fbdev, SDL. QIDI hardware should work with either DRM or fbdev depending on the OS setup. No display configuration is needed -- HelixScreen picks the right backend automatically.

### Touch Input

HelixScreen uses libinput for touch input and should auto-detect `/dev/input/eventX` devices on QIDI hardware. If touch input doesn't work, check that input devices are present and accessible:

```bash
ls /dev/input/event*
```

Ensure the user running HelixScreen has read permissions on the event device. Running as root (common on QIDI printers) avoids permission issues.

## Auto-Detection

HelixScreen auto-detects all six supported QIDI models (X-Max 3, X-Plus 3, X-Smart 3, Q1 Pro, Plus 4, Q2) using several heuristics:

- Hostname patterns (`qidi`, `x-max`, `x-plus`, `x-smart`, `xsmart`, `q1`, `plus4`)
- Active chamber heater presence (X-Max 3, X-Plus 3, Q1 Pro, Plus 4 -- X-Smart 3 has a passive enclosure with no heater)
- MCU identification patterns (RP2040 toolhead -- QIDI dual-MCU signature)
- Build volume dimensions
- QIDI-specific G-code macros (`M141`, `M191`, `CLEAR_NOZZLE`)

No manual printer configuration is needed in most cases. HelixScreen identifies your QIDI model and applies the correct settings automatically.

## Print Start Tracking

HelixScreen uses the `qidi` print start profile to track progress through your printer's start sequence. The profile recognizes QIDI's typical startup phases:

1. Homing
2. Bed heating
3. Nozzle cleaning (`CLEAR_NOZZLE`)
4. Z tilt adjust
5. Bed mesh calibration
6. Nozzle heating
7. Chamber heating
8. Print begins

The progress bar updates as each phase completes, so you can see exactly where your printer is in its startup routine.

## WiFi

WiFi works out of the box on QIDI hardware — no manual driver or wpa_supplicant configuration needed for HelixScreen.

### Q2 WiFi Hardware (reference)

The Q2 uses a USB WiFi dongle from Tenda Technologies with a **Realtek RTL8188GU** chip (2.4 GHz only, 802.11n). The community-recommended driver is the out-of-tree [wandercn/RTL8188GU](https://github.com/wandercn/RTL8188GU) (DKMS module `RTL8188GUXX`); stock QIDI firmware already ships a working driver.

### WiFi Management

HelixScreen auto-selects the right backend:

- **wpa_supplicant backend** — used on stock QIDI firmware. Connects via the wpa_supplicant control socket, scans/connects/saves, and persists credentials via `SAVE_CONFIG`.
- **NetworkManager backend** — auto-detected if NetworkManager is installed (e.g., Armbian/OpenQIDI reflash).

The stock Q2 `QD_Q2/bin/client` binary also writes wpa_supplicant config directly. When HelixScreen replaces the stock UI, the installer takes over network management cleanly.

## QIDI Box (Filament Changer)

QIDI sells a 4-slot RFID-aware filament changer — the **QIDI Box** — for PLUS4, Q2, and MAX4 (not for Q1 Pro or X-Max 3). Chainable to 16 slots, active drying up to 65°C, MIFARE Classic RFID.

HelixScreen has a **stub backend** for the Box (`AmsType::QIDI_BOX`, `AmsBackendQidi`). The type round-trips through the enum, factory, and UI identify wizard, but no protocol is implemented — every operation logs a warning and reports not-supported. Detection is **not** wired up yet; the `"ams_type": "qidi_box"` capability on the Plus 4 and Q2 printer-database entries is informational today.

Full context, references to the `qidi-community/Plus4-Wiki` open-source reimplementation, and the follow-up work list live in [`FILAMENT_MANAGEMENT.md` → QIDI Box](../FILAMENT_MANAGEMENT.md#qidi-box-qidi-plus4--q2--max4).

Current Solution: Install community box firmware, [Bunny Box](https://github.com/Wazzup77/Bunny-Box) for box access. Most stock features currently work and extra features work while following the Bunny Box setup wiki.

Stock integration is blocked on test-hardware access.

## Known Limitations

- **Most older QIDI models have TJC HMI serial displays** -- The X-Max 3, X-Plus 3, Q1 Pro, and X-Smart 3 all use TJC (Nextion-compatible) displays connected via serial UART. HelixScreen cannot drive these. For on-device install, a physical screen replacement (HDMI or DSI touchscreen) is required. Remote-control mode is unaffected.
- **Q2 resolution is very small** -- The Q2's 480x272 display uses the MICRO layout. Some UI elements may be cramped but the layout is functional.
- **Q2 has limited RAM** -- ~498 MB total. HelixScreen must be memory-conscious on this device.
- **Plus 4 and Max 4 untested** -- Detection heuristics and display rendering for these models are based on specs. Community testers welcome.
- **No chamber heater control UI** -- QIDI printers have heated chambers, but HelixScreen doesn't yet have a dedicated chamber temperature control panel.

## Q2 Hardware Details

Gathered from firmware analysis and user reports:

- **SoC:** Quad-core ARM Cortex-A35 @ 1.1 GHz (aarch64), ~498 MB RAM, 32 GB eMMC
- **OS:** Debian Bullseye (glibc 2.31, kernel 5.10.160, systemd)
- **Main MCU:** STM32F407 (`QIDI_MAIN_V2`) via USB
- **Toolhead MCU:** STM32F103 via UART (`/dev/ttyS4`)
- **Display:** 4.3" 480x272 IPS capacitive (Goodix touch)
- **WiFi:** USB dongle, Realtek RTL8188GU (Tenda), 2.4 GHz only
- **SSH:** user `mks`, password `makerbase`
- **Klipper stack:** Standard Klipper + Moonraker (port 7125) + Fluidd, managed via systemd
- **Stock UI:** Closed-source binary at `/home/mks/QD_Q2/bin/client`, pinned to CPU core 0 via `taskset`

Firmware source reference: [53Aries/Q2-Firmware](https://github.com/53Aries/Q2-Firmware)

## Community Testing

We have a confirmed install report on the Q2 (2026-04). The one-line installer works, display and touch render correctly, and WiFi connects without manual configuration via the wpa_supplicant backend.

We still need testers for the **Plus 4** and **Max 4**. If you can help:

1. Build or download the aarch64 binary
2. Deploy it to your QIDI printer
3. Report back: Does it start? Does the display render? Does touch work? Is your printer detected correctly? Does WiFi work?
4. File issues at the HelixScreen GitHub repository

## Related Projects

- **[FreeDi](https://github.com/Phil1988/FreeDi)** -- Replaces QIDI's stock OS with Armbian and mainline Klipper. Recommended base OS for running HelixScreen on QIDI hardware.
- **[GuppyScreen](https://github.com/ballaswag/guppyscreen)** -- Another LVGL-based touchscreen display for Klipper printers.
- **[KlipperScreen](https://github.com/KlipperScreen/KlipperScreen)** -- Python/GTK-based display interface (typically requires an external monitor).
