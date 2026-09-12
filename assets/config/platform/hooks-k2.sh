#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Creality K2 / K2 Pro / K2 Plus
#
# K2 series runs OpenWrt 21.02 with procd init system.
# Stock UI is managed by /etc/init.d/app (procd service).
# Processes: display-server, web-server, Monitor, master-server, etc.

# shellcheck disable=SC3043  # local is supported by the K2 shell (bash/ash)

platform_stop_competing_uis() {
    # Stop the stock Creality UI via procd (clean shutdown)
    if [ -f /etc/init.d/app ]; then
        /etc/init.d/app stop 2>/dev/null || true
    fi

    # Kill any lingering stock UI processes
    for proc in display-server Monitor master-server audio-server \
                wifi-server app-server upgrade-server; do
        killall "$proc" 2>/dev/null || true
    done

    # Kill the Creality boot animation (/etc/init.d/S01play -> /sbin/boot-play).
    # It does NOT draw to /dev/fb0 — it puts a YUV *video layer* on the Allwinner
    # display engine at z-order 5, composited ON TOP of the fbdev UI layer (z0)
    # that our splash and helix-screen render to. Stock firmware signals it to
    # exit (bootanimation_exit) once the Creality UI is ready; we replaced that
    # UI, so nothing ever stops it and the animation covers our splash AND the UI
    # for the entire Moonraker gate (~70s) until it finally dies. SIGTERM lets it
    # run its de_disp_uninit cleanup so the z5 layer is released. procd does not
    # respawn it (no respawn param), so a one-shot kill here is sufficient.
    killall boot-play 2>/dev/null || true

    # web-server is intentionally NOT killed — it serves the Creality
    # Cloud integration and camera stream (webrtc_local). Stopping it
    # would break remote monitoring via the Creality app. The disable
    # below keeps the whole app set from starting at boot, so the
    # carve-out needs its own starter: /etc/init.d/helix-k2-webserver
    # (config/k2-webserver.init) brings web-server up at boot
    # independently of the app service (prestonbrown/helixscreen#1617).

    # Persistently disable the stock UI service (reversible)
    if [ -x /etc/init.d/app ]; then
        /etc/init.d/app disable 2>/dev/null || true
    fi
}

platform_enable_backlight() {
    :
}

platform_wait_for_services() {
    # Wait for Moonraker to be ready (K2 Moonraker is on port 7125).
    # K2's dual Cortex-A7 is slow; Moonraker+Klipper cold-boot init routinely
    # exceeds 30s, so allow up to 120s (matching hooks-ad5m-forgex). On timeout
    # the UI launches anyway — MoonrakerClient auto-reconnects indefinitely
    # (~2s backoff), so a late Moonraker just shows a brief disconnected state.
    #
    # A SINGLE long-lived python3 process does the polling rather than spawning
    # python3 once per second (cold-start cost adds up on the dual-A7 and can
    # mask an already-ready server). It also:
    #   - uses CLOCK_MONOTONIC so the timeout is immune to NTP jumps mid-boot,
    #   - writes a heartbeat/status line to the splash status file each second
    #     (keeps the boot splash alive — no blank screen — and shows progress),
    #   - logs how long detection actually took, so launcher.log reveals whether
    #     a ready Moonraker is being detected promptly (warm-restart diagnosis).
    # Use 127.0.0.1 (not localhost): K2 has no /etc/hosts entry for localhost.
    # Timeout and endpoint are overridable (non-standard Moonraker host/port, or
    # tests); production defaults match the stock K2 layout.
    local timeout_sec="${HELIX_MOONRAKER_WAIT_TIMEOUT:-120}"
    local ready_url="${HELIX_MOONRAKER_READY_URL:-http://127.0.0.1:7125/server/info}"
    local status_file="${HELIX_SPLASH_STATUS_FILE:-/tmp/helix-splash-status}"
    python3 - "$timeout_sec" "$status_file" "$ready_url" <<'PY'
import sys, time, urllib.request

timeout = int(sys.argv[1])
status_file = sys.argv[2]
ready_url = sys.argv[3]
start = time.monotonic()

def write_status(msg):
    try:
        with open(status_file, "w") as f:
            f.write(msg + "\n")
    except OSError:
        pass

while True:
    elapsed = int(time.monotonic() - start)
    if elapsed >= timeout:
        write_status("Starting without printer…")
        print("[hooks-k2] Warning: Moonraker not ready after %ds "
              "(UI will start; it auto-reconnects)" % timeout, flush=True)
        sys.exit(1)
    try:
        urllib.request.urlopen(ready_url, timeout=2)
        print("[hooks-k2] Moonraker ready after %ds" % elapsed, flush=True)
        write_status("Starting HelixScreen…")
        sys.exit(0)
    except Exception:
        pass
    if elapsed % 10 == 0:
        print("[hooks-k2] Waiting for Moonraker... (%ds)" % elapsed, flush=True)
    # Write only the label — helix-splash owns the elapsed-seconds counter (from
    # its own monotonic start) so the count keeps climbing through helix-screen's
    # startup too, after this gate exits. The per-second rewrite still refreshes
    # the file mtime, which is the splash's heartbeat.
    write_status("Starting Klipper…")
    time.sleep(1)
PY
    return $?
}

