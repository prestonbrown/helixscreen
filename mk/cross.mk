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
#   make PLATFORM_TARGET=ad5m  # Cross-compile for Adventurer 5M (armv7-a)
#   make PLATFORM_TARGET=cc1   # Cross-compile for Centauri Carbon 1 (armv7-a)
#   make PLATFORM_TARGET=mips  # Cross-compile for MIPS32 devices (K1)
#   make PLATFORM_TARGET=k1    # Alias for mips (Creality K1 series)
#   make PLATFORM_TARGET=ad5x  # Cross-compile for FlashForge AD5X (mips)
#   make PLATFORM_TARGET=k2    # Cross-compile for Creality K2 series (ARM)
#   make PLATFORM_TARGET=snapmaker-u1 # Cross-compile for Snapmaker U1 (aarch64)
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
    TARGET_CFLAGS := -march=armv8-a -funwind-tables -I/usr/aarch64-linux-gnu/include -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_BINARY_VARIANT=\"drm\"
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

else ifeq ($(PLATFORM_TARGET),pi-fbdev)
    # -------------------------------------------------------------------------
    # Raspberry Pi (aarch64) - fbdev only, no GL dependencies
    # Fallback for systems without EGL/GLES2/GBM libraries.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    TARGET_CFLAGS := -march=armv8-a -funwind-tables \
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

else ifeq ($(PLATFORM_TARGET),pi-both)
    # -------------------------------------------------------------------------
    # Raspberry Pi (aarch64) - Dual-link mode: compile once, link DRM + fbdev
    # Produces both build/pi/bin/helix-screen (DRM) and build/pi-fbdev/bin/helix-screen (fbdev)
    # in a single compilation pass. Used by CI to cut build time in half.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    TARGET_CFLAGS := -march=armv8-a -funwind-tables -I/usr/aarch64-linux-gnu/include -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_BINARY_VARIANT=\"drm\"
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
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -funwind-tables \
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

else ifeq ($(PLATFORM_TARGET),pi32-fbdev)
    # -------------------------------------------------------------------------
    # Raspberry Pi 32-bit (armhf) - fbdev only, no GL dependencies
    # Fallback for systems without EGL/GLES2/GBM libraries.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-linux-gnueabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -funwind-tables \
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

else ifeq ($(PLATFORM_TARGET),pi32-both)
    # -------------------------------------------------------------------------
    # Raspberry Pi 32-bit (armhf) - Dual-link mode: compile once, link DRM + fbdev
    # Produces both build/pi32/bin/helix-screen (DRM) and build/pi32-fbdev/bin/helix-screen (fbdev)
    # in a single compilation pass. Used by CI to cut build time in half.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-linux-gnueabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -funwind-tables \
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
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_AD5M
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
    TARGET_CFLAGS := -march=mips32r5 -mtune=mips32r5 -mabi=32 -mnan=2008 -mfp64 \
        -Os -flto -ffunction-sections -fdata-sections \
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
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a7 \
        -Os -flto -ffunction-sections -fdata-sections \
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
    # Strip binary for size on memory-constrained device
    STRIP_BINARY := yes

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
    # -fno-unwind-tables -fno-asynchronous-unwind-tables: Remove exception tables
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
        -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables \
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
    TARGET_CFLAGS := -march=mips32r2 -mtune=mips32r2 -mnan=2008 -mfp64 \
        -Os -ffunction-sections -fdata-sections \
        -fomit-frame-pointer \
        -isystem include/compat \
        -Wno-error=conversion -Wno-error=sign-conversion \
        -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_K1
    # Dynamic linking with NaN2008 dynamic linker
    # NO -static flag! System libs resolved at runtime on the K1.
    TARGET_LDFLAGS := -Wl,--gc-sections -Wl,-O2 -Wl,--as-needed \
        -Wl,--dynamic-linker=/lib/ld-linux-mipsn8.so.1
    ENABLE_SSL := no
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := k1-dynamic
    STRIP_BINARY := yes

else ifeq ($(PLATFORM_TARGET),k2)
    # -------------------------------------------------------------------------
    # Creality K2 Series - Allwinner A133 (ARM Cortex-A53)
    # Specs: 480x800 display (portrait), ~512MB RAM, musl libc (Tina Linux)
    # -------------------------------------------------------------------------
    # FULLY STATIC BUILD with musl: Same proven strategy as K1 target.
    # Uses Bootlin's armv7-eabihf musl toolchain (32-bit ARM hard-float).
    #
    # We target armv7 despite the A53 being aarch64-capable because Tina Linux
    # (OpenWrt) commonly uses 32-bit userland, and entware packages are armv7sf.
    #
    # UNTESTED: Based on research. See docs/printer-research/CREALITY_K2_PLUS_RESEARCH.md
    # Key unknowns: actual ARM variant (armv7 vs aarch64), libc, fb orientation
    CROSS_COMPILE ?= arm-buildroot-linux-musleabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-buildroot-linux-musleabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
        -Os -flto=auto -ffunction-sections -fdata-sections \
        -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -fmerge-all-constants -fno-ident \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_K2
    TARGET_LDFLAGS := -Wl,--gc-sections -Wl,-O2 -Wl,--as-needed -flto=auto -static
    # SSL disabled - Moonraker is local on port 4408 (stock firmware)
    ENABLE_SSL := no
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := k2
    STRIP_BINARY := yes

