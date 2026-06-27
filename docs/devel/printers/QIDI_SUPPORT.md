<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# QIDI Printer Support

HelixScreen supports the entire QIDI 3-series and 4-series in two distinct modes. Which mode you use depends on whether you want HelixScreen running on the printer's built-in screen, or on a separate touchscreen device (Raspberry Pi, repurposed tablet, etc.) controlling the printer over the network.

## Two Operating Modes

**Remote control (network client) -- works on every supported QIDI**

If your QIDI is running Klipper + Moonraker -- whether that's stock firmware, [FreeDi](https://github.com/Phil1988/FreeDi), [FreeQIDI](https://github.com/Phil1988/FreeQIDI), or another community stack -- HelixScreen can control it remotely from a separate touchscreen device. The printer's local display (TJC HMI or otherwise) is irrelevant in this mode; HelixScreen talks to Moonraker over WebSocket on port 7125 the same way Mainsail or Fluidd does.

All six QIDI models in the printer database (X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro, Plus 4, Q2) are auto-detected as remote targets. Just point HelixScreen at the printer's hostname or IP and the correct profile loads automatically.

**On-device install (replace the built-in UI)**

This mode requires a Linux framebuffer display. Most older QIDI models use TJC HMI displays (the Chinese Nextion OEM) connected over serial UART -- standalone MCU-driven screens that HelixScreen cannot drive without a physical screen swap. FreeDi targets that serial display from the mainboard side with its own firmware; HelixScreen can't.

Of the newer models, **Q2** and **Max 4** ship with Linux framebuffer displays and can run HelixScreen directly on the printer.

## Display Compatibility

QIDI uses two fundamentally different display architectures:

- **TJC HMI (serial)** -- A standalone microcontroller-driven display connected to the mainboard via serial UART. These are flashed with `.tft` firmware files via microSD card. HelixScreen **cannot** drive these displays. FreeDi targets this display type.
- **Linux framebuffer** -- A display driven directly by the Linux SoC via fbdev or DRM. HelixScreen **can** run on these.

### Why TJC HMI is structurally incompatible (not a missing feature)

The TJC panel on Plus 4 and the 3-series is part of QIDI's **MKS PI smart-panel** stack: the panel runs its own firmware and *is* the UI. The Klipper host pushes UI state and pre-rendered thumbnails to it over serial UART using TJC commands. Thumbnails go through `libColPic.so` — a closed-source ARM aarch64 encoder shipped at `/home/mks/libColPic.so` on stock QIDI firmware — which packs RGB565 buffers into a custom RLE+palette `.tjc` blob the panel can decode.

HelixScreen renders LVGL directly to a Linux framebuffer / DRM / SDL surface; it has no architecture for pushing UI to a remote serial HMI. Even if we added a TJC backend, the panel firmware would still be the actual UI — HelixScreen would be reduced to a thumbnail-encoder shim, not a touchscreen UI replacement. The two architectures don't meet halfway.

A community pure-Python reimplementation of `libColPic.so` exists (byte-for-byte verified, 30/30 cases against the original, by `Sib6019`, May 22 2026; full panel + encoder writeup in [`../printer-research/QIDI_PLUS_4_RESEARCH.md`](../printer-research/QIDI_PLUS_4_RESEARCH.md) § 0). It is useful to anyone driving the stock panel from host-side tooling (FreeDi, community firmware). It is **not** a path to running HelixScreen on the stock display.

The only way to run HelixScreen on-device on a Plus 4 or 3-series printer is to replace the MKS panel with a Linux-driven display (HDMI/DSI/SDL framebuffer). Remote-control mode is unaffected.

## Models

QIDI uses two generations of mainboard:

- **Older models (X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro, Plus 4):** MKSPI boards with Rockchip RK3328, ARM Cortex-A53 (aarch64), 1 GB RAM. These all use TJC HMI serial displays.
- **Newer models (Q2, Max 4):** New-generation boards with quad-core ARM Cortex-A35 (aarch64), ~498 MB RAM. Both drive Linux framebuffer displays from the SoC.

"On-device" below means whether HelixScreen can replace the printer's built-in display. **All six models work as remote targets regardless of this column.**

