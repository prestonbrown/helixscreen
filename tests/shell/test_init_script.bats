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
