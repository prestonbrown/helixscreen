<p align="center">
  <img src="assets/images/helix-icon-256.png" alt="HelixScreen" width="128"/>
  <br>
  <h1 align="center">HelixScreen</h1>
  <p align="center"><strong>A modern touch interface for Klipper 3D printers</strong></p>
  <p align="center"><a href="https://helixscreen.org">helixscreen.org</a></p>
</p>

<p align="center">
  <a href="https://github.com/prestonbrown/helixscreen/actions/workflows/build.yml"><img src="https://github.com/prestonbrown/helixscreen/actions/workflows/build.yml/badge.svg?branch=main" alt="Build"></a>
  <a href="https://github.com/prestonbrown/helixscreen/actions/workflows/quality.yml"><img src="https://github.com/prestonbrown/helixscreen/actions/workflows/quality.yml/badge.svg?branch=main" alt="Code Quality"></a>
  <a href="https://www.gnu.org/licenses/gpl-3.0"><img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPL v3"></a>
  <a href="https://lvgl.io/"><img src="https://img.shields.io/badge/LVGL-9.5-green.svg" alt="LVGL"></a>
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg" alt="Platform">
  <a href="https://github.com/prestonbrown/helixscreen/releases"><img src="https://img.shields.io/github/v/release/prestonbrown/helixscreen?label=version" alt="Version"></a>
  <a href="https://discord.gg/RZCT2StKhr"><img src="https://img.shields.io/discord/1472057991346782238?logo=discord&label=Discord&color=5865F2" alt="Discord"></a>
</p>

Your printer can do way more than your current touchscreen lets you. Bed mesh visualization, input shaper graphs, multi-material management, print history — it's all trapped in a browser tab. HelixScreen puts it at your fingertips.

Fast, beautiful, and frugal enough to run on hardware you already own — your printer's onboard SoC, a Raspberry Pi from a drawer, or anything newer.

---

