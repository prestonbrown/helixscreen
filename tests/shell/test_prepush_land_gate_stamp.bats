#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# pre-push trusting a make land-gate PASS (.githooks/lib/land-gate-stamp.sh).
# The hook runs against a fixture repo whose gates are stand-ins that each
# print their name, so a test reads which ones a push ran.

load helpers

HOOK="$BATS_TEST_DIRNAME/../../.githooks/pre-push"
STAMP_LIB="$BATS_TEST_DIRNAME/../../.githooks/lib/land-gate-stamp.sh"

setup() {
    export HELIX_LAND_GATE_STAMP_DIR="$BATS_TEST_TMPDIR/stamps"
    export HELIX_LAND_GATE_SHARED_STAMP_DIR="$BATS_TEST_TMPDIR/shared-stamps"
    # Every ssh is this fake: it logs the host, and runs the remote command
    # under sh as the host's login shell would, with that host's state
    # directory at $BATS_TEST_TMPDIR/remote/<host>. down.* refuses.
    mock_command_script ssh 'while [ $# -gt 0 ]; do case "$1" in -o) shift 2 ;; *) break ;; esac; done
echo "$1" >> "$BATS_TEST_TMPDIR/ssh-calls"
case "$1" in down.*) echo "ssh: connect to host $1 port 22: Connection refused" >&2; exit 255 ;; esac
XDG_STATE_HOME="$BATS_TEST_TMPDIR/remote/$1" exec sh -c "$2"'
    unset HELIX_PREPUSH_FULL HELIX_PREPUSH_IN_PLACE
    ROOT="$BATS_TEST_TMPDIR/repo"
    git init -q -b main "$ROOT"
    git -C "$ROOT" config user.email t@t
    git -C "$ROOT" config user.name t
    mkdir -p "$ROOT/scripts" "$ROOT/lib"
    printf '#!/bin/sh\necho "ran: quality-checks"\n' >"$ROOT/scripts/quality-checks.sh"
    chmod +x "$ROOT/scripts/quality-checks.sh"
    local gate
    for gate in check_clang_diagnostics esp32_syntax_check check_patch_drift; do
        printf 'print("ran: %s")\n' "$gate" >"$ROOT/scripts/$gate.py"
    done
    touch "$ROOT/lib/.keep"
    echo base >"$ROOT/f"
    git -C "$ROOT" add -A
    git -C "$ROOT" commit -qm base
    BASE=$(git -C "$ROOT" rev-parse HEAD)
    echo tip >"$ROOT/f"
    git -C "$ROOT" commit -qam tip
    TIP=$(git -C "$ROOT" rev-parse HEAD)
}

stamp() { # <sha>: what make land-gate records on a PASS
    (cd "$ROOT" && . "$STAMP_LIB" && land_gate_stamp_write "$1")
}

remote_stamp() { # <host> <sha>: a gate on that host passed sha, in a clone of its own
    (cd "$ROOT" && unset HELIX_LAND_GATE_SHARED_STAMP_DIR \
        && XDG_STATE_HOME="$BATS_TEST_TMPDIR/remote/$1" \
           HELIX_LAND_GATE_STAMP_DIR="$BATS_TEST_TMPDIR/clone-on-$1" \
           bash -c '. "$1" && land_gate_stamp_write "$2"' _ "$STAMP_LIB" "$2")
}

gate_hosts() { printf 'HELIX_LAND_GATE_HOSTS=%s\n' "$1" >"$HELIX_BUILD_HOSTS_FILE"; }

run_hook() { # push TIP to a remote that has BASE
    run env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
        bash -c 'cd "$1" && printf "refs/heads/main %s refs/heads/main %s\n" "$2" "$3" | bash "$4"' \
        _ "$ROOT" "$TIP" "$BASE" "$HOOK"
}

