---
name: helix-build
description: >
  HelixScreen build system — triggers when editing Makefile, mk/*.mk (cross.mk, tests.mk, deps.mk, rules.mk,
  remote.mk, etc.), docker/Dockerfile.* build images, .github/workflows/ CI configs, mk/images.mk asset
  pipelines, mk/fonts.mk font generation, or mk/patches.mk LVGL patches. Also for cross-compilation targets
  (pi-docker, ad5m-docker, k1-docker, etc.), GCC 7.5 compatibility issues in MIPS32/ARM targets, Pi dual-link
  DRM+fbdev builds (mk/pi-dual-link.mk), display backend configuration (SDL/DRM/fbdev), ccache setup, or
  precompiled header changes (include/lvgl_pch.h).
---

# HelixScreen Build System (GNU Make + Docker)

Pure GNU Make — no CMake, no Meson. ~4,300 lines across 14 modular `.mk` files in mk/. Cross-compilation via docker/Dockerfile.* images for 10+ targets (ARM, MIPS, x86). CI/CD in .github/workflows/.

## Quick Reference

```bash
make -j                    # Parallel incremental build (recommended)
make dev                   # -O0 fast dev build (~2x faster)
make build                 # Clean parallel build with timing
make V=1                   # Verbose (full compiler commands)
make clean && make -j      # Full rebuild
make help                  # All targets and options
make check-deps            # Verify dependencies
make install-deps          # Interactive auto-install
```

## Build Configuration

| Flag | Default | Purpose |
|------|---------|---------|
| `V=1` | 0 | Verbose: show full commands |
| `OPT=0\|1\|2` | 2 | Optimization level (`dev` = `OPT=0 -j`) |
| `JOBS=N` | auto | Parallel job count |
| `NO_COLOR=1` | off | Disable colored output |
| `WERROR=1` | off | Treat warnings as errors (`make strict`) |
| `SANITIZE=address` | off | ASAN instrumentation |
| `ENABLE_MOCKS=no` | yes | Disable mock backends |

## Cross-Compilation Targets

All cross-compilation uses Docker — no local toolchain needed. Images auto-build on first use.

```bash
# Docker builds (recommended)
make pi-docker             # Raspberry Pi 64-bit (aarch64, DRM+GLES)
make pi-all-docker         # Pi 64-bit — DRM + fbdev in one pass
make pi32-docker           # Pi 32-bit (armhf, DRM+GLES)
make pi32-all-docker       # Pi 32-bit — both variants
make ad5m-docker           # FlashForge Adventurer 5M (armv7-a, fbdev)
make cc1-docker            # Elegoo Centauri Carbon 1 (armv7-a, fbdev)
make k1-docker             # Creality K1 static (MIPS32r2, musl)
make ad5x-docker           # FlashForge AD5X (MIPS32r5, glibc)
make k1-dynamic-docker     # Creality K1 dynamic (MIPS32r2, glibc)
make k2-docker             # Creality K2 (ARM, musl)
make snapmaker-u1-docker   # Snapmaker U1 (aarch64)
make x86-docker            # x86_64 Debian SBCs (DRM+GLES)
make x86-all-docker        # x86_64 — both DRM + fbdev

# Direct cross-compile (needs local toolchain)
make PLATFORM_TARGET=pi    # Same as above but without Docker
```

### Target Specifications

| Target | Arch | Display | C Lib | Docker Image |
|--------|------|---------|-------|--------------|
| Pi 64-bit | aarch64 | DRM+fbdev | glibc | `toolchain-pi` (Bullseye) |
| Pi 32-bit | armv7-a | DRM+fbdev | glibc | `toolchain-pi32` (Bullseye) |
| AD5M | armv7-a | fbdev 800×480 | glibc 2.25 | `toolchain-ad5m` (Buster) |
| CC1 | armv7-a | fbdev 480×272 | glibc 2.23 | `toolchain-cc1` (Bookworm) |
| K1 static | MIPS32r2 | fbdev 480×400 | musl | `toolchain-k1` (Bookworm) |
| K1 dynamic | MIPS32r2 | fbdev 480×400 | glibc 2.29 | `toolchain-k1-dynamic` |
| AD5X | MIPS32r5 | fbdev | glibc | `toolchain-ad5x` |
| K2 | armv7-a | fbdev 480×1600 | musl | `toolchain-k2` |
| Snapmaker U1 | aarch64 | DRM | glibc | `toolchain-snapmaker-u1` |
| x86 | x86_64 | DRM+fbdev | glibc | `toolchain-x86` (Bullseye) |

### Display Backends

| Backend | Define | Libraries | Use Case |
|---------|--------|-----------|----------|
| SDL | `HELIX_DISPLAY_SDL` | SDL2 | Desktop dev |
| DRM | `HELIX_DISPLAY_DRM` | libdrm, libinput, EGL, GLESv2, GBM | Pi GPU |
| fbdev | `HELIX_DISPLAY_FBDEV` | (none) | Embedded framebuffer |

## Modular Makefile Structure

| File | Lines | Purpose |
|------|-------|---------|
| `Makefile` | ~630 | Configuration, variables, platform detection, includes |
| `mk/cross.mk` | ~750 | Cross-compilation, toolchain setup, display backends, Docker targets |
| `mk/tests.mk` | ~880 | All test targets (unit, integration, by-feature) |
| `mk/deps.mk` | ~500 | Dependency checking, installation, libhv/wpa_supplicant builds |
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
| `mk/pi-dual-link.mk` | ~200 | Pi dual-link (compile once, link DRM + fbdev) |

## GCC 7.5 Compatibility (K1 Dynamic)

K1 dynamic uses GCC 7.5 — all code must stay compatible:

| Feature | Status | Workaround |
|---------|--------|------------|
| `std::from_chars` (int) | ❌ | `std::strtol` / `std::strtod` |
| `std::atomic<time_point>` | ❌ | `std::atomic<int64_t>` (nanoseconds) |
| C++20 designated init | ❌ | Explicit struct init + field assignment |
| `directory_entry` members | ❌ | Free functions: `std::filesystem::is_regular_file(path)` |
| `<filesystem>` | ✅ | Compat shim: `include/compat/filesystem` aliases `experimental::filesystem` |
| `-lstdc++fs` | required | Auto-added for `k1-dynamic` in `mk/cross.mk` |
| LTO (`-flto`) | ❌ | Disabled for `k1-dynamic`; plain `ar`/`ranlib` |

## Pi Dual-Link Build

Release builds produce DRM + fbdev binaries from a single compilation pass:
1. All sources compile once with DRM superset defines
2. Only 4 display-specific files recompile for fbdev
3. Two link steps produce separate binaries
4. `verify-fbdev` catches accidental DRM symbol leakage into fbdev binary

```bash
make pi-all-docker          # Both DRM + fbdev
make PLATFORM_TARGET=pi -j  # DRM only
```

## Key Patterns

### Build Output Tags
- `[CC]` cyan — C sources (LVGL)
- `[CXX]` blue — C++ sources
- `[FONT]`/`[ICON]` green — Asset compilation
- `[LD]` magenta — Linking
- `✓` green / `✗` red / `⚠` yellow — Status

### Compiler Selection
Priority: `CC`/`CXX` env > `clang++` (with stdlib test) > `g++`. On Arch Linux with broken Clang+libstdc++, auto-falls back to GCC.

### ccache
Auto-detected and wrapped around CC/CXX. ~7x speedup for rebuilds with unchanged source content.

### Precompiled Header
`include/lvgl_pch.h` covers LVGL, helix-xml, spdlog, nlohmann JSON, STL. External libs only — no project headers.

### Yocto Mode
`PLATFORM_TARGET=yocto` — bitbake provides CC/CXX with sysroot. No ccache, no submodule builds.

## Reference Files

- **[references/build-system.md](references/build-system.md)** — Build system internals: dependency management, library builds, submodule handling, patch system, font/icon generation, IDE/LSP support, build performance
- **[references/cross-compilation.md](references/cross-compilation.md)** — Docker toolchains, Dockerfile architecture, deploy targets, target-specific configuration
- **[references/makefile-reference.md](references/makefile-reference.md)** — Key Makefile variables, all build targets, linker flags per platform
- **[references/development.md](references/development.md)** — Dev environment setup, daily workflow, running the app, logging, testing modes, contributing
- **[references/ci-cd.md](references/ci-cd.md)** — GitHub Actions workflows, CI build steps, R2 release pipeline, quality checks