else ifeq ($(PLATFORM_TARGET),snapmaker-u1)
    # -------------------------------------------------------------------------
    # Snapmaker U1 - Rockchip ARM64 (aarch64)
    # Specs: 480x320 display, Debian Trixie, glibc
    # -------------------------------------------------------------------------
    # FULLY STATIC BUILD: Avoids glibc version mismatches across Debian versions.
    # Uses same aarch64-linux-gnu toolchain as Pi but with static linking.
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    TARGET_CFLAGS := -march=armv8-a -Os -flto -ffunction-sections -fdata-sections \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_SNAPMAKER_U1
    TARGET_LDFLAGS := -Wl,--gc-sections -flto -static
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := no
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := snapmaker-u1
    STRIP_BINARY := yes

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
    $(error Unknown PLATFORM_TARGET: $(PLATFORM_TARGET). Valid options: native, pi, pi32, ad5m, cc1, mips, k1, ad5x, k1-dynamic, k2, snapmaker-u1)
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

    # Override build directories for cross-compilation
    ifneq ($(BUILD_SUBDIR),)
        BUILD_DIR := build/$(BUILD_SUBDIR)
        BIN_DIR := $(BUILD_DIR)/bin
        OBJ_DIR := $(BUILD_DIR)/obj
    endif

    # Print cross-compilation info
    $(info )
    $(info ========================================)
    $(info Cross-compiling for: $(PLATFORM_TARGET))
    $(info Architecture: $(TARGET_ARCH))
    $(info Compiler: $(CC))
    $(info Output: $(BUILD_DIR))
    $(info ========================================)
    $(info )
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

.PHONY: pi pi-both pi32 pi32-both ad5m cc1 mips k1 ad5x k1-dynamic k2 snapmaker-u1 pi-docker pi32-docker ad5m-docker cc1-docker mips-docker k1-docker ad5x-docker k1-dynamic-docker k2-docker snapmaker-u1-docker docker-toolchains docker-toolchain-snapmaker-u1 cross-info ensure-docker ensure-buildx maybe-stop-colima

# Persistent ccache for Docker builds — bind-mounts a host directory so the
# cache survives across container runs (the container is --rm).  Per-platform
# subdirectories avoid cross-toolchain collisions.  The host dir is created
# on first use with the current user's ownership (no root permission issues).
DOCKER_CCACHE_BASE ?= $(HOME)/.cache/helixscreen-ccache
docker-ccache-args = -v "$(DOCKER_CCACHE_BASE)/$(1)":/ccache -e CCACHE_DIR=/ccache
ensure-ccache-dir = @mkdir -p "$(DOCKER_CCACHE_BASE)/$(1)"

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
	@echo "$(CYAN)$(BOLD)Cross-compiling for Creality K2 series (ARM Cortex-A53)...$(RESET)"
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
	$(Q)docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,pi) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi-fbdev-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi fbdev fallback via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi-fbdev)
	$(Q)docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,pi-fbdev) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi-fbdev SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi-all-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi (DRM + fbdev) via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi)
	$(Q)docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,pi) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi-both SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi 32-bit via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32)
	$(Q)docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,pi32) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32 SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-fbdev-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi 32-bit fbdev fallback via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32-fbdev)
	$(Q)docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,pi32-fbdev) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32-fbdev SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-all-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi 32-bit (DRM + fbdev) via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32)
	$(Q)docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,pi32) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32-both SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@$(MAKE) --no-print-directory maybe-stop-colima

ad5m-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Adventurer 5M via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-ad5m >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-ad5m; \
	fi
	$(call ensure-ccache-dir,ad5m)
	$(Q)docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,ad5m) helixscreen/toolchain-ad5m \
		make PLATFORM_TARGET=ad5m SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
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
	$(Q)docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,ad5x) helixscreen/toolchain-ad5x \
		make PLATFORM_TARGET=ad5x SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
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
	$(call ensure-ccache-dir,cc1)
	$(Q)docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,cc1) helixscreen/toolchain-cc1 \
		make PLATFORM_TARGET=cc1 SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@# Extract CA certificates from Docker image for HTTPS verification on device
	@mkdir -p build/cc1/certs
	@docker run --rm helixscreen/toolchain-cc1 cat /etc/ssl/certs/ca-certificates.crt > build/cc1/certs/ca-certificates.crt 2>/dev/null \
		&& echo "$(GREEN)✓ CA certificates extracted$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not extract CA certificates (HTTPS may rely on device certs)$(RESET)"
	@$(MAKE) --no-print-directory maybe-stop-colima

mips-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for MIPS32 devices via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-k1 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-k1; \
	fi
	$(call ensure-ccache-dir,k1)
	# Do not inherit host jobserver flags into containerized make.
	$(Q)docker run --rm --user $$(id -u):$$(id -g) -e MAKEFLAGS= -v "$(PWD)":/src -w /src $(call docker-ccache-args,k1) helixscreen/toolchain-k1 \
		make PLATFORM_TARGET=mips SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
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
	$(Q)docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,k1-dynamic) helixscreen/toolchain-k1-dynamic \
		make PLATFORM_TARGET=k1-dynamic SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@$(MAKE) --no-print-directory maybe-stop-colima

k2-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Creality K2 series via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-k2 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-k2; \
	fi
	$(call ensure-ccache-dir,k2)
	$(Q)docker run --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,k2) helixscreen/toolchain-k2 \
		make PLATFORM_TARGET=k2 SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@$(MAKE) --no-print-directory maybe-stop-colima

snapmaker-u1-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Snapmaker U1 via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-snapmaker-u1 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-snapmaker-u1; \
	fi
	$(call ensure-ccache-dir,snapmaker-u1)
	$(Q)docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(PWD)":/src -w /src $(call docker-ccache-args,snapmaker-u1) helixscreen/toolchain-snapmaker-u1 \
		make PLATFORM_TARGET=snapmaker-u1 SKIP_OPTIONAL_DEPS=1 -j$$(nproc)
	@# Extract CA certificates from Docker image for HTTPS verification on device
	@mkdir -p build/snapmaker-u1/certs
	@docker run --rm helixscreen/toolchain-snapmaker-u1 cat /etc/ssl/certs/ca-certificates.crt > build/snapmaker-u1/certs/ca-certificates.crt 2>/dev/null \
		&& echo "$(GREEN)✓ CA certificates extracted$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not extract CA certificates (HTTPS may rely on device certs)$(RESET)"
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
docker-toolchains: docker-toolchain-pi docker-toolchain-pi32 docker-toolchain-ad5m docker-toolchain-ad5x docker-toolchain-cc1 docker-toolchain-k1 docker-toolchain-k1-dynamic docker-toolchain-k2 docker-toolchain-snapmaker-u1
	@echo "$(GREEN)$(BOLD)All Docker toolchains built successfully$(RESET)"

