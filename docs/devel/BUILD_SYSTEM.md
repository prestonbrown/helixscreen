# Build System Documentation

This document describes the HelixScreen prototype build system, including automatic patch application, multi-display support, and development workflows.

**For common development tasks**, see **[DEVELOPMENT.md](DEVELOPMENT.md)** - this document covers advanced build system internals.

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

# Build for Creality K1 (MIPS32, static/musl)
make mips-docker          # Or: make k1-docker (alias)

# Build for FlashForge AD5X (MIPS32r5, glibc)
make ad5x-docker

# Build for Creality K1 series (MIPS32, dynamic/glibc)
make k1-dynamic-docker

# Build for Creality K2 series (ARM, tested on K2 Plus)
make k2-docker

# Verify the binaries
file build/pi/bin/helix-screen     # ELF 64-bit LSB, ARM aarch64
file build/pi32/bin/helix-screen   # ELF 32-bit LSB, ARM, EABI5
file build/ad5m/bin/helix-screen   # ELF 32-bit LSB, ARM, EABI5
file build/cc1/bin/helix-screen    # ELF 32-bit LSB, ARM, EABI5
file build/mips/bin/helix-screen   # ELF 32-bit LSB, MIPS32 (static)
file build/ad5x/bin/helix-screen  # ELF 32-bit LSB, MIPS32r5 (dynamic)
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
| **Creality K1** | `make mips-docker` | MIPS32r2 (musl) | fbdev | `build/mips/` |
| **FlashForge AD5X** | `make ad5x-docker` | MIPS32r5 (glibc) | fbdev | `build/ad5x/` |
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

   Everything else the container needs from the host rides on `$(DOCKER_HOST_CONTEXT)`
   (`mk/cross.mk`), which every `docker run` that mounts the tree must pass —
   `tests/shell/test_build_provenance.bats` fails the build if one does not. It carries two
   things, both of which exist because a **git worktree** is not self-contained:

   - `DOCKER_WORKTREE_MOUNT` — `scripts/setup-worktree.sh` symlinks `lib/<submodule>` to the
     main checkout by absolute path, so those links dangle inside a container that mounts only
     `$(PWD)`. The real submodule tree is bind-mounted at its own absolute path so every
     `lib/*` link resolves identically inside and out.
   - `DOCKER_GIT_HASH_ENV` — a worktree's `.git` is a *file* reading
     `gitdir: $(MAIN)/.git/worktrees/<name>`, a path outside the mount, so git cannot resolve
     `HEAD` in the container and `scripts/gen-git-hash.sh` used to stamp
     `HELIX_GIT_HASH "unknown"` — silently, since the build still succeeded, leaving any
     evidence gathered on a printer unattributable. The hash is resolved on the host and
     passed in as `HELIX_GIT_HASH`, which the script prefers over its own lookup.

4. **Build Features Stamp**: The link rule writes `build/<platform>/bin/.build-features`
   recording which optional subsystems the binary actually contains (currently
   `remote_control`). `make deploy-*` runs as a separate make invocation with no
   `PLATFORM_TARGET` and so cannot re-derive what the cross build chose; the stamp carries it
   across, and the deploy turns on the matching runtime switch on the device. See
   `docs/devel/HELIXCTL.md`.

5. **Display Backend Selection**: Cross-compilation automatically selects the appropriate display backend:
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
make k2-docker           # Creality K2 series via Docker (tested on K2 Plus)
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

#### Creality K2 Series (K2, K2 Pro, K2 Plus) — Tested on K2 Plus
- **CPU**: Allwinner sun8iw20p1 (ARM Cortex-A7, dual-core, 57 BogoMIPS)
- **Toolchain**: Bootlin `armv7-eabihf-musl` (GCC 12, musl libc)
- **Display**: 480x800 fbdev (all K2 models; framebuffer is double-buffered → 480x1600 virtual)
- **Input**: evdev for touch
- **C Library**: musl (static linking)
- **RAM**: ~488 MB
- **Moonraker**: Port 7125 (direct), port 4408 (nginx proxy)
- **Docker Image**: `helixscreen/toolchain-k2` (Debian Bookworm)
- **OS**: OpenWrt 21.02-SNAPSHOT, Linux 5.4.61 armv7l, procd init (NOT systemd)
- See `docs/devel/printers/CREALITY_K2_SUPPORT.md` for full hardware details.

### Dockerfile Architecture

```
docker/
├── Dockerfile.pi          # Pi 64-bit toolchain (Debian Bullseye, GCC 10)
├── Dockerfile.pi32        # Pi 32-bit toolchain (Debian Bullseye, GCC 10)
├── Dockerfile.ad5m        # AD5M toolchain (Debian Buster, GCC 8)
├── Dockerfile.cc1         # CC1 toolchain (Debian Bookworm, ARM GCC 10.3)
├── Dockerfile.k1          # K1 static toolchain (Bootlin mips32el-musl, GCC 12)
├── Dockerfile.k1-dynamic  # K1 dynamic toolchain (crosstool-NG, GCC 7.5, glibc 2.29)
├── Dockerfile.k2          # K2 toolchain (Bootlin armv7-eabihf-musl, GCC 12)
├── Dockerfile.ad5x        # AD5X toolchain (MIPS, ZMOD)
├── Dockerfile.snapmaker-u1 # Snapmaker U1 toolchain
└── Dockerfile.x86         # x86 native/container build
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
make deploy-pi                    # Deploy binaries + assets, restart in background
make deploy-pi-fg                 # Deploy and run in foreground (debug)

# Customize target (default PI_HOST is 192.168.1.113, NOT helixpi.local — which doesn't resolve)
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
# Create at custom path — the second argument is a PATH, not a base branch
./scripts/setup-worktree.sh feature/foo /tmp/helixscreen-foo

# Branch from something other than the current HEAD
./scripts/setup-worktree.sh --base feature/parent feature/child

# Set up existing worktree without creating
./scripts/setup-worktree.sh --setup-only feature/i18n

# Skip the initial build
./scripts/setup-worktree.sh --no-build feature/quick-test
```