**Quick Links:** [Website](https://helixscreen.org) · [Features](#features) · [Screenshots](#screenshots) · [Installation](#installation) · [User Guide](docs/user/USER_GUIDE.md) · [FAQ](#faq) · [Contributing](CONTRIBUTING.md) · [Changelog](CHANGELOG.md) · [Discord](https://discord.gg/RZCT2StKhr)

---

## Why HelixScreen?

- **Customizable dashboard** — Multi-page grid with drag-to-reposition, edge resize, and 30+ widgets including temperature graphs, fan arcs, and power toggles
- **Every feature at your fingertips** — 30+ panels, 20+ overlays, 20+ modals, 300+ XML layouts
- **~15MB RAM on embedded targets, ~75MB disk** — sips memory on a Creality K1 or Flashforge AD5M; a few times more on 64-bit Pi, still well under what other touchscreen UIs need. Your printer's onboard SoC or an older Pi is plenty — no need to buy new hardware.
- **80+ printers in the database** — Auto-detects your hardware and configures itself
- **Multi-material ready** — AFC, Happy Hare, ACE, AD5X IFS, CFS, Snapmaker U1, tool changers, Spoolman
- **Exclude objects** — Tap-to-exclude overhead map view with object outlines during prints
- **Looks great** — Light/dark themes with 17 presets, responsive layouts, GPU-accelerated blur
- **First-run wizard** — Guided setup discovers your printer's capabilities
- **9 languages** — English, German, Spanish, French, Italian, Japanese, Portuguese, Russian, and Chinese

<details>
<summary><strong>Technical comparison</strong></summary>

| Feature | HelixScreen | GuppyScreen | KlipperScreen |
|---------|-------------|-------------|---------------|
| UI Framework | LVGL 9 XML | LVGL 8 C | GTK 3 (Python) |
| Declarative UI | Full XML with reactive bindings | C only | Python only |
| RAM Usage | ~15MB (32-bit) | ~15-20MB | ~50MB |
| Disk Size | ~75-115MB | ~60-80MB | ~50MB |
| Multi-Material | 7 backends | Limited | Basic |
| Printer Database | 80+ models | — | Manual config |
| Display Layouts | Auto-detecting (tiny to ultrawide) | Fixed | Configurable |
| Internationalization | 9 languages | — | 40+ languages |
| Status | 1.0 (active) | Inactive | Mature (maintenance) |
| Language | C++17 | C | Python 3 |

</details>

## Screenshots

### Home Panel
<img src="docs/images/screenshot-home-panel.png" alt="Home Panel" width="800"/>

### Print File Browser
<img src="docs/images/screenshot-print-select-card.png" alt="Print Select" width="800"/>

### Bed Mesh Visualization
<img src="docs/images/screenshot-bed-mesh-panel.png" alt="Bed Mesh" width="800"/>

<details>
<summary><strong>More screenshots</strong></summary>

### Controls Panel
<img src="docs/images/screenshot-controls-panel.png" alt="Controls Panel" width="800"/>

### Motion Controls
<img src="docs/images/screenshot-motion-panel.png" alt="Motion Controls" width="800"/>

### AMS / Filament Management
<img src="docs/images/screenshot-ams-panel.png" alt="AMS Panel" width="800"/>

### Input Shaper Results
<img src="docs/images/screenshot-shaper-results.png" alt="Input Shaper Results" width="800"/>

### PID Tuning
<img src="docs/images/screenshot-pid-panel.png" alt="PID Tuning" width="800"/>

### Settings
<img src="docs/images/screenshot-settings-panel.png" alt="Settings" width="800"/>

### First-Run Wizard
<img src="docs/images/screenshot-wizard-wifi.png" alt="Setup Wizard" width="800"/>

</details>

See [docs/devel/GALLERY.md](docs/devel/GALLERY.md) for the full gallery.

## Features

**Dashboard** — Customizable multi-page grid with drag-to-reposition, edge resize, and a catalog of 30+ widgets. Temperature graphs, fan arcs, power toggles, camera feeds, active spool, favorite macros — add what matters, hide what doesn't. Per-breakpoint layout persistence.

**Printer Control** — Print management with G-code preview, motion controls, temperature presets with per-material overrides, multi-fan control, Z-offset, speed/flow tuning, live filament consumption tracking, power device management.

**Multi-Material** — 7 filament system backends: AFC (Box Turtle, ViViD), Happy Hare (ERCF, 3MS, Tradrack, Night Owl), ACE (Anycubic ACE Pro), AD5X IFS, Creality CFS, Snapmaker U1 (with RFID spool recognition), and tool changers. Multi-unit and multi-backend support. Full Spoolman integration with spool creation wizard.

**Visualization** — 3D G-code layer preview with memory-aware geometry budgets, 3D bed mesh with async rendering, print thumbnails, frequency response charts, unified temperature graph.

**Calibration** — Input shaper with frequency response charts, PID tuning with live graph, MPC calibration (Kalico), belt tension tuning, bed mesh, screws tilt adjust, Z-offset, firmware retraction, probe management.

**Integrations** — HelixPrint plugin, power devices with quick-toggle, print history, timelapse (Moonraker plugin), exclude objects with tap-to-exclude map view, LED control (5 backends), sound alerts (SDL/PWM/M300), Bluetooth label printing (Brother QL/PT, Niimbot, MakeID).

**Display** — Auto-detecting layout system (480x320 to 1920x480 ultrawide), display rotation (0/90/180/270) with auto-detection, light/dark themes with 17 presets and live theme editor, GPU-accelerated backdrop blur, screensavers.

**System** — First-run wizard with guided hardware discovery, 80+ printer models with auto-detection, 9 languages, opt-in crash reporting with debug bundles, KIAUH installer, versioned config migration.

## Supported Platforms

| Platform | Architecture | Status |
|----------|-------------|--------|
| Raspberry Pi 3/4/5, CM4, Zero 2 W (64-bit) | aarch64 | Tested |
| Raspberry Pi 3/4 (32-bit) | armhf | Tested |
| BTT Pad / CB1 / CB2 / Manta | aarch64 | Tested |
| Creality K1 / K1C / K1 Max | MIPS32 | Tested |
| Creality K2 Max / K2 Plus | ARM (musl) | Tested |
| Creality Sonic Pad | armhf | Tested |
| Flashforge AD5M / AD5M Pro | armv7-a | Tested |
| Flashforge AD5X | MIPS32 | Tested |
| Snapmaker U1 (SnapSwap toolchanger) | aarch64 | Tested |
| QIDI Q2, Plus 4, Max 4 | aarch64 | Supported¹ |
| Sovol SV06 / SV08 | Pi build | Tested |
| Elegoo Centauri Carbon | armv7-a | Tested² |
| x86 Mini PC (Debian) | x86_64 | Tested |
| macOS / Linux desktop | x86_64 / ARM64 | Development / CI |

¹ QIDI models with Linux framebuffer displays (Q2, Max 4) only. Stock firmware runs standard Moonraker and works directly; community firmware like [FreeDi](https://github.com/Phil1988/FreeDi), [53Aries/Q2-Firmware](https://github.com/53Aries/Q2-Firmware), or [FreeQIDI](https://github.com/Phil1988/FreeQIDI) is optional. Older models (X-Smart 3, X-Plus 3, X-Max 3, Q1 Pro, Plus4) use TJC serial displays and are **not compatible** without a screen replacement.

² Elegoo Centauri Carbon requires the community [OpenCentauri COSMOS](https://github.com/OpenCentauri/cosmos) firmware ([docs](https://docs.opencentauri.cc/klipper-conversion/cosmos/cosmos/); stock Elegoo firmware has no SSH, Klipper, or Moonraker). Ships with factory white-balance calibration for the 4.3" panel.

## Installation

> **Run these commands on your printer's host computer, not your local machine.**
> SSH into your Raspberry Pi, BTT board, or similar host. For all-in-one printers (Creality K1/K2, Flashforge AD5M/Pro), SSH directly into the printer.

**One-line install:**
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer auto-detects your platform, downloads the correct binary, sets up the service, and launches the first-run wizard. To update:
```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --update
```

Also available through [KIAUH](https://github.com/dw-0/kiauh) as an extension.

**Flashforge AD5M/Pro:** We provide a [ready-made firmware image](https://github.com/prestonbrown/ff5m) (Forge-X fork with HelixScreen pre-configured) — just flash from a USB drive. Or install manually on an existing Forge-X/Klipper Mod setup.

See the [Installation Guide](docs/user/INSTALL.md) for detailed instructions, display configuration, and troubleshooting.

## Development

```bash
# Check/install dependencies
make check-deps && make install-deps

# Build
make -j

# Run with mock printer (no hardware needed)
./build/bin/helix-screen --test -vv

# Run with real printer
./build/bin/helix-screen

# XML hot reload (edit XML, switch panels to see changes live)
HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv

# Run tests
make test-run
```

**Controls:** Click navigation icons, press 'S' for screenshot, use `-v` (INFO), `-vv` (DEBUG), or `-vvv` (TRACE) for logging.

**Test suite:** 5,000+ test cases across 460+ test files covering printer state, UI components, XML parsing, multi-material, and more.

See [docs/devel/DEVELOPMENT.md](docs/devel/DEVELOPMENT.md) for detailed setup, cross-compilation, and contributing guidelines.

## FAQ

**How is this different from GuppyScreen/KlipperScreen?**
More features, far lower RAM use (~15MB on embedded targets vs ~50MB for KlipperScreen), and actively developed. The lighter footprint means the printer you have or a Pi you've owned for years is plenty — no need to chase new SBC hardware. See the [comparison table](#why-helixscreen).

**Which printers are supported?**
Any Klipper + Moonraker printer. 80+ models in the auto-detection database spanning Voron, Creality, QIDI, Anycubic, Flashforge, Sovol, RatRig, FLSUN, Elegoo, Prusa, Snapmaker, and more. The wizard auto-discovers your printer's capabilities even if it's not in the database.

**What screen sizes are supported?**
480x320, 800x480, 1024x600, and 1920x480 (ultrawide) with auto-detecting layouts. Display rotation (0/90/180/270) with auto-detection.

**What multi-material systems work?**
AFC (Box Turtle, ViViD), Happy Hare (ERCF, 3MS, Tradrack, Night Owl), ACE (Anycubic ACE Pro), AD5X IFS, Creality CFS, Snapmaker U1 (with RFID spool recognition), and tool changers (viesturz/klipper-toolchanger). Full Spoolman integration for spool management.

See [docs/user/FAQ.md](docs/user/FAQ.md) for the full FAQ.

## Troubleshooting

| Issue | Solution |
|-------|----------|
| SDL2 or build tools missing | `make install-deps` |
| Submodule empty | `git submodule update --init --recursive` |
| Can't connect to Moonraker | Check IP/port in settings.json |
| Wizard not showing | Delete settings.json to trigger it |
| Display upside down | Set rotation in settings or check `panel_orientation` in `/proc/cmdline` |

See [docs/user/TROUBLESHOOTING.md](docs/user/TROUBLESHOOTING.md) for more solutions, or open a [GitHub issue](https://github.com/prestonbrown/helixscreen/issues).

## Documentation

### User Guides
| Guide | Description |
|-------|-------------|
| [Installation](docs/user/INSTALL.md) | Setup for Pi, Sonic Pad, K1, K2, AD5M, AD5X, QIDI |
| [User Guide](docs/user/USER_GUIDE.md) | Using HelixScreen — panels, overlays, settings |
| [Configuration](docs/user/CONFIGURATION.md) | All settings with examples |
| [Upgrading](docs/user/UPGRADING.md) | Version upgrade instructions |
| [FAQ](docs/user/FAQ.md) | Common questions |
| [Troubleshooting](docs/user/TROUBLESHOOTING.md) | Problem solutions |
| [Telemetry & Privacy](docs/user/TELEMETRY.md) | What data is collected (opt-in) |

### Developer Guides
| Guide | Description |
|-------|-------------|
| [Development](docs/devel/DEVELOPMENT.md) | Build system, workflow, contributing |
| [Architecture](docs/devel/ARCHITECTURE.md) | System design, patterns |
| [LVGL9 XML Guide](docs/devel/LVGL9_XML_GUIDE.md) | XML syntax reference |
| [UI Contributor Guide](docs/devel/UI_CONTRIBUTOR_GUIDE.md) | Breakpoints, tokens, colors, widgets |
| [Changelog](CHANGELOG.md) | Release history |
| [Roadmap](docs/devel/ROADMAP.md) | Feature timeline |

## Community

**[Join the HelixScreen Discord](https://discord.gg/RZCT2StKhr)** — Get help, share your setup, request features, and follow development.

**Also discussed in:**
- [FuriousForging Discord](https://discord.gg/Cg4yas4V) — #mods-and-projects ([jump to HelixScreen topic](https://discord.com/channels/1323351124069191691/1444485365376352276))
- [VORONDesign Discord](https://discord.gg/voron) — #voc_works ([jump to HelixScreen topic](https://discord.com/channels/460117602945990666/1468467369407156346))

### Co-Maintainers Wanted

We're looking for co-maintainers to help grow HelixScreen! You can contribute broadly across the project or own a specific area that interests you:

- **Printer support** — Maintain builds and testing for specific platforms (Creality, QIDI, Flashforge, etc.)
- **Multi-material backends** — Own a filament system integration (AFC, Happy Hare, ACE, CFS, etc.)
- **UI/UX** — Help design and implement panels, overlays, and responsive layouts
- **Localization** — Maintain translations for your language
- **Documentation** — Keep guides accurate and help new users get started
- **Testing & CI** — Expand the test suite and maintain build infrastructure

If you're interested, join the [Discord](https://discord.gg/RZCT2StKhr) and introduce yourself, or open a [GitHub Discussion](https://github.com/prestonbrown/helixscreen/discussions).

**Bug Reports & Feature Requests:** [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues) — please include your printer model and logs (`helix-screen -vv`) when reporting bugs.

## License

GPL v3 — See [LICENSE](LICENSE) for details.

## Acknowledgments

**Inspired by:** [GuppyScreen](https://github.com/ballaswag/guppyscreen) (general architecture, LVGL-based approach), [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen) (feature inspiration)

**Built with:** [LVGL 9.5](https://lvgl.io/), [Klipper](https://www.klipper3d.org/), [Moonraker](https://github.com/Arksine/moonraker), [libhv](https://github.com/ithewei/libhv), [spdlog](https://github.com/gabime/spdlog), [SDL2](https://www.libsdl.org/)

**AI-Assisted Development:** Built with [Claude Code](https://github.com/anthropics/claude-code) by [Anthropic](https://www.anthropic.com/)
