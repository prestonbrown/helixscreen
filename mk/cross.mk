# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - Cross-Compilation Module
# Handles cross-compilation for embedded ARM targets
#
# Usage:
#   make                       # Native build (SDL)
#   make PLATFORM_TARGET=pi    # Cross-compile for Raspberry Pi (aarch64, DRM+GLES)
#   make PLATFORM_TARGET=pi-fbdev  # Cross-compile for Pi (aarch64, fbdev fallback)
#   make PLATFORM_TARGET=pi32  # Cross-compile for Raspberry Pi (armhf, DRM+GLES)
#   make PLATFORM_TARGET=pi32-fbdev  # Cross-compile for Pi (armhf, fbdev fallback)
#   make PLATFORM_TARGET=pi-both  # Pi 64-bit: compile once, link DRM + fbdev
#   make PLATFORM_TARGET=pi32-both  # Pi 32-bit: compile once, link DRM + fbdev
#   make PLATFORM_TARGET=ad5m  # Cross-compile for Adventurer 5M (armv7-a, bundled toolchain)
#   make PLATFORM_TARGET=ad5m-br # Adventurer 5M via buildroot-provided toolchain (kmod)
#   make PLATFORM_TARGET=cc1   # Cross-compile for Centauri Carbon 1 (armv7-a)
#   make PLATFORM_TARGET=mips  # Cross-compile for MIPS32 devices (K1)
#   make PLATFORM_TARGET=k1    # Alias for mips (Creality K1 series)
#   make PLATFORM_TARGET=ad5x  # Cross-compile for FlashForge AD5X (mips)
#   make PLATFORM_TARGET=k2    # Cross-compile for Creality K2 series (ARM)
#   make PLATFORM_TARGET=snapmaker-u1 # Cross-compile for Snapmaker U1 (aarch64)
#   make PLATFORM_TARGET=x86   # Build for x86_64 Debian SBCs (DRM+GLES)
#   make PLATFORM_TARGET=x86-fbdev  # Build for x86_64 Debian SBCs (fbdev fallback)
#   make PLATFORM_TARGET=x86-both   # x86_64: compile once, link DRM + fbdev
#   make pi-docker             # Docker-based Pi build (64-bit, DRM+GLES)
#   make pi-fbdev-docker       # Docker-based Pi build (64-bit, fbdev fallback)
#   make pi-all-docker         # Docker-based Pi build (both variants)
#   make pi32-docker           # Docker-based Pi build (32-bit, DRM+GLES)
#   make pi32-fbdev-docker     # Docker-based Pi build (32-bit, fbdev fallback)
#   make pi32-all-docker       # Docker-based Pi build (both 32-bit variants)
#   make ad5m-docker           # Docker-based AD5M build
#   make cc1-docker            # Docker-based CC1 build
#   make k1-docker             # Docker-based K1/MIPS build
#   make ad5x-docker           # Docker-based AD5X/MIPS build
#   make k2-docker             # Docker-based K2 build
#   make snapmaker-u1-docker   # Docker-based Snapmaker U1 build
#   make x86-docker            # Docker-based x86_64 build (DRM+GLES)
#   make x86-fbdev-docker      # Docker-based x86_64 build (fbdev fallback)
#   make x86-all-docker        # Docker-based x86_64 build (both variants)

# =============================================================================
# Docker Build Configuration
# =============================================================================

# NPROC_DOCKER_RUN: Number of parallel jobs for Docker builds
# Capped at 8 to prevent resource exhaustion in containerized environments
# Can be overridden via environment variable: NPROC_DOCKER_RUN=<value>
_NPROC_HOST := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
NPROC_DOCKER_RUN ?= $(shell echo $$(($(_NPROC_HOST) > 8 ? 8 : $(_NPROC_HOST))))

# =============================================================================
# Target Platform Definitions
# =============================================================================

# Note: We use PLATFORM_TARGET to avoid collision with Makefile's TARGET (binary path)
PLATFORM_TARGET ?= native

ifeq ($(PLATFORM_TARGET),pi)
    # -------------------------------------------------------------------------
    # Raspberry Pi (Mainsail OS) - aarch64 / ARM64
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    # Include paths for cross-compilation:
    # - /usr/aarch64-linux-gnu/include: arm64 sysroot headers
    # - /usr/include/libdrm: drm.h (needed by xf86drmMode.h)
    # -Wno-error=conversion: LVGL headers have int32_t->float conversions that GCC 12 flags
    # -DHELIX_RELEASE_BUILD: Disables debug features like LV_USE_ASSERT_STYLE
    # -funwind-tables: Emit ARM unwind info (.ARM.exidx) so backtrace() can walk
    # the full call stack in crash reports. ~5-10% code size increase, zero runtime cost.
    # -fno-omit-frame-pointer: required for the crash handler's fp_walk_backtrace /
    # stack-scan fallback (libgcc backtrace() can't cross the signal frame on ARM).
    # Without it SIGABRT reports get garbage frames. Matches ad5x/cc1/k1/k2/snapmaker-u1.
    TARGET_CFLAGS := -march=armv8-a -fno-omit-frame-pointer -funwind-tables -I/usr/aarch64-linux-gnu/include -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := yes
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    # SSL enabled for HTTPS/WSS support
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi
    # Strip binary for size - embedded targets don't need debug symbols
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),pi-fbdev)
    # -------------------------------------------------------------------------
    # Raspberry Pi (aarch64) - fbdev only, no GL dependencies
    # Fallback for systems without EGL/GLES2/GBM libraries.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    TARGET_CFLAGS := -march=armv8-a -fno-omit-frame-pointer -funwind-tables \
        -I/usr/aarch64-linux-gnu/include \
        -Wno-error=conversion -Wno-error=sign-conversion \
        -DHELIX_RELEASE_BUILD -DHELIX_BINARY_VARIANT=\"fbdev\"
    DISPLAY_BACKEND := fbdev
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi-fbdev
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),pi-both)
    # -------------------------------------------------------------------------
    # Raspberry Pi (aarch64) - Dual-link mode: compile once, link DRM + fbdev
    # Produces both build/pi/bin/helix-screen (DRM) and build/pi-fbdev/bin/helix-screen (fbdev)
    # in a single compilation pass. Used by CI to cut build time in half.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    TARGET_CFLAGS := -march=armv8-a -fno-omit-frame-pointer -funwind-tables -I/usr/aarch64-linux-gnu/include -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := yes
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi
    STRIP_BINARY := yes
    FONT_TIERS := all
    PI_DUAL_LINK := yes

else ifeq ($(PLATFORM_TARGET),pi32)
    # -------------------------------------------------------------------------
    # Raspberry Pi 32-bit (MainsailOS armhf) - armv7-a hard-float
    # -------------------------------------------------------------------------
    # For 32-bit Raspberry Pi OS / MainsailOS (armv7l).
    # Covers Pi 2/3/4/5 running 32-bit userland.
    # Same strategy as 64-bit Pi: dynamic linking, static OpenSSL, DRM display.
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-linux-gnueabihf
    # -funwind-tables: Emit ARM unwind info (.ARM.exidx) so backtrace() can walk
    # the full call stack in crash reports. ~5-10% code size increase, zero runtime cost.
    # -fno-omit-frame-pointer: required for the crash handler's fp_walk_backtrace /
    # stack-scan fallback (libgcc backtrace() can't cross the signal frame on ARM).
    # Without it SIGABRT reports get garbage frames. Matches ad5x/cc1/k1/k2/snapmaker-u1.
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-omit-frame-pointer -funwind-tables \
        -I/usr/arm-linux-gnueabihf/include -I/usr/include/libdrm \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_PI32 -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := yes
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi32
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),pi32-fbdev)
    # -------------------------------------------------------------------------
    # Raspberry Pi 32-bit (armhf) - fbdev only, no GL dependencies
    # Fallback for systems without EGL/GLES2/GBM libraries.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-linux-gnueabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-omit-frame-pointer -funwind-tables \
        -I/usr/arm-linux-gnueabihf/include \
        -Wno-error=conversion -Wno-error=sign-conversion \
        -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_PI32 -DHELIX_BINARY_VARIANT=\"fbdev\"
    DISPLAY_BACKEND := fbdev
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi32-fbdev
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),pi32-both)
    # -------------------------------------------------------------------------
    # Raspberry Pi 32-bit (armhf) - Dual-link mode: compile once, link DRM + fbdev
    # Produces both build/pi32/bin/helix-screen (DRM) and build/pi32-fbdev/bin/helix-screen (fbdev)
    # in a single compilation pass. Used by CI to cut build time in half.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-linux-gnueabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-omit-frame-pointer -funwind-tables \
        -I/usr/arm-linux-gnueabihf/include -I/usr/include/libdrm \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_PI32 -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := yes
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi32
    STRIP_BINARY := yes
    FONT_TIERS := all
    PI_DUAL_LINK := yes

else ifeq ($(PLATFORM_TARGET),ad5m)
    # -------------------------------------------------------------------------
    # Flashforge Adventurer 5M - Cortex-A7 (armv7-a hard-float)
    # Specs: 800x480 display, 110MB RAM, glibc 2.25
    # -------------------------------------------------------------------------
    # FULLY STATIC BUILD: Link everything statically to avoid glibc version
    # conflicts. The ARM toolchain's sysroot has glibc 2.33 symbols, but AD5M
    # only has glibc 2.25. Static linking sidesteps this entirely.
    # Trade-off: Larger binary (~5-8MB vs ~2MB) but guaranteed compatibility.
    CROSS_COMPILE ?= arm-none-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-none-linux-gnueabihf
    # Memory-optimized build flags:
    # -Os: Optimize for size (vs -O2 for speed)
    # -flto: Link-Time Optimization for dead code elimination
    # -ffunction-sections/-fdata-sections: Allow linker to remove unused sections
    # -Wno-error=conversion: LVGL headers have int32_t->float conversions that GCC flags
    # -DHELIX_RELEASE_BUILD: Disables debug features like LV_USE_ASSERT_STYLE
    # NOTE: AD5M framebuffer is 32bpp (ARGB8888), as is lv_conf.h (LV_COLOR_DEPTH=32)
    # -funwind-tables: Emit ARM unwind info (.ARM.exidx) so backtrace() can walk
    # the full call stack in crash reports. ~5-10% code size, zero runtime cost.
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a7 \
        -Os -flto -ffunction-sections -fdata-sections -funwind-tables \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_AD5M \
        -DHELIX_HAS_LABEL_PRINTER=0 -DHELIX_HAS_CFS=0 -DHELIX_HAS_IFS=0
    # -Wl,--gc-sections: Remove unused sections during linking (works with -ffunction-sections)
    # -flto: Must match compiler flag for LTO to work
    # -static: Fully static binary - no runtime dependencies on system libs
    # This avoids glibc version mismatch (binary needs 2.33, system has 2.25)
    # -lstdc++fs: Required for std::experimental::filesystem on GCC 10.x
    TARGET_LDFLAGS := -Wl,--gc-sections -flto -static -lstdc++fs
    # SSL enabled for HTTPS/WSS support with Moonraker
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := ad5m
    # Strip binary for size on memory-constrained device
    STRIP_BINARY := yes
    FONT_TIERS := medium large

else ifeq ($(PLATFORM_TARGET),ad5m-br)
    # -------------------------------------------------------------------------
    # Flashforge Adventurer 5M - buildroot-compat variant for AD5M Klipper Mod
    # integration. Parallels `ad5m` but:
    #   - Expects CROSS_COMPILE from caller (buildroot's arm-buildroot-linux-gnueabihf-)
    #   - No -static (buildroot links dynamically against its sysroot)
    #   - No strip (buildroot strips target binaries itself)
    #   - Dynamic OpenSSL (buildroot ships libssl; no static-link needed)
    # All other CPU tuning, size optimizations, and asset gates match `ad5m`.
    # -------------------------------------------------------------------------
    # CROSS_COMPILE is NOT defaulted here — buildroot passes it via TARGET_CONFIGURE_OPTS.
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-buildroot-linux-gnueabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a7 \
        -Os -flto -ffunction-sections -fdata-sections -funwind-tables \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_AD5M \
        -DHELIX_HAS_LABEL_PRINTER=0 -DHELIX_HAS_CFS=0 -DHELIX_HAS_IFS=0
    # No -static — buildroot wants dynamic linking against its sysroot
    TARGET_LDFLAGS := -Wl,--gc-sections -flto -lstdc++fs
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := ad5m-br
    # No strip — buildroot strips target binaries itself
    STRIP_BINARY := no
    FONT_TIERS := medium large

else ifeq ($(PLATFORM_TARGET),ad5x)
    # -------------------------------------------------------------------------
    # AD5X: Ingenic X2600, 800x480, multi-color IFS
    # Specs: 800x480 display
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= mipsel-buildroot-linux-gnu-
    TARGET_ARCH := mips32r5
    TARGET_TRIPLE := mipsel-buildroot-linux-gnu
    # Memory-optimized build flags:
    # -Os: Optimize for size (vs -O2 for speed)
    # -flto: Link-Time Optimization for dead code elimination
    # -ffunction-sections/-fdata-sections: Allow linker to remove unused sections
    # -Wno-error=conversion: LVGL headers have int32_t->float conversions that GCC flags
    # -DHELIX_RELEASE_BUILD: Disables debug features like LV_USE_ASSERT_STYLE
    # NOTE: ad5x framebuffer is 32bpp (ARGB8888), as is lv_conf.h (LV_COLOR_DEPTH=32)
    # -funwind-tables: Emit DWARF unwind info so backtrace() can walk the full
    # call stack in crash reports. Small code size cost, zero runtime cost.
    TARGET_CFLAGS := -march=mips32r5 -mtune=mips32r5 -mabi=32 -mnan=2008 -mfp64 \
        -Os -flto -ffunction-sections -fdata-sections -fno-omit-frame-pointer -funwind-tables \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_AD5X
    # -Wl,--gc-sections: Remove unused sections during linking (works with -ffunction-sections)
    # -flto: Must match compiler flag for LTO to work
    TARGET_LDFLAGS := -Wl,--gc-sections -flto
    # SSL enabled for HTTPS/WSS support with Moonraker
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := ad5x
    # Strip binary for size on memory-constrained device
    STRIP_BINARY := yes
    FONT_TIERS := medium large

else ifeq ($(PLATFORM_TARGET),cc1)
    # -------------------------------------------------------------------------
    # Elegoo Centauri Carbon 1 - Allwinner R528 (armv7-a Cortex-A7)
    # Specs: 480x272 display, 112MB RAM, glibc 2.23
    # -------------------------------------------------------------------------
    # FULLY STATIC BUILD: Same strategy as AD5M. Link everything statically to
    # avoid glibc version conflicts. CC1 has glibc 2.23 (even older than AD5M's
    # 2.25), making static linking essential.
    # Trade-off: Larger binary (~5-8MB vs ~2MB) but guaranteed compatibility.
    CROSS_COMPILE ?= arm-none-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-none-linux-gnueabihf
    # Memory-optimized build flags (same as AD5M — similar hardware constraints):
    # -Os: Optimize for size (vs -O2 for speed)
    # -flto: Link-Time Optimization for dead code elimination
    # -ffunction-sections/-fdata-sections: Allow linker to remove unused sections
    # -Wno-error=conversion: LVGL headers have int32_t->float conversions that GCC flags
    # -DHELIX_RELEASE_BUILD: Disables debug features like LV_USE_ASSERT_STYLE
    # NOTE: CC1 framebuffer is 32bpp (ARGB8888), as is lv_conf.h (LV_COLOR_DEPTH=32)
    # -funwind-tables: Emit ARM unwind info (.ARM.exidx) so backtrace() can walk
    # the full call stack in crash reports. ~5-10% code size, zero runtime cost.
    # NOTE: ALSO required for C++ exception unwinding — cannot be dropped while
    # libstdc++/libhv/openssl (which throw) are statically linked. Attempting to
    # strip via -fno-unwind-tables yielded zero delta (tables come from libstdc++).
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a7 \
        -Os -flto -ffunction-sections -fdata-sections -funwind-tables \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_CC1
    # -Wl,--gc-sections: Remove unused sections during linking (works with -ffunction-sections)
    # -flto: Must match compiler flag for LTO to work
    # -static: Fully static binary - no runtime dependencies on system libs
    # This avoids glibc version mismatch (binary needs 2.33, system has 2.23)
    # -lstdc++fs: Required for std::experimental::filesystem on GCC 10.x
    TARGET_LDFLAGS := -Wl,--gc-sections -flto -static -lstdc++fs
    # SSL enabled for HTTPS/WSS support with Moonraker
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := cc1
    # Mock backends are dev/test scaffolding. The Makefile defaults ENABLE_MOCKS
    # to yes and no cross target has ever overridden it, so every shipped device
    # binary has carried the full mock Moonraker client. mk/cross.mk is included
    # before the Makefile's `?=`, so setting it here wins. The
    # #ifdef HELIX_ENABLE_MOCKS guards at every consumer are already complete --
    # the ESP32 port builds this way today.
    ENABLE_MOCKS := no
    # Strip binary for size on memory-constrained device
    STRIP_BINARY := yes
    FONT_TIERS := micro tiny
    # HELIX_LANG filter (mk/translations.mk) is available here but NOT set —
    # users must be able to pick any of the 9 shipped locales at runtime. When
    # the runtime-loaded translation .bin infrastructure lands, we can ship
    # English compiled-in + all others on disk and get the ~500 KB win back.

else ifneq ($(filter mips k1,$(PLATFORM_TARGET)),)
    # -------------------------------------------------------------------------
    # MIPS32 Devices (Creality K1) - Ingenic XBurst2
    # K1: Ingenic X2000E, 480x400, 256MB RAM
    # MIPS32r2, musl libc, fbdev display, evdev touch
    # -------------------------------------------------------------------------
    # FULLY STATIC BUILD with musl: Cleaner than glibc static linking.
    # No getaddrinfo warnings, smaller binaries, guaranteed portability.
    # Uses Bootlin's mips32el musl toolchain (same as pellcorp/grumpyscreen).
    CROSS_COMPILE ?= mipsel-buildroot-linux-musl-
    TARGET_ARCH := mips32r2
    TARGET_TRIPLE := mipsel-buildroot-linux-musl
    # Optimized build flags for Ingenic MIPS32r2 (XBurst2):
    #
    # Architecture flags:
    # -march=mips32r2: Target instruction set (X2000E/X2600 are MIPS32 R5 compatible)
    # -mtune=mips32r2: Tune for MIPS32r2 pipeline
    #
    # Size optimization:
    # -Os: Optimize for size
    # -fomit-frame-pointer: Don't keep frame pointer (saves registers/stack)
    # -funwind-tables: Emit DWARF unwind info for crash backtrace quality
    # -fmerge-all-constants: Merge duplicate string/numeric constants
    # -fno-ident: Don't embed GCC version string
    #
    # LTO and dead code elimination:
    # -flto=auto: Link-Time Optimization with parallel jobs
    # -ffunction-sections/-fdata-sections: Enable per-function/data sections
    #
    # Note: -mno-abicalls/-mno-shared omitted - they break configure tests
    # for submodule builds even though final binary is static
    #
    TARGET_CFLAGS := -march=mips32r2 -mtune=mips32r2 \
        -Os -flto=auto -ffunction-sections -fdata-sections \
        -fno-omit-frame-pointer -funwind-tables \
        -fmerge-all-constants -fno-ident \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_MIPS
    # Linker flags:
    # -Wl,--gc-sections: Remove unused sections (works with -ffunction-sections)
    # -flto=auto: Match compiler LTO flag, uses all CPUs
    # -static: Fully static binary - musl makes this clean and portable
    # -Wl,-O2: Linker optimization level
    # -Wl,--as-needed: Only link libraries that are actually used
    TARGET_LDFLAGS := -Wl,--gc-sections -Wl,-O2 -Wl,--as-needed -flto=auto -static
    # SSL enabled for HTTPS/WSS support (updates, remote Moonraker)
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := mips
    # Strip binary for size on memory-constrained device
    STRIP_BINARY := yes
    FONT_TIERS := small medium

