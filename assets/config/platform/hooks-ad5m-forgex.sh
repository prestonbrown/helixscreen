#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: AD5M with ForgeX firmware
#
# ForgeX runs BusyBox init (SysV-style) with a chroot environment at
# /data/.mod/.forge-x containing Python utilities for hardware control.
# The AD5M has only ~107MB RAM, so boot sequencing matters greatly.
#
# Key coordination point: /tmp/helixscreen_active
#   ForgeX's S99root script checks this flag. When present, S99root skips
#   its own screen output (boot logo, status messages) so it doesn't stomp
#   on HelixScreen's framebuffer. ForgeX's screen.sh also checks this flag
#   to skip backlight dimming (eco mode) while HelixScreen is running.
#   The flag is set in platform_pre_start() and removed in platform_post_stop().

# shellcheck disable=SC3043  # local is supported by BusyBox ash

# ForgeX chroot location
FORGEX_CHROOT="/data/.mod/.forge-x"

# Backlight control script path (INSIDE the chroot, not on the host)
FORGEX_BACKLIGHT="/root/printer_data/py/backlight.py"

# Boot splash ownership. ForgeX 1.4.2 replaced its static boot images with a
# persistent `splash` daemon that owns /dev/fb0 and page-flips, started by
# S00init and stopped only by S99root. S90helixscreen starts helix-splash
# before forking its wait subshell, so on those releases two processes drive
# the framebuffer for the whole S90->S99 window and tear over each other.
#
# Releases up to 1.4.1 have no such daemon: they blit load.img.xz then
# splash.img.xz and nothing repaints afterwards. Nothing is competing for the
# framebuffer there, and our splash is the only thing showing boot progress,
# so it stays on.
#
# Probe for the daemon rather than reading version.txt. Version strings are
# not trustworthy here: there is no 1.4.2 tag (only 1.4.2-beta-2), `main`
# still reports 1.4.1, and tag 1.3.1 ships a version.txt reading 1.3.0.
#
# Sourced at file scope by both helixscreen.init and helix-launcher.sh, so one
# assignment covers both. An explicit HELIX_NO_SPLASH in the environment wins.
FORGEX_SPLASH_BIN="${FORGEX_SPLASH_BIN:-/opt/config/mod/.bin/exec/splash}"
if [ -z "${HELIX_NO_SPLASH}" ]; then
    if [ -x "$FORGEX_SPLASH_BIN" ]; then
        HELIX_NO_SPLASH=1
    else
        HELIX_NO_SPLASH=0
    fi
    export HELIX_NO_SPLASH
fi

# ForgeX 1.4.2 introduced netd, which owns every network decision in non-Stock
# display modes: it loads the Wi-Fi driver itself, runs its own wpa_supplicant
# against a private config, and enforces a single transport -- with
# mode=ETHERNET it stops wpa_supplicant and takes wlan0 down.
#
# netd performs that teardown when it starts, in S55boot. Our hooks run later,
# in S90, so a driver or supplicant we start here survives as a stray process
# on an interface netd believes it already removed. Leave networking entirely
# alone on releases that ship netd.
FORGEX_NETD_BIN="${FORGEX_NETD_BIN:-/opt/config/mod/.bin/exec/netd}"
FORGEX_NET_SYSFS="${FORGEX_NET_SYSFS:-/sys/class/net}"
FORGEX_WPA_BIN="${FORGEX_WPA_BIN:-/usr/sbin/wpa_supplicant}"
FORGEX_WPA_CONF="${FORGEX_WPA_CONF:-/etc/wpa_supplicant.conf}"

forgex_netd_owns_network() {
    [ -x "$FORGEX_NETD_BIN" ]
}

