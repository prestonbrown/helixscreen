#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Creality K1 / K1C / K1 Max / K1 SE

platform_stop_competing_uis() {
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

    _kill_boot_logo
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

_kill_boot_logo(){
	if [ -f /etc/init.d/S12boot_display ]; then
		# kill the boot display so it does not keep showing over top of hellixscreen
		killall boot_display 2> /dev/null
	fi

	if [ -f  /usr/data/pellcorp/k1/black.jpg ]; then
		# display black transition image to stop the corrupt image being shown
		cmd_jpeg_display /usr/data/pellcorp/k1/black.jpg &
	fi
}

platform_enable_backlight() {
    :
}

platform_wait_for_services() {
    :
}

platform_pre_start() {
    export HELIX_CACHE_DIR="/usr/data/helixscreen/cache"
}

platform_post_stop() {
    :
}