@test "a tip the land-gate passed skips the sweep and still runs ESP32 and drift" {
    stamp "$TIP"
    run_hook
    [ "$status" -eq 0 ]
    contains "make land-gate passed ${TIP:0:9}" "$output"
    lacks "ran: quality-checks" "$output"
    contains "ran: esp32_syntax_check" "$output"
    contains "ran: check_patch_drift" "$output"
}

@test "from a dirty tree, a gated tip skips the sweep and the clang gate" {
    stamp "$TIP"
    echo in-flight >"$ROOT/f"
    run_hook
    [ "$status" -eq 0 ]
    contains "throwaway checkout" "$output"
    contains "make land-gate passed ${TIP:0:9}" "$output"
    lacks "ran: quality-checks" "$output"
    lacks "ran: check_clang_diagnostics" "$output"
    contains "ran: esp32_syntax_check" "$output"
    contains "ran: check_patch_drift" "$output"
}

@test "a stamp for another commit skips nothing" {
    stamp "$BASE"
    run_hook
    [ "$status" -eq 0 ]
    lacks "land-gate passed" "$output"
    contains "ran: quality-checks" "$output"
    echo in-flight >"$ROOT/f"
    run_hook
    [ "$status" -eq 0 ]
    contains "ran: quality-checks" "$output"
    contains "ran: check_clang_diagnostics" "$output"
}

@test "HELIX_PREPUSH_FULL=1 sweeps a gated tip anyway" {
    stamp "$TIP"
    HELIX_PREPUSH_FULL=1 run_hook
    [ "$status" -eq 0 ]
    lacks "land-gate passed" "$output"
    contains "ran: quality-checks" "$output"
}

@test "a stamp written in a linked worktree covers the push from the main tree" {
    unset HELIX_LAND_GATE_STAMP_DIR
    git -C "$ROOT" worktree add -q --detach "$BATS_TEST_TMPDIR/wt" "$TIP"
    (cd "$BATS_TEST_TMPDIR/wt" && . "$STAMP_LIB" && land_gate_stamp_write "$TIP")
    [ -f "$ROOT/.git/helix-land-gate-pass/$TIP" ]
    (cd "$ROOT" && . "$STAMP_LIB" && land_gate_passed "$TIP")
    run bash -c 'cd "$1" && . "$2" && land_gate_passed "$3"' _ "$ROOT" "$STAMP_LIB" "$BASE"
    [ "$status" -ne 0 ]
}

@test "only a full lowercase hex SHA names a stamp" {
    stamp "$TIP"
    run bash -c '. "$1" && land_gate_passed ""' _ "$STAMP_LIB"
    [ "$status" -ne 0 ]
    run bash -c '. "$1" && land_gate_stamp_write "../escape"' _ "$STAMP_LIB"
    [ "$status" -ne 0 ]
    [ ! -e "$BATS_TEST_TMPDIR/escape" ]
}

@test "a gate host holding a record of the exact tip skips the sweep and caches it here" {
    remote_stamp gate.invalid "$TIP"
    gate_hosts down.invalid,gate.invalid
    run_hook
    [ "$status" -eq 0 ]
    contains "make land-gate passed ${TIP:0:9} on gate.invalid; skipping" "$output"
    lacks "ran: quality-checks" "$output"
    contains "ran: esp32_syntax_check" "$output"
    contains "ran: check_patch_drift" "$output"
    [ "$(cat "$BATS_TEST_TMPDIR/ssh-calls")" = "$(printf 'down.invalid\ngate.invalid')" ]
    grep -q "^host gate.invalid" "$HELIX_LAND_GATE_STAMP_DIR/$TIP"
    # The next push of the same tip finds the cached record and asks nobody.
    rm "$BATS_TEST_TMPDIR/ssh-calls"
    run_hook
    [ "$status" -eq 0 ]
    contains "make land-gate passed ${TIP:0:9}" "$output"
    lacks "ran: quality-checks" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-calls" ]
}

