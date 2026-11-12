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

    # Stop and persistently disable the stock Creality display stack.
    # S99start_app launches the entire stock stack; if it remains executable it
    # will respawn every boot since it runs after S99helixscreen alphabetically.
    # The backend servers (master-server, app-server, web-server) are NOT
    # framebuffer contenders; they are what Creality Print and Creality Cloud
    # talk to, and /etc/init.d/S99creality-backend starts them at boot
    # (prestonbrown/helixscreen#1468). Everything else in the stock stack
    # simply never starts: with S99start_app dead nothing launches it.
    if [ -f /etc/init.d/S99start_app ]; then
        if [ -x /etc/init.d/S99start_app ]; then
            /etc/init.d/S99start_app stop 2>/dev/null || true
            # Persistently disable (reversible with chmod +x). Gated on -x:
            # this de-executes a stock UI that could still run; one the
            # installer already disabled (and recorded) stays down untouched.
            chmod a-x /etc/init.d/S99start_app 2>/dev/null || true
        fi
        # Monitor dies first: it is a watchdog that respawns display-server
        # moments after the kill below.
        for proc in Monitor display-server; do
            killall "$proc" 2>/dev/null || true
        done
    fi

    # S99start_app also manages dropbear (SSH) on stock K1 firmware.
    # Disabling it kills SSH on next reboot (#535). Ensure SSH survives.
    _ensure_ssh_running
}

# Where each launch and stop records whether SSH is up. Moonraker's logs
# folder, so a user with no shell can download it from Mainsail or Fluidd and
# the debug bundle fetches it (src/system/platform_table.cpp); the state dir
# when a K1 has no printer_data. Both are on /usr/data, which survives
# reboots and updates, unlike /tmp and /var/log.
if [ -d /usr/data/printer_data/logs ]; then
    K1_SSH_LOG="/usr/data/printer_data/logs/helixscreen-ssh.log"
else
    K1_SSH_LOG="/usr/data/helixscreen-state/logs/helixscreen-ssh.log"
fi
K1_SSH_LOG_MAX_BYTES=65536

# dropbear's pids and whether anything listens on port 22, as one line.
# scripts/lib/installer/competing_uis.sh#log_k1_ssh_state is the installer's
# copy; this file ships without the installer modules beside it.
_k1_ssh_state() {
    _kss_pids=$(pidof dropbear 2>/dev/null || true)
    _kss_listen=unknown
    if command -v netstat >/dev/null 2>&1; then
        if netstat -tln 2>/dev/null | grep -q ':22 '; then
            _kss_listen=yes
        else
            _kss_listen=no
        fi
    fi
    printf 'dropbear=%s port22=%s' "${_kss_pids:-none}" "$_kss_listen"
}

# Append one timestamped line to K1_SSH_LOG, keeping the newer half once it
# outgrows K1_SSH_LOG_MAX_BYTES. Never fails the caller.
_k1_ssh_log() {
    mkdir -p "$(dirname "$K1_SSH_LOG")" 2>/dev/null || return 0
    if [ -f "$K1_SSH_LOG" ]; then
        _ksl_size=$(wc -c < "$K1_SSH_LOG" 2>/dev/null || echo 0)
        if [ "$_ksl_size" -gt "$K1_SSH_LOG_MAX_BYTES" ]; then
            tail -c $((K1_SSH_LOG_MAX_BYTES / 2)) "$K1_SSH_LOG" > "$K1_SSH_LOG.tmp" 2>/dev/null &&
                mv -f "$K1_SSH_LOG.tmp" "$K1_SSH_LOG" 2>/dev/null
        fi
    fi
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$K1_SSH_LOG" 2>/dev/null || true
    return 0
}

# Ensure SSH (dropbear) is running. On stock K1, dropbear is started by
# S99start_app which we disable. Start it independently if needed.
_ensure_ssh_running() {
    _k1_ssh_log "launch: $(_k1_ssh_state)"
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
            _k1_ssh_log "launch: started via $script: $(_k1_ssh_state)"
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
        _k1_ssh_log "launch: started $dropbear_bin directly: $(_k1_ssh_state)"
    else
        _k1_ssh_log "launch: no dropbear binary found, SSH stays down"
    fi
}

platform_enable_backlight() {
    :
}

platform_wait_for_services() {
    :
}

platform_pre_start() {
    # Cache and logs live beside the payload, never inside it. The install root
    # is what an update replaces, and not only by our own hand: a Moonraker
    # `type: web` entry does shutil.rmtree(path) before extracting. Anything
    # under it goes on every update - logs vanish exactly when someone needs
    # them, and the thumbnail cache is rebuilt from nothing.
    export HELIX_CACHE_DIR="${HELIX_CACHE_DIR:-/usr/data/helixscreen-state/cache}"

    # LVGL decodes each thumbnail on every draw with no cache. 2 MB is the knee
    # measured on a K1C: -14% process CPU over a scroll-the-grid scenario, RSS
    # slightly BELOW the uncached run, and the first size at which no image is
    # too large to store (1 MB leaves 28 of them uncacheable). Larger buys 0.4
    # points for four times the memory. The K1 family is the only board where
    # this resolved cleanly - the K2 and AD5X measured inside their own run-to-run
    # spread, so they stay at the built-in 0.
    export HELIX_IMAGE_CACHE_KB="${HELIX_IMAGE_CACHE_KB:-2048}"

    # Logging policy: write to /usr/data (mmcblk0p10 ext4, ~5.7 GB free on
    # a typical install), NOT to /tmp. /tmp here is a ~104 MB tmpfs and
    # /var/log is a symlink into it (../tmp), so spdlog's syslog target
    # AND the launcher.log redirect both land in RAM. K1 family RAM is
    # ~256 MB total, often <50 MB free — log volume there steals from the
    # UI. Constrain rotation to 1 MiB × 3 (~3 MiB cap); at WARN/INFO that
    # gives months of headroom, and a debug session still has a bounded
    # window before rolling over.
    export HELIX_LOG_DEST="${HELIX_LOG_DEST:-file}"
    export HELIX_LOG_FILE="${HELIX_LOG_FILE:-/usr/data/helixscreen-state/logs/helix.log}"
    export HELIX_LOG_ROTATE_BYTES="${HELIX_LOG_ROTATE_BYTES:-1048576}"
    export HELIX_LOG_ROTATE_FILES="${HELIX_LOG_ROTATE_FILES:-3}"
    mkdir -p "/usr/data/helixscreen-state/logs" 2>/dev/null || true
}

platform_post_stop() {
    _k1_ssh_log "stop: $(_k1_ssh_state)"
}
