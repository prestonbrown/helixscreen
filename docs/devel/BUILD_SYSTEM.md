# Build System Documentation

This document describes the HelixScreen prototype build system, including automatic patch application, multi-display support, and development workflows.

**For common development tasks**, see **[DEVELOPMENT.md](../DEVELOPMENT.md)** - this document covers advanced build system internals.

## Cross-Compilation (Embedded Targets)

HelixScreen supports cross-compilation for embedded ARM targets using Docker-based toolchains. This allows building binaries for Raspberry Pi and other embedded displays directly from macOS or Linux development machines.

### Quick Start

```bash
# Build for Raspberry Pi 64-bit (aarch64/ARM64)
make pi-docker

# Build for Raspberry Pi 32-bit (armhf/armv7l)
make pi32-docker

# Build for Flashforge Adventurer 5M (armv7-a/ARM32)
make ad5m-docker

# Build for Elegoo Centauri Carbon 1 (armv7-a/ARM32)
make cc1-docker

# Build for MIPS targets — K1 and AD5X (MIPS32, static/musl)
make mips-docker          # Or: make k1-docker / make ad5x-docker (aliases)

# Build for Creality K1 series (MIPS32, dynamic/glibc)
make k1-dynamic-docker

# Build for Creality K2 series (ARM, UNTESTED)
make k2-docker

# Verify the binaries
file build/pi/bin/helix-screen     # ELF 64-bit LSB, ARM aarch64
file build/pi32/bin/helix-screen   # ELF 32-bit LSB, ARM, EABI5
file build/ad5m/bin/helix-screen   # ELF 32-bit LSB, ARM, EABI5
file build/cc1/bin/helix-screen    # ELF 32-bit LSB, ARM, EABI5
file build/mips/bin/helix-screen   # ELF 32-bit LSB, MIPS32 (static)
file build/k1-dynamic/bin/helix-screen  # ELF 32-bit LSB, MIPS32 (dynamic)
file build/k2/bin/helix-screen     # ELF 32-bit LSB, ARM, EABI5
```

Docker images are **automatically built** on first use - no manual setup required!

### Supported Targets

| Target | Command | Architecture | Display | Output Directory |
|--------|---------|--------------|---------|------------------|
| **Raspberry Pi (64-bit)** | `make pi-docker` | aarch64 (ARM64) | DRM/fbdev | `build/pi/` |
| **Raspberry Pi (32-bit)** | `make pi32-docker` | armv7-a (armhf) | DRM/fbdev | `build/pi32/` |
| **Adventurer 5M** | `make ad5m-docker` | armv7-a (hard-float) | fbdev | `build/ad5m/` |
| **Centauri Carbon 1** | `make cc1-docker` | armv7-a (hard-float) | fbdev | `build/cc1/` |
| **MIPS (K1/AD5X)** | `make mips-docker` | MIPS32r2 (musl) | fbdev | `build/mips/` |
| **Creality K1 (dynamic)** | `make k1-dynamic-docker` | MIPS32r2 (glibc) | fbdev | `build/k1-dynamic/` |
| **Creality K2** | `make k2-docker` | armv7-a (musl) | fbdev | `build/k2/` |
| **Native (SDL)** | `make` | Host architecture | SDL2 | `build/` |

### How It Works

1. **Docker Toolchains**: Each target has a Dockerfile (`docker/Dockerfile.pi`, `docker/Dockerfile.pi32`, `docker/Dockerfile.ad5m`, etc.) that contains the cross-compiler, sysroot libraries, and build tools.

2. **Auto-Build Images**: When you run `make pi-docker`, `make pi32-docker`, or `make ad5m-docker`, the build system automatically:
   - Checks if the Docker image exists
   - Builds the image if missing (takes 2-5 minutes first time)
   - Runs the cross-compilation inside the container

3. **Volume Mounting**: Your source code is mounted into the container, so compiled binaries appear directly in your `build/` directory.

4. **Display Backend Selection**: Cross-compilation automatically selects the appropriate display backend:
   - **Pi / Pi32**: DRM (preferred) with fbdev fallback
   - **AD5M / CC1**: fbdev (framebuffer)

### Build Targets

```bash
# Docker-based builds (recommended - no toolchain installation needed)
make pi-docker           # Raspberry Pi 64-bit via Docker (DRM only)
make pi-all-docker       # Raspberry Pi 64-bit — both DRM + fbdev (single pass)
make pi32-docker         # Raspberry Pi 32-bit via Docker (DRM only)
make pi32-all-docker     # Raspberry Pi 32-bit — both DRM + fbdev (single pass)
make ad5m-docker         # Adventurer 5M via Docker
make cc1-docker          # Centauri Carbon 1 via Docker
make k1-docker           # Creality K1 series via Docker (static/musl)
make k1-dynamic-docker   # Creality K1 series via Docker (dynamic/glibc)
make k2-docker           # Creality K2 series via Docker (UNTESTED)
make docker-toolchains   # Pre-build all Docker images

# Direct cross-compilation (requires toolchain installed on host)
make pi                  # Raspberry Pi 64-bit (needs aarch64-linux-gnu-gcc)
make pi32                # Raspberry Pi 32-bit (needs arm-linux-gnueabihf-gcc)
make ad5m                # Adventurer 5M (needs arm-linux-gnueabihf-gcc)
make cc1                 # Centauri Carbon 1 (needs arm-linux-gnueabihf-gcc)
make k1                  # Creality K1 static (needs Bootlin mips32el-musl toolchain)
make k1-dynamic          # Creality K1 dynamic (needs custom NaN2008 GCC 7.5 toolchain)
make k2                  # Creality K2 (needs Bootlin armv7-eabihf-musl toolchain)

# Information
make cross-info          # Show cross-compilation help
```

### Target Specifications

#### Raspberry Pi 64-bit (Mainsail OS)
- **CPU**: Cortex-A72/A76 (64-bit ARM)
- **Toolchain**: `aarch64-linux-gnu-gcc` (GCC 10+)
- **Display**: DRM preferred, fbdev fallback
- **Input**: libinput for touch
- **Docker Image**: `helixscreen/toolchain-pi` (Debian Bullseye)

#### Raspberry Pi 32-bit (Mainsail OS)
- **CPU**: Cortex-A7/A53/A72 in 32-bit mode (armv7-a, hard-float + NEON)
- **Toolchain**: `arm-linux-gnueabihf-gcc` (GCC 10+)
- **Display**: DRM preferred, fbdev fallback
- **Input**: libinput for touch
- **Docker Image**: `helixscreen/toolchain-pi32` (Debian Bullseye)
- **Coverage**: Pi 2, 3, 4, 5 running 32-bit Raspberry Pi OS / MainsailOS

