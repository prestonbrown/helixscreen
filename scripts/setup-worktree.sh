#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# HelixScreen Worktree Setup Script
# Creates or configures a git worktree for fast isolated builds

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

usage() {
    echo "Usage: $0 [OPTIONS] <branch-name> [worktree-path]"
    echo ""
    echo "Creates or sets up a git worktree for fast isolated builds."
    echo ""
    echo "Arguments:"
    echo "  branch-name     Branch to checkout (will be created if it doesn't exist)"
    echo "  worktree-path   Path for worktree (default: .worktrees/<branch-name>)"
    echo "                  NOTE: this is a PATH, not a base branch. To branch from"
    echo "                  something other than HEAD, use --base."
    echo ""
    echo "Options:"
    echo "  --base <ref>    Commit/branch to create the new branch from"
    echo "                  (default: HEAD of the tree you run this from)"
    echo "  --setup-only    Only set up an existing worktree, don't create it"
    echo "  --unlink        Replace lib/ symlinks with what git expects, so git"
    echo "                  status/merge/rebase/stash work in this worktree"
    echo "  --relink        Restore the lib/ symlinks after --unlink"
    echo "  --no-build      Skip the initial build after setup"
    echo "  -h, --help      Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 feature/new-panel           # Create worktree in .worktrees/new-panel"
    echo "  $0 feature/foo /tmp/foo        # Create worktree in /tmp/foo"
    echo "  $0 --base feature/a feature/b  # Branch feature/b off feature/a, not HEAD"
    echo "  $0 --setup-only feature/i18n   # Just set up existing worktree"
    echo ""
    echo "  # Merging or rebasing inside a worktree (git cannot scan symlinked submodules):"
    echo "  $0 --unlink                    # from inside the worktree"
    echo "  git merge origin/main           # resolve any conflicts now"
    echo "  $0 --relink                    # REQUIRED before committing, and to build"
    echo "  git commit                      # hook compiles, so relink must come first"
    echo ""
    echo "  Commits do NOT need --unlink; only whole-tree ops (status/merge/rebase/stash) do."
    echo "  Never run --unlink/--relink while a build is in flight."
    echo ""
    echo "Strategy:"
    echo "  - Adopts the main tree's mtimes for byte-identical files (so the cloned"
    echo "    objects are not all invalidated by the fresh checkout timestamp)"
    echo "  - Configures ccache for cross-worktree reuse (no cold rebuild per worktree)"
    echo "  - Clones build/obj/ from main tree (APFS copy-on-write — instant, zero disk)"
    echo "  - Symlinks lib/ from main tree (all submodule sources + generated headers)"
    echo "  - Clones compiled libraries (libhv.a) and the PCH — copies, not symlinks,"
    echo "    so a rebuild here can never write back into the main tree"
    echo "  - Copies compile_commands.json with rewritten paths for clangd"
    echo "  - Symlinks node_modules and .venv for font/python tools"
    echo "  - Uses .git/info/exclude for clean git status"
}

# --- lib/ link management ---------------------------------------------------
# A worktree shares the main tree's submodule checkouts, and their IN-TREE build
# artifacts, by symlinking each lib/ entry. That sharing is the whole point: it
# is why a fresh worktree builds in seconds instead of recompiling every
# submodule from cold.
#
# The cost is that git refuses to scan a tree where a gitlink path is a symlink:
#   error: expected submodule path 'lib/cpp-terminal' not to be a symbolic link
# which aborts `git status`, `merge`, `rebase` and `stash` outright.
#
# So: --unlink before a merge/rebase, --relink after. Relinking is NOT optional;
# the empty submodule dirs left by --unlink have no headers, so the build fails
# with 'lvgl.h file not found' until the symlinks are back.
#
# Do NOT unlink to commit. `git add <paths>` and `git commit` both work fine with
# the symlinks in place, and the pre-commit hook compiles the tree — so committing
# while unlinked fails with "Build failed - fix compilation errors". That includes
# concluding a merge: resolve conflicts unlinked, then --relink, THEN commit.
#
# Nothing here may run while a build is in flight: pulling lib/ out from under a
# compile fails it with missing headers, and re-linking mid-compile is no better.
LIB_NON_SUBMODULE_ITEMS=("tuibox.h" "mdns")

# Submodules that get a PRIVATE checkout per worktree instead of a symlink.
#
# lib/helix-xml is ours (prestonbrown/helix-xml) and CLAUDE.md says to edit it
# directly rather than carry a patch — which makes a symlink actively wrong:
# every worktree would be editing the MAIN tree's submodule working tree, so two
# branches could not hold different engine versions, and an edit made here would
# surface as dirt in main's `git status` for another session to sweep up.
# Cloning it costs ~2.6 MB and a couple of seconds, against the ~GB and minutes
# that make symlinking lvgl/libhv worthwhile. A real checkout is also what git
# expects, so these need no --unlink/--relink dance.
LIB_PRIVATE_SUBMODULES=("lib/helix-xml")

is_private_submodule() {
    local candidate="$1" p
    for p in "${LIB_PRIVATE_SUBMODULES[@]}"; do
        [[ "$p" == "$candidate" ]] && return 0
    done
    return 1
}

# Copy a build artifact into the worktree as cheaply as the filesystem allows,
# preserving mtime — make's up-to-date decisions depend on it.
#   macOS/APFS:  cp -c            -> clonefile(2), instant, zero disk until diverged
#   Linux/btrfs+xfs: --reflink=auto -> same idea, silently falls back to a full copy
#   anything else: a plain copy
clone_file() {
    local src="$1" dst="$2"
    cp -c "$src" "$dst" 2>/dev/null \
        || cp --reflink=auto "$src" "$dst" 2>/dev/null \
        || cp "$src" "$dst"
    touch -r "$src" "$dst"
}

# Symlinked submodules only. A private checkout is a normal submodule as far as
# git is concerned, so including it here would have --unlink replace a real
# checkout (possibly holding uncommitted engine work) with an empty directory.
lib_submodule_paths() {
    local path
    git -C "$MAIN_TREE" config --file .gitmodules --get-regexp path \
        | grep "^submodule\." | awk '{print $2}' | grep "^lib/" \
    | while read -r path; do
        is_private_submodule "$path" || echo "$path"
    done
}