| Model | Display Type | Resolution | On-Device Install? | Notes |
|-------|-------------|------------|--------------------|-------|
| Max 4 | Linux framebuffer (5" capacitive) | 800x480 | **Likely yes** (untested) | Same new-gen mainboard as Q2, with a larger framebuffer display. |
| Q2 | Linux framebuffer (4.3" IPS capacitive) | 480x272 | **Yes** (confirmed) | Goodix touch controller. User-confirmed working install. WiFi requires wpa_supplicant backend (see below). |
| Plus 4 | TJC HMI (serial) | 800x480 | **No** | Panel identified: `TJC8048X250_011C_I_Z03`, firmware V1.65, CST3240 touch, driven by `xindi` daemon over `/dev/ttyS1 @ 115200`. Requires screen replacement. Auto-detected as remote target. |
| Q1 Pro | TJC HMI (serial) | 480x272 | **No** | Requires screen replacement. TJC model TJC4827X243_011. Auto-detected as remote target. |
| X-Max 3 | TJC HMI (serial) | 800x480 | **No** | Requires screen replacement (HDMI/DSI touchscreen). Auto-detected as remote target. |
| X-Plus 3 | TJC HMI (serial) | 800x480 | **No** | Requires screen replacement. Same display firmware as X-Max 3. Auto-detected as remote target. |
| X-Smart 3 | TJC HMI (serial) | 480x272 | **No** | Requires screen replacement. Smallest of the 3-series (175x180x170, passive enclosure, no active chamber heater). Auto-detected as remote target. |

## Remote Control (Network Client)

No QIDI-side install is needed. Run HelixScreen on a Raspberry Pi, repurposed Android tablet, or any other supported device, and add the QIDI printer by hostname or IP. Auto-detection identifies the model from Klipper objects, macros, hostname, and build volume. The right print start profile and capabilities load automatically.

This works on **stock firmware** (Q2, Max 4 all run standard Moonraker) and on **community stacks** like [FreeDi](https://github.com/Phil1988/FreeDi), [FreeQIDI](https://github.com/Phil1988/FreeQIDI), or [53Aries/Q2-Firmware](https://github.com/53Aries/Q2-Firmware) -- anything that exposes Moonraker on port 7125.

For the older 3-series (X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro, Plus 4), FreeDi is the easy path to a clean Klipper + Moonraker + Mainsail stack. FreeDi's own `FreeDiLCD` keeps the printer's local TJC display alive; HelixScreen runs separately on your touchscreen device and controls the printer over the network.

## Adding a HelixScreen Touchscreen to a TJC-Display QIDI

If your QIDI is X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro, Plus 4 — all ship with the MKS PI smart-panel (TJC HMI) — there are two paths to running HelixScreen on-printer.

**Path A (recommended): add a separate touchscreen device.** Leave the stock panel where it is (or unplug it and ignore it). Mount a small Linux-driven touchscreen alongside the printer; it runs HelixScreen and talks to the printer's Moonraker over WiFi on port 7125. Same end-state UI, an hour of setup, no chassis work.

**Path B (hard, niche): swap the internal panel.** Pull the TJC panel, wire an SBC and a DSI/HDMI display into the original cutout, share power off the printer's 5V rail. Several FreeDi-community builds have done this. Multi-evening hardware project (case modification, EMI shielding from steppers, cable routing) for the same software outcome as Path A. Worth it only if preserving the original aesthetic matters more than your time.

### Path A hardware (ranked by ease)

1. **Android tablet you already own** — HelixScreen ships an Android APK. Sideload it, point at the printer's IP, mount the tablet with whatever bracket you like. Zero hardware purchase. Best path if a spare tablet is sitting in a drawer.
2. **BTT Pad 7 (or similar CB1/CB2-based standalone)** — purpose-built 7" touchscreen with an aarch64 SBC built in. Runs the standard Pi build of HelixScreen. ~$100–150. Power over USB-C, mount on a screen arm, done.
3. **Raspberry Pi 4 (or 5) + official 7" DSI touchscreen** — the reference setup. Pi 4 with 2 GB is plenty; Pi 5 if you want headroom. HelixScreen's DRM backend works out of the box on Bookworm. ~$120–180. Lots of community enclosures on Printables.
4. **Raspberry Pi Zero 2 W + small HDMI/SPI touchscreen** — cheapest path, fits in tight enclosures. 512 MB RAM covers HelixScreen but leaves no headroom for plugins or timelapse. ~$50.
5. **Repurpose a SonicPad, AD5M, or other supported device** — anything in the hardware-support table works. Point it at the QIDI and run.

### Setup

The screen-side device runs the standard one-line installer (see [`../../user/INSTALL.md`](../../user/INSTALL.md)). The printer side needs no changes — stock QIDI firmware already exposes Moonraker on port 7125. Auto-detection picks up the Plus 4 / 3-series model via hostname or QIDI-specific macros and applies the correct preset.

If you want HelixScreen to autostart on the screen-side device, follow the platform-specific setup in `INSTALL.md` (typically a systemd service on a Pi).

## On-Device Installation

This section is for replacing the printer's built-in display with HelixScreen running directly on the printer. Only supported on Linux-framebuffer models (see table above).

### Prerequisites

- A QIDI printer with a Linux framebuffer display (Q2, Max 4 — **not** Plus 4, see Display Compatibility above)
- SSH access to the printer
- Stock firmware works directly. Community firmware like [53Aries/Q2-Firmware](https://github.com/53Aries/Q2-Firmware) or [FreeQIDI](https://github.com/Phil1988/FreeQIDI) may also be used.

### One-Line Installer (Recommended)

The standard HelixScreen installer works on QIDI hardware out of the box. SSH into the printer and run:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer auto-detects QIDI-class SBCs (hostname `linaro-alip` + `/home/mks` on stock Q2; the same Pi/aarch64 binary covers Max 4) and sets up the systemd service, launcher, and config under the correct user.

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

HelixScreen auto-detects all six supported QIDI models (X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro, Plus 4, Q2) using several heuristics:

- Hostname patterns (`qidi`, `x-max`, `x-plus`, `x-smart`, `xsmart`, `q1`, `plus4`, `plus-4`)
- Active chamber heater presence (X-Plus 3, X-Max 3, Q1 Pro, Plus 4 -- X-Smart 3 has a passive enclosure with no heater)
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
- **NetworkManager backend** — auto-detected if NetworkManager is installed (e.g., Armbian/FreeQIDI reflash).

The stock Q2 `QD_Q2/bin/client` binary also writes wpa_supplicant config directly. When HelixScreen replaces the stock UI, the installer takes over network management cleanly.

## QIDI Box (Filament Changer)

QIDI sells a 4-slot RFID-aware filament changer — the **QIDI Box** — for Plus 4, Q2, and MAX4 (not for Q1 Pro or X-Max 3). Chainable to 16 slots, active drying up to 65°C, MIFARE Classic RFID.

HelixScreen has a **read-only state mirror** and a **gated write-path** for the Box (`AmsType::QIDI_BOX`, `AmsBackendQidi`). Issue #954 contributed the Python sources for the stock Klipper extensions (`box_extras`, `box_stepper`, `box_rfid`, `aht20_f`, `box_heater_fan`, `box_detect`, `buttons_irq`) which were the protocol reference for this work.

**What works today:**

- **Detection** — `PrinterDiscovery` recognises `box_stepper slot<N>` Klipper objects and registers `AmsType::QIDI_BOX`. Slot count derives from how many `box_stepper slot<N>` sections exist (4/8/12/16). Happy Hare takes priority if both are present.
- **State mirror** — subscribes to `notify_status_update` and parses `save_variables.variables`:
  - `enable_box` → unit connected/disconnected
  - `box_count` → resizes the slot vector (4 per box, up to 16)
  - `slot<N>` → per-slot `SlotStatus` (0=empty, 1=available, 2=loaded, 3=transitional, -1/-2/-3=BLOCKED)
  - `value_t<N>="slot<M>"` → tool-to-slot mapping
  - `last_load_slot` → authoritative LOADED (with `"slot-1"` = nothing loaded)
  - `filament_slot<N>` / `color_slot<N>` / `vendor_slot<N>` → raw RFID indices in a private side-table
- **Temperature profiles** — fetches `/server/files/config/officiall_filas_list.cfg` via Moonraker's file API at `on_started()`. Parses the ConfigParser INI sections (`[fila<N>]` with `min_temp` / `max_temp` / `box_min_temp` / `box_max_temp`), caches them, and applies the nozzle min/max to `SlotInfo` whenever a `filament_slot<N>` index arrives. HTTP failure is non-fatal.
- **Bootstrap** — `on_started()` issues a `printer.objects.query` for `save_variables` + `box_extras` so the initial snapshot lands; subsequent `notify_status_update` frames carry deltas only.
- **Heater drying state** — `heater_generic heater_box<N>` notifications (temperature/target) flow into `AmsUnit::environment` as the max across all boxes, so the UI can show drying-active state regardless of which physical box is active.
- **Write-path** — always enabled: `load_filament` (`T<tool>`), `unload_filament` (`UNLOAD_T<tool>`, supports `-1` for active slot), `change_tool` (`T<tool>`), and `set_tool_mapping` (`SAVE_VARIABLE VARIABLE=value_t<t> VALUE="slot<s>"`). Commands verified against QIDI's open-source firmware (`box_stepper.py`/`box_extras.py`, #1030). Each op logs at `info` (entry log + raw G-code) for field visibility.

**Known gaps:**

- `aht20_f heater_box<N>` humidity isn't subscribed today — the classifier in `moonraker_discovery_sequence.cpp` only recognises `temperature_sensor <name>`/`temperature_fan <name>`/`tmc2240`/`tmc5160` as sensors. The parser handles humidity if it arrives, but the subscription side has to be extended.
- `color_slot<N>` / `vendor_slot<N>` raw indices are captured but not yet mapped to material/color/brand strings — the python source doesn't read those names from `officiall_filas_list.cfg`, so the palette source needs separate identification.
- No `qidi_box_64.png` logo asset yet — `AmsState::get_system_logo_path()` returns nullptr for `"qidi box"`, UI falls back to a generic AMS chip.
- Write-path needs field validation against real hardware. Tracking via issue #954.

Full context and references to the `qidi-community/Plus4-Wiki` open-source reimplementation live in [`FILAMENT_MANAGEMENT.md` → QIDI Box](../FILAMENT_MANAGEMENT.md#qidi-box-qidi-plus4--q2--max4).

**Alternative path: [Bunny Box](https://github.com/Wazzup77/Bunny-Box)** — a community open-source replacement that reimplements the QIDI Box as a [Happy Hare](https://github.com/moggieuk/Happy-Hare) MMU. HelixScreen already has Happy Hare support, so a printer flashed with Bunny Box is controllable through HelixScreen via its existing Happy Hare integration. Plus 4 is the most mature target (tested on stock QIDI 1.7.3, FreeDi, and Kalico); Q2 is in active testing; Max 4 is not yet supported. Bunny Box currently depends on the maintainer's [Happy Hare fork](https://github.com/Wazzup77/Happy-Hare) for QIDI-specific hall-sensor and cutter handling, pending upstream merge.

HelixScreen ships a **Q2 Happy Hare preset** (`presets/qidi_q2.json`) for exactly this path — a Q2 running Happy Hare (e.g. Bunny Box) auto-detects and loads with the Happy Hare MMU pre-gate sensors and heaters wired up. The Q2 install and WiFi were confirmed working by a community user; HelixScreen owns no QIDI hardware, so the Q2 path is community-validated rather than team-tested (prestonbrown/helixscreen#997).

## Known Limitations

- **Most QIDI models have TJC HMI serial displays** -- The X-Max 3, X-Plus 3, Q1 Pro, X-Smart 3, and **Plus 4** all use TJC (Nextion-compatible) displays connected via serial UART. HelixScreen cannot drive these. For on-device install, a physical screen replacement (HDMI or DSI touchscreen) is required. Remote-control mode is unaffected.
- **Q2 resolution is very small** -- The Q2's 480x272 display uses the MICRO layout. Some UI elements may be cramped but the layout is functional.
- **Q2 has limited RAM** -- ~498 MB total. HelixScreen must be memory-conscious on this device.
- **Max 4 untested** -- Detection heuristics and display rendering for this model are based on specs. Community testers welcome.
- **No mmu heater control UI** -- The Qidi Box has a dedicated heater, but HelixScreen doesn't yet have a dedicated MMU temp widget.

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

We still need testers for the **Max 4** (on-device install). If you can help:

1. Build or download the aarch64 binary
2. Deploy it to your QIDI printer
3. Report back: Does it start? Does the display render? Does touch work? Is your printer detected correctly? Does WiFi work?
4. File issues at the HelixScreen GitHub repository

## Related Projects

- **[FreeDi](https://github.com/Phil1988/FreeDi)** -- Replaces QIDI's stock OS with Armbian and mainline Klipper. Recommended base OS for running HelixScreen on QIDI hardware.
- **[GuppyScreen](https://github.com/ballaswag/guppyscreen)** -- Another LVGL-based touchscreen display for Klipper printers.
- **[KlipperScreen](https://github.com/KlipperScreen/KlipperScreen)** -- Python/GTK-based display interface (typically requires an external monitor).