A new branch is cut from the HEAD of **whichever tree you run the script in**,
which is not necessarily `main` — the script prints the resolved base commit so
a stale or unexpected one is visible immediately. Use `--base <ref>` to pick it
explicitly. Passing a branch name as the second argument is rejected, as is any
path inside the repo outside `.worktrees/`.

### What setup-worktree.sh Does

The script optimizes for **fast builds** by sharing artifacts from the main tree:

1. **Symlinks lib/** — third-party submodules symlinked (no clone/configure time).
   `lib/helix-xml` is the exception: it is **ours** and CLAUDE.md says to edit it directly
   rather than carry a patch, so a symlink would put every worktree's engine edits into the
   *main* tree's submodule working copy — shared with every other worktree, and showing up as
   dirt in main's `git status` for another session to sweep. It gets a private per-worktree
   checkout instead (~2.6 MB, seconds), listed in `LIB_PRIVATE_SUBMODULES`. Its gitdir lands
   under `.git/worktrees/<name>/modules/`, `origin` stays the public GitHub remote, and
   because a real checkout is what git already expects it needs no `--unlink`/`--relink`.
2. **Adopts the main tree's mtimes** for every byte-identical file — without this, nothing below actually saves you anything (see next section)
3. **Clones compiled libraries** — `libhv.a`, `libwpa_client.a` from main tree
4. **Clones the precompiled header** — `lvgl_pch.h.gch` (27MB)
5. **Symlinks tools** — `node_modules/`, `.venv/`
6. **Clones build objects** — copies `build/obj/` and `build/generated/` from the main tree (APFS clonefile on macOS; plain copy on Linux)
7. **Configures ccache for cross-worktree reuse** — so the worktree builds against the *same* ccache the main tree populated, when ccache is installed (see below)
8. **Validates architecture** — wrong-arch `.o`/`.a` files (left by a prior cross-compile) are detected and cleared so `make` rebuilds them correctly
9. **Configures git** — `.git/info/exclude` + `--skip-worktree` keep `git status` clean despite the symlinks

**Trade-off**: If you need to modify library code (`lib/`), un-symlink that specific directory first (`rm lib/<name> && cp -a $MAIN/lib/<name> lib/`).

> **Build outputs are cloned, never symlinked.** `libhv.a` and `lvgl_pch.h.gch` used to be
> symlinks into the main tree. They are build *outputs*, so make rewrites them — and both `cp`
> (the tail of `make libhv-build`) and `clang -o` follow a symlink, writing straight into the
> main tree. Measured: one fresh-worktree build moved the main tree's `libhv.a` mtime forward
> five days, which left the main tree's own PCH older than it and put ~1900 objects back on the
> main tree's next build. On APFS `cp -c` is a clonefile, so a private copy costs nothing.

### Why worktree builds are fast

A fresh `git worktree add` stamps **every** file with the checkout mtime, so make sees the whole
tree as newer than the cloned objects. It bites twice over:

- `$(PCH)` lists `include/lvgl_pch.h` and `lv_conf.h` as prerequisites, and *every* C++ object
  lists `$(PCH)` — one fresh header invalidates all ~1970 objects;
- the `.d` files list `include/*.h` per object, and those are fresh too — so fixing only the PCH
  would still leave every object out of date.

Measured on a fresh worktree of an up-to-date main tree: **1945 of 1967 cloned objects
recompiled**, 396s of wall clock, for a tree where nothing had changed.

`setup-worktree.sh` fixes this by walking the checkout and, **for each file whose content is
byte-identical to the main tree's file at the same path**, adopting that file's mtime
(`scripts/sync-worktree-mtimes.py`, ~1s for the 3900-file tree). The build markers
`build/.patches-applied` and `.fonts.stamp` get the same treatment — they are prerequisites of
the PCH, libhv, and all 46 font objects, so stamping them `now` was on its own worth ~560
recompiles *and* was what made every fresh worktree rebuild libhv over the main tree's copy.

This is deliberately **not** blanket mtime back-dating, which is how you ship a stale build. The
invariant is that for matching content the (source, object) mtime ordering in the worktree equals
the ordering in the main tree, so make reaches the same up-to-date decision here that it reached
there, against objects cloned from there. Anything that differs — a branch with real changes, an
edit made after setup — keeps its fresh mtime and rebuilds normally:

| Scenario | Objects rebuilt |
|----------|-----------------|
| Worktree at the same commit, clean | 0 (`make test -j` = 3.2s, link only) |
| Worktree at an older commit (1 source differs) | 1 — and the binary carries the *old* code |
| Source edited after setup | 1 |
| `lv_conf.h` edited | PCH + 1888 |

What it inherits rather than fixes: if the main tree's own build is stale, the worktree
reproduces that staleness. The object clone always had that property.

> **If a fresh worktree still takes minutes, this is almost always why.** `lib/` is symlinked
> from the main tree, so the libhv submodule is *shared by every tree*. Rebuilding libhv anywhere
> regenerates `lib/libhv/include/hv/json.hpp`, which is a `$(PCH)` prerequisite — so the PCH, and
> with it all ~1970 objects, goes out of date in the main tree and in every worktree at once. The
> mtime sync cannot repair it: the script deliberately never follows the `lib/` symlinks, and
> `git ls-files` does not reach inside a submodule. Observed during this work: a worktree that
> should have built in 5s took 462s (680 objects + PCH) purely because an unrelated tree had
> rebuilt libhv twenty minutes earlier. The fix is to let one tree rebuild once — after that
> every tree is fast again. Un-sharing `lib/` would remove the coupling entirely, but that is the
> core of the current worktree design.

### The ccache config the script sets

ccache is optional (it is not installed on every dev box — check with `command -v ccache`), and
with the mtime sync in place it is no longer what makes a *clean* worktree fast. Where it matters
is every build that genuinely has to recompile — after touching `lv_conf.h`, or after another tree
rebuilds libhv and re-invalidates the shared PCH (see the box above). That is the difference
between ~400s and a few tens of seconds.

> **`sloppiness` is not optional — without it ccache caches nothing here.** Every native build
> compiles with `-include $(PCH)`, and ccache refuses to cache *any* compilation that uses a
> precompiled header unless `sloppiness` permits it. Measured on a single `-include` compile:
> `Uncacheable calls: 1/1 (100%)` before, `Cacheable calls: 1/1 (100%)` after, with the repeat
> compile hitting. This was silently true for a long time — `base_dir` and `hash_dir` were being
> set while the cache stayed empty, which is why this doc used to credit ccache for a speedup it
> was never delivering. **Both** flags are required; `pch_defines` alone still measures 0%
> cacheable. If ccache has been installed on a machine for a while, check `ccache --get-config
> sloppiness` before assuming it has been doing anything.
>
> The cost of `time_macros` is that ccache stops hashing `__DATE__`/`__TIME__`. Exactly one site
> uses them — `ui_settings_about.cpp` reads `__DATE__ + 7` for the About screen's copyright year —
> so across a New Year that screen can show the previous year until the file is next recompiled.
> Cosmetic, and the only such site in `src/`, `include/`, `lib/helix-xml/`, or the build flags.

But it only helps across worktrees if it is configured for it. The native build compiles with `-g` (debug info), and ccache's default `hash_dir=true` folds the absolute working directory into the cache key — so an object cached while building in the main tree never matches the same source compiled under `.worktrees/<name>/`. Every worktree would start stone cold.

`setup-worktree.sh` fixes this once, by writing global ccache config (only when unset, so it never clobbers a value you chose):

| ccache setting | Set to | Why |
|----------------|--------|-----|
| `base_dir` | `$HOME` | Rewrites absolute paths under `$HOME` to relative before hashing, so main-tree and worktree paths collapse to the same key |
| `hash_dir` | `false` | Stops folding the cwd (the `-g` debug-path component) into the key |
| `sloppiness` | `pch_defines,time_macros` | Without it ccache refuses to cache any `-include $(PCH)` compile — i.e. all of them. See the box above for the `__DATE__` tradeoff |
| `max_size` | `25G` (raised, never lowered) | The default 5 GiB thrashes once several worktrees + cross-compile caches share it, re-causing cold misses |

For the script's own initial build it also exports `CCACHE_BASEDIR` (the longest common ancestor of the main tree and the worktree, so it works even for out-of-tree paths like `/tmp/foo`), `CCACHE_NOHASHDIR=1`, and `CCACHE_SLOPPINESS`.

These are global ccache settings, so they apply to normal main-tree builds too, not just worktrees — running `setup-worktree.sh` once is enough to fix a whole machine. Worth doing on every machine you build on, including remote build hosts: an existing ccache install with `base_dir`/`hash_dir` already set can still be caching nothing if `sloppiness` was never configured.

> **Caveat:** `hash_dir=false` is global, so cached objects carry whichever `DW_AT_comp_dir` (debug source path) compiled them first. For throwaway dev worktrees this is cosmetic, but gdb inside a worktree may point at the main-tree paths. If you do serious in-worktree debugging, build that target with `CCACHE_DISABLE=1`.

Verify the cache is actually being shared after a build:

```bash
ccache -s        # "Hits" should climb sharply on the 2nd+ worktree build
ccache -p | grep -E 'base_dir|hash_dir|max_size'
```

### Typical worktree workflow

```bash
# 1. Spin up an isolated workspace for a feature (builds automatically)
./scripts/setup-worktree.sh feature/my-feature
cd .worktrees/my-feature

# 2. Iterate — XML-only changes need no rebuild (loaded at runtime + hot reload is ON by default)
./build/bin/helix-screen --test -vv
# ...C++ changes:
make -j && ./build/bin/helix-screen --test -vv

# 3. Run the relevant tests before committing
make test-run

# 4. Commit in the worktree (it's a normal checkout on its own branch)
git add -A && git commit -m "feat(scope): ..."

# 5. When done, merge/push from the worktree, then tear it down (see Cleanup)
```

If a worktree already exists but its symlinks/objects drifted (e.g. after a `git submodule update` in the main tree), re-run setup in place — it's idempotent:

```bash
cd .worktrees/my-feature && ../../scripts/setup-worktree.sh --setup-only --no-build .
# (or just `../../scripts/setup-worktree.sh` with no args from inside the worktree —
#  it auto-detects the branch and path)
```

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
- **Modular design**: ~9,100 lines split across the top-level `Makefile` plus 17 `mk/*.mk` modules for maintainability
- **Color-coded output** for easy visual parsing
- **Verbosity control** to show/hide full compiler commands
- **Automatic dependency checking** before builds with smart canvas detection
- **Interactive installation** of missing dependencies (`make install-deps`)
- **Automatic code formatting** for C/C++ and XML files
- **Fail-fast error handling** with clear diagnostics
- **Parallel build support** with output synchronization
- **Build timing** for performance tracking

**XML stays runtime-loaded — compile-time codegen was measured and declined.** A build-time
XML-to-C component compiler was designed and measured (2026-08-08; the design doc has since
been deleted) and declined: runtime-loaded XML with hot reload won on build complexity and
iteration speed. Revisit only if startup cost on SPI-flash platforms demands it.

### Modular Makefile Structure

The build system is organized into focused modules:

| File | Lines | Purpose |
|------|-------|---------|
| `Makefile` | ~1095 | Configuration, variables, platform detection, module includes |
| `mk/cross.mk` | ~2990 | Cross-compilation, toolchain setup, display backends |
| `mk/tests.mk` | ~924 | All test targets (unit, integration, by-feature) |
| `mk/deps.mk` | ~672 | Dependency checking, installation, libhv/wpa_supplicant |
| `mk/patches.mk` | ~666 | LVGL patch application |
| `mk/rules.mk` | ~561 | Compilation rules, linking, main build targets |
| `mk/remote.mk` | ~297 | Remote deployment (Pi, AD5M) |
| `mk/fonts.mk` | ~283 | Font/icon generation, Material icons |
| `mk/images.mk` | ~264 | Image conversion (PNG, SVG) |
| `mk/tools.mk` | ~251 | Development tool targets |
| `mk/pi-dual-link.mk` | ~249 | Pi dual-link build (compile once, link DRM + fbdev) |
| `mk/watchdog.mk` | ~198 | Hardware watchdog support |
| `mk/splash.mk` | ~183 | Splash screen generation |
| `mk/translations.mk` | ~168 | Translation string generation |
| `mk/format.mk` | ~110 | Code and XML formatting |
| `mk/bluetooth.mk` | ~86 | Bluetooth support |
| `mk/display-lib.mk` | ~77 | Display library configuration |
| `mk/filaments.mk` | ~22 | Filament database generation |

Each module is self-contained with GPL-3 copyright headers and clear separation of concerns.

### Quick Start

```bash
# Parallel build (auto-detects CPU cores)
make -j

# Fast development build (-O0, ~2x faster compilation)
make dev

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
- **`OPT=0|1|2`** - Optimization level (default: 2). Use `OPT=0` for fastest compilation, `OPT=2` for release. `make dev` is shorthand for `OPT=0 -j`.
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
2. **Generate patch** — scope the diff to the files you touched, or you will capture every
   other patch that is currently applied:
   ```bash
   cd lib/lvgl
   git diff src/path/to/file.c > ../../patches/my-new-patch.patch
   ```
   If more than one patch touches that file, even a scoped diff folds the others in. Sixteen
   files are shared today (`src/misc/lv_event.c` by seven patches). Check with
   `grep -l "diff --git a/<path>" patches/*.patch` and use the pristine-file method in
   `patches/README.md` § "Regenerating a patch whose file is shared".
3. **Update Makefile** to apply the patch in the `apply-patches` target. Use
   `git -C $(LVGL_DIR) apply --check <patch>` as the apply condition rather than a
   "is file X dirty?" test, which breaks as soon as another patch touches X.
4. **Document** in `patches/README.md`

### Patch Gotchas (hard-won)

Two traps cost a full debugging session on 2026-06-14 when the libhv DNS resolver
fallback patch silently stopped reaching the binary on the AD5M (on-machine
update checks failed with "Connection failed"). Both are now regression-tested in
`tests/shell/test_libhv_dns_resolver_patch.bats`.

1. **Guard on the actual change, not on a side effect.** A patch that adds NEW
   files *and* edits an existing one must not gate re-application on the new
   file's existence. The old guard used `[ ! -f base/dns_resolv.c ]`; a submodule
   reset reverted the tracked `base/hsocket.c` (the wiring) but left the
   untracked `dns_resolv.c` orphaned, so the guard declared "already applied" and
   never re-wired `hsocket.c`. Result: resolver compiled but **never called**.
   Guard on a marker string *inside the edited file* and self-heal:
   ```make
   if ! grep -q "dns_resolv_resolve" "$(LIBHV_DIR)/base/hsocket.c"; then
       rm -f .../base/dns_resolv.c .../base/dns_resolv.h;   # drop orphans
       git -C $(LIBHV_DIR) checkout -- base/hsocket.c;       # pristine
       git -C $(LIBHV_DIR) apply .../libhv-dns-resolver-fallback.patch
   fi
   ```

2. **A patched file compiled into a static `.a` must invalidate that `.a`.**
   `$(LIBHV_LIB)` (build/<plat>/lib/libhv.a) originally had **no
   prerequisites** → built once, never rebuilt when a patch changed the libhv
   source. Because the resolver *call site* lives only in `hsocket.c` (→ inside
   `libhv.a`) while `dns_resolv.c` is compiled separately into the app, a stale
   archive kept a pristine `hsocket.o` (pure `getaddrinfo` → `EAI_SYSTEM` /
   `ret=-11` on static glibc) across every rebuild. **This is dev-only** — a
   fresh CI build has no `libhv.a` yet — but the fix is to depend on the stamp:
   ```make
   $(LIBHV_LIB): $(PATCHES_STAMP)
   ```
   When in doubt, `rm build/<plat>/lib/libhv.a` to force a clean archive, and
   confirm a patch's marker actually made it in: `strings <binary> | grep <sym>`.

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
./build/bin/helix-screen -d 1 -s small --skip-splash
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
npm run convert-noto-24

# Generate all fonts
npm run convert-noto-all
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

## Make Target Reference

The Makefile is **self-documenting** — these help targets are the authoritative, always-current list (the tables below are a curated tour of the typical ones):

| Command | Shows |
|---------|-------|
| `make help` | The common build/dependency/quality targets |
| `make help-build` | Build, dependency, and patch targets |
| `make help-test` | Test targets and test discovery |
| `make help-cross` | Cross-compilation + per-device deployment targets and options |
| `make help-remote` | Remote build system (build on a fast host, fetch binaries) |
| `make help-images` / `help-splash` / `help-watchdog` | Asset/splash/watchdog targets |
| `make help-all` | **Everything**, all topics combined |
| `make cross-info` | Current cross-compile configuration (platform, backend) |

### Native build & run

| Target | What it does |
|--------|--------------|
| `make -j` (`all`) | Build the main binary (default), `-O2`, auto-parallel |
| `make dev` | Fast build at `-O0` (~2× faster compile; larger/slower binary) |
| `make OPT=1 -j` | `-O1` middle ground |
| `make build` | Clean build with progress + timing |
| `make run` | Build and run the UI |
| `make clean` | Remove build artifacts (keeps deps) |
| `make distclean` | Deep clean to fresh-checkout state |
| `make V=1 …` | Verbose (show full compiler commands) |
| `make JOBS=N …` | Cap parallel job count |

Run flags worth knowing: `./build/bin/helix-screen --test -vv` (mock printer + DEBUG logs, hot reload ON by default for live XML editing).

### Tests

The build system has 30+ test targets by feature area; see **[TESTING.md](TESTING.md)** for the tag taxonomy. Most-used:

| Target | What it does |
|--------|--------------|
| `make test` | Build tests (does **not** run them) |
| `make test-run` | Build **and** run tests in parallel (recommended, ~4–8× faster) |
| `make test-serial` | Run sequentially (debugging thread issues) |
| `make test-smoke` | Quick smoke subset (~30s) for rapid iteration |
| `make test-all` | All tests incl. `[slow]` |
| `make test-asan` / `test-tsan` | Run under Address/Thread sanitizer |
| `make test-list-tags` | List available tags |
| `make test-xml` | Configure, build and run the helix-xml engine suite (CMake + Unity + ctest) - **not** part of `make test` |
| `./build/bin/helix-tests "[tag]"` | Run a specific tag (e.g. `[ams]`, `[gcode]`) |

`make test-xml` is the odd one out: it drives the standalone suite in the `lib/helix-xml/`
submodule rather than `helix-tests`. Its build tree is `build/helix-xml-tests/` (outside the
submodule) and LVGL comes from CMake `FetchContent` pinned at v9.5.0, so the **first**
configure needs network and takes minutes; every run after that is a no-op configure plus a
couple of seconds of ctest. Forward extra ctest args with
`make test-xml HELIX_XML_CTEST_ARGS='-R test_expr'`. See
**[TESTING.md](TESTING.md)** § "helix-xml Engine Tests".

### Code quality & IDE

| Target | What it does |
|--------|--------------|
| `make format` | clang-format all C/C++ + xmllint XML |
| `make format-staged` | Format only staged files (pre-commit) |
| `make quality` | All quality checks (formatting, headers, conflicts) |
| `make setup-hooks` | Enable the git pre-commit hook |
| `make compile_commands` | Merge `compile_commands.json` for clangd (~1–2s) |
| `make compile_commands_full` | Full regen via bear/compiledb (slow; use if fragments corrupt) |

### Dependencies & patches

| Target | What it does |
|--------|--------------|
| `make check-deps` | Verify build dependencies (see below) |
| `make install-deps` | Interactively install missing deps |
| `make venv-setup` | Create `.venv` with Python asset/telemetry deps |
| `make libs-clean` | Clean all built library artifacts |
| `make apply-patches` | Apply LVGL/libhv patches (idempotent; auto-run before builds) |
| `make reapply-patches` | Force re-apply (repair manually-edited patched files) |

### Asset regeneration

Usually invoked after editing icons/images. See **[Font Generation](#font-generation)** and **[Icon Generation](#icon-generation)** for the full pipeline.

| Target | What it does |
|--------|--------------|
| `make regen-fonts` | Regenerate MDI icon fonts via `scripts/regen_mdi_fonts.sh` |
| `make regen-text-fonts` | Regenerate Noto Sans text fonts (incl. CJK) |
| `make regen-icon-consts` | Regenerate icon string constants in `globals.xml` |
| `make validate-fonts` | Verify every codepoint is present in the compiled fonts |
| `make regen-images` | Regenerate pre-rendered splash images (all sizes) |
| `make gen-printer-images` | Pre-render printer DB images |
| `make translations` | Regenerate translation tables from YAML |
| `make translation-coverage` | Show per-language translation coverage |

### Cross-compile, deploy & remote build

These get their own deep section above — see **[Cross-Compilation](#cross-compilation-embedded-targets)**. Shape of it:

- **Build:** `make <target>-docker` (recommended, no local toolchain) or `make <target>` (needs host toolchain). Targets: `pi`, `pi32`, `ad5m`, `ad5x`, `cc1`, `k1`, `k1-dynamic`, `k2`, `snapmaker-u1`, `x86`.
- **Deploy + run on device:** `make <target>-test` (build + deploy + run fg), `make deploy-<target>` (background), `deploy-<target>-fg` (foreground), `deploy-<target>-bin` (binaries only, fast iteration), `<target>-ssh`.
- **Host override:** `make deploy-pi PI_HOST=192.168.1.50`. Defaults live in `mk/cross.mk` — note `PI_HOST` actually defaults to `192.168.1.113` (the `make help-cross` text saying `helixpi.local` is stale, and `helixpi.local` does not resolve). `K2_HOST` has **no** default and must be supplied.
- **Remote build:** `make remote-pi` / `remote-ad5m` / `remote-native` build on a fast Linux host (`REMOTE_HOST`, default `thelio.local`) and fetch the binaries back. `make remote-status` checks readiness.

### Utilities

| Target | What it does |
|--------|--------------|
| `make demo` | Build LVGL demo widgets (LVGL API testing) |
| `make symbols` | Extract `.sym` + `.debug` (crash-backtrace resolution) |
| `make strip` | Strip the binary for release |
| `make screenshots` | Generate documentation screenshots |
| `make print-cxxflags` / `print-ldflags` | Dump resolved flags (build debugging) |

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

Additionally, `lib/helix-xml/` is the XML engine — a permanent MIT fork taken from LVGL at `a15dcbeb5` (`v9.4.0-358`), the last commit before v9.5 removed XML from core. It is a submodule, but **ours**: [prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml). That makes its workflow the opposite of every other submodule here — edit the files directly, commit and push inside `lib/helix-xml/`, then commit the bumped pointer in this repo. It gets no `patches/*.patch` entry, and it is excluded from clang-format. See `HELIX_XML_FORK.md`.

Being its own repo, it also carries its own tests and its own CI. `lib/helix-xml/tests/` is a standalone CMake + Unity suite that builds the engine against a pinned upstream LVGL v9.5.0 rather than our patched `lib/lvgl`; run it with `make test-xml` (`make test` does not build it), and see **[TESTING.md](TESTING.md)** § "helix-xml Engine Tests". `.github/workflows/ci.yml` *inside* the submodule covers a gcc + clang matrix, ASAN/UBSAN, and a conf-guards job. `scripts/quality-checks.sh` runs the suite on commits that stage a `lib/helix-xml` change, but only once `build/helix-xml-tests/` has been configured by hand - the first configure fetches LVGL and is too slow for a commit hook.

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

The build compiles ~566 app source files. The dominant cost is **template instantiation and optimization passes** (not header parsing — preprocessing takes <1s even for the worst files). At `-O2`, individual files take 8–15s to compile.

**Speed tiers** (full app rebuild, 32-core machine):

| Method | Wall time | Notes |
|--------|-----------|-------|
| `make -j` (cold, no ccache) | ~4.5 min | Baseline |
| `make dev` (cold, no ccache) | ~2.5 min | `-O0` skips optimizer passes |
| `make -j` (ccache populated) | ~38 sec | 98% cache hit rate |
| `make dev` (ccache populated) | ~20 sec | `-O0` + ccache |
| Touch one `.cpp`, rebuild | ~7 sec | Recompile + relink |
| Touch widely-included `.h` | ~8 sec | ccache direct hit (content unchanged) |

**Solutions (most impactful first)**:

1. **Install ccache** — by far the biggest win. The Makefile auto-detects and wraps the compiler. Gives ~7x speedup for rebuilds where source content hasn't actually changed (e.g., switching branches back and forth, touching headers without real edits).
   ```bash
   # Ubuntu/Debian
   sudo apt install ccache
   # macOS
   brew install ccache
   ```

2. **Use `make dev` for daily development** — builds at `-O0`, cutting per-file compile time roughly in half. Library code still builds at `-O2` since it rarely changes. The binary is larger and slower at runtime, but compilation is ~2x faster.
   ```bash
   make dev          # -O0, auto-parallel
   make OPT=1 -j    # -O1 (middle ground: some optimization, faster than -O2)
   make -j           # -O2 (default, for release/CI)
   ```

3. **Use parallel builds**: `make -j` (auto-detects all cores)
4. **Use incremental builds**: `make -j` instead of `make clean && make`

**Header fan-out** — changing these headers triggers the most recompilation:

| Header | Files affected |
|--------|---------------|
| `theme_manager.h` | ~144 |
| `ui_update_queue.h` | ~144 |
| `moonraker_api.h` | ~122 |
| `app_globals.h` | ~121 |
| `printer_state.h` | ~104 |

With ccache installed, touching these headers without content changes costs ~8s (direct cache hit). Actual content changes recompile all dependents (~2 min at `-O2`, ~1 min at `-O0`).

**Precompiled header** (`include/lvgl_pch.h`): Covers LVGL, helix-xml, spdlog, nlohmann JSON, and common STL headers. These are precompiled once and reused across all translation units. Don't add project headers to the PCH — only stable external libraries.

#### ccache across worktrees and Docker cross-builds

ccache (`~/.ccache`) is shared per-user, but two things stop it from being reused as widely as you'd expect:

**1. Worktree path mismatch (native builds).** Because the native build compiles with `-g` and ccache defaults to `hash_dir=true`, the absolute working directory is part of the cache key — so the same source in `.worktrees/foo/` misses everything the main tree cached. `setup-worktree.sh` configures ccache (`base_dir=$HOME`, `hash_dir=false`, `max_size=25G`) so worktree builds reuse the main tree's objects. See **[Git Worktrees → Why worktree builds are fast](#why-worktree-builds-are-fast-and-the-ccache-config-the-script-sets)** for the full rationale and caveats. If you build outside `$HOME` (e.g. `/tmp`), set `CCACHE_BASEDIR` to a common ancestor yourself.

**2. Docker cross-builds use a separate, per-target cache.** Containers can't see `~/.ccache`, so each `*-docker` target bind-mounts its own persistent cache directory (`mk/cross.mk`):

```makefile
DOCKER_CCACHE_BASE ?= $(HOME)/.cache/helixscreen-ccache
docker-ccache-args = -v "$(DOCKER_CCACHE_BASE)/$(1)":/ccache -e CCACHE_DIR=/ccache
```

So `make pi-docker` caches into `~/.cache/helixscreen-ccache/pi/`, `make ad5m-docker` into `.../ad5m/`, etc. — one cache per architecture (they must stay separate; a Pi aarch64 object is meaningless to an AD5M armv7-a build). First cross-build of a target is cold; subsequent ones hit ~98%. Override the base location with `DOCKER_CCACHE_BASE=/path make pi-docker`. To wipe a single target's cache, `rm -rf ~/.cache/helixscreen-ccache/<target>`.

Concurrent Docker cross-builds are serialized by `scripts/cross-compile-lock.sh` to avoid thrashing the machine — this is automatic.

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

## Makefile Variable & Target Reference

Absorbed from the `helix-build` skill so there is one home for it. Values verified against
`Makefile` and `mk/*.mk`.

### Compiler and flags

| Variable | Default | Notes |
|----------|---------|-------|
| `CC` | clang > gcc | Auto-detected, with a stdlib test for clang |
| `CXX` | clang++ > g++ | Auto-falls back on a broken libstdc++ |
| `OPT` | `2` | Optimization level: 0 (dev), 1, 2 (release) |
| `CXXFLAGS` | `-std=c++17 -Wall -Wextra -O{OPT} -g` | Extended per platform |
| `SUBMODULE_CFLAGS` | `-std=c11 -O2 -g -D_GNU_SOURCE -w` | Third-party code, warnings suppressed |
| `DEPFLAGS` | `-MMD -MP` | Header dependency tracking |

### Version

Read from `VERSION.txt` (MAJOR.MINOR.PATCH) and injected as `-DHELIX_VERSION="..."`:
`HELIX_VERSION`, `HELIX_VERSION_MAJOR` / `_MINOR` / `_PATCH`.

The short git hash is **not** a global define. It changes on every commit, and
`VERSION_DEFINES` lands on every translation unit's command line, which ccache's
direct mode hashes — so a global `-DHELIX_GIT_HASH` invalidated the whole
project's cache on every push (measured: 86% direct hits on a sha matching the
restored cache, 43.7% on any other, and a 12-minute build became two hours).
`scripts/gen-git-hash.sh` writes it to `build/generated/helix_git_hash.h`
instead, rewriting only when the hash changes, and only
`src/system/helix_version.cpp` includes it. Read it through `helix_git_hash()`
from `helix_version.h`; the macro is not visible anywhere else.

### Build directories

| Variable | Default | Per-platform override |
|----------|---------|-----------------------|
| `BUILD_DIR` | `build` | `mk/cross.mk` sets per platform |
| `BIN_DIR` | `build/bin` | e.g. `build/pi/bin` |
| `OBJ_DIR` | `build/obj` | e.g. `build/pi/obj` |
| `BUILD_SUBDIR` | (none) | Platform name: `pi`, `ad5m`, `k1` |

### Cross-compile variables

| Variable | Purpose |
|----------|---------|
| `TARGET_ARCH` | Target architecture for the active cross target |
| `TARGET_TRIPLE` | Toolchain triple (e.g. `aarch64-linux-gnu`) |
| `STRIP_BINARY` | Whether to strip the output binary for size |
| `FONT_TIERS` | Which font size tiers to embed for the target — see below |

#### FONT_TIERS

Font faces are the largest single chunk of `.rodata`, so each target links only the
tiers it can actually display. Legal values are `all` (the default, `mk/fonts.mk:107`)
or any subset of `micro tiny small medium large xlarge xxlarge`. Assignments live per
target in `mk/cross.mk`:

| `PLATFORM_TARGET` | Tiers |
|-------------------|-------|
| `pi`, `pi-fbdev`, `pi-both`, `pi32`, `pi32-fbdev`, `pi32-both` | `all` |
| `x86`, `x86-fbdev`, `x86-both`, `native` | `all` |
| `ad5m`, `ad5m-br`, `ad5x` | `medium large` |
| `mips` / `k1`, `k1-dynamic` | `small medium` |
| `k2` | `large xlarge` |
| `snapmaker-u1` | `tiny small` |
| `cc1`, `yocto` | `micro tiny` |

`HELIX_MAX_FONT_TIER` is derived from this (`mk/cross.mk:728-750`; `micro=0` …
`xxlarge=6`). Two consumers read it: `theme_manager` uses it to distinguish an
expected-missing font (pruned by tier) from an unexpected-missing one (a build bug),
and `cjk_font_manager` uses it to pick its CJK face.

The consequence for layout work: a `<string>` token naming a face outside the
target's tiers silently fails to register on that target, and the token falls back
down the ladder. If you add a font token for a large tier, check it against the
tier list of the smallest device that will run it.

### Feature gates

| Variable | Default | Purpose |
|----------|---------|---------|
| `ENABLE_SDL` | yes (native) | SDL2 desktop display |
| `ENABLE_OPENGLES` | per target | EGL/GLES GPU rendering |
| `ENABLE_GLES_3D` | yes (Linux) | 3D gcode rendering |
| `ENABLE_SCREENSAVER` | yes (desktop/Pi) | Flying toasters |
| `ENABLE_MOCKS` | yes | Mock backends for development |
| `ENABLE_SSL` | per target | OpenSSL for HTTPS/WSS |
| `HELIX_HAS_LABEL_PRINTER` | 1 | Label printer feature |
| `HELIX_HAS_CFS` | 1 | CFS feature |
| `HELIX_HAS_IFS` | 1 | IFS feature |

### Linker flags by platform

```
macOS          -lSDL2 -lhv -lz -lm -lpthread -liconv
               -framework Foundation -framework CoreWLAN -framework CoreLocation …
Linux native   -lSDL2 -lhv -lwpa_client -lusb-1.0 -lssl -lcrypto -ldl -lstdc++fs -lGLESv2
Pi (aarch64)   -L/usr/lib/aarch64-linux-gnu -lhv -lwpa_client -lnl-genl-3 -lnl-3
               -ldrm -linput -lEGL -lGLESv2 -lgbm -lusb-1.0 -lssl -lcrypto -lasound …
K1 static      -lhv -lwpa_client -lnl-genl-3 -lnl-3 -latomic -ldl -lz -lm -lpthread
K1 dynamic     -Wl,-Bstatic -lhv -lwpa_client -lnl-genl-3 -lnl-3 -lstdc++fs
               -Wl,-Bdynamic -lstdc++ -lz -lm -lpthread -lrt -ldl -latomic -lgcc_s
K2 (ARM musl)  -lhv -lwpa_client -lnl-genl-3 -lnl-3 -ldl -lz -lm -lpthread
Yocto          preserves bitbake LDFLAGS, appends the project libs
```

### Source organization

Application sources are `src/*.cpp` at up to three directory levels, plus `src/*.mm` for the
macOS Objective-C++ WiFi code. `src/tools/*.cpp` and `src/bluetooth/*.cpp` build under separate
rules.

Excluded from the main binary: `test_*.cpp`, `src/helix_splash.cpp` and
`src/helix_watchdog.cpp` (separate binaries), and `src/lvgl-demo/main.cpp`.

Bundled libraries: `lib/lvgl/` (XML and expat sources excluded — those live in
`lib/helix-xml/`), `lib/helix-xml/`, `lib/lv_markdown/`, `lib/quirc/`, `lib/cpp-terminal/`.
LVGL's thorvg `.cpp` files compile separately.

### Targets

| Build | |
|-------|-|
| `all` | Default: main binary with dependency checks |
| `build` | Clean parallel build with timing |
| `dev` | `OPT=0 -j` fast development build |
| `strict` | Build with `-Werror` |
| `clean` | Remove all build artifacts |
| `install` | Stage to `DESTDIR/opt/helixscreen/` |

| Code quality | |
|--------------|-|
| `format` / `format-staged` | clang-format + xmllint, all or staged files |
| `quality` | Run `scripts/quality-checks.sh` |
| `check-deps` / `install-deps` | Verify, then interactively install dependencies |

| Assets | |
|--------|-|
| `regen-fonts` / `generate-fonts` | Regenerate MDI icon fonts |
| `validate-fonts` | Verify icons are present in the compiled fonts |
| `icon` | Generate the app icon from the logo |
| `apply-patches` / `reset-patches` | Apply or revert submodule patches |

| Tools and debug | |
|-----------------|-|
| `compile_commands` / `compile_commands_full` | Merge `.ccj` fragments (~1-2s) or fully regenerate |
| `moonraker-inspector` / `tools` | Diagnostic tools |
| `symbols` / `strip` | Extract `.sym` + `.debug`, or strip for size (cross-compile only) |
| `print-ldflags` / `print-target-cflags` / `print-cxxflags` / `print-strip` | Print computed values |

## Best Practices

### Development Workflow

1. **Edit code** in `src/` or `include/`
2. **Run `make dev`** - fast build at `-O0` with auto-patching (or `make -j` for optimized build)
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

**spdlog submodule:** uses the fmt-11.2.0 branch. Initialize with
`git submodule update --init --recursive` on a fresh clone.

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
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - The 15-minute whole-app model + chapter-series routing
- **[CLAUDE.md](../CLAUDE.md)** - Development context and AI assistant guidelines
- **[patches/README.md](../../patches/README.md)** - Patch documentation
