#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/teardown-worktree.sh resolves its target by combining a filesystem
# path guess with a check against `git worktree list --porcelain`. When a
# worktree's OWN .git pointer is already gone - a previous teardown that got
# partway through rm -rf before root-owned leftovers (a docker build, say)
# stopped it - every `git -C "$WT_ABS"` call in the script discovers upward
# past that missing pointer and silently answers for whatever repository it
# finds next, which for a worktree living under the main tree is the main
# tree itself. The script then reports the main tree's branch and uncommitted
# files as the worktree's own, and --force on it attempts to delete the main
# tree's own checked-out branch - saved only by git's own refusal to delete
# a branch checked out somewhere.
#
# These tests pin that a worktree missing its .git pointer is refused with a
# clear message naming only its own leftover files, never the main repo's,
# and that a normal teardown (no corruption) still completes.

SCRIPT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/teardown-worktree.sh"

setup() {
    load helpers
    MAIN="$BATS_TEST_TMPDIR/mainrepo"
    mkdir -p "$MAIN"
    cd "$MAIN" || return 1
    git init -q -b master .
    git config user.email t@example.com
    git config user.name tester
    echo hello > README.md
    git add README.md
    git commit -qm base --no-verify
}

make_worktree() {
    local name="$1"
    mkdir -p "$MAIN/.worktrees"
    git worktree add -q -b "feature/$name" "$MAIN/.worktrees/$name"
}

@test "a normal worktree tears down cleanly" {
    make_worktree normal
    run "$SCRIPT" normal --into master
    [ "$status" -eq 0 ]
    contains "Teardown complete" "$output"
    [ ! -d "$MAIN/.worktrees/normal" ]
    run git -C "$MAIN" worktree list --porcelain
    lacks ".worktrees/normal" "$output"
}

# A branch made by setup-worktree.sh tracks origin/<default>, and `git branch -d`
# checks containment in that ref. It moves only on a fetch or on a push made by
# remote NAME, so these two cases pin the difference between a ref that is merely
# stale and work that is genuinely not on the remote.
make_remote_worktree() {
    local name="$1"
    REMOTE_PATH="$BATS_TEST_TMPDIR/remote.git"
    git init -q --bare -b master "$REMOTE_PATH"
    git -C "$MAIN" remote add origin "$REMOTE_PATH"
    git -C "$MAIN" push -q origin master
    mkdir -p "$MAIN/.worktrees"
    git -C "$MAIN" worktree add -q -b "feature/$name" "$MAIN/.worktrees/$name" origin/master
    echo work > "$MAIN/.worktrees/$name/work.txt"
    git -C "$MAIN/.worktrees/$name" add work.txt
    git -C "$MAIN/.worktrees/$name" commit -qm work --no-verify
    git -C "$MAIN" merge -q --no-ff -m merge "feature/$name"
}

@test "a stale tracking ref does not report pushed work as unpushed" {
    make_remote_worktree pushed
    # By URL, the way a push goes when the remote's SSH path is being avoided or
    # the push came from another machine: the remote has it, origin/master does not.
    git -C "$MAIN" push -q "$REMOTE_PATH" master:master
    run git -C "$MAIN" merge-base --is-ancestor feature/pushed origin/master
    [ "$status" -ne 0 ]

    run "$SCRIPT" pushed --into master
    [ "$status" -eq 0 ]
    contains "was stale" "$output"
    run git -C "$MAIN" branch --list feature/pushed
    [ -z "$output" ]
}

@test "work the remote does not have keeps its branch" {
    make_remote_worktree unpushed

    run "$SCRIPT" unpushed --into master
    [ "$status" -eq 0 ]
    # The refusal has to be the reason, not a silent skip that happens to leave
    # the branch behind.
    contains "git refused -d" "$output"
    run git -C "$MAIN" branch --list feature/unpushed
    contains "feature/unpushed" "$output"
}

