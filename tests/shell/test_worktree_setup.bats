#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/setup-worktree.sh — lib/ sharing policy.
#
# The script symlinks each lib/ submodule into the main tree so a worktree
# builds in seconds instead of recompiling ~GB of submodules. That is right for
# the submodules nothing rewrites, and wrong for the ones that are rewritten
# per branch: lib/helix-xml is ours and is edited directly, and lib/lvgl,
# lib/libhv and lib/lua are rewritten by patches/, which is per-branch. Sharing
# one checkout between branches that disagree about either is unsatisfiable —
# each tree's correct action invalidates the other's — so those get a private
# checkout per worktree.

load helpers

setup() {
    load helpers
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
    # The submodule each list belongs to is its <NAME>_DIR in the Makefile.
    patched=$(grep -oE '^[A-Z0-9]+_PATCHED_FILES' mk/patches.mk | sort -u)
    [ "$(wc -w <<<"$patched")" -ge 3 ] || return 1
    private=$(grep -E '^LIB_PRIVATE_SUBMODULES=' "$SCRIPT")
    for p in $patched; do
        path=$(sed -n "s/^${p%_PATCHED_FILES}_DIR := //p" Makefile)
        [ -n "$path" ] || { echo "no ${p%_PATCHED_FILES}_DIR in the Makefile" >&2; return 1; }
        [[ "$private" == *"\"$path\""* ]] || { echo "patched but shared: $path" >&2; return 1; }
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
    mkdir -p "$root/main/scripts/lib" "$root/main/patches" "$root/main/mk"
    cp scripts/setup-worktree.sh "$root/main/scripts/"
    cp scripts/lib/worktree_lib.sh "$root/main/scripts/lib/"
    cp scripts/sync-worktree-mtimes.py "$root/main/scripts/"
    cp scripts/ccache-shared-config.sh "$root/main/scripts/"
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

@test "installed tool dependencies are linked from the main tree, the telemetry worker's included" {
    tmp="$(cd "$(mktemp -d)" && pwd -P)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    mkdir -p "$tmp/main/server/telemetry-worker"
    echo '{}' > "$tmp/main/server/telemetry-worker/package.json"
    git -C "$tmp/main" add server && git -C "$tmp/main" commit -qm worker
    mkdir -p "$tmp/main/node_modules" "$tmp/main/.venv" "$tmp/main/server/telemetry-worker/node_modules"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/deps
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    wt="$tmp/main/.worktrees/deps"
    for rel in node_modules .venv server/telemetry-worker/node_modules; do
        [ -L "$wt/$rel" ] || { echo "$rel is not a symlink" >&2; return 1; }
        [ "$(readlink "$wt/$rel")" = "$tmp/main/$rel" ] || { echo "$rel -> $(readlink "$wt/$rel")" >&2; return 1; }
    done
    rm -rf "$tmp"
}

@test "the usage text names the telemetry worker's node_modules link" {
    run bash "$SCRIPT" --help
    [[ "$output" == *"telemetry worker's node_modules"* ]] || { echo "$output" >&2; return 1; }
}

@test "lib/lua is a private checkout, not a symlink into the main tree" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    git -C "$tmp/main" -c protocol.file.allow=always submodule add -q "$tmp/upstream" lib/lua
    git -C "$tmp/main" commit -qm "lua submodule"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/iso
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    wt="$tmp/main/.worktrees/iso"
    [ ! -L "$wt/lib/lua" ] || { echo "lib/lua is a symlink: $(readlink "$wt/lib/lua")" >&2; return 1; }
    run cat "$wt/lib/lua/.git"
    [[ "$output" == *"worktrees/iso/modules/"*"lua" ]] || { echo "$output" >&2; return 1; }
    rm -rf "$tmp"
}

# A shared .git/modules/<name> gitdir has one core.worktree for every tree.
# --unlink leaves an empty directory git may initialize, which aims that pointer
# here; --relink puts the symlink back and must aim the pointer back too, or
# the main tree's `git status` depends on this worktree existing
# (prestonbrown/helixscreen#1621).
#
# lib/ftxui is a real shared submodule, so --relink leaves the worktree's
# lib/ftxui a symlink into the main tree by the time pointers are checked.
add_shared_submodule() {
    local root="$1"
    git -C "$root/main" -c protocol.file.allow=always submodule add -q "$root/upstream" lib/ftxui
    git -C "$root/main" commit -qm "shared submodule"
}