#### Flashforge Adventurer 5M
- **CPU**: Cortex-A7 (32-bit ARM, hard-float)
- **Toolchain**: `arm-linux-gnueabihf-gcc` (GCC 8.3)
- **Display**: 800×480 framebuffer (`/dev/fb0`)
- **Input**: evdev for touch (`/dev/input/event4`)
- **C Library**: glibc 2.25 (requires older toolchain for compatibility)
- **RAM**: 110MB total (~36MB available with Klipper running)
- **Docker Image**: `helixscreen/toolchain-ad5m` (Debian Buster)

#### Elegoo Centauri Carbon 1
- **SoC**: Allwinner R528 / sun8iw20 (Cortex-A7 dual-core, armv7-a hard-float)
- **Toolchain**: ARM GCC 10.3 (`arm-none-linux-gnueabihf-gcc`)
- **Display**: 480×272 framebuffer (`/dev/fb0`), 32bpp ARGB8888
- **Input**: evdev for touch (Goodix gt9xxnew_ts on `/dev/input/event1`)
- **C Library**: glibc 2.23 (static linking avoids version conflicts)
- **RAM**: 112MB total (~34MB available with Klipper running)
- **Docker Image**: `helixscreen/toolchain-cc1` (Debian Bookworm)

#### Creality K1 Series — Static (K1C, K1 Max)
- **CPU**: Ingenic X2000E (MIPS32r2 dual-core @ 1.2 GHz)
- **Toolchain**: Bootlin `mips32el-musl` (GCC 12, musl libc)
- **Display**: 480×400 framebuffer
- **Input**: evdev for touch
- **C Library**: musl (fully static binary — no system library dependencies)
- **RAM**: 256MB
- **Docker Image**: `helixscreen/toolchain-k1` (Debian Bookworm)