@test "a worktree missing its .git pointer is refused, not misattributed to the main tree" {
    make_worktree broken
    echo leftover > "$MAIN/.worktrees/broken/leftover.txt"
    rm -f "$MAIN/.worktrees/broken/.git"

    # Untracked and never committed, so it exists ONLY in the main tree's own
    # working directory - never checked out into any worktree. A script that
    # escapes to the main tree via `git -C <worktree> status` would report it;
    # one that does not can never mention it.
    echo only-in-main-tree > "$MAIN/main-tree-uncommitted.txt"

    run "$SCRIPT" broken --into master
    [ "$status" -ne 0 ]

    # The whole point: never names the main tree's own state.
    lacks "main-tree-uncommitted.txt" "$output"
    lacks "branch:   master" "$output"

    # It does explain what is actually left in the worktree.
    contains "leftover.txt" "$output"
    contains "no .git of its own" "$output"

    # Refusing must not have touched anything - directory and registration
    # are both still there for the operator (or --force) to act on.
    [ -d "$MAIN/.worktrees/broken" ]
    run git -C "$MAIN" worktree list --porcelain
    contains ".worktrees/broken" "$output"

    # And the main tree's own untracked marker is still just sitting there -
    # nothing tried to report on it, let alone touch it.
    [ -f "$MAIN/main-tree-uncommitted.txt" ]
}

@test "--force finishes a worktree missing its .git pointer without touching the main tree's branch" {
    make_worktree broken2
    echo leftover > "$MAIN/.worktrees/broken2/leftover.txt"
    rm -f "$MAIN/.worktrees/broken2/.git"

    run "$SCRIPT" broken2 --into master --force
    [ "$status" -eq 0 ]

    # Recovery mode skips branch cleanup outright rather than guess at
    # containment from a repo it can no longer safely query - it must never
    # even ATTEMPT a branch deletion.
    lacks "Deleting branch" "$output"

    [ ! -d "$MAIN/.worktrees/broken2" ]
    run git -C "$MAIN" worktree list --porcelain
    lacks ".worktrees/broken2" "$output"

    # The main tree's own branch and tracked file are untouched.
    run git -C "$MAIN" branch --show-current
    contains "master" "$output"
    [ -f "$MAIN/README.md" ]

    run git -C "$MAIN" branch --list "feature/broken2"
    contains "feature/broken2" "$output"
}

@test "an unregistered directory is refused, never treated as a worktree" {
    mkdir -p "$BATS_TEST_TMPDIR/not-a-worktree"
    run "$SCRIPT" "$BATS_TEST_TMPDIR/not-a-worktree" --into master
    [ "$status" -ne 0 ]
    contains "does not list that path as a worktree" "$output"
}

@test "the main tree itself is refused even when named directly" {
    run "$SCRIPT" "$MAIN" --into master
    [ "$status" -ne 0 ]
    contains "that is the main tree" "$output"
}

# ---------------------------------------------------------- submodule pointers
#
# .git/modules/<name> is common to every worktree, so initializing a submodule
# inside one aims that shared core.worktree at it. Teardown has to aim it back
# at the main checkout before deleting the directory, or every other worktree
# symlinking that submodule fails `git status` with "cannot chdir". The two
# module depths carry different ../ runs and are restored independently, and a
# pointer aimed anywhere else is not the script's to touch.

fake_module_pointer() {
    mkdir -p "$MAIN/.git/modules/$1"
    printf '[core]\n\tworktree = %s\n' "$2" > "$MAIN/.git/modules/$1/config"
}

module_pointer() {
    git config --file "$MAIN/.git/modules/$1/config" --get core.worktree
}

@test "a submodule pointer aimed into the removed worktree is restored" {
    make_worktree doomed
    mkdir -p "$MAIN/lib/glm"
    fake_module_pointer glm "../../../.worktrees/doomed/lib/glm"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(module_pointer glm)" = "../../../lib/glm" ]
}

@test "a restored pointer keeps the deeper lib/ module ../ run" {
    make_worktree doomed
    mkdir -p "$MAIN/lib/ftxui"
    fake_module_pointer lib/ftxui "../../../../.worktrees/doomed/lib/ftxui"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(module_pointer lib/ftxui)" = "../../../../lib/ftxui" ]
}

