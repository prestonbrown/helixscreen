# Development Guide

This document covers development environment setup, build processes, and daily development workflows for the HelixScreen prototype.

## Development Environment Setup

### Prerequisites by Platform

**macOS (Homebrew):**
```bash
brew install cmake bear imagemagick python3 node
# Optional (auto-built from submodules if not installed):
#   brew install sdl2 spdlog libhv
npm install         # Install lv_font_conv and lv_img_conv
make venv-setup     # Set up Python venv with pypng and lz4 for icon conversion
```

**Minimum macOS Version:** macOS 10.15 (Catalina) or newer required for CoreWLAN/CoreLocation WiFi APIs. The build system enforces this via `-mmacosx-version-min=10.15` deployment target.

**Note:** SDL2, spdlog, and libhv can be installed via package manager OR built automatically from submodules. The build system detects and uses system versions when available (preferred for faster builds).

**Debian/Ubuntu (apt):**
```bash
sudo apt install cmake bear imagemagick python3 clang make npm
# Optional (auto-built from submodules if not installed):
#   sudo apt install libsdl2-dev spdlog libhv-dev
npm install         # Install lv_font_conv and lv_img_conv
make venv-setup     # Set up Python venv with pypng and lz4 for icon conversion
```

**Fedora/RHEL/CentOS (dnf):**
```bash
sudo dnf install cmake bear ImageMagick python3 clang make npm
# Optional (auto-built from submodules if not installed):
#   sudo dnf install SDL2-devel spdlog-devel libhv-devel
npm install         # Install lv_font_conv and lv_img_conv
make venv-setup     # Set up Python venv with pypng and lz4 for icon conversion
```

### Dependency Overview

**Core Dependencies (Required):**
- **`clang`** - C/C++ compiler with C++17 support
- **`cmake`** - Build system for SDL2 (version 3.16+)
- **`make`** - GNU Make build system
- **`python3`** - Icon generation scripts and build tools
  - **`pypng`** / **`lz4`** - Python packages for PNG to C array conversion (installed via `make venv-setup` into `.venv/`)
- **`node`** / **`npm`** - Package manager for JavaScript dependencies
- **`lv_font_conv`** - Font converter (installed via `npm install`)

**Optional (Auto-Built from Submodules if Not Installed):**
- **SDL2** - Display simulator (auto-built from submodule if not system-installed)
- **spdlog** - Logging library (header-only, auto-uses submodule if not system-installed)
- **libhv** - WebSocket/HTTP client (auto-built from submodule if not system-installed)

**Always Built from Submodules:**
- **lvgl** - LVGL 9.4 graphics library (project-specific patches, must use submodule)
- **TinyGL** - Software 3D rasterizer for G-code visualization (optional, enabled by default via `ENABLE_TINYGL_3D=yes`)

**Development Tools (Optional):**
- **`bear`** - Generates `compile_commands.json` for IDE/LSP support
- **`imagemagick`** - Screenshot conversion and icon generation

### Automated Dependency Management

```bash
make check-deps      # Check what's missing (includes Python venv check)
make install-deps    # Auto-install (interactive, includes venv setup)
make venv-setup      # Manually set up Python venv with pypng and lz4
```

The build system automatically:
- Checks for Python virtual environment (`.venv/`)
- Verifies pypng and lz4 packages are installed
- Sets up the venv when running `make install-deps`

For complete dependency management details, see **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)**.

## Build System

### Quick Build Commands

```bash
# Parallel incremental build (recommended for daily development)
make -j

# Clean parallel build with progress and timing
make build

# Run after building
./build/bin/helix-ui-proto

# Run with specific theme mode
./build/bin/helix-ui-proto --dark    # Force dark mode
./build/bin/helix-ui-proto --light   # Force light mode

# Generate IDE support (one-time setup)
make compile_commands

# Clean rebuild (only when needed)
make clean && make -j

# Build without 3D rendering support (smaller binary, faster builds)
make -j ENABLE_TINYGL_3D=no
```

**Build Configuration:** See [BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md) for additional build options like `ENABLE_TINYGL_3D` and `V=1` (verbose mode).

### Theme Mode Control

The UI supports dark and light themes:

```bash
# Use stored preference from config file (default)
./build/bin/helix-ui-proto

# Override with dark mode
./build/bin/helix-ui-proto --dark

# Override with light mode
./build/bin/helix-ui-proto --light
```

Theme preference is saved to `helixconfig.json` and persists across launches unless overridden by command-line flags.

### Keyboard