@test "--relink restores a shared submodule pointer aimed into the worktree" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    add_shared_submodule "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/relink
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    modules="$tmp/main/.git/modules"
    mkdir -p "$modules/spdlog"
    git config --file "$modules/lib/ftxui/config" core.worktree "../../../../.worktrees/relink/lib/ftxui"
    git config --file "$modules/spdlog/config" core.worktree "../../../lib/spdlog"

    cd "$tmp/main/.worktrees/relink" || return 1
    run bash scripts/setup-worktree.sh --relink
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
    [ -L lib/ftxui ] || { echo "lib/ftxui is not a symlink" >&2; return 1; }

    [ "$(git config --file "$modules/lib/ftxui/config" core.worktree)" = "../../../../lib/ftxui" ]
    [ "$(git config --file "$modules/spdlog/config" core.worktree)" = "../../../lib/spdlog" ]
    cd / && rm -rf "$tmp"
}

@test "--relink restores an absolute pointer aimed into the worktree" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    add_shared_submodule "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/absolute
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    modules="$tmp/main/.git/modules"
    git config --file "$modules/lib/ftxui/config" core.worktree "$tmp/main/.worktrees/absolute/lib/ftxui"

    cd "$tmp/main/.worktrees/absolute" || return 1
    run bash scripts/setup-worktree.sh --relink
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    [ "$(git config --file "$modules/lib/ftxui/config" core.worktree)" = "../../../../lib/ftxui" ]
    cd / && rm -rf "$tmp"
}

# A worktree outside the main tree gets a pointer with a longer ../ run than the
# gitdir needs to reach the main tree, so the rewrite cannot keep its prefix.
@test "--relink restores a pointer into a worktree that lives outside the main tree" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    add_shared_submodule "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/away "$tmp/away"
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    modules="$tmp/main/.git/modules"
    git config --file "$modules/lib/ftxui/config" core.worktree "../../../../../away/lib/ftxui"

    cd "$tmp/away" || return 1
    run bash scripts/setup-worktree.sh --relink
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    [ "$(git config --file "$modules/lib/ftxui/config" core.worktree)" = "../../../../lib/ftxui" ]
    cd / && rm -rf "$tmp"
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

@test "a branch whose patches differ gets a checkout free of the main tree's patches" {
    # The private checkout is copied from the main tree, applied patches and all.
    # reapply-patches resets only the files the BRANCH's patch list names, so a
    # patch only the main tree carries - one hunk edited, one file created - has to
    # be undone before it, or it ships in the branch's build.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    main="$tmp/main"
    printf 'reapply-patches:\n\t@true\n' > "$main/Makefile"
    git -C "$main" add Makefile
    git -C "$main" commit -qm stub
    git -C "$main" branch rel

    cat > "$main/patches/lvgl_main_only.patch" <<'PATCH'
diff --git a/src/lv_thing.c b/src/lv_thing.c
--- a/src/lv_thing.c
+++ b/src/lv_thing.c
@@ -1 +1 @@
-int v = 2;
+int v = 3;
diff --git a/src/lv_new.c b/src/lv_new.c
new file mode 100644
--- /dev/null
+++ b/src/lv_new.c
@@ -0,0 +1 @@
+int n = 1;
PATCH
    git -C "$main" add patches/lvgl_main_only.patch
    git -C "$main" commit -qm "main-only patch"
    git -C "$main/lib/lvgl" apply "$main/patches/lvgl_main_only.patch"

    # A Mac's sed has no GNU extensions; --posix gives GNU sed the same limits.
    local real_sed
    real_sed=$(command -v sed)
    mock_command_script sed "if \"$real_sed\" --version >/dev/null 2>&1; then exec \"$real_sed\" --posix \"\$@\"; fi
exec \"$real_sed\" \"\$@\""

    run bash "$main/scripts/setup-worktree.sh" --base rel --no-build feat/back
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }

    wt="$main/.worktrees/back"
    run cat "$wt/lib/lvgl/src/lv_thing.c"
    [ "$output" = "int v = 2;" ] || { echo "main-only hunk survived: $output" >&2; return 1; }
    [ ! -e "$wt/lib/lvgl/src/lv_new.c" ] || { echo "main-only file survived" >&2; return 1; }

    # The main tree keeps its own patches.
    run cat "$main/lib/lvgl/src/lv_thing.c"
    [ "$output" = "int v = 3;" ] || { echo "main tree was rewritten: $output" >&2; return 1; }
    [ -e "$main/lib/lvgl/src/lv_new.c" ] || { echo "main tree lost its file" >&2; return 1; }
    rm -rf "$tmp"
}

