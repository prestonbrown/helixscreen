#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for platform hook files.
# Verifies the hook contract (all required functions defined),
# shellcheck compliance, and basic syntax validity.

load helpers

HOOKS_DIR="assets/config/platform"
HOOK_FILES="hooks-ad5m-forgex.sh hooks-ad5m-kmod.sh hooks-ad5m-zmod.sh hooks-pi.sh hooks-k1.sh hooks-k2.sh hooks-snapmaker-u1.sh"
REQUIRED_FUNCTIONS="platform_stop_competing_uis platform_enable_backlight platform_wait_for_services platform_pre_start platform_post_stop"

# --- Hook contract tests: every hook file must define all 5 functions ---

@test "forgex hooks define all required functions" {
    ( . "$HOOKS_DIR/hooks-ad5m-forgex.sh"
      for func in $REQUIRED_FUNCTIONS; do
          type "$func" >/dev/null 2>&1
      done )
}

@test "kmod hooks define all required functions" {
    ( . "$HOOKS_DIR/hooks-ad5m-kmod.sh"
      for func in $REQUIRED_FUNCTIONS; do
          type "$func" >/dev/null 2>&1
      done )
}

@test "pi hooks define all required functions" {
    ( . "$HOOKS_DIR/hooks-pi.sh"
      for func in $REQUIRED_FUNCTIONS; do
          type "$func" >/dev/null 2>&1
      done )
}

@test "zmod hooks define all required functions" {
    ( . "$HOOKS_DIR/hooks-ad5m-zmod.sh"
      for func in $REQUIRED_FUNCTIONS; do
          type "$func" >/dev/null 2>&1
      done )
}

# The ad5m-zmod hook is shared by AD5M-ZMOD and AD5X (scripts/lib/installer/main.sh
# maps platform ad5x -> hook ad5m-zmod). Its app-log path must stay under
# /opt/config: ghzserg's TAR_CONFIG archiver collects /opt/config/ (plus
# /usr/prog/config/, /usr/data/logs/ on AD5X and /data/logFiles/ on AD5M) and
# nothing under /data/helixscreen/, so moving the log back to /data makes it
# invisible to every support archive a user can send us. That is the exact
# regression that reopens #1249.
@test "zmod hook logs under /opt/config so ZMOD's TAR_CONFIG archives it" {
    run sh -c '
        HOOKS_DIR="'"$HOOKS_DIR"'"
        . "$HOOKS_DIR/hooks-ad5m-zmod.sh"
        # Neutralize the hook side effects; we only want the exported vars.
        mkdir() { :; }
        touch() { :; }
        platform_pre_start >/dev/null 2>&1
        echo "dest=$HELIX_LOG_DEST"
        echo "file=$HELIX_LOG_FILE"
    '
    [ "$status" -eq 0 ]

    # The file sink must be explicitly selected — "auto" never resolves to File.
    [[ "$output" == *"dest=file"* ]]

    log_file=$(echo "$output" | sed -n 's/^file=//p')
    [ -n "$log_file" ]
    case "$log_file" in
        /opt/config/*) ;;
        *) echo "HELIX_LOG_FILE must live under /opt/config/ (got '$log_file')" >&2
           return 1 ;;
    esac

    # NOT helixscreen.log: ghzserg's S80helixscreen init script owns that name
    # in the same directory for the launcher/stderr redirect stream.
    case "$log_file" in
        */helixscreen.log)
           echo "HELIX_LOG_FILE collides with ghzserg's launcher redirect target" >&2
           return 1 ;;
    esac
}

@test "zmod hook creates its log directory before the app needs it" {
    grep -q 'mkdir -p "/opt/config/mod_data/log"' "$HOOKS_DIR/hooks-ad5m-zmod.sh"
}

@test "zmod hook keeps rotation tight for the 107 MB AD5M" {
    run sh -c '
        HOOKS_DIR="'"$HOOKS_DIR"'"
        . "$HOOKS_DIR/hooks-ad5m-zmod.sh"
        mkdir() { :; }
        touch() { :; }
        platform_pre_start >/dev/null 2>&1
        echo "bytes=$HELIX_LOG_ROTATE_BYTES files=$HELIX_LOG_ROTATE_FILES"
    '
    [ "$status" -eq 0 ]
    [[ "$output" == *"bytes=1048576"* ]]
    [[ "$output" == *"files=3"* ]]
}