docker-toolchain-pi: ensure-buildx
	@echo "$(CYAN)Building Raspberry Pi toolchain Docker image...$(RESET)"
	$(Q)docker buildx build --platform linux/amd64 -t helixscreen/toolchain-pi -f docker/Dockerfile.pi docker/

docker-toolchain-pi32: ensure-buildx
	@echo "$(CYAN)Building Raspberry Pi 32-bit toolchain Docker image...$(RESET)"
	$(Q)docker buildx build --platform linux/amd64 -t helixscreen/toolchain-pi32 -f docker/Dockerfile.pi32 docker/

docker-toolchain-ad5m: ensure-buildx
	@echo "$(CYAN)Building Adventurer 5M toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-ad5m -f docker/Dockerfile.ad5m docker/

docker-toolchain-ad5x: ensure-buildx
	@echo "$(CYAN)Building Adventurer 5X toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-ad5x -f docker/Dockerfile.ad5x docker/

docker-toolchain-cc1: ensure-buildx
	@echo "$(CYAN)Building Centauri Carbon 1 toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-cc1 -f docker/Dockerfile.cc1 docker/

docker-toolchain-k1: ensure-buildx
	@echo "$(CYAN)Building MIPS32 K1 toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-k1 -f docker/Dockerfile.k1 docker/

docker-toolchain-k1-dynamic: ensure-buildx
	@echo "$(CYAN)Building Creality K1 series (dynamic) toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-k1-dynamic -f docker/Dockerfile.k1-dynamic docker/

docker-toolchain-k2: ensure-buildx
	@echo "$(CYAN)Building Creality K2 series toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-k2 -f docker/Dockerfile.k2 docker/

docker-toolchain-snapmaker-u1: ensure-buildx
	@echo "$(CYAN)Building Snapmaker U1 toolchain Docker image...$(RESET)"
	$(Q)docker buildx build --platform linux/amd64 -t helixscreen/toolchain-snapmaker-u1 -f docker/Dockerfile.snapmaker-u1 docker/

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
	echo "  $${G}docker-toolchains$${X}    - Build all Docker toolchain images"; \
	echo "  $${G}docker-toolchain-pi$${X}  - Build Pi toolchain image only"; \
	echo "  $${G}docker-toolchain-pi32$${X} - Build Pi 32-bit toolchain image only"; \
	echo "  $${G}docker-toolchain-ad5m$${X} - Build AD5M toolchain image only"; \
	echo "  $${G}docker-toolchain-cc1$${X}  - Build CC1 toolchain image only"; \
	echo "  $${G}docker-toolchain-k1$${X}  - Build K1 static toolchain image only"; \
	echo "  $${G}docker-toolchain-k1-dynamic$${X} - Build K1 dynamic toolchain image only"; \
	echo "  $${G}docker-toolchain-k2$${X}  - Build K2 toolchain image only"; \
	echo "  $${G}docker-toolchain-snapmaker-u1$${X} - Build Snapmaker U1 toolchain image only"; \
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
	echo "$${C}K1 Deployment - Static (UNTESTED):$${X}"; \
	echo "  $${G}deploy-k1$${X}            - Deploy and restart in background"; \
	echo "  $${G}deploy-k1-fg$${X}         - Deploy and run in foreground (debug)"; \
	echo "  $${G}deploy-k1-bin$${X}        - Deploy binaries only (fast iteration)"; \
	echo "  $${G}k1-test$${X}              - Full cycle: docker build + deploy + run (fg)"; \
	echo "  $${G}k1-ssh$${X}               - SSH into the K1"; \
	echo ""; \
	echo "$${C}K1 Deployment - Dynamic (UNTESTED):$${X}"; \
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
	echo "  $${Y}SNAPMAKER_U1_USER$${X}=user     - Snapmaker U1 username (default: lava)"; \
	echo "  $${Y}SNAPMAKER_U1_DEPLOY_DIR$${X}=path - Snapmaker U1 deploy directory (default: /opt/helixscreen)"; \
	echo ""; \
	echo "$${C}Current Configuration:$${X}"; \
	echo "  Platform target: $(PLATFORM_TARGET)"; \
	echo "  Display backend: $(DISPLAY_BACKEND)"; \
	echo "  SDL enabled: $(ENABLE_SDL)"

# =============================================================================
# Common Deployment Settings
# =============================================================================

# Rsync flags for asset sync: delete stale files, checksum-based skip, exclude junk
DEPLOY_RSYNC_FLAGS := -avz --delete --checksum
DEPLOY_ASSET_EXCLUDES := --exclude='test_gcodes' --exclude='gcode' --exclude='.DS_Store' --exclude='*.pyc' --exclude='helixconfig*.json' --exclude='.claude-recall' --exclude='._*' \
	--exclude='assets/fonts/*.c' --exclude='*.icns' --exclude='mdi-icon-metadata.json.gz' --exclude='moonraker-plugin/tests'
# Tar-compatible excludes (same patterns, different syntax)
DEPLOY_TAR_EXCLUDES := --exclude='test_gcodes' --exclude='gcode' --exclude='.DS_Store' --exclude='*.pyc' --exclude='helixconfig*.json' --exclude='.claude-recall' --exclude='._*' \
	--exclude='assets/fonts/*.c' --exclude='*.icns' --exclude='mdi-icon-metadata.json.gz' --exclude='moonraker-plugin/tests'
DEPLOY_ASSET_DIRS := ui_xml assets config moonraker-plugin