@test "a gate host whose record is for another commit runs the sweep, in one line" {
    remote_stamp gate.invalid "$BASE"
    gate_hosts gate.invalid
    run_hook
    [ "$status" -eq 0 ]
    contains "ran: quality-checks" "$output"
    lacks "land-gate passed" "$output"
    [ "$(grep -c "no make land-gate record" <<<"$output")" -eq 1 ]
    contains "no make land-gate record for ${TIP:0:9} on any gate host (gate.invalid: no record)" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$TIP" ]
}

@test "an unreachable gate host runs the sweep and says so in one line" {
    remote_stamp down.invalid "$TIP"
    gate_hosts down.invalid
    run_hook
    [ "$status" -eq 0 ]
    contains "ran: quality-checks" "$output"
    [ "$(grep -c "down.invalid" <<<"$output")" -eq 1 ]
    contains "(down.invalid: refused); running the sweep" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$TIP" ]
}

@test "with no gate hosts configured a push runs no ssh at all" {
    remote_stamp gate.invalid "$TIP"
    run_hook
    [ "$status" -eq 0 ]
    contains "ran: quality-checks" "$output"
    lacks "gate host" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-calls" ]
}

@test "from a dirty tree, a gate host's record skips the sweep as a local stamp does" {
    remote_stamp gate.invalid "$TIP"
    gate_hosts gate.invalid
    echo in-flight >"$ROOT/f"
    run_hook
    [ "$status" -eq 0 ]
    contains "throwaway checkout" "$output"
    contains "make land-gate passed ${TIP:0:9} on gate.invalid" "$output"
    lacks "ran: quality-checks" "$output"
    lacks "ran: check_clang_diagnostics" "$output"
    contains "ran: esp32_syntax_check" "$output"
    contains "ran: check_patch_drift" "$output"
}

@test "HELIX_PREPUSH_FULL=1 asks no gate host" {
    remote_stamp gate.invalid "$TIP"
    gate_hosts gate.invalid
    HELIX_PREPUSH_FULL=1 run_hook
    [ "$status" -eq 0 ]
    contains "ran: quality-checks" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-calls" ]
}

@test "a stamp is written to the clone and to the record every clone on the host shares" {
    stamp "$TIP"
    [ -f "$HELIX_LAND_GATE_STAMP_DIR/$TIP" ]
    [ -f "$HELIX_LAND_GATE_SHARED_STAMP_DIR/$TIP" ]
    grep -q "^make land-gate PASS at " "$HELIX_LAND_GATE_SHARED_STAMP_DIR/$TIP"
    grep -qx "host $(hostname)" "$HELIX_LAND_GATE_SHARED_STAMP_DIR/$TIP"
    grep -qx "repo $ROOT" "$HELIX_LAND_GATE_SHARED_STAMP_DIR/$TIP" \
        || grep -qx "repo $(cd "$ROOT" && pwd -P)" "$HELIX_LAND_GATE_SHARED_STAMP_DIR/$TIP"
    # Another clone on this host, with no stamp of its own, reads the shared one.
    rm "$HELIX_LAND_GATE_STAMP_DIR/$TIP"
    (cd "$ROOT" && . "$STAMP_LIB" && land_gate_passed "$TIP")
    run bash -c '. "$1" && land_gate_passed "$2"' _ "$STAMP_LIB" "$BASE"
    [ "$status" -ne 0 ]
}

@test "a SHA that is not hex, or a host name that is not one, never reaches ssh" {
    run env HELIX_LAND_GATE_HOSTS=gate.invalid bash -c '. "$1" && land_gate_ask_hosts "$2"' _ "$STAMP_LIB" "abc;touch x"
    [ "$status" -ne 0 ]
    run env HELIX_LAND_GATE_HOSTS='-oProxyCommand=x,a;b' bash -c '. "$1" && land_gate_ask_hosts "$2"' _ "$STAMP_LIB" "$TIP"
    [ "$status" -ne 0 ]
    contains "-oProxyCommand=x: not a host name" "$output"
    contains "a;b: not a host name" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-calls" ]
}
