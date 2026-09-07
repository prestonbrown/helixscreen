#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/setup-worktree.sh — lib/ sharing policy.
#
# The script symlinks each lib/ submodule into the main tree so a worktree
# builds in seconds instead of recompiling ~GB of submodules. That is right for
# the submodules nothing rewrites, and wrong for the three that are rewritten
# per branch: lib/helix-xml is ours and is edited directly, and lib/lvgl and
# lib/libhv are rewritten by patches/, which is per-branch. Sharing one checkout
# between branches that disagree about either is unsatisfiable — each tree's
# correct action invalidates the other's — so those three get a private checkout
# per worktree.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    SCRIPT="scripts/setup-worktree.sh"
}

@test "helix-xml gets a private per-worktree checkout, not a symlink" {
    run grep -E '^LIB_PRIVATE_SUBMODULES=' "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *'lib/helix-xml'* ]]
}

@test "every submodule patches/ rewrites gets a private checkout" {
    # A patched submodule left symlinked is the whole defect: patches/ is
    # per-branch and the checkout would not be, so one tree's reapply-patches
    # silently redefines what every other tree compiles.
    patched=$(grep -oE '^(LVGL|LIBHV)_PATCHED_FILES' mk/patches.mk | sort -u)
    [ -n "$patched" ] || return 1
    private=$(grep -E '^LIB_PRIVATE_SUBMODULES=' "$SCRIPT")
    for p in $patched; do
        case "$p" in
            LVGL_PATCHED_FILES) path="lib/lvgl" ;;
            LIBHV_PATCHED_FILES) path="lib/libhv" ;;
        esac
        [[ "$private" == *"$path"* ]] || { echo "patched but shared: $path" >&2; return 1; }
    done
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

# --- main-tree resolution (prestonbrown/helixscreen#1350 fallout) -------------
#
# Every worktree carries its own scripts/, so running a worktree's copy made
# SCRIPT_DIR/.. resolve to that worktree and MAIN_TREE point at it. The lib/
# loop then rm -rf'd each real submodule and symlinked it to the path it had
# just deleted. Two agent worktrees lost their lib/ that way.

@test "MAIN_TREE is re-resolved from git-common-dir, not just SCRIPT_DIR/.." {
    run grep -c 'git-common-dir' "$SCRIPT"
    [ "$status" -eq 0 ]
    [ "$output" -ge 1 ]
}

@test "a relative git-common-dir is resolved against MAIN_TREE before use" {
    # `git rev-parse --git-common-dir` answers a bare ".git" from a main-tree
    # root. Using that unresolved would cd to the CWD's parent, not the repo's.
    run bash -c "grep -c 'GIT_COMMON_DIR\" != /\*' '$SCRIPT'"
    [ "$output" -ge 1 ]
}

# --- the private checkout, end to end -----------------------------------------
#
# Everything above reads the script's text. These build a miniature repo with one
# submodule named lib/lvgl — a name LIB_PRIVATE_SUBMODULES covers — run the real
# script over it, and assert on what lands on disk.

# Builds $1/upstream (two commits) and $1/main (a repo with it at lib/lvgl),
# with just enough of the tree for the script to run. Echoes nothing; the caller
# uses $1/main.
build_fixture_repo() {
    local root="$1"
    git init -q "$root/upstream"
    git -C "$root/upstream" config user.email "t@example.invalid"
    git -C "$root/upstream" config user.name "t"
    mkdir -p "$root/upstream/src"
    echo "int v = 1;" > "$root/upstream/src/lv_thing.c"
    git -C "$root/upstream" add src/lv_thing.c
    git -C "$root/upstream" commit -qm first
    echo "int v = 2;" > "$root/upstream/src/lv_thing.c"
    git -C "$root/upstream" add src/lv_thing.c
    git -C "$root/upstream" commit -qm second

    git init -q "$root/main"
    git -C "$root/main" config user.email "t@example.invalid"
    git -C "$root/main" config user.name "t"
    mkdir -p "$root/main/scripts" "$root/main/patches" "$root/main/mk"
    cp scripts/setup-worktree.sh "$root/main/scripts/"
    cp scripts/sync-worktree-mtimes.py "$root/main/scripts/"
    : > "$root/main/patches/.keep"
    # git refuses a file:// submodule unless the transport is allowed on the
    # command line; the repo-config form is not consulted for the inner clone.
    git -C "$root/main" -c protocol.file.allow=always submodule add -q "$root/upstream" lib/lvgl
    git -C "$root/main" add scripts patches .gitmodules lib/lvgl
    git -C "$root/main" commit -qm init
}

@test "a private submodule is a real checkout with its own git dir" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"   # never touch the real one
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    wt="$tmp/main/.worktrees/iso"
    [ ! -L "$wt/lib/lvgl" ] || { echo "still a symlink" >&2; return 1; }
    [ -d "$wt/lib/lvgl" ] || return 1
    # The git dir must be this worktree's own. Inheriting the main tree's is the
    # sharing the private checkout exists to remove.
    run cat "$wt/lib/lvgl/.git"
    [[ "$output" == *"worktrees/iso/modules/"*"lvgl" ]] || { echo "$output" >&2; return 1; }
    rm -rf "$tmp"
}

