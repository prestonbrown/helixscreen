#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# `make t` runs the test binary under a timeout, so a hung binary fails the
# target instead of holding it forever. The binary is a fake; test-build is
# marked up to date (-o) so make runs only the t recipe.

bats_require_minimum_version 1.5.0
load helpers

REPO="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"

setup() {
    FAKE_BIN="$BATS_TEST_TMPDIR/fake-tests"
    STUB_PID="$BATS_TEST_TMPDIR/stub.pid"
}

# make_t [make overrides...]: bounded, so a target that never kills its binary
# fails the test instead of hanging the suite. A stub that survives make is
# killed by the pid it recorded: it would otherwise hold the output pipe open.
make_t() {
    local rc=0
    (cd "$(scratch_tree)" && timeout 25 make --no-print-directory -o test-build TEST_BIN="$FAKE_BIN" \
        F='[x]' "$@" t) || rc=$?
    if [ -f "$STUB_PID" ]; then kill -KILL "$(cat "$STUB_PID")" 2>/dev/null || true; fi
    return "$rc"
}

@test "a passing binary passes through untouched" {
    printf '#!/bin/sh\necho "All tests passed ($1)"\n' > "$FAKE_BIN"
    chmod +x "$FAKE_BIN"
    run make_t TEST_TIMEOUT=5
    [ "$status" -eq 0 ]
    contains "All tests passed ([x])" "$output"
    lacks "was killed" "$output"
}

@test "a failing binary keeps its own failure, with no timeout message" {
    printf '#!/bin/sh\necho "test cases: 1 | 1 failed"\nexit 3\n' > "$FAKE_BIN"
    chmod +x "$FAKE_BIN"
    run make_t TEST_TIMEOUT=5
    [ "$status" -ne 0 ]
    contains "Error 3" "$output"
    lacks "was killed" "$output"
}

@test "a hung binary is killed after TEST_TIMEOUT and the target fails loudly" {
    printf '#!/bin/sh\necho $$ > "%s"\nexec sleep 60\n' "$STUB_PID" > "$FAKE_BIN"
    chmod +x "$FAKE_BIN"
    SECONDS=0
    run make_t TEST_TIMEOUT=1
    [ "$status" -ne 0 ]
    [ "$SECONDS" -lt 30 ]
    contains "was killed after 1s" "$output"
}

@test "a binary that ignores SIGTERM is killed by the SIGKILL backstop" {
    # A crash handler deadlocked on the malloc lock never acts on SIGTERM.
    printf '#!/bin/sh\necho $$ > "%s"\ntrap "" TERM\nwhile :; do sleep 1; done\n' "$STUB_PID" \
        > "$FAKE_BIN"
    chmod +x "$FAKE_BIN"
    SECONDS=0
    run make_t TEST_TIMEOUT=1 TIMEOUT_KILL_AFTER=1
    [ "$status" -ne 0 ]
    [ "$SECONDS" -lt 30 ]
    contains "was killed after 1s" "$output"
    contains "Error 137" "$output"
}
