#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/cloud/session-start.sh: the cloud-env SessionStart hook must stay a
# silent no-op on any machine that never ran the cloud setup script — a
# laptop, thelio — since it runs at the top of every session there too. Where
# it does act, it must be idempotent: the same hook runs on every session in
# a cloud environment, not once.
#
# HELIX_CLOUD_ENV_DIR / HELIX_CLOUD_SEED point the hook at scratch paths so
# these tests never touch the real /opt locations. `make` is mocked because
# the fixture repos below carry no Makefile of their own; `git submodule
# update` runs for real against a fixture repo with zero submodules, which is
# a fast no-op rather than something worth mocking.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/scripts/cloud/session-start.sh"

setup() {
    load helpers
    mock_command make ""

    export HELIX_CLOUD_ENV_DIR="$BATS_TEST_TMPDIR/cloud-env"
    export HELIX_CLOUD_SEED="$BATS_TEST_TMPDIR/seed"

    REPO="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$REPO"
    git -C "$REPO" init -q
    git -C "$REPO" config user.email test@test.com
    git -C "$REPO" config user.name test
    echo hello > "$REPO/f.txt"
    git -C "$REPO" add f.txt
    git -C "$REPO" commit -qm init
}

# Marks the environment as cloud-warmed, without which the hook is a no-op.
mark_ready() {
    mkdir -p "$HELIX_CLOUD_ENV_DIR"
    echo "date: test" > "$HELIX_CLOUD_ENV_DIR/READY"
}

# A real, tiny, non-bare repo at HELIX_CLOUD_SEED — its .git/objects is what
# the hook's alternates check looks for.
make_seed_repo() {
    mkdir -p "$HELIX_CLOUD_SEED"
    git -C "$HELIX_CLOUD_SEED" init -q
    git -C "$HELIX_CLOUD_SEED" config user.email test@test.com
    git -C "$HELIX_CLOUD_SEED" config user.name test
    echo world > "$HELIX_CLOUD_SEED/x.txt"
    git -C "$HELIX_CLOUD_SEED" add x.txt
    git -C "$HELIX_CLOUD_SEED" commit -qm seed
}

run_hook() {
    (cd "$REPO" && "$HOOK")
}

@test "without the READY marker, the hook exits 0 and prints nothing" {
    run run_hook
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "READY + a real seed clone: alternates gets the seed objects path, and both git config keys are set" {
    mark_ready
    make_seed_repo

    run run_hook
    [ "$status" -eq 0 ]

    local alt="$REPO/.git/objects/info/alternates"
    [ -f "$alt" ] || fail "alternates file was not created"
    contains "$HELIX_CLOUD_SEED/.git/objects" "$(cat "$alt")"

    run git -C "$REPO" config --get submodule.alternateLocation
    [ "$status" -eq 0 ]
    contains "superproject" "$output"

    run git -C "$REPO" config --get submodule.alternateErrorStrategy
    [ "$status" -eq 0 ]
    contains "info" "$output"
}

@test "running the hook twice does not duplicate the alternates line" {
    mark_ready
    make_seed_repo

    run run_hook
    [ "$status" -eq 0 ]
    run run_hook
    [ "$status" -eq 0 ]

    local alt="$REPO/.git/objects/info/alternates"
    local count
    count=$(grep -Fxc "$HELIX_CLOUD_SEED/.git/objects" "$alt")
    [ "$count" -eq 1 ] || fail "expected exactly one alternates line, found $count"
}

@test "READY present but no seed: no alternates file is written, hook still exits 0" {
    mark_ready
    # HELIX_CLOUD_SEED is exported by setup() but nothing was ever created there.

    run run_hook
    [ "$status" -eq 0 ]

    local alt="$REPO/.git/objects/info/alternates"
    [ -f "$alt" ] && fail "alternates file should not exist without a seed"
    refute_grep "alternate" "$REPO/.git/config"
}