@test "k1 hooks define all required functions" {
    ( . "$HOOKS_DIR/hooks-k1.sh"
      for func in $REQUIRED_FUNCTIONS; do
          type "$func" >/dev/null 2>&1
      done )
}

@test "snapmaker-u1 hooks define all required functions" {
    ( . "$HOOKS_DIR/hooks-snapmaker-u1.sh"
      for func in $REQUIRED_FUNCTIONS; do
          type "$func" >/dev/null 2>&1
      done )
}

# --- Shellcheck compliance ---

@test "forgex hooks pass shellcheck" {
    shellcheck -s sh "$HOOKS_DIR/hooks-ad5m-forgex.sh"
}

@test "kmod hooks pass shellcheck" {
    shellcheck -s sh "$HOOKS_DIR/hooks-ad5m-kmod.sh"
}

@test "pi hooks pass shellcheck" {
    shellcheck -s sh "$HOOKS_DIR/hooks-pi.sh"
}

@test "zmod hooks pass shellcheck" {
    shellcheck -s sh "$HOOKS_DIR/hooks-ad5m-zmod.sh"
}

@test "k1 hooks pass shellcheck" {
    shellcheck -s sh "$HOOKS_DIR/hooks-k1.sh"
}

@test "snapmaker-u1 hooks pass shellcheck" {
    shellcheck -s sh "$HOOKS_DIR/hooks-snapmaker-u1.sh"
}

# --- Syntax validity ---

@test "forgex hooks have valid sh syntax" {
    sh -n "$HOOKS_DIR/hooks-ad5m-forgex.sh"
}

@test "kmod hooks have valid sh syntax" {
    sh -n "$HOOKS_DIR/hooks-ad5m-kmod.sh"
}

@test "pi hooks have valid sh syntax" {
    sh -n "$HOOKS_DIR/hooks-pi.sh"
}

@test "zmod hooks have valid sh syntax" {
    sh -n "$HOOKS_DIR/hooks-ad5m-zmod.sh"
}

@test "k1 hooks have valid sh syntax" {
    sh -n "$HOOKS_DIR/hooks-k1.sh"
}

@test "snapmaker-u1 hooks have valid sh syntax" {
    sh -n "$HOOKS_DIR/hooks-snapmaker-u1.sh"
}

@test "k2 hooks define all required functions" {
    ( . "$HOOKS_DIR/hooks-k2.sh"
      for func in $REQUIRED_FUNCTIONS; do
          type "$func" >/dev/null 2>&1
      done )
}

@test "k2 hooks pass shellcheck" {
    shellcheck -s sh "$HOOKS_DIR/hooks-k2.sh"
}

@test "k2 hooks have valid sh syntax" {
    sh -n "$HOOKS_DIR/hooks-k2.sh"
}

@test "k2 gate writes splash heartbeat and detects a ready Moonraker fast" {
    # Behavioral check of the Moonraker gate: against an already-ready endpoint
    # it must return 0 quickly and leave the splash status file in the
    # "ready/handing off" state. Uses a local mock HTTP server so the test
    # never touches a real printer.
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"

    local tmp port status_file
    tmp="$(mktemp -d)"
    status_file="$tmp/splash-status"
    port=7191

    # Minimal mock Moonraker /server/info on 127.0.0.1:$port
    python3 -c "
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self): self.send_response(200); self.end_headers(); self.wfile.write(b'{}')
    def log_message(self, *a): pass
http.server.HTTPServer(('127.0.0.1', $port), H).serve_forever()
" &
    local server_pid=$!
    sleep 1

    HELIX_MOONRAKER_READY_URL="http://127.0.0.1:$port/server/info" \
        HELIX_MOONRAKER_WAIT_TIMEOUT=10 \
        HELIX_SPLASH_STATUS_FILE="$status_file" \
        bash -c ". '$HOOKS_DIR/hooks-k2.sh'; platform_wait_for_services"
    local rc=$?

    kill "$server_pid" 2>/dev/null || true
    [ "$rc" -eq 0 ]
    grep -q "HelixScreen" "$status_file"
    rm -rf "$tmp"
}