The on-screen keyboard features:
- Number row always visible (1-0)
- QWERTY layout with backspace on row 4 (right of M, above Enter)
- Long-press keys for alternative characters (e.g., hold 'a' for '@', '1' for '%')
- Mode switching: ?123 for symbols, ABC to return, Shift for uppercase
- Visual key feedback on press (pop-overs)

No configuration needed - the keyboard is always enabled and ready to use.

### Build System Features

- **Auto-parallel builds** - Detects CPU cores automatically
- **Color-coded output** - `[CXX]` (blue), `[CC]` (cyan), `[LD]` (magenta)
- **Incremental compilation** - Only rebuilds changed files
- **Automatic patch application** - LVGL patches applied transparently

**Verbose mode:**
```bash
make V=1  # Shows full compiler commands
```

**For advanced features** (fonts, icons, patches, troubleshooting), see **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)**.

## Configuration File Management

**Pattern:** User-specific config files are git-ignored. Reference/default values live in `data/` directory as templates.

### Template-Based Configuration

```bash
# First time setup: Copy template to create user config
cp config/helixconfig.json.template helixconfig.json

# Template is versioned, user config is git-ignored
git status
# helixconfig.json appears in .gitignore
```

**Why this pattern:**
- **helixconfig.json** (top-level) - User-specific settings, git-ignored
- **config/helixconfig.json.template** - Default values, versioned in git
- Prevents accidental commits of user-specific settings (API keys, local paths, preferences)
- Provides reference defaults for new developers/installations

### Adding New Config Fields

When adding new configuration options:

1. **Add to template** (`config/helixconfig.json.template`)
   ```json
   {
     "config_path": "helixconfig.json",
     "dark_mode": false,          // NEW: Theme preference
     "default_printer": "...",
     ...
   }
   ```

2. **Document in code** (src/config.cpp or relevant module)
   ```cpp
   // Read theme preference (default: false = light mode)
   bool dark_mode = config->get<bool>("/dark_mode", false);
   ```

3. **Update user's config** (automatic via config->save() or manual copy from template)

**Important:** NEVER commit `helixconfig.json` (top-level). Only commit template changes.

## Multi-Display Development (macOS)

Control which display the UI window appears on:

```bash
./build/bin/helix-ui-proto --display 1     # Display 1 (secondary)
./build/bin/helix-ui-proto -d 1 -s small   # Combined options
```

For complete multi-display details, see **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)**.

## DPI Configuration & Hardware Profiles

### Overview

LVGL's theme system scales UI elements based on display DPI (dots per inch). This ensures consistent physical sizing across different screen densities—a button should be roughly the same physical size whether displayed on a low-DPI 7" screen or a high-DPI 4.3" screen.

**Key Formula:**
```c
// LVGL DPI scaling: LV_DPX_CALC(dpi, value) = (dpi * value + 80) / 160
// Reference DPI: 160 (no scaling)
// Example: PAD_DEF=12 @ 160 DPI → 12 pixels
//          PAD_DEF=12 @ 187 DPI → 14 pixels (scaled up)
```

### Default DPI

**Default: 160 DPI** (defined in `lv_conf.h` as `LV_DPI_DEF`)

This is LVGL's reference DPI where theme padding values (12/16/20) are used exactly as specified with no scaling. Chosen as a conservative baseline that doesn't assume high-density displays.

### Testing Different Hardware Profiles

Use the `--dpi` flag to test how the UI will appear on target hardware:

```bash
# Reference DPI (160) - no scaling
./build/bin/helix-ui-proto --dpi 160

# 7" @ 1024x600 (170 DPI) - BTT Pad 7, similar displays
./build/bin/helix-ui-proto -s medium --dpi 170

# 5" @ 800x480 (187 DPI) - Common 5" LCD panels
./build/bin/helix-ui-proto -s medium --dpi 187

# 4.3" @ 720x480 (201 DPI) - FlashForge AD5M, compact screens
./build/bin/helix-ui-proto -s small --dpi 201
```

### Target Hardware DPI Reference

| Hardware | Size | Resolution | DPI | Notes |
|----------|------|------------|-----|-------|
| **Reference** | — | — | **160** | LVGL baseline (no scaling) |
| 7" LCD | 7.0" | 1024×600 | 170 | BTT Pad 7, common tablets |
| 5" LCD | 5.0" | 800×480 | 187 | Popular touch panels |
| AD5M | 4.3" | 720×480 | 201 | FlashForge printer display |