else ifeq ($(PLATFORM_TARGET),k1-dynamic)
    # -------------------------------------------------------------------------
    # Creality K1 Series - Dynamic Linking (Ingenic X2000E MIPS32r2)
    # Specs: 480x400 display (K1/K1C/K1Max), 480x800 (K2), 256MB RAM, glibc 2.29
    # -------------------------------------------------------------------------
    # DYNAMIC BUILD: Links against K1's native glibc 2.29 system libraries.
    # Requires custom NaN2008+FP64 toolchain (built via crosstool-NG).
    # Project-specific libraries (libhv, libnl, wpa) are statically linked.
    # System libraries (libc, libstdc++, libm, etc.) are dynamically linked.
    CROSS_COMPILE ?= mipsel-k1-linux-gnu-
    TARGET_ARCH := mips32r2
    TARGET_TRIPLE := mipsel-k1-linux-gnu
    # NaN2008 + FP64 flags are critical for ABI compatibility with K1 firmware
    # No LTO: GCC 7.5 static toolchain doesn't ship liblto_plugin.so
    # -isystem include/compat: Filesystem shim — GCC 7 only has <experimental/filesystem>
    # -funwind-tables: Emit DWARF unwind info so backtrace() can walk the full
    # call stack in crash reports. Small code size cost, zero runtime cost.
    TARGET_CFLAGS := -march=mips32r2 -mtune=mips32r2 -mnan=2008 -mfp64 \
        -Os -ffunction-sections -fdata-sections -funwind-tables \
        -fno-omit-frame-pointer \
        -isystem include/compat \
        -Wno-error=conversion -Wno-error=sign-conversion \
        -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_K1
    # Dynamic linking with NaN2008 dynamic linker
    # NO -static flag! System libs resolved at runtime on the K1.
    TARGET_LDFLAGS := -Wl,--gc-sections -Wl,-O2 -Wl,--as-needed \
        -Wl,--dynamic-linker=/lib/ld-linux-mipsn8.so.1
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := k1-dynamic
    STRIP_BINARY := yes
    FONT_TIERS := small medium

else ifeq ($(PLATFORM_TARGET),k2)
    # -------------------------------------------------------------------------
    # Creality K2 Series - Allwinner T113 / sun8iw20p1 (ARM Cortex-A7, dual-core)
    # Specs: 480x800 display (portrait), ~512MB RAM, glibc 2.29 rootfs (Tina Linux)
    # -------------------------------------------------------------------------
    # FULLY STATIC BUILD with musl: Same proven strategy as K1 target.
    # Uses Bootlin's armv7-eabihf musl toolchain (32-bit ARM hard-float).
    #
    # We target armv7 (the Cortex-A7's native 32-bit ISA) because Tina Linux
    # (OpenWrt) commonly uses 32-bit userland, and entware packages are armv7sf.
    #
    # The K2 root filesystem is glibc 2.29, NOT musl — but we ship a FULLY
    # STATIC binary built with the Bootlin musl toolchain, so it runs regardless
    # of the device libc. See docs/devel/printer-research/CREALITY_K2_PLUS_RESEARCH.md.
    CROSS_COMPILE ?= arm-buildroot-linux-musleabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-buildroot-linux-musleabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
        -O2 -flto=auto -ffunction-sections -fdata-sections \
        -fno-omit-frame-pointer -funwind-tables \
        -fmerge-all-constants -fno-ident \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_K2
    TARGET_LDFLAGS := -Wl,--gc-sections -Wl,-O2 -Wl,--as-needed -flto=auto -static
    # HTTPS is required for the update check, R2 self-update download, telemetry,
    # and crash/debug-bundle upload. (Local Moonraker is plain HTTP and works
    # regardless of this.) Static OpenSSL is cross-built into the K2 toolchain
    # sysroot by docker/Dockerfile.k2, mirroring the K1 musl target.
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := k2
    STRIP_BINARY := yes
    FONT_TIERS := large xlarge

else ifeq ($(PLATFORM_TARGET),snapmaker-u1)
    # -------------------------------------------------------------------------
    # Snapmaker U1 - Rockchip RK3562 ARM64 (aarch64)
    # Specs: 3.5" 480x320 32bpp display, 961MB RAM, Debian Trixie, glibc
    # -------------------------------------------------------------------------
    # DRM backend with DRM keepalive: Platform hooks hold /dev/dri/card0 open
    # before killing the stock UI, preventing CRTC teardown. HelixScreen then
    # takes over as DRM master for proper double-buffered page flipping.
    # No libinput — uses evdev for touch input (no libinput on device).
    # Static-link libstdc++/libgcc to avoid ABI mismatch (device has GCC 12).
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    TARGET_CFLAGS := -march=armv8-a -fno-omit-frame-pointer -funwind-tables -Os -flto -ffunction-sections -fdata-sections \
        -I/usr/include/libdrm \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_SNAPMAKER_U1
    TARGET_LDFLAGS := -Wl,--gc-sections -flto -static-libstdc++ -static-libgcc
    SNAPMAKER_SKIP_LIBINPUT := yes
    ENABLE_SSL := yes
    DISPLAY_BACKEND := drm
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := snapmaker-u1
    STRIP_BINARY := yes
    FONT_TIERS := tiny small

else ifeq ($(PLATFORM_TARGET),x86)
    # -------------------------------------------------------------------------
    # x86_64 Debian SBCs - DRM/KMS display
    # -------------------------------------------------------------------------
    # Native x86_64 build inside a Bullseye Docker container for glibc 2.31
    # compatibility. Same dynamic-linking strategy as Pi: system glibc,
    # static OpenSSL. Runs on both Debian Bullseye and Bookworm x86_64.
    CROSS_COMPILE :=
    TARGET_ARCH := x86_64
    TARGET_TRIPLE := x86_64-linux-gnu
    # -funwind-tables: Emit unwind info so backtrace() can walk the full
    # call stack in crash reports. Negligible code size cost, zero runtime cost.
    TARGET_CFLAGS := -march=x86-64 -funwind-tables -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_X86 -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := yes
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := x86
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),x86-fbdev)
    # -------------------------------------------------------------------------
    # x86_64 Debian SBCs - fbdev only, no GL dependencies
    # Fallback for systems without EGL/GLES2/GBM libraries.
    # -------------------------------------------------------------------------
    CROSS_COMPILE :=
    TARGET_ARCH := x86_64
    TARGET_TRIPLE := x86_64-linux-gnu
    TARGET_CFLAGS := -march=x86-64 -funwind-tables \
        -Wno-error=conversion -Wno-error=sign-conversion \
        -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_X86 -DHELIX_BINARY_VARIANT=\"fbdev\"
    DISPLAY_BACKEND := fbdev
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := x86-fbdev
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),x86-both)
    # -------------------------------------------------------------------------
    # x86_64 Debian SBCs - Dual-link mode: compile once, link DRM + fbdev
    # Produces both build/x86/bin/helix-screen (DRM) and build/x86-fbdev/bin/helix-screen (fbdev)
    # in a single compilation pass. Used by CI to cut build time in half.
    # -------------------------------------------------------------------------
    CROSS_COMPILE :=
    TARGET_ARCH := x86_64
    TARGET_TRIPLE := x86_64-linux-gnu
    TARGET_CFLAGS := -march=x86-64 -funwind-tables -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_X86 -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := yes
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := x86
    STRIP_BINARY := yes
    FONT_TIERS := all
    PI_DUAL_LINK := yes

else ifeq ($(PLATFORM_TARGET),yocto)
    # -------------------------------------------------------------------------
    # Yocto / BitBake cross-build (OpenCentauri COSMOS → Centauri Carbon 1 today;
    # generic enough for any future Yocto machine/BSP).
    #
    # Bitbake's EXTRA_OEMAKE passes CC/CXX/AR/LD/STRIP/RANLIB as full paths and
    # populates CFLAGS/CXXFLAGS/LDFLAGS/PKG_CONFIG env with the target sysroot,
    # --march, security, and optimization flags. We add nothing that could
    # conflict — the recipe's DEPENDS supplies every vendored library we'd
    # otherwise build from submodule (libhv, spdlog, fmt, alsa-lib, libdrm,
    # libusb, libnl, wpa-supplicant, etc.). See docs/devel/YOCTO_BUILD.md.
    # -------------------------------------------------------------------------
    # Bitbake passes CROSS_COMPILE=yocto- as a sentinel; clear it so the
    # prefix-based CC/CXX override below does not run (bitbake's CC already
    # has the full path + machine flags baked in).
    override CROSS_COMPILE :=
    TARGET_ARCH := yocto
    TARGET_TRIPLE := yocto
    # Bitbake drives CFLAGS/LDFLAGS via env; don't add platform flags here.
    TARGET_CFLAGS :=
    TARGET_LDFLAGS :=
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := yocto
    # Don't strip — bitbake's package split handles debug/strip via FILES:${PN}-dbg.
    STRIP_BINARY := no
    # CC1 has 112MB RAM; trim fonts like the existing cc1 target.
    FONT_TIERS := micro tiny
    # Skip all in-tree submodule dep builds — Yocto recipe provides via DEPENDS.
    YOCTO_BUILD := yes

else ifeq ($(PLATFORM_TARGET),native)
    # -------------------------------------------------------------------------
    # Native desktop build (macOS / Linux)
    # -------------------------------------------------------------------------
    CROSS_COMPILE :=
    TARGET_ARCH := $(shell uname -m)
    TARGET_TRIPLE :=
    TARGET_CFLAGS :=
    DISPLAY_BACKEND := sdl
    ENABLE_SSL := yes
    ENABLE_SDL := yes
    ENABLE_EVDEV := no
    BUILD_SUBDIR :=

else
    $(error Unknown PLATFORM_TARGET: $(PLATFORM_TARGET). Valid options: native, pi, pi32, x86, ad5m, ad5m-br, cc1, mips, k1, ad5x, k1-dynamic, k2, snapmaker-u1, yocto)
endif

# =============================================================================
# Cross-Compiler Configuration
# =============================================================================

ifneq ($(CROSS_COMPILE),)
    # Override compilers for cross-compilation
    CC := $(CROSS_COMPILE)gcc
    CXX := $(CROSS_COMPILE)g++
    # Use gcc-ar and gcc-ranlib for LTO compatibility (they load the LTO plugin)
    AR := $(CROSS_COMPILE)gcc-ar
    STRIP := $(CROSS_COMPILE)strip
    RANLIB := $(CROSS_COMPILE)gcc-ranlib
    LD := $(CROSS_COMPILE)ld

    # k1-dynamic: GCC 7.5 static toolchain doesn't ship liblto_plugin.so,
    # so gcc-ar/gcc-ranlib fail. Use plain ar/ranlib (LTO is disabled anyway).
    ifeq ($(PLATFORM_TARGET),k1-dynamic)
        AR := $(CROSS_COMPILE)ar
        RANLIB := $(CROSS_COMPILE)ranlib
    endif

    # Re-apply ccache wrapping: the CC/CXX assignments above clobber the
    # ccache prefix the main Makefile sets (~line 143), so without this
    # every cross-compile is a cold build. Observed symptom: CI ccache stats
    # showed 0 hits / 0 misses / 0 kB after ~60 minute Pi builds.
    ifneq ($(shell command -v ccache 2>/dev/null),)
        CC := ccache $(CC)
        CXX := ccache $(CXX)
    endif
endif

# Override build directories when BUILD_SUBDIR is set (cross-compilation or
# native platform targets like x86-both where CROSS_COMPILE is empty).
# When SANITIZE=address, suffix with -asan so instrumented builds don't
# clobber the regular build tree (different objects, different libhv.a).
ifneq ($(BUILD_SUBDIR),)
    ifeq ($(SANITIZE),address)
        BUILD_SUBDIR := $(BUILD_SUBDIR)-asan
    endif
    ifeq ($(SANITIZE),thread)
        BUILD_SUBDIR := $(BUILD_SUBDIR)-tsan
    endif
    BUILD_DIR := build/$(BUILD_SUBDIR)
    BIN_DIR := $(BUILD_DIR)/bin
    OBJ_DIR := $(BUILD_DIR)/obj
endif

# Print platform info when cross-compiling or targeting a specific platform
ifneq ($(PLATFORM_TARGET),native)
ifneq ($(PLATFORM_TARGET),)
    $(info )
    $(info ========================================)
    $(info Building for: $(PLATFORM_TARGET))
    $(info Architecture: $(TARGET_ARCH))
    $(info Compiler: $(CC))
    $(info Output: $(BUILD_DIR))
    $(info ========================================)
    $(info )
endif
endif

# =============================================================================
# Target-Specific Flags
# =============================================================================

ifdef TARGET_CFLAGS
    CFLAGS += $(TARGET_CFLAGS)
    CXXFLAGS += $(TARGET_CFLAGS)
    SUBMODULE_CFLAGS += $(TARGET_CFLAGS)
    SUBMODULE_CXXFLAGS += $(TARGET_CFLAGS)
endif

# =============================================================================
# HELIX_MAX_FONT_TIER - compile-time maximum font tier linked for this platform
# =============================================================================
# Derived from FONT_TIERS (set per-platform above). Used by theme_manager to
# distinguish expected-missing fonts (pruned by tier) from unexpected-missing
# fonts (build bug). Tier numbers: micro=0, tiny=1, small=2, medium=3, large=4,
# xlarge=5, xxlarge=6.
ifeq ($(FONT_TIERS),all)
    HELIX_MAX_FONT_TIER := 6
