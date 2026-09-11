#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the claim that .githooks takes while a merge holds the tree.
#
# git runs pre-commit only for a merge that STOPPED to be resolved. A merge that
# applies cleanly runs pre-merge-commit and post-merge instead, and never runs
# pre-commit or post-commit. Both entry points therefore have to take the same
# claim, or a clean merge holds the shared tree announcing nothing while its
# hook rebuilds.
#
# Every test runs against a throwaway repo under BATS_TEST_TMPDIR with
# HELIX_CLAIM_DIR pointed at it, so nothing here touches this checkout's claims.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"

# A miniature repo carrying this checkout's hooks, its helix-claim, and two
# branches that merge cleanly.
setup() {
    FIX="$BATS_TEST_TMPDIR/fix"
    mkdir -p "$FIX"
    cd "$FIX" || return 1
    git init -q .
    git config user.email t@example.com
    git config user.name tester
    mkdir -p scripts .githooks/lib
    cp "$REPO_ROOT/scripts/helix-claim" scripts/.real-claim
    # A shim so a test can assert the hook actually invoked helix-claim, rather
    # than only that no claim survived - which is also true if it never ran.
    CALLS="$BATS_TEST_TMPDIR/claim-calls.log"
    export CALLS
    printf '#!/bin/sh\necho "$*" >> "%s"\nexec "%s/scripts/.real-claim" "$@"\n' \
        "$CALLS" "$FIX" > scripts/helix-claim
    cp "$REPO_ROOT/.githooks/pre-commit" "$REPO_ROOT/.githooks/pre-merge-commit" \
       "$REPO_ROOT/.githooks/post-merge" "$REPO_ROOT/.githooks/post-commit" .githooks/
    cp "$REPO_ROOT/.githooks/lib/claim-tree.sh" .githooks/lib/
    chmod +x scripts/helix-claim scripts/.real-claim .githooks/pre-merge-commit .githooks/post-merge \
             .githooks/post-commit
    git config core.hooksPath "$FIX/.githooks"
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"

    echo base > f.txt
    git add f.txt
    git commit -qm base --no-verify
    git checkout -qb side
    echo side > side.txt
    git add side.txt
    git commit -qm side --no-verify
    git checkout -q -
    echo main > main.txt
    git add main.txt
    git commit -qm mainline --no-verify
}

# The setup commits run post-commit, so a count is only meaningful against a log
# that starts empty at the operation under test.
reset_calls() {
    : > "$CALLS"
}

# A live process that is not the git process, to stand in for a peer session.
start_peer() {
    sleep 120 &
    PEER_PID=$!
    scripts/.real-claim take worktree:fix "peer session" --pid "$PEER_PID" >/dev/null
}

teardown() {
    [ -n "${PEER_PID:-}" ] && kill "$PEER_PID" 2>/dev/null
    return 0
}

@test "a clean merge claims the tree and releases it again" {
    reset_calls
    run git -c merge.autoStash=false merge --no-ff side -m "merge side"
    [ "$status" -eq 0 ]
    # Assert the hook ran, not merely that nothing survived it.
    run grep -c '^take worktree:fix' "$CALLS"
    [ "$output" -eq 1 ]
    run grep -c '^release-if-owned-by' "$CALLS"
    [ "$output" -eq 1 ]
    run scripts/.real-claim list
    contains "no claims" "$output"
}

@test "a clean merge does not steal a claim another session holds" {
    start_peer
    reset_calls
    run git -c merge.autoStash=false merge --no-ff side -m "merge side"
    [ "$status" -eq 0 ]
    contains "Another session is working in this tree" "$output"
    run scripts/.real-claim list
    contains "peer session" "$output"
    # It looked, and declined to take.
    run grep -c '^check worktree:fix' "$CALLS"
    [ "$output" -ge 1 ]
    run grep -c '^take worktree:fix' "$CALLS"
    [ "$output" -eq 0 ]
}

@test "post-merge leaves a claim owned by a different pid alone" {
    start_peer
    reset_calls
    git -c merge.autoStash=false merge --no-ff side -m "merge side" >/dev/null 2>&1
    # post-merge has to have run and declined; a hook that never ran would also
    # leave the claim standing.
    run grep -c '^release-if-owned-by' "$CALLS"
    [ "$output" -eq 1 ]
    run scripts/.real-claim check worktree:fix
    contains "peer session" "$output"
}

@test "HELIX_CLAIM_STRICT refuses the merge instead of warning" {
    start_peer
    before="$(git rev-parse HEAD)"
    HELIX_CLAIM_STRICT=1 run git -c merge.autoStash=false merge --no-ff side -m "merge side"
    contains "Refusing" "$output"
    [ "$before" = "$(git rev-parse HEAD)" ]
    git merge --abort 2>/dev/null || true
}

@test "a claim this session already holds is not reported as a foreign holder" {
    # The careful workflow is to claim the tree, then merge. The hook claims
    # under the git process, so without ancestry awareness it reports the caller
    # to itself - and the warning would then fire for exactly the people
    # following the protocol. $$ is the test shell, an ancestor of the merge.
    scripts/helix-claim take worktree:fix "this session" --pid $$ >/dev/null
    reset_calls
    run git -c merge.autoStash=false merge --no-ff side -m "merge side"
    [ "$status" -eq 0 ]
    lacks "Another session is working in this tree" "$output"
    # It is left in place, not taken over and not dropped.
    run scripts/.real-claim check worktree:fix
    contains "this session" "$output"
}

@test "a conflicted merge still claims through pre-commit" {
    # Both branches touch one file, so the merge stops and the completing
    # `git commit` is what runs pre-commit.
    git checkout -q -b clash HEAD~1
    echo theirs > f.txt
    git add f.txt
    git commit -qm theirs --no-verify
    git checkout -q -
    echo ours > f.txt
    git add f.txt
    git commit -qm ours --no-verify
    run git -c merge.autoStash=false merge --no-ff clash -m "merge clash"
    [ "$status" -ne 0 ]
    [ -f .git/MERGE_HEAD ]
    # pre-commit sources the same helper, so the claim block is reachable here.
    run grep -c claim_tree .githooks/pre-commit
    [ "$output" -ge 1 ]
}

@test "the helper is the only copy of the claim logic" {
    run grep -c 'helix-claim take' .githooks/pre-commit .githooks/pre-merge-commit
    contains "pre-commit:0" "$output"
    contains "pre-merge-commit:0" "$output"
    run grep -c 'helix-claim take' .githooks/lib/claim-tree.sh
    [ "$output" -eq 1 ]
}