#### Creality K1 Series — Dynamic (K1C, K1 Max)
- **CPU**: Ingenic X2000E (MIPS32r2 dual-core @ 1.2 GHz)
- **Toolchain**: Custom `mipsel-k1-linux-gnu-` (GCC 7.5 built via crosstool-NG, NaN2008+FP64 ABI)
- **Display**: 480×400 framebuffer
- **Input**: evdev for touch
- **C Library**: glibc 2.29 (links dynamically against K1's native system libraries)
- **Linking**: Mixed — project libraries (libhv, libnl, wpa) static; system libraries (libc, libstdc++, libm, libpthread) dynamic
- **RAM**: 256MB
- **Docker Image**: `helixscreen/toolchain-k1-dynamic` (custom, builds toolchain from source)
- **GCC 7.5 constraints**: See [GCC 7.5 Compatibility](#gcc-75-compatibility-k1-dynamic-target) section above
- **Why two K1 targets?** Static/musl is simpler and more portable. Dynamic/glibc produces smaller binaries (shared system libs) and avoids musl edge cases, but requires the custom NaN2008 toolchain.

#### Creality K2 Series (K2, K2 Pro, K2 Plus) — UNTESTED
- **CPU**: Allwinner A133/T800 (ARM Cortex-A53, quad-core)
- **Toolchain**: Bootlin `armv7-eabihf-musl` (GCC 12, musl libc)
- **Display**: 480×800 framebuffer (portrait panel, may be rotated to 800×480 by driver)
- **Input**: evdev for touch (Goodix GT9xx or TLSC6x)
- **C Library**: musl (static linking — system libc unconfirmed, static avoids the question)
- **RAM**: ~512MB (unconfirmed)
- **Moonraker**: Stock on port 4408 (no community firmware needed)
- **Docker Image**: `helixscreen/toolchain-k2` (Debian Bookworm)
- **Known unknowns**: ARM variant (armv7 vs aarch64 userland), framebuffer orientation, exact install paths. See `docs/printer-research/CREALITY_K2_PLUS_RESEARCH.md` Section 13 for diagnostic commands.

### Dockerfile Architecture

```
docker/
├── Dockerfile.pi          # Pi 64-bit toolchain (Debian Bullseye, GCC 10)
├── Dockerfile.pi32        # Pi 32-bit toolchain (Debian Bullseye, GCC 10)
├── Dockerfile.ad5m        # AD5M toolchain (Debian Buster, GCC 8)
├── Dockerfile.cc1         # CC1 toolchain (Debian Bookworm, ARM GCC 10.3)
├── Dockerfile.k1          # K1 static toolchain (Bootlin mips32el-musl, GCC 12)
├── Dockerfile.k1-dynamic  # K1 dynamic toolchain (crosstool-NG, GCC 7.5, glibc 2.29)
└── Dockerfile.k2          # K2 toolchain (Bootlin armv7-eabihf-musl, GCC 12)
```

The Dockerfiles handle:
- Cross-compiler installation (`crossbuild-essential-*`)
- Target architecture libraries (`:arm64` / `:armhf` packages)
- SSL/crypto libraries for Moonraker WebSocket
- Environment variables for cross-compilation

### Build System Integration

Cross-compilation is handled by `mk/cross.mk`, which defines:

```makefile
# Set target platform (native, pi, pi32, ad5m, cc1, k1, k1-dynamic, k2)
PLATFORM_TARGET ?= native

# Cross-compiler configuration
CROSS_COMPILE := arm-linux-gnueabihf-  # For AD5M
CC := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++

# Target-specific flags
TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
TARGET_LDFLAGS := -lstdc++fs  # GCC 8 requires this for std::filesystem

# Display backend selection
DISPLAY_BACKEND := fbdev  # or drm, sdl
```

### GCC 7.5 Compatibility (K1 Dynamic Target)

The K1 dynamic build uses a custom GCC 7.5 toolchain targeting the K1's native glibc 2.29. GCC 7.5 only supports C++17 partially, so code must avoid certain features. This applies to **all code in the codebase** — even native builds should stay compatible.

**What works:**
- Most of C++17 (`std::optional`, `std::string_view`, structured bindings, `if constexpr`, etc.)
- `<filesystem>` via the compat shim at `include/compat/filesystem` (aliases `std::experimental::filesystem` → `std::filesystem`)

**Gotchas to avoid:**

| Feature | GCC 7 Status | Workaround | Example |
|---------|-------------|------------|---------|
| **`std::from_chars` (integers)** | Not available | Use `std::strtol` / `std::strtod` | `src/util/version.cpp` |
| **`std::atomic<time_point>`** | Doesn't compile | Store as `std::atomic<int64_t>` (nanoseconds) | `include/gcode_streaming_controller.h` |
| **C++20 designated initializers** | Not supported (`{.foo = 1}`) | Initialize struct explicitly, then assign fields | `src/ui/ui_fan_control_overlay.cpp` |
| **`directory_entry` member functions** | `.is_regular_file()`, `.file_size()`, `.last_write_time()` missing | Use free functions: `std::filesystem::is_regular_file(entry.path())` | `src/print/thumbnail_cache.cpp`, `src/plugin/plugin_manager.cpp` |
| **`-lstdc++fs`** | Required for `<experimental/filesystem>` | Added automatically for `k1-dynamic` in `mk/cross.mk` and `mk/watchdog.mk` | — |
| **LTO (`-flto`)** | GCC 7.5 static toolchain lacks `liblto_plugin.so` | Disabled for `k1-dynamic`; uses plain `ar`/`ranlib` instead of `gcc-ar`/`gcc-ranlib` | `mk/cross.mk` |

**Filesystem compat shim** (`include/compat/filesystem`):
- For GCC < 8: includes `<experimental/filesystem>` and aliases it into `std::filesystem`
- For GCC 8+/Clang/MSVC: passes through to the real `<filesystem>` via `#include_next`
- Activated by `-isystem include/compat` in the K1 dynamic target flags

**When adding new code:** Always use `std::filesystem::is_regular_file(path)` (free function) rather than `entry.is_regular_file()` (member function). The free-function forms work on both GCC 7 and modern compilers.

### Troubleshooting

**Docker not installed:**
```bash
# macOS - Option 1: Docker Desktop (GUI)
brew install --cask docker

# macOS - Option 2: Colima (lightweight, CLI-only, recommended)
brew install colima docker
colima start --cpu 4 --memory 8  # Start VM with 4 cores, 8GB RAM

# Linux
sudo apt install docker.io
sudo usermod -aG docker $USER  # Logout/login after this
```

**Colima tips (macOS):**
```bash
colima start                    # Start with defaults
colima start --cpu 4 --memory 8 # Custom resources (faster builds)
colima stop                     # Stop VM when not needed
colima status                   # Check if running
```

**Docker image build fails:**
```bash
# Rebuild with no cache
docker build --no-cache -t helixscreen/toolchain-ad5m -f docker/Dockerfile.ad5m docker/
```

**"file format not recognized" linker error:**
This means a library was built for the wrong architecture. Clean and rebuild:
```bash
rm -rf build/ad5m lib/wpa_supplicant/wpa_supplicant/*.a
make ad5m-docker
```

**std::filesystem undefined references (AD5M only):**
GCC 8 requires `-lstdc++fs` for std::filesystem. This is already configured in `mk/cross.mk` for AD5M target.

### Deploying to Target

**Using make targets (recommended for Pi):**
```bash
# Full cycle: build + deploy + run on Pi
make pi-test

# Deploy only (after building)
make deploy-pi                    # Deploy binaries + assets
make deploy-pi-run                # Deploy and run

# Customize target
make deploy-pi PI_HOST=192.168.1.50 PI_USER=pi
```

**Using make targets for AD5M:**
```bash
# Full cycle: remote build on thelio + deploy + run
make ad5m-test

# Remote build only (builds on thelio.local, fetches binaries)
make remote-ad5m

# Deploy only (after building)
make deploy-ad5m                  # Deploy binaries + assets, restart in background
make deploy-ad5m-fg               # Deploy and run in foreground (debug)
make deploy-ad5m-bin              # Deploy binaries only (fast iteration)

# Customize target (mDNS may not resolve - use IP instead)
make deploy-ad5m AD5M_HOST=192.168.1.67
```

**Note:** The AD5M's mDNS (`ad5m.local`) may not resolve reliably. Use the IP address directly:
```bash
# Find your AD5M's IP from your router or the printer's network settings
AD5M_HOST=192.168.1.67 make deploy-ad5m
```

**Manual deployment:**
```bash
# Raspberry Pi
scp build/pi/bin/helix-screen pi@mainsailos.local:~/

# Adventurer 5M (via SSH or SD card)
scp build/ad5m/bin/helix-screen root@192.168.1.x:/usr/data/
```

### Logging on Target

HelixScreen automatically detects the best logging backend:

| Platform | Default Backend | View Logs |
|----------|----------------|-----------|
| Linux + systemd | journal | `journalctl -t helix -f` |
| Linux (no systemd) | syslog | `tail -f /var/log/syslog \| grep helix` |
| File fallback | rotating file | `tail -f /var/log/helix-screen.log` |

**Override via CLI:**
```bash
./helix-screen --log-dest=journal   # Force systemd journal
./helix-screen --log-dest=file --log-file=/tmp/debug.log
```

**systemd service:** The included `config/helixscreen.service` automatically logs to journal. View with:
```bash
sudo journalctl -u helixscreen -f
```

### Display Backend Selection

The build system automatically selects display backends:

| Backend | Define | Libraries | Use Case |
|---------|--------|-----------|----------|
| **SDL** | `HELIX_DISPLAY_SDL` | SDL2 | Desktop development |
| **DRM** | `HELIX_DISPLAY_DRM` | libdrm, libinput | Pi with KMS |
| **fbdev** | `HELIX_DISPLAY_FBDEV` | (none) | Embedded framebuffer |

Display backend is selected via `DISPLAY_BACKEND` in `mk/cross.mk` and controls:
- LVGL driver compilation (`lv_conf.h` conditionals)
- Display initialization in `display_backend.cpp`
- Input driver selection (SDL mouse, evdev touch, libinput)

### Pi Dual-Link Build (Compile Once, Link Twice)

Pi release builds produce two binaries: DRM (GPU-accelerated) and fbdev (framebuffer fallback). Instead of compiling all ~900 source files twice, the **dual-link build** compiles everything once with DRM superset defines, then links two binaries with different display libraries and link flags.

**This cuts Pi CI build time roughly in half (~40 min instead of 80+).**

```bash
# Dual-link build (produces both DRM + fbdev in one pass)
make PLATFORM_TARGET=pi-both -j       # Direct (requires toolchain)
make pi-all-docker                     # Docker (recommended)

# Individual builds still work (for development/debugging)
make PLATFORM_TARGET=pi -j            # DRM only
make PLATFORM_TARGET=pi-fbdev -j      # fbdev only
```

#### How It Works

1. **Compile phase**: All source files compile once using DRM superset defines (`-DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV -DHELIX_ENABLE_OPENGLES`). Objects go to `build/pi/obj/`.

2. **Variant-specific compilation** (only 4 files):
   - `display_backend.cpp`, `display_backend_fbdev.cpp`, `touch_calibration.cpp` → compiled into `build/pi/display-fbdev/` without DRM defines, archived as `libhelix-display-fbdev.a`
   - `crash_reporter.cpp` → compiled into `build/pi/fbdev-variant/` with `-DHELIX_BINARY_VARIANT="fbdev"`

3. **DRM link**: All objects + LVGL DRM drivers + OpenGLES objects + `-ldrm -linput -lEGL -lGLESv2 -lgbm` → `build/pi/bin/helix-screen`

4. **fbdev link**: Common objects (minus DRM drivers, OpenGLES objects, DRM display backend) + `libhelix-display-fbdev.a` + fbdev crash_reporter.o → `build/pi-fbdev/bin/helix-screen`

5. **Verification**: `verify-fbdev` automatically checks the fbdev binary has no DRM/GLES undefined symbols.

#### Build Output Layout

```
build/
  pi/
    obj/                        # ALL objects (shared between both links)
    lib/
      libhelix-display.a        # DRM display library (used by splash/watchdog too)
      libhelix-display-fbdev.a  # fbdev display library
    display-fbdev/              # fbdev display backend objects
    fbdev-variant/              # fbdev crash_reporter.o
    bin/
      helix-screen              # DRM binary
      helix-splash              # Splash (DRM only)
      helix-watchdog            # Watchdog (DRM only)
  pi-fbdev/
    bin/
      helix-screen              # fbdev binary (linked from pi/ objects)
```

#### Important Caveats for Developers

- **`#ifdef HELIX_DISPLAY_DRM` in non-display files**: The shared objects are compiled with DRM defines enabled. If you add `#ifdef HELIX_DISPLAY_DRM` to a non-display source file, that code path will execute in the fbdev binary too. Only `display_backend*.cpp` is recompiled for fbdev. Use runtime detection (`DisplayBackend::get_type()`) instead of compile-time guards for behavior that should differ between DRM and fbdev.
- **Adding new DRM-only sources**: If you add a new source file that references DRM/GLES symbols (e.g., `drmModeGetResources`), you must add its object to `LVGL_DRM_DRIVER_OBJS` in `mk/pi-dual-link.mk` to exclude it from the fbdev link. The `verify-fbdev` target will catch this if you forget.
- **The `pi` and `pi-both` targets share `build/pi/`**: Switching between them doesn't trigger a clean rebuild — the arch-change detection maps both to the same build directory.

#### Files

| File | Purpose |
|------|---------|
| `mk/pi-dual-link.mk` | Fbdev display lib, crash reporter variant, fbdev link rule, verify/strip targets |
| `mk/cross.mk` | `pi-both` / `pi32-both` platform target definitions |
| `mk/rules.mk` | Conditional `all` target (uses `strip-both` in dual-link mode) |

---

## Git Worktrees

Git worktrees allow parallel development on multiple branches without switching contexts. HelixScreen uses worktrees for feature development.

### Creating a Worktree

Use `setup-worktree.sh` to create and configure worktrees with fast builds:

```bash
# Create worktree with new branch (one command does everything)
./scripts/setup-worktree.sh feature/my-feature

# Creates at .worktrees/my-feature, builds automatically
cd .worktrees/my-feature
./build/bin/helix-screen --test -vv
```

### Script Options

```bash
# Create at custom path
./scripts/setup-worktree.sh feature/foo /tmp/helixscreen-foo

# Set up existing worktree without creating
./scripts/setup-worktree.sh --setup-only feature/i18n

# Skip the initial build
./scripts/setup-worktree.sh --no-build feature/quick-test
```

### What setup-worktree.sh Does

The script optimizes for **fast builds** by sharing artifacts from the main tree:

1. **Symlinks lib/** - All submodules symlinked (no clone/configure time)
2. **Symlinks compiled libraries** - `libhv.a`, `libTinyGL.a` from main tree
3. **Symlinks precompiled header** - `lvgl_pch.h.gch` (22MB saved)
4. **Symlinks tools** - `node_modules/`, `.venv/`
5. **Configures git** - Uses `--skip-worktree` for clean `git status`

**Trade-off**: If you need to modify library code (`lib/`), un-symlink that specific directory first.

### Managing Worktrees

```bash
# List existing worktrees
git worktree list

# Example output:
# /Users/you/code/helixscreen                    abc1234 [main]
# /Users/you/code/helixscreen/.worktrees/i18n    def5678 [feature/i18n]
```

### Cleanup

```bash
# Remove a worktree
git worktree remove .worktrees/my-feature

# Or force remove if dirty
git worktree remove --force .worktrees/my-feature
```

---

## Build System Overview

The project uses **GNU Make** with a modular architecture:
- **Modular design**: ~4,300 lines split across 14 files for maintainability
- **Color-coded output** for easy visual parsing
- **Verbosity control** to show/hide full compiler commands
- **Automatic dependency checking** before builds with smart canvas detection
- **Interactive installation** of missing dependencies (`make install-deps`)
- **Automatic code formatting** for C/C++ and XML files
- **Fail-fast error handling** with clear diagnostics
- **Parallel build support** with output synchronization
- **Build timing** for performance tracking

### Modular Makefile Structure

The build system is organized into focused modules:

| File | Lines | Purpose |
|------|-------|---------|
| `Makefile` | ~630 | Configuration, variables, platform detection, module includes |
| `mk/tests.mk` | ~880 | All test targets (unit, integration, by-feature) |
| `mk/cross.mk` | ~750 | Cross-compilation, toolchain setup, display backends |
| `mk/deps.mk` | ~500 | Dependency checking, installation, libhv/wpa_supplicant |
| `mk/rules.mk` | ~340 | Compilation rules, linking, main build targets |
| `mk/remote.mk` | ~280 | Remote deployment (Pi, AD5M) |
| `mk/images.mk` | ~200 | Image conversion (PNG, SVG) |
| `mk/patches.mk` | ~130 | LVGL patch application |
| `mk/fonts.mk` | ~120 | Font/icon generation, Material icons |
| `mk/watchdog.mk` | ~120 | Hardware watchdog support |
| `mk/format.mk` | ~110 | Code and XML formatting |
| `mk/splash.mk` | ~110 | Splash screen generation |
| `mk/tools.mk` | ~110 | Development tool targets |
| `mk/display-lib.mk` | ~60 | Display library configuration |
| `mk/pi-dual-link.mk` | ~200 | Pi dual-link build (compile once, link DRM + fbdev) |

Each module is self-contained with GPL-3 copyright headers and clear separation of concerns.

### Quick Start

```bash
# Parallel build (auto-detects CPU cores)
make -j

# Clean parallel build with progress/timing
make build

# Verbose mode (shows full commands)
make V=1

# Code formatting (clang-format for C/C++, xmllint for XML)
make format              # Format all files
make format-staged       # Format only staged files

# Dependency checking (comprehensive)
make check-deps

# Auto-install missing dependencies (interactive)
make install-deps

# Help (shows all targets and options)
make help

# Apply patches manually (usually automatic)
make apply-patches

# IDE/LSP support (auto-generated after builds, or manually)
make compile_commands    # Merge existing fragments (~1-2s)
```

### Build Configuration Options

The build system supports several configuration flags to customize the build:

**TinyGL 3D Rendering** (default: enabled)
```bash
# Build with TinyGL 3D rendering support (default)
make -j

# Build without TinyGL 3D rendering
make -j ENABLE_TINYGL_3D=no
```

When `ENABLE_TINYGL_3D=yes` (default):
- TinyGL library is built from integrated source (`tinygl/`)
- 3D rendering code is compiled in
- `ENABLE_TINYGL_3D` preprocessor define is set

When `ENABLE_TINYGL_3D=no`:
- TinyGL library is not built
- 3D rendering code is excluded via `#ifdef ENABLE_TINYGL_3D` guards
- Smaller binary size, faster builds

**Verbosity Control** (default: quiet)
```bash
# Quiet mode (default) - shows progress
make -j

# Verbose mode - shows full compiler commands
make -j V=1
```

## Dependency Management

The build system includes comprehensive dependency checking and automatic installation.

### Checking Dependencies

```bash
make check-deps
```

This checks for:
- **System tools**: C/C++ compiler, cmake, make, python3, npm
- **Code formatters**: clang-format (C/C++), xmllint (XML validation/formatting)
- **Libraries**: pkg-config
- **Canvas dependencies**: cairo, pango, libpng, libjpeg, librsvg (for lv_img_conv)
- **npm packages**: lv_font_conv, lv_img_conv
- **Optional libraries**: SDL2, spdlog, libhv (uses system if available, otherwise builds from submodules)
- **Git submodules**: LVGL (always built from submodule)

The checker is **platform-aware** and shows the correct install commands for:
- **macOS** (Homebrew)
- **Debian/Ubuntu** (apt)
- **Fedora/RHEL** (dnf)

### Installing Dependencies

```bash
make install-deps
```

This **interactively installs** missing dependencies:
1. Detects your platform
2. Lists packages to be installed
3. Shows the command it will run
4. Asks for confirmation before proceeding
5. Installs system packages via brew/apt/dnf
6. Runs `npm install` for lv_font_conv/lv_img_conv
7. Initializes git submodules if needed

**Smart Canvas Detection**: Uses `pkg-config` to detect exactly which canvas libraries are missing and only installs what's needed.

**Automatic Builds**: Git submodules (libhv, wpa_supplicant, spdlog) are built automatically by the main build system when missing - no manual intervention needed.

### Library Clean Targets

Individual clean targets are available for forcing rebuilds of specific libraries without a full `make clean`. This is useful when:
- Switching between native and cross-compilation
- Build flags have changed
- Debugging library-specific issues

```bash
make libhv-clean    # Clean libhv WebSocket library artifacts
make sdl2-clean     # Clean SDL2 CMake build directory
make lvgl-clean     # Clean LVGL compiled objects
make libs-clean     # Clean all library artifacts at once
```

**Cross-Compilation Note**: When cross-compiling (e.g., `make ad5m-docker`), libhv is **automatically cleaned** before each build to prevent architecture mixing. This adds ~5 seconds but ensures correct builds.

### Test Harness

The dependency system includes a comprehensive test suite:

```bash
./tests/test_deps.sh
```

Tests 9 scenarios with 22 assertions covering dependency detection, platform-specific commands, and auto-installation workflow.

### Build Options

- **`V=1`** - Verbose mode: shows full compiler commands instead of short `[CC]`/`[CXX]` tags
- **`JOBS=N`** - Set parallel job count (default: auto-detects CPU cores)
- **`NO_COLOR=1`** - Disable colored output (useful for CI/CD)
- **`-j<N>`** - Enable parallel builds with N jobs (NOT auto-enabled by default)

### Build Output

The build system uses color-coded tags:

- **`[CC]`** (cyan) - Compiling C sources (LVGL)
- **`[CXX]`** (blue) - Compiling C++ sources (app code)
- **`[FONT]`** (green) - Compiling font assets
- **`[ICON]`** (green) - Compiling icon assets
- **`[LD]`** (magenta) - Linking binary
- **`✓`** (green) - Success messages
- **`✗`** (red) - Error messages
- **`⚠`** (yellow) - Warning messages

### Error Handling

When compilation fails, the build system:
1. Shows the failed file with a red `✗` marker
2. Displays the full compiler command for debugging
3. Exits immediately (fail-fast behavior)

Example:
```
[CXX] src/ui_panel_home.cpp
✗ Compilation failed: src/ui_panel_home.cpp
Command: clang++ -std=c++17 -Wall -Wextra -O2 -g -I. -Iinclude ...
```

## Code Formatting

The build system includes automatic code formatting for C/C++ and XML files, integrated with pre-commit hooks.

### Formatters

- **clang-format** - Formats C, C++, and Objective-C files according to `.clang-format` config
- **xmllint** - Formats and validates XML layout files with consistent indentation

### Configuration

**`.clang-format`** (LLVM-based with project customizations):
- **Indentation**: 4 spaces, no tabs
- **Line length**: 100 characters
- **Braces**: K&R style (same line)
- **Pointers**: Left-aligned (`int* ptr`)
- **Includes**: Auto-sorted with grouping (project → external → system)

### Formatting Commands

```bash
# Format all C/C++ and XML files
make format

# Format only staged files (useful before commit)
make format-staged

# Check formatting without modifying files
./scripts/quality-checks.sh
```

### Pre-commit Integration

Formatting is automatically checked by the pre-commit hook (`.git/hooks/pre-commit`), which calls `scripts/quality-checks.sh --staged-only`:

1. **Checks staged files** for formatting issues
2. **Reports files** that need formatting
3. **Prevents commit** if formatting issues are found
4. **Suggests fix**: Run `make format-staged` or `clang-format -i <file>`

To bypass (not recommended):
```bash
git commit --no-verify
```

### Quality Checks

The `scripts/quality-checks.sh` script runs multiple checks:
- **Code formatting** (clang-format)
- **XML formatting** (xmllint)
- **XML validation** (xmllint --noout)
- **Copyright headers** (GPL v3 SPDX identifiers)
- **Merge conflict markers**
- **Trailing whitespace**
- **Build verification** (pre-commit only)

Used by both:
- **Pre-commit hook** (staged files only)
- **CI/CD** (all files)

## Automatic Patch Application

The build system automatically applies patches to git submodules before compilation.

### How It Works

1. **Patch Storage**: All submodule patches are stored in `patches/` (in the repository root)
2. **Auto-Detection**: Makefile checks if patches are already applied before each build
3. **Idempotent**: Safe to run multiple times - patches are only applied once
4. **Transparent**: No manual intervention needed for normal development

### Patch: LVGL SDL Window Position

**File**: `patches/lvgl_sdl_window_position.patch`

**Purpose**: Adds multi-display support to LVGL 9's SDL driver by reading environment variables.

**Environment Variables**:
- `HELIX_SDL_DISPLAY` - Display number (0, 1, 2...) to center window on
- `HELIX_SDL_XPOS` - X coordinate for exact window position
- `HELIX_SDL_YPOS` - Y coordinate for exact window position

**Application Logic** (in `Makefile`):
```makefile
apply-patches:
	@echo "Checking LVGL patches..."
	@if git -C $(LVGL_DIR) diff --quiet src/drivers/sdl/lv_sdl_window.c; then \
		# File is clean, apply patch
		git -C $(LVGL_DIR) apply ../patches/lvgl_sdl_window_position.patch
	else \
		# File already modified (patch applied)
		echo "✓ LVGL SDL window position patch already applied"
	fi
```

**Status Messages**:
- `✓ Patch applied successfully` - Patch was applied during this build
- `✓ LVGL SDL window position patch already applied` - Patch was already present
- `⚠ Cannot apply patch (already applied or conflicts)` - Manual intervention needed

### Adding New Patches

To add a new submodule patch:

1. **Make changes** in the submodule directory
2. **Generate patch**:
   ```bash
   cd lib/lvgl
   git diff > ../../patches/my-new-patch.patch
   ```
3. **Update Makefile** to apply the patch in the `apply-patches` target
4. **Document** in `patches/README.md`

## Multi-Display Support (macOS)

The prototype supports multi-monitor development workflows with automatic window positioning.

### Command Line Arguments

```bash
# Display-based positioning (centered)
./build/bin/helix-screen --display 0    # Main display
./build/bin/helix-screen --display 1    # Secondary display
./build/bin/helix-screen -d 2           # Third display (short form)

# Exact pixel coordinates
./build/bin/helix-screen --x-pos 100 --y-pos 200
./build/bin/helix-screen -x 1500 -y -500  # Works with negative Y (display above)

# Combined with other options
./build/bin/helix-screen -d 1 -s small --panel home
```

### Implementation Details

**Flow**:
1. `main.cpp` parses command line arguments
2. Sets environment variables before LVGL initialization:
   ```cpp
   setenv("HELIX_SDL_DISPLAY", "1", 1);  // For --display 1
   // or
   setenv("HELIX_SDL_XPOS", "100", 1);   // For --x-pos 100
   setenv("HELIX_SDL_YPOS", "200", 1);   // For --y-pos 200
   ```
3. LVGL SDL driver reads environment variables during window creation
4. Uses `SDL_GetDisplayBounds()` to query display geometry
5. Calculates center position: `display_x + (display_w - window_w) / 2`
6. Calls `SDL_SetWindowPosition()` after window creation (fixes macOS quirks)

**Source Files**:
- `src/main.cpp` - Argument parsing and environment setup (lines 218-220, 385-401)
- `lvgl/src/drivers/sdl/lv_sdl_window.c` - Window positioning logic (patch)

### Screenshot Script Integration

The `scripts/screenshot.sh` script automatically uses display positioning:

```bash
# Default: opens on display 1 (keeps terminal visible on display 0)
./scripts/screenshot.sh helix-screen output-name panel

# Override display
HELIX_SCREENSHOT_DISPLAY=0 ./scripts/screenshot.sh helix-screen output panel
```

**How it works**:
```bash
# In screenshot.sh
HELIX_SCREENSHOT_DISPLAY=${HELIX_SCREENSHOT_DISPLAY:-1}  # Default to display 1
EXTRA_ARGS="--display $HELIX_SCREENSHOT_DISPLAY $EXTRA_ARGS"
```

This ensures the UI window appears on a different display from the terminal, making it easier to monitor build output and screenshots simultaneously.

## Parallel Compilation

**Important**: Parallel builds are **NOT** enabled by default. Use `-j` flag explicitly.

### Platform Detection

```makefile
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS
    NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
    PLATFORM := macOS
else
    # Linux
    NPROC := $(shell nproc 2>/dev/null || echo 4)
    PLATFORM := Linux
endif
```

### Usage

```bash
make -j          # Auto-detect CPU cores and parallelize (recommended)
make -j16        # Explicit job count (current system has 16 cores)
make JOBS=16     # Set job count via variable
make build       # Clean parallel build (auto-detects cores)
```

The build system uses `--output-sync=target` to prevent interleaved output during parallel builds.

## Font Generation

The build system uses `lv_font_conv` to convert TrueType fonts into LVGL-compatible C arrays.

### Font Types

**Material Design Icons (MDI):**
- Source: `scripts/regen_mdi_fonts.sh` (single source of truth)
- Font: `assets/fonts/materialdesignicons-webfont.ttf`
- Output: `mdi_icons_16.c`, `mdi_icons_24.c`, `mdi_icons_32.c`, `mdi_icons_48.c`, `mdi_icons_64.c`
- Codepoint mapping: `include/ui_icon_codepoints.h`

**Noto Sans Text Fonts:**
- Source: `package.json` npm scripts
- Font: `assets/fonts/NotoSans-Regular.ttf`, `NotoSans-Bold.ttf`
- Output: `noto_sans_*.c`, `noto_sans_bold_*.c`

### How It Works

MDI icon fonts are regenerated when `scripts/regen_mdi_fonts.sh` changes. The build system uses Make's dependency tracking to only regenerate when needed.

**Automatic regeneration:**
```bash
make          # Checks fonts and regenerates if regen script is newer
```

**Manual regeneration:**
```bash
make regen-fonts       # Regenerate MDI icon fonts from regen script
make generate-fonts    # Explicit font regeneration
```

### Adding New Icon Glyphs

To add new Material Design Icons:

1. **Find the icon** at https://pictogrammers.com/library/mdi/
2. **Get the codepoint** (e.g., `wifi-strength-4` = `0xF0928`)
3. **Edit `scripts/regen_mdi_fonts.sh`** and add the codepoint:
   ```bash
   MDI_ICONS+=",0xF0928"    # wifi-strength-4
   ```
4. **Add to codepoints header** (`include/ui_icon_codepoints.h`):
   ```cpp
   {"wifi_strength_4",    "\xF3\xB0\xA4\xA8"},  // F0928 wifi-strength-4
   ```
5. **Regenerate fonts:**
   ```bash
   make regen-fonts
   make -j
   ```

### Requirements

- **Node.js and npm** - Required for font generation
  - macOS: `brew install node`
  - Ubuntu/Debian: `sudo apt install npm`
  - Fedora/RHEL: `sudo dnf install npm`
- **lv_font_conv** - Installed automatically via `npm install` (see `package.json` devDependencies)

### Troubleshooting

**npm not found:**
```bash
# macOS
brew install node

# Linux
sudo apt install npm   # Debian/Ubuntu
sudo dnf install npm   # Fedora/RHEL

# Verify
npm --version
```

**Fonts not regenerating:**
```bash
# Force regeneration by touching the regen script
touch scripts/regen_mdi_fonts.sh
make generate-fonts
```

**Missing icons:**
```bash
# Validate all icons in codepoints.h are in the font
make validate-fonts
```

**Manual font generation:**
```bash
# Generate specific size
npm run convert-font-24

# Generate all fonts
npm run convert-all-fonts
```

## Icon Generation

The build system includes automated icon generation with platform-specific output formats.

### Quick Start

```bash
# Generate/regenerate icon from source logo
make icon
```

**Output:**
- **macOS**: `helix-icon.icns` (multi-resolution bundle) + `helix-icon.png` (650x650)
- **Linux**: `helix-icon.png` (650x650 for application use)

### Requirements

**Required:**
- `imagemagick` - Image processing (`magick` command)
  - macOS: `brew install imagemagick`
  - Ubuntu/Debian: `sudo apt install imagemagick`
  - Fedora/RHEL: `sudo dnf install ImageMagick`

**macOS only:**
- `iconutil` - macOS icon bundle creator (built-in on macOS)

### What It Does

The `make icon` target performs the following steps:

**All platforms:**
1. **Crops source logo** (`assets/images/helixscreen-logo.png`) to just the circular helix
2. **Creates square icon** at 650x650px with transparent background → `helix-icon.png`

**macOS only (additional steps):**
3. **Generates 12 resolutions**:
   - Standard: 16x16, 32x32, 64x64, 128x128, 256x256, 512x512
   - Retina (@2x): 32x32, 64x64, 128x128, 256x256, 512x512, 1024x1024
4. **Bundles into .icns** file using `iconutil` → `helix-icon.icns`
5. **Cleans up** temporary iconset directory

### Generated Files

**All platforms:**
- **`assets/images/helix-icon.png`** - Cropped square logo (650x650px, ~245KB)

**macOS only:**
- **`assets/images/helix-icon.icns`** - macOS icon bundle (~1.3MB with all resolutions)
- **`assets/images/icon.iconset/`** - Temporary directory (auto-deleted after .icns creation)

### Usage

**Cross-platform:**
- SDL window icons (programmatically via `SDL_SetWindowIcon()` with PNG)
- Linux `.desktop` files (Icon= field pointing to PNG)

**macOS specific:**
- macOS `.app` bundles with `Info.plist` (CFBundleIconFile pointing to .icns)
- Dock/Finder display when bundled as application

### Troubleshooting

**ImageMagick not found (macOS):**
```bash
brew install imagemagick
make icon
```

**ImageMagick not found (Linux):**
```bash
# Ubuntu/Debian
sudo apt install imagemagick

# Fedora/RHEL
sudo dnf install ImageMagick
```

**Linux output:**
- Linux builds generate PNG only (not .icns)
- This is expected - `.icns` is macOS-specific
- PNG icons work with SDL and Linux desktop environments

**Regenerating after logo changes:**
```bash
# Update assets/images/helixscreen-logo.png
make icon  # Regenerates all icon files (platform-specific)
```

## SVG to PNG Conversion

When converting SVG files to PNG for use in the project, **always use `rsvg-convert`** from the librsvg library.

### Why Not ImageMagick?

ImageMagick's SVG renderer doesn't correctly handle certain SVG features (transforms, filters, complex paths). This produces corrupted output—often solid white/black rectangles instead of the intended graphics.

### Using rsvg-convert

```bash
# Single file at specific size:
rsvg-convert logo.svg -w 64 -h 64 -o logo_64.png

# Batch convert all SVGs in a directory:
for svg in *.svg; do
  name="${svg%.svg}"
  rsvg-convert "$svg" -w 64 -h 64 -o "${name}_64.png"
done

# Common size options:
rsvg-convert input.svg -w 128 -h 128 -o output.png  # By pixel dimensions
rsvg-convert input.svg --dpi-x 192 --dpi-y 192 -o output.png  # By DPI
```

### Installation

The `librsvg` package is already tracked as a dependency for lv_img_conv. Install with:

```bash
# macOS
brew install librsvg

# Debian/Ubuntu
sudo apt install librsvg2-bin

# Fedora/RHEL
sudo dnf install librsvg2-tools
```

### Current Usage

- **AMS logos** (`assets/images/ams/`) - Multi-material system icons converted from SVG sources

---

## Build Targets

### Primary Targets

- **`all`** (default) - Build the main binary with dependency checks
- **`build`** - Clean parallel build with progress and timing
- **`clean`** - Remove all build artifacts
- **`run`** - Build and run the prototype
- **`help`** - Show comprehensive help with all targets and options

### Development Targets

- **`compile_commands`** - Merge compile command fragments into `compile_commands.json` (~1-2s)
- **`compile_commands_full`** - Full regeneration via compiledb/bear (slow, use if fragments corrupted)
- **`check-deps`** - Verify all build dependencies are installed
- **`apply-patches`** - Manually apply submodule patches (usually automatic)
- **`icon`** - Generate macOS .icns icon from logo (requires `imagemagick`, `iconutil`)

### Test Targets

The build system includes 30+ test targets organized by feature area. For the complete list with tag taxonomy and usage examples, see **[TESTING.md](TESTING.md)**.

**Quick reference:**
```bash
make test-run              # Run all tests in parallel (recommended)
make test-quick            # Fast subset for rapid iteration
./build/bin/helix-tests "[tag]"  # Run tests by tag (e.g., [printing], [ams])
```

### Demo Target

- **`demo`** - Build LVGL demo widgets (for LVGL API testing)

## Dependency Checking

Before building, the system automatically checks for required dependencies:

**Required:**
- `clang` / `clang++` - C/C++ compiler with C++17 support
- `cmake` - Build system for SDL2 when building from submodule (version 3.16+)
- Git submodules: `lvgl`, `wpa_supplicant` (auto-built by build system)

**Optional (uses system if available, otherwise builds from submodules):**
- `sdl2`, `spdlog`, `libhv` - Auto-detected and built only if not system-installed

**Optional:**
- `compiledb` or `bear` - Only for `compile_commands_full` (normal builds auto-generate)
- `imagemagick` - For screenshot conversion and icon generation
- `iconutil` - For macOS .icns icon generation (macOS only, built-in)

### Manual Dependency Check

```bash
make check-deps
```

Example output:
```
Checking build dependencies...
✓ clang found: Apple clang version 17.0.0
✓ clang++ found: Apple clang version 17.0.0
✓ SDL2: Using system version 2.32.10
✓ cmake found: cmake version 3.30.5
✓ libhv: Using submodule version
✓ spdlog: Using submodule version (header-only)
✓ LVGL found: lvgl

All dependencies satisfied!
```

If dependencies are missing, the check provides installation instructions.

## IDE/LSP Support (compile_commands.json)

The build system uses **incremental compile command generation** for fast IDE integration.

### How It Works

1. **During compilation**: Each `.o` file generates a `.ccj` (compile command JSON) fragment alongside it
2. **After build**: Fragments are automatically merged into `compile_commands.json`
3. **Adding new files**: Just compile them - fragments are created automatically

This replaces the slow `compiledb make -n -B` approach (which did a full dry-run) with instant merges.

### Usage

```bash
# Normal workflow - compile_commands.json is auto-updated after every build
make -j

# Manual merge (if you want to update without building)
make compile_commands      # ~1-2 seconds for ~1000 files

# Full regeneration (slow, use only if fragments are corrupted)
make compile_commands_full
```

### Fragment Storage

- Fragments are stored as `.ccj` files next to `.o` files in `build/obj/`
- They're automatically cleaned with `make clean`
- They're gitignored (inside `build/`)

### Troubleshooting

**compile_commands.json has missing entries:**
```bash
# Ensure all targets are built
make -j && make test-build
make compile_commands
```

**JSON validation errors:**
```bash
# Check if JSON is valid
python3 -m json.tool compile_commands.json > /dev/null

# If corrupted, do a full regeneration
make compile_commands_full
```

---

## Dependency Management

### Git Submodules

The project uses git submodules for external dependencies:

- `lvgl` - LVGL 9.5 graphics library (with automatic patches)
- `libhv` - HTTP/WebSocket client library (auto-built)
- `spdlog` - Logging library
- `wpa_supplicant` - WiFi control (Linux only, auto-built)

Additionally, `lib/helix-xml/` contains the extracted XML engine (originally from LVGL 9.4, MIT licensed). This is **not** a submodule — it lives directly in the repository with XML patches baked in permanently.

**Automatic handling**: Submodule dependencies are built automatically when missing. Patches are applied automatically before builds. Never commit changes directly to submodules - always create patches instead.

### SDL2

SDL2 is a system dependency installed via package manager:

```bash
# macOS
brew install sdl2

# Debian/Ubuntu
sudo apt install libsdl2-dev

# Fedora/RHEL
sudo dnf install SDL2-devel
```

The Makefile uses `sdl2-config` to auto-detect paths:
```makefile
SDL2_CFLAGS := $(shell sdl2-config --cflags)
SDL2_LIBS := $(shell sdl2-config --libs)
```

## Troubleshooting

### Patch Application Fails

**Symptom**: `⚠ Cannot apply patch (already applied or conflicts)`

**Causes**:
1. Submodule was manually modified (expected if patch is working)
2. Patch conflicts with newer LVGL version
3. Patch file is corrupted

**Solutions**:
```bash
# Check if file is modified (expected)
git -C lvgl diff src/drivers/sdl/lv_sdl_window.c

# Revert to original (re-applies patch on next build)
git -C lvgl checkout src/drivers/sdl/lv_sdl_window.c
make apply-patches

# Force re-apply
git -C lvgl checkout src/drivers/sdl/lv_sdl_window.c
git -C lvgl apply ../patches/lvgl_sdl_window_position.patch
```

### Build Performance

**Symptom**: Slow compilation

**Solutions**:
- Use parallel builds: `make -j` (auto-detects all cores)
- Use incremental builds: `make -j` instead of `make clean && make`
- Check CPU usage during build (should be near 100% with parallel builds)
- Use `make build` for optimized clean builds with timing

### Clang Standard Library Issues (Arch Linux)

**Symptom**: `fatal error: 'stdlib.h' file not found` at `#include_next <stdlib.h>`

**Cause**: Clang can't find GCC's libstdc++ headers on bleeding-edge distros (Arch with GCC 15+).

**Automatic Fix**: The build system detects this and auto-falls back to g++. You'll see:
```
Note: clang++ has stdlib issues on this system, using g++ instead
```

**Manual Override**: Force a specific compiler:
```bash
CXX=g++ CC=gcc make -j     # Use GCC
CXX=clang++ make -j        # Force Clang (may fail)
```

### SDL2 Not Found

**Symptom**: `sdl2-config: command not found`

**Solutions**:
```bash
# macOS
brew install sdl2

# Debian/Ubuntu
sudo apt install libsdl2-dev

# Verify installation
which sdl2-config
sdl2-config --version
```

## Best Practices

### Development Workflow

1. **Edit code** in `src/` or `include/`
2. **Run `make -j`** - parallel incremental build with auto-patching
3. **Test** with `./build/bin/helix-screen`
4. **Screenshot** with `./scripts/screenshot.sh` (auto-opens on display 1)
5. **Commit** with working incremental changes

For debugging build issues:
```bash
make clean
make V=1   # Verbose sequential build
```

### Clean Builds

Only use `make clean && make` when:
- Switching branches with significant changes
- Build artifacts are corrupted
- Troubleshooting mysterious build errors

**Avoid** clean rebuilds for normal development (wastes time).

### Submodule Management

**Never**:
- Commit changes directly to submodules
- Update submodule commits without testing
- Modify submodule files without creating patches

**Always**:
- Create patches for submodule changes
- Document patches in `patches/README.md`
- Test patch application on clean checkouts

## See Also

- **[README.md](../README.md)** - Project overview and quick start
- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Development environment, workflow, and contributing
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - System design and technical patterns
- **[CLAUDE.md](../CLAUDE.md)** - Development context and AI assistant guidelines
- **[patches/README.md](../patches/README.md)** - Patch documentation