else ifneq ($(filter xxlarge,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 6
else ifneq ($(filter xlarge,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 5
else ifneq ($(filter large,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 4
else ifneq ($(filter medium,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 3
else ifneq ($(filter small,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 2
else ifneq ($(filter tiny,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 1
else ifneq ($(filter micro,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 0
else
    HELIX_MAX_FONT_TIER := 6
endif
CFLAGS += -DHELIX_MAX_FONT_TIER=$(HELIX_MAX_FONT_TIER)
CXXFLAGS += -DHELIX_MAX_FONT_TIER=$(HELIX_MAX_FONT_TIER)
SUBMODULE_CFLAGS += -DHELIX_MAX_FONT_TIER=$(HELIX_MAX_FONT_TIER)
SUBMODULE_CXXFLAGS += -DHELIX_MAX_FONT_TIER=$(HELIX_MAX_FONT_TIER)

# =============================================================================
# Size flags for the memory-constrained boards
# =============================================================================
# On these devices helix-screen's file-backed text competes for page cache with
# Klipper: measured on a CC1, Klipper takes 3675 major faults and Moonraker 5526
# while helix-screen takes 68, because helix-screen's working set is what drives
# the reclaim. A Klipper stalled on flash IO is a "Timer too close".
#
# -DNDEBUG   drops assert() and nlohmann's JSON_ASSERT. LVGL's asserts are
#            controlled separately by LV_USE_ASSERT_* and are unaffected.
# -fno-rtti  the codebase is already RTTI-free by policy and lint-enforced
#            (tests/shell/test_code_lint.bats); a grep for non-comment
#            typeid/dynamic_cast across src/ and include/ returns zero. The
#            ESP32 firmware already builds this way via ESP-IDF's
#            CONFIG_COMPILER_CXX_RTTI. Applied to our C++ only, NOT to
#            SUBMODULE_CXXFLAGS -- libhv throws, and its catch clauses want
#            typeinfo for the thrown types.
ifeq ($(PLATFORM_TARGET),cc1)
    CFLAGS += -DNDEBUG
    CXXFLAGS += -DNDEBUG -fno-rtti
    SUBMODULE_CFLAGS += -DNDEBUG
    SUBMODULE_CXXFLAGS += -DNDEBUG
endif

# For size-optimized targets, override -O2 with -Os
# (GCC uses last optimization flag, but this makes it explicit)
ifeq ($(PLATFORM_TARGET),ad5m)
    CFLAGS := $(subst -O2,-Os,$(CFLAGS))
    CXXFLAGS := $(subst -O2,-Os,$(CXXFLAGS))
    SUBMODULE_CFLAGS := $(subst -O2,-Os,$(SUBMODULE_CFLAGS))
    SUBMODULE_CXXFLAGS := $(subst -O2,-Os,$(SUBMODULE_CXXFLAGS))
endif

ifeq ($(PLATFORM_TARGET),cc1)
    CFLAGS := $(subst -O2,-Os,$(CFLAGS))
    CXXFLAGS := $(subst -O2,-Os,$(CXXFLAGS))
    SUBMODULE_CFLAGS := $(subst -O2,-Os,$(SUBMODULE_CFLAGS))
    SUBMODULE_CXXFLAGS := $(subst -O2,-Os,$(SUBMODULE_CXXFLAGS))
endif

ifeq ($(PLATFORM_TARGET),k1)
    CFLAGS := $(subst -O2,-Os,$(CFLAGS))
    CXXFLAGS := $(subst -O2,-Os,$(CXXFLAGS))
    SUBMODULE_CFLAGS := $(subst -O2,-Os,$(SUBMODULE_CFLAGS))
    SUBMODULE_CXXFLAGS := $(subst -O2,-Os,$(SUBMODULE_CXXFLAGS))
endif

ifeq ($(PLATFORM_TARGET),k1-dynamic)
    CFLAGS := $(subst -O2,-Os,$(CFLAGS))
    CXXFLAGS := $(subst -O2,-Os,$(CXXFLAGS))
    SUBMODULE_CFLAGS := $(subst -O2,-Os,$(SUBMODULE_CFLAGS))
    SUBMODULE_CXXFLAGS := $(subst -O2,-Os,$(SUBMODULE_CXXFLAGS))
endif

ifeq ($(PLATFORM_TARGET),snapmaker-u1)
    CFLAGS := $(subst -O2,-Os,$(CFLAGS))
    CXXFLAGS := $(subst -O2,-Os,$(CXXFLAGS))
    SUBMODULE_CFLAGS := $(subst -O2,-Os,$(SUBMODULE_CFLAGS))
    SUBMODULE_CXXFLAGS := $(subst -O2,-Os,$(SUBMODULE_CXXFLAGS))
endif

ifdef TARGET_LDFLAGS
    LDFLAGS += $(TARGET_LDFLAGS)
endif

# Apply AddressSanitizer flags AFTER target/submodule flag merge so the main
# binary AND in-tree subprojects (LVGL, helix-xml, ThorVG) all get
# instrumented. For cross-compile, statically link libasan so the deployed
# binary is self-contained and the device doesn't need libasan installed.
# Strip is disabled because ASAN reports rely on symbol info in the binary.
ifeq ($(SANITIZE),address)
    CFLAGS += $(SANITIZE_FLAGS)
    CXXFLAGS += $(SANITIZE_FLAGS)
    SUBMODULE_CFLAGS += $(SANITIZE_FLAGS)
    SUBMODULE_CXXFLAGS += $(SANITIZE_FLAGS)
    LDFLAGS += $(SANITIZE_FLAGS)
    ifneq ($(CROSS_COMPILE),)
        LDFLAGS += -static-libasan
    endif
    STRIP_BINARY := no
endif

# Same treatment for ThreadSanitizer. Instrumenting the submodules matters here
# as much as it does for ASAN: a race is only reported when TSAN sees BOTH
# accesses, so an uninstrumented LVGL or libhv turns a real race into silence.
ifeq ($(SANITIZE),thread)
    CFLAGS += $(SANITIZE_FLAGS)
    CXXFLAGS += $(SANITIZE_FLAGS)
    SUBMODULE_CFLAGS += $(SANITIZE_FLAGS)
    SUBMODULE_CXXFLAGS += $(SANITIZE_FLAGS)
    LDFLAGS += $(SANITIZE_FLAGS)
    ifneq ($(CROSS_COMPILE),)
        LDFLAGS += -static-libtsan
    endif
    STRIP_BINARY := no
endif

# Display backend defines (used by display_backend.cpp and lv_conf.h for conditional compilation)
# Must be added to SUBMODULE_*FLAGS as well for LVGL driver compilation
ifeq ($(DISPLAY_BACKEND),drm)
    CFLAGS += -DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV
    CXXFLAGS += -DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV
    SUBMODULE_CFLAGS += -DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV
    SUBMODULE_CXXFLAGS += -DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV
    # GPU-accelerated rendering via EGL/OpenGL ES (Pi targets)
    ifeq ($(ENABLE_OPENGLES),yes)
        CFLAGS += -DHELIX_ENABLE_OPENGLES
        CXXFLAGS += -DHELIX_ENABLE_OPENGLES
        SUBMODULE_CFLAGS += -DHELIX_ENABLE_OPENGLES
        SUBMODULE_CXXFLAGS += -DHELIX_ENABLE_OPENGLES
    endif
    # DRM backend linker flags are added in Makefile's cross-compile section
else ifeq ($(DISPLAY_BACKEND),fbdev)
    CFLAGS += -DHELIX_DISPLAY_FBDEV
    CXXFLAGS += -DHELIX_DISPLAY_FBDEV
    SUBMODULE_CFLAGS += -DHELIX_DISPLAY_FBDEV
    SUBMODULE_CXXFLAGS += -DHELIX_DISPLAY_FBDEV
else ifeq ($(DISPLAY_BACKEND),sdl)
    CFLAGS += -DHELIX_DISPLAY_SDL
    CXXFLAGS += -DHELIX_DISPLAY_SDL
    SUBMODULE_CFLAGS += -DHELIX_DISPLAY_SDL
    SUBMODULE_CXXFLAGS += -DHELIX_DISPLAY_SDL
endif

# Evdev input support
ifeq ($(ENABLE_EVDEV),yes)
    CFLAGS += -DHELIX_INPUT_EVDEV
    CXXFLAGS += -DHELIX_INPUT_EVDEV
    SUBMODULE_CFLAGS += -DHELIX_INPUT_EVDEV
    SUBMODULE_CXXFLAGS += -DHELIX_INPUT_EVDEV
endif

# NOTE: LV_COLOR_DEPTH is platform-conditional in lv_conf.h.
# Constrained FBDEV devices (AD5M, CC1, K1/MIPS, AD5X, K2, U1) use 16 (RGB565).
# Pi (DRM) and desktop (SDL) use 32 (ARGB8888).

# =============================================================================
# Cross-Compilation Build Targets
# =============================================================================

.PHONY: pi pi-both pi32 pi32-both ad5m ad5m-br cc1 mips k1 ad5x k1-dynamic k2 snapmaker-u1 x86 x86-both pi-docker pi32-docker ad5m-docker cc1-docker mips-docker k1-docker ad5x-docker k1-dynamic-docker k2-docker ustreamer-k2 snapmaker-u1-docker x86-docker x86-fbdev-docker x86-all-docker docker-toolchains docker-toolchain-snapmaker-u1 docker-toolchain-x86 cross-info ensure-docker ensure-buildx maybe-stop-colima

# Persistent ccache for Docker builds — bind-mounts a host directory so the
# cache survives across container runs (the container is --rm).  Per-platform
# subdirectories avoid cross-toolchain collisions.  The host dir is created
# on first use with the current user's ownership (no root permission issues).
DOCKER_CCACHE_BASE ?= $(HOME)/.cache/helixscreen-ccache
docker-ccache-args = -v "$(DOCKER_CCACHE_BASE)/$(1)":/ccache -e CCACHE_DIR=/ccache
ensure-ccache-dir = @mkdir -p "$(DOCKER_CCACHE_BASE)/$(1)"

# Worktree cross-build support.
# scripts/setup-worktree.sh symlinks lib/<submodule> to the main checkout (absolute
# paths) so a worktree builds fast without duplicating ~GB of submodules. But Docker
# only bind-mounts $(PWD) at /src, so those absolute symlinks dangle inside the
# container — the dependency check reports "LVGL not found" and relative LVGL includes
# resolve to the wrong tree. When building from a worktree, also bind-mount the real
# submodule tree at its own absolute path so every lib/* symlink resolves identically
# inside and outside the container. Empty for a normal checkout, where lib/ already
# lives under $(PWD). Detection: realpath of lib/lvgl is outside $(CURDIR).
WORKTREE_LIB_REAL := $(patsubst %/,%,$(dir $(realpath lib/lvgl)))
ifeq ($(findstring $(CURDIR)/,$(WORKTREE_LIB_REAL)/),)
DOCKER_WORKTREE_MOUNT := $(if $(WORKTREE_LIB_REAL),-v "$(WORKTREE_LIB_REAL)":"$(WORKTREE_LIB_REAL)")
else
DOCKER_WORKTREE_MOUNT :=
endif

# Build provenance. The same mount boundary hides the git metadata: a worktree's
# .git is a FILE reading "gitdir: $(MAIN)/.git/worktrees/<name>", and that path
# is not under $(PWD), so git cannot resolve HEAD inside the container.
# scripts/gen-git-hash.sh fell back to "unknown" and the deployed binary could
# not say which commit produced it — silently, because the build still succeeds.
# Resolve it on the host, where git always works, and hand it to the container.
# Recursive '=' so a native build never pays for the git call.
# $(or) short-circuits, so an inherited HELIX_GIT_HASH (mk/remote.mk sets it over
# ssh; a release pipeline can too) skips the git call entirely — which matters
# because the build host may have no .git at all.
HELIX_GIT_HASH_HOST = $(or $(HELIX_GIT_HASH),$(shell git -c safe.directory='*' rev-parse --short HEAD 2>/dev/null))
DOCKER_GIT_HASH_ENV = $(if $(HELIX_GIT_HASH_HOST),-e HELIX_GIT_HASH=$(HELIX_GIT_HASH_HOST))

# Everything a container needs from the host beyond -v "$(PWD)":/src. Every
# `docker run` below must pass this — tests/shell/test_build_provenance.bats
# fails the build if one does not, so a new platform target cannot regress
# provenance or worktree support by omission.
DOCKER_HOST_CONTEXT = $(DOCKER_WORKTREE_MOUNT) $(DOCKER_GIT_HASH_ENV)

# Direct cross-compilation (requires toolchain installed)
pi:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi (aarch64)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=pi -j$(NPROC) all

pi32:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi 32-bit (armv7-a)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=pi32 -j$(NPROC) all

ad5m:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Adventurer 5M (armv7-a)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=ad5m -j$(NPROC) all

ad5x:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Adventurer 5X (mips)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=ad5x -j$(NPROC) all

cc1:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Centauri Carbon 1 (armv7-a)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=cc1 -j$(NPROC) all

mips:
	@echo "$(CYAN)$(BOLD)Cross-compiling for MIPS32 devices K1...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=mips -j$(NPROC) all

k1: mips
k1-dynamic:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Creality K1 series (MIPS32, dynamic linking)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=k1-dynamic -j$(NPROC) all

k2:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Creality K2 series (ARM Cortex-A7)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=k2 -j$(NPROC) all

snapmaker-u1:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Snapmaker U1 (aarch64)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=snapmaker-u1 -j$(NPROC) all

# Docker-based cross-compilation (recommended)
# SKIP_OPTIONAL_DEPS=1 skips npm, clang-format, python venv, and other development tools

# Helper target to ensure Docker daemon is running
# On macOS with Colima, automatically starts it with resources based on host hardware
# Allocates ~50% of system RAM (min 6GB, max 16GB) and ~50% of CPUs (min 2, max 8)
# Handles zombie Colima state (VM running but Docker socket dead) with automatic restart
.PHONY: ensure-docker
ensure-docker:
	@if docker info >/dev/null 2>&1; then \
		exit 0; \
	fi; \
	if [ "$(UNAME_S)" = "Darwin" ]; then \
		if command -v colima >/dev/null 2>&1; then \
			TOTAL_RAM_GB=$$(( $$(sysctl -n hw.memsize) / 1073741824 )); \
			TOTAL_CPUS=$$(sysctl -n hw.ncpu); \
			COLIMA_RAM=$$(( TOTAL_RAM_GB / 2 )); \
			COLIMA_CPUS=$$(( TOTAL_CPUS / 2 )); \
			[ $$COLIMA_RAM -lt 6 ] && COLIMA_RAM=6; \
			[ $$COLIMA_RAM -gt 16 ] && COLIMA_RAM=16; \
			[ $$COLIMA_CPUS -lt 2 ] && COLIMA_CPUS=2; \
			[ $$COLIMA_CPUS -gt 8 ] && COLIMA_CPUS=8; \
			echo "$(YELLOW)Docker not running. Starting Colima ($${COLIMA_RAM}GB RAM, $${COLIMA_CPUS} CPUs)...$(RESET)"; \
			echo "$(CYAN)  (Based on host: $${TOTAL_RAM_GB}GB RAM, $${TOTAL_CPUS} CPUs)$(RESET)"; \
			if colima list 2>/dev/null | grep -q "default"; then \
				CURRENT_RAM=$$(colima list 2>/dev/null | awk '/default/ {gsub(/GiB/,""); print $$5}'); \
				if [ "$$CURRENT_RAM" != "" ] && [ "$$CURRENT_RAM" -lt "$$COLIMA_RAM" ]; then \
					echo "$(YELLOW)⚠ Existing Colima VM has $${CURRENT_RAM}GB RAM (need $${COLIMA_RAM}GB)$(RESET)"; \
					echo "$(YELLOW)  Run 'colima delete' then retry to resize$(RESET)"; \
				fi; \
			fi; \
			colima start --memory $$COLIMA_RAM --cpu $$COLIMA_CPUS && echo "$(GREEN)✓ Colima started$(RESET)"; \
			if docker info >/dev/null 2>&1; then \
				exit 0; \
			fi; \
			echo "$(YELLOW)Docker socket not responding after start. Restarting Colima...$(RESET)"; \
			colima stop 2>/dev/null || true; \
			sleep 2; \
			colima start --memory $$COLIMA_RAM --cpu $$COLIMA_CPUS; \
			if docker info >/dev/null 2>&1; then \
				echo "$(GREEN)✓ Colima restarted successfully$(RESET)"; \
				exit 0; \
			fi; \
			echo "$(RED)Docker still not responding after Colima restart.$(RESET)"; \
			echo "$(YELLOW)Try manually: colima delete && colima start$(RESET)"; \
			exit 1; \
		elif [ -e "/Applications/Docker.app" ]; then \
			echo "$(RED)Docker Desktop is installed but not running.$(RESET)"; \
			echo "$(YELLOW)Please start Docker Desktop from your Applications folder.$(RESET)"; \
			exit 1; \
		else \
			echo "$(RED)Docker is not installed.$(RESET)"; \
			echo "$(YELLOW)Install with: brew install colima docker docker-buildx && colima start$(RESET)"; \
			exit 1; \
		fi; \
	else \
		echo "$(RED)Docker daemon is not running.$(RESET)"; \
		echo "$(YELLOW)Start it with: sudo systemctl start docker$(RESET)"; \
		exit 1; \
	fi

# Helper target to ensure Docker BuildKit/buildx is available
# BuildKit is Docker's modern image builder with better caching and parallel builds
# The legacy builder is deprecated and will be removed in a future Docker release
.PHONY: ensure-buildx
ensure-buildx: ensure-docker
	@if docker buildx version >/dev/null 2>&1; then \
		exit 0; \
	fi; \
	echo "$(YELLOW)Docker BuildKit (buildx) not found.$(RESET)"; \
	echo "The legacy Docker builder is deprecated and will be removed."; \
	if [ "$(UNAME_S)" = "Darwin" ]; then \
		echo "$(YELLOW)Install with: brew install docker-buildx$(RESET)"; \
	else \
		echo "$(YELLOW)See: https://docs.docker.com/go/buildx/$(RESET)"; \
	fi; \
	exit 1

pi-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

# AddressSanitizer build for the Pi (DRM). Output lands in build/pi-asan/.
# Uses -static-libasan (via mk/cross.mk SANITIZE block) so the binary is
# self-contained — no libasan install needed on the device. For sharper stack
# traces, override OPT: `make pi-asan-docker OPT=1`.
pi-asan-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi with AddressSanitizer via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi-asan)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi-asan) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi SANITIZE=address SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi-fbdev-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi fbdev fallback via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi-fbdev)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi-fbdev) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi-fbdev SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi-all-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi (DRM + fbdev) via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi-both SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi 32-bit via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi32) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32 SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

# AddressSanitizer build for the Pi 32-bit (DRM). Output lands in build/pi32-asan/.
# Uses -static-libasan (via mk/cross.mk SANITIZE block) so the binary is
# self-contained — no libasan install needed on the device. For sharper stack
# traces, override OPT: `make pi32-asan-docker OPT=1`.
pi32-asan-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi 32-bit with AddressSanitizer via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32-asan)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi32-asan) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32 SANITIZE=address SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-fbdev-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi 32-bit fbdev fallback via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32-fbdev)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi32-fbdev) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32-fbdev SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-all-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi 32-bit (DRM + fbdev) via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi32) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32-both SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

ad5m-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Adventurer 5M via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-ad5m >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-ad5m; \
	fi
	$(call ensure-ccache-dir,ad5m)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,ad5m) helixscreen/toolchain-ad5m \
		make PLATFORM_TARGET=ad5m SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@# Extract CA certificates from Docker image for HTTPS verification on device
	@mkdir -p build/ad5m/certs
	@docker run --rm helixscreen/toolchain-ad5m cat /etc/ssl/certs/ca-certificates.crt > build/ad5m/certs/ca-certificates.crt 2>/dev/null \
		&& echo "$(GREEN)✓ CA certificates extracted$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not extract CA certificates (HTTPS may rely on device certs)$(RESET)"
	@$(MAKE) --no-print-directory maybe-stop-colima

ad5x-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Adventurer 5X via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-ad5x >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-ad5x; \
	fi
	$(call ensure-ccache-dir,ad5x)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,ad5x) helixscreen/toolchain-ad5x \
		make PLATFORM_TARGET=ad5x SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@# Extract CA certificates from Docker image for HTTPS verification on device
	@mkdir -p build/ad5x/certs
	@docker run --rm helixscreen/toolchain-ad5x cat /etc/ssl/certs/ca-certificates.crt > build/ad5x/certs/ca-certificates.crt 2>/dev/null \
		&& echo "$(GREEN)✓ CA certificates extracted$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not extract CA certificates (HTTPS may rely on device certs)$(RESET)"
	@$(MAKE) --no-print-directory maybe-stop-colima

cc1-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Centauri Carbon 1 via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-cc1 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-cc1; \
	fi
	@# Generate translations on the host FIRST (docker image lacks python/yaml).
	@# Pass PLATFORM_TARGET=cc1 so cross.mk sets HELIX_LANG for the generator.
	@$(MAKE) --no-print-directory PLATFORM_TARGET=cc1 $(TRANS_XML)
	$(call ensure-ccache-dir,cc1)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,cc1) helixscreen/toolchain-cc1 \
		make PLATFORM_TARGET=cc1 SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@# Extract CA certificates from Docker image for HTTPS verification on device
	@mkdir -p build/cc1/certs
	@docker run --rm helixscreen/toolchain-cc1 cat /etc/ssl/certs/ca-certificates.crt > build/cc1/certs/ca-certificates.crt 2>/dev/null \
		&& echo "$(GREEN)✓ CA certificates extracted$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not extract CA certificates (HTTPS may rely on device certs)$(RESET)"
	@# Restore the full-language translation packs so the committed XML in
	@# ui_xml/translations/ doesn't drift from git HEAD after a CC1 build. The
	@# stamp file detects that HELIX_LANG is now empty and regenerates all locales.
	@$(MAKE) --no-print-directory $(TRANS_XML) >/dev/null 2>&1 \
		&& echo "$(DIM)✓ Restored full translation packs (repo clean)$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not restore full translation packs — run 'make translations'$(RESET)"
	@$(MAKE) --no-print-directory maybe-stop-colima

mips-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for MIPS32 devices via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-k1 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-k1; \
	fi
	$(call ensure-ccache-dir,k1)
	# Do not inherit host jobserver flags into containerized make.
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -e MAKEFLAGS= -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,k1) helixscreen/toolchain-k1 \
		make PLATFORM_TARGET=mips SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@# Extract CA certificates from Docker image for HTTPS verification on device
	@mkdir -p build/mips/certs
	@docker run --rm helixscreen/toolchain-k1 cat /etc/ssl/certs/ca-certificates.crt > build/mips/certs/ca-certificates.crt 2>/dev/null \
		&& echo "$(GREEN)✓ CA certificates extracted$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not extract CA certificates (HTTPS may rely on device certs)$(RESET)"
	@$(MAKE) --no-print-directory maybe-stop-colima

k1-docker: mips-docker


k1-dynamic-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Creality K1 series (dynamic) via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-k1-dynamic >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-k1-dynamic; \
	fi
	$(call ensure-ccache-dir,k1-dynamic)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,k1-dynamic) helixscreen/toolchain-k1-dynamic \
		make PLATFORM_TARGET=k1-dynamic SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

k2-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Creality K2 series via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-k2 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-k2; \
	fi
	$(call ensure-ccache-dir,k2)
	$(Q)scripts/cross-compile-lock.sh docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,k2) helixscreen/toolchain-k2 \
		make PLATFORM_TARGET=k2 SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

# Cross-build the static armv7 ustreamer (MJPEG camera server) for K2.
# Runs the vendored build script inside the K2 toolchain image. Idempotent:
# scripts/ustreamer/build-ustreamer-k2.sh skips the build if a valid static
# ARM binary already sits at build/k2/bin/ustreamer (FORCE_REBUILD=1 to force).
# Source tarballs are cached under build/.cache/ustreamer; checksums verified.
# Output lands at build/k2/bin/ustreamer so release-k2 picks it up.
ustreamer-k2: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling static ustreamer for Creality K2 via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-k2 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-k2; \
	fi
	$(Q)scripts/cross-compile-lock.sh docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src -e FORCE_REBUILD="$(FORCE_REBUILD)" helixscreen/toolchain-k2 \
		bash scripts/ustreamer/build-ustreamer-k2.sh
	@$(MAKE) --no-print-directory maybe-stop-colima

snapmaker-u1-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Snapmaker U1 via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-snapmaker-u1 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-snapmaker-u1; \
	fi
	$(call ensure-ccache-dir,snapmaker-u1)
	$(Q)scripts/cross-compile-lock.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,snapmaker-u1) helixscreen/toolchain-snapmaker-u1 \
		make PLATFORM_TARGET=snapmaker-u1 SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@# Extract CA certificates from Docker image for HTTPS verification on device
	@mkdir -p build/snapmaker-u1/certs
	@docker run --rm helixscreen/toolchain-snapmaker-u1 cat /etc/ssl/certs/ca-certificates.crt > build/snapmaker-u1/certs/ca-certificates.crt 2>/dev/null \
		&& echo "$(GREEN)✓ CA certificates extracted$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not extract CA certificates (HTTPS may rely on device certs)$(RESET)"
	@$(MAKE) --no-print-directory maybe-stop-colima

x86-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Building for x86_64 Debian via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-x86 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-x86; \
	fi
	$(call ensure-ccache-dir,x86)
	$(Q)scripts/cross-compile-lock.sh docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,x86) helixscreen/toolchain-x86 \
		make PLATFORM_TARGET=x86 SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

x86-fbdev-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Building x86_64 fbdev fallback via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-x86 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-x86; \
	fi
	$(call ensure-ccache-dir,x86-fbdev)
	$(Q)scripts/cross-compile-lock.sh docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,x86-fbdev) helixscreen/toolchain-x86 \
		make PLATFORM_TARGET=x86-fbdev SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

x86-all-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Building x86_64 (DRM + fbdev) via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-x86 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-x86; \
	fi
	$(call ensure-ccache-dir,x86)
	$(Q)scripts/cross-compile-lock.sh docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,x86) helixscreen/toolchain-x86 \
		make PLATFORM_TARGET=x86-both SKIP_OPTIONAL_DEPS=1 $(if $(ENABLE_REMOTE_CONTROL),ENABLE_REMOTE_CONTROL=$(ENABLE_REMOTE_CONTROL)) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

# Stop Colima after build to free up RAM (macOS only)
# Only stops if Colima is running and we're on macOS
.PHONY: maybe-stop-colima
maybe-stop-colima:
	@if [ "$(UNAME_S)" = "Darwin" ] && command -v colima >/dev/null 2>&1; then \
		if colima status >/dev/null 2>&1; then \
			echo "$(CYAN)Stopping Colima to free up RAM...$(RESET)"; \
			colima stop && echo "$(GREEN)✓ Colima stopped$(RESET)"; \
		fi; \
	fi

# Build Docker toolchain images
docker-toolchains: docker-toolchain-pi docker-toolchain-pi32 docker-toolchain-ad5m docker-toolchain-ad5x docker-toolchain-cc1 docker-toolchain-k1 docker-toolchain-k1-dynamic docker-toolchain-k2 docker-toolchain-snapmaker-u1 docker-toolchain-x86
	@echo "$(GREEN)$(BOLD)All Docker toolchains built successfully$(RESET)"

docker-toolchain-pi: ensure-buildx
	@echo "$(CYAN)Building Raspberry Pi toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-pi -f docker/Dockerfile.pi docker/

docker-toolchain-pi32: ensure-buildx
	@echo "$(CYAN)Building Raspberry Pi 32-bit toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-pi32 -f docker/Dockerfile.pi32 docker/

docker-toolchain-ad5m: ensure-buildx
	@echo "$(CYAN)Building Adventurer 5M toolchain Docker image...$(RESET)"
	$(Q)docker buildx build --platform linux/amd64 -t helixscreen/toolchain-ad5m -f docker/Dockerfile.ad5m docker/