# Stop stock FlashForge UI and competing screen UIs.
# The AD5M stock firmware runs ffstartup-arm which launches firmwareExe
# (the stock Qt touchscreen UI). Both must be killed for HelixScreen to
# have exclusive framebuffer access.
platform_stop_competing_uis() {
    # Stop stock FlashForge firmware UI (ffstartup-arm -> firmwareExe)
    if [ -f /opt/PROGRAM/ffstartup-arm ]; then
        echo "Stopping stock FlashForge UI..."
        if command -v killall >/dev/null 2>&1; then
            killall firmwareExe 2>/dev/null || true
            killall ffstartup-arm 2>/dev/null || true
        else
            for pid in $(pidof firmwareExe 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
            for pid in $(pidof ffstartup-arm 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    fi

    # Stop any known competing third-party UIs
    for ui in guppyscreen GuppyScreen KlipperScreen klipperscreen featherscreen FeatherScreen; do
        # Stop via init scripts if they exist
        for initscript in /etc/init.d/S*"${ui}"* /opt/config/mod/.root/S*"${ui}"*; do
            if [ -x "$initscript" ] 2>/dev/null; then
                echo "Stopping competing UI: $initscript"
                "$initscript" stop 2>/dev/null || true
            fi
        done
        # Kill remaining processes
        if command -v killall >/dev/null 2>&1; then
            killall "$ui" 2>/dev/null || true
        else
            for pid in $(pidof "$ui" 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    done

    # Brief pause to let processes exit
    sleep 1
}

# Enable the display backlight via ForgeX's chroot Python utility.
# ForgeX may leave the backlight off when display mode is STOCK or during
# boot transitions. We explicitly set it to 100% before starting HelixScreen.
# The backlight.py script uses ioctl calls that must run inside the chroot.
platform_enable_backlight() {
    local full_backlight_path="${FORGEX_CHROOT}${FORGEX_BACKLIGHT}"
    if [ -d "$FORGEX_CHROOT" ] && [ -x "$full_backlight_path" ]; then
        echo "Enabling backlight via ForgeX chroot..."
        /usr/sbin/chroot "$FORGEX_CHROOT" "$FORGEX_BACKLIGHT" 100 2>/dev/null || true
        return 0
    fi

    echo "Warning: Could not enable backlight (chroot=$FORGEX_CHROOT, script=$full_backlight_path)"
    return 1
}

# Wait for Moonraker to become responsive before starting HelixScreen.
# On the AD5M's ~107MB RAM, launching helix-screen while moonraker is still
# initializing causes severe swap thrashing. By waiting here with only the
# lightweight splash screen running, moonraker can start without memory
# competition, dramatically improving total boot time.
platform_wait_for_services() {
    # Only wait on ForgeX -- it has the memory constraints
    if [ ! -d "$FORGEX_CHROOT" ]; then
        return 0
    fi

    # Check if moonraker is even enabled (ForgeX can disable it)
    local moonraker_disabled
    moonraker_disabled=$(/usr/sbin/chroot "$FORGEX_CHROOT" /bin/sh -c \
        'cd / 2>/dev/null; /opt/config/mod/.shell/commands/zconf.sh /opt/config/mod_data/variables.cfg --get "disable_moonraker" "0"' 2>/dev/null) || true
    if [ "$moonraker_disabled" = "1" ]; then
        echo "Moonraker disabled, skipping wait"
        return 0
    fi

    echo "Waiting for Moonraker (reduces memory pressure)..."
    local timeout=120
    local waited=0
    # Heartbeat/status file for the boot splash. Each rewrite keeps the splash
    # alive (no blank screen during the wait) and updates its status line.
    local status_file="${HELIX_SPLASH_STATUS_FILE:-/tmp/helix-splash-status}"
    while [ "$waited" -lt "$timeout" ]; do
        # Use wget since curl may not be available on BusyBox base system.
        # 2s per-request timeout tolerates load better than 1s.
        if wget -q -O /dev/null --timeout=2 http://localhost:7125/server/info 2>/dev/null; then
            echo "Moonraker ready after ${waited}s"
            echo "Starting HelixScreen…" >"$status_file" 2>/dev/null || true
            return 0
        fi
        # Label only — helix-splash owns the elapsed-seconds counter (from its own
        # monotonic start), so the count keeps climbing through helix-screen's
        # startup too. The rewrite still refreshes the file mtime = the heartbeat.
        echo "Starting Klipper…" >"$status_file" 2>/dev/null || true
        sleep 1
        waited=$((waited + 1))
        # Progress indicator every 10 seconds
        if [ $((waited % 10)) -eq 0 ]; then
            echo "  Still waiting for Moonraker... (${waited}s)"
        fi
    done

    echo "Starting without printer…" >"$status_file" 2>/dev/null || true
    echo "Warning: Moonraker not ready after ${timeout}s, starting anyway"
    return 1
}

# Load the Realtek 8821cu USB WiFi driver if (a) the .ko exists, (b) no
# wlan* interface is already up, and (c) /sbin/insmod is available.
#
# Forge-X's own auto_run.sh insmods /lib/modules/8821cu.ko at boot, but on
# customised setups (e.g., where helix-screen replaces the stock UI launcher
# and auto_run.sh is short-circuited) the driver never loads. The user then
# sees `wlan0` missing → check_wifi_hardware() reports "No WiFi hardware
# found" even though the dongle is plugged in. This hook makes the load
# idempotent — runs only when needed, mirrors Forge-X's own behaviour, and
# is a no-op on AD5M boards without a USB WiFi adapter.
platform_load_wifi_driver() {
    if forgex_netd_owns_network; then
        return 0  # netd owns the radio on this release
    fi
    # ip(8) isn't always installed on AD5M Forge-X — fall back to /sys/class/net.
    # POSIX glob (no `ls | grep`): if no wlan* exists, the loop sees the
    # literal pattern and `[ -e ]` is false. (shellcheck SC2010)
    for ifc in /sys/class/net/wlan*; do
        [ -e "$ifc" ] && return 0  # interface already up — nothing to do
    done
    if [ ! -x /sbin/insmod ]; then
        return 0  # no insmod, can't help
    fi
    # Try the modules-tree path first, fall back to Forge-X's symlinked copy.
    for ko in /lib/modules/$(uname -r)/8821cu.ko /lib/modules/8821cu.ko; do
        if [ -f "$ko" ]; then
            echo "Loading WiFi driver: $ko"
            /sbin/insmod "$ko" 2>&1 || true
            sleep 1  # USB enumeration after driver bind
            return 0
        fi
    done
}

# Ensure wpa_supplicant daemon is running so helix-screen's WiFi backend has
# a control socket to talk to. Forge-X's wifi_connect.sh starts wpa_supplicant
# only once the user has provided WiFi credentials via stock UI; on a fresh
# install where helix-screen's wizard handles WiFi setup, the daemon never
# starts on its own and the backend reports SERVICE_NOT_RUNNING. Idempotent:
# checks for existing pid + socket before starting.
platform_start_wpa_supplicant() {
    if forgex_netd_owns_network; then
        return 0  # netd owns the radio on this release
    fi
    # Skip if already running with a valid control socket
    if [ -S /var/run/wpa_supplicant/wlan0 ] && pidof wpa_supplicant >/dev/null 2>&1; then
        return 0
    fi
    # POSIX glob (no `ls | grep`, shellcheck SC2010): bail if no wlan* exists.
    has_wlan=0
    for ifc in "$FORGEX_NET_SYSFS"/wlan*; do
        [ -e "$ifc" ] && { has_wlan=1; break; }
    done
    if [ "$has_wlan" -eq 0 ]; then
        return 0  # no interface to bind to
    fi
    if [ ! -x "$FORGEX_WPA_BIN" ] || [ ! -f "$FORGEX_WPA_CONF" ]; then
        return 0  # binary or config missing, nothing we can do
    fi
    mkdir -p /var/run/wpa_supplicant
    echo "Starting wpa_supplicant daemon..."
    "$FORGEX_WPA_BIN" -B -i wlan0 -c "$FORGEX_WPA_CONF" 2>&1 || true
}

# Pre-start setup: set the active flag so ForgeX knows HelixScreen owns the display.
# This must happen BEFORE stopping competing UIs or enabling backlight, because
# ForgeX's screen.sh could run at any time via Klipper's delayed_gcode.
platform_pre_start() {
    export HELIX_CACHE_DIR="/data/helixscreen/cache"

    # Logging policy: write to flash (/data is ext4 with ~4.6 GB free), NOT
    # to /tmp. AD5M has only ~107 MB RAM and /tmp is a 54 MB tmpfs — under
    # normal load free memory is single-digit MB, so any log volume to tmpfs
    # actively steals from the UI. Worse, /var/log is a symlink to /tmp here
    # (Yocto convention), so spdlog's syslog target also lands in RAM. Force
    # the file sink to flash to bypass both paths.
    export HELIX_LOG_DEST=file
    export HELIX_LOG_FILE="/data/helixscreen/logs/helix.log"
    export HELIX_LOG_ROTATE_BYTES=1048576
    export HELIX_LOG_ROTATE_FILES=3
    mkdir -p "/data/helixscreen/logs" 2>/dev/null || true

    touch /tmp/helixscreen_active
    platform_load_wifi_driver
    platform_start_wpa_supplicant
}

# Wait for ForgeX boot sequence to complete before starting helix-screen.
# S99root runs AFTER S90helixscreen and writes directly to /dev/fb0 (boot logos,
# status messages, logged binary). Even with screen.sh patches, S99root can
# outlive Moonraker startup and stomp on the framebuffer after helix-screen launches.
# By waiting for S99root to exit, we guarantee a clean handoff.
platform_wait_for_boot_complete() {
    local s99root="/opt/config/mod/.root/S99root"
    if [ ! -f "$s99root" ]; then
        return 0
    fi

    echo "Waiting for ForgeX boot to complete..."
    local timeout=60
    local waited=0

    while [ "$waited" -lt "$timeout" ]; do
        # BusyBox-compatible process check for S99root script
        # shellcheck disable=SC2009  # pgrep not available on all BusyBox builds
        if ! ps w 2>/dev/null | grep -v grep | grep -q "S99root"; then
            echo "ForgeX boot complete after ${waited}s"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
        if [ $((waited % 10)) -eq 0 ]; then
            echo "  Still waiting for ForgeX boot... (${waited}s)"
        fi
    done

    echo "Warning: ForgeX boot still running after ${timeout}s, starting anyway"
    return 1
}

# Post-stop cleanup: remove the active flag so ForgeX can resume normal display control.
# After this, S99root and screen.sh will behave as if no third-party UI is present.
platform_post_stop() {
    rm -f /tmp/helixscreen_active
}
