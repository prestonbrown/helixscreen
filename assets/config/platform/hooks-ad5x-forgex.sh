#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: FlashForge AD5X running the Forge-X rig payload
#
# Adapted from HelixScreen's assets/config/platform/hooks-ad5x.sh (the Z-Mod
# AD5X hook). Same board, same rules: the mod userland is a MIPS buildroot
# chroot with its own glibc, HelixScreen runs inside it, and /data does not
# exist. Two things differ on this rig, both addressed below:
#
#   - The chroot is $MOD (/usr/data/.mod/.forge-x), not Z-Mod's /usr/data/.mod/.zmod,
#     and the install root is the synced payload /opt/config/mod/.bin/helixscreen.
#     init_buildroot bind-mounts /opt/config into the chroot at the same path,
#     so these paths hold on both sides of the chroot.
#   - Durable state lives under /opt/config/mod_data (the same tree the mod's
#     own logs and variables use), so the cache no longer points at Z-Mod's
#     /srv/helixscreen.

# shellcheck disable=SC3043  # local is supported by BusyBox ash

# Stop anything that would fight us for /dev/fb0. On this rig the stock Qt UI
# is already dead (bootstrap step 2) and Forge-X's own splash/typer binaries
# are ARM builds that cannot run on this MIPS board at all, so this is a
# belt-and-braces sweep of stale processes.
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

# Backlight is kernel/hardware managed on this platform.
platform_enable_backlight() {
    return 0
}

# Cold-boot readiness probe: the framebuffer must exist and be readable
# before the UI opens it. Moonraker readiness is deliberately not checked:
# helix-screen handles a disconnected Moonraker gracefully, and Moonraker
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
    # Durable cache under mod_data, beside the mod's own state. The payload
    # tree itself is synced (replaceable), so runtime caches never live in it.
    export HELIX_CACHE_DIR="/opt/config/mod_data/helixscreen/cache"
    mkdir -p "$HELIX_CACHE_DIR" 2>/dev/null || true

    # The app log lands where the mod's other logs live. helix.log, NOT
    # helixscreen.log: .shell/helixscreen.sh redirects the launcher's stderr
    # stream into helixscreen.log, and reusing a name interleaves two writers.
    export HELIX_LOG_DEST=file
    export HELIX_LOG_FILE="/opt/config/mod_data/log/helix.log"
    export HELIX_LOG_ROTATE_BYTES=1048576
    export HELIX_LOG_ROTATE_FILES=3
    mkdir -p "/opt/config/mod_data/log" 2>/dev/null || true

    # Remote control (helix-screen ctl) is enabled in config/helixscreen.env,
    # not here, so the switch lives next to the log level rather than in code.

    touch /tmp/helixscreen_active
}

platform_post_stop() {
    rm -f /tmp/helixscreen_active
}
