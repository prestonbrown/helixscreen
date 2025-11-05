<p align="center">
  <img src="assets/images/helix-icon-64.png" alt="HelixScreen" width="128"/>
  <br>
  <h1 align="center">HelixScreen</h1>
  <p align="center"><em>A modern, lightweight touch interface for Klipper/Moonraker 3D printers</em></p>
</p>

<p align="center">
  <a href="https://github.com/prestonbrown/helixscreen/actions/workflows/build.yml"><img src="https://github.com/prestonbrown/helixscreen/actions/workflows/build.yml/badge.svg?branch=main" alt="Build"></a>
  <a href="https://github.com/prestonbrown/helixscreen/actions/workflows/quality.yml"><img src="https://github.com/prestonbrown/helixscreen/actions/workflows/quality.yml/badge.svg?branch=main" alt="Code Quality"></a>
  <a href="https://www.gnu.org/licenses/gpl-3.0"><img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPL v3"></a>
  <a href="https://lvgl.io/"><img src="https://img.shields.io/badge/LVGL-9.4.0-green.svg" alt="LVGL"></a>
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg" alt="Platform">
  <a href="https://en.cppreference.com/w/cpp/17"><img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17"></a>
</p>

HelixScreen is a next-generation printer control interface built from the ground up using LVGL 9's declarative XML system. Designed for embedded hardware with limited resources, it brings advanced Klipper features to printers that ship with restrictive vendor UIs.

**Built on proven foundations:**
- Based on [GuppyScreen](https://github.com/ballaswag/guppyscreen) architecture and design patterns
- Potential integration with [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen) features
- Modern XML-based UI with reactive data binding (LVGL 9.4)

**Key Goals:**
- 🚀 **More features** - Unlock Klipper's full potential beyond vendor limitations
- 💻 **Better hardware support** - Run on limited embedded devices (Pi, BTT Pad, vendor displays)
- 🎨 **Modern UI** - Clean, responsive touch interface with visual polish
- 📦 **Lightweight** - Minimal resource footprint for constrained hardware

## Target Hardware

- **Raspberry Pi** (Pi 3/4/5, Zero 2 W)
- **BTT Pad 7** / similar touch displays
- **Vendor printer displays** (Creality K1/K1 Max, FlashForge AD5M, etc.)
- **Generic Linux ARM/x64** with framebuffer support
- **Development simulator:** macOS/Linux desktop with SDL2

## Quick Start

### Install Dependencies

**Automated setup:**
```bash
make check-deps     # Check what's missing
make install-deps   # Auto-install missing dependencies (interactive)
```

**Manual setup (macOS):**
```bash
brew install sdl2 python3 node
npm install  # Install lv_font_conv and lv_img_conv
```

**Manual setup (Debian/Ubuntu):**
```bash
sudo apt install libsdl2-dev python3 clang make npm
npm install  # Install lv_font_conv and lv_img_conv
```

### Build & Run

```bash
# Build (parallel, auto-detects CPU cores)
make -j

# Run simulator (production mode - requires real hardware/printer)
./build/bin/helix-ui-proto

# Run in test mode (all components mocked - no hardware needed)
./build/bin/helix-ui-proto --test

# Test mode with selective real components
./build/bin/helix-ui-proto --test --real-moonraker      # Real printer, mock network
./build/bin/helix-ui-proto --test --real-wifi --real-files  # Real WiFi/files, mock rest

# Controls: Click navigation icons, press 'S' for screenshot
```

### Test Mode

The prototype includes a comprehensive test mode for development without hardware:

- **Production Mode** (default): Never uses mocks, requires real hardware
- **Test Mode** (`--test`): Uses mock implementations for all components
- **Selective Real Components**: Override specific mocks with `--real-*` flags:
  - `--real-wifi`: Use real WiFi hardware
  - `--real-ethernet`: Use real Ethernet hardware
  - `--real-moonraker`: Connect to real printer
  - `--real-files`: Use real files from printer

Test mode displays a banner showing which components are mocked vs real.

## Key Features

### Declarative XML UI System

Complete UI defined in XML files - no C++ layout code needed:

```xml
<!-- Define a panel in XML -->
<component>
  <view extends="lv_obj" style_bg_color="#bg_dark" style_pad_all="20">
    <lv_label text="Nozzle Temperature" style_text_color="#text_primary"/>
    <lv_label bind_text="temp_text" style_text_font="montserrat_28"/>
  </view>
</component>
```

```cpp
// C++ is pure logic - zero layout code
ui_panel_nozzle_init_subjects();
lv_xml_create(screen, "nozzle_panel", NULL);
ui_panel_nozzle_update(210);  // All bound widgets update automatically
```

### Reactive Data Binding

LVGL 9's Subject-Observer pattern enables automatic UI updates:
- **No manual widget management** - XML bindings handle everything
- **Type-safe updates** - One data change updates multiple UI elements instantly
- **Clean separation** - UI structure and business logic are independent

### Global Theme System

Change the entire UI appearance by editing one file (`ui_xml/globals.xml`):
```xml
<consts>
  <color name="primary_color" value="0xff4444"/>
  <color name="bg_dark" value="0x1a1a1a"/>
</consts>
```

## Architecture

```
XML Layout (ui_xml/*.xml)
    ↓ bind_text / bind_value / bind_flag
Reactive Subjects (lv_subject_t)
    ↓ lv_subject_set_* / copy_*
C++ Application Logic (src/*.cpp)
```

**Key Innovation:** The entire UI is defined in XML files. C++ code only handles initialization and reactive data updates—zero layout or styling logic.

## Documentation

- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Build system, dependencies, and daily workflow
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System design, data flow, and technical patterns
- **[CONTRIBUTING.md](CONTRIBUTING.md)** - Code standards, testing, and submission guidelines
- **[LVGL 9 XML Guide](docs/LVGL9_XML_GUIDE.md)** - Complete XML syntax reference
- **[Quick Reference](docs/QUICK_REFERENCE.md)** - Common patterns and code snippets
- **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)** - Build configuration and patches
- **[HANDOFF.md](HANDOFF.md)** - Current work status and priorities
- **[ROADMAP.md](docs/ROADMAP.md)** - Planned features and milestones

## License

GPL v3 - See individual source files for copyright headers.

## Acknowledgments

**HelixScreen builds upon:**
- **[GuppyScreen](https://github.com/ballaswag/guppyscreen)** - Core architecture and Moonraker integration patterns
- **[KlipperScreen](https://github.com/KlipperScreen/KlipperScreen)** - Feature inspiration and UI concepts

**Technology Stack:**
- **[LVGL 9.4](https://lvgl.io/)** - Light and Versatile Graphics Library
- **[Klipper](https://www.klipper3d.org/)** - Advanced 3D printer firmware
- **[Moonraker](https://github.com/Arksine/moonraker)** - Klipper API server