**DPI Calculation:**
```
DPI = sqrt(width² + height²) / diagonal_inches

Example (5" @ 800×480):
DPI = sqrt(800² + 480²) / 5 = 933.06 / 5 = 186.6 ≈ 187
```

### How DPI Affects UI

**At 160 DPI (reference):**
- SMALL screen (≤480px): PAD_DEF=12, PAD_SMALL=8, PAD_TINY=2
- MEDIUM screen (481-800px): PAD_DEF=16, PAD_SMALL=10, PAD_TINY=4
- LARGE screen (>800px): PAD_DEF=20, PAD_SMALL=12, PAD_TINY=6

**At 187 DPI (5" screen):**
- MEDIUM screen PAD_DEF: `(187 * 16 + 80) / 160 = 19` pixels
- ~19% larger than reference to maintain physical size

**At 201 DPI (4.3" screen):**
- MEDIUM screen PAD_DEF: `(201 * 16 + 80) / 160 = 20` pixels
- ~25% larger than reference for smaller, denser display

### Test Suite

Verify DPI scaling behavior:
```bash
make test-responsive-theme  # Includes DPI scaling tests
```

Tests cover:
- Breakpoint classification (SMALL/MEDIUM/LARGE)
- DPI scaling accuracy for all hardware profiles (160/170/187/201)
- Theme toggle preservation

### When to Override DPI

**Keep default (160) when:**
- Developing on desktop/laptop without target hardware
- Creating screenshots for documentation
- Testing responsive layouts independent of density

**Override DPI when:**
- Testing how UI appears on specific target hardware
- Verifying touch target sizes are appropriate
- Validating spacing/padding looks good at actual density
- Generating screenshots that match real device appearance

## spdlog Logging Library

### Overview

HelixScreen uses [spdlog](https://github.com/gabime/spdlog) for all console/file logging. The library is integrated as an **independent git submodule** (not shared with parent guppyscreen project).

**Version:** fmt 11.2.0 branch (updated 2025-11-01)
- Eliminates fmt deprecation warnings
- Clean build output
- Header-only integration

### Submodule Management

**Initialize after clone:**
```bash
git submodule update --init --recursive spdlog
```

**Check submodule status:**
```bash
git submodule status spdlog
# Should show: 14ab43ec0b9df6cf793c75785ec5f70b65f2f965 spdlog (v1.2.1-2508-g14ab43ec)
```

**Update to latest fmt-11.2.0:**
```bash
cd spdlog
git fetch origin
git checkout origin/fmt-11.2.0
cd ..
git add spdlog
git commit -m "update: spdlog to latest fmt-11.2.0"
```

### Usage in Code

**ALWAYS use spdlog** - NEVER printf/cout/cerr/LV_LOG_*:

```cpp
#include <spdlog/spdlog.h>

spdlog::info("Application started");
spdlog::debug("Debug value: {}", variable);
spdlog::warn("Warning: {}", message);
spdlog::error("Error occurred: {}", error_msg);
```

**Log levels:** `trace()`, `debug()`, `info()`, `warn()`, `error()`, `critical()`

**Verbosity flags:** `-v` (info), `-vv` (debug), `-vvv` (trace). Default: warn only.

### Why Independent Submodule?

Previously, spdlog was a symlink to `../spdlog` (shared with parent guppyscreen project). This caused issues:
- Couldn't upgrade HelixScreen's fmt version independently
- Build warnings from old fmt 9.0.1
- Tight coupling between projects

**Solution:** Separate submodule allows independent version control and eliminates deprecation warnings.

## Multi-Display Development (macOS)

Control which display the UI window appears on for dual-monitor workflows:

**How it works:**
- Uses `SDL_GetDisplayBounds()` to query actual display geometry
- Calculates true center position for the specified display
- Supports multi-monitor setups with different resolutions

**Available displays:**
Run without arguments to see auto-detected display information in logs.

## macOS Location Permission (WiFi Development)

**Required for:** Testing real WiFi scanning/connection on macOS (CoreWLAN backend)

macOS 10.15+ requires Location Services permission for WiFi scanning because network SSIDs can reveal physical location. The app will automatically fall back to mock WiFi backend if permission is not granted.

### Why Manual Permission Grant is Needed

macOS's TCC (Transparency, Consent, and Control) system only shows automatic permission dialogs for:
- Signed app bundles (`.app` packages)
- Apps distributed via Mac App Store

Command-line binaries (like our development build) require manual permission grant.

### Grant Location Permission for Development

**Option 1: Grant Permission to Terminal.app (Easiest)**

Command-line apps inherit permissions from their parent process:

1. Open **System Settings** → **Privacy & Security** → **Location Services**
2. Find **Terminal** (or your terminal app) in the list
3. Toggle it **ON**
4. Restart your terminal
5. Run the app - it will now have location access through Terminal

**Option 2: TCC Database Direct Modification (Advanced)**

Add permission directly via Terminal with Full Disk Access:

```bash
# First: Grant Terminal.app "Full Disk Access" in System Settings
# Settings → Privacy & Security → Full Disk Access → Enable Terminal

# Get absolute path to binary
BINARY_PATH="$(cd $(dirname $0) && pwd)/build/bin/helix-ui-proto"

# Add location permission (macOS 15+ format)
sudo sqlite3 ~/Library/Application\ Support/com.apple.TCC/TCC.db \
  "INSERT OR REPLACE INTO access VALUES('kTCCServiceLocation','$BINARY_PATH',0,2,4,1,NULL,NULL,0,'UNUSED',NULL,0,$(date +%s));"

# Kill TCC daemon to reload permissions
sudo killall tccd
```

**Option 3: Create Minimal App Bundle (Most Proper)**

This makes macOS treat it as a real app that can request permissions:

```bash
# Create app bundle structure
mkdir -p HelixUI.app/Contents/MacOS
mkdir -p HelixUI.app/Contents/Resources

# Copy binary
cp build/bin/helix-ui-proto HelixUI.app/Contents/MacOS/

# Create Info.plist
cat > HelixUI.app/Contents/Info.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>helix-ui-proto</string>
    <key>CFBundleIdentifier</key>
    <string>net.356c.helixui</string>
    <key>CFBundleName</key>
    <string>HelixUI</string>
    <key>NSLocationWhenInUseUsageDescription</key>
    <string>HelixScreen needs WiFi access for printer connectivity</string>
</dict>
</plist>
EOF

# Run from bundle - will trigger permission prompt
open HelixUI.app
```

### Verify Permission Status

Run the app with verbose logging to see permission status:

```bash
./build/bin/helix-ui-proto --wizard -vv 2>&1 | grep -i "location\|permission\|wifi"
```

**Expected output with permission granted:**
```
[info] [macOS] Location permission already granted
[info] [WiFiMacOS] CoreWLAN backend initialized successfully
```

**Expected output without permission (falls back to mock):**
```
[warning] [WiFiMacOS] Location permission not determined
[warning] [WifiBackend] CoreWLAN backend failed - falling back to mock
```

### Revoking Permission

To test permission denial or reset state:

```bash
# Remove permission via System Settings
# Settings → Privacy & Security → Location Services → Remove helix-ui-proto

# Or reset via tccutil (requires SIP disabled or Full Disk Access)
tccutil reset Location
```

## Screenshot Workflow

### Interactive Screenshots

```bash
# Run the UI and press 'S' to take screenshot
./build/bin/helix-ui-proto
# Press 'S' key -> saves timestamped PNG to /tmp/
```

### Automated Screenshot Script

```bash
# Basic usage (auto-opens on display 1)
./scripts/screenshot.sh helix-ui-proto output-name [panel] [options]

# Examples
./scripts/screenshot.sh helix-ui-proto home-screen home
./scripts/screenshot.sh helix-ui-proto motion-panel motion -s small
./scripts/screenshot.sh helix-ui-proto controls controls -s large

# Override display for screenshots
HELIX_SCREENSHOT_DISPLAY=0 ./scripts/screenshot.sh helix-ui-proto test home

# Auto-open in Preview after capture
HELIX_SCREENSHOT_OPEN=1 ./scripts/screenshot.sh helix-ui-proto review home
```

**Script features:**
- ✅ **Automatic display positioning** - Opens on display 1 by default
- ✅ **Panel validation** - Catches invalid panel names before running
- ✅ **Dependency checking** - Verifies ImageMagick is installed
- ✅ **Smart cleanup** - Auto-removes BMP, keeps only compressed PNG
- ✅ **Error handling** - Clear error messages with troubleshooting hints

Screenshots saved to `/tmp/ui-screenshot-[name].png`

## Icon & Asset Workflow

### FontAwesome Icon Generation

```bash
python3 scripts/generate-icon-consts.py  # After editing include/ui_fonts.h
```

Updates `ui_xml/globals.xml` with UTF-8 byte sequences for all icons.

### Application Icon Generation

```bash
make icon  # Generates platform-specific icons
```

For complete font/icon generation details, see **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)**.

## IDE & Editor Support

### Generate LSP Support

```bash
make compile_commands  # Requires 'bear' to be installed
```

This creates `compile_commands.json` for IDE language servers (clangd, etc.).

### Recommended Setup

**VS Code:**
- Install C/C++ extension pack
- Install clangd extension
- Ensure `compile_commands.json` is in project root

**Vim/Neovim:**
- Configure clangd LSP client
- Ensure `compile_commands.json` is available

**CLion/other IDEs:**
- Import as Makefile project
- Point to generated `compile_commands.json`

## Daily Development Workflow

### Typical Development Cycle

1. **Edit code** in `src/` or `include/`
2. **Edit XML** in `ui_xml/` (layout/styling changes - no rebuild needed)
3. **Build** with `make -j` (parallel incremental build)
4. **Test** with `./build/bin/helix-ui-proto --test [panel_name]` (test mode)
5. **Screenshot** with `./scripts/screenshot.sh` or press **S** in UI
6. **Commit** with working incremental changes

### Test Mode Development

The test mode system allows development without hardware or network infrastructure:

**Basic Usage:**
```bash
# Full mock mode - no hardware needed
./build/bin/helix-ui-proto --test

# Test with real printer but mock network
./build/bin/helix-ui-proto --test --real-moonraker

# Test with real WiFi but mock printer
./build/bin/helix-ui-proto --test --real-wifi

# Mixed mode - real WiFi and printer, mock Ethernet
./build/bin/helix-ui-proto --test --real-wifi --real-moonraker
```

**Available Flags:**
- `--test` - Enable test mode (required for all mock functionality)
- `--real-wifi` - Use real WiFi hardware instead of mock
- `--real-ethernet` - Use real Ethernet hardware instead of mock
- `--real-moonraker` - Connect to real printer instead of mock
- `--real-files` - Use real files from printer instead of test data

**Test Mode Features:**
- Mock WiFi backend with 10 realistic networks
- Mock Ethernet backend with simulated connection
- Test print files with various sizes and metadata
- Simulated connection delays and failures (5% auth failure rate)
- Visual banner showing what's mocked vs real

**Production Safety:**
- **NEVER** uses mocks without explicit `--test` flag
- Production mode fails gracefully if hardware unavailable
- Clear error messages instead of silent mock fallbacks
- Test mode banner prevents confusion about mock vs real

### XML vs C++ Changes

**XML file changes:**
- Layout, styling, colors, text content
- **No recompilation needed** - changes visible immediately on restart
- Edit `ui_xml/*.xml` files directly

**C++ file changes:**
- Business logic, subject bindings, event handlers
- **Requires rebuild** - run `make -j` after changes
- Edit `src/*.cpp` and `include/*.h` files

### Performance Tips

**Use incremental builds:**
```bash
make -j  # Only rebuilds changed files
```

**Avoid clean rebuilds unless necessary:**
```bash
# Only when troubleshooting build issues
make clean && make -j
```

**Parallel compilation:**
```bash
make -j     # Auto-detects all CPU cores (recommended)
make -j16   # Explicit core count (current system has 16 cores)
```

## Troubleshooting

### Quick Fixes

**SDL2 not found:**
```bash
brew install sdl2                # macOS
sudo apt install libsdl2-dev     # Linux
```

**Compilation errors:**
```bash
make clean && make V=1  # Verbose mode
```

**Missing dependencies:**
```bash
make check-deps     # Check what's missing
make install-deps   # Auto-install
```

**For complete troubleshooting** (patches, performance, runtime issues), see **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)**.

## Cross-Platform Considerations

**This project is developed on both macOS and Linux:**
- **NEVER invoke compilers directly** (`clang++`, `g++`) - always use `make`
- Makefile auto-detects available compiler (clang > gcc priority)
- Platform-specific features handled via Makefile platform detection
- **WiFi Backend:** macOS uses CoreWLAN, Linux uses wpa_supplicant

## Memory & Performance Analysis

See **[docs/MEMORY_ANALYSIS.md](docs/MEMORY_ANALYSIS.md)** for detailed analysis tools and techniques.

## Related Documentation

- **[README.md](README.md)** - Project overview and quick start
- **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)** - Complete build system reference
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System design and patterns
- **[CONTRIBUTING.md](CONTRIBUTING.md)** - Code standards and workflow
- **[LVGL 9 XML Guide](docs/LVGL9_XML_GUIDE.md)** - Complete XML system reference
- **[Quick Reference](docs/QUICK_REFERENCE.md)** - Common patterns and gotchas