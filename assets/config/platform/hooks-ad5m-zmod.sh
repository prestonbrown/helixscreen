#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: AD5M / AD5M Pro with ZMOD firmware
#
# ZMOD provides Klipper, Moonraker, and SSH access on FlashForge AD5 series
# printers. Unlike ForgeX, ZMOD does not use a chroot or custom display
# management scripts. S80guppyscreen is managed by ZMOD — do NOT modify it.
#
# Key coordination point: /tmp/helixscreen_active
#   Used to signal that HelixScreen owns the display framebuffer.

# shellcheck disable=SC3043  # local is supported by BusyBox ash

# Stop competing screen UIs so HelixScreen can access the framebuffer.
# ZMOD does not run stock FlashForge UI (ffstartup-arm) or Xorg.
platform_stop_competing_uis() {
    # Stop known competing UIs via init scripts and process kill
    for ui in guppyscreen GuppyScreen KlipperScreen klipperscreen featherscreen FeatherScreen; do
        for initscript in /etc/init.d/S*"${ui}"*; do
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

    # Brief pause to let processes exit
    sleep 1
}

# ZMOD does not require special backlight control.
# The display backlight is managed by the kernel/hardware.
platform_enable_backlight() {
    return 0
}

# ZMOD cold-boot readiness probe.
#
# ZMOD ships S80guppyscreen; we install as S80helixscreen. Alphabetical ordering
# runs guppyscreen first in the same init slot, and on a cold boot the display
# framebuffer may not be stable by the time we try to take it over. A short
# probe for /dev/fb0 smooths over the race. Moonraker readiness is not checked
# here — helix-screen handles a disconnected Moonraker gracefully.
platform_wait_for_services() {
    local fb_timeout=10
    local fb_waited=0
    while [ "$fb_waited" -lt "$fb_timeout" ]; do
        if [ -c /dev/fb0 ] && [ -r /dev/fb0 ]; then
            return 0
        fi
        sleep 1
        fb_waited=$((fb_waited + 1))
    done
    echo "Warning: /dev/fb0 not ready after ${fb_timeout}s, starting anyway"
    return 0
}

# Pre-start setup: set the active flag so other services know HelixScreen
# owns the display.
platform_pre_start() {
    export HELIX_CACHE_DIR="/data/helixscreen/cache"

    # Logging policy: see hooks-ad5m-forgex.sh for the RAM/tmpfs rationale —
    # AD5M has 107 MB RAM, /tmp is a 54 MB tmpfs typically at <10 MB free, and
    # /var/log is symlinked to /tmp, so the app log must go to persistent
    # storage with tight rotation.
    #
    # ZMOD-specific: the destination is /opt/config, NOT /data, because ZMOD's
    # own log archiver decides what a user can actually send us. ghzserg's
    # TAR_CONFIG (tar_config.sh) collects:
    #   AD5X  /opt/config/ /usr/prog/config/ /usr/data/logs/
    #         /usr/prog/app_startup.sh /tmp/*.txt
    #   AD5M  /opt/config/ /data/logFiles/ /tmp/*.txt
    # /data/helixscreen/ is in neither list, so the app log we were writing
    # there never reached a single support archive (issue #1249). /opt/config
    # is a bind-mount of the durable mod config dir on both models, `log` is
    # not in TAR_CONFIG's exclude list, and mod_data/log/ already exists and is
    # writable on real hardware.
    #
    # Filename is helix.log, deliberately NOT helixscreen.log: ghzserg's
    # /etc/init.d/S80helixscreen (their fork of our init script) hardcodes
    # LOGFILE="/opt/config/mod_data/log/helixscreen.log" and redirects the
    # launcher subshell into it. That file is theirs — it carries the
    # [helix-launcher] stderr stream and any crash/glibc output. Reusing the
    # name would interleave two writers into one file.
    export HELIX_LOG_DEST=file
    export HELIX_LOG_FILE="/opt/config/mod_data/log/helix.log"
    export HELIX_LOG_ROTATE_BYTES=1048576
    export HELIX_LOG_ROTATE_FILES=3
    mkdir -p "/opt/config/mod_data/log" 2>/dev/null || true

    touch /tmp/helixscreen_active
}

# Post-stop cleanup: remove the active flag.
platform_post_stop() {
    rm -f /tmp/helixscreen_active
}