# Common deploy recipe (called with: $(call deploy-common,SSH_TARGET,DEPLOY_DIR,BIN_DIR))
# Usage: $(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi/bin)
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
	@if [ ! -f build/assets/images/prerendered/thumbnail-placeholder-160.bin ]; then \
		echo "$(DIM)Generating placeholder images...$(RESET)"; \
		$(MAKE) gen-placeholder-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/splash-3d-dark-small.bin ]; then \
		echo "$(DIM)Generating 3D splash images...$(RESET)"; \
		$(MAKE) gen-splash-3d; \
	fi
	@# Stop running processes and prepare directory
	@# Stop systemd service first (prevents respawning), then kill any stragglers
	ssh $(1) "sudo systemctl stop helixscreen 2>/dev/null; systemctl --user stop helix-screen 2>/dev/null; killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 0.5; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null; while pidof helix-screen helix-splash helix-watchdog >/dev/null 2>&1; do sleep 0.2; done; true"
	ssh $(1) "mkdir -p $(2)/bin"
	ssh $(1) "rm -f $(2)/*.xml 2>/dev/null || true"
	@# Sync binaries and launcher to bin/
	rsync -avz --progress $(3)/helix-screen $(3)/helix-splash $(1):$(2)/bin/
	@if [ -f $(3)/helix-watchdog ]; then rsync -avz $(3)/helix-watchdog $(1):$(2)/bin/; fi
	rsync -avz scripts/helix-launcher.sh $(1):$(2)/bin/
	@# Sync installer script (needed for auto-updates)
	rsync -avz scripts/$(INSTALLER_FILENAME) $(1):$(2)/
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
	@# Prefer systemd restart (picks up Environment= vars like HELIX_DISPLAY_BACKEND)
	@ssh $(PI_SSH_TARGET) "sudo systemctl restart helixscreen 2>/dev/null" \
		|| ssh $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && setsid ./bin/helix-launcher.sh </dev/null >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"
	@echo "$(DIM)Logs: ssh $(PI_SSH_TARGET) 'journalctl -u helixscreen -f'$(RESET)"

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
	rsync -avz --progress build/pi32/bin/helix-screen build/pi32/bin/helix-splash $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/bin/
	@if [ -f build/pi32/bin/helix-watchdog ]; then rsync -avz build/pi32/bin/helix-watchdog $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/bin/; fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	@echo "$(CYAN)Restarting helix-screen on $(PI_HOST)...$(RESET)"
	@ssh $(PI_SSH_TARGET) "sudo systemctl restart helixscreen 2>/dev/null" \
		|| ssh $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && setsid ./bin/helix-launcher.sh </dev/null >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"

