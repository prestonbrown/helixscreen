#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for ensure_lmd_running in hooks-snapmaker-u1.sh.
#
# lmd (the U1 camera/MQTT supervisor) is started once at boot by a firmware
# init script and nothing restarts it. The hook restarts it through that same
# script, found by content (whichever /etc/init.d/S* launches /usr/bin/lmd), so
# stock and paxx firmware each set lmd up their own way.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/assets/config/platform/hooks-snapmaker-u1.sh"

setup() {
    export HELIX_INITD_DIR="$BATS_TEST_TMPDIR/init.d"
    export INIT_LOG="$BATS_TEST_TMPDIR/init_calls.log"
    mkdir -p "$HELIX_INITD_DIR" "$BATS_TEST_TMPDIR/bin"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    mock_pidof_lmd ""
}

# pidof stub: prints $1 (and succeeds) only when asked about lmd.
mock_pidof_lmd() {
    cat > "$BATS_TEST_TMPDIR/bin/pidof" <<STUB
#!/bin/sh
if [ "\$1" = "lmd" ] && [ -n "$1" ]; then echo "$1"; exit 0; fi
exit 1
STUB
    chmod +x "$BATS_TEST_TMPDIR/bin/pidof"
}

# An init script named $1 that records how it was called. With $2 = lmd it
# launches /usr/bin/lmd the way the stock 2.1.x S90lmd does.
make_init() {
    local launches=""
    [ "${2:-}" = "lmd" ] && launches='start-stop-daemon -S -b -x /usr/bin/lmd -- -c "$APPS_CGROUP"'
    cat > "$HELIX_INITD_DIR/$1" <<STUB
#!/bin/sh
# $launches
echo "$1 \$* imposter=\${V4L2_IMPOSTER_SOCKET_PATH:-none}" >> "$INIT_LOG"
STUB
    chmod +x "$HELIX_INITD_DIR/$1"
}

@test "lmd already running: no init script is called" {
    make_init S90lmd lmd
    mock_pidof_lmd 1234
    ( . "$HOOK"; ensure_lmd_running )
    [ ! -e "$INIT_LOG" ]
}

@test "lmd not running: its init script is run once with start, without the old imposter env" {
    make_init S50dropbear
    make_init S90lmd lmd
    make_init S99input-event-daemon
    ( . "$HOOK"; ensure_lmd_running )
    [ "$(cat "$INIT_LOG")" = "S90lmd start imposter=none" ]
}

@test "stock firmware: lmd is restarted even without paxx's capture socket" {
    [ ! -e /tmp/capture-mipi-raw.sock ] || skip "host has a capture socket"
    make_init S90lmd lmd
    ( . "$HOOK"; ensure_lmd_running )
    grep -q '^S90lmd start' "$INIT_LOG"
}

@test "the init script is found by what it launches, not by its name" {
    make_init S42camera lmd
    make_init S90lmd
    ( . "$HOOK"; ensure_lmd_running )
    [ "$(cat "$INIT_LOG")" = "S42camera start imposter=none" ]
}

@test "no init script launches lmd: nothing runs and the hook succeeds" {
    make_init S50dropbear
    run sh -c '. "'"$HOOK"'"; ensure_lmd_running'
    [ "$status" -eq 0 ]
    [ ! -e "$INIT_LOG" ]
}

@test "a non-executable script that mentions lmd is skipped" {
    make_init S90lmd lmd
    chmod -x "$HELIX_INITD_DIR/S90lmd"
    run sh -c '. "'"$HOOK"'"; ensure_lmd_running'
    [ "$status" -eq 0 ]
    [ ! -e "$INIT_LOG" ]
}