# Replace symlinks with what git expects: an empty directory for a submodule
# (i.e. "not initialized", which git tolerates), and real content for the
# tracked non-submodule entries, which would otherwise read as deleted.
unlink_lib_for_git() {
    echo -e "${CYAN}Unlinking lib/ so git can scan this worktree...${RESET}"
    local submod name
    for submod in $(lib_submodule_paths); do
        if [[ -L "$WORKTREE_PATH/$submod" ]]; then
            rm "$WORKTREE_PATH/$submod"
            mkdir -p "$WORKTREE_PATH/$submod"
            echo -e "  $submod: ${YELLOW}symlink -> empty dir${RESET}"
        fi
    done
    for name in "${LIB_NON_SUBMODULE_ITEMS[@]}"; do
        if [[ -L "$WORKTREE_PATH/lib/$name" ]]; then
            rm "$WORKTREE_PATH/lib/$name"
            cp -R "$MAIN_TREE/lib/$name" "$WORKTREE_PATH/lib/$name"
            echo -e "  lib/$name: ${YELLOW}symlink -> real copy (tracked content)${RESET}"
        fi
    done
    echo -e "${GREEN}✓ git operations enabled — run --relink when done${RESET}"
}

# Parse arguments
SETUP_ONLY=false
NO_BUILD=false
LINK_MODE=""
BRANCH=""
WORKTREE_PATH=""
BASE_REF=""
EXPLICIT_PATH=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --setup-only)
            SETUP_ONLY=true
            shift
            ;;
        --no-build)
            NO_BUILD=true
            shift
            ;;
        --base)
            if [[ -z "${2:-}" ]]; then
                echo -e "${RED}Error: --base needs a commit or branch${RESET}"
                exit 1
            fi
            BASE_REF="$2"
            shift 2
            ;;
        --unlink|--relink)
            LINK_MODE="${1#--}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo -e "${RED}Unknown option: $1${RESET}"
            usage
            exit 1
            ;;
        *)
            if [[ -z "$BRANCH" ]]; then
                BRANCH="$1"
            elif [[ -z "$WORKTREE_PATH" ]]; then
                WORKTREE_PATH="$1"
                EXPLICIT_PATH="$1"
            else
                echo -e "${RED}Too many arguments${RESET}"
                usage
                exit 1
            fi
            shift
            ;;
    esac
done

# Get the main tree root (where this script lives)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN_TREE="$(cd "$SCRIPT_DIR/.." && pwd)"

# Auto-detect: if run from inside an existing worktree with no args, set up in-place
if [[ -z "$BRANCH" ]]; then
    # Check if we're inside a git worktree (not the main tree)
    CURRENT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    GIT_COMMON="$(git rev-parse --git-common-dir 2>/dev/null || true)"
    if [[ -n "$CURRENT_ROOT" && -n "$GIT_COMMON" ]] && \
       [[ "$(cd "$CURRENT_ROOT" && pwd)" != "$(cd "$GIT_COMMON/.." && pwd)" ]]; then
        # We're in a worktree — infer branch and path
        BRANCH="$(git rev-parse --abbrev-ref HEAD)"
        WORKTREE_PATH="$CURRENT_ROOT"
        # Override MAIN_TREE: SCRIPT_DIR/.. would resolve to the worktree root,
        # not the actual main tree. Use git-common-dir to find the real main tree.
        MAIN_TREE="$(cd "$GIT_COMMON/.." && pwd)"
        SETUP_ONLY=true
        echo -e "${YELLOW}Auto-detected worktree: $WORKTREE_PATH (branch: $BRANCH)${RESET}"
        echo -e "${YELLOW}Main tree: $MAIN_TREE${RESET}"
    else
        echo -e "${RED}Error: branch-name is required (or run from inside a worktree)${RESET}"
        usage
        exit 1
    fi
fi

# Default worktree path: .worktrees/<branch-basename>
if [[ -z "$WORKTREE_PATH" ]]; then
    # Extract just the last part of the branch name (e.g., feature/foo -> foo)
    BRANCH_BASENAME="${BRANCH##*/}"
    WORKTREE_PATH="$MAIN_TREE/.worktrees/$BRANCH_BASENAME"
fi

