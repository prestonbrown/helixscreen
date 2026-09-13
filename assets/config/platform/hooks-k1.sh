#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Creality K1 / K1C / K1 Max / K1 SE

platform_stop_competing_uis() {
    # boot_display (started by /etc/init.d/S12boot_display) draws the Creality
    # boot logo on all K1 firmware variants. On stock firmware it's stopped by
    # S99start_app's stop_boot_display(), but SimpleAF removes S99start_app
    # while leaving S12boot_display in place — so kill it unconditionally.
    killall boot_display 2>/dev/null || true

    # Stop and persistently disable stock Creality UI (display-server, Monitor, etc.)
    # S99start_app launches the entire stock UI stack; if it remains executable it
    # will respawn every boot since it runs after S99helixscreen alphabetically.
    if [ -f /etc/init.d/S99start_app ]; then
        if [ -x /etc/init.d/S99start_app ]; then
            /etc/init.d/S99start_app stop 2>/dev/null || true
            # Persistently disable (reversible with chmod +x)
            chmod a-x /etc/init.d/S99start_app 2>/dev/null || true
        fi
        # Kill any remaining stock UI processes (full list from S99start_app)
        for proc in display-server Monitor master-server audio-server \
                    wifi-server app-server upgrade-server web-server; do
            killall "$proc" 2>/dev/null || true
        done
    fi

    # S99start_app also manages dropbear (SSH) on stock K1 firmware.
    # Disabling it kills SSH on next reboot (#535). Ensure SSH survives.
    _ensure_ssh_running
}

# Ensure SSH (dropbear) is running. On stock K1, dropbear is started by
# S99start_app which we disable. Start it independently if needed.
_ensure_ssh_running() {
    # Already running — nothing to do
    if pidof dropbear >/dev/null 2>&1; then
        return 0
    fi

    # Try existing init script
    for script in /etc/init.d/S50dropbear /etc/init.d/S*dropbear*; do
        [ -f "$script" ] || continue
        chmod +x "$script" 2>/dev/null || true
        "$script" start 2>/dev/null || true
        if pidof dropbear >/dev/null 2>&1; then
            return 0
        fi
    done

    # Start directly as fallback and create init script for next boot
    dropbear_bin=""
    for bin in /usr/sbin/dropbear /usr/bin/dropbear /sbin/dropbear; do
        if [ -x "$bin" ]; then
            dropbear_bin="$bin"
            break
        fi
    done

    if [ -n "$dropbear_bin" ]; then
        "$dropbear_bin" -R 2>/dev/null || true
        # Create init script so it starts on future boots without our help
        if [ ! -f /etc/init.d/S50dropbear ]; then
            cat > /etc/init.d/S50dropbear << INITEOF
#!/bin/sh
DROPBEAR="${dropbear_bin}"
PIDFILE="/var/run/dropbear.pid"
case "\$1" in
    start) [ -x "\$DROPBEAR" ] && "\$DROPBEAR" -R -P "\$PIDFILE" ;;
    stop) [ -f "\$PIDFILE" ] && kill "\$(cat "\$PIDFILE")" 2>/dev/null; killall dropbear 2>/dev/null; rm -f "\$PIDFILE" ;;
    restart) \$0 stop; sleep 1; \$0 start ;;
esac
INITEOF
            chmod +x /etc/init.d/S50dropbear
        fi
    fi
}

platform_enable_backlight() {
    :
}

platform_wait_for_services() {
    :
}

platform_pre_start() {
    export HELIX_CACHE_DIR="${HELIX_CACHE_DIR:-/usr/data/helixscreen/cache}"

    # Logging policy: write to /usr/data (mmcblk0p10 ext4, ~5.7 GB free on
    # a typical install), NOT to /tmp. /tmp here is a ~104 MB tmpfs and
    # /var/log is a symlink into it (../tmp), so spdlog's syslog target
    # AND the launcher.log redirect both land in RAM. K1 family RAM is
    # ~256 MB total, often <50 MB free — log volume there steals from the
    # UI. Constrain rotation to 1 MiB × 3 (~3 MiB cap); at WARN/INFO that
    # gives months of headroom, and a debug session still has a bounded
    # window before rolling over.
    export HELIX_LOG_DEST="${HELIX_LOG_DEST:-file}"
    export HELIX_LOG_FILE="${HELIX_LOG_FILE:-/usr/data/helixscreen/logs/helix.log}"
    export HELIX_LOG_ROTATE_BYTES="${HELIX_LOG_ROTATE_BYTES:-1048576}"
    export HELIX_LOG_ROTATE_FILES="${HELIX_LOG_ROTATE_FILES:-3}"
    mkdir -p "/usr/data/helixscreen/logs" 2>/dev/null || true
}

platform_post_stop() {
    :
}