platform_pre_start() {
    export HELIX_CACHE_DIR="/mnt/UDISK/helixscreen-state/cache"

    # Logging policy: write to /mnt/UDISK (27 GB UDISK partition, ext4).
    # K2 /tmp is a 244 MB tmpfs (more headroom than CC1/AD5M) and /var/log
    # is on the persistent overlayfs root, so the init-script awk resolver
    # already keeps launcher.log out of RAM. But routing the structured
    # spdlog stream to UDISK too keeps everything in one place and avoids
    # filling the 240 MB overlay with debug volume when someone leaves
    # HELIX_LOG_LEVEL=debug enabled in helixscreen.env.
    export HELIX_LOG_DEST=file
    export HELIX_LOG_FILE="/mnt/UDISK/helixscreen-state/logs/helix.log"
    export HELIX_LOG_ROTATE_BYTES=2097152
    export HELIX_LOG_ROTATE_FILES=3
    mkdir -p "/mnt/UDISK/helixscreen-state/logs" 2>/dev/null || true

    # K2 has no curl — ensure HelixScreen knows to skip HTTPS features
    # SSL is disabled in the K2 build, but set this for safety
    export HELIX_DISABLE_SSL=1

    # Bring up WiFi in the background. The stock wifi-server manages
    # wpa_supplicant but we killed it in platform_stop_competing_uis(),
    # so we re-establish it here as a courtesy. Running synchronously
    # cost ~7s (kill + sleep 1 + wpa_supplicant + sleep 3 + udhcpc) and
    # blocked the entire boot path even when eth0 was already up — pure
    # waste. Backgrounding lets boot proceed; helix-screen reaches
    # Moonraker over localhost regardless, and external connectivity
    # (updates, telemetry) follows whichever interface ends up associated.
    if [ -e /sys/class/net/wlan0 ] && ! ip addr show wlan0 2>/dev/null | grep -q 'inet '; then
        local wpa_conf="/etc/wifi/wpa_supplicant/wpa_supplicant.conf"
        if [ -f "$wpa_conf" ]; then
            (
                killall wpa_supplicant 2>/dev/null || true
                sleep 1
                wpa_supplicant -B -i wlan0 -c "$wpa_conf" -D nl80211 2>/dev/null || true
                sleep 3
                udhcpc -i wlan0 -n -q 2>/dev/null || true
            ) >/dev/null 2>&1 &
        fi
    fi
}

platform_post_stop() {
    # Re-enable stock UI if HelixScreen is being uninstalled
    # (installer calls this; normal stop does NOT re-enable)
    :
}
