#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/setup-worktree.sh — lib/ sharing policy.
#
# The script symlinks each lib/ submodule into the main tree so a worktree
# builds in seconds instead of recompiling ~GB of submodules. That is right for
# the third-party ones, which we never edit (changes go through patches/), and
# wrong for lib/helix-xml, which is ours and is edited directly: a symlink makes
# every worktree edit land in the MAIN tree's submodule working copy, shared
# with every other worktree and visible as dirt in main's `git status`.

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    SCRIPT="scripts/setup-worktree.sh"
}

@test "helix-xml gets a private per-worktree checkout, not a symlink" {
    run grep -E '^LIB_PRIVATE_SUBMODULES=' "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *'lib/helix-xml'* ]]
}

@test "the symlink loop skips private submodules" {
    # Without this guard the loop would symlink helix-xml first and the private
    # checkout would then be replacing a link it had just created — or worse,
    # order-dependently, not replacing it at all.
    run bash -c "sed -n '/^link_lib_from_main/,/^}/p' '$SCRIPT' | grep -c 'is_private_submodule'"
    [ "$output" -ge 1 ]
}

@test "--unlink does not touch private submodules" {
    # lib_submodule_paths feeds --unlink, which replaces each entry with an
    # EMPTY DIRECTORY. Applied to a real checkout that would discard any
    # uncommitted engine work in it.
    run bash -c "sed -n '/^lib_submodule_paths/,/^}/p' '$SCRIPT' | grep -c 'is_private_submodule'"
    [ "$output" -ge 1 ]
}

@test "every private submodule is a real submodule in .gitmodules" {
    # A typo here fails open: is_private_submodule never matches, the entry is
    # symlinked as before, and nothing complains.
    names=$(sed -n 's/^LIB_PRIVATE_SUBMODULES=(\(.*\))/\1/p' "$SCRIPT" | tr -d '"')
    [ -n "$names" ]
    for n in $names; do
        run git config --file .gitmodules --get-regexp path
        [[ "$output" == *"$n"* ]] || { echo "not a submodule: $n" >&2; return 1; }
    done
}

@test "no patch targets helix-xml" {
    # CLAUDE.md: helix-xml is edited and committed directly, never patched. A
    # patch for it would be destroyed by the `git restore` that the patch
    # workflow runs against the submodule after generating the diff. (Prose
    # mentions of helix-xml in patches.mk are fine — this looks for a rule.)
    run bash -c "grep -nE '^[A-Z_]*helix[_-]?xml[A-Z_]*(_PATCHED_FILES)? *[:+]?=' mk/patches.mk -i"
    [ "$status" -eq 1 ]
    run bash -c "ls patches/ | grep -i 'helix.*xml'"
    [ "$status" -ne 0 ]
}