# Full cycle: build + deploy + run in foreground
pi32-test: pi32-docker deploy-pi32-fg

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
	@echo "$(DIM)Transferring assets...$(RESET)"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_ASSET_DIRS) | ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR) && tar -xf -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/assets/images/prerendered $(AD5M_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR)/assets/images && tar -xf -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR)/assets/images/printers && tar -xf -"; \
	fi
	@# Deploy CA certificates for HTTPS verification
	@if [ -f build/ad5m/certs/ca-certificates.crt ]; then \
		echo "$(DIM)Transferring CA certificates...$(RESET)"; \
		ssh $(AD5M_SSH_TARGET) "mkdir -p $(AD5M_DEPLOY_DIR)/certs"; \
		cat build/ad5m/certs/ca-certificates.crt | ssh $(AD5M_SSH_TARGET) "cat > $(AD5M_DEPLOY_DIR)/certs/ca-certificates.crt"; \
	fi
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
	COPYFILE_DISABLE=1 tar -cf - --exclude='test_gcodes' --exclude='gcode' --exclude='.DS_Store' --exclude='.claude-recall' --exclude='._*' ui_xml assets config | ssh $(AD5M_SSH_TARGET) "cd $(AD5M_DEPLOY_DIR) && tar -xf -"
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
# Note: CC1 has rsync and scp in /opt/bin
#
# Deploy directory: /opt/helixscreen (CC1 has /opt mounted on UDISK with ~6.5GB)
# Override with CC1_DEPLOY_DIR if needed.
CC1_HOST ?= cc1.local
CC1_USER ?= root
CC1_DEPLOY_DIR ?= /opt/helixscreen

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
	@# Stop running processes and prepare directory
	ssh $(CC1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; mkdir -p $(CC1_DEPLOY_DIR)/bin"
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
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_ASSET_DIRS) | ssh $(CC1_SSH_TARGET) "cd $(CC1_DEPLOY_DIR) && tar -xf -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(CC1_SSH_TARGET) "mkdir -p $(CC1_DEPLOY_DIR)/assets/images/prerendered $(CC1_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(CC1_SSH_TARGET) "cd $(CC1_DEPLOY_DIR)/assets/images && tar -xf -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(CC1_SSH_TARGET) "cd $(CC1_DEPLOY_DIR)/assets/images/printers && tar -xf -"; \
	fi
	@# Deploy CA certificates for HTTPS verification
	@if [ -f build/cc1/certs/ca-certificates.crt ]; then \
		echo "$(DIM)Transferring CA certificates...$(RESET)"; \
		ssh $(CC1_SSH_TARGET) "mkdir -p $(CC1_DEPLOY_DIR)/certs"; \
		cat build/cc1/certs/ca-certificates.crt | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/certs/ca-certificates.crt"; \
	fi
	@echo "$(GREEN)✓ Deployed to $(CC1_HOST):$(CC1_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Starting helix-screen on $(CC1_HOST)...$(RESET)"
	ssh $(CC1_SSH_TARGET) "cd $(CC1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen started in background$(RESET)"
	@echo "$(DIM)Logs: ssh $(CC1_SSH_TARGET) 'logread -f | grep helix'$(RESET)"

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
	ssh $(CC1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; mkdir -p $(CC1_DEPLOY_DIR)/bin"
	cat build/cc1/bin/helix-screen | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-screen"
	cat build/cc1/bin/helix-splash | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/cc1/bin/helix-watchdog ]; then \
		cat build/cc1/bin/helix-watchdog | ssh $(CC1_SSH_TARGET) "cat > $(CC1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(CC1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	@echo "$(CYAN)Restarting helix-screen on $(CC1_HOST)...$(RESET)"
	ssh $(CC1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; cd $(CC1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
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
# Specs: Rockchip ARM64, 480x320 display, Debian Trixie
#
# Example: make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=192.168.1.100
# Note: U1 runs Debian with standard tools (scp, ssh, tar)
SNAPMAKER_U1_HOST ?= snapmaker-u1.local
SNAPMAKER_U1_USER ?= lava
SNAPMAKER_U1_SSH_TARGET := $(SNAPMAKER_U1_USER)@$(SNAPMAKER_U1_HOST)
SNAPMAKER_U1_DEPLOY_DIR ?= /opt/helixscreen

# =============================================================================
# Snapmaker U1 Deployment Targets
# =============================================================================

.PHONY: deploy-snapmaker-u1 deploy-snapmaker-u1-fg deploy-snapmaker-u1-bin snapmaker-u1-ssh

deploy-snapmaker-u1:
	@test -f build/snapmaker-u1/bin/helix-screen || { echo "$(RED)Error: build/snapmaker-u1/bin/helix-screen not found. Run 'make snapmaker-u1-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) "sudo killall helix-screen helix-splash 2>/dev/null || true; sudo mkdir -p $(SNAPMAKER_U1_DEPLOY_DIR)/bin"
	scp build/snapmaker-u1/bin/helix-screen $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/
	@if [ -f build/snapmaker-u1/bin/helix-splash ]; then scp build/snapmaker-u1/bin/helix-splash $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/; fi
	ssh $(SNAPMAKER_U1_SSH_TARGET) "chmod +x $(SNAPMAKER_U1_DEPLOY_DIR)/bin/helix-*"
	@# Transfer assets
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_ASSET_DIRS) | ssh $(SNAPMAKER_U1_SSH_TARGET) "cd $(SNAPMAKER_U1_DEPLOY_DIR) && tar -xf -"
	@if [ -f "build/snapmaker-u1/certs/ca-certificates.crt" ]; then \
		echo "  $(DIM)Deploying CA certificates...$(RESET)"; \
		ssh $(SNAPMAKER_U1_SSH_TARGET) "mkdir -p $(SNAPMAKER_U1_DEPLOY_DIR)/certs"; \
		scp build/snapmaker-u1/certs/ca-certificates.crt $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/certs/; \
	fi
	@echo "$(GREEN)✓ Deployed to $(SNAPMAKER_U1_HOST):$(SNAPMAKER_U1_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Starting helix-screen on $(SNAPMAKER_U1_HOST)...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) "cd $(SNAPMAKER_U1_DEPLOY_DIR) && sudo ./bin/helix-screen &"

deploy-snapmaker-u1-fg:
	@test -f build/snapmaker-u1/bin/helix-screen || { echo "$(RED)Error: build/snapmaker-u1/bin/helix-screen not found. Run 'make snapmaker-u1-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) "sudo killall helix-screen helix-splash 2>/dev/null || true; sudo mkdir -p $(SNAPMAKER_U1_DEPLOY_DIR)/bin"
	scp build/snapmaker-u1/bin/helix-screen $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/
	ssh $(SNAPMAKER_U1_SSH_TARGET) "chmod +x $(SNAPMAKER_U1_DEPLOY_DIR)/bin/helix-*"
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_ASSET_DIRS) | ssh $(SNAPMAKER_U1_SSH_TARGET) "cd $(SNAPMAKER_U1_DEPLOY_DIR) && tar -xf -"
	@echo "$(GREEN)✓ Deployed to $(SNAPMAKER_U1_HOST):$(SNAPMAKER_U1_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Starting helix-screen on $(SNAPMAKER_U1_HOST) (foreground, verbose)...$(RESET)"
	ssh -t $(SNAPMAKER_U1_SSH_TARGET) "cd $(SNAPMAKER_U1_DEPLOY_DIR) && sudo ./bin/helix-screen -vv"

deploy-snapmaker-u1-bin:
	@test -f build/snapmaker-u1/bin/helix-screen || { echo "$(RED)Error: build/snapmaker-u1/bin/helix-screen not found. Run 'make snapmaker-u1-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binary only to $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) "sudo killall helix-screen 2>/dev/null || true"
	scp build/snapmaker-u1/bin/helix-screen $(SNAPMAKER_U1_SSH_TARGET):$(SNAPMAKER_U1_DEPLOY_DIR)/bin/
	ssh $(SNAPMAKER_U1_SSH_TARGET) "chmod +x $(SNAPMAKER_U1_DEPLOY_DIR)/bin/helix-screen"
	@echo "$(GREEN)✓ Binary deployed$(RESET)"
	@echo "$(CYAN)Restarting helix-screen on $(SNAPMAKER_U1_HOST)...$(RESET)"
	ssh $(SNAPMAKER_U1_SSH_TARGET) "cd $(SNAPMAKER_U1_DEPLOY_DIR) && sudo ./bin/helix-screen &"

snapmaker-u1-ssh:
	ssh $(SNAPMAKER_U1_SSH_TARGET)

# =============================================================================
# K1 Deployment Configuration (UNTESTED)
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
# K1 Deployment Targets (UNTESTED)
# =============================================================================

.PHONY: deploy-k1 deploy-k1-fg deploy-k1-bin k1-ssh k1-test

# Deploy full application to K1 using tar/ssh (K1 BusyBox has no rsync)
deploy-k1:
	@test -f build/mips/bin/helix-screen || { echo "$(RED)Error: build/mips/bin/helix-screen not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@test -f build/mips/bin/helix-splash || { echo "$(RED)Error: build/mips/bin/helix-splash not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(K1_SSH_TARGET):$(K1_DEPLOY_DIR)...$(RESET)"
	@echo "$(YELLOW)NOTE: K1 deployment is UNTESTED - please report issues$(RESET)"
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
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; mkdir -p $(K1_DEPLOY_DIR)/bin"
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
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_ASSET_DIRS) | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && tar -xf -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(K1_SSH_TARGET) "mkdir -p $(K1_DEPLOY_DIR)/assets/images/prerendered $(K1_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR)/assets/images && tar -xf -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR)/assets/images/printers && tar -xf -"; \
	fi
	@echo "$(GREEN)✓ Deployed to $(K1_HOST):$(K1_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Starting helix-screen on $(K1_HOST)...$(RESET)"
	ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen started in background$(RESET)"
	@echo "$(DIM)Logs: ssh $(K1_SSH_TARGET) 'tail -f /var/log/messages | grep helix'$(RESET)"

# Deploy and run in foreground with verbose logging (for interactive debugging)
deploy-k1-fg:
	@test -f build/mips/bin/helix-screen || { echo "$(RED)Error: build/mips/bin/helix-screen not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@test -f build/mips/bin/helix-splash || { echo "$(RED)Error: build/mips/bin/helix-splash not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@echo "$(YELLOW)NOTE: K1 deployment is UNTESTED - please report issues$(RESET)"
	$(call deploy-common,$(K1_SSH_TARGET),$(K1_DEPLOY_DIR),build/mips/bin)
	@echo "$(CYAN)Starting helix-screen on $(K1_HOST) (foreground, verbose)...$(RESET)"
	ssh -t $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug"

# Deploy binaries only (fast, for quick iteration)
deploy-k1-bin:
	@test -f build/mips/bin/helix-screen || { echo "$(RED)Error: build/mips/bin/helix-screen not found. Run 'make mips-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binaries only to $(K1_SSH_TARGET):$(K1_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; mkdir -p $(K1_DEPLOY_DIR)/bin"
	cat build/mips/bin/helix-screen | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K1_DEPLOY_DIR)/bin/helix-screen"
	cat build/mips/bin/helix-splash | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/mips/bin/helix-watchdog ]; then \
		cat build/mips/bin/helix-watchdog | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	@echo "$(CYAN)Restarting helix-screen on $(K1_HOST)...$(RESET)"
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
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
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; mkdir -p $(K1_DEPLOY_DIR)/bin"
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
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_ASSET_DIRS) | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR) && tar -xf -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(K1_SSH_TARGET) "mkdir -p $(K1_DEPLOY_DIR)/assets/images/prerendered $(K1_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR)/assets/images && tar -xf -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(K1_SSH_TARGET) "cd $(K1_DEPLOY_DIR)/assets/images/printers && tar -xf -"; \
	fi
	@echo "$(GREEN)✓ Deployed to $(K1_HOST):$(K1_DEPLOY_DIR)$(RESET)"
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
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; mkdir -p $(K1_DEPLOY_DIR)/bin"
	cat build/k1-dynamic/bin/helix-screen | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K1_DEPLOY_DIR)/bin/helix-screen"
	cat build/k1-dynamic/bin/helix-splash | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K1_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/k1-dynamic/bin/helix-watchdog ]; then \
		cat build/k1-dynamic/bin/helix-watchdog | ssh $(K1_SSH_TARGET) "cat > $(K1_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K1_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	@echo "$(CYAN)Restarting helix-screen on $(K1_HOST)...$(RESET)"
	ssh $(K1_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; sleep 1; cd $(K1_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
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
# UNTESTED: Based on research, not hardware validation.
# See docs/printer-research/CREALITY_K2_PLUS_RESEARCH.md
#
# Example: make deploy-k2 K2_HOST=192.168.1.100
# Note: K2 uses BusyBox/OpenWrt - tar/ssh transfer, no rsync expected
K2_HOST ?= k2.local
K2_USER ?= root
K2_DEPLOY_DIR ?= /opt/helixscreen

# Build SSH target for K2
K2_SSH_TARGET := $(K2_USER)@$(K2_HOST)

# =============================================================================
# K2 Deployment Targets (UNTESTED)
# =============================================================================

.PHONY: deploy-k2 deploy-k2-fg deploy-k2-bin k2-ssh k2-test

# Deploy full application to K2 using tar/ssh (K2 BusyBox has no rsync)
deploy-k2:
	@test -f build/k2/bin/helix-screen || { echo "$(RED)Error: build/k2/bin/helix-screen not found. Run 'make k2-docker' first.$(RESET)"; exit 1; }
	@test -f build/k2/bin/helix-splash || { echo "$(RED)Error: build/k2/bin/helix-splash not found. Run 'make k2-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying HelixScreen to $(K2_SSH_TARGET):$(K2_DEPLOY_DIR)...$(RESET)"
	@echo "$(YELLOW)NOTE: K2 deployment is UNTESTED - please report issues$(RESET)"
	@# Generate pre-rendered images if missing
	@if [ ! -f build/assets/images/prerendered/splash-logo-small.bin ]; then \
		echo "$(DIM)Generating pre-rendered splash images...$(RESET)"; \
		$(MAKE) gen-images; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ] || [ -z "$$(ls -A build/assets/images/printers/prerendered/*.bin 2>/dev/null)" ]; then \
		echo "$(DIM)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@# Stop running processes and prepare directory
	ssh $(K2_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash display-server 2>/dev/null || true; mkdir -p $(K2_DEPLOY_DIR)/bin"
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
	COPYFILE_DISABLE=1 tar -cf - $(DEPLOY_TAR_EXCLUDES) $(DEPLOY_ASSET_DIRS) | ssh $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR) && tar -xf -"
	@# Transfer pre-rendered images
	@if [ -d build/assets/images/prerendered ] && ls build/assets/images/prerendered/*.bin >/dev/null 2>&1; then \
		echo "$(DIM)Transferring pre-rendered images...$(RESET)"; \
		ssh $(K2_SSH_TARGET) "mkdir -p $(K2_DEPLOY_DIR)/assets/images/prerendered $(K2_DEPLOY_DIR)/assets/images/printers/prerendered"; \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images prerendered | ssh $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR)/assets/images && tar -xf -"; \
	fi
	@if [ -d build/assets/images/printers/prerendered ] && ls build/assets/images/printers/prerendered/*.bin >/dev/null 2>&1; then \
		COPYFILE_DISABLE=1 tar -cf - -C build/assets/images/printers prerendered | ssh $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR)/assets/images/printers && tar -xf -"; \
	fi
	@echo "$(GREEN)✓ Deployed to $(K2_HOST):$(K2_DEPLOY_DIR)$(RESET)"
	@echo "$(CYAN)Starting helix-screen on $(K2_HOST)...$(RESET)"
	ssh $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR) && ./bin/helix-launcher.sh >/dev/null 2>&1 &"
	@echo "$(GREEN)✓ helix-screen started in background$(RESET)"
	@echo "$(DIM)Logs: ssh $(K2_SSH_TARGET) 'logread -f | grep helix'$(RESET)"

# Deploy and run in foreground with verbose logging (for interactive debugging)
deploy-k2-fg:
	@test -f build/k2/bin/helix-screen || { echo "$(RED)Error: build/k2/bin/helix-screen not found. Run 'make k2-docker' first.$(RESET)"; exit 1; }
	@test -f build/k2/bin/helix-splash || { echo "$(RED)Error: build/k2/bin/helix-splash not found. Run 'make k2-docker' first.$(RESET)"; exit 1; }
	@echo "$(YELLOW)NOTE: K2 deployment is UNTESTED - please report issues$(RESET)"
	$(call deploy-common,$(K2_SSH_TARGET),$(K2_DEPLOY_DIR),build/k2/bin)
	@echo "$(CYAN)Starting helix-screen on $(K2_HOST) (foreground, verbose)...$(RESET)"
	ssh -t $(K2_SSH_TARGET) "cd $(K2_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug"

# Deploy binaries only (fast, for quick iteration)
deploy-k2-bin:
	@test -f build/k2/bin/helix-screen || { echo "$(RED)Error: build/k2/bin/helix-screen not found. Run 'make k2-docker' first.$(RESET)"; exit 1; }
	@echo "$(CYAN)Deploying binaries only to $(K2_SSH_TARGET):$(K2_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(K2_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null || true; mkdir -p $(K2_DEPLOY_DIR)/bin"
	cat build/k2/bin/helix-screen | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-screen && chmod +x $(K2_DEPLOY_DIR)/bin/helix-screen"
	cat build/k2/bin/helix-splash | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-splash && chmod +x $(K2_DEPLOY_DIR)/bin/helix-splash"
	@if [ -f build/k2/bin/helix-watchdog ]; then \
		cat build/k2/bin/helix-watchdog | ssh $(K2_SSH_TARGET) "cat > $(K2_DEPLOY_DIR)/bin/helix-watchdog && chmod +x $(K2_DEPLOY_DIR)/bin/helix-watchdog"; \
	fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
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

# Assets to include (exclude test_gcodes, gcode test files)
RELEASE_ASSETS := assets/fonts assets/images

# Clean up release assets: remove files that are compiled into the binary or dev-only
# .c font files are compiled into the binary at build time (28 MB savings)
# .icns is a macOS icon format, not used on embedded targets
# mdi-icon-metadata is dev-only (icon search tooling)
define release-clean-assets
	@find $(1)/assets/fonts -name '*.c' -delete 2>/dev/null || true
	@find $(1)/assets -name '*.icns' -delete 2>/dev/null || true
	@find $(1)/assets -name 'mdi-icon-metadata.json.gz' -delete 2>/dev/null || true
endef

.PHONY: release-pi release-pi32 release-ad5m release-k1 release-ad5x release-k1-dynamic release-k2 release-snapmaker-u1 release-all release-clean pi-fbdev-docker pi32-fbdev-docker pi-all-docker pi32-all-docker

# Package Pi release
release-pi: | build/pi/bin/helix-screen build/pi/bin/helix-splash build/pi-fbdev/bin/helix-screen
	@echo "$(CYAN)$(BOLD)Packaging Pi release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/pi/bin/helix-screen build/pi/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/pi/bin/helix-watchdog ]; then cp build/pi/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@if [ -f build/pi-fbdev/bin/helix-screen ]; then cp build/pi-fbdev/bin/helix-screen $(RELEASE_DIR)/helixscreen/bin/helix-screen-fbdev; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
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
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-pi.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-pi.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-pi-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-pi-$(RELEASE_VERSION).tar.gz + helixscreen-pi.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-pi-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-pi.zip

# Package Pi 32-bit release (same structure as 64-bit Pi)
release-pi32: | build/pi32/bin/helix-screen build/pi32/bin/helix-splash build/pi32-fbdev/bin/helix-screen
	@echo "$(CYAN)$(BOLD)Packaging Pi 32-bit release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/pi32/bin/helix-screen build/pi32/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/pi32/bin/helix-watchdog ]; then cp build/pi32/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@if [ -f build/pi32-fbdev/bin/helix-screen ]; then cp build/pi32-fbdev/bin/helix-screen $(RELEASE_DIR)/helixscreen/bin/helix-screen-fbdev; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
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
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-pi32.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-pi32.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-pi32-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-pi32-$(RELEASE_VERSION).tar.gz + helixscreen-pi32.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-pi32-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-pi32.zip

# Package AD5M release
# Note: AD5M uses BusyBox which doesn't support tar -z, so we create uncompressed tar + gzip separately
# Includes pre-configured helixconfig.json for Adventurer 5M Pro (skips setup wizard)
release-ad5m: | build/ad5m/bin/helix-screen build/ad5m/bin/helix-splash
	@echo "$(CYAN)$(BOLD)Packaging AD5M release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/ad5m/bin/helix-screen build/ad5m/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/ad5m/bin/helix-watchdog ]; then cp build/ad5m/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@# Copy AD5M Pro default config as config/helixconfig.json (skips wizard on first run)
	@cp config/presets/ad5m.json $(RELEASE_DIR)/helixscreen/config/helixconfig.json
	@echo "  $(DIM)Included pre-configured config/helixconfig.json for AD5M Pro$(RESET)"
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
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
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-ad5m.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-ad5m.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-ad5m-$(RELEASE_VERSION).tar.gz helixscreen
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
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
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
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-ad5x.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-ad5x.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-ad5x-$(RELEASE_VERSION).tar.gz helixscreen
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
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
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
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-cc1.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-cc1.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-cc1-$(RELEASE_VERSION).tar.gz helixscreen
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
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
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
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-k1.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-k1.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-k1-$(RELEASE_VERSION).tar.gz helixscreen
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
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
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
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-k1-dynamic.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-k1-dynamic.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-k1-dynamic-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-k1-dynamic-$(RELEASE_VERSION).tar.gz + helixscreen-k1-dynamic.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-k1-dynamic-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-k1-dynamic.zip

# Package K2 release
release-k2: | build/k2/bin/helix-screen build/k2/bin/helix-splash
	@echo "$(CYAN)$(BOLD)Packaging K2 release v$(VERSION)...$(RESET)"
	@mkdir -p $(RELEASE_DIR)/helixscreen/bin
	@cp build/k2/bin/helix-screen build/k2/bin/helix-splash $(RELEASE_DIR)/helixscreen/bin/
	@if [ -f build/k2/bin/helix-watchdog ]; then cp build/k2/bin/helix-watchdog $(RELEASE_DIR)/helixscreen/bin/; fi
	@cp scripts/helix-launcher.sh $(RELEASE_DIR)/helixscreen/bin/
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@# Remove any personal config — release ships template only (installer copies it on first run)
	@rm -f $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME)
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
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
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-k2.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-k2.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-k2-$(RELEASE_VERSION).tar.gz helixscreen
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
	@cp -r ui_xml config $(RELEASE_DIR)/helixscreen/
	@rm -f $(RELEASE_DIR)/helixscreen/config/helixconfig.json $(RELEASE_DIR)/helixscreen/config/helixconfig-test.json
	@cp scripts/$(INSTALLER_FILENAME) $(RELEASE_DIR)/helixscreen/ 2>/dev/null || true
	@chmod +x $(RELEASE_DIR)/helixscreen/$(INSTALLER_FILENAME) 2>/dev/null || true
	@mkdir -p $(RELEASE_DIR)/helixscreen/scripts
	@cp scripts/uninstall.sh $(RELEASE_DIR)/helixscreen/scripts/ 2>/dev/null || true
	@mkdir -p $(RELEASE_DIR)/helixscreen/assets
	@for asset in $(RELEASE_ASSETS); do \
		if [ -d "$$asset" ]; then cp -r "$$asset" $(RELEASE_DIR)/helixscreen/assets/; fi; \
	done
	@if [ -f "build/snapmaker-u1/certs/ca-certificates.crt" ]; then \
		mkdir -p $(RELEASE_DIR)/helixscreen/certs; \
		cp build/snapmaker-u1/certs/ca-certificates.crt $(RELEASE_DIR)/helixscreen/certs/; \
		echo "  $(DIM)Included CA certificates for HTTPS$(RESET)"; \
	fi
	@find $(RELEASE_DIR)/helixscreen -name '.DS_Store' -delete 2>/dev/null || true
	$(call release-clean-assets,$(RELEASE_DIR)/helixscreen)
	@xattr -cr $(RELEASE_DIR)/helixscreen 2>/dev/null || true
	@echo '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"$(RELEASE_VERSION)","asset_name":"helixscreen-snapmaker-u1.zip"}' > $(RELEASE_DIR)/helixscreen/release_info.json
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-snapmaker-u1.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf helixscreen-snapmaker-u1-$(RELEASE_VERSION).tar.gz helixscreen
	@rm -rf $(RELEASE_DIR)/helixscreen
	@echo "$(GREEN)✓ Created $(RELEASE_DIR)/helixscreen-snapmaker-u1-$(RELEASE_VERSION).tar.gz + helixscreen-snapmaker-u1.zip$(RESET)"
	@ls -lh $(RELEASE_DIR)/helixscreen-snapmaker-u1-$(RELEASE_VERSION).tar.gz $(RELEASE_DIR)/helixscreen-snapmaker-u1.zip

# Package all releases
release-all: release-pi release-pi32 release-ad5m release-cc1 release-k1 release-ad5x release-k1-dynamic release-k2
	@echo "$(GREEN)$(BOLD)✓ All releases packaged in $(RELEASE_DIR)/$(RESET)"
	@ls -lh $(RELEASE_DIR)/*.tar.gz $(RELEASE_DIR)/*.zip

# Clean release artifacts
release-clean:
	@rm -rf $(RELEASE_DIR)
	@echo "$(GREEN)✓ Release directory cleaned$(RESET)"

# Aliases for package-* (matches scripts/package.sh naming)
# These trigger the full build + package workflow
.PHONY: package-ad5m package-cc1 package-pi package-pi32 package-k1 package-ad5x package-k1-dynamic package-k2 package-snapmaker-u1 package-all package-clean
package-ad5m: ad5m-docker gen-images-ad5m gen-splash-3d-ad5m gen-printer-images release-ad5m
package-cc1: cc1-docker gen-images gen-printer-images release-cc1
package-pi: pi-all-docker gen-images gen-splash-3d gen-printer-images release-pi
package-pi32: pi32-all-docker gen-images gen-splash-3d gen-printer-images release-pi32
package-k1: mips-docker gen-images gen-splash-3d-k1 gen-printer-images release-k1
package-ad5x: mips-docker gen-images gen-splash-3d-k1 gen-printer-images release-ad5x
package-k1-dynamic: k1-dynamic-docker gen-images gen-splash-3d-k1 gen-printer-images release-k1-dynamic
package-k2: k2-docker gen-images gen-printer-images release-k2
package-snapmaker-u1: snapmaker-u1-docker gen-images gen-printer-images release-snapmaker-u1
package-all: package-ad5m package-cc1 package-pi package-pi32 package-k1 package-ad5x package-k1-dynamic package-k2
package-clean: release-clean

# Convenience aliases (verb-target → target-verb)
.PHONY: pi-deploy ad5m-deploy
pi-deploy: deploy-pi
ad5m-deploy: deploy-ad5m