@test "a private checkout copied from an unpatched main tree gets this branch's patches" {
    # Matching patches/ says the main tree SHOULD carry the patches, not that its
    # checkout does. A pristine main checkout copies into a pristine private one,
    # and only the marker check notices.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    main="$tmp/main"
    cat > "$main/scripts/check_patch_markers.py" <<'PY'
import sys
from pathlib import Path
lvgl = Path(sys.argv[sys.argv.index("--lvgl") + 1])
sys.exit(0 if "int v = 9;" in (lvgl / "src/lv_thing.c").read_text() else 1)
PY
    printf 'reapply-patches:\n\t@echo "int v = 9;" > lib/lvgl/src/lv_thing.c\n' > "$main/Makefile"
    git -C "$main" add Makefile scripts/check_patch_markers.py
    git -C "$main" commit -qm "marker stub"

    run bash "$main/scripts/setup-worktree.sh" --base HEAD --no-build feat/unpatched
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
    [[ "$output" == *"missing patches"* ]] || { echo "$output" >&2; return 1; }
    run cat "$main/.worktrees/unpatched/lib/lvgl/src/lv_thing.c"
    [ "$output" = "int v = 9;" ] || { echo "copy left unpatched: $output" >&2; return 1; }
    rm -rf "$tmp"
}

@test "a cloned contributors header is kept only when its inputs match this branch" {
    # The header is generated from CONTRIBUTORS.txt by gen-contributors.sh. A
    # clone made from another branch's inputs can read newer than this branch's
    # and survive the build.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    main="$tmp/main"
    echo "alice" > "$main/CONTRIBUTORS.txt"
    git -C "$main" add CONTRIBUTORS.txt
    git -C "$main" commit -qm contributors
    git -C "$main" branch older
    echo "bob" >> "$main/CONTRIBUTORS.txt"
    git -C "$main" commit -qam "another contributor"
    mkdir -p "$main/build/generated"
    echo "kContributors main" > "$main/build/generated/contributors.h"

    run bash "$main/scripts/setup-worktree.sh" --base HEAD --no-build feat/same
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
    [ -f "$main/.worktrees/same/build/generated/contributors.h" ] || { echo "dropped a matching header" >&2; return 1; }

    run bash "$main/scripts/setup-worktree.sh" --base older --no-build feat/other
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
    [ ! -e "$main/.worktrees/other/build/generated/contributors.h" ] || { echo "kept another branch's header" >&2; return 1; }
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
    contains "max_size: 25G (was 5.0G)" "$output"
    rm -rf "$tmp"
}

@test "refuses to set up a worktree on top of the main tree, and destroys nothing" {
    tmp="$(mktemp -d)"
    git -C "$tmp" init -q
    git -C "$tmp" config user.email "t@example.invalid"
    git -C "$tmp" config user.name "t"
    mkdir -p "$tmp/lib/keepme" "$tmp/scripts/lib"
    echo "precious" > "$tmp/lib/keepme/file.txt"
    cp "$SCRIPT" "$tmp/scripts/setup-worktree.sh"
    cp scripts/lib/worktree_lib.sh "$tmp/scripts/lib/"
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

# The compile database is inherited from the main tree and describes ITS branch.
# A source that exists there and not on the worktree's branch arrives as an entry
# naming a file that is not here, and quality-checks.sh hands every entry to
# clang++ — so the pre-push hook rejects a push over files the branch never had.
# Both halves have to be pruned: the build reassembles the JSON from the .ccj
# fragments, so cleaning only the JSON lets the next build restore the phantoms.

@test "a compile_commands entry for a file absent on this branch is dropped" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"

    # main-tree database naming one real source and one that only exists there
    cat > "$tmp/main/compile_commands.json" <<JSON
[
  {"directory": "$tmp/main", "file": "$tmp/main/scripts/setup-worktree.sh", "command": "cc -c real"},
  {"directory": "$tmp/main", "file": "$tmp/main/src/only_on_main.c", "command": "cc -c src/only_on_main.c"}
]
JSON

    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/ccj
    [ "$status" -eq 0 ] || fail "setup failed: $output"

    wt="$tmp/main/.worktrees/ccj"
    [ -f "$wt/compile_commands.json" ] || fail "no compile_commands.json produced"

    # The absent source must be gone; the present one must survive, or the
    # filter is just deleting the database.
    run grep -c "only_on_main.c" "$wt/compile_commands.json"
    [ "$output" = "0" ] || fail "phantom entry survived: $(cat "$wt/compile_commands.json")"
    grep -q "setup-worktree.sh" "$wt/compile_commands.json" \
        || fail "real entry was dropped too: $(cat "$wt/compile_commands.json")"
}

