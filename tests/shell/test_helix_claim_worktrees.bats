#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The claim store belongs to the repository, not to whichever worktree is asking.
#
# A session asking "is main free?" is almost never standing in main, so a store
# anchored per-worktree would answer FREE for a tree another session is holding,
# and would let a second `take` succeed on top of the first. An advisory lock is
# allowed to be ignored; it is not allowed to fail open.

load helpers

CLAIM="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/helix-claim"

setup() {
    unset HELIX_CLAIM_DIR
    MAIN="$BATS_TEST_TMPDIR/mainrepo"
    LINKED="$MAIN/.worktrees/feat"
    mkdir -p "$MAIN"
    cd "$MAIN" || return 1
    git init -q .
    git config user.email t@example.com
    git config user.name tester
    echo a > f.txt
    git add f.txt
    git commit -qm base --no-verify
    git worktree add -q -b feat "$LINKED"
    # An owner that outlives the test body, so the claim reads LIVE throughout.
    sleep 120 &
    OWNER=$!
}

teardown() {
    [ -n "${OWNER:-}" ] && kill "$OWNER" 2>/dev/null
    return 0
}

@test "a claim taken in the main tree is LIVE from a linked worktree" {
    "$CLAIM" take worktree:mainrepo "held by main" --pid "$OWNER" >/dev/null
    cd "$LINKED"
    run "$CLAIM" check worktree:mainrepo
    [ "$status" -eq 1 ]
    contains "held by main" "$output"
}

@test "take from a linked worktree refuses a claim the main tree holds" {
    "$CLAIM" take worktree:mainrepo "held by main" --pid "$OWNER" >/dev/null
    cd "$LINKED"
    run "$CLAIM" take worktree:mainrepo "second session"
    [ "$status" -ne 0 ]
    contains "REFUSED" "$output"
}

@test "a claim taken in a linked worktree is LIVE from the main tree" {
    cd "$LINKED"
    "$CLAIM" take worktree:feat "held by the worktree" --pid "$OWNER" >/dev/null
    cd "$MAIN"
    run "$CLAIM" check worktree:feat
    [ "$status" -eq 1 ]
    contains "held by the worktree" "$output"
}

@test "one store serves the whole repository" {
    "$CLAIM" take worktree:mainrepo "held" --pid "$OWNER" >/dev/null
    cd "$LINKED"
    "$CLAIM" take build:feat "a build" --pid "$OWNER" >/dev/null
    run bash -c "find '$MAIN' -name .helix-claims -type d | wc -l"
    [ "$output" -eq 1 ]
}

@test "list from a linked worktree sees the main tree's claims" {
    "$CLAIM" take worktree:mainrepo "held by main" --pid "$OWNER" >/dev/null
    cd "$LINKED"
    run "$CLAIM" list
    contains "held by main" "$output"
}

@test "HELIX_CLAIM_DIR still overrides the repository store" {
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/elsewhere"
    "$CLAIM" take worktree:mainrepo "held" --pid "$OWNER" >/dev/null
    [ -d "$HELIX_CLAIM_DIR" ]
    run bash -c "find '$MAIN' -name .helix-claims -type d | wc -l"
    [ "$output" -eq 0 ]
}
