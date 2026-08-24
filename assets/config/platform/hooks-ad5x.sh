#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: FlashForge AD5X with Z-Mod firmware
#
# The AD5X is NOT the AD5M with a different badge, which is why this file exists
# rather than reusing hooks-ad5m-zmod.sh:
#
#   - Z-Mod runs the AD5X's mod userland inside a chroot at /usr/data/.mod/.zmod,
#     with its own glibc and dynamic linker. HelixScreen runs inside it, so every
#     path here is a path as seen from within that chroot.
#   - The install root is /srv/helixscreen (Z-Mod ships and launches it there via
#     its own /etc/init.d/S80helixscreen), not /opt/helixscreen or /data/helixscreen.
#   - /data does not exist on this machine at all, on the host or in the chroot.
#     The AD5M hooks export HELIX_CACHE_DIR=/data/helixscreen/cache, which would
#     silently point at nothing here.
#
# Durable storage is /usr/data (mmcblk0p7, ~5.5G); / is read-only squashfs and
# /tmp and /run are tmpfs.

# shellcheck disable=SC3043  # local is supported by BusyBox ash

# Z-Mod already decides which alternate screen runs: DISPLAY_OFF GUPPY=0 Helix=1
# writes guppy/helix into mod_data/variables.cfg and Z-Mod honours it, so
# GuppyScreen is not started alongside us. Stop anything that did get up anyway —
# a stale process from a previous mode switch would fight us for /dev/fb0.
platform_stop_competing_uis() {
    for ui in guppyscreen GuppyScreen KlipperScreen klipperscreen; do
        for initscript in /etc/init.d/S*"${ui}"*; do
            if [ -x "$initscript" ] 2>/dev/null; then
                echo "Stopping competing UI: $initscript"
                "$initscript" stop 2>/dev/null || true
            fi
        done
        for pid in $(pidof "$ui" 2>/dev/null); do
            kill "$pid" 2>/dev/null || true
        done
    done
    sleep 1
}

# Backlight is kernel/hardware managed on this platform, as on the AD5M.
platform_enable_backlight() {
    return 0
}

# Cold-boot readiness probe. Z-Mod ships S80guppyscreen in the same init slot as
# S80helixscreen, so ordering within that slot is alphabetical and the framebuffer
# may not be settled when we start. Moonraker readiness is deliberately not checked:
# helix-screen handles a disconnected Moonraker gracefully, and on this box Moonraker
# takes well over a minute to answer after a cold boot.
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
    return 0
}

platform_pre_start() {
    # Durable and NOT collected by TAR_CONFIG, so a support archive does not carry
    # a cache. /srv/helixscreen lives on /usr/data inside the chroot.
    export HELIX_CACHE_DIR="/srv/helixscreen/cache"
    mkdir -p "$HELIX_CACHE_DIR" 2>/dev/null || true

    # Log where the user can actually send it from. ghzserg's tar_config.sh has a
    # dedicated AD5X branch that collects:
    #   /opt/config/  /usr/prog/config/  /usr/data/logs/  /usr/prog/app_startup.sh
    #   /tmp/*.txt
    # excluding logo, save, database, shapers, ssh.key, .git, .shell, notify.txt
    # and printer_data. `log` is not excluded, and /opt/config is a bind mount of
    # the durable mod config dir, so mod_data/log/ rides along. Anything written
    # outside those roots never reaches a support archive at all (#1249).
    #
    # helix.log, deliberately NOT helixscreen.log: Z-Mod's own S80helixscreen
    # hardcodes LOGFILE=/opt/config/mod_data/log/helixscreen.log and redirects the
    # launcher subshell into it. That file is theirs and carries the
    # [helix-launcher] stream; reusing the name interleaves two writers.
    export HELIX_LOG_DEST=file
    export HELIX_LOG_FILE="/opt/config/mod_data/log/helix.log"
    export HELIX_LOG_ROTATE_BYTES=1048576
    export HELIX_LOG_ROTATE_FILES=3
    mkdir -p "/opt/config/mod_data/log" 2>/dev/null || true

    # Remote control (helix-screen ctl) stays OFF by default. To enable it on this
    # platform, uncomment the two lines below. ZMOD owns /etc/init.d/S80helixscreen,
    # so there is nowhere to add --remote that a firmware update will not overwrite;
    # helix-screen therefore also accepts HELIX_REMOTE_CONTROL, and this file is the
    # supported place to set it because the installer reinstalls it on update.
    # export HELIX_REMOTE_CONTROL=1
    # export HELIX_REMOTE_SOCKET=/tmp/helix-screen.sock

    touch /tmp/helixscreen_active
}

platform_post_stop() {
    rm -f /tmp/helixscreen_active
}