@test "k2 gate times out cleanly and marks splash 'no printer'" {
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"

    local tmp status_file
    tmp="$(mktemp -d)"
    status_file="$tmp/splash-status"

    # Point at a closed port with a short timeout: gate must return non-zero
    # and leave the "starting without printer" message for the splash.
    run env HELIX_MOONRAKER_READY_URL="http://127.0.0.1:1/server/info" \
        HELIX_MOONRAKER_WAIT_TIMEOUT=2 \
        HELIX_SPLASH_STATUS_FILE="$status_file" \
        bash -c ". '$HOOKS_DIR/hooks-k2.sh'; platform_wait_for_services"

    [ "$status" -ne 0 ]
    grep -q "without printer" "$status_file"
    rm -rf "$tmp"
}

# --- Init script integration tests ---

INIT_SCRIPT="config/helixscreen.init"

@test "init script defines no-op defaults" {
    # The no-op defaults should be defined even without a hook file present
    grep -q 'platform_stop_competing_uis().*:' "$INIT_SCRIPT"
    grep -q 'platform_enable_backlight().*:' "$INIT_SCRIPT"
    grep -q 'platform_wait_for_services().*:' "$INIT_SCRIPT"
    grep -q 'platform_pre_start().*:' "$INIT_SCRIPT"
    grep -q 'platform_post_stop().*:' "$INIT_SCRIPT"
}

@test "init script sources platform hooks" {
    grep -q 'PLATFORM_HOOKS' "$INIT_SCRIPT"
    grep -q '\. "\$PLATFORM_HOOKS"' "$INIT_SCRIPT"
}

@test "init script has no inline backlight function" {
    # The enable_backlight function should no longer exist
    ! grep -q '^enable_backlight()' "$INIT_SCRIPT"
}

@test "init script has no inline Moonraker wait function" {
    # The wait_for_moonraker function should no longer exist
    ! grep -q '^wait_for_moonraker()' "$INIT_SCRIPT"
}

@test "init script has no inline competing UI function" {
    # The stop_competing_uis function should no longer exist
    ! grep -q '^stop_competing_uis()' "$INIT_SCRIPT"
}

@test "init script calls platform hooks in start" {
    grep -q 'platform_pre_start' "$INIT_SCRIPT"
    grep -q 'platform_stop_competing_uis' "$INIT_SCRIPT"
    grep -q 'platform_enable_backlight' "$INIT_SCRIPT"
    grep -q 'platform_wait_for_services' "$INIT_SCRIPT"
}

@test "init script calls platform hooks in stop" {
    grep -q 'platform_post_stop' "$INIT_SCRIPT"
}

@test "init script passes sh syntax check" {
    sh -n "$INIT_SCRIPT"
}

@test "init script has start/stop/restart/status cases" {
    grep -q 'start)' "$INIT_SCRIPT"
    grep -q 'stop)' "$INIT_SCRIPT"
    grep -q 'restart' "$INIT_SCRIPT"
    grep -q 'status)' "$INIT_SCRIPT"
}

# --- Bundle integrity tests ---

@test "bundled installer passes syntax check" {
    sh -n scripts/install.sh
}

@test "bundled installer contains configure_platform" {
    grep -q 'configure_platform' scripts/install.sh
}

@test "bundled installer contains deploy_platform_hooks" {
    grep -q 'deploy_platform_hooks' scripts/install.sh
}

@test "bundled installer contains detect_klipper_user" {
    grep -q 'detect_klipper_user' scripts/install.sh
}

@test "bundled installer contains record_disabled_service" {
    grep -q 'record_disabled_service' scripts/install.sh
}

@test "bundled installer contains fix_install_ownership" {
    grep -q 'fix_install_ownership' scripts/install.sh
}

@test "bundled installer contains detect_pi_install_dir function" {
    grep -q 'detect_pi_install_dir()' scripts/install.sh
}

@test "bundled installer calls detect_pi_install_dir in Pi branch" {
    # The Pi branch of set_install_paths must call detect_pi_install_dir
    grep -A5 'detect_klipper_user' scripts/install.sh | grep -q 'detect_pi_install_dir'
}

@test "bundled installer captures _USER_INSTALL_DIR" {
    grep -q '_USER_INSTALL_DIR=' scripts/install.sh
}

# --- Parity tests: platform.sh functions must exist in bundled install.sh ---
# These catch the case where a function is added to platform.sh but not
# to the bundled installer (which is a separate copy of the modules).