@test "a .ccj fragment for a file absent on this branch is dropped" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"

    mkdir -p "$tmp/main/build/obj"
    printf '{"directory":"%s","file":"%s/scripts/setup-worktree.sh","command":"cc"}\n' \
        "$tmp/main" "$tmp/main" > "$tmp/main/build/obj/mainpath.ccj"
    printf '{"directory":"%s","file":"%s/src/only_on_main.c","command":"cc"}\n' \
        "$tmp/main" "$tmp/main" > "$tmp/main/build/obj/phantom.ccj"

    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/frag
    [ "$status" -eq 0 ] || fail "setup failed: $output"

    wt="$tmp/main/.worktrees/frag"
    # Prove the clone and the prune both actually ran. Without this the two
    # assertions below pass on files that were never copied in the first place.
    [ -d "$wt/build/obj" ] || fail "build/obj was never cloned: $output"
    echo "$output" | grep -q "pruned" \
        || fail "prune step never reported: $output"

    # A fragment for a source this branch lacks is dropped, or the next build
    # reassembles the database with it and the gate fails again.
    [ ! -f "$wt/build/obj/phantom.ccj" ] \
        || fail "phantom fragment survived; the next build would restore it"

    # One whose source DOES exist here is kept and repointed at this worktree —
    # dropping it would throw away the cloned-object benefit, and leaving the
    # main-tree path would put another tree's source in this database.
    [ -f "$wt/build/obj/mainpath.ccj" ] \
        || fail "a usable fragment was pruned; clangd would start empty"
    # Assert on the shape, not the absolute prefix: /var and /private/var name
    # the same directory on macOS and either spelling can appear.
    grep -q "\.worktrees/frag/scripts/setup-worktree\.sh" "$wt/build/obj/mainpath.ccj" \
        || fail "fragment was not repointed: $(cat "$wt/build/obj/mainpath.ccj")"
}

# --- an existing worktree is somebody's -----------------------------------------
#
# The default path is .worktrees/<last segment of the branch>, so a new branch
# can name a tree another session is working in. Every setup step rewrites what
# it finds there (private submodule patches reset, mtimes synced, build outputs
# pruned), so an existing directory stops the script unless it is a deliberate
# --setup-only of the same branch by a session that does not collide with a
# live claim on it.

# Stamps the existing tree's private submodule so a test can tell whether setup
# touched it.
mark_tree() {
    echo "int wip = 1;" > "$1/lib/lvgl/src/lv_thing.c"
}

@test "a new branch whose last segment names another branch's worktree is refused untouched" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build fix/shared-name
    [ "$status" -eq 0 ] || fail "first setup failed: $output"
    wt="$tmp/main/.worktrees/shared-name"
    mark_tree "$wt"

    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build test/shared-name
    [ "$status" -eq 1 ] || fail "a colliding branch was set up: $output"
    [[ "$output" == *"already holds branch 'fix/shared-name'"* ]] || fail "no reason given: $output"
    grep -q "wip" "$wt/lib/lvgl/src/lv_thing.c" || fail "the existing tree's submodule was rewritten"
    run git -C "$tmp/main" rev-parse --verify --quiet test/shared-name
    [ "$status" -ne 0 ] || fail "the colliding branch was created anyway"
    [ "$(git -C "$wt" rev-parse --abbrev-ref HEAD)" = "fix/shared-name" ]
    rm -rf "$tmp"
}

@test "re-running for the same branch without --setup-only is refused untouched" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/again
    [ "$status" -eq 0 ] || fail "first setup failed: $output"
    wt="$tmp/main/.worktrees/again"
    mark_tree "$wt"

    run bash "$tmp/main/scripts/setup-worktree.sh" --no-build feat/again
    [ "$status" -eq 1 ] || fail "an existing worktree was set up again: $output"
    [[ "$output" == *"--setup-only"* ]] || fail "no way forward offered: $output"
    grep -q "wip" "$wt/lib/lvgl/src/lv_thing.c" || fail "the existing tree's submodule was rewritten"
    rm -rf "$tmp"
}