@test "patching a private submodule leaves the main tree's copy alone" {
    # The property the whole change exists to create.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    wt="$tmp/main/.worktrees/iso"
    echo "int v = 99;" > "$wt/lib/lvgl/src/lv_thing.c"
    run cat "$tmp/main/lib/lvgl/src/lv_thing.c"
    [ "$output" = "int v = 2;" ] || { echo "main tree was rewritten: $output" >&2; return 1; }

    # And the reverse: the main tree cannot rewrite the worktree's.
    echo "int v = 7;" > "$tmp/main/lib/lvgl/src/lv_thing.c"
    run cat "$wt/lib/lvgl/src/lv_thing.c"
    [ "$output" = "int v = 99;" ] || { echo "worktree was rewritten: $output" >&2; return 1; }
    rm -rf "$tmp"
}

@test "a private submodule is not marked skip-worktree, before or after migration" {
    # skip-worktree hides the symlink typechange for the shared submodules. On a
    # private checkout there is no typechange to hide, and the mark would instead
    # hide a real change of pinned revision from `git status`, `git add` and the
    # revision check — the one thing that has to stay visible.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    wt="$tmp/main/.worktrees/iso"
    run git -C "$wt" ls-files -v lib/lvgl
    [[ "$output" != S* ]] || { echo "marked skip-worktree: $output" >&2; return 1; }

    # A worktree set up before the submodule became private carries the mark
    # already; re-running has to clear it, not leave it.
    git -C "$wt" update-index --skip-worktree lib/lvgl
    run bash "$tmp/main/scripts/setup-worktree.sh" --setup-only --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
    run git -C "$wt" ls-files -v lib/lvgl
    [[ "$output" != S* ]] || { echo "mark not cleared: $output" >&2; return 1; }
    rm -rf "$tmp"
}

@test "an interrupted materialization is redone rather than left broken" {
    # The state an interrupted init leaves: a git dir with a gutted checkout
    # beside it. It does not self-heal, and the symptom is a build error naming
    # a missing object file, which points nowhere near submodules.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    wt="$tmp/main/.worktrees/iso"
    rm -rf "$wt/lib/lvgl/src"
    [ ! -f "$wt/lib/lvgl/src/lv_thing.c" ] || return 1

    run bash "$tmp/main/scripts/setup-worktree.sh" --setup-only --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
    [ -f "$wt/lib/lvgl/src/lv_thing.c" ] || { echo "not recovered" >&2; return 1; }
    rm -rf "$tmp"
}

@test "setup fails loudly when a private submodule is not at the pinned revision" {
    # A submodule at the wrong revision compiles, links, and is not the code the
    # branch describes. Nothing downstream reports it, so setup has to.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    wt="$tmp/main/.worktrees/iso"
    first=$(git -C "$tmp/upstream" rev-list --max-parents=0 HEAD)
    git -C "$wt/lib/lvgl" checkout -q --detach "$first"

    run bash "$tmp/main/scripts/setup-worktree.sh" --setup-only --no-build feat/iso
    [ "$status" -ne 0 ] || { echo "accepted a wrong revision" >&2; echo "$output" >&2; return 1; }
    [[ "$output" == *"this branch pins"* ]] || { echo "$output" >&2; return 1; }
    rm -rf "$tmp"
}

@test "a ccache that reports max_size without a unit does not abort setup" {
    # ccache answers --get-config max_size in two spellings: the parsable
    # "5.0G" and the human-readable "5.0 GiB". Under `set -o pipefail` a
    # unit-matching grep whose result decides an assignment's exit status
    # aborts the whole run on the first spelling, and the last thing printed is
    # an unrelated line about sloppiness.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    mock_command_script "ccache" 'case "$1" in
  --get-config)
    case "$2" in
      max_size) echo "5.0G" ;;
      hash_dir) echo "true" ;;
      *) echo "" ;;
    esac ;;
esac
exit 0'
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
    # Survival is half of it; the ceiling still has to be read correctly.
    contains "raised to 25G (was 5.0G)" "$output"
    rm -rf "$tmp"
}

@test "refuses to set up a worktree on top of the main tree, and destroys nothing" {
    tmp="$(mktemp -d)"
    git -C "$tmp" init -q
    git -C "$tmp" config user.email "t@example.invalid"
    git -C "$tmp" config user.name "t"
    mkdir -p "$tmp/lib/keepme" "$tmp/scripts"
    echo "precious" > "$tmp/lib/keepme/file.txt"
    cp "$SCRIPT" "$tmp/scripts/setup-worktree.sh"
    git -C "$tmp" add lib scripts
    git -C "$tmp" commit -qm init

    # "." makes WORKTREE_PATH resolve to the main tree itself -- the shape that
    # deleted lib/. The guard must stop it before the symlink loop runs.
    cd "$tmp" || return 1
    run bash "$tmp/scripts/setup-worktree.sh" somebranch .

    [ "$status" -ne 0 ]
    contains "same directory" "$output"
    # The real assertion: the destructive path never executed.
    [ -f "$tmp/lib/keepme/file.txt" ]
    [ "$(cat "$tmp/lib/keepme/file.txt")" = "precious" ]
    rm -rf "$tmp"
}
