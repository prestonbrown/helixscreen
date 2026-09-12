#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Snapmaker U1 (Paxx Extended Firmware)
#
# The Snapmaker U1 runs Buildroot 2024.02 (Rockchip RK3562) with busybox SysV
# init — NOT Debian/systemd. Its stock touchscreen UI is /usr/bin/gui.
# HelixScreen uses the DRM backend for double-buffered page flipping.
#
# The build ships NO helix-watchdog, and busybox SysV init does not respawn S99
# children — so a boot-time SIGTERM to helix-screen would be permanent without a
# supervisor. We opt into the launcher's boot-time respawn self-heal below, and
# decouple WiFi bring-up from helix-screen's lifetime so a momentary UI death
# never strands the device off-network.
#
# DRM CRTC keepalive: The U1's display is driven by a Rockchip DRM/KMS
# pipeline (rockchipdrmfb). When the stock UI process exits, the kernel's
# VOP2 driver disables the CRTC, leaving the display permanently black.
# To prevent this, we spawn a background process that holds /dev/dri/card0
# open while we kill the stock UI. Once HelixScreen opens the DRM device
# itself, the keepalive process exits — but the CRTC stays active because
# HelixScreen now has its own fd on the device.
#
# Stock UI: /usr/bin/gui (started by S99screen init script)
# Camera supervisor: /usr/bin/lmd (started by /etc/init.d/S99v4l2-mpp-mipi at
#   boot — and *only* at boot; no fake-service auto-restart). lmd forks
#   /usr/bin/unisrv which subscribes to MQTT topic camera/request and answers
#   Klipper's TIMELAPSE_START / camera.take_a_photo RPCs.
# Touch: TLSC6x capacitive controller (tlsc6x_touch on /dev/input/event0)
# Display: 480x320 32bpp rockchipdrmfb (/dev/fb0)
# SSH access: root@<ip> (password: snapmaker) via extended firmware

# PID of the background keepalive process
DRM_KEEPALIVE_PID=""

# Opt into the launcher's boot-time respawn self-heal. The U1 ships no
# helix-watchdog and busybox init does not respawn S99 children, so a single
# boot-time SIGTERM to helix-screen (handled as _exit(0)) would otherwise be
# permanent. The launcher (helix-launcher.sh) reads HELIX_BOOT_RESPAWN_MAX and
# respawns a fast-exiting helix-screen up to this many times. Respect any value
# the user pinned in helixscreen.env.
export HELIX_BOOT_RESPAWN_MAX="${HELIX_BOOT_RESPAWN_MAX:-3}"

# WiFi restore configuration. The stock Snapmaker app saves the user's network
# to HELIX_SAVED_WPA and loads it into wpa_supplicant at runtime; since we
# replaced the stock app, we do it ourselves (see ensure_wifi_associated).
# Overridable for tests via the environment.
HELIX_SAVED_WPA="${HELIX_SAVED_WPA:-/oem/printer_data/gui/wpa_supplicant.conf}"
HELIX_WIFI_FLAG="${HELIX_WIFI_FLAG:-/tmp/helix-wifi-restore.active}"
HELIX_WIFI_IFACE="${HELIX_WIFI_IFACE:-wlan0}"