@test "--setup-only of the same branch still runs" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/again
    [ "$status" -eq 0 ] || fail "first setup failed: $output"
    run bash "$tmp/main/scripts/setup-worktree.sh" --setup-only --no-build feat/again
    [ "$status" -eq 0 ] || fail "a deliberate re-setup was refused: $output"
    rm -rf "$tmp"
}

@test "--setup-only of a different branch's worktree is refused" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build fix/shared-name
    [ "$status" -eq 0 ] || fail "first setup failed: $output"
    run bash "$tmp/main/scripts/setup-worktree.sh" --setup-only --no-build test/shared-name
    [ "$status" -eq 1 ] || fail "set up a tree holding another branch: $output"
    rm -rf "$tmp"
}

@test "a live claim by another session refuses even a same-branch --setup-only" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf" HELIX_CLAIM_DIR="$tmp/claims"
    build_fixture_repo "$tmp"
    cp scripts/helix-claim "$tmp/main/scripts/"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/held
    [ "$status" -eq 0 ] || fail "first setup failed: $output"
    wt="$tmp/main/.worktrees/held"
    mark_tree "$wt"

    sleep 60 &
    holder=$!
    (cd "$tmp/main" && scripts/helix-claim take worktree:held impl --pid "$holder") >/dev/null
    run bash "$tmp/main/scripts/setup-worktree.sh" --setup-only --no-build feat/held
    kill "$holder"
    [ "$status" -eq 1 ] || fail "set up a tree another session holds: $output"
    [[ "$output" == *"another session holds"* ]] || fail "no reason given: $output"
    grep -q "wip" "$wt/lib/lvgl/src/lv_thing.c" || fail "the held tree's submodule was rewritten"
    rm -rf "$tmp"
}

@test "your own claim on the tree does not block a --setup-only" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf" HELIX_CLAIM_DIR="$tmp/claims"
    build_fixture_repo "$tmp"
    cp scripts/helix-claim "$tmp/main/scripts/"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/mine
    [ "$status" -eq 0 ] || fail "first setup failed: $output"
    (cd "$tmp/main" && scripts/helix-claim take worktree:mine impl) >/dev/null
    run bash "$tmp/main/scripts/setup-worktree.sh" --setup-only --no-build feat/mine
    [ "$status" -eq 0 ] || fail "your own claim blocked you: $output"
    rm -rf "$tmp"
}

# --- build stamps that decide whether the cloned objects survive ---------------
#
# Each of these is either compared by content at parse time (and rewritten `now`
# on a mismatch) or is a prerequisite of build/.patches-applied, which every
# object reaches. A fresh one on any of them rebuilds the whole cloned tree, and
# a stale .build-target makes the first make run `make clean`.

@test "the main tree's build stamps reach the worktree with content and mtime" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    mkdir -p "$tmp/main/build"
    printf 'native | some compiler 1.0\n' > "$tmp/main/build/.build-target"
    printf '123_456' > "$tmp/main/build/.thirdparty-abi"
    printf '789_10' > "$tmp/main/build/.patches-applied-id"
    touch -d '2020-01-02 03:04:05' "$tmp/main/build/.thirdparty-abi" "$tmp/main/build/.patches-applied-id"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/stamps
    [ "$status" -eq 0 ] || fail "setup failed: $output"

    wt="$tmp/main/.worktrees/stamps"
    for f in build/.build-target build/.thirdparty-abi build/.patches-applied-id; do
        cmp -s "$tmp/main/$f" "$wt/$f" || fail "$f differs from the main tree's: $(cat "$wt/$f" 2>&1)"
    done
    for f in build/.thirdparty-abi build/.patches-applied-id; do
        [ "$(stat -c %Y "$wt/$f")" = "$(stat -c %Y "$tmp/main/$f")" ] || fail "$f has a fresh mtime"
    done
    rm -rf "$tmp"
}

@test "with no .build-target in the main tree the worktree gets none" {
    # make then records the current toolchain without cleaning. Any value the
    # script invents can disagree with make's and clean the cloned objects away.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/nomarker
    [ "$status" -eq 0 ] || fail "setup failed: $output"
    [ ! -e "$tmp/main/.worktrees/nomarker/build/.build-target" ] || fail "an invented .build-target was written"
    rm -rf "$tmp"
}