# The worktree's lib/ftxui is a symlink into the main tree, as setup leaves it,
# and a dry run still sees the pointer through it.
@test "a dry run names the pointer it would restore and writes nothing" {
    make_worktree doomed
    mkdir -p "$MAIN/lib/ftxui" "$MAIN/.worktrees/doomed/lib"
    ln -s "$MAIN/lib/ftxui" "$MAIN/.worktrees/doomed/lib/ftxui"
    fake_module_pointer lib/ftxui "../../../../.worktrees/doomed/lib/ftxui"
    # --force only gets past the untracked symlink; -n still changes nothing.
    run "$SCRIPT" doomed --into master --force -n
    [ "$status" -eq 0 ]
    contains "ftxui: points into this worktree, would restore to ../../../../lib/ftxui" "$output"
    [ "$(module_pointer lib/ftxui)" = "../../../../.worktrees/doomed/lib/ftxui" ]
    [ -d "$MAIN/.worktrees/doomed" ]
}

@test "a pointer into a sibling whose name extends the worktree's is left alone" {
    make_worktree doomed
    mkdir -p "$MAIN/.worktrees/doomed-2/lib"
    fake_module_pointer lib/x "../../../../.worktrees/doomed-2/lib/x"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(module_pointer lib/x)" = "../../../../.worktrees/doomed-2/lib/x" ]
}

@test "a pointer aimed outside the removed worktree is left alone" {
    make_worktree doomed
    mkdir -p "$MAIN/lib/spdlog"
    fake_module_pointer spdlog "../../../lib/spdlog"
    run "$SCRIPT" doomed --into master
    [ "$status" -eq 0 ]
    [ "$(module_pointer spdlog)" = "../../../lib/spdlog" ]
}

# A tree whose owner is reading code rather than building runs no compiler, holds
# no commits and reports a clean status, so every other guard passes it. The
# advisory claim is the only signal that phase leaves behind.
make_claimed_worktree() {
    local name="$1"
    mkdir -p "$MAIN/scripts"
    cp "$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/helix-claim" "$MAIN/scripts/helix-claim"
    chmod +x "$MAIN/scripts/helix-claim"
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    make_worktree "$name"
    "$MAIN/scripts/helix-claim" take "worktree:$name" "reading code" >/dev/null 2>&1
}

@test "teardown refuses a worktree whose claim has a live owner" {
    make_claimed_worktree claimed
    run "$SCRIPT" claimed --into master
    [ "$status" -eq 1 ]
    contains "is claimed and its owner is alive" "$output"
    [ -d "$MAIN/.worktrees/claimed" ]
}

@test "--force removes a worktree despite a live claim" {
    make_claimed_worktree forced
    run "$SCRIPT" forced --into master --force
    [ "$status" -eq 0 ]
    contains "removing anyway" "$output"
    [ ! -d "$MAIN/.worktrees/forced" ]
}

@test "a released claim lets teardown proceed" {
    make_claimed_worktree released
    "$MAIN/scripts/helix-claim" release "worktree:released" >/dev/null 2>&1
    run "$SCRIPT" released --into master
    [ "$status" -eq 0 ]
    contains "Teardown complete" "$output"
    [ ! -d "$MAIN/.worktrees/released" ]
}

# A branch carrying commits of its own is what --force-branch exists for: git
# refuses -d on it, so the script must either keep it or discard it on request,
# and must never describe the discarding case as lossless.
make_diverged_worktree() {
    local name="$1"
    make_worktree "$name"
    echo diverged > "$MAIN/.worktrees/$name/own.txt"
    git -C "$MAIN/.worktrees/$name" add own.txt
    git -C "$MAIN/.worktrees/$name" commit -qm "work only on this branch" --no-verify
}

@test "a branch --into does not contain survives teardown by default" {
    make_diverged_worktree diverged
    run "$SCRIPT" diverged --into master
    [ "$status" -eq 0 ]
    contains "NOT contained" "$output"
    contains "--force-branch" "$output"
    run git -C "$MAIN" branch --list feature/diverged
    contains "feature/diverged" "$output"
}

@test "--force-branch deletes a branch --into does not contain" {
    make_diverged_worktree diverged
    run "$SCRIPT" diverged --into master --force-branch
    [ "$status" -eq 0 ]
    contains "discards" "$output"
    [ ! -d "$MAIN/.worktrees/diverged" ]
    run git -C "$MAIN" branch --list feature/diverged
    lacks "feature/diverged" "$output"
}