docker-toolchain-ad5x: ensure-buildx
	@echo "$(CYAN)Building Adventurer 5X toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-ad5x -f docker/Dockerfile.ad5x docker/

docker-toolchain-cc1: ensure-buildx
	@echo "$(CYAN)Building Centauri Carbon 1 toolchain Docker image...$(RESET)"
	$(Q)docker buildx build --platform linux/amd64 -t helixscreen/toolchain-cc1 -f docker/Dockerfile.cc1 docker/

docker-toolchain-k1: ensure-buildx
	@echo "$(CYAN)Building MIPS32 K1 toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-k1 -f docker/Dockerfile.k1 docker/

docker-toolchain-k1-dynamic: ensure-buildx
	@echo "$(CYAN)Building Creality K1 series (dynamic) toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-k1-dynamic -f docker/Dockerfile.k1-dynamic docker/

docker-toolchain-k2: ensure-buildx
	@echo "$(CYAN)Building Creality K2 series toolchain Docker image...$(RESET)"
	$(Q)docker buildx build --platform linux/amd64 -t helixscreen/toolchain-k2 -f docker/Dockerfile.k2 docker/

docker-toolchain-snapmaker-u1: ensure-buildx
	@echo "$(CYAN)Building Snapmaker U1 toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-snapmaker-u1 -f docker/Dockerfile.snapmaker-u1 docker/

docker-toolchain-x86: ensure-buildx
	@echo "$(CYAN)Building x86_64 Debian toolchain Docker image...$(RESET)"
	$(Q)docker buildx build --platform linux/amd64 -t helixscreen/toolchain-x86 -f docker/Dockerfile.x86 docker/

# Display cross-compilation info (alias for help-cross)
cross-info: help-cross

# Cross-compilation help
.PHONY: help-cross
help-cross:
	@if [ -t 1 ] && [ -n "$(TERM)" ] && [ "$(TERM)" != "dumb" ]; then \
		B='$(BOLD)'; G='$(GREEN)'; Y='$(YELLOW)'; C='$(CYAN)'; X='$(RESET)'; \
	else \
		B=''; G=''; Y=''; C=''; X=''; \
	fi; \
	echo "$${B}Cross-Compilation & Deployment$${X}"; \
	echo ""; \
	echo "$${C}Docker Cross-Compilation (recommended):$${X}"; \
	echo "  $${G}pi-docker$${X}            - Build for Raspberry Pi (aarch64) via Docker"; \
	echo "  $${G}pi32-docker$${X}          - Build for Raspberry Pi 32-bit (armhf) via Docker"; \
	echo "  $${G}ad5m-docker$${X}          - Build for Adventurer 5M (armv7-a) via Docker"; \
	echo "  $${G}cc1-docker$${X}           - Build for Centauri Carbon 1 (armv7-a) via Docker"; \
	echo "  $${G}k1-docker$${X}            - Build for Creality K1 series (MIPS32, static) via Docker"; \
	echo "  $${G}k1-dynamic-docker$${X}    - Build for Creality K1 series (MIPS32, dynamic) via Docker"; \
	echo "  $${G}k2-docker$${X}            - Build for Creality K2 series (ARM, static) via Docker"; \
	echo "  $${G}snapmaker-u1-docker$${X}  - Build for Snapmaker U1 (aarch64, static) via Docker"; \
	echo "  $${G}x86-docker$${X}           - Build for x86_64 Debian SBCs (DRM+GLES) via Docker"; \
	echo "  $${G}docker-toolchains$${X}    - Build all Docker toolchain images"; \
	echo "  $${G}docker-toolchain-pi$${X}  - Build Pi toolchain image only"; \
	echo "  $${G}docker-toolchain-pi32$${X} - Build Pi 32-bit toolchain image only"; \
	echo "  $${G}docker-toolchain-ad5m$${X} - Build AD5M toolchain image only"; \
	echo "  $${G}docker-toolchain-cc1$${X}  - Build CC1 toolchain image only"; \
	echo "  $${G}docker-toolchain-k1$${X}  - Build K1 static toolchain image only"; \
	echo "  $${G}docker-toolchain-k1-dynamic$${X} - Build K1 dynamic toolchain image only"; \
	echo "  $${G}docker-toolchain-k2$${X}  - Build K2 toolchain image only"; \
	echo "  $${G}docker-toolchain-snapmaker-u1$${X} - Build Snapmaker U1 toolchain image only"; \
	echo "  $${G}docker-toolchain-x86$${X}  - Build x86_64 Debian toolchain image only"; \
	echo ""; \
	echo "$${C}Direct Cross-Compilation (requires local toolchain):$${X}"; \
	echo "  $${G}pi$${X}                   - Cross-compile for Raspberry Pi (64-bit)"; \
	echo "  $${G}pi32$${X}                 - Cross-compile for Raspberry Pi (32-bit)"; \
	echo "  $${G}ad5m$${X}                 - Cross-compile for Adventurer 5M"; \
	echo "  $${G}cc1$${X}                  - Cross-compile for Centauri Carbon 1"; \
	echo "  $${G}k1$${X}                   - Cross-compile for Creality K1 series (static)"; \
	echo "  $${G}k1-dynamic$${X}           - Cross-compile for Creality K1 series (dynamic)"; \
	echo "  $${G}k2$${X}                   - Cross-compile for Creality K2 series"; \
	echo "  $${G}snapmaker-u1$${X}         - Cross-compile for Snapmaker U1 (aarch64)"; \
	echo ""; \
	echo "$${C}Pi Deployment (64-bit):$${X}"; \
	echo "  $${G}deploy-pi$${X}            - Deploy and restart in background (default)"; \
	echo "  $${G}deploy-pi-fg$${X}         - Deploy and run in foreground (debug)"; \
	echo "  $${G}pi-test$${X}              - Full cycle: build + deploy + run (fg)"; \
	echo "  $${G}pi-ssh$${X}               - SSH into the Pi"; \
	echo ""; \
	echo "$${C}Pi Deployment (32-bit):$${X}"; \
	echo "  $${G}deploy-pi32$${X}          - Deploy and restart in background"; \
	echo "  $${G}deploy-pi32-fg$${X}       - Deploy and run in foreground (debug)"; \
	echo "  $${G}deploy-pi32-bin$${X}      - Deploy binaries only (fast iteration)"; \
	echo "  $${G}pi32-test$${X}            - Full cycle: build + deploy + run (fg)"; \
	echo ""; \
	echo "$${C}AD5M Deployment:$${X}"; \
	echo "  $${G}deploy-ad5m$${X}          - Deploy and restart in background"; \
	echo "  $${G}deploy-ad5m-fg$${X}       - Deploy and run in foreground (debug)"; \
	echo "  $${G}deploy-ad5m-bin$${X}      - Deploy binaries only (fast iteration)"; \
	echo "  $${G}ad5m-test$${X}            - Full cycle: remote build + deploy + run (fg)"; \
	echo "  $${G}ad5m-ssh$${X}             - SSH into the AD5M"; \
	echo ""; \
	echo "$${C}CC1 Deployment:$${X}"; \
	echo "  $${G}deploy-cc1$${X}           - Deploy and restart in background"; \
	echo "  $${G}deploy-cc1-fg$${X}        - Deploy and run in foreground (debug)"; \
	echo "  $${G}deploy-cc1-bin$${X}       - Deploy binaries only (fast iteration)"; \
	echo "  $${G}cc1-test$${X}             - Full cycle: docker build + deploy + run (fg)"; \
	echo "  $${G}cc1-ssh$${X}              - SSH into the CC1"; \
	echo ""; \
	echo "$${C}K1 Deployment - Static:$${X}"; \
	echo "  $${G}deploy-k1$${X}            - Deploy and restart in background"; \
	echo "  $${G}deploy-k1-fg$${X}         - Deploy and run in foreground (debug)"; \
	echo "  $${G}deploy-k1-bin$${X}        - Deploy binaries only (fast iteration)"; \
	echo "  $${G}k1-test$${X}              - Full cycle: docker build + deploy + run (fg)"; \
	echo "  $${G}k1-ssh$${X}               - SSH into the K1"; \
	echo ""; \
	echo "$${C}K1 Deployment - Dynamic:$${X}"; \
	echo "  $${G}deploy-k1-dynamic$${X}    - Deploy and restart in background"; \
	echo "  $${G}deploy-k1-dynamic-fg$${X} - Deploy and run in foreground (debug)"; \
	echo "  $${G}deploy-k1-dynamic-bin$${X} - Deploy binaries only (fast iteration)"; \
	echo "  $${G}k1-dynamic-test$${X}      - Full cycle: docker build + deploy + run (fg)"; \
	echo ""; \
	echo "$${C}K2 Deployment (UNTESTED):$${X}"; \
	echo "  $${G}deploy-k2$${X}            - Deploy and restart in background"; \
	echo "  $${G}deploy-k2-fg$${X}         - Deploy and run in foreground (debug)"; \
	echo "  $${G}deploy-k2-bin$${X}        - Deploy binaries only (fast iteration)"; \
	echo "  $${G}k2-test$${X}              - Full cycle: docker build + deploy + run (fg)"; \
	echo "  $${G}k2-ssh$${X}               - SSH into the K2"; \
	echo ""; \
	echo "$${C}Snapmaker U1 Deployment:$${X}"; \
	echo "  $${G}deploy-snapmaker-u1$${X}  - Deploy and restart in background"; \
	echo "  $${G}deploy-snapmaker-u1-fg$${X} - Deploy and run in foreground (debug)"; \
	echo "  $${G}deploy-snapmaker-u1-bin$${X} - Deploy binaries only (fast iteration)"; \
	echo "  $${G}snapmaker-u1-ssh$${X}     - SSH into the Snapmaker U1"; \
	echo ""; \
	echo "$${C}Deployment Options:$${X}"; \
	echo "  $${Y}PI_HOST$${X}=hostname     - Pi hostname (default: helixpi.local)"; \
	echo "  $${Y}PI_USER$${X}=user         - Pi username (default: from SSH config)"; \
	echo "  $${Y}PI_DEPLOY_DIR$${X}=path   - Deployment directory (default: ~/helixscreen)"; \
	echo "  $${Y}AD5M_HOST$${X}=hostname   - AD5M hostname/IP (default: ad5m.local)"; \
	echo "  $${Y}AD5M_USER$${X}=user       - AD5M username (default: root)"; \
	echo "  $${Y}AD5M_DEPLOY_DIR$${X}=path - AD5M deploy directory (default: /opt/helixscreen)"; \
	echo "  $${Y}CC1_HOST$${X}=hostname     - CC1 hostname/IP (default: cc1.local)"; \
	echo "  $${Y}CC1_USER$${X}=user         - CC1 username (default: root)"; \
	echo "  $${Y}CC1_DEPLOY_DIR$${X}=path   - CC1 deploy directory (default: /opt/helixscreen)"; \
	echo "  $${Y}K1_HOST$${X}=hostname     - K1 hostname/IP (default: k1.local)"; \
	echo "  $${Y}K1_USER$${X}=user         - K1 username (default: root)"; \
	echo "  $${Y}K1_DEPLOY_DIR$${X}=path   - K1 deploy directory (default: /usr/data/helixscreen)"; \
	echo "  $${Y}K2_HOST$${X}=hostname     - K2 hostname/IP (default: k2.local)"; \
	echo "  $${Y}K2_USER$${X}=user         - K2 username (default: root)"; \
	echo "  $${Y}K2_DEPLOY_DIR$${X}=path   - K2 deploy directory (default: /opt/helixscreen)"; \
	echo "  $${Y}SNAPMAKER_U1_HOST$${X}=hostname - Snapmaker U1 hostname/IP (default: snapmaker-u1.local)"; \
	echo "  $${Y}SNAPMAKER_U1_USER$${X}=user     - Snapmaker U1 username (default: root)"; \
	echo "  $${Y}SNAPMAKER_U1_DEPLOY_DIR$${X}=path - Snapmaker U1 deploy directory (default: /userdata/helixscreen)"; \
	echo ""; \
	echo "$${C}Current Configuration:$${X}"; \
	echo "  Platform target: $(PLATFORM_TARGET)"; \
	echo "  Display backend: $(DISPLAY_BACKEND)"; \
	echo "  SDL enabled: $(ENABLE_SDL)"

# =============================================================================
# Common Deployment Settings
# =============================================================================

# Rsync flags for asset sync: delete stale files, checksum-based skip, exclude junk
DEPLOY_RSYNC_FLAGS := -avzz --delete --checksum
# Runtime state files: generated at runtime (crash dumps, telemetry queues, tool
# mappings). Shipping these from a dev machine to a target overwrites the
# target's real state with dev-machine state — or worse, leaves a stale
# crash_report.txt on a healthy device that triggers the "previously crashed"
# modal on next boot. These are gitignored but gitignore doesn't affect rsync.
DEPLOY_RUNTIME_EXCLUDES := --exclude='crash.txt' --exclude='crash_*.txt' --exclude='crash_report.txt' --exclude='.crash_restart_count' --exclude='telemetry_*.json' --exclude='tool_spools.json' --exclude='filament_slot_overrides.json'
DEPLOY_ASSET_EXCLUDES := --exclude='test_gcodes' --exclude='gcode' --exclude='.DS_Store' --exclude='*.pyc' --exclude='settings*.json' --exclude='helixconfig*.json' --exclude='helixscreen.env' --exclude='.claude-recall' --exclude='._*' \
	--exclude='assets/fonts/*.c' --exclude='assets/fonts/*.ttf' --exclude='assets/fonts/*.otf' --exclude='assets/fonts/.clang-format' \
	--exclude='*.icns' --exclude='mdi-icon-metadata.json.gz' --exclude='moonraker-plugin/tests' \
	$(DEPLOY_RUNTIME_EXCLUDES)
# Zero the owner/group recorded in every archive we create.
#
# A bare `tar -czvf` stamps each entry with the build machine's numeric uid/gid
# (1001 for the GitHub Actions runner, 1000 for a local build host). Root
# extracts with --same-owner by default on both GNU and BusyBox tar, so the
# install lands owned by a uid that has no /etc/passwd entry on the printer.
#
# GNU tar and bsdtar spell this differently and neither accepts the other's
# form, and release packaging runs on both (Linux in CI, macOS locally) — so
# probe once here rather than guessing.
TAR_OWNER_FLAGS := $(shell if tar --owner=0 --group=0 -cf /dev/null -T /dev/null >/dev/null 2>&1; \
	then echo --owner=0 --group=0 --numeric-owner; \
	else echo --uid 0 --gid 0 --numeric-owner; fi)

# Tar-compatible excludes (same patterns, different syntax)
DEPLOY_TAR_EXCLUDES := --exclude='test_gcodes' --exclude='gcode' --exclude='.DS_Store' --exclude='*.pyc' --exclude='settings*.json' --exclude='helixconfig*.json' --exclude='helixscreen.env' --exclude='.claude-recall' --exclude='._*' \
	--exclude='assets/fonts/*.c' --exclude='assets/fonts/*.ttf' --exclude='assets/fonts/*.otf' --exclude='assets/fonts/.clang-format' \
	--exclude='*.icns' --exclude='mdi-icon-metadata.json.gz' --exclude='moonraker-plugin/tests' \
	$(DEPLOY_RUNTIME_EXCLUDES)
# Exclude tracker files (MOD/MED) on platforms without HELIX_HAS_TRACKER
# rsync syntax:
DEPLOY_NO_TRACKER := --exclude='*.mod' --exclude='*.med'
# tar syntax:
DEPLOY_TAR_NO_TRACKER := --exclude='*.mod' --exclude='*.med'
DEPLOY_ASSET_DIRS := ui_xml assets config moonraker-plugin

# Common deploy recipe (called with: $(call deploy-common,SSH_TARGET,DEPLOY_DIR,BIN_DIR))
# Usage: $(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi/bin)
# Turn on the runtime switches that match what the binary being deployed
# actually contains.
#
# ENABLE_REMOTE_CONTROL is a BUILD flag and defaults to no for every cross
# target, so a device binary only has the ctl server compiled in when a
# developer asked for it explicitly. Having it compiled in and left off is not a
# state anyone wants: the server still only listens under --remote, and the
# SysV/systemd units exec helix-launcher.sh with no arguments, so without
# HELIX_REMOTE_CONTROL=1 in the device's helixscreen.env the flag silently buys
# nothing and diagnosing the printer means walking to it. `make deploy-*` is a
# separate make invocation with no PLATFORM_TARGET, so it cannot re-derive the
# build's choice — $(BIN_DIR)/.build-features, written by the link rule in
# mk/rules.mk, carries it across.
#
# Opt back out with HELIX_REMOTE_CONTROL=0 in the device env file; the key is
# only rewritten when the value differs, so that choice sticks across redeploys.
#
#   $(1) ssh target   $(2) device deploy dir   $(3) host bin dir just deployed
define sync-device-features
	@if [ "$$(sed -n 's/^remote_control=//p' $(3)/.build-features 2>/dev/null)" = "yes" ]; then \
		echo "$(DIM)Binary has the remote-control server — enabling it on the device...$(RESET)"; \
		scripts/device-env-set.sh "$(1)" "$(2)/config/helixscreen.env" HELIX_REMOTE_CONTROL 1; \
		echo "$(DIM)  drive it: ssh $(1) 'cd $(2) && ./bin/helix-screen ctl navigate settings'$(RESET)"; \
	fi
endef

define deploy-common
	@echo "$(CYAN)Deploying HelixScreen to $(1):$(2)...$(RESET)"
	@# Generate pre-rendered splash images if missing (all small-display platforms use the same files)
	@if [ ! -f build/assets/images/prerendered/splash-logo-small.bin ]; then \
		echo "$(DIM)Generating pre-rendered splash images...$(RESET)"; \
		$(MAKE) gen-images; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ] || [ -z "$$(ls -A build/assets/images/printers/prerendered/*.bin 2>/dev/null)" ]; then \
		echo "$(DIM)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/benchy_thumbnail_white.bin ]; then \
		echo "$(DIM)Generating placeholder images...$(RESET)"; \
		$(MAKE) gen-placeholder-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/splash-3d-dark-small.bin ]; then \
		echo "$(DIM)Generating 3D splash images...$(RESET)"; \
		$(MAKE) gen-splash-3d; \
	fi
	@# Stop running processes and prepare directory
	@# Stop update watcher first (prevents PathChanged restart during file sync),
	@# then stop the main service and kill any stragglers
	ssh $(1) "sudo systemctl stop helixscreen-update.path 2>/dev/null; sudo systemctl stop helixscreen 2>/dev/null; systemctl --user stop helix-screen 2>/dev/null; killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 0.5; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null; while pidof helix-screen helix-splash helix-watchdog >/dev/null 2>&1; do sleep 0.2; done; true"
	ssh $(1) "mkdir -p $(2)/bin"
	ssh $(1) "rm -f $(2)/*.xml 2>/dev/null || true"
	@# Sync binaries and launcher to bin/
	rsync -avzz --progress $(3)/helix-screen $(3)/helix-splash $(1):$(2)/bin/
	@if [ -f $(3)/helix-watchdog ]; then rsync -avzz $(3)/helix-watchdog $(1):$(2)/bin/; fi
	@# Sync Bluetooth plugin if built (runtime-loaded via dlopen, same dir as binary)
	@BT_SO_DIR=$$(dirname $(3))"/lib/libhelix-bluetooth.so"; \
	if [ -f "$$BT_SO_DIR" ]; then \
		echo "$(DIM)Deploying Bluetooth plugin...$(RESET)"; \
		rsync -avzz "$$BT_SO_DIR" $(1):$(2)/bin/; \
	fi
	rsync -avzz scripts/helix-launcher.sh $(1):$(2)/bin/
	@# Sync installer script (needed for auto-updates)
	rsync -avzz scripts/$(INSTALLER_FILENAME) $(1):$(2)/
	@# Sync assets (--delete removes stale files)
	rsync $(DEPLOY_RSYNC_FLAGS) $(DEPLOY_ASSET_EXCLUDES) $(DEPLOY_ASSET_DIRS) $(1):$(2)/
	@# Sync pre-rendered images
	@if [ -d build/assets/images/prerendered ]; then \
		rsync $(DEPLOY_RSYNC_FLAGS) build/assets/images/prerendered/ $(1):$(2)/assets/images/prerendered/; \
	fi
	@if [ -d build/assets/images/printers/prerendered ]; then \
		rsync $(DEPLOY_RSYNC_FLAGS) build/assets/images/printers/prerendered/ $(1):$(2)/assets/images/printers/prerendered/; \
	fi
	@# Fix ownership - rsync preserves macOS uid:gid which prevents writes on target
	@echo "$(DIM)Fixing file ownership...$(RESET)"
	@ssh $(1) "if [ \$$(id -u) -ne 0 ]; then sudo chown -R \$$(id -u):\$$(id -g) $(2); else chown -R \$$(id -u):\$$(id -g) $(2)/config 2>/dev/null || true; fi"
	$(call sync-device-features,$(1),$(2),$(3))
