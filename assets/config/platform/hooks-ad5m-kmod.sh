#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: AD5M with Klipper Mod firmware
#
# Klipper Mod runs on the AD5M hardware but uses a different software stack
# than ForgeX. It typically includes Xorg for display and may run KlipperScreen
# as the default UI. HelixScreen renders directly to the framebuffer (/dev/fb0),
# so Xorg must be stopped to release the display.

# Stop Xorg and KlipperScreen so HelixScreen can access the framebuffer.
# Xorg takes over /dev/fb0's layer, preventing direct framebuffer rendering.
# KlipperScreen runs as a Python process under Xorg.
platform_stop_competing_uis() {
    # Stop Xorg via its init script (required for framebuffer access)
    if [ -x "/etc/init.d/S40xorg" ]; then
        echo "Stopping Xorg (Klipper Mod)..."
        /etc/init.d/S40xorg stop 2>/dev/null || true
        # Kill any remaining Xorg processes
        if command -v killall >/dev/null 2>&1; then
            killall Xorg 2>/dev/null || true
            killall X 2>/dev/null || true
        else
            for pid in $(pidof Xorg 2>/dev/null) $(pidof X 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    fi

    # Stop known competing UIs
    for ui in guppyscreen GuppyScreen KlipperScreen klipperscreen featherscreen FeatherScreen; do
        for initscript in /etc/init.d/S*"${ui}"* /opt/config/mod/.root/S*"${ui}"*; do
            if [ -x "$initscript" ] 2>/dev/null; then
                echo "Stopping competing UI: $initscript"
                "$initscript" stop 2>/dev/null || true
            fi
        done
        if command -v killall >/dev/null 2>&1; then
            killall "$ui" 2>/dev/null || true
        else
            for pid in $(pidof "$ui" 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    done

    # Kill python-based KlipperScreen (runs as a python3 process)
    # shellcheck disable=SC2009
    for pid in $(ps 2>/dev/null | grep -E 'python.*screen\.py' | grep -v grep | awk '{print $1}'); do
        echo "Killing KlipperScreen python process (PID $pid)"
        kill "$pid" 2>/dev/null || true
    done

    # Brief pause to let processes exit
    sleep 1
}

# Klipper Mod does not require special backlight control.
# The display backlight is managed normally by the kernel/hardware.
platform_enable_backlight() {
    return 0
}

# Klipper Mod systems typically have more RAM available (e.g., running on
# a Pi or with swap configured), so no service wait is needed.
platform_wait_for_services() {
    return 0
}

# No flag file coordination needed on Klipper Mod -- there is no S99root
# or screen.sh equivalent that checks for a third-party UI.
platform_pre_start() {
    export HELIX_CACHE_DIR="${HELIX_CACHE_DIR:-/data/helixscreen/cache}"

    # Logging policy: see hooks-ad5m-forgex.sh for the full rationale.
    # Short version: AD5M has 107 MB RAM, /tmp is a 54 MB tmpfs typically
    # at <10 MB free, and /var/log is symlinked to /tmp. Write logs to
    # /data (ext4, 4.6 GB free) and constrain rotation tightly.
    export HELIX_LOG_DEST="${HELIX_LOG_DEST:-file}"
    export HELIX_LOG_FILE="${HELIX_LOG_FILE:-/data/helixscreen/logs/helix.log}"
    export HELIX_LOG_ROTATE_BYTES="${HELIX_LOG_ROTATE_BYTES:-1048576}"
    export HELIX_LOG_ROTATE_FILES="${HELIX_LOG_ROTATE_FILES:-3}"
    mkdir -p "/data/helixscreen/logs" 2>/dev/null || true

    return 0
}

platform_post_stop() {
    return 0
}