# Make worktree path absolute
if [[ ! "$WORKTREE_PATH" = /* ]]; then
    WORKTREE_PATH="$MAIN_TREE/$WORKTREE_PATH"
fi

# Guard: arg 2 is a PATH. Passing a base branch there is an easy slip, and it
# used to succeed silently — mkdir -p created a real directory of that name in
# the repo root and the branch was cut from HEAD instead of the intended base.
if [[ -n "$EXPLICIT_PATH" && ! -e "$WORKTREE_PATH" ]] \
   && git -C "$MAIN_TREE" rev-parse --verify --quiet "$EXPLICIT_PATH" >/dev/null; then
    echo -e "${RED}Error: '$EXPLICIT_PATH' is a git ref, but argument 2 is the worktree PATH.${RESET}"
    echo -e "To branch from it instead:"
    echo -e "  ${CYAN}$0 --base $EXPLICIT_PATH $BRANCH${RESET}"
    echo -e "To really use it as a path, create the directory first."
    exit 1
fi

# Guard: keep worktrees out of the tracked tree. Anywhere outside the main tree
# (/tmp/foo and friends) is fine; inside it, only .worktrees/ is — otherwise the
# new tree shows up as a mountain of untracked files in the main tree's status.
if [[ "$WORKTREE_PATH" == "$MAIN_TREE"/* && "$WORKTREE_PATH" != "$MAIN_TREE"/.worktrees/* ]] \
   && [[ ! -e "$WORKTREE_PATH" ]]; then
    echo -e "${RED}Error: refusing to create a worktree inside the main tree at${RESET}"
    echo -e "  $WORKTREE_PATH"
    echo -e "Use ${CYAN}.worktrees/<name>${RESET} (the default), or a path outside $MAIN_TREE."
    exit 1
fi

echo -e "${BOLD}${CYAN}HelixScreen Worktree Setup${RESET}"
echo -e "Main tree:    $MAIN_TREE"
echo -e "Worktree:     $WORKTREE_PATH"
echo -e "Branch:       $BRANCH"
echo ""

# Step 1: Create or verify the worktree
if [[ "$SETUP_ONLY" == "false" ]]; then
    if [[ -d "$WORKTREE_PATH" ]]; then
        echo -e "${YELLOW}Worktree already exists at $WORKTREE_PATH${RESET}"
    else
        # Validate BEFORE creating anything, so a rejected invocation leaves no
        # half-made directory behind.
        BRANCH_EXISTS=false
        if git -C "$MAIN_TREE" rev-parse --verify --quiet "$BRANCH" >/dev/null; then
            BRANCH_EXISTS=true
            if [[ -n "$BASE_REF" ]]; then
                echo -e "${RED}Error: branch '$BRANCH' already exists, so --base would be ignored.${RESET}"
                echo -e "Drop --base to check it out, or pick a new branch name."
                exit 1
            fi
        elif [[ -n "$BASE_REF" ]] \
             && ! git -C "$MAIN_TREE" rev-parse --verify --quiet "$BASE_REF" >/dev/null; then
            echo -e "${RED}Error: base '$BASE_REF' is not a valid commit or branch${RESET}"
            exit 1
        fi

        echo -e "${CYAN}Creating worktree...${RESET}"
        mkdir -p "$(dirname "$WORKTREE_PATH")"

        if [[ "$BRANCH_EXISTS" == "true" ]]; then
            git -C "$MAIN_TREE" worktree add "$WORKTREE_PATH" "$BRANCH"
        else
            # Default to the HEAD of whichever tree invoked us, which is NOT
            # necessarily main — say which commit that resolved to, so a stale
            # or unexpected base is visible now rather than three commits later.
            BASE="${BASE_REF:-HEAD}"
            BASE_DESC="$(git -C "$MAIN_TREE" log --oneline -1 "$BASE")"
            echo -e "${YELLOW}Branch '$BRANCH' doesn't exist, creating from ${BOLD}$BASE${RESET}${YELLOW}:${RESET}"
            echo -e "  ${CYAN}$BASE_DESC${RESET}"
            git -C "$MAIN_TREE" worktree add -b "$BRANCH" "$WORKTREE_PATH" "$BASE"
        fi
        echo -e "${GREEN}✓ Worktree created${RESET}"
    fi
else
    if [[ ! -d "$WORKTREE_PATH" ]]; then
        echo -e "${RED}Error: Worktree doesn't exist at $WORKTREE_PATH${RESET}"
        echo -e "Use without --setup-only to create it"
        exit 1
    fi
fi

cd "$WORKTREE_PATH"

# Step 2: Symlink lib/ submodules from main tree (instead of cloning fresh)
link_lib_from_main() {
    # Step 2: Symlink lib/ submodules from main tree (instead of cloning fresh)
    # This includes source headers AND generated files (like libhv/include/hv/)
    # We symlink each submodule directory individually to preserve lib/ structure
    echo -e "${CYAN}Symlinking lib/ submodules from main tree...${RESET}"

    # Get list of submodules in lib/
    SUBMODULES=$(git -C "$MAIN_TREE" config --file .gitmodules --get-regexp path | grep "^submodule\." | awk '{print $2}' | grep "^lib/")
    # Also include non-submodule files in lib/
    LIB_ITEMS=("tuibox.h" "mdns")

    # Ensure lib/ directory exists
    mkdir -p "$WORKTREE_PATH/lib"

    # Symlink each submodule directory
    for submod in $SUBMODULES; do
        MAIN_SUBMOD="$MAIN_TREE/$submod"
        WORKTREE_SUBMOD="$WORKTREE_PATH/$submod"

        if is_private_submodule "$submod"; then
            continue
        fi

        if [[ -L "$WORKTREE_SUBMOD" ]]; then
            echo -e "  $submod: ${GREEN}already symlinked${RESET}"
        elif [[ -d "$WORKTREE_SUBMOD" ]]; then
            echo -e "  $submod: ${YELLOW}replacing with symlink${RESET}"
            rm -rf "$WORKTREE_SUBMOD"
            ln -s "$MAIN_SUBMOD" "$WORKTREE_SUBMOD"
        else
            ln -s "$MAIN_SUBMOD" "$WORKTREE_SUBMOD"
            echo -e "  $submod: ${GREEN}symlinked${RESET}"
        fi
    done

    # Symlink non-submodule items
    for item in "${LIB_ITEMS[@]}"; do
        MAIN_ITEM="$MAIN_TREE/lib/$item"
        WORKTREE_ITEM="$WORKTREE_PATH/lib/$item"

        if [[ -e "$MAIN_ITEM" ]]; then
            if [[ -L "$WORKTREE_ITEM" ]]; then
                echo -e "  lib/$item: ${GREEN}already symlinked${RESET}"
            elif [[ -e "$WORKTREE_ITEM" ]]; then
                rm -rf "$WORKTREE_ITEM"
                ln -s "$MAIN_ITEM" "$WORKTREE_ITEM"
                echo -e "  lib/$item: ${GREEN}symlinked${RESET}"
            else
                ln -s "$MAIN_ITEM" "$WORKTREE_ITEM"
                echo -e "  lib/$item: ${GREEN}symlinked${RESET}"
            fi
        fi
    done

    checkout_private_submodules
}

# Give each private submodule its own working tree at the commit this branch
# points at. The gitdir lands under .git/worktrees/<name>/modules/, so the
# checkout is independent of the main tree's and of every other worktree's, and
# `git submodule status` reports it clean. origin stays the public GitHub remote,
# so committing and pushing from in here works exactly as it does in main.
checkout_private_submodules() {
    local submod
    for submod in "${LIB_PRIVATE_SUBMODULES[@]}"; do
        [[ -e "$MAIN_TREE/$submod/.git" ]] || continue
        if [[ -L "$WORKTREE_PATH/$submod" ]]; then
            rm "$WORKTREE_PATH/$submod"
        fi
        if [[ -e "$WORKTREE_PATH/$submod/.git" ]]; then
            echo -e "  $submod: ${GREEN}already a private checkout${RESET}"
            continue
        fi
        echo -e "  $submod: ${CYAN}private checkout (ours — edited directly, not patched)${RESET}"
        if ! git -C "$WORKTREE_PATH" submodule update --init "$submod" >/dev/null 2>&1; then
            echo -e "  $submod: ${YELLOW}checkout failed${RESET}"
            # Only a partial clone may be swept: anything with a .git returned
            # above, and a directory holding files but no .git is something a
            # person put there, not ours to delete.
            if [[ -d "$WORKTREE_PATH/$submod" ]] \
               && [[ -n "$(ls -A "$WORKTREE_PATH/$submod" 2>/dev/null)" ]]; then
                echo -e "  ${RED}$submod has content but is not a checkout — leaving it alone.${RESET}"
                echo -e "  ${YELLOW}Resolve by hand, then re-run with --setup-only.${RESET}"
                continue
            fi
            echo -e "  ${YELLOW}falling back to a symlink — edits here will land in the MAIN tree's submodule${RESET}"
            rmdir "$WORKTREE_PATH/$submod" 2>/dev/null || true
            ln -s "$MAIN_TREE/$submod" "$WORKTREE_PATH/$submod"
        fi
    done
}

# --unlink / --relink operate on lib/ only and then stop. They must NOT fall
# through to the rest of setup: that re-clones build/obj from the main tree,
# which would discard this worktree's build state mid-merge.
if [[ -n "$LINK_MODE" ]]; then
    if [[ "$LINK_MODE" == "unlink" ]]; then
        unlink_lib_for_git
    else
        link_lib_from_main
        echo -e "${GREEN}✓ lib/ relinked — builds will reuse the main tree again${RESET}"
    fi
    exit 0
fi

link_lib_from_main

# Step 2b: Adopt the main tree's mtimes for byte-identical files
#
# Without this, everything below is nearly worthless. `git worktree add` writes
# every file fresh, so the whole checkout is newer than the artifacts we are
# about to clone, and make rebuilds essentially all of them — twice over:
#
#   - $(PCH) lists include/lvgl_pch.h and lv_conf.h as prerequisites, and EVERY
#     C++ object lists $(PCH), so one fresh header invalidates the entire tree;
#   - the .d files list include/*.h per object, and those are fresh too, so
#     fixing only the PCH would still leave every object out of date.
#
# Measured on a fresh worktree of an up-to-date main tree: 1945 of 1967 cloned
# objects recompiled (~6.5 min) purely because of checkout timestamps.
#
# This is NOT blanket back-dating. A file's mtime is changed only when its
# content is byte-identical to the main tree's file at the same path, and the
# value adopted is that same file's mtime. The invariant is that for matching
# content the (source, object) mtime ordering in the worktree equals the
# ordering in the main tree — so make reaches the same up-to-date decision here
# that it reached there, against objects cloned from there. Anything that
# differs (a branch with real changes, a later edit) keeps its fresh mtime and
# rebuilds normally. See scripts/sync-worktree-mtimes.py.
echo -e "${CYAN}Aligning file timestamps with main tree...${RESET}"
if ! python3 "$MAIN_TREE/scripts/sync-worktree-mtimes.py" \
        --main "$MAIN_TREE" --worktree "$WORKTREE_PATH"; then
    echo -e "  ${YELLOW}mtime sync failed — build will be correct but slow${RESET}"
fi

# Step 3: Create build directory structure and clone object files
echo -e "${CYAN}Setting up build directory...${RESET}"
mkdir -p build/lib build/obj build/bin

# Step 3b: Hardlink object files from main tree
# This is the big win — avoids recompiling 900+ .o files from scratch.
# Hardlinks are instant, zero disk cost (same filesystem), and make will
# only recompile files whose sources diverge in the worktree.
MAIN_OBJ="$MAIN_TREE/build/obj"
WORKTREE_OBJ="$WORKTREE_PATH/build/obj"
if [[ -d "$MAIN_OBJ" ]]; then
    # Check if obj/ already has content (re-run of setup)
    OBJ_COUNT=$(find "$WORKTREE_OBJ" -name "*.o" 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$OBJ_COUNT" -gt 10 ]]; then
        echo -e "  build/obj: ${GREEN}already populated ($OBJ_COUNT objects)${RESET}"
    else
        echo -e "${CYAN}Cloning build objects from main tree...${RESET}"
        # Clone all build artifacts (.o, .d, .ccj) from main tree.
        # On macOS/APFS: cp -Rc uses clonefile() — instant, zero disk until modified,
        #   and each side is independent (no risk of worktree clobbering main tree).
        # On Linux: falls back to regular copy (still faster than recompiling).
        # Either way, make only recompiles files whose sources diverge in the worktree.
        CLONE_START=$(date +%s)
        rm -rf "$WORKTREE_OBJ"
        cp -Rc "$MAIN_OBJ" "$WORKTREE_OBJ" 2>/dev/null || cp -a "$MAIN_OBJ" "$WORKTREE_OBJ"
        CLONE_END=$(date +%s)
        NEW_COUNT=$(find "$WORKTREE_OBJ" -name "*.o" 2>/dev/null | wc -l | tr -d ' ')
        echo -e "  build/obj: ${GREEN}cloned $NEW_COUNT objects in $((CLONE_END - CLONE_START))s${RESET}"

        # Drop objects whose source is uncommitted in the main tree.
        #
        # A cloned object was built from the main tree's WORKING copy. The
        # worktree checks out the COMMITTED version, so for any path the main
        # tree reports dirty the two disagree and the object describes code that
        # is not in this worktree. The mtime sync leaves such a file fresh, which
        # is usually enough to force a rebuild, but it is not enough on its own:
        # the compiler cache can still answer the fresh compile with an object
        # built elsewhere (base_dir collapses the worktree path, and
        # `sloppiness = pch_defines` lets a differing PCH through). The failure is
        # silent and lands at link time as an undefined reference, pointing at
        # whichever worktree compiled first.
        #
        # Deleting the object removes the choice: that TU compiles here, now.
        DIRTY_OBJS=0
        while read -r src_rel; do
            [[ -n "$src_rel" ]] || continue
            case "$src_rel" in src/*.cpp | src/*.c) ;; *) continue ;; esac
            obj_rel="${src_rel#src/}"
            obj_rel="${obj_rel%.*}.o"
            if [[ -f "$WORKTREE_OBJ/$obj_rel" ]]; then
                rm -f "$WORKTREE_OBJ/$obj_rel"
                DIRTY_OBJS=$((DIRTY_OBJS + 1))
            fi
        done < <(git -C "$MAIN_TREE" status --porcelain -- 'src/*.cpp' 'src/*.c' 2>/dev/null | sed 's/^...//')
        if [[ "$DIRTY_OBJS" -gt 0 ]]; then
            echo -e "  build/obj: ${YELLOW}dropped $DIRTY_OBJS object(s) whose source is uncommitted in the main tree${RESET}"
        fi

        # Validate object file architecture — cross-compilation leaves wrong-arch .o files
        SAMPLE_OBJ=$(find "$WORKTREE_OBJ" -name "*.o" -print -quit 2>/dev/null)
        if [[ -n "$SAMPLE_OBJ" ]]; then
            # Probe with `file`, not `objdump -f`. Apple's objdump cannot emit the
            # GNU "architecture:" line for Mach-O at all, so under `set -euo
            # pipefail` the grep found nothing, the pipeline exited 1, and the
            # whole setup aborted HERE — leaving the worktree with cloned objects
            # but no PCH, no build markers, no git excludes and no initial build.
            # (Re-running the script "fixed" it only because the second run skips
            # this branch entirely.) `file -b` answers on both toolchains:
            #   macOS: "Mach-O 64-bit object arm64"
            #   Linux: "ELF 64-bit LSB relocatable, x86-64, ..."
            SAMPLE_ARCH=$(file -b "$SAMPLE_OBJ" 2>/dev/null || true)
            HOST_ARCH_CHECK=$(uname -m)
            # macOS reports Apple Silicon as arm64, Linux as aarch64. Without this
            # the aarch64 branch below never matched on a Mac and the check was dead.
            [[ "$HOST_ARCH_CHECK" == "arm64" ]] && HOST_ARCH_CHECK=aarch64
            ARCH_MISMATCH=false
            # Only a POSITIVE identification of the wrong architecture may set this.
            # An empty or unrecognized probe must fall through untouched: this flag
            # gates an `rm -rf` of the object cache, so failing open costs one slow
            # build while failing closed silently deletes a good cache on every
            # setup. That is exactly what a bare `|| true` on the old objdump
            # pipeline would have done on an Intel Mac — empty SAMPLE_ARCH matched
            # neither "x86-64" nor "i386", so both negated tests passed.
            case "$HOST_ARCH_CHECK" in
                x86_64)
                    if [[ "$SAMPLE_ARCH" == *arm64* || "$SAMPLE_ARCH" == *aarch64* ]]; then
                        ARCH_MISMATCH=true
                    fi
                    ;;
                aarch64)
                    if [[ "$SAMPLE_ARCH" == *x86-64* || "$SAMPLE_ARCH" == *x86_64* ||
                          "$SAMPLE_ARCH" == *i386* ]]; then
                        ARCH_MISMATCH=true
                    fi
                    ;;
            esac
            if [[ "$ARCH_MISMATCH" == "true" ]]; then
                echo -e "  build/obj: ${YELLOW}wrong architecture ($SAMPLE_ARCH for $HOST_ARCH_CHECK), clearing — will rebuild from scratch${RESET}"
                rm -rf "$WORKTREE_OBJ"
                mkdir -p "$WORKTREE_OBJ"
            fi
        fi
    fi
else
    echo -e "  build/obj: ${YELLOW}main tree not built yet (will build from scratch)${RESET}"
fi

# Step 3b2: Clone generated headers (build/generated/contributors.h)
# Small, but a missing one puts the objects that include it — and therefore the
# link — back on the critical path of an otherwise no-op build.
MAIN_GEN="$MAIN_TREE/build/generated"
if [[ -d "$MAIN_GEN" && ! -d "$WORKTREE_PATH/build/generated" ]]; then
    cp -Rc "$MAIN_GEN" "$WORKTREE_PATH/build/generated" 2>/dev/null \
        || cp -a "$MAIN_GEN" "$WORKTREE_PATH/build/generated"
    echo -e "  build/generated: ${GREEN}cloned${RESET}"
fi

# Step 3c: Copy compile_commands.json for clangd support
if [[ -f "$MAIN_TREE/compile_commands.json" ]]; then
    # Use sed to rewrite paths from main tree to worktree
    sed "s|${MAIN_TREE}|${WORKTREE_PATH}|g" "$MAIN_TREE/compile_commands.json" > "$WORKTREE_PATH/compile_commands.json"
    echo -e "  compile_commands.json: ${GREEN}copied and paths rewritten${RESET}"
fi

# Step 4: Clone compiled libraries from the main tree
#
# Copies, not symlinks, for the same reason as the PCH below: these are build
# OUTPUTS. `make libhv-build` ends by copying the freshly-ar'd archive to
# build/lib/libhv.a, and cp follows a symlink — so a worktree that rebuilds
# libhv writes into the MAIN TREE's build/lib. Observed: a single fresh-worktree
# build moved the main tree's libhv.a mtime forward by five days, which left the
# main tree's own PCH older than it and put ~1900 objects back on the main
# tree's next build. One worktree setup, and the main tree rebuilds the world.
#
# The old `touch -h` here was also load-bearing in the wrong direction: it
# stamped the archive `now` to look "newer than source files", which is exactly
# the kind of invented timestamp that hides real work.
echo -e "${CYAN}Cloning compiled libraries from main tree...${RESET}"

MAIN_LIBS=("libhv.a" "libwpa_client.a")
for lib in "${MAIN_LIBS[@]}"; do
    MAIN_LIB="$MAIN_TREE/build/lib/$lib"
    WORKTREE_LIB="$WORKTREE_PATH/build/lib/$lib"

    if [[ -f "$MAIN_LIB" ]]; then
        if [[ -L "$WORKTREE_LIB" ]]; then
            rm -f "$WORKTREE_LIB"
            clone_file "$MAIN_LIB" "$WORKTREE_LIB"
            echo -e "  $lib: ${YELLOW}was a symlink into the main tree, replaced with a private clone${RESET}"
        elif [[ -f "$WORKTREE_LIB" ]]; then
            echo -e "  $lib: ${GREEN}already present (worktree-local)${RESET}"
        else
            clone_file "$MAIN_LIB" "$WORKTREE_LIB"
            echo -e "  $lib: ${GREEN}cloned${RESET}"
        fi
    else
        echo -e "  $lib: ${YELLOW}not found in main tree (will build from scratch)${RESET}"
    fi
done

# Step 5: Clone the precompiled header if it exists
#
# Deliberately a COPY, not a symlink. The PCH is a build OUTPUT: make rebuilds it
# whenever lv_conf.h / include/lvgl_pch.h / the patch stamp move, and clang opens
# the output path with O_CREAT|O_TRUNC, which follows a symlink. A symlinked PCH
# therefore lets a worktree write its own PCH straight into the main tree — and a
# worktree that changed lv_conf.h would leave the main tree, and every other
# worktree sharing that symlink, linking against a PCH built for someone else's
# feature flags. On APFS `cp -c` is a clonefile: instant, zero disk until one
# side diverges, and independent. mtime is preserved so make still sees it as
# up to date relative to the (now mtime-synced) prerequisites.
MAIN_PCH="$MAIN_TREE/build/lvgl_pch.h.gch"
WORKTREE_PCH="$WORKTREE_PATH/build/lvgl_pch.h.gch"
if [[ -f "$MAIN_PCH" ]]; then
    if [[ -L "$WORKTREE_PCH" ]]; then
        # Legacy worktree from an older setup run — swap the symlink for a clone
        # before a build can write through it.
        rm -f "$WORKTREE_PCH"
        clone_file "$MAIN_PCH" "$WORKTREE_PCH"
        echo -e "  lvgl_pch.h.gch: ${YELLOW}was a symlink into the main tree, replaced with a private clone${RESET}"
    elif [[ -f "$WORKTREE_PCH" ]]; then
        # This worktree already has its own PCH. It may have been built here from
        # locally-modified prerequisites, so leave it alone and let make decide.
        echo -e "  lvgl_pch.h.gch: ${GREEN}already present (worktree-local)${RESET}"
    else
        clone_file "$MAIN_PCH" "$WORKTREE_PCH"
        echo -e "  lvgl_pch.h.gch: ${GREEN}cloned${RESET}"
    fi
else
    echo -e "  lvgl_pch.h.gch: ${YELLOW}not found in main tree (will build from scratch)${RESET}"
fi

# Step 5b: Validate library architectures
# Cross-compilation (make pi-test) can leave ARM .a files in build/lib/.
# Detect and remove them so make rebuilds for the correct architecture.
echo -e "${CYAN}Validating library architectures...${RESET}"
HOST_ARCH=$(uname -m)
for lib_file in "$WORKTREE_PATH/build/lib/"*.a; do
    [[ -f "$lib_file" ]] || continue
    [[ -L "$lib_file" ]] && continue  # Skip symlinks — they point to main tree
    LIB_NAME=$(basename "$lib_file")
    # Check first object file's architecture
    # `|| true`: see the objdump note in step 3b — a probe that cannot read the
    # file must yield "unknown", not abort the script under `set -e`.
    LIB_ARCH=$(objdump -f "$lib_file" 2>/dev/null | grep -m1 "architecture:" | awk -F',' '{print $1}' | awk '{print $NF}' || true)
    if [[ -n "$LIB_ARCH" ]]; then
        case "$HOST_ARCH" in
            x86_64)
                if [[ "$LIB_ARCH" != *"x86-64"* && "$LIB_ARCH" != *"i386"* ]]; then
                    echo -e "  $LIB_NAME: ${YELLOW}wrong architecture ($LIB_ARCH), removing — will rebuild${RESET}"
                    rm -f "$lib_file"
                fi
                ;;
            aarch64)
                if [[ "$LIB_ARCH" != *"aarch64"* ]]; then
                    echo -e "  $LIB_NAME: ${YELLOW}wrong architecture ($LIB_ARCH), removing — will rebuild${RESET}"
                    rm -f "$lib_file"
                fi
                ;;
        esac
    fi
done

echo -e "${GREEN}✓ Libraries configured${RESET}"

# Step 6: Symlink node_modules (font converter tools)
echo -e "${CYAN}Symlinking node_modules...${RESET}"
if [[ -d "$MAIN_TREE/node_modules" ]]; then
    if [[ -L "$WORKTREE_PATH/node_modules" ]]; then
        echo -e "  node_modules: ${GREEN}already symlinked${RESET}"
    elif [[ -d "$WORKTREE_PATH/node_modules" ]]; then
        echo -e "  node_modules: ${YELLOW}exists as real directory, replacing with symlink${RESET}"
        rm -rf "$WORKTREE_PATH/node_modules"
        ln -s "$MAIN_TREE/node_modules" "$WORKTREE_PATH/node_modules"
    else
        ln -s "$MAIN_TREE/node_modules" "$WORKTREE_PATH/node_modules"
        echo -e "  node_modules: ${GREEN}symlinked${RESET}"
    fi
else
    echo -e "  node_modules: ${YELLOW}not found in main tree${RESET}"
fi

# Step 7: Symlink Python venv
echo -e "${CYAN}Symlinking Python venv...${RESET}"
if [[ -d "$MAIN_TREE/.venv" ]]; then
    if [[ -L "$WORKTREE_PATH/.venv" ]]; then
        echo -e "  .venv: ${GREEN}already symlinked${RESET}"
    elif [[ -d "$WORKTREE_PATH/.venv" ]]; then
        echo -e "  .venv: ${YELLOW}exists as real directory, replacing with symlink${RESET}"
        rm -rf "$WORKTREE_PATH/.venv"
        ln -s "$MAIN_TREE/.venv" "$WORKTREE_PATH/.venv"
    else
        ln -s "$MAIN_TREE/.venv" "$WORKTREE_PATH/.venv"
        echo -e "  .venv: ${GREEN}symlinked${RESET}"
    fi
else
    echo -e "  .venv: ${YELLOW}not found in main tree${RESET}"
fi
echo -e "${GREEN}✓ Development tools configured${RESET}"

# Step 8: Configure git excludes for symlinks (keeps git status clean)
echo -e "${CYAN}Configuring git excludes...${RESET}"

# Get the common git dir (shared by all worktrees)
# Note: info/exclude is read from GIT_COMMON_DIR, not the worktree's gitdir
GIT_COMMON_DIR=$(git -C "$WORKTREE_PATH" rev-parse --git-common-dir)
# Make it absolute if it's relative
if [[ ! "$GIT_COMMON_DIR" = /* ]]; then
    GIT_COMMON_DIR="$WORKTREE_PATH/$GIT_COMMON_DIR"
fi

EXCLUDE_FILE="$GIT_COMMON_DIR/info/exclude"
mkdir -p "$(dirname "$EXCLUDE_FILE")"

# Items to exclude (symlinks we created + build artifacts)
# Note: We exclude lib/* specifically because lib/ itself is a real directory
EXCLUDES=(
    "# HelixScreen worktree setup - auto-generated excludes"
    "lib/*"
    "node_modules"
    ".venv"
    "build/"
    "compile_commands.json"
    ".fonts.stamp"
)

# Add excludes if not already present
for exclude in "${EXCLUDES[@]}"; do
    if ! grep -qF "$exclude" "$EXCLUDE_FILE" 2>/dev/null; then
        echo "$exclude" >> "$EXCLUDE_FILE"
    fi
done

# Mark symlinked paths as skip-worktree so git ignores typechanges
echo -e "${CYAN}Marking symlinks as skip-worktree...${RESET}"
cd "$WORKTREE_PATH"

# Mark all lib/ submodules
for submod in $SUBMODULES; do
    git update-index --skip-worktree "$submod" 2>/dev/null || true
done

# Mark other lib items
git update-index --skip-worktree lib/tuibox.h 2>/dev/null || true
# lib/mdns is a directory, not a submodule - mark its contents
git update-index --skip-worktree lib/mdns/mdns.h 2>/dev/null || true

echo -e "${GREEN}✓ Git excludes configured${RESET}"

# Step 9: Create build marker files to skip redundant checks
#
# .patches-applied and .fonts.stamp are prerequisites, not just markers, so a
# fresh `now` timestamp on them is not free:
#   - $(PATCHES_STAMP) is a prerequisite of $(PCH), every LVGL/helix-xml/font
#     object and libhv.a — stamping it `now` invalidates all of them;
#   - .fonts.stamp is a prerequisite of assets/fonts/*.c, which have no recipe,
#     so make marks them updated and recompiles all 46 font objects.
# Adopt the main tree's timestamps when it has them, for the same reason the
# source mtimes are synced above: reproduce the main tree's state rather than
# invent a newer one. .deps-checked is only ever compared against
# scripts/check-deps.sh, so `now` is both correct and what we want there.
echo -e "${CYAN}Creating build markers...${RESET}"
touch "$WORKTREE_PATH/build/.deps-checked"
copy_marker_mtime() {
    # $1 = path relative to tree root
    local main_marker="$MAIN_TREE/$1" wt_marker="$WORKTREE_PATH/$1"
    touch "$wt_marker"
    if [[ -f "$main_marker" ]]; then
        touch -r "$main_marker" "$wt_marker"
    fi
}
copy_marker_mtime "build/.patches-applied"
copy_marker_mtime ".fonts.stamp"
echo "native" > "$WORKTREE_PATH/build/.build-target"
echo -e "${GREEN}✓ Build markers created${RESET}"

# Step 9c: Seed runtime config from the main tree
#
# config/settings.json is gitignored, so a fresh worktree has none and the app
# boots straight into the first-run wizard — every ctl navigate/click then lands
# on a screen that isn't there. Copy (never symlink) the main tree's settings so
# the worktree starts on the home panel: a symlink would let a worktree run
# mutate the main tree's config, and worktrees exist to be disposable.
echo -e "${CYAN}Seeding runtime config...${RESET}"
for cfg in settings.json printer_database.json; do
    MAIN_CFG="$MAIN_TREE/config/$cfg"
    WORKTREE_CFG="$WORKTREE_PATH/config/$cfg"
    if [[ ! -f "$MAIN_CFG" ]]; then
        echo -e "  $cfg: ${YELLOW}not in main tree (skipping)${RESET}"
    elif [[ -e "$WORKTREE_CFG" ]]; then
        echo -e "  $cfg: ${GREEN}already present${RESET}"
    else
        mkdir -p "$WORKTREE_PATH/config"
        cp "$MAIN_CFG" "$WORKTREE_CFG"
        echo -e "  $cfg: ${GREEN}copied from main tree${RESET}"
    fi
done

# Step 9b: Configure ccache for cross-worktree reuse
#
# The native build compiles with -g (debug info). With ccache's default
# hash_dir=true, the absolute working directory is folded into the cache key,
# so an object compiled in the main tree NEVER matches the same source compiled
# under .worktrees/<name>/ — every worktree starts cold and recompiles from
# scratch. Two settings fix this:
#   - base_dir: rewrite absolute paths under it to relative before hashing
#   - hash_dir=false: stop hashing the cwd (the -g debug-path component)
# With both, a worktree build reuses the main tree's cached objects, so the
# "rebuild everything" make does after a fresh checkout becomes cache hits
# instead of real compiles.
CCACHE_BIN="$(command -v ccache 2>/dev/null || true)"
if [[ -n "$CCACHE_BIN" ]]; then
    echo -e "${CYAN}Configuring ccache for cross-worktree reuse...${RESET}"

    # Longest common ancestor of the main tree and this worktree — covers both
    # the default .worktrees/<name> layout and out-of-tree paths like /tmp/foo.
    common_ancestor() {
        local p1="$1" p2="$2"
        while [[ "$p1" != "$p2" ]]; do
            if [[ ${#p1} -gt ${#p2} ]]; then p1="$(dirname "$p1")"; else p2="$(dirname "$p2")"; fi
        done
        echo "$p1"
    }
    BUILD_BASEDIR="$(common_ancestor "$MAIN_TREE" "$WORKTREE_PATH")"

    # Persist global ccache config so later manual `make` runs in the worktree
    # hit the shared cache too — not just this script's initial build. base_dir
    # defaults to $HOME (the standard broad choice covering all in-home worktrees);
    # only set when unset so we never override an intentional user value.
    CUR_BASEDIR="$(ccache --get-config base_dir 2>/dev/null || true)"
    if [[ -z "$CUR_BASEDIR" ]]; then
        ccache --set-config "base_dir=$HOME" 2>/dev/null \
            && echo -e "  base_dir: ${GREEN}set to $HOME${RESET}"
    else
        echo -e "  base_dir: ${GREEN}already set ($CUR_BASEDIR)${RESET}"
    fi
    if [[ "$(ccache --get-config hash_dir 2>/dev/null || true)" == "true" ]]; then
        ccache --set-config hash_dir=false 2>/dev/null \
            && echo -e "  hash_dir: ${GREEN}disabled (paths no longer in cache key)${RESET}"
    else
        echo -e "  hash_dir: ${GREEN}already disabled${RESET}"
    fi

    # WITHOUT THIS, ccache CACHES NOTHING. Every native build compiles with
    # -include $(PCH), and ccache refuses to cache any compilation using a
    # precompiled header unless sloppiness allows it. Measured before/after on
    # a single -include compile: "Uncacheable calls: 1/1 (100%)" -> "Cacheable
    # calls: 1/1 (100%)", with the repeat compile hitting. base_dir and hash_dir
    # were set here for a long time while the cache stayed empty for this reason.
    #
    # BOTH flags are required — pch_defines alone still measured 0% cacheable.
    #
    # The cost of time_macros is that ccache stops hashing __DATE__/__TIME__.
    # Exactly one site uses them: ui_settings_about.cpp reads __DATE__ + 7 for
    # the About screen's copyright year, so across a New Year that screen can
    # show the previous year until the file is next recompiled. Cosmetic, and
    # the only such site in src/, include/, lib/helix-xml/ or the build flags.
    CUR_SLOPPY="$(ccache --get-config sloppiness 2>/dev/null || true)"
    if [[ "$CUR_SLOPPY" != *pch_defines* || "$CUR_SLOPPY" != *time_macros* ]]; then
        ccache --set-config sloppiness=pch_defines,time_macros 2>/dev/null \
            && echo -e "  sloppiness: ${GREEN}pch_defines,time_macros (PCH builds are now cacheable)${RESET}"
    else
        echo -e "  sloppiness: ${GREEN}already allows PCH caching ($CUR_SLOPPY)${RESET}"
    fi

    # The shared cache thrashes hard once a couple of worktrees + cross-compiles
    # pile in (default 5 GiB fills and evicts constantly, re-causing cold misses).
    # Raise the ceiling so objects survive between builds. Only ever raise it.
    CUR_MAX="$(ccache --get-config max_size 2>/dev/null || true)"
    MAX_NUM="$(echo "$CUR_MAX" | grep -oE '[0-9]+' | head -1)"
    MAX_UNIT="$(echo "$CUR_MAX" | grep -oiE '[KMGT]i?B' | head -1)"
    if [[ "$MAX_UNIT" =~ ^[Kk] || "$MAX_UNIT" =~ ^[Mm] ]] || \
       { [[ "$MAX_UNIT" =~ ^[Gg] ]] && [[ -n "$MAX_NUM" ]] && [[ "$MAX_NUM" -lt 25 ]]; }; then
        ccache --max-size=25G >/dev/null 2>&1 \
            && echo -e "  max_size: ${GREEN}raised to 25G (was $CUR_MAX)${RESET}"
    else
        echo -e "  max_size: ${GREEN}already ample ($CUR_MAX)${RESET}"
    fi

    # Export for this script's own build below. base_dir must be a prefix of the
    # worktree path; use the precise common ancestor in case the worktree lives
    # outside $HOME (e.g. /tmp/foo), which the global $HOME base_dir wouldn't cover.
    export CCACHE_BASEDIR="$BUILD_BASEDIR"
    export CCACHE_NOHASHDIR=1
    export CCACHE_SLOPPINESS=pch_defines,time_macros
    echo -e "  this build: ${GREEN}CCACHE_BASEDIR=$BUILD_BASEDIR CCACHE_NOHASHDIR=1${RESET}"
    echo -e "              ${GREEN}CCACHE_SLOPPINESS=pch_defines,time_macros${RESET}"
else
    # Loud on purpose. The mtime sync keeps an UNCHANGED worktree fast on its
    # own, which makes a missing ccache easy to not notice — right up until the
    # first build that genuinely has to recompile, which then costs minutes
    # instead of seconds. The old one-line yellow note got lost in the setup
    # output and the build-system doc even credited ccache for a speedup it was
    # not delivering, because it was never installed here.
    echo ""
    echo -e "${RED}${BOLD}================================================================${RESET}"
    echo -e "${RED}${BOLD}  ccache is NOT installed — recompiles will be SLOW${RESET}"
    echo -e "${RED}${BOLD}================================================================${RESET}"
    echo -e "${YELLOW}A clean worktree still builds fast (timestamps are aligned above)."
    echo -e "But any build that has to recompile — you edit lv_conf.h, or another"
    echo -e "tree rebuilds libhv and re-invalidates the shared PCH — pays full"
    echo -e "price with no cache to fall back on: ~400s instead of ~10s.${RESET}"
    echo ""
    case "$(uname -s)" in
        Darwin) echo -e "  Install:  ${CYAN}${BOLD}brew install ccache${RESET}" ;;
        Linux)
            echo -e "  Install:  ${CYAN}${BOLD}sudo apt install ccache${RESET}"
            echo -e "            ${CYAN}sudo dnf install ccache${RESET}  /  ${CYAN}sudo pacman -S ccache${RESET}"
            ;;
        *) echo -e "  Install ccache with your platform's package manager." ;;
    esac
    echo -e "  Then re-run: ${CYAN}${BOLD}$0 --setup-only ${BRANCH:-<branch>}${RESET}"
    echo -e "  (ccache is optional — the worktree works fine without it.)"
    echo ""
fi

# Step 10: Build (optional)
if [[ "$NO_BUILD" == "false" ]]; then
    echo ""
    echo -e "${BOLD}${CYAN}Running initial build...${RESET}"
    cd "$WORKTREE_PATH"
    if make -j; then
        echo ""
        echo -e "${GREEN}${BOLD}✓ Build successful!${RESET}"
    else
        echo ""
        echo -e "${RED}${BOLD}✗ Build failed${RESET}"
        exit 1
    fi
fi

echo ""
echo -e "${GREEN}${BOLD}✓ Worktree setup complete!${RESET}"
echo ""
echo -e "To work in this worktree:"
echo -e "  ${CYAN}cd $WORKTREE_PATH${RESET}"
echo ""
echo -e "Git status should be clean. To verify:"
echo -e "  ${CYAN}cd $WORKTREE_PATH && git status${RESET}"
echo ""
# Running the app from a worktree collides with the main tree on two fixed
# per-user paths: the ctl socket and the config-dir flock. Hand over both
# overrides so parallel sessions don't drive each other's instance.
WT_NAME="$(basename "$WORKTREE_PATH")"
echo -e "${BOLD}To run and drive the app from this worktree${RESET} (both are required —"
echo -e "a bare ${CYAN}helix-screen ctl${RESET} drives whichever instance started first):"
echo -e "  ${CYAN}export HELIX_SOCK=/tmp/helix-${WT_NAME}.sock${RESET}"
echo -e "  ${CYAN}export HELIX_CONFIG_DIR=/tmp/helix-config-${WT_NAME}${RESET}"
echo -e "  ${CYAN}mkdir -p \"\$HELIX_CONFIG_DIR\"${RESET}  # required — the app aborts if it is missing"
echo -e "  ${CYAN}./build/bin/helix-screen --test -vv --remote-socket \"\$HELIX_SOCK\" &${RESET}"
echo -e "  ${CYAN}./build/bin/helix-screen ctl -s \"\$HELIX_SOCK\" navigate settings${RESET}"
echo -e "See ${CYAN}docs/devel/HELIXCTL.md${RESET} § \"Running a fully isolated second instance\"."
echo ""
echo -e "${YELLOW}Note: lib/ is symlinked from main tree. If you need to modify"
echo -e "library code, un-symlink that specific directory first.${RESET}"