endef

# =============================================================================
# Pi Deployment Configuration
# =============================================================================

# Pi deployment settings (can override via environment or command line)
# Example: make deploy-pi PI_HOST=192.168.1.50 PI_USER=pi
# PI_USER defaults to empty (uses SSH config or current user)
# PI_DEPLOY_DIR defaults to ~/helixscreen (full app directory)
PI_HOST ?= 192.168.1.113
PI_USER ?=
PI_DEPLOY_DIR ?= ~/helixscreen

# Build SSH target: user@host or just host if no user specified
ifdef PI_USER
    PI_SSH_TARGET := $(PI_USER)@$(PI_HOST)
else
    PI_SSH_TARGET := $(PI_HOST)
endif

# =============================================================================
# Pi Deployment Targets
# =============================================================================

.PHONY: deploy-pi deploy-pi-fg deploy-pi-quiet pi-ssh pi-test

# Deploy full application to Pi and restart in background
deploy-pi:
	@test -f build/pi/bin/helix-screen || { echo "$(RED)Error: build/pi/bin/helix-screen not found. Run 'make pi-docker' first.$(RESET)"; exit 1; }
	@test -f build/pi/bin/helix-splash || { echo "$(RED)Error: build/pi/bin/helix-splash not found. Run 'make pi-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi/bin)
	@echo "$(GREEN)✓ Deployed to $(PI_HOST):$(PI_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Restarting helix-screen on $(PI_HOST)...$(RESET)"
	@# Prefer systemd restart (picks up Environment= vars and keeps logs in journal).
	@# Use -n (non-interactive sudo) to fail fast rather than hang on a password prompt.
	@# Fallback uses ssh -f which forks into background AFTER authentication; without -f,
	@# ssh keeps the connection open as long as the remote child has stdout attached,
	@# even with </dev/null >/dev/null and a trailing &, and the make target hangs.
	@ssh $(PI_SSH_TARGET) "sudo -n systemctl start helixscreen" 2>/dev/null \
		|| ssh -f $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && setsid ./bin/helix-launcher.sh </dev/null >/dev/null 2>&1" \
		|| echo "$(YELLOW)⚠ Failed to restart helix-screen (set up passwordless sudo for systemctl or install the systemd unit)$(RESET)"
	@# Re-arm update watcher (stopped by deploy-common to prevent double restart)
	@ssh $(PI_SSH_TARGET) "sudo -n systemctl start helixscreen-update.path" 2>/dev/null || true
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"
	@echo "$(DIM)Logs: ssh $(PI_SSH_TARGET) 'sudo journalctl -u helixscreen -f'$(RESET)"

# Deploy and run in foreground with debug logging (for interactive debugging)
deploy-pi-fg:
	@test -f build/pi/bin/helix-screen || { echo "$(RED)Error: build/pi/bin/helix-screen not found. Run 'make pi-docker' first.$(RESET)"; exit 1; }
	@test -f build/pi/bin/helix-splash || { echo "$(RED)Error: build/pi/bin/helix-splash not found. Run 'make pi-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi/bin)
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) (foreground, debug mode)...$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug --log-dest=console"

# Deploy and run in foreground without debug logging (production mode)
deploy-pi-quiet:
	@test -f build/pi/bin/helix-screen || { echo "$(RED)Error: build/pi/bin/helix-screen not found. Run 'make pi-docker' first.$(RESET)"; exit 1; }
	@test -f build/pi/bin/helix-splash || { echo "$(RED)Error: build/pi/bin/helix-splash not found. Run 'make pi-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi/bin)
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) (foreground)...$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ./bin/helix-launcher.sh"

# Convenience: SSH into the Pi
pi-ssh:
	ssh $(PI_SSH_TARGET)

# Full cycle: build + deploy + run in foreground
pi-test: pi-docker deploy-pi-fg

# =============================================================================
# Pi AddressSanitizer Deployment Targets
# =============================================================================
# Deploys the SANITIZE=address build (build/pi-asan/bin/) and starts the
# binary with ASAN_OPTIONS configured for the wizard-transition investigation.
# Suppressions file is shipped to the device alongside the binary.

.PHONY: deploy-pi-asan deploy-pi-asan-fg pi-asan-test

# Pi-side ASAN runtime config. detect_leaks=0 because LSan trips on every
# normal LVGL image cache entry; abort_on_error=1 so the first UAF takes the
# process down with a coredump-style report; fast_unwind_on_malloc=0 makes
# stack traces reliable at the cost of speed (worth it for a stress run).
#
# asan.supp holds LeakSanitizer (`leak:`) rules, which belong in LSAN_OPTIONS,
# NOT ASAN_OPTIONS:suppressions= — feeding a leak-only file to ASAN's own
# suppression parser makes it abort at startup with "failed to parse
# suppressions" (hit while reproducing 783DVYKD, 2026-06-16). Keep them split.
# (LSAN_OPTIONS is only consulted when detect_leaks=1, but homing the file there
# keeps it correct if leak detection is re-enabled.)
PI_ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=1:fast_unwind_on_malloc=0:print_stacktrace=1:halt_on_error=1:strip_path_prefix=/src/
PI_LSAN_OPTIONS ?= suppressions=$(PI_DEPLOY_DIR)/asan.supp

deploy-pi-asan:
	@test -f build/pi-asan/bin/helix-screen || { echo "$(RED)Error: build/pi-asan/bin/helix-screen not found. Run 'make pi-asan-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi-asan/bin)
	@echo "$(CYAN)Shipping ASAN suppressions...$(RESET)"
	$(Q)rsync -avzz tests/asan.supp $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/
	@echo "$(GREEN)✓ Deployed ASAN build to $(PI_HOST):$(PI_DEPLOY_DIR)$(RESET)"
	@echo "$(YELLOW)Note: do NOT systemd-restart — ASAN binaries need ASAN_OPTIONS in env.$(RESET)"

# Run the ASAN binary in the foreground with ASAN_OPTIONS injected.
# Output stays attached to your terminal so you see the ASAN report immediately
# when (if) the wizard navigation provokes a heap error.
deploy-pi-asan-fg: deploy-pi-asan
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) under AddressSanitizer...$(RESET)"
	@echo "$(DIM)ASAN_OPTIONS=$(PI_ASAN_OPTIONS) LSAN_OPTIONS=$(PI_LSAN_OPTIONS)$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ASAN_OPTIONS='$(PI_ASAN_OPTIONS)' LSAN_OPTIONS='$(PI_LSAN_OPTIONS)' ./bin/helix-launcher.sh --debug --log-dest=console"

# Full cycle: ASAN build + deploy + run in foreground.
pi-asan-test: pi-asan-docker deploy-pi-asan-fg

# =============================================================================
# Pi 32-bit Deployment Targets
# =============================================================================
# Shares PI_HOST/PI_USER/PI_DEPLOY_DIR with 64-bit Pi (same device, different binary arch)

.PHONY: deploy-pi32 deploy-pi32-fg deploy-pi32-bin pi32-test

# Deploy full application to Pi (32-bit) and restart in background
deploy-pi32:
	@test -f build/pi32/bin/helix-screen || { echo "$(RED)Error: build/pi32/bin/helix-screen not found. Run 'make pi32-docker' first.$(RESET)"; exit 1; }
	@test -f build/pi32/bin/helix-splash || { echo "$(RED)Error: build/pi32/bin/helix-splash not found. Run 'make pi32-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi32/bin)
	@echo "$(GREEN)✓ Deployed to $(PI_HOST):$(PI_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Restarting helix-screen on $(PI_HOST)...$(RESET)"
	@ssh $(PI_SSH_TARGET) "sudo systemctl restart helixscreen 2>/dev/null" \
		|| ssh $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && setsid ./bin/helix-launcher.sh </dev/null >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"
	@echo "$(DIM)Logs: ssh $(PI_SSH_TARGET) 'journalctl -u helixscreen -f'$(RESET)"

# Deploy and run in foreground with debug logging (for interactive debugging)
deploy-pi32-fg:
	@test -f build/pi32/bin/helix-screen || { echo "$(RED)Error: build/pi32/bin/helix-screen not found. Run 'make pi32-docker' first.$(RESET)"; exit 1; }
	@test -f build/pi32/bin/helix-splash || { echo "$(RED)Error: build/pi32/bin/helix-splash not found. Run 'make pi32-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi32/bin)
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) (foreground, debug mode)...$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug --log-dest=console"

# Deploy binaries only (fast, for quick iteration)
deploy-pi32-bin:
	@test -f build/pi32/bin/helix-screen || { echo "$(RED)Error: build/pi32/bin/helix-screen not found. Run 'make pi32-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binaries only to $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(PI_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 0.5; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null; while pidof helix-screen helix-splash helix-watchdog >/dev/null 2>&1; do sleep 0.2; done; true"
	ssh $(PI_SSH_TARGET) "mkdir -p $(PI_DEPLOY_DIR)/bin"
	rsync -avzz --progress build/pi32/bin/helix-screen build/pi32/bin/helix-splash $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/bin/
	@if [ -f build/pi32/bin/helix-watchdog ]; then rsync -avzz build/pi32/bin/helix-watchdog $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/bin/; fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	$(call sync-device-features,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi32/bin)
	@echo "$(CYAN)Restarting helix-screen on $(PI_HOST)...$(RESET)"
	@ssh $(PI_SSH_TARGET) "sudo systemctl restart helixscreen 2>/dev/null" \
		|| ssh $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && setsid ./bin/helix-launcher.sh </dev/null >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"

# Full cycle: build + deploy + run in foreground
pi32-test: pi32-docker deploy-pi32-fg

.PHONY: deploy-pi32-asan deploy-pi32-asan-fg pi32-asan-test

# Deploy the pi32 ASAN binary. Shares PI_ASAN_OPTIONS with the 64-bit Pi target.
deploy-pi32-asan:
	@test -f build/pi32-asan/bin/helix-screen || { echo "$(RED)Error: build/pi32-asan/bin/helix-screen not found. Run 'make pi32-asan-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi32-asan/bin)
	@echo "$(CYAN)Shipping ASAN suppressions...$(RESET)"
	$(Q)rsync -avzz tests/asan.supp $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/
	@echo "$(GREEN)✓ Deployed ASAN build to $(PI_HOST):$(PI_DEPLOY_DIR)$(RESET)"
	@echo "$(YELLOW)Note: do NOT systemd-restart — ASAN binaries need ASAN_OPTIONS in env.$(RESET)"

# Run the pi32 ASAN binary in the foreground with ASAN_OPTIONS injected so the
# heap error report lands on your terminal as soon as the boot/print-history
# load provokes it.
deploy-pi32-asan-fg: deploy-pi32-asan
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) under AddressSanitizer...$(RESET)"
	@echo "$(DIM)ASAN_OPTIONS=$(PI_ASAN_OPTIONS) LSAN_OPTIONS=$(PI_LSAN_OPTIONS)$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ASAN_OPTIONS='$(PI_ASAN_OPTIONS)' LSAN_OPTIONS='$(PI_LSAN_OPTIONS)' ./bin/helix-launcher.sh --debug --log-dest=console"

# Full cycle: ASAN build + deploy + run in foreground.
pi32-asan-test: pi32-asan-docker deploy-pi32-asan-fg

# =============================================================================
# AD5M Deployment Configuration
# =============================================================================

# AD5M deployment settings (can override via environment or command line)
# Example: make deploy-ad5m AD5M_HOST=192.168.1.100
# Note: AD5M uses BusyBox and only has scp (no rsync), so we use scp -O for compatibility
#
# Deploy directory is auto-detected:
#   - KlipperMod: /root/printer_software/helixscreen (if /root/printer_software exists)
#   - Forge-X/Stock: /opt/helixscreen
# Override with AD5M_DEPLOY_DIR if needed.
AD5M_HOST ?= ad5m.local
AD5M_USER ?= root

# Build SSH target for AD5M
AD5M_SSH_TARGET := $(AD5M_USER)@$(AD5M_HOST)

# Auto-detect deploy directory (KlipperMod vs Forge-X/Stock)
# Can be overridden: make deploy-ad5m AD5M_DEPLOY_DIR=/custom/path
AD5M_DEPLOY_DIR ?= $(shell ssh -o ConnectTimeout=5 $(AD5M_SSH_TARGET) \
	"if [ -d /root/printer_software ]; then echo /root/printer_software/helixscreen; else echo /opt/helixscreen; fi" 2>/dev/null || echo /opt/helixscreen)

# =============================================================================
# AD5M Deployment Targets
# =============================================================================

.PHONY: deploy-ad5m deploy-ad5m-fg deploy-ad5m-bin ad5m-ssh ad5m-test

# Deploy full application to AD5M using tar/scp (AD5M BusyBox has no rsync)
deploy-ad5m:
	@test -f build/ad5m/bin/helix-screen || { echo "$(RED)Error: build/ad5m/bin/helix-screen not found. Run 'make remote-ad5m' first.$(RESET)"; exit 1; }
	@test -f build/ad5m/bin/helix-splash || { echo "$(RED)Error: build/ad5m/bin/helix-splash not found. Run 'make remote-ad5m' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)...$(RESET)"
	@# Generate pre-rendered images if missing
	@if [ ! -f build/assets/images/prerendered/splash-logo-small.bin ]; then \
		echo "$(DIM)Generating pre-rendered splash images...$(RESET)"; \
		$(MAKE) gen-images-ad5m; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ] || [ -z "$$(ls -A build/assets/images/printers/prerendered/*.bin 2>/dev/null)" ]; then \
		echo "$(DIM)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/splash-3d-dark-small.bin ]; then \
		echo "$(DIM)Generating 3D splash images...$(RESET)"; \
		$(MAKE) gen-splash-3d-ad5m; \
	fi
	@# Stop running processes and wait for them to exit (prevents "Text file busy")
	ssh $(AD5M_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 0.5; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null; while pidof helix-screen helix-splash helix-watchdog >/dev/null 2>&1; do sleep 0.2; done; true"
	ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/bin"
	@# Transfer binaries via cat/ssh (AD5M has no scp sftp-server)
	@echo "$(DIM)Transferring binaries...$(RESET)"
	cat build/ad5m/bin/helix-screen | ssh $(AD5M_SSH_TARGET) "cat > $(AD5M_DEPLOY_DIR)/bin/helix-screen && chmod +x $(AD5M_DEPLOY_DIR)/bin/helix-screen"
	cat build/ad5m/bin/helix-splash | ssh $(AD5M_SSH_TARGET) "cat > $(AD5M_DEPLOY_DIR)/bin/helix-splash && chmod +x $(AD5M_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/ad5m/bin/helix-watchdog ]; then \
		cat build/ad5m/bin/helix-watchdog | ssh $(AD5M_SSH_TARGET) "cat > $(AD5M_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(AD5M_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	cat scripts/helix-launcher.sh | ssh $(AD5M_SSH_TARGET) "cat > $(AD5M_DEPLOY_DIR)/bin/helix-launcher.sh && chmod +x $(AD5M_DEPLOY_DIR)/bin/helix-launcher.sh"
	@# Transfer installer script (needed for auto-updates)
	cat scripts/$(INSTALLER_FILENAME) | ssh $(AD5M_SSH_TARGET) "cat > $(AD5M_DEPLOY_DIR)/$(INSTALLER_FILENAME) && chmod +x $(AD5M_DEPLOY_DIR)/$(INSTALLER_FILENAME)"
	@# Transfer assets via tar (uses shared DEPLOY_TAR_EXCLUDES and DEPLOY_ASSET_DIRS)
	@# AD5M now has tracker support (PWM PCM mode) — include .mod/.med files
	@echo "$(DIM)Transferring assets...$(RESET)"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_ASSET_DIRS) | ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR) && tar -xof -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/assets/images/prerendered $(AD5M_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR)/assets/images && tar -xof -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR)/assets/images/printers && tar -xof -"; \
	fi
	@# Deploy CA certificates for HTTPS verification
	@if [ -f build/ad5m/certs/ca-certificates.crt ]; then \
		echo "$(DIM)Transferring CA certificates...$(RESET)"; \
		ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/certs"; \
		cat build/ad5m/certs/ca-certificates.crt | ssh $(AD5M_SSH_TARGET) "cat > $(AD5M_DEPLOY_DIR)/certs/ca-certificates.crt"; \
	fi
	@# AD5M-specific: Deploy platform hooks (auto-detect firmware variant)
	@echo "$(DIM)Deploying platform hooks...$(RESET)"
	@ssh $(AD5M_SSH_TARGET) '\
		mkdir -p $(AD5M_DEPLOY_DIR)/platform; \
		if [ -d /mnt/data/.klipper_mod ]; then \
			HOOK="$(AD5M_DEPLOY_DIR)/assets/config/platform/hooks-ad5m-kmod.sh"; \
		elif [ -d /opt/config/mod/.root ]; then \
			HOOK="$(AD5M_DEPLOY_DIR)/assets/config/platform/hooks-ad5m-forgex.sh"; \
		else \
			HOOK=""; \
		fi; \
		if [ -n "$$HOOK" ] && [ -f "$$HOOK" ]; then \
			cp "$$HOOK" "$(AD5M_DEPLOY_DIR)/platform/hooks.sh"; \
			chmod +x "$(AD5M_DEPLOY_DIR)/platform/hooks.sh"; \
			echo "Platform hooks deployed: $$HOOK"; \
		else \
			echo "No platform hooks to deploy"; \
		fi'
	@# AD5M-specific: Update init script in /etc/init.d/ if it differs
	@echo "$(DIM)Checking init script...$(RESET)"
	@ssh $(AD5M_SSH_TARGET) '\
		INIT_SCRIPT=""; \
		if [ -f /etc/init.d/S80helixscreen ]; then INIT_SCRIPT="/etc/init.d/S80helixscreen"; \
		elif [ -f /etc/init.d/S90helixscreen ]; then INIT_SCRIPT="/etc/init.d/S90helixscreen"; fi; \
		if [ -n "$$INIT_SCRIPT" ]; then \
			if ! cmp -s "$$INIT_SCRIPT" "$(AD5M_DEPLOY_DIR)/config/helixscreen.init" 2>/dev/null; then \
				echo "Updating $$INIT_SCRIPT..."; \
				cp "$(AD5M_DEPLOY_DIR)/config/helixscreen.init" "$$INIT_SCRIPT"; \
				sed -i "s|DAEMON_DIR=\"/opt/helixscreen\"|DAEMON_DIR=\"$(AD5M_DEPLOY_DIR)\"|" "$$INIT_SCRIPT"; \
				chmod +x "$$INIT_SCRIPT"; \
				echo "Init script updated"; \
			else \
				echo "Init script unchanged"; \
			fi; \
		fi'
	@echo "$(GREEN)✓ Deployed to $(AD5M_HOST):$(AD5M_DEPLOY_DIR)$(RESET)"
	$(call sync-device-features,$(AD5M_SSH_TARGET),$(AD5M_DEPLOY_DIR),build/ad5m/bin)
	@echo "$(CYAN)Restarting helix-screen on $(AD5M_HOST)...$(RESET)"
	ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted in background$(RESET)"
	@echo "$(DIM)Logs: ssh $(AD5M_SSH_TARGET) 'tail -f /var/log/messages | grep helix'$(RESET)"

# Legacy deploy using tar/scp (for systems without rsync)
deploy-ad5m-legacy:
	@test -f build/ad5m/bin/helix-screen || { echo "$(RED)Error: build/ad5m/bin/helix-screen not found. Run 'make remote-ad5m' first.$(RESET)"; exit 1; }
	@test -f build/ad5m/bin/helix-splash || { echo "$(RED)Error: build/ad5m/bin/helix-splash not found. Run 'make remote-ad5m' first.$(RESET)"; exit 1; }
	@# Generate pre-rendered images if missing (requires Python/PIL)
	@if [ ! -f build/assets/images/prerendered/splash-logo-small.bin ]; then \
		echo "$(CYAN)Generating pre-rendered splash images for AD5M...$(RESET)"; \
		$(MAKE) gen-images-ad5m; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ]; then \
		echo "$(CYAN)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/splash-3d-dark-small.bin ]; then \
		echo "$(CYAN)Generating 3D splash images for AD5M...$(RESET)"; \
		$(MAKE) gen-splash-3d-ad5m; \
	fi
	@echo "$(CYAN)Deploying HelixScreen to $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)...$(RESET)"
	@echo "  Binaries: helix-screen, helix-splash, helix-watchdog"
	@echo "  Assets: ui_xml/, assets/ (excl. test files), config/"
	ssh $(AD5M_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 0.5; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null; while pidof helix-screen helix-splash helix-watchdog >/dev/null 2>&1; do sleep 0.2; done; true"
	ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/bin"
	scp -O build/ad5m/bin/helix-screen build/ad5m/bin/helix-splash $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)/bin/
	@if [ -f build/ad5m/bin/helix-watchdog ]; then scp -O build/ad5m/bin/helix-watchdog $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)/bin/; fi
	scp -O scripts/helix-launcher.sh $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)/bin/
	@echo "$(DIM)Transferring assets (excluding test files)...$(RESET)"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) ui_xml assets config | ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR) && tar -xof -"
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered splash images...$(RESET)"; \
		ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/assets/images/prerendered"; \
		scp -O build/assets/images/prerendered/*.bin $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)/assets/images/prerendered/; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered printer images...$(RESET)"; \
		ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		scp -O build/assets/images/printers/prerendered/*.bin $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)/assets/images/printers/prerendered/; \
	fi
	@# Update init script in /etc/init.d/ if it differs from deployed version
	@echo "$(DIM)Checking init script...$(RESET)"
	@ssh $(AD5M_SSH_TARGET) '\
		INIT_SCRIPT=""; \
		if [ -f /etc/init.d/S80helixscreen ]; then INIT_SCRIPT="/etc/init.d/S80helixscreen"; \
		elif [ -f /etc/init.d/S90helixscreen ]; then INIT_SCRIPT="/etc/init.d/S90helixscreen"; fi; \
		if [ -n "$$INIT_SCRIPT" ]; then \
			if ! cmp -s "$$INIT_SCRIPT" "$(AD5M_DEPLOY_DIR)/config/helixscreen.init" 2>/dev/null; then \
				echo "Updating $$INIT_SCRIPT..."; \
				cp "$(AD5M_DEPLOY_DIR)/config/helixscreen.init" "$$INIT_SCRIPT"; \
				sed -i "s|DAEMON_DIR=\"/opt/helixscreen\"|DAEMON_DIR=\"$(AD5M_DEPLOY_DIR)\"|" "$$INIT_SCRIPT"; \
				chmod +x "$$INIT_SCRIPT"; \
				echo "Init script updated"; \
			else \
				echo "Init script unchanged"; \
			fi; \
		fi'
	@echo "$(GREEN)✓ Deployed to $(AD5M_HOST):$(AD5M_DEPLOY_DIR)$(RESET)"
	$(call sync-device-features,$(AD5M_SSH_TARGET),$(AD5M_DEPLOY_DIR),build/ad5m/bin)
	@echo "$(CYAN)Restarting helix-screen on $(AD5M_HOST)...$(RESET)"
	ssh $(AD5M_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; cd $(AD5M_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted in background$(RESET)"
	@echo "$(DIM)Logs: ssh $(AD5M_SSH_TARGET) 'tail -f /var/log/messages | grep helix'$(RESET)"

# Deploy and run in foreground with verbose logging (for interactive debugging)
deploy-ad5m-fg:
	@test -f build/ad5m/bin/helix-screen || { echo "$(RED)Error: build/ad5m/bin/helix-screen not found. Run 'make remote-ad5m' first.$(RESET)"; exit 1; }
	@test -f build/ad5m/bin/helix-splash || { echo "$(RED)Error: build/ad5m/bin/helix-splash not found. Run 'make remote-ad5m' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(AD5M_SSH_TARGET),$(AD5M_DEPLOY_DIR),build/ad5m/bin)
	@echo "$(CYAN)Starting helix-screen on $(AD5M_HOST) (foreground, verbose)...$(RESET)"
	ssh -t $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug"

# Deploy binaries only (fast, for quick iteration)
deploy-ad5m-bin:
	@test -f build/ad5m/bin/helix-screen || { echo "$(RED)Error: build/ad5m/bin/helix-screen not found. Run 'make remote-ad5m' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binaries only to $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(AD5M_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 0.5; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null; while pidof helix-screen helix-splash helix-watchdog >/dev/null 2>&1; do sleep 0.2; done; true"
	ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/bin"
	scp -O build/ad5m/bin/helix-screen build/ad5m/bin/helix-splash $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)/bin/
	@if [ -f build/ad5m/bin/helix-watchdog ]; then scp -O build/ad5m/bin/helix-watchdog $(AD5M_SSH_TARGET):$(AD5M_DEPLOY_DIR)/bin/; fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	$(call sync-device-features,$(AD5M_SSH_TARGET),$(AD5M_DEPLOY_DIR),build/ad5m/bin)
	@echo "$(CYAN)Restarting helix-screen on $(AD5M_HOST)...$(RESET)"
	ssh $(AD5M_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; cd $(AD5M_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"

# Convenience: SSH into the AD5M
ad5m-ssh:
	ssh $(AD5M_SSH_TARGET)

# Full cycle: remote build + deploy + run in foreground
ad5m-test: remote-ad5m deploy-ad5m-fg

# =============================================================================
# CC1 Deployment Configuration
# =============================================================================

# Centauri Carbon 1 deployment settings (can override via environment or command line)
# Example: make deploy-cc1 CC1_HOST=192.168.1.100
#
# Deploy directory defaults to /user-resource/helixscreen (COSMOS stock firmware
# install path). COSMOS mounts / read-only, so /opt does not exist; writable
# space lives under /data and /user-resource. Override CC1_DEPLOY_DIR for
# alternate firmware variants (Forge-X, OpenCentauri) as needed.
#
# Start/stop goes through /etc/init.d/helixscreen (the install script registers
# it under S80/S90). This preserves splash, watchdog, and platform hooks rather
# than racing them with killall.
CC1_HOST ?= cc1.local
CC1_USER ?= root
CC1_DEPLOY_DIR ?= /user-resource/helixscreen
CC1_INIT_SCRIPT ?= /etc/init.d/helixscreen

# Build SSH target for CC1
CC1_SSH_TARGET := $(CC1_USER)@$(CC1_HOST)

# =============================================================================
# CC1 Deployment Targets
# =============================================================================

.PHONY: deploy-cc1 deploy-cc1-fg deploy-cc1-bin cc1-ssh cc1-test

# Deploy full application to CC1 using tar/ssh
deploy-cc1:
	@test -f build/cc1/bin/helix-screen || { echo "$(RED)Error: build/cc1/bin/helix-screen not found. Run 'make cc1-docker' first.$(RESET)"; exit 1; }
	@test -f build/cc1/bin/helix-splash || { echo "$(RED)Error: build/cc1/bin/helix-splash not found. Run 'make cc1-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(CC1_SSH_TARGET):$(CC1_DEPLOY_DIR)...$(RESET)"
	@# Generate pre-rendered images if missing
	@if [ ! -f build/assets/images/prerendered/splash-logo-small.bin ]; then \
		echo "$(DIM)Generating pre-rendered splash images...$(RESET)"; \
		$(MAKE) gen-images; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ] || [ -z "$$(ls -A build/assets/images/printers/prerendered/*.bin 2>/dev/null)" ]; then \
		echo "$(DIM)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@# Stop running processes via init script (falls back to killall for pre-init installs)
	ssh $(CC1_SSH_TARGET) "if [ -x $(CC1_INIT_SCRIPT) ]; then $(CC1_INIT_SCRIPT) stop || true; else killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; fi; rm -f /tmp/helix-screen.lock; mkdir -p $(CC1_DEPLOY_DIR)/bin"
	@# Transfer binaries via cat/ssh
	@echo "$(DIM)Transferring binaries...$(RESET)"
	cat build/cc1/bin/helix-screen | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-screen"
	cat build/cc1/bin/helix-splash | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/cc1/bin/helix-watchdog ]; then \
		cat build/cc1/bin/helix-watchdog | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	cat scripts/helix-launcher.sh | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-launcher.sh && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-launcher.sh"
	@# Transfer installer script (needed for auto-updates)
	cat scripts/install.sh | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/install.sh && chmod +x $(CC1_DEPLOY_DIR)/install.sh"
	@# Transfer assets via tar (uses shared DEPLOY_TAR_EXCLUDES and DEPLOY_ASSET_DIRS)
	@echo "$(DIM)Transferring assets...$(RESET)"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_TAR_NO_TRACKER) $(DEPLOY_ASSET_DIRS) | ssh $(CC1_SSH_TARGET) "cd $(CC1_DEPLOY_DIR) && tar -xof -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(CC1_SSH_TARGET) "mkdir -p $(CC1_DEPLOY_DIR)/assets/images/prerendered $(CC1_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(CC1_SSH_TARGET) "cd $(CC1_DEPLOY_DIR)/assets/images && tar -xof -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(CC1_SSH_TARGET) "cd $(CC1_DEPLOY_DIR)/assets/images/printers && tar -xof -"; \
	fi
	@# Deploy CA certificates for HTTPS verification
	@if [ -f build/cc1/certs/ca-certificates.crt ]; then \
		echo "$(DIM)Transferring CA certificates...$(RESET)"; \
		ssh $(CC1_SSH_TARGET) "mkdir -p $(CC1_DEPLOY_DIR)/certs"; \
		cat build/cc1/certs/ca-certificates.crt | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/certs/ca-certificates.crt"; \
	fi
	@echo "$(GREEN)✓ Deployed to $(CC1_HOST):$(CC1_DEPLOY_DIR)$(RESET)"
	$(call sync-device-features,$(CC1_SSH_TARGET),$(CC1_DEPLOY_DIR),build/cc1/bin)
	@echo "$(CYAN)Starting helix-screen on $(CC1_HOST)...$(RESET)"
	ssh $(CC1_SSH_TARGET) "if [ -x $(CC1_INIT_SCRIPT) ]; then $(CC1_INIT_SCRIPT) start; else cd $(CC1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 & fi"
	@echo "$(GREEN)✓ helix-screen started$(RESET)"
	@echo "$(DIM)Logs: ssh $(CC1_SSH_TARGET) 'tail -f /tmp/helixscreen.log'$(RESET)"

# Deploy and run in foreground with verbose logging (for interactive debugging)
deploy-cc1-fg:
	@test -f build/cc1/bin/helix-screen || { echo "$(RED)Error: build/cc1/bin/helix-screen not found. Run 'make cc1-docker' first.$(RESET)"; exit 1; }
	@test -f build/cc1/bin/helix-splash || { echo "$(RED)Error: build/cc1/bin/helix-splash not found. Run 'make cc1-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(CC1_SSH_TARGET),$(CC1_DEPLOY_DIR),build/cc1/bin)
	@echo "$(CYAN)Starting helix-screen on $(CC1_HOST) (foreground, verbose)...$(RESET)"
	ssh -t $(CC1_SSH_TARGET) "cd $(CC1_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug"

# Deploy binaries only (fast, for quick iteration)
deploy-cc1-bin:
	@test -f build/cc1/bin/helix-screen || { echo "$(RED)Error: build/cc1/bin/helix-screen not found. Run 'make cc1-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binaries only to $(CC1_SSH_TARGET):$(CC1_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(CC1_SSH_TARGET) "if [ -x $(CC1_INIT_SCRIPT) ]; then $(CC1_INIT_SCRIPT) stop || true; else killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; fi; rm -f /tmp/helix-screen.lock; mkdir -p $(CC1_DEPLOY_DIR)/bin"
	cat build/cc1/bin/helix-screen | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-screen"
	cat build/cc1/bin/helix-splash | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/cc1/bin/helix-watchdog ]; then \
		cat build/cc1/bin/helix-watchdog | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	$(call sync-device-features,$(CC1_SSH_TARGET),$(CC1_DEPLOY_DIR),build/cc1/bin)
	@echo "$(CYAN)Restarting helix-screen on $(CC1_HOST)...$(RESET)"
	ssh $(CC1_SSH_TARGET) "if [ -x $(CC1_INIT_SCRIPT) ]; then $(CC1_INIT_SCRIPT) start; else cd $(CC1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 & fi"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"

# Convenience: SSH into the CC1
cc1-ssh:
	ssh $(CC1_SSH_TARGET)

# Full cycle: docker build + deploy + run in foreground
cc1-test: cc1-docker deploy-cc1-fg

# =============================================================================
# Snapmaker U1 Deployment Configuration
# =============================================================================
# Snapmaker U1 deployment settings
# Specs: Rockchip RK3562 ARM64, 3.5" 480x320 display, 961MB RAM, Debian Trixie
# Deploy to /home/lava/helixscreen (persistent across reboots — /opt/ is overlay)
# No sudo on device — run as root directly
#
# Example: make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=192.168.30.103
SNAPMAKER_U1_HOST ?= snapmaker-u1.local
SNAPMAKER_U1_USER ?= root
SNAPMAKER_U1_SSH_TARGET := $(SNAPMAKER_U1_USER)@$(SNAPMAKER_U1_HOST)
SNAPMAKER_U1_DEPLOY_DIR ?= /userdata/helixscreen

# =============================================================================
# Snapmaker U1 Deployment Targets
# =============================================================================

.PHONY: deploy-snapmaker-u1 deploy-snapmaker-u1-fg deploy-snapmaker-u1-bin snapmaker-u1-ssh

# Common deploy recipe for Snapmaker U1
# Deploys binaries, assets, hooks, init script, and sets up auto-start
define snapmaker-u1-deploy-common
	ssh $(SNAPMAKER_U1_SSH_TARGET) "killall helix-screen helix-splash helix-watchdog 2>/dev/null || true; mkdir -p $(SNAPMAKER_U1_DEPLOY_DIR)/bin $(SNAPMAKER_U1_DEPLOY_DIR)/platform"
	scp build/snapmaker-u1/bin/helix-screen $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/
	@if [ -f build/snapmaker-u1/bin/helix-splash ]; then scp build/snapmaker-u1/bin/helix-splash $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/; fi
	@if [ -f build/snapmaker-u1/bin/helix-watchdog ]; then scp build/snapmaker-u1/bin/helix-watchdog $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/; fi
	ssh $(SNAPMAKER_U1_SSH_TARGET) "chmod +x $(SNAPMAKER_U1_DEPLOY_DIR)/bin/helix-*"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_TAR_NO_TRACKER) $(DEPLOY_ASSET_DIRS) | ssh $(SNAPMAKER_U1_SSH_TARGET) "cd $(SNAPMAKER_U1_DEPLOY_DIR) && tar -xof -"
	cat assets/config/platform/hooks-snapmaker-u1.sh | ssh $(SNAPMAKER_U1_SSH_TARGET) "cat > $(SNAPMAKER_U1_DEPLOY_DIR)/platform/hooks.sh && chmod +x $(SNAPMAKER_U1_DEPLOY_DIR)/platform/hooks.sh"
	scp scripts/helix-launcher.sh $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/ && ssh $(SNAPMAKER_U1_SSH_TARGET) "chmod +x $(SNAPMAKER_U1_DEPLOY_DIR)/bin/helix-launcher.sh"
	@# Patch the init script AT THE PATH THE INSTALLER USES — config/helixscreen.init.
	@# install_service_snapmaker_u1() (scripts/install.sh) patches that copy, and
	@# snapmaker-u1-setup-autostart.sh points the S99 boot hooks at it. Deploy used
	@# to patch a top-level $(SNAPMAKER_U1_DEPLOY_DIR)/helixscreen.init instead, so
	@# the tar's UNPATCHED config/ copy (DAEMON_DIR="/opt/helixscreen") was what ran
	@# at boot: `make deploy` worked, every reboot came up dark, and the device sat
	@# on the stock splash with no WiFi. Same sed pattern as the installer
	@# (DAEMON_DIR=.* not a literal /opt match) so an already-patched copy is
	@# re-pointed rather than silently skipped.
	cat config/helixscreen.init | ssh $(SNAPMAKER_U1_SSH_TARGET) "sed 's|DAEMON_DIR=.*|DAEMON_DIR=\"$(SNAPMAKER_U1_DEPLOY_DIR)\"  # Updated by installer for different firmware|' > $(SNAPMAKER_U1_DEPLOY_DIR)/config/helixscreen.init && chmod +x $(SNAPMAKER_U1_DEPLOY_DIR)/config/helixscreen.init"
	@# Remove the legacy top-level copy this target used to write, so a stale
	@# divergent init can't be started by hand or by an older habit.
	ssh $(SNAPMAKER_U1_SSH_TARGET) "rm -f $(SNAPMAKER_U1_DEPLOY_DIR)/helixscreen.init"
	@if [ -f "build/snapmaker-u1/certs/ca-certificates.crt" ]; then \
		echo "  $(DIM)Deploying CA certificates...$(RESET)"; \
		ssh $(SNAPMAKER_U1_SSH_TARGET) "mkdir -p $(SNAPMAKER_U1_DEPLOY_DIR)/certs"; \
		scp build/snapmaker-u1/certs/ca-certificates.crt $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/certs/; \
	fi
	@echo "  $(DIM)Setting up auto-start...$(RESET)"
	scp scripts/snapmaker-u1-setup-autostart.sh $(SNAPMAKER_U1_SSH_TARGET):/tmp/
	ssh $(SNAPMAKER_U1_SSH_TARGET) "sh /tmp/snapmaker-u1-setup-autostart.sh $(SNAPMAKER_U1_DEPLOY_DIR)"
	@# Mirror the installer's setup_config_symlink: helixscreen.env lives at the
	@# Klipper-convention path (printer_data/config/helixscreen/) so Mainsail/Fluidd
	@# can edit it, and the install-dir copy is a symlink. Fast deploys don't run
	@# the full installer, so this step repairs the symlink if it got lost (e.g. an
	@# older Makefile version copied a real env file into the install dir).
	@echo "  $(DIM)Verifying helixscreen.env symlink...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) 'PD=/oem/printer_data/config/helixscreen; IE=$(SNAPMAKER_U1_DEPLOY_DIR)/config/helixscreen.env; CE=$$PD/helixscreen.env; mkdir -p $$PD; if [ -L "$$IE" ]; then :; elif [ -f "$$IE" ] && [ ! -e "$$CE" ]; then mv "$$IE" "$$CE" && ln -s "$$CE" "$$IE" && echo "  migrated install env to $$CE"; elif [ -f "$$IE" ] && [ -f "$$CE" ]; then if cmp -s "$$IE" "$$CE"; then rm -f "$$IE" && ln -s "$$CE" "$$IE" && echo "  collapsed duplicate install env to symlink"; else echo "  WARN: helixscreen.env diverges between $$IE and $$CE — keeping both, please reconcile manually"; fi; elif [ ! -e "$$IE" ] && [ -f "$$CE" ]; then ln -s "$$CE" "$$IE" && echo "  created missing install env symlink"; fi'
endef

deploy-snapmaker-u1:
	@test -f build/snapmaker-u1/bin/helix-screen || { echo "$(RED)Error: build/snapmaker-u1/bin/helix-screen not found. Run 'make snapmaker-u1-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)...$(RESET)"
	$(call snapmaker-u1-deploy-common)
	@echo "$(GREEN)✓ Deployed to $(SNAPMAKER_U1_HOST):$(SNAPMAKER_U1_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Starting helix-screen on $(SNAPMAKER_U1_HOST)...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) "$(SNAPMAKER_U1_DEPLOY_DIR)/config/helixscreen.init start"

deploy-snapmaker-u1-fg:
	@test -f build/snapmaker-u1/bin/helix-screen || { echo "$(RED)Error: build/snapmaker-u1/bin/helix-screen not found. Run 'make snapmaker-u1-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)...$(RESET)"
	$(call snapmaker-u1-deploy-common)
	@echo "$(GREEN)✓ Deployed to $(SNAPMAKER_U1_HOST):$(SNAPMAKER_U1_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Starting helix-screen on $(SNAPMAKER_U1_HOST) (foreground, verbose)...$(RESET)"
	ssh -t $(SNAPMAKER_U1_SSH_TARGET) "cd $(SNAPMAKER_U1_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug"

deploy-snapmaker-u1-bin:
	@test -f build/snapmaker-u1/bin/helix-screen || { echo "$(RED)Error: build/snapmaker-u1/bin/helix-screen not found. Run 'make snapmaker-u1-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binary only to $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) "killall helix-screen helix-watchdog 2>/dev/null || true"
	scp build/snapmaker-u1/bin/helix-screen $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/
	ssh $(SNAPMAKER_U1_SSH_TARGET) "chmod +x $(SNAPMAKER_U1_DEPLOY_DIR)/bin/helix-screen"
	@echo "$(GREEN)✓ Binary deployed$(RESET)"
	$(call sync-device-features,$(SNAPMAKER_U1_SSH_TARGET),$(SNAPMAKER_U1_DEPLOY_DIR),build/snapmaker-u1/bin)
	@echo "$(CYAN)Restarting helix-screen on $(SNAPMAKER_U1_HOST)...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) "$(SNAPMAKER_U1_DEPLOY_DIR)/config/helixscreen.init restart"

snapmaker-u1-ssh:
	ssh $(SNAPMAKER_U1_SSH_TARGET)

# =============================================================================
# K1 Deployment Configuration
# =============================================================================
# Creality K1 series deployment settings
# Based on pellcorp/creality Simple AF structure
#
# Example: make deploy-k1 K1_HOST=192.168.1.100
# Note: K1 uses BusyBox, similar to AD5M - tar/ssh transfer, no rsync
#
# Default deploy directory follows Simple AF convention: /usr/data/helixscreen
# Override with K1_DEPLOY_DIR if needed.
K1_HOST ?= k1.local
K1_USER ?= root
K1_DEPLOY_DIR ?= /usr/data/helixscreen

# Build SSH target for K1
K1_SSH_TARGET := $(K1_USER)@$(K1_HOST)

# =============================================================================
# K1 Deployment Targets
# =============================================================================

.PHONY: deploy-k1 deploy-k1-fg deploy-k1-bin k1-ssh k1-test

# Deploy full application to K1 using tar/ssh (K1 BusyBox has no rsync)
deploy-k1:
	@test -f build/mips/bin/helix-screen || { echo "$(RED)Error: build/mips/bin/helix-screen not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@test -f build/mips/bin/helix-splash || { echo "$(RED)Error: build/mips/bin/helix-splash not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(K1_SSH_TARGET):$(K1_DEPLOY_DIR)...$(RESET)"
	@# Generate pre-rendered images if missing
	@if [ ! -f build/assets/images/prerendered/splash-logo-small.bin ]; then \
		echo "$(DIM)Generating pre-rendered splash images...$(RESET)"; \
		$(MAKE) gen-images; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ] || [ -z "$$(ls -A build/assets/images/printers/prerendered/*.bin 2>/dev/null)" ]; then \
		echo "$(DIM)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/splash-3d-dark-tiny_alt.bin ]; then \
		echo "$(DIM)Generating 3D splash images for K1...$(RESET)"; \
		$(MAKE) gen-splash-3d-k1; \
	fi
	@# Stop running processes and prepare directory
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock; mkdir -p $(K1_DEPLOY_DIR)/bin"
	@# Transfer binaries via cat/ssh
	@echo "$(DIM)Transferring binaries...$(RESET)"
	cat build/mips/bin/helix-screen | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K1_DEPLOY_DIR)/bin/helix-screen"
	cat build/mips/bin/helix-splash | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/mips/bin/helix-watchdog ]; then \
		cat build/mips/bin/helix-watchdog | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	cat scripts/helix-launcher.sh | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-launcher.sh && chmod +x $(K1_DEPLOY_DIR)/bin/helix-launcher.sh"
	@# Transfer assets via tar
	@echo "$(DIM)Transferring assets...$(RESET)"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_TAR_NO_TRACKER) $(DEPLOY_ASSET_DIRS) | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && tar -xof -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(K1_SSH_TARGET) "mkdir -p $(K1_DEPLOY_DIR)/assets/images/prerendered $(K1_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR)/assets/images && tar -xof -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR)/assets/images/printers && tar -xof -"; \
	fi
	@# Install/update init script for boot persistence
	@echo "$(DIM)Installing init script...$(RESET)"
	@cat config/helixscreen.init | ssh $(K1_SSH_TARGET) '\
		DEST=/etc/init.d/S99helixscreen; \
		cat > $$DEST && chmod +x $$DEST && \
		sed -i "s|DAEMON_DIR=.*|DAEMON_DIR=\"$(K1_DEPLOY_DIR)\"|" $$DEST && \
		echo "Init script installed at $$DEST"'
	@echo "$(GREEN)✓ Deployed to $(K1_HOST):$(K1_DEPLOY_DIR)$(RESET)"
	$(call sync-device-features,$(K1_SSH_TARGET),$(K1_DEPLOY_DIR),build/mips/bin)
	@echo "$(CYAN)Starting helix-screen on $(K1_HOST)...$(RESET)"
	ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen started in background$(RESET)"
	@echo "$(DIM)Logs: ssh $(K1_SSH_TARGET) 'tail -f /var/log/messages | grep helix'$(RESET)"

# Deploy and run in foreground with verbose logging (for interactive debugging)
deploy-k1-fg:
	@test -f build/mips/bin/helix-screen || { echo "$(RED)Error: build/mips/bin/helix-screen not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@test -f build/mips/bin/helix-splash || { echo "$(RED)Error: build/mips/bin/helix-splash not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(K1_SSH_TARGET),$(K1_DEPLOY_DIR),build/mips/bin)
	@echo "$(CYAN)Starting helix-screen on $(K1_HOST) (foreground, verbose)...$(RESET)"
	ssh -t $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug"

# Deploy binaries only (fast, for quick iteration)
deploy-k1-bin:
	@test -f build/mips/bin/helix-screen || { echo "$(RED)Error: build/mips/bin/helix-screen not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binaries only to $(K1_SSH_TARGET):$(K1_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock; mkdir -p $(K1_DEPLOY_DIR)/bin"
	cat build/mips/bin/helix-screen | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K1_DEPLOY_DIR)/bin/helix-screen"
	cat build/mips/bin/helix-splash | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/mips/bin/helix-watchdog ]; then \
		cat build/mips/bin/helix-watchdog | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	$(call sync-device-features,$(K1_SSH_TARGET),$(K1_DEPLOY_DIR),build/mips/bin)
	@echo "$(CYAN)Restarting helix-screen on $(K1_HOST)...$(RESET)"
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock; cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"

# Convenience: SSH into the K1
k1-ssh:
	ssh $(K1_SSH_TARGET)

# Full cycle: docker build + deploy + run in foreground
k1-test: k1-docker deploy-k1-fg

# =============================================================================
# K1 Dynamic Deployment Targets
# =============================================================================
# Same as K1 static targets but using build/k1-dynamic/ paths

.PHONY: deploy-k1-dynamic deploy-k1-dynamic-fg deploy-k1-dynamic-bin k1-dynamic-test

# Deploy full application to K1 (dynamic build) using tar/ssh
deploy-k1-dynamic:
	@test -f build/k1-dynamic/bin/helix-screen || { echo "$(RED)Error: build/k1-dynamic/bin/helix-screen not found. Run 'make k1-dynamic-docker' first.$(RESET)"; exit 1; }
	@test -f build/k1-dynamic/bin/helix-splash || { echo "$(RED)Error: build/k1-dynamic/bin/helix-splash not found. Run 'make k1-dynamic-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen (dynamic) to $(K1_SSH_TARGET):$(K1_DEPLOY_DIR)...$(RESET)"
	@# Generate pre-rendered images if missing
	@if [ ! -f build/assets/images/prerendered/splash-logo-small.bin ]; then \
		echo "$(DIM)Generating pre-rendered splash images...$(RESET)"; \
		$(MAKE) gen-images; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ] || [ -z "$$(ls -A build/assets/images/printers/prerendered/*.bin 2>/dev/null)" ]; then \
		echo "$(DIM)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/splash-3d-dark-tiny_alt.bin ]; then \
		echo "$(DIM)Generating 3D splash images for K1...$(RESET)"; \
		$(MAKE) gen-splash-3d-k1; \
	fi
	@# Stop running processes and prepare directory
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock; mkdir -p $(K1_DEPLOY_DIR)/bin"
	@# Transfer binaries via cat/ssh
	@echo "$(DIM)Transferring binaries...$(RESET)"
	cat build/k1-dynamic/bin/helix-screen | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K1_DEPLOY_DIR)/bin/helix-screen"
	cat build/k1-dynamic/bin/helix-splash | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/k1-dynamic/bin/helix-watchdog ]; then \
		cat build/k1-dynamic/bin/helix-watchdog | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	cat scripts/helix-launcher.sh | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-launcher.sh && chmod +x $(K1_DEPLOY_DIR)/bin/helix-launcher.sh"
	@# Transfer assets via tar
	@echo "$(DIM)Transferring assets...$(RESET)"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_TAR_NO_TRACKER) $(DEPLOY_ASSET_DIRS) | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && tar -xof -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(K1_SSH_TARGET) "mkdir -p $(K1_DEPLOY_DIR)/assets/images/prerendered $(K1_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR)/assets/images && tar -xof -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR)/assets/images/printers && tar -xof -"; \
	fi
	@echo "$(GREEN)✓ Deployed to $(K1_HOST):$(K1_DEPLOY_DIR)$(RESET)"
	$(call sync-device-features,$(K1_SSH_TARGET),$(K1_DEPLOY_DIR),build/k1-dynamic/bin)
	@echo "$(CYAN)Starting helix-screen on $(K1_HOST)...$(RESET)"
	ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen started in background$(RESET)"
	@echo "$(DIM)Logs: ssh $(K1_SSH_TARGET) 'tail -f /var/log/messages | grep helix'$(RESET)"

# Deploy and run in foreground with verbose logging (for interactive debugging)
deploy-k1-dynamic-fg:
	@test -f build/k1-dynamic/bin/helix-screen || { echo "$(RED)Error: build/k1-dynamic/bin/helix-screen not found. Run 'make k1-dynamic-docker' first.$(RESET)"; exit 1; }
	@test -f build/k1-dynamic/bin/helix-splash || { echo "$(RED)Error: build/k1-dynamic/bin/helix-splash not found. Run 'make k1-dynamic-docker' first.$(RESET)"; exit 1; }
	$(call deploy-common,$(K1_SSH_TARGET),$(K1_DEPLOY_DIR),build/k1-dynamic/bin)
	@echo "$(CYAN)Starting helix-screen on $(K1_HOST) (foreground, verbose)...$(RESET)"
	ssh -t $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug"

# Deploy binaries only (fast, for quick iteration)
deploy-k1-dynamic-bin:
	@test -f build/k1-dynamic/bin/helix-screen || { echo "$(RED)Error: build/k1-dynamic/bin/helix-screen not found. Run 'make k1-dynamic-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binaries only to $(K1_SSH_TARGET):$(K1_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock; mkdir -p $(K1_DEPLOY_DIR)/bin"
	cat build/k1-dynamic/bin/helix-screen | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K1_DEPLOY_DIR)/bin/helix-screen"
	cat build/k1-dynamic/bin/helix-splash | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/k1-dynamic/bin/helix-watchdog ]; then \
		cat build/k1-dynamic/bin/helix-watchdog | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	$(call sync-device-features,$(K1_SSH_TARGET),$(K1_DEPLOY_DIR),build/k1-dynamic/bin)
	@echo "$(CYAN)Restarting helix-screen on $(K1_HOST)...$(RESET)"
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock; cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"

# Full cycle: docker build + deploy + run in foreground
k1-dynamic-test: k1-dynamic-docker deploy-k1-dynamic-fg

# =============================================================================
# K2 Deployment Configuration
# =============================================================================
#
# Creality K2 series deployment (K2, K2 Pro, K2 Plus)
# Based on K1 deployment pattern but adapted for K2 differences:
#   - Tina Linux (OpenWrt-based) with procd init (not SysV)
#   - Stock Moonraker on port 4408 (no community tools needed)
#   - 32 GB storage (plenty of room)
#   - SSH: root / creality_2024
#
# Hardware-validated on K2 Plus (2026-03-23).
# See docs/devel/printers/CREALITY_K2_SUPPORT.md
#
# Example: make deploy-k2 K2_HOST=192.168.30.196
# Note: K2 uses BusyBox/OpenWrt - tar/ssh transfer, no rsync
# Note: K2 hostname does NOT resolve via mDNS - always use IP address
K2_HOST ?=
K2_USER ?= root
# Must match the installer's K2 INSTALL_DIR (scripts/lib/installer/platform.sh:
# k2 branch → /opt/helixscreen) and the init script's DAEMON_DIR set below.
# /mnt/UDISK is only KLIPPER_HOME (printer_data/config), NOT the program dir;
# deploying there left the binary where the daemon never runs it.
K2_DEPLOY_DIR ?= /opt/helixscreen

# Build SSH target for K2 (lazy evaluation — only errors when deploy targets actually use it)
K2_SSH_TARGET = $(if $(K2_HOST),$(K2_USER)@$(K2_HOST),$(error K2_HOST is required. K2 does not resolve via mDNS. Use: make deploy-k2 K2_HOST=192.168.x.x))

# =============================================================================
# K2 Deployment Targets
# =============================================================================

.PHONY: deploy-k2 deploy-k2-fg deploy-k2-bin k2-ssh k2-test

# Deploy full application to K2 using tar/ssh (K2 BusyBox has no rsync)
deploy-k2:
	@test -f build/k2/bin/helix-screen || { echo "$(RED)Error: build/k2/bin/helix-screen not found. Run 'make k2-docker' first.$(RESET)"; exit 1; }
	@test -f build/k2/bin/helix-splash || { echo "$(RED)Error: build/k2/bin/helix-splash not found. Run 'make k2-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(K2_SSH_TARGET):$(K2_DEPLOY_DIR)...$(RESET)"
	@# Generate pre-rendered images if missing
	@if [ ! -f build/assets/images/prerendered/splash-logo-small.bin ]; then \
		echo "$(DIM)Generating pre-rendered splash images...$(RESET)"; \
		$(MAKE) gen-images; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ] || [ -z "$$(ls -A build/assets/images/printers/prerendered/*.bin 2>/dev/null)" ]; then \
		echo "$(DIM)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@# Stop running processes, remove stale lock, and prepare directory
	ssh $(K2_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock; mkdir -p $(K2_DEPLOY_DIR)/bin $(K2_DEPLOY_DIR)/platform"
	@# Deploy platform hooks (stops stock display-server cleanly via procd)
	@if [ -f assets/config/platform/hooks-k2.sh ]; then \
		cat assets/config/platform/hooks-k2.sh | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/platform/hooks.sh && chmod +x $(K2_DEPLOY_DIR)/platform/hooks.sh"; \
	fi
	@# Transfer binaries via cat/ssh
	@echo "$(DIM)Transferring binaries...$(RESET)"
	cat build/k2/bin/helix-screen | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K2_DEPLOY_DIR)/bin/helix-screen"
	cat build/k2/bin/helix-splash | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K2_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/k2/bin/helix-watchdog ]; then \
		cat build/k2/bin/helix-watchdog | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K2_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	cat scripts/helix-launcher.sh | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-launcher.sh && chmod +x $(K2_DEPLOY_DIR)/bin/helix-launcher.sh"
	@# Transfer assets via tar
	@echo "$(DIM)Transferring assets...$(RESET)"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_TAR_NO_TRACKER) $(DEPLOY_ASSET_DIRS) | ssh $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR) && tar -xof -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(K2_SSH_TARGET) "mkdir -p $(K2_DEPLOY_DIR)/assets/images/prerendered $(K2_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR)/assets/images && tar -xof -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR)/assets/images/printers && tar -xof -"; \
	fi
	@# Install/update init script + procd shim for boot persistence.
	@# K2 (procd) silently skips plain SysV scripts at boot ([L086]) — only
	@# scripts with `#!/bin/sh /etc/rc.common` + DEPEND= are invoked. The
	@# shim at /etc/init.d/helixscreen is what procd's boot iterator picks up;
	@# it delegates to the SysV script. Single source of truth for the shim
	@# is config/helixscreen-k2-procd-shim.sh — also used by
	@# install_procd_shim_k2() in scripts/lib/installer/service.sh. One ssh
	@# (set -e) so any failure aborts the deploy; rc.d symlinks are verified
	@# post-enable because `enable` exits 0 even when the symlinks are wrong.
	@echo "$(DIM)Installing init script + procd shim...$(RESET)"
	@COPYFILE_DISABLE=1 tar -cf - -C config helixscreen.init helixscreen-k2-procd-shim.sh \
		| ssh $(K2_SSH_TARGET) 'set -e; \
			cd /tmp && tar -xof - && \
			cp helixscreen.init /etc/init.d/S99helixscreen && \
			chmod +x /etc/init.d/S99helixscreen && \
			sed -i "s|DAEMON_DIR=.*|DAEMON_DIR=\"/opt/helixscreen\"|" /etc/init.d/S99helixscreen && \
			cp helixscreen-k2-procd-shim.sh /etc/init.d/helixscreen && \
			chmod +x /etc/init.d/helixscreen && \
			rm -f /etc/rc.d/S99helixscreen /etc/rc.d/K01helixscreen && \
			/etc/init.d/helixscreen enable && \
			s99=$$(readlink /etc/rc.d/S99helixscreen 2>/dev/null || true); \
			k01=$$(readlink /etc/rc.d/K01helixscreen 2>/dev/null || true); \
			if [ "$$s99" != "../init.d/helixscreen" ] || [ "$$k01" != "../init.d/helixscreen" ]; then \
				echo "ERROR: rc.d symlinks not pointing at shim (S99=$$s99 K01=$$k01)" >&2; \
				exit 1; \
			fi; \
			rm -f /tmp/helixscreen.init /tmp/helixscreen-k2-procd-shim.sh; \
			echo "Init script + procd shim installed (boot symlinks verified)"'
	@# Ensure /opt/helixscreen symlink exists (points to UDISK for storage)
	@ssh $(K2_SSH_TARGET) '\
		if [ ! -e /opt/helixscreen ]; then \
			ln -s $(K2_DEPLOY_DIR) /opt/helixscreen; \
			echo "Created /opt/helixscreen symlink"; \
		fi'
	@echo "$(GREEN)✓ Deployed to $(K2_HOST):$(K2_DEPLOY_DIR)$(RESET)"
	$(call sync-device-features,$(K2_SSH_TARGET),$(K2_DEPLOY_DIR),build/k2/bin)
	@echo "$(CYAN)Starting helix-screen on $(K2_HOST)...$(RESET)"
	ssh $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen started in background$(RESET)"
	@echo "$(DIM)Logs: ssh $(K2_SSH_TARGET) 'logread -f | grep helix'$(RESET)"

# Deploy and run in foreground with verbose logging (for interactive debugging)
# Note: K2 has no rsync (BusyBox) — uses same tar/ssh transfer as deploy-k2
deploy-k2-fg: deploy-k2
	@echo "$(CYAN)Restarting helix-screen on $(K2_HOST) (foreground, verbose)...$(RESET)"
	ssh $(K2_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock"
	ssh -t $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug"

# Deploy binaries only (fast, for quick iteration)
deploy-k2-bin:
	@test -f build/k2/bin/helix-screen || { echo "$(RED)Error: build/k2/bin/helix-screen not found. Run 'make k2-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binaries only to $(K2_SSH_TARGET):$(K2_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(K2_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null || true; rm -f /tmp/helix-screen.lock; mkdir -p $(K2_DEPLOY_DIR)/bin"
	cat build/k2/bin/helix-screen | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K2_DEPLOY_DIR)/bin/helix-screen"
	cat build/k2/bin/helix-splash | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K2_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/k2/bin/helix-watchdog ]; then \
		cat build/k2/bin/helix-watchdog | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K2_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	$(call sync-device-features,$(K2_SSH_TARGET),$(K2_DEPLOY_DIR),build/k2/bin)
	@echo "$(CYAN)Restarting helix-screen on $(K2_HOST)...$(RESET)"
	ssh $(K2_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; cd $(K2_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"

# Convenience: SSH into the K2
k2-ssh:
	ssh $(K2_SSH_TARGET)

# Full cycle: docker build + deploy + run in foreground
k2-test: k2-docker deploy-k2-fg

# =============================================================================
# Release Packaging
# =============================================================================
# Creates distributable tar.gz archives for each platform
# Includes: binaries, ui_xml, config, assets (fonts/images only, no test files)

RELEASE_DIR := releases
VERSION := $(shell cat VERSION.txt 2>/dev/null || echo "dev")
# Release filenames include 'v' prefix to match git tag convention (v0.9.3)
RELEASE_VERSION := v$(VERSION)

# Top-level asset dirs to include in release tarballs, derived from the shared
# packaging manifest (scripts/gen-packaging-manifest.sh) minus test-data dirs
# that are too large to ship. This replaces the previous hand-maintained
# whitelist, which silently missed assets/config post-bfeba7c26 (v0.99.33
# regression, fix e0840a4b6) and has also been missing assets/sounds all along
# (tracker music used by snake game and settings UI).
#
# Adding a new assets/<foo>/ directory to the source tree will now ship it
# automatically. To explicitly exclude something, add it to RELEASE_ASSETS_EXCLUDE.
RELEASE_ASSETS_ALL := $(shell scripts/gen-packaging-manifest.sh 2>/dev/null | grep -E '^assets/[^/]+$$')
RELEASE_ASSETS_EXCLUDE := assets/test_gcodes assets/test_timelapse
RELEASE_ASSETS := $(filter-out $(RELEASE_ASSETS_EXCLUDE),$(RELEASE_ASSETS_ALL))

# Root-level asset FILES (directly under assets/, not in a subdirectory). The
# RELEASE_ASSETS dir loop only copies directories, so any loose file must be
# listed here explicitly. filaments.json is the unified filament catalog,
# loaded on demand by FilamentCatalog (src/printer/filament_catalog.cpp).
# (mdi-icon-metadata.json.gz is intentionally NOT shipped — dev-only
# icon-search tooling, see release-clean-assets below.)
RELEASE_ASSET_FILES := assets/filaments.json

# Clean up release assets: remove files that are compiled into the binary or dev-only
# .c font files are compiled into the binary at build time
# .ttf/.otf source fonts are only used by font regen scripts, not at runtime (~35 MB savings)
# .icns is a macOS icon format, not used on embedded targets
# mdi-icon-metadata is dev-only (icon search tooling)
# .clang-format is dev-only
define release-clean-assets
	@find $(1)/assets/fonts -name '*.c' -delete 2>/dev/null || true
	@find $(1)/assets/fonts -name '*.ttf' -delete 2>/dev/null || true
	@find $(1)/assets/fonts -name '*.otf' -delete 2>/dev/null || true
	@find $(1)/assets/fonts -name '.clang-format' -delete 2>/dev/null || true
	@find $(1)/assets -name '*.icns' -delete 2>/dev/null || true
	@find $(1)/assets -name 'mdi-icon-metadata.json.gz' -delete 2>/dev/null || true
	@# assets/sounds is 919 KB of MOD/MED tracker modules, playable only where
	@# the tracker player is compiled in. TRACKER_CXXFLAGS (Makefile, sound
	@# section) is the same switch that gates that code, so the payload and the
	@# player can't drift apart. AD5M has sound but deliberately no tracker --
	@# its single core busy-waits and kills prints -- and CC1/K1/K2/MIPS have
	@# neither, so all of them were shipping music they can never play.
	$(if $(TRACKER_CXXFLAGS),,@rm -rf $(1)/assets/sounds)
endef

# PII / runtime-state files that must NEVER ship in a release tarball.
# Personal config (settings.json, helixconfig*.json) is handled per-target
# because some targets ship a curated default. This list is for things that
# get auto-generated at runtime and have no business in a public release.
# Keep in sync with DEPLOY_RUNTIME_EXCLUDES (~ line 1388).
define release-strip-pii
	@rm -f $(1)/config/telemetry_device.json \
	       $(1)/config/telemetry_queue.json \
	       $(1)/config/tool_spools.json \
	       $(1)/config/filament_slot_overrides.json \
	       $(1)/config/crash_report.txt \
	       $(1)/config/crash_history.json \
	       $(1)/config/feedback_queue.json \
	       $(1)/config/.helix-screen.lock \
	       2>/dev/null || true
endef

# Developer showcase panels (see ENABLE_DEV_PANELS in Makefile). Their .cpp files
# are filtered out of APP_SRCS and their XML registrations compiled out for every
# cross target, so these layouts are unreachable in a release build — nothing
# registers or instantiates them. Ship the asset half gated the same way the code
# half is.
#
# Unconditional on purpose: release-* recipes run in the top-level (native) make,
# where ENABLE_DEV_PANELS defaults to yes, while the binaries they package come
# from separate cross sub-makes where it defaults to no. Keying the strip off the
# ambient $(ENABLE_DEV_PANELS) would therefore never fire.
DEV_PANEL_XML := gcode_test_panel.xml glyphs_panel.xml step_test_panel.xml test_panel.xml

# Stage ui_xml/ + config/ into a release tree, minus the dev-panel layouts.
# Args: $(1) = staged release root (e.g. $(RELEASE_DIR)/helixscreen)
define release-copy-xml-config
	@cp -r ui_xml config $(1)/
	@rm -f $(addprefix $(1)/ui_xml/,$(DEV_PANEL_XML))
	@# Minify the STAGED copy only -- never ui_xml/ in the source tree. The XML
	@# engine keeps a verbatim copy of every component's <view> source text alive
	@# for the whole session (lv_xml_component.c extract_view_content: it is
	@# re-parsed on each lv_xml_create, so it cannot be freed), which makes shipped
	@# comments and indentation permanently resident heap on every device. Fails
	@# loudly rather than shipping unminified: this is not an optional nicety on
	@# the 114 MB boards, and a silent skip is how the FONT_TIERS trim went
	@# unnoticed for months.
	@python3 scripts/minify_xml_tree.py $(1)/ui_xml
endef

# Bake release_info.json (consumed by Moonraker's type:web self-update) into the
# staged release tree at $(RELEASE_DIR)/helixscreen. The asset_name is resolved
# through helix_self_update_asset() in scripts/lib/installer/platform.sh — the
# single source of truth shared with the installer's runtime fallback — so the
# baked value and the fallback can't diverge and send Moonraker after the wrong
# (or non-zip) asset (prestonbrown/helixscreen#993).
# Args: $(1) = platform key (pi, pi32, ad5m, ad5x, cc1, k1, k1-dynamic, k2,
#              snapmaker-u1, x86)
define write-release-info
	@asset=$$(. scripts/lib/installer/platform.sh >/dev/null 2>&1 && helix_self_update_asset $(1)); \
	echo "{\"project_name\":\"helixscreen\",\"project_owner\":\"prestonbrown\",\"version\":\"$(RELEASE_VERSION)\",\"asset_name\":\"$$asset\"}" > $(RELEASE_DIR)/helixscreen/release_info.json
endef

.PHONY: release-pi release-pi32 release-ad5m release-k1 release-ad5x release-k1-dynamic release-k2 release-snapmaker-u1 release-x86 release-all release-clean pi-fbdev-docker pi32-fbdev-docker pi-all-docker pi32-all-docker x86-fbdev-docker x86-all-docker

# Package Pi release
release-pi: | build/pi/bin/helix-screen build/pi/bin/helix-splash build/pi-fbdev/bin/helix-screen
	@echo "$(CYAN)$(BOLD)Packaging Pi release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/pi/bin/helix-screen build/pi/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/pi/bin/helix-watchdog ]; then cp build/pi/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@if [ -f build/pi/lib/libhelix-bluetooth.so ]; then cp build/pi/lib/libhelix-bluetooth.so $(RELEASE_DIR)/helixscreen/bin/; fi
	@if [ -f build/pi-fbdev/bin/helix-screen ]; then cp build/pi-fbdev/bin/helix-screen $(RELEASE_DIR)/helixscreen/bin/helix-screen-fbdev; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings.json $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@# Copy pre-rendered images from build directory (splash + printer images)
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,pi)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-pi.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-pi-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-pi-$(RELEASE_VERSION).tar.gz + helixscreen-pi.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-pi-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-pi.zip

# Package Pi 32-bit release (same structure as 64-bit Pi)
release-pi32: | build/pi32/bin/helix-screen build/pi32/bin/helix-splash build/pi32-fbdev/bin/helix-screen
	@echo "$(CYAN)$(BOLD)Packaging Pi 32-bit release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/pi32/bin/helix-screen build/pi32/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/pi32/bin/helix-watchdog ]; then cp build/pi32/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@if [ -f build/pi32/lib/libhelix-bluetooth.so ]; then cp build/pi32/lib/libhelix-bluetooth.so $(RELEASE_DIR)/helixscreen/bin/; fi
	@if [ -f build/pi32-fbdev/bin/helix-screen ]; then cp build/pi32-fbdev/bin/helix-screen $(RELEASE_DIR)/helixscreen/bin/helix-screen-fbdev; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings.json $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@# Copy pre-rendered images from build directory (splash + printer images)
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,pi32)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-pi32.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-pi32-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-pi32-$(RELEASE_VERSION).tar.gz + helixscreen-pi32.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-pi32-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-pi32.zip

# Package AD5M release
# Note: AD5M uses BusyBox which doesn't support tar -z, so we create uncompressed tar + gzip separately
# Includes pre-configured settings.json for Adventurer 5M Pro (skips setup wizard)
release-ad5m: | build/ad5m/bin/helix-screen build/ad5m/bin/helix-splash
	@echo "$(CYAN)$(BOLD)Packaging AD5M release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/ad5m/bin/helix-screen build/ad5m/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/ad5m/bin/helix-watchdog ]; then cp build/ad5m/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@# Copy AD5M Pro default config as config/settings.json (skips wizard on first run)
	@cp assets/config/presets/ad5m.json $(RELEASE_DIR)/helixscreen/config/settings.json
	@echo "  $(DIM)Included pre-configured config/settings.json for AD5M Pro$(RESET)"
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@# Copy pre-rendered images from build directory (splash + printer images)
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@# Bundle CA certificates for HTTPS verification (fallback if device lacks system certs)
	@if [ -f "build/ad5m/certs/ca-certificates.crt" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/certs; \
		cp build/ad5m/certs/ca-certificates.crt $(RELEASE_DIR)/helixscreen/certs/; \
		echo "  $(DIM)Included CA certificates for HTTPS$(RESET)"; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,ad5m)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-ad5m.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-ad5m-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-ad5m-$(RELEASE_VERSION).tar.gz + helixscreen-ad5m.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-ad5m-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-ad5m.zip

# Package AD5X release
release-ad5x: | build/ad5x/bin/helix-screen build/ad5x/bin/helix-splash
	@echo "$(CYAN)$(BOLD)Packaging AD5X release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/ad5x/bin/helix-screen build/ad5x/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/ad5x/bin/helix-watchdog ]; then cp build/ad5x/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Install AD5X preset as default config (skips hardware wizard on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp assets/config/presets/ad5x.json $(RELEASE_DIR)/helixscreen/config/settings.json
	@echo "  $(DIM)Included pre-configured config/settings.json for AD5X$(RESET)"
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@# Copy pre-rendered images from build directory (splash + printer images)
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@# Bundle CA certificates for HTTPS verification (fallback if device lacks system certs)
	@if [ -f "build/ad5x/certs/ca-certificates.crt" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/certs; \
		cp build/ad5x/certs/ca-certificates.crt $(RELEASE_DIR)/helixscreen/certs/; \
		echo "  $(DIM)Included CA certificates for HTTPS$(RESET)"; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,ad5x)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-ad5x.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-ad5x-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-ad5x-$(RELEASE_VERSION).tar.gz + helixscreen-ad5x.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-ad5x-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-ad5x.zip

# Package CC1 release
release-cc1: | build/cc1/bin/helix-screen build/cc1/bin/helix-splash
	@echo "$(CYAN)$(BOLD)Packaging CC1 release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/cc1/bin/helix-screen build/cc1/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/cc1/bin/helix-watchdog ]; then cp build/cc1/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Install CC1 preset as default config (skips hardware wizard on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp assets/config/presets/cc1.json $(RELEASE_DIR)/helixscreen/config/settings.json
	@echo "  $(DIM)Included pre-configured config/settings.json for CC1$(RESET)"
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@# Copy pre-rendered images from build directory (splash + printer images)
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@# Bundle CA certificates for HTTPS verification (fallback if device lacks system certs)
	@if [ -f "build/cc1/certs/ca-certificates.crt" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/certs; \
		cp build/cc1/certs/ca-certificates.crt $(RELEASE_DIR)/helixscreen/certs/; \
		echo "  $(DIM)Included CA certificates for HTTPS$(RESET)"; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,cc1)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-cc1.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-cc1-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-cc1-$(RELEASE_VERSION).tar.gz + helixscreen-cc1.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-cc1-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-cc1.zip

# Package K1 release
release-k1: | build/mips/bin/helix-screen build/mips/bin/helix-splash
	@echo "$(CYAN)$(BOLD)Packaging K1 release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/mips/bin/helix-screen build/mips/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/mips/bin/helix-watchdog ]; then cp build/mips/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Install K1 preset as default config (skips hardware wizard on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp assets/config/presets/k1.json $(RELEASE_DIR)/helixscreen/config/settings.json
	@echo "  $(DIM)Included pre-configured config/settings.json for K1$(RESET)"
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@# Copy pre-rendered images from build directory (splash + printer images)
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@# Bundle CA certificates for HTTPS verification (fallback if device lacks system certs)
	@if [ -f "build/mips/certs/ca-certificates.crt" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/certs; \
		cp build/mips/certs/ca-certificates.crt $(RELEASE_DIR)/helixscreen/certs/; \
		echo "  $(DIM)Included CA certificates for HTTPS$(RESET)"; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,k1)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-k1.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-k1-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-k1-$(RELEASE_VERSION).tar.gz + helixscreen-k1.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-k1-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-k1.zip

# Package K1 Dynamic release
release-k1-dynamic: | build/k1-dynamic/bin/helix-screen build/k1-dynamic/bin/helix-splash
	@echo "$(CYAN)$(BOLD)Packaging K1 Dynamic release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/k1-dynamic/bin/helix-screen build/k1-dynamic/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/k1-dynamic/bin/helix-watchdog ]; then cp build/k1-dynamic/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings.json $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,k1-dynamic)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-k1-dynamic.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-k1-dynamic-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-k1-dynamic-$(RELEASE_VERSION).tar.gz + helixscreen-k1-dynamic.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-k1-dynamic-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-k1-dynamic.zip

# Package K2 release
release-k2: | build/k2/bin/helix-screen build/k2/bin/helix-splash
	@echo "$(CYAN)$(BOLD)Packaging K2 release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/k2/bin/helix-screen build/k2/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/k2/bin/helix-watchdog ]; then cp build/k2/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@# Static MJPEG camera server. Built by `make ustreamer-k2` (cached, idempotent);
	@# trigger it here so a clean release always bundles it. Ships at bin/ustreamer.
	@$(MAKE) --no-print-directory ustreamer-k2
	@if [ -f build/k2/bin/ustreamer ]; then \
		cp build/k2/bin/ustreamer $(RELEASE_DIR)/helixscreen/bin/; \
		echo "  $(DIM)Bundled static ustreamer (MJPEG camera server) at bin/ustreamer$(RESET)"; \
	else \
		echo "$(YELLOW)  WARNING: build/k2/bin/ustreamer missing — release will not include the camera server$(RESET)"; \
	fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Install K2 preset as default config (skips hardware wizard on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp assets/config/presets/k2.json $(RELEASE_DIR)/helixscreen/config/settings.json
	@echo "  $(DIM)Included pre-configured config/settings.json for K2$(RESET)"
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,k2)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-k2.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-k2-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-k2-$(RELEASE_VERSION).tar.gz + helixscreen-k2.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-k2-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-k2.zip

# Package Snapmaker U1 release
release-snapmaker-u1: | build/snapmaker-u1/bin/helix-screen
	@echo "$(CYAN)$(BOLD)Packaging Snapmaker U1 release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/snapmaker-u1/bin/helix-screen $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/snapmaker-u1/bin/helix-splash ]; then cp build/snapmaker-u1/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/ 2>/dev/null || true
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Install Snapmaker U1 preset as default config (skips hardware wizard on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp assets/config/presets/snapmaker_u1.json $(RELEASE_DIR)/helixscreen/config/settings.json
	@echo "  $(DIM)Included pre-configured config/settings.json for Snapmaker U1$(RESET)"
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/ 2>/dev/null || true
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME) 2>/dev/null || true
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/ 2>/dev/null || true
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/ 2>/dev/null || true
	@cp scripts/snapmaker-u1-setup-autostart.sh $(RELEASE_DIR)/helixscreen/scripts/ 2>/dev/null || true
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@if [ -f "build/snapmaker-u1/certs/ca-certificates.crt" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/certs; \
		cp build/snapmaker-u1/certs/ca-certificates.crt $(RELEASE_DIR)/helixscreen/certs/; \
		echo "  $(DIM)Included CA certificates for HTTPS$(RESET)"; \
	fi
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,snapmaker-u1)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-snapmaker-u1.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-snapmaker-u1-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-snapmaker-u1-$(RELEASE_VERSION).tar.gz + helixscreen-snapmaker-u1.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-snapmaker-u1-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-snapmaker-u1.zip

# Package x86_64 Debian release (same structure as Pi)
release-x86: | build/x86/bin/helix-screen build/x86/bin/helix-splash build/x86-fbdev/bin/helix-screen
	@echo "$(CYAN)$(BOLD)Packaging x86 release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/x86/bin/helix-screen build/x86/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/x86/bin/helix-watchdog ]; then cp build/x86/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@if [ -f build/x86/lib/libhelix-bluetooth.so ]; then cp build/x86/lib/libhelix-bluetooth.so $(RELEASE_DIR)/helixscreen/bin/; fi
	@if [ -f build/x86-fbdev/bin/helix-screen ]; then cp build/x86-fbdev/bin/helix-screen $(RELEASE_DIR)/helixscreen/bin/helix-screen-fbdev; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	$(call release-copy-xml-config,$(RELEASE_DIR)/helixscreen)
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/settings.json $(RELEASE_DIR)/helixscreen/config/settings-test.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	$(call release-strip-pii,$(RELEASE_DIR)/helixscreen)
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@cp -r scripts/kiauh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@for f in $(RELEASE_ASSET_FILES); do \
		if [ -f "$$f" ]; then cp "$$f" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@# Copy pre-rendered images from build directory (splash + printer images)
	@if [ -d "build/assets/images/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/prerendered; \
		cp -r build/assets/images/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/prerendered/; \
	fi
	@if [ -d "build/assets/images/printers/prerendered" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered; \
		cp -r build/assets/images/printers/prerendered/* $(RELEASE_DIR)/helixscreen/assets/images/printers/prerendered/; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	$(call write-release-info,x86)
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-x86.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf helixscreen-x86-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-x86-$(RELEASE_VERSION).tar.gz + helixscreen-x86.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-x86-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-x86.zip

# Package all releases
release-all: release-pi release-pi32 release-ad5m release-cc1 release-k1 release-ad5x release-k1-dynamic release-k2 release-snapmaker-u1 release-x86
	@echo "$(GREEN)$(BOLD)✓ All releases packaged in $(RELEASE_DIR)/$(RESET)"
	@ls -lh $(RELEASE_DIR)/*.tar.gz $(RELEASE_DIR)/*.zip

# Clean release artifacts
release-clean:
	@rm -rf $(RELEASE_DIR)
	@echo "$(GREEN)✓ Release directory cleaned$(RESET)"

# Aliases for package-* — trigger the full build + package workflow.
# The legacy scripts/package.sh wrapper was deleted; these targets are now
# the single entry point for building release artifacts.
.PHONY: package-ad5m package-cc1 package-pi package-pi32 package-k1 package-ad5x package-k1-dynamic package-k2 package-snapmaker-u1 package-x86 package-all package-clean
package-ad5m: ad5m-docker gen-images-ad5m gen-splash-3d-ad5m gen-printer-images release-ad5m
package-cc1: cc1-docker gen-images gen-printer-images release-cc1
package-pi: pi-all-docker gen-images gen-splash-3d gen-printer-images release-pi
package-pi32: pi32-all-docker gen-images gen-splash-3d gen-printer-images release-pi32
package-k1: mips-docker gen-images gen-splash-3d-k1 gen-printer-images release-k1
package-ad5x: mips-docker gen-images gen-splash-3d-k1 gen-printer-images release-ad5x
package-k1-dynamic: k1-dynamic-docker gen-images gen-splash-3d-k1 gen-printer-images release-k1-dynamic
package-k2: k2-docker gen-images gen-printer-images release-k2
package-snapmaker-u1: snapmaker-u1-docker gen-images gen-printer-images release-snapmaker-u1
package-x86: x86-all-docker gen-images gen-splash-3d gen-printer-images release-x86
package-all: package-ad5m package-cc1 package-pi package-pi32 package-k1 package-ad5x package-k1-dynamic package-k2 package-snapmaker-u1 package-x86
package-clean: release-clean

# Convenience aliases (verb-target → target-verb)
.PHONY: pi-deploy ad5m-deploy
pi-deploy: deploy-pi
ad5m-deploy: deploy-ad5m
