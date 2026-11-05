#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Liveness on a box with no /proc. A claim's owner is a pid plus that pid's
# start time; where /proc is missing (a Mac) both come from ps(1). Reading only
# /proc there makes every claim STALE, so a second session takes a tree the
# first is still holding. HELIX_CLAIM_PROC points the /proc reads at a missing
# directory so the ps path runs here on Linux as well.

load helpers

CLAIM="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/helix-claim"

setup() {
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    export HELIX_CLAIM_PROC="$BATS_TEST_TMPDIR/no-proc"
    mkdir -p "$HELIX_CLAIM_DIR"
    sleep 120 &
    OWNER=$!
}

teardown() {
    [ -n "${OWNER:-}" ] && kill "$OWNER" 2>/dev/null
    return 0
}

@test "without /proc a claim reads LIVE while its owner runs and STALE once it exits" {
    run "$CLAIM" take heavy:probe probing --pid "$OWNER"
    [ "$status" -eq 0 ]
    run "$CLAIM" check heavy:probe
    [ "$status" -eq 1 ]
    contains "LIVE" "$output"

    kill "$OWNER"
    wait "$OWNER" 2>/dev/null || true
    OWNER=""
    run "$CLAIM" check heavy:probe
    [ "$status" -eq 0 ]
    contains "STALE" "$output"
}

@test "without /proc a claim stays LIVE when the system timezone changes" {
    TZ=America/Los_Angeles run "$CLAIM" take heavy:probe probing --pid "$OWNER"
    [ "$status" -eq 0 ]
    TZ=America/New_York run "$CLAIM" check heavy:probe
    [ "$status" -eq 1 ]
    contains "LIVE" "$output"
}

@test "without /proc a claim records a start time, and a different one reads STALE" {
    "$CLAIM" take heavy:probe probing --pid "$OWNER" >/dev/null
    local f="$HELIX_CLAIM_DIR/heavy_probe.json"
    grep -qE '"pid_start": "[^"]+"' "$f"

    # The same pid with another start time is a later process reusing it.
    sed 's/"pid_start": "[^"]*"/"pid_start": "Thu Jan  1 00:00:00 1970"/' "$f" > "$f.new"
    mv "$f.new" "$f"
    run "$CLAIM" check heavy:probe
    contains "STALE" "$output"
}

@test "without /proc a second take is refused while the first owner runs" {
    "$CLAIM" take heavy:probe first --pid "$OWNER" >/dev/null
    sleep 120 &
    local other=$!
    run "$CLAIM" take heavy:probe second --pid "$other"
    kill "$other" 2>/dev/null
    [ "$status" -eq 1 ]
    contains "REFUSED" "$output"
}

@test "without /proc run starts the command, claims it, and releases afterwards" {
    run "$CLAIM" run heavy:probe -- sh -c 'echo ran-here'
    [ "$status" -eq 0 ]
    contains "ran-here" "$output"
    run "$CLAIM" check heavy:probe
    contains "FREE" "$output"
}

@test "a live claim's process-tree RSS prints without a usage error" {
    "$CLAIM" take heavy:probe probing --pid "$OWNER" >/dev/null
    run "$CLAIM" check heavy:probe
    contains "rss=" "$output"
    [[ "$output" != *usage:* ]]
}

@test "resources prints memory, load and cpu figures on this box" {
    unset HELIX_CLAIM_PROC
    run "$CLAIM" resources --no-test-host
    [ "$status" -eq 0 ]
    [[ "${lines[0]}" =~ [0-9.]+GB\ available,\ load\ [0-9.]+\ [0-9.]+\ [0-9.]+\ on\ [0-9]+\ cpus ]]
}