@test "parity: all platform.sh functions exist in bundled installer" {
    # Extract function names from platform.sh
    local funcs
    funcs=$(grep -E '^[a-z_]+\(\)' scripts/lib/installer/platform.sh | sed 's/().*//')
    for func in $funcs; do
        if ! grep -q "${func}()" scripts/install.sh; then
            echo "MISSING in install.sh: ${func}()"
            return 1
        fi
    done
}

@test "parity: Pi branch in install.sh matches platform.sh" {
    # Both files should call detect_pi_install_dir (not hardcode /opt/helixscreen)
    # in their Pi/else branch of set_install_paths
    refute_sh "grep -A3 'Pi and other platforms' scripts/install.sh | grep -q 'INSTALL_DIR=\"/opt/helixscreen\"'"
    refute_sh "grep -A3 'Pi and other platforms' scripts/lib/installer/platform.sh | grep -q 'INSTALL_DIR=\"/opt/helixscreen\"'"
}

# --- Snapmaker U1 remote screen (fb-http) ---

@test "snapmaker-u1 hooks define remote-screen functions" {
    ( . "$HOOKS_DIR/hooks-snapmaker-u1.sh"
      for func in _remote_screen_enabled start_remote_screen stop_remote_screen; do
          type "$func" >/dev/null 2>&1
      done )
}

@test "start_remote_screen is a no-op when fb-http is absent" {
    run sh -c '
        . "'"$HOOKS_DIR"'/hooks-snapmaker-u1.sh"
        HELIX_FB_HTTP="/nonexistent/fb-http.py"
        HELIX_REMOTE_SCREEN_PID="$(mktemp -u)"
        start_remote_screen
        rc=$?
        [ ! -f "$HELIX_REMOTE_SCREEN_PID" ] || echo "PIDFILE-CREATED"
        exit $rc
    '
    [ "$status" -eq 0 ]
    [[ "$output" != *"PIDFILE-CREATED"* ]]
}

@test "stop_remote_screen clears a stale pidfile safely" {
    run sh -c '
        . "'"$HOOKS_DIR"'/hooks-snapmaker-u1.sh"
        pf="$(mktemp)"
        echo 999999 > "$pf"          # a PID that is not running
        HELIX_REMOTE_SCREEN_PID="$pf"
        stop_remote_screen
        rc=$?
        [ ! -f "$pf" ] || echo "PIDFILE-LEFT"
        exit $rc
    '
    [ "$status" -eq 0 ]
    [[ "$output" != *"PIDFILE-LEFT"* ]]
}

@test "platform_pre_start starts remote screen; post_stop stops it" {
    # Both hooks must reference the remote-screen lifecycle so the feature is
    # actually wired into HelixScreen's start/stop path.
    grep -A25 '^platform_pre_start()' "$HOOKS_DIR/hooks-snapmaker-u1.sh" | grep -q 'start_remote_screen'
    grep -A15 '^platform_post_stop()' "$HOOKS_DIR/hooks-snapmaker-u1.sh" | grep -q 'stop_remote_screen'
}

@test "start_remote_screen discards fb-http output (no unbounded tmpfs log)" {
    # Regression guard: fb-http is long-lived and /screen/ is polled continuously,
    # so its request log is unbounded. On the U1 /tmp is tmpfs, and an unbounded
    # log there starves Klipper (the 498 MB tmpfs-fill failure). The launch must
    # discard output to /dev/null, never redirect into a growable /tmp file.
    refute grep -q '/tmp/fb-http\.log' "$HOOKS_DIR/hooks-snapmaker-u1.sh"
    grep -A2 '^    start-stop-daemon -S' "$HOOKS_DIR/hooks-snapmaker-u1.sh" | grep -q '>/dev/null 2>&1'
}

@test "backend probe selects DRM flags when fb-http advertises --backend" {
    run sh -c '
        . "'"$HOOKS_DIR"'/hooks-snapmaker-u1.sh"
        f="$(mktemp)"
        printf "%s\n" "    parser.add_argument(\"--backend\", default=\"auto\")" > "$f"
        HELIX_FB_HTTP="$f"
        _remote_screen_backend_args
        rm -f "$f"
    '
    [ "$status" -eq 0 ]
    [[ "$output" == *"--backend drm"* ]]
    [[ "$output" == *"--drm-device /dev/dri/card0"* ]]
    [[ "$output" == *"--drm-wait 60"* ]]
}