@test "a private submodule's HEAD adopts the main tree's HEAD mtime" {
    # The HEAD file is a prerequisite of build/.patches-applied.
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    main_head="$(git -C "$tmp/main/lib/lvgl" rev-parse --absolute-git-dir)/HEAD"
    touch -d '2020-01-02 03:04:05' "$main_head"
    run bash "$tmp/main/scripts/setup-worktree.sh" --base HEAD --no-build feat/headtime
    [ "$status" -eq 0 ] || fail "setup failed: $output"

    wt_head="$(git -C "$tmp/main/.worktrees/headtime/lib/lvgl" rev-parse --absolute-git-dir)/HEAD"
    [ "$(stat -c %Y "$wt_head")" = "$(stat -c %Y "$main_head")" ] || fail "HEAD has a fresh mtime"
    rm -rf "$tmp"
}

# --- seeding build/obj ----------------------------------------------------------
#
# make links every object it finds. An object whose source this branch does not
# have (or a source it adds) has no fresh mtime to force a rebuild, so it would
# be linked into a binary that matches neither branch.

@test "build/obj is not cloned when the base's source set differs from the main tree's" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    main="$tmp/main"
    git -C "$main" branch rel
    mkdir -p "$main/src" "$main/build/obj"
    echo 'int only_on_main;' > "$main/src/only_on_main.c"
    git -C "$main" add src/only_on_main.c
    git -C "$main" commit -qm "main-only source"
    : > "$main/build/obj/only_on_main.o"

    run bash "$main/scripts/setup-worktree.sh" --base rel --no-build feat/stale
    [ "$status" -eq 0 ] || fail "setup failed: $output"
    [ ! -e "$main/.worktrees/stale/build/obj/only_on_main.o" ] \
        || fail "main's object was seeded into a branch without its source"
    rm -rf "$tmp"
}

@test "build/obj is cloned when the base has the same source set as the main tree" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    main="$tmp/main"
    git -C "$main" branch rel
    mkdir -p "$main/build/obj"
    : > "$main/build/obj/seeded.o"
    # A change to an existing source is not a reason to skip the clone.
    echo '# touched' >> "$main/scripts/setup-worktree.sh"
    git -C "$main" commit -qam "edit"

    run bash "$main/scripts/setup-worktree.sh" --base rel --no-build feat/seeded
    [ "$status" -eq 0 ] || fail "setup failed: $output"
    [ -e "$main/.worktrees/seeded/build/obj/seeded.o" ] || fail "object was not cloned: $output"
    rm -rf "$tmp"
}

@test "build/obj is not cloned when a lib/ submodule is pinned at another revision" {
    tmp="$(mktemp -d)"
    export CCACHE_CONFIGPATH="$tmp/ccache.conf"
    build_fixture_repo "$tmp"
    main="$tmp/main"
    printf 'reapply-patches:\n\t@true\n' > "$main/Makefile"
    git -C "$main" add Makefile
    git -C "$main" commit -qm stub
    git -C "$main" branch rel
    git -C "$main/lib/lvgl" -c user.name=t -c user.email=t@t commit -q --allow-empty -m bump
    git -C "$main" commit -qam "bump lvgl pin"
    mkdir -p "$main/build/obj"
    : > "$main/build/obj/lvgl_obj.o"

    run bash "$main/scripts/setup-worktree.sh" --base rel --no-build feat/pin
    [ "$status" -eq 0 ] || fail "setup failed: $output"
    [ ! -e "$main/.worktrees/pin/build/obj/lvgl_obj.o" ] \
        || fail "objects built against another lvgl revision were seeded"
    rm -rf "$tmp"
}

@test "every make the script runs picks gmake only on macOS" {
    # On Linux `make` may be the jobpool shim; /usr/bin/gmake bypasses it and
    # the worktree's cold build runs its own -j beside every pooled build.
    # On macOS bare `make` is Apple's 3.81, which the Makefile refuses.
    lacks 'command -v gmake ||' "$(cat "$SCRIPT")"
    contains 'MAKE_BIN="gmake"' "$(cat "$SCRIPT")"
    contains '"$(uname -s)" == Darwin' "$(cat "$SCRIPT")"
    contains '"$MAKE_BIN" reapply-patches' "$(cat "$SCRIPT")"
    contains '"$MAKE_BIN" -j' "$(cat "$SCRIPT")"
    # A make run as a command (not inside echo text or a comment) goes
    # through MAKE_BIN.
    run bash -c "grep -nE '^[[:space:]]*(if !? *)?make[[:space:]]' '$SCRIPT'"
    [ -z "$output" ] || fail "bare make invocation: $output"
}
