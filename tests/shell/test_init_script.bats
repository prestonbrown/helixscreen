#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for helixscreen.init (SysV init script).
# Verifies HOME export and backup directory creation that prevent
# config loss on Moonraker updates (K1/AD5M).

INIT_SCRIPT="config/helixscreen.init"

setup() {
    load helpers
}

# =============================================================================
# HOME environment variable
# =============================================================================

@test "init script sets HOME when unset" {
    # Extract the HOME-setting block and verify it defaults to /root
    grep -q 'HOME.*=/root' "$INIT_SCRIPT"
}

@test "init script exports HOME" {
    grep -q 'export HOME' "$INIT_SCRIPT"
}

@test "init script does not override existing HOME" {
    # The := syntax means "set only if unset"
    grep -q '${HOME:=' "$INIT_SCRIPT"
}

# =============================================================================
# Backup directory creation
# =============================================================================

@test "init script creates /var/lib/helixscreen backup directory" {
    grep -q 'mkdir.*-p.*/var/lib/helixscreen' "$INIT_SCRIPT"
}

@test "init script creates HOME/.helixscreen fallback backup directory" {
    grep -q 'mkdir.*\.helixscreen' "$INIT_SCRIPT"
}

@test "backup dir creation does not fail if mkdir fails" {
    # The mkdir commands should have || true or similar to prevent script exit
    # on systems where /var/lib is read-only
    local line
    line=$(grep 'mkdir.*-p.*/var/lib/helixscreen' "$INIT_SCRIPT")
    echo "$line" | grep -qE '\|\|.*true|2>/dev/null'
}

# =============================================================================
# Pid file directory
# =============================================================================

# The whole script, run as `start` from a sandboxed install dir. /var/run is
# rewritten to $1; PATH hardening is dropped so the pidof mock resolves.
_run_init_start() { # run dir
    local T="$BATS_TEST_TMPDIR/inst"
    mkdir -p "$T/bin" "$T/config"
    printf '#!/bin/sh\nexec sleep 30\n' > "$T/bin/helix-screen"
    chmod +x "$T/bin/helix-screen"
    sed -e "s|/var/run|$1|g" -e 's|/usr/sbin:/usr/bin:/sbin:/bin:||' \
        "$INIT_SCRIPT" > "$T/config/helixscreen.init"
    mock_command_script "pidof" 'exit 1'
    HOME="$BATS_TEST_TMPDIR/home" TMPDIR="$BATS_TEST_TMPDIR/tmp" \
        run sh "$T/config/helixscreen.init" start
}

