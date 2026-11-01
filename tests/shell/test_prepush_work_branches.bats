#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# pre-push sweeps a work-branch push with the commit-level gates only, and a push
# that reaches main, release/* or a tag with everything. The hook runs against a
# fixture repo whose gates are stand-ins that print their name and the mode.

load helpers

HOOK="$BATS_TEST_DIRNAME/../../.githooks/pre-push"

setup() {
    export HELIX_LAND_GATE_STAMP_DIR="$BATS_TEST_TMPDIR/stamps"
    export HELIX_LAND_GATE_SHARED_STAMP_DIR="$BATS_TEST_TMPDIR/shared-stamps"
    unset HELIX_PREPUSH_FULL HELIX_PREPUSH_IN_PLACE HELIX_QC_SKIP_PUSH_ONLY
    ROOT="$BATS_TEST_TMPDIR/repo"
    git init -q -b main "$ROOT"
    git -C "$ROOT" config user.email t@t
    git -C "$ROOT" config user.name t
    mkdir -p "$ROOT/scripts" "$ROOT/lib"
    printf '#!/bin/sh\necho "ran: quality-checks skip_push_only=${HELIX_QC_SKIP_PUSH_ONLY:-0}"\n' \
        >"$ROOT/scripts/quality-checks.sh"
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

push() { # <remote ref>...: one stdin line per ref, each pushing TIP over BASE
    local lines="" ref
    for ref in "$@"; do lines="${lines}refs/heads/x $TIP $ref $BASE"$'\n'; done
    run env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY \
        bash -c 'cd "$1" && printf "%s" "$2" | bash "$3"' _ "$ROOT" "$lines" "$HOOK"
}

@test "a push to main runs every gate, the clang divergence gate included" {
    echo dirty >"$ROOT/f" # the isolated path is where the clang gate runs separately
    push refs/heads/main
    [ "$status" -eq 0 ]
    contains "ran: quality-checks skip_push_only=0" "$output"
    contains "ran: check_clang_diagnostics" "$output"
}

@test "a push to a release branch or a tag runs every gate" {
    push refs/heads/release/1.1
    [ "$status" -eq 0 ]
    contains "skip_push_only=0" "$output"
    push refs/tags/v1.2.0
    [ "$status" -eq 0 ]
    contains "skip_push_only=0" "$output"
}

@test "a work-branch push runs the commit-level gates and leaves the rest to the land gate" {
    echo dirty >"$ROOT/f"
    push refs/heads/fix/thing
    [ "$status" -eq 0 ]
    contains "ran: quality-checks skip_push_only=1" "$output"
    contains "work-branch sweep" "$output"
    lacks "ran: check_clang_diagnostics" "$output"
    contains "ran: check_patch_drift" "$output"
}

@test "a push that updates a work branch and main together runs every gate" {
    push refs/heads/fix/thing refs/heads/main
    contains "skip_push_only=0" "$output"
    push refs/heads/main refs/heads/fix/thing
    contains "skip_push_only=0" "$output"
}

@test "HELIX_PREPUSH_FULL=1 sweeps a work branch fully" {
    HELIX_PREPUSH_FULL=1 push refs/heads/fix/thing
    [ "$status" -eq 0 ]
    contains "skip_push_only=0" "$output"
}