# Stop Snapmaker's stock touchscreen UI so HelixScreen can access the display.
#
# CRITICAL: We must hold /dev/dri/card0 open before killing the stock UI.
# Without this, the VOP2 driver disables the CRTC and the display goes
# permanently black until reboot.
platform_stop_competing_uis() {
    # Spawn a background process that holds the DRM device open.
    # It stays alive until helix-screen opens /dev/dri/card0 itself (detected
    # via /proc/*/fd), or for a maximum of 30 seconds as a safety timeout.
    if [ -e /dev/dri/card0 ]; then
        (
            # Hold the device open via our own fd
            exec 3>/dev/dri/card0
            echo "DRM keepalive: holding /dev/dri/card0 (pid $$)"
            # Wait until helix-screen has the device open, or timeout
            elapsed=0
            while [ "$elapsed" -lt 30 ]; do
                for pid_dir in /proc/[0-9]*/fd; do
                    pid=$(echo "$pid_dir" | sed 's|/proc/\([0-9]*\)/fd|\1|')
                    comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
                    case "$comm" in
                        helix-screen)
                            if readlink "$pid_dir"/* 2>/dev/null | grep -q '/dev/dri/card0'; then
                                echo "DRM keepalive: helix-screen (pid $pid) has /dev/dri/card0, releasing"
                                exit 0
                            fi
                            ;;
                    esac
                done
                sleep 1
                elapsed=$((elapsed + 1))
            done
            echo "DRM keepalive: timeout after 30s, releasing"
        ) &
        DRM_KEEPALIVE_PID=$!
        echo "DRM keepalive: background process PID $DRM_KEEPALIVE_PID"
    fi

    # Kill stock UI processes only. unisrv (proprietary camera/MQTT daemon
    # handling Klipper's TIMELAPSE_START) and lmd (its supervisor) are NOT UIs
    # and must keep running for timelapse to work. Killing lmd permanently
    # disables camera RPC until reboot — lmd has no supervisor of its own and
    # /etc/init.d/S99v4l2-mpp-mipi only starts it at boot.
    # NOTE: Do NOT call /etc/init.d/S99screen stop — on the U1, S99screen is
    # patched to delegate to helixscreen.init, which would cause infinite recursion.
    for ui in gui snapmaker-ui snapmaker-screen KlipperScreen klipperscreen; do
        if command -v killall >/dev/null 2>&1; then
            killall "$ui" 2>/dev/null || true
        else
            for pid in $(pidof "$ui" 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    done

    # Kill python-based KlipperScreen if running
    # shellcheck disable=SC2009
    for pid in $(ps aux 2>/dev/null | grep -E 'python.*screen\.py' | grep -v grep | awk '{print $2}'); do
        echo "Killing KlipperScreen python process (PID $pid)"
        kill "$pid" 2>/dev/null || true
    done

    # Brief pause to let processes settle
    sleep 1
}

# The U1 display backlight is managed by the kernel/hardware.
platform_enable_backlight() {
    return 0
}

# The U1's SysV init starts Klipper/Moonraker (S60klipper/S61moonraker) before
# our S99 launcher, so they should be available by the time HelixScreen starts.
platform_wait_for_services() {
    return 0
}

# Ensure lmd (camera/MQTT supervisor) is running. lmd is started once at boot
# by /etc/init.d/S99v4l2-mpp-mipi with no auto-restart wrapper, so if it ever
# dies — historically because older helixscreen builds killed it in
# platform_stop_competing_uis — Klipper's TIMELAPSE_START hits a 5-second MQTT
# timeout and pauses the print. We re-launch lmd with the same imposter env
# the stock init uses; lmd then forks unisrv as its child.
ensure_lmd_running() {
    if pidof lmd >/dev/null 2>&1; then
        return 0
    fi
    if [ ! -x /usr/bin/lmd ]; then
        return 0
    fi
    if [ ! -e /tmp/capture-mipi-raw.sock ]; then
        # Capture pipeline not up yet; lmd would just fail to attach. Let
        # S99v4l2-mpp-mipi finish its sequence on its own.
        return 0
    fi
    echo "lmd not running — restarting camera supervisor"
    rm -f /var/run/unisrv.pid
    (
        export LD_PRELOAD=/usr/local/lib/libv4l2-imposter.so
        export V4L2_IMPOSTER_SOCKET_PATH=/tmp/capture-mipi-raw.sock
        export V4L2_IMPOSTER_DEVICE=/dev/video11
        export V4L2_IMPOSTER_WIDTH=1920
        export V4L2_IMPOSTER_HEIGHT=1080
        export V4L2_IMPOSTER_FORMAT=nv12
        start-stop-daemon -S -b -q -m -p /var/run/unisrv.pid -x /usr/bin/lmd
    )
}

# True (0) if the WiFi interface reports an associated/completed state.
_helix_wifi_connected() {
    wpa_cli -i "$HELIX_WIFI_IFACE" status 2>/dev/null | grep -q '^wpa_state=COMPLETED'
}

# Synchronously push the saved network into wpa_supplicant. Idempotent and safe
# to call repeatedly: returns 0 if it applied (or the link was already up), 1 if
# there was nothing to apply (no saved config, no wpa_cli, or empty credentials).
_helix_wifi_apply_saved() {
    [ -f "$HELIX_SAVED_WPA" ] || return 1
    command -v wpa_cli >/dev/null 2>&1 || return 1

    _ssid=$(grep 'ssid=' "$HELIX_SAVED_WPA" | head -1 | sed 's/.*ssid="\(.*\)"/\1/')
    _psk=$(grep 'psk=' "$HELIX_SAVED_WPA" | head -1 | sed 's/.*psk="\(.*\)"/\1/')
    [ -n "$_ssid" ] && [ -n "$_psk" ] || return 1

    # Already connected — don't churn an up link.
    if _helix_wifi_connected; then
        return 0
    fi

    _netid=$(wpa_cli -i "$HELIX_WIFI_IFACE" add_network 2>/dev/null | tail -1)
    if [ -n "$_netid" ] && [ "$_netid" != "FAIL" ]; then
        wpa_cli -i "$HELIX_WIFI_IFACE" set_network "$_netid" ssid "\"$_ssid\"" >/dev/null 2>&1
        wpa_cli -i "$HELIX_WIFI_IFACE" set_network "$_netid" psk "\"$_psk\"" >/dev/null 2>&1
        wpa_cli -i "$HELIX_WIFI_IFACE" enable_network "$_netid" >/dev/null 2>&1
        wpa_cli -i "$HELIX_WIFI_IFACE" select_network "$_netid" >/dev/null 2>&1
        echo "WiFi: restoring saved network '$_ssid' (id=$_netid)"
    fi
    return 0
}

# Bring WiFi up INDEPENDENTLY of helix-screen's lifetime.
#
# The stock Snapmaker UI (which we replace) is what loads the user's saved WiFi
# credentials into wpa_supplicant at runtime. With the stock UI disabled, only
# we restore WiFi — and the old approach (one synchronous attempt in
# platform_pre_start) both raced wpa_supplicant/wlan0 at early boot AND coupled
# network recovery to helix's start path, so a momentary helix death stranded
# the device off-network and unreachable.
#
# So we spawn a DETACHED, idempotent worker that waits for the interface, then
# applies the saved network and retries until it associates. The worker outlives
# helix-screen (a `( ) &` grandchild reparented to init; the launcher's cleanup
# trap only killalls helix-* by name, not this sh worker), so the device stays
# reachable even if the UI dies. Returns immediately — never blocks boot.
ensure_wifi_associated() {
    [ -f "$HELIX_SAVED_WPA" ] || return 0
    command -v wpa_cli >/dev/null 2>&1 || return 0

    # Already up — nothing to do.
    if _helix_wifi_connected; then
        return 0
    fi

    # Single-flight: don't stack workers across the init + launcher pre_start
    # calls (both run platform_pre_start). A live worker owns HELIX_WIFI_FLAG.
    if [ -f "$HELIX_WIFI_FLAG" ] && kill -0 "$(cat "$HELIX_WIFI_FLAG" 2>/dev/null)" 2>/dev/null; then
        return 0
    fi

    (
        trap 'rm -f "$HELIX_WIFI_FLAG" 2>/dev/null || true' EXIT
        # Wait for wpa_supplicant's control interface (up to ~30s).
        _w=0
        while [ "$_w" -lt 30 ]; do
            wpa_cli -i "$HELIX_WIFI_IFACE" status >/dev/null 2>&1 && break
            sleep 1
            _w=$((_w + 1))
        done
        # Apply + verify, a few attempts.
        _try=0
        while [ "$_try" -lt 6 ]; do
            _helix_wifi_connected && break
            _helix_wifi_apply_saved
            sleep 3
            _try=$((_try + 1))
        done
    ) &
    echo $! > "$HELIX_WIFI_FLAG" 2>/dev/null || true
    return 0
}

# ── Remote screen: serve the live UI to Mainsail/Fluidd ──────────────────────
#
# PAXX Extended Firmware ships a remote-screen tool, /usr/local/bin/fb-http.py,
# that serves the display over HTTP (127.0.0.1:8092, proxied by nginx's /screen/
# route into the [webcam gui] Moonraker slot). On firmware 1.4 the stock init
# started it from S99fb-http — but that is exactly the boot-glob script
# HelixScreen's autostart hijacks to launch HelixScreen instead, so under
# HelixScreen the remote screen never comes up.
#
# We start it from here instead. This hook runs on every HelixScreen start —
# i.e. on every boot, via the same hijacked init that already defeats the
# overlay boot-glob trap — so no separate (never-booting) S9x script is needed.
# We drive fb-http's DRM backend, which captures /dev/dri/card0 directly: that
# is the buffer HelixScreen renders to, so the remote view matches the panel
# with NO framebuffer mirroring (obviating the /dev/fb0 staleness problem — see
# docs/devel/printers/SNAPMAKER_U1_SUPPORT.md). Gated on the PAXX
# `web remote_screen` toggle so the firmware setting is honored.
HELIX_REMOTE_SCREEN_PID="${HELIX_REMOTE_SCREEN_PID:-/var/run/helix-remote-screen.pid}"
HELIX_FB_HTTP="${HELIX_FB_HTTP:-/usr/local/bin/fb-http.py}"
HELIX_FB_HTTP_HTML="${HELIX_FB_HTTP_HTML:-/usr/local/share/fb-http/html}"

# True (0) when the PAXX extended-firmware `web remote_screen` toggle is on.
# No toggle present (non-PAXX / older firmware) -> treat as disabled.
_remote_screen_enabled() {
    _cfg=""
    for _c in /oem/printer_data/config/extended/extended2.cfg \
              /home/lava/printer_data/config/extended/extended2.cfg; do
        [ -f "$_c" ] && { _cfg="$_c"; break; }
    done
    [ -n "$_cfg" ] || return 1
    [ -x /usr/local/bin/extended-config.py ] || return 1
    _rs=$(/usr/local/bin/extended-config.py get "$_cfg" web remote_screen false 2>/dev/null)
    case "$_rs" in [Tt][Rr][Uu][Ee]) return 0 ;; *) return 1 ;; esac
}

# Echo the backend-selection flags for the installed fb-http, or nothing.
#
# fb-http ships in two incompatible flavors:
#   - Newer builds (current PAXX Extended Firmware) expose a DRM backend
#     (--backend/--drm-device/--drm-wait) that captures /dev/dri/card0 — the
#     buffer HelixScreen renders to — directly, so the remote view matches the
#     panel with no framebuffer mirroring.
#   - Older/mainline builds (paxx12/screen-apps) register only
#     --port/--bind/--fb/--touch/--html-dir and read /dev/fb0.
# Passing the DRM flags to a build that lacks them makes argparse exit(2) before
# it binds :8092 — a silent dead feed. So probe the tool (its source advertises
# every flag it accepts) and only emit DRM flags when it actually supports them;
# otherwise fb-http falls back to its own fbdev default (/dev/fb0). Split out as
# a helper so the capability probe is unit-testable.
_remote_screen_backend_args() {
    if grep -q -- '--backend' "$HELIX_FB_HTTP" 2>/dev/null; then
        echo "--backend drm --drm-device /dev/dri/card0 --drm-wait 60"
    fi
}

# Start fb-http (idempotent). No-op if the tool is absent or the toggle is off.
start_remote_screen() {
    [ -f "$HELIX_FB_HTTP" ] || return 0
    _remote_screen_enabled || return 0
    if [ -f "$HELIX_REMOTE_SCREEN_PID" ] && \
       kill -0 "$(cat "$HELIX_REMOTE_SCREEN_PID" 2>/dev/null)" 2>/dev/null; then
        return 0
    fi
    _rs_backend=$(_remote_screen_backend_args)
    if [ -n "$_rs_backend" ]; then
        # --drm-wait lets fb-http wait for the DRM device to be ready, so it is
        # safe to launch here (before helix-screen becomes DRM master); capture
        # is read-only and does not contend for DRM master.
        echo "Remote screen: starting fb-http (DRM capture of /dev/dri/card0) on 127.0.0.1:8092"
    else
        # No DRM backend: fb-http reads /dev/fb0. HelixScreen renders into its own
        # DRM dumb buffer and never touches fb0, so fb0 would be stale — UNLESS the
        # in-app fb0 mailbox mirror is enabled. Export HELIX_REMOTE_SCREEN_FB0 so
        # helix-screen (launched after this hook, inheriting our env) mirrors each
        # rendered frame into /dev/fb0, making fb-http's snapshot the live UI. Only
        # on the fbdev path — the DRM branch captures the real buffer directly and
        # needs no mirror. See docs/devel/printers/SNAPMAKER_U1_SUPPORT.md.
        export HELIX_REMOTE_SCREEN_FB0="/dev/fb0"
        echo "Remote screen: starting fb-http (fbdev /dev/fb0) + in-app fb0 mirror on 127.0.0.1:8092"
    fi
    # Output is discarded to /dev/null: fb-http is a long-lived daemon (whole UI
    # session) and Mainsail/Fluidd poll /screen/ continuously, so its per-request
    # logging is unbounded. On the U1 /tmp is tmpfs, and an unbounded log there
    # starves Klipper — the failure mode that filled tmpfs with 498 MB of stray
    # output on this platform (see docs/devel/LOGGING.md and the LOGFILE tmpfs
    # guard in config/helixscreen.init).
    #
    # $_rs_backend is deliberately unquoted so it word-splits into flags (or
    # vanishes when empty); it is a fixed internal string, never user input.
    start-stop-daemon -S -b -m -p "$HELIX_REMOTE_SCREEN_PID" -x /bin/sh -- -c \
        "exec /usr/bin/python3 $HELIX_FB_HTTP --bind 127.0.0.1 --port 8092 $_rs_backend --html-dir $HELIX_FB_HTTP_HTML >/dev/null 2>&1"
    unset _rs_backend
}

# Stop fb-http if we started it.
#
# If fb-http exits on its own before we stop (startup failure/crash), the pidfile
# goes stale and its PID may be recycled to an unrelated process. `start-stop-daemon
# -K -p` matches the pidfile ALONE, so it would then TERM whatever now owns that PID.
# `--exec`/`--name` are unreliable here (we exec python3 through a /bin/sh wrapper,
# so the running comm/exe is python3, not fb-http), so we verify the process's
# cmdline still references fb-http before signaling, and always clear the pidfile.
stop_remote_screen() {
    if [ -f "$HELIX_REMOTE_SCREEN_PID" ]; then
        _rs_pid=$(cat "$HELIX_REMOTE_SCREEN_PID" 2>/dev/null)
        _fb_name=$(basename "$HELIX_FB_HTTP")
        if [ -n "$_rs_pid" ] && kill -0 "$_rs_pid" 2>/dev/null && \
           tr '\0' ' ' < "/proc/$_rs_pid/cmdline" 2>/dev/null | grep -qF "$_fb_name"; then
            start-stop-daemon -K -p "$HELIX_REMOTE_SCREEN_PID" -s TERM 2>/dev/null || \
                kill -TERM "$_rs_pid" 2>/dev/null || true
        fi
        rm -f "$HELIX_REMOTE_SCREEN_PID"
        unset _rs_pid _fb_name
    else
        pkill -f "$HELIX_FB_HTTP" 2>/dev/null || true
    fi
}

platform_pre_start() {
    # Cache and logs live beside the payload, never inside it. The install root
    # is what an update replaces, and not only by our own hand: a Moonraker
    # `type: web` entry does shutil.rmtree(path) before extracting. Anything
    # under it goes on every update - logs vanish exactly when someone needs
    # them, and the thumbnail cache is rebuilt from nothing.
    export HELIX_CACHE_DIR="/userdata/helixscreen-state/cache"
    # Force DRM device — skip auto-detection which may race with connector state
    export HELIX_DRM_DEVICE="/dev/dri/card0"

    # Recover the camera supervisor if a prior helixscreen build killed it.
    # Idempotent: no-op when lmd is already alive.
    ensure_lmd_running

    # Bring WiFi up independently of helix-screen's lifetime (detached worker).
    # Must NOT block boot or strand the device if helix later dies.
    ensure_wifi_associated

    # Serve the live UI to Mainsail/Fluidd via the firmware's fb-http tool when
    # the PAXX `web remote_screen` toggle is on (no-op otherwise).
    start_remote_screen

    return 0
}

platform_post_stop() {
    # Stop the remote-screen server we started in platform_pre_start.
    stop_remote_screen

    # Kill the keepalive process if still running
    if [ -n "$DRM_KEEPALIVE_PID" ]; then
        kill "$DRM_KEEPALIVE_PID" 2>/dev/null || true
        wait "$DRM_KEEPALIVE_PID" 2>/dev/null || true
        echo "DRM keepalive: cleaned up process $DRM_KEEPALIVE_PID"
        DRM_KEEPALIVE_PID=""
    fi

    # Do NOT restart /usr/bin/gui — the stock Snapmaker UI takes ownership of
    # wpa_supplicant on launch and drops the active WiFi connection, breaking
    # SSH and any in-flight install/update mid-stream (issue #797). Leaving the
    # display blank during stop is preferable to wedging the network. The stock
    # UI is restored by the uninstaller when the user explicitly removes us.
    return 0
}