_kill_started() {
    local f
    for f in "$@"; do
        [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null
    done
    return 0
}

@test "init start creates a missing pid directory and reports the start" {
    local rd="$BATS_TEST_TMPDIR/chroot/var/run"
    _run_init_start "$rd"
    _kill_started "$rd/helixscreen.pid"

    [ "$status" -eq 0 ] || fail "start failed: $output"
    lacks "Failed to start" "$output"
    [ -s "$rd/helixscreen.pid" ] || fail "no pid file in the created directory"
}

@test "init start keeps its pid file in TMPDIR when the run dir is a dangling link" {
    local rd="$BATS_TEST_TMPDIR/chroot/var/run"
    mkdir -p "$BATS_TEST_TMPDIR/chroot/var" "$BATS_TEST_TMPDIR/tmp"
    ln -s ../run "$rd"
    _run_init_start "$rd"
    _kill_started "$BATS_TEST_TMPDIR/tmp/helixscreen.pid"

    [ "$status" -eq 0 ] || fail "start failed: $output"
    lacks "Failed to start" "$output"
    [ -s "$BATS_TEST_TMPDIR/tmp/helixscreen.pid" ] || fail "no pid file in TMPDIR"
}

# =============================================================================
# Launcher log location
# =============================================================================

# Runs `start` with the install at $S/usr/data/helixscreen, /var/log absent
# (the K1's tmpfs case) and /usr/data remapped under $S. The launcher stand-in
# answers --print-env, then writes one line and stays up, like the supervisor.
_run_init_start_k1() { # sandbox-root
    local S="$1" T="$1/usr/data/helixscreen"
    mkdir -p "$T/bin" "$T/config" "$S/run"
    printf '#!/bin/sh\n[ "$1" = --print-env ] && exit 0\necho launcher-alive\nexec sleep 3\n' > "$T/bin/helix-launcher.sh"
    printf '#!/bin/sh\nexec sleep 30\n' > "$T/bin/helix-screen"
    chmod +x "$T/bin/helix-launcher.sh" "$T/bin/helix-screen"
    sed -e "s|DAEMON_DIR=\"/opt/helixscreen\"|DAEMON_DIR=\"$T\"|" \
        -e "s|/var/run|$S/run|g" -e "s|/var/log|$S/nolog|g" \
        -e "s|/usr/data/|$S/usr/data/|g" \
        -e 's|/usr/sbin:/usr/bin:/sbin:/bin:||' \
        "$INIT_SCRIPT" > "$T/config/helixscreen.init"
    mock_command_script "pidof" 'exit 1'
    HOME="$S/home" TMPDIR="$S/tmp" run sh "$T/config/helixscreen.init" start
    _kill_started "$S/run/helixscreen.pid"
}

@test "launcher log lives in the state root, so an update swapping the install tree keeps it" {
    local S="$BATS_TEST_TMPDIR/k1"
    mkdir -p "$S/usr/data/helixscreen-state"
    _run_init_start_k1 "$S"
    [ "$status" -eq 0 ] || fail "start failed: $output"

    local log="$S/usr/data/helixscreen-state/logs/launcher.log"
    local _
    for _ in 1 2 3 4 5 6 7 8 9 10; do grep -q launcher-alive "$log" 2>/dev/null && break; sleep 0.2; done
    grep -q launcher-alive "$log" || fail "launcher output not in $log"
    [ ! -e "$S/usr/data/helixscreen/logs/launcher.log" ] || fail "log still written inside the install tree"

    # The update swap: the tree moves to .old, which is then deleted.
    mv "$S/usr/data/helixscreen" "$S/usr/data/helixscreen.old"
    rm -rf "$S/usr/data/helixscreen.old"
    grep -q launcher-alive "$log" || fail "log lost with the old tree"
}

@test "launcher log falls back to .helixscreen/logs beside the install when no state root exists" {
    local S="$BATS_TEST_TMPDIR/k1b"
    _run_init_start_k1 "$S"
    [ "$status" -eq 0 ] || fail "start failed: $output"

    local log="$S/usr/data/.helixscreen/logs/launcher.log"
    local _
    for _ in 1 2 3 4 5 6 7 8 9 10; do grep -q launcher-alive "$log" 2>/dev/null && break; sleep 0.2; done
    grep -q launcher-alive "$log" || fail "launcher output not in $log"
    [ ! -e "$S/usr/data/helixscreen/logs" ] || fail "install tree grew a logs dir"
}

# =============================================================================
# Without pidof (Z-Mod's chroot ships none)
# =============================================================================

# The script with no pidof to call: the mock answers as the shell does for a
# missing command. Processes are found through a fake /proc whose entries name
# sleeps this test started, so nothing outside the test can match.
# $2 "absent": the copy names a pidof that cannot exist, since the suite's
# sandbox keeps a pidof on PATH; otherwise the mock answers 127, as the shell
# does for a command it cannot find.
_init_without_pidof() { # sandbox-root [absent]
    local S="$1" T="$1/inst"
    mkdir -p "$T/bin" "$T/config" "$S/run" "$S/proc"
    printf '#!/bin/sh\nexit 0\n' > "$T/bin/helix-screen"
    chmod +x "$T/bin/helix-screen"
    sed -e "s|/var/run|$S/run|g" -e 's|/usr/sbin:/usr/bin:/sbin:/bin:||' \
        "$INIT_SCRIPT" > "$T/config/helixscreen.init"
    if [ "${2:-}" = absent ]; then
        sed -i 's/command -v pidof/command -v pidof_absent_in_this_test/' "$T/config/helixscreen.init"
        mock_command_script "pidof" 'echo pidof-called >&2; exit 1'
    else
        mock_command_script "pidof" 'exit 127'
    fi
}

_fake_proc() { # sandbox-root pid name
    mkdir -p "$1/proc/$2"
    echo "$3" > "$1/proc/$2/comm"
}

# A signalled child of this shell stays a zombie until reaped, and kill -0
# still succeeds on a zombie, so liveness is read from the process state.
_running() { # pid
    local state
    state=$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null) || return 1
    [ -n "$state" ] && [ "$state" != Z ]
}

_stop_without_pidof() { # mode
    local S="$BATS_TEST_TMPDIR/s" wd app decoy
    _init_without_pidof "$S" "$1"
    sleep 300 & wd=$!
    sleep 300 & app=$!
    sleep 300 & decoy=$!
    _fake_proc "$S" "$wd" helix-watchdog
    _fake_proc "$S" "$app" helix-screen
    _fake_proc "$S" "$decoy" helix-screen-x

    HOME="$S/home" HELIX_PROC_ROOT="$S/proc" run sh "$S/inst/config/helixscreen.init" stop
    local wd_alive=0 app_alive=0 decoy_alive=0
    _running "$wd" && wd_alive=1
    _running "$app" && app_alive=1
    _running "$decoy" && decoy_alive=1
    kill "$wd" "$app" "$decoy" 2>/dev/null || true

    [ "$status" -eq 0 ] || fail "stop failed: $output"
    [ "$wd_alive" -eq 0 ] || fail "helix-watchdog still running after stop"
    [ "$app_alive" -eq 0 ] || fail "helix-screen still running after stop"
    [ "$decoy_alive" -eq 1 ] || fail "stop ended a process whose name only starts with helix-screen"
    lacks "pidof-called" "$output"
}

@test "init stop without pidof ends the watchdog and app it finds in /proc" {
    _stop_without_pidof
}

@test "init stop with no pidof on PATH at all ends the watchdog and app it finds in /proc" {
    _stop_without_pidof absent
}

@test "init status without pidof reports an app it finds in /proc" {
    local S="$BATS_TEST_TMPDIR/s" app
    _init_without_pidof "$S"
    sleep 300 & app=$!
    _fake_proc "$S" "$app" helix-screen

    HOME="$S/home" HELIX_PROC_ROOT="$S/proc" run sh "$S/inst/config/helixscreen.init" status
    kill "$app" 2>/dev/null || true

    [ "$status" -eq 0 ] || fail "status said not running: $output"
}