@test "backend probe emits no DRM flags on a fbdev-only fb-http" {
    run sh -c '
        . "'"$HOOKS_DIR"'/hooks-snapmaker-u1.sh"
        f="$(mktemp)"
        printf "%s\n" "    parser.add_argument(\"--fb\", default=\"/dev/fb0\")" > "$f"
        HELIX_FB_HTTP="$f"
        out="$(_remote_screen_backend_args)"
        [ -z "$out" ] || echo "UNEXPECTED:$out"
        rm -f "$f"
    '
    [ "$status" -eq 0 ]
    [[ "$output" != *"--backend"* ]]
    [[ "$output" != *"UNEXPECTED"* ]]
}

@test "stop_remote_screen does not signal a recycled unrelated PID" {
    # If fb-http died and its PID was recycled to an unrelated process, the stale
    # pidfile must NOT cause that process to be TERM'd. stop_remote_screen verifies
    # the PID's cmdline references fb-http before signaling.
    run sh -c '
        . "'"$HOOKS_DIR"'/hooks-snapmaker-u1.sh"
        sleep 30 &                   # stand-in for an unrelated, recycled-PID process
        upid=$!
        pf="$(mktemp)"
        echo "$upid" > "$pf"         # pidfile points at the unrelated live PID
        HELIX_REMOTE_SCREEN_PID="$pf"
        stop_remote_screen
        rc=$?
        if kill -0 "$upid" 2>/dev/null; then echo "SURVIVED"; kill "$upid" 2>/dev/null; fi
        [ ! -f "$pf" ] || echo "PIDFILE-LEFT"
        exit $rc
    '
    [ "$status" -eq 0 ]
    [[ "$output" == *"SURVIVED"* ]]      # unrelated process must NOT be killed
    [[ "$output" != *"PIDFILE-LEFT"* ]]  # stale pidfile is still cleaned up
}

@test "start_remote_screen exports HELIX_REMOTE_SCREEN_FB0 on the fbdev path" {
    # On a fbdev-only fb-http (no DRM backend), the hook must export
    # HELIX_REMOTE_SCREEN_FB0 so helix-screen's in-app fb0 mirror activates and
    # fb-http serves the live UI instead of a stale /dev/fb0 frame.
    run sh -c '
        stubdir="$(mktemp -d)"
        printf "#!/bin/sh\nexit 0\n" > "$stubdir/start-stop-daemon"
        chmod +x "$stubdir/start-stop-daemon"
        PATH="$stubdir:$PATH"
        . "'"$HOOKS_DIR"'/hooks-snapmaker-u1.sh"
        _remote_screen_enabled() { return 0; }   # force the toggle on
        f="$(mktemp)"
        printf "%s\n" "    parser.add_argument(\"--fb\", default=\"/dev/fb0\")" > "$f"  # fbdev-only, no --backend
        HELIX_FB_HTTP="$f"
        HELIX_REMOTE_SCREEN_PID="$(mktemp -u)"
        start_remote_screen
        echo "FB0=${HELIX_REMOTE_SCREEN_FB0:-UNSET}"
        rm -f "$f"; rm -rf "$stubdir"
    '
    [ "$status" -eq 0 ]
    [[ "$output" == *"FB0=/dev/fb0"* ]]
}

@test "start_remote_screen does NOT export HELIX_REMOTE_SCREEN_FB0 on the DRM path" {
    # A DRM-capable fb-http captures /dev/dri/card0 directly; the in-app mirror is
    # unnecessary there, so the env var must stay unset.
    run sh -c '
        stubdir="$(mktemp -d)"
        printf "#!/bin/sh\nexit 0\n" > "$stubdir/start-stop-daemon"
        chmod +x "$stubdir/start-stop-daemon"
        PATH="$stubdir:$PATH"
        . "'"$HOOKS_DIR"'/hooks-snapmaker-u1.sh"
        _remote_screen_enabled() { return 0; }
        f="$(mktemp)"
        printf "%s\n" "    parser.add_argument(\"--backend\", default=\"auto\")" > "$f"  # DRM-capable
        HELIX_FB_HTTP="$f"
        HELIX_REMOTE_SCREEN_PID="$(mktemp -u)"
        start_remote_screen
        echo "FB0=${HELIX_REMOTE_SCREEN_FB0:-UNSET}"
        rm -f "$f"; rm -rf "$stubdir"
    '
    [ "$status" -eq 0 ]
    [[ "$output" == *"FB0=UNSET"* ]]
}
