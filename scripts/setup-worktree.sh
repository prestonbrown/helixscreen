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
    echo "  --unlink        Replace the remaining lib/ symlinks with what git expects,"
    echo "                  so git status/merge/rebase/stash work in this worktree."
    echo "                  Private checkouts (lvgl, libhv, helix-xml) are untouched."
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
    echo "  - Symlinks the unpatched lib/ submodules from the main tree (sources +"
    echo "    generated headers), and gives lvgl/libhv/helix-xml a PRIVATE checkout"
    echo "    copied from it, so this branch's patches/ stay inside this worktree"
    echo "  - Clones compiled libraries (libhv.a) and the PCH — copies, not symlinks,"
    echo "    so a rebuild here can never write back into the main tree"
    echo "  - Copies compile_commands.json with rewritten paths for clangd"
    echo "  - Symlinks node_modules and .venv for font/python tools"
    echo "  - Uses .git/info/exclude for clean git status"
}

# --- lib/ link management ---------------------------------------------------
# A worktree shares the main tree's checkout of every submodule NOT listed in
# LIB_PRIVATE_SUBMODULES, and their IN-TREE build artifacts, by symlinking each
# lib/ entry. That sharing is the whole point: it is why a fresh worktree builds
# in seconds instead of recompiling every submodule from cold. It is safe for
# exactly the submodules no tree rewrites — nothing patches or edits these.
#
# The cost is that git refuses to scan a tree where a gitlink path is a symlink:
#   error: expected submodule path 'lib/cpp-terminal' not to be a symbolic link
# which aborts `git status`, `merge`, `rebase` and `stash` outright.
#
# So: --unlink before a merge/rebase, --relink after. Relinking is NOT optional;
# the empty submodule dirs left by --unlink have no headers, so the build fails
# with a missing-header error until the symlinks are back. Private checkouts are
# untouched by both — they are what git expects and never blocked a scan.
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
#
# lib/lvgl and lib/libhv are here for a second reason: they are the two
# submodules patches/ rewrites, and patches/ is per-branch. One shared checkout
# cannot satisfy two branches carrying different patch sets — each tree's
# `make reapply-patches` redefines what every other tree compiles, and each
# correct action invalidates the other. A private checkout per worktree is what
# makes the patch set a property of the branch again
# (prestonbrown/helixscreen#1471).
#
# A real checkout is also what git expects, so these need no --unlink/--relink
# dance; the submodules still symlinked below do.
LIB_PRIVATE_SUBMODULES=("lib/helix-xml" "lib/lvgl" "lib/libhv")

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

# clone_file for a whole directory. Every fallback preserves mtimes: GNU cp -R
# does not on its own, so the reflink form has to carry -a as well.
clone_tree() {
    local src="$1" dst="$2"
    cp -Rc "$src" "$dst" 2>/dev/null \
        || cp -a --reflink=auto "$src" "$dst" 2>/dev/null \
        || cp -a "$src" "$dst"
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

# SCRIPT_DIR/.. is only the main tree when this copy of the script is the main
# tree's copy. Every worktree has its own scripts/, so running a worktree's copy
# leaves MAIN_TREE pointing at that worktree — and the lib/ step below then
# rm -rf's each real submodule and symlinks it to the path it just deleted.
# git-common-dir names the real main tree from anywhere in the repo. It can come
# back relative (plain ".git" from a main-tree root), so resolve it against
# MAIN_TREE before use.
GIT_COMMON_DIR="$(git -C "$MAIN_TREE" rev-parse --git-common-dir 2>/dev/null || true)"
if [[ -n "$GIT_COMMON_DIR" ]]; then
    [[ "$GIT_COMMON_DIR" != /* ]] && GIT_COMMON_DIR="$MAIN_TREE/$GIT_COMMON_DIR"
    RESOLVED_MAIN=""
    if cd "$GIT_COMMON_DIR/.." 2>/dev/null; then
        RESOLVED_MAIN="$(pwd -P)"
        cd "$SCRIPT_DIR" || exit 1
    fi
    if [[ -n "$RESOLVED_MAIN" && "$RESOLVED_MAIN" != "$MAIN_TREE" ]]; then
        echo -e "${YELLOW}Running a worktree's copy of this script.${RESET}"
        echo -e "${YELLOW}Main tree is $RESOLVED_MAIN, not $MAIN_TREE.${RESET}"
        MAIN_TREE="$RESOLVED_MAIN"
    fi
fi

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

# Guard: the worktree must never be the main tree. The lib/ step rm -rf's each
# real submodule and replaces it with a symlink into MAIN_TREE, so if the two
# resolve to one path it deletes the content and links each entry to itself.
# Both are resolved with -P so a symlinked path cannot slip past the compare.
if [[ -e "$WORKTREE_PATH" ]] \
   && [[ "$(cd "$MAIN_TREE" && pwd -P)" == "$(cd "$WORKTREE_PATH" && pwd -P)" ]]; then
    echo -e "${RED}Error: the worktree path and the main tree are the same directory:${RESET}"
    echo -e "  $(cd "$MAIN_TREE" && pwd -P)"
    echo -e "Setting up a worktree on top of itself would delete lib/ and replace"
    echo -e "each entry with a symlink to the path it just removed."
    echo -e "Run this from the main tree naming a branch, or from inside a real"
    echo -e "worktree with ${CYAN}--setup-only${RESET} and no branch argument."
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
    # Also include non-submodule files in lib/ (LIB_NON_SUBMODULE_ITEMS, above)

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
    for item in "${LIB_NON_SUBMODULE_ITEMS[@]}"; do
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
    warn_if_docker_mount_probes_lvgl
}

# mk/cross.mk bind-mounts the directory the symlinked lib/ entries point into, so
# a Docker cross-build resolves them the same way inside the container. A tree
# that works out whether it is a worktree by asking where lib/lvgl really lives
# gets the wrong answer once lvgl is a private checkout under this worktree: it
# concludes there is nothing to mount, and the entries that ARE still symlinks
# dangle. Native builds are unaffected.
warn_if_docker_mount_probes_lvgl() {
    local cross="$WORKTREE_PATH/mk/cross.mk"
    [[ -f "$cross" ]] || return 0
    grep -q 'realpath lib/lvgl' "$cross" || return 0
    echo -e "  ${YELLOW}mk/cross.mk on this branch detects a worktree by probing lib/lvgl,${RESET}"
    echo -e "  ${YELLOW}which is now a private checkout here — Docker cross-builds from this${RESET}"
    echo -e "  ${YELLOW}worktree will not mount the main tree's lib/. Merge the branch that${RESET}"
    echo -e "  ${YELLOW}generalizes that detection, or cross-build from the main tree.${RESET}"
}

# The patch-drift stamp check_patch_drift.py keeps in a submodule's git dir. It
# records which revision of each patch went in, so it belongs to the checkout it
# describes and has to travel with a copied one.
PATCH_DRIFT_STAMP="helix-patches-applied.json"

# Set when a private submodule ends up somewhere the main tree's patches do not
# describe, so the patch reconcile below knows it has real work to do.
PRIVATE_SUBMODULES_NEED_PATCHES=false

# Git names a submodule's git dir after its .gitmodules SECTION, which is not
# always its path: lib/lvgl is section "lvgl", so its git dir is modules/lvgl
# while lib/helix-xml's is modules/lib/helix-xml.
submodule_section_name() {
    local want="$1"
    git -C "$MAIN_TREE" config --file .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null \
        | awk -v p="$want" '$2 == p { n = $1; sub(/^submodule\./, "", n); sub(/\.path$/, "", n); print n; exit }'
}

submodule_origin_url() {
    git -C "$MAIN_TREE" config --file .gitmodules --get "submodule.$1.url" 2>/dev/null || true
}

# True when a directory holds anything besides its own .git.
has_worktree_content() {
    local dir="$1" entry
    for entry in "$dir"/* "$dir"/.[!.]*; do
        [[ -e "$entry" || -L "$entry" ]] || continue
        [[ "$(basename "$entry")" == ".git" ]] && continue
        return 0
    done
    return 1
}

# Materialize one private submodule by COPYING the main tree's checkout rather
# than checking it out fresh. A fresh checkout writes fresh mtimes, and every
# object in the cloned build/obj/ that was compiled against those headers is
# then out of date — a new worktree would pay a full cold rebuild for a
# submodule whose content it already has. The copy-on-write primitive keeps the
# mtimes, so those objects stay valid.
#
# Three pieces have to be separated for this to be a private checkout rather
# than a second view of a shared one:
#   - the FILES are copied, so this tree's patches only ever rewrite its own;
#   - the GIT DIR is a local clone, which hardlinks the object store: no network,
#     no second copy of 500 MB of packs, and independent refs, HEAD and index;
#   - the `.git` file is written fresh. Copying the source's would name the
#     source's git dir, which is precisely the sharing being removed here.
# Returns non-zero when there is nothing to copy from, leaving the caller to
# fall back to a fresh clone.
materialize_private_submodule() {
    local submod="$1"
    local src="$MAIN_TREE/$submod" dst="$WORKTREE_PATH/$submod"
    local name url src_gitdir wt_gitdir pinned src_head entry base

    src_gitdir="$(git -C "$src" rev-parse --absolute-git-dir 2>/dev/null || true)"
    src_head="$(git -C "$src" rev-parse --verify --quiet HEAD 2>/dev/null || true)"
    [[ -n "$src_gitdir" && -n "$src_head" && -d "$src_gitdir" ]] || return 1

    name="$(submodule_section_name "$submod")"
    [[ -n "$name" ]] || name="$submod"
    wt_gitdir="$WORKTREE_GIT_DIR/modules/$name"
    pinned="$(git -C "$WORKTREE_PATH" rev-parse --verify --quiet "HEAD:$submod" 2>/dev/null || true)"

    rm -rf "$wt_gitdir"
    mkdir -p "$(dirname "$wt_gitdir")" "$(dirname "$dst")"
    git clone --quiet --local --no-checkout --separate-git-dir "$wt_gitdir" \
        "$src_gitdir" "$dst" 2>/dev/null || return 1
    git -C "$wt_gitdir" config core.worktree "$dst"
    url="$(submodule_origin_url "$submod")"
    # origin is the public remote, not the neighbouring checkout it was cloned
    # from, so committing and pushing from in here works as it does in main.
    [[ -n "$url" ]] && git -C "$dst" remote set-url origin "$url"

    for entry in "$src"/* "$src"/.[!.]*; do
        [[ -e "$entry" ]] || continue
        base="$(basename "$entry")"
        [[ "$base" == ".git" ]] && continue
        clone_tree "$entry" "$dst/"
    done

    # HEAD and the index describe the commit the files were copied FROM, so the
    # checkout reads exactly as the main tree's does: clean apart from the patch
    # hunks in its working tree.
    git -C "$dst" update-ref --no-deref HEAD "$src_head"
    git -C "$dst" read-tree HEAD
    if [[ -f "$src_gitdir/$PATCH_DRIFT_STAMP" ]]; then
        clone_file "$src_gitdir/$PATCH_DRIFT_STAMP" "$wt_gitdir/$PATCH_DRIFT_STAMP"
    fi

    # Reconcile the pin. The main tree sits at ITS branch's commit, which this
    # branch need not share. Checking out the pinned commit rewrites only the
    # files that actually differ between the two, so everything else keeps the
    # mtime that keeps its object valid — which is what makes copy-then-fix
    # cheaper than a fresh checkout rather than merely different. --force
    # discards the copied patch hunks, and can discard nothing else: this
    # working tree was created from a copy seconds ago and no one has seen it.
    if [[ -n "$pinned" && "$pinned" != "$src_head" ]]; then
        echo -e "  $submod: ${YELLOW}pin differs from the main tree ($(echo "$src_head" | cut -c1-8) -> $(echo "$pinned" | cut -c1-8))${RESET}"
        git -C "$dst" checkout --detach --force --quiet "$pinned"
        PRIVATE_SUBMODULES_NEED_PATCHES=true
    fi
    return 0
}

# Give each private submodule its own working tree at the commit this branch
# points at. The git dir lands under .git/worktrees/<name>/modules/, so the
# checkout is independent of the main tree's and of every other worktree's.
checkout_private_submodules() {
    local submod dst wt_gitdir name
    WORKTREE_GIT_DIR="$(git -C "$WORKTREE_PATH" rev-parse --absolute-git-dir)"

    for submod in "${LIB_PRIVATE_SUBMODULES[@]}"; do
        dst="$WORKTREE_PATH/$submod"
        name="$(submodule_section_name "$submod")"
        [[ -n "$name" ]] || name="$submod"
        wt_gitdir="$WORKTREE_GIT_DIR/modules/$name"

        if [[ -L "$dst" ]]; then
            rm "$dst"
        fi

        # An interrupted run leaves a git dir with no checkout beside it, or a
        # checkout gutted down to its .git — a state that does not self-heal and
        # surfaces as a build error naming a missing object file rather than a
        # submodule. Both are ours and both are safe to redo, because the only
        # thing in either is what this script put there.
        if [[ -d "$wt_gitdir" ]] && { [[ ! -e "$dst/.git" ]] || ! has_worktree_content "$dst"; }; then
            echo -e "  $submod: ${YELLOW}interrupted materialization — redoing it${RESET}"
            rm -rf "$dst" "$wt_gitdir"
        fi

        if [[ -e "$dst/.git" ]]; then
            echo -e "  $submod: ${GREEN}already a private checkout${RESET}"
            continue
        fi

        # Files with no .git and no git dir of ours beside them are something a
        # person put there, not ours to delete. Everything past this point may
        # clear the path, so the check has to come first.
        if [[ -d "$dst" ]] && has_worktree_content "$dst"; then
            echo -e "  ${RED}$submod has content but is not a checkout — leaving it alone.${RESET}"
            echo -e "  ${YELLOW}Resolve by hand, then re-run with --setup-only.${RESET}"
            continue
        fi

        rm -rf "$dst"
        if materialize_private_submodule "$submod"; then
            echo -e "  $submod: ${GREEN}private checkout (copied from the main tree, mtimes kept)${RESET}"
            continue
        fi

        # No usable checkout to copy from: fall back to a fresh clone, which
        # needs the network and arrives unpatched.
        echo -e "  $submod: ${CYAN}private checkout (fresh clone — nothing to copy from)${RESET}"
        rm -rf "$dst" "$wt_gitdir"
        mkdir -p "$dst"
        if git -C "$WORKTREE_PATH" submodule update --init "$submod" >/dev/null 2>&1; then
            PRIVATE_SUBMODULES_NEED_PATCHES=true
            continue
        fi

        echo -e "  $submod: ${YELLOW}checkout failed${RESET}"
        echo -e "  ${YELLOW}falling back to a symlink — patches applied here will land in the MAIN tree's submodule${RESET}"
        rmdir "$dst" 2>/dev/null || true
        ln -s "$MAIN_TREE/$submod" "$dst"
    done
}

# Assert every lib/ submodule sits at the commit this branch pins. `git
# submodule status` cannot answer this in a worktree — it refuses to scan a tree
# where a gitlink path is a symlink — so ask each checkout directly.
#
# A private submodule at the wrong revision is fatal: it compiles, it links, and
# it is not the code this branch describes. A symlinked one can only ever be at
# the main tree's revision, so a mismatch there is a warning about a difference
# nothing in this worktree can fix.
check_submodule_pins() {
    local submod pinned actual fatal=false
    echo -e "${CYAN}Verifying submodule revisions...${RESET}"
    for submod in $(git -C "$MAIN_TREE" config --file .gitmodules --get-regexp path \
                        | grep "^submodule\." | awk '{print $2}' | grep "^lib/"); do
        pinned="$(git -C "$WORKTREE_PATH" rev-parse --verify --quiet "HEAD:$submod" 2>/dev/null || true)"
        actual="$(git -C "$WORKTREE_PATH/$submod" rev-parse --verify --quiet HEAD 2>/dev/null || true)"
        [[ -n "$pinned" && -n "$actual" ]] || continue
        [[ "$pinned" == "$actual" ]] && continue
        if is_private_submodule "$submod"; then
            echo -e "  ${RED}${BOLD}$submod is at $actual, this branch pins $pinned${RESET}"
            fatal=true
        else
            echo -e "  ${YELLOW}$submod is at $actual, this branch pins $pinned${RESET}"
            echo -e "  ${YELLOW}  (shared with the main tree — it holds whichever revision that tree checked out)${RESET}"
        fi
    done
    if [[ "$fatal" == "true" ]]; then
        echo ""
        echo -e "${RED}${BOLD}================================================================${RESET}"
        echo -e "${RED}${BOLD}  A private submodule is not at the revision this branch pins.${RESET}"
        echo -e "${RED}${BOLD}================================================================${RESET}"
        echo -e "${YELLOW}Building now would compile a different version of that library than"
        echo -e "the branch describes, and nothing downstream would say so.${RESET}"
        echo -e "Fix with: ${CYAN}git -C <submodule> checkout <pinned-sha>${RESET}"
        exit 1
    fi
    echo -e "${GREEN}✓ Submodules match this branch's pins${RESET}"
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

# Step 3d: Point claude-recall at the MAIN tree's .claude-recall/
#
# claude-recall resolves its lessons + stats directory from `git rev-parse
# --show-toplevel`, which inside a worktree is the WORKTREE. Every worktree
# therefore accumulated its own LESSONS.md/stats.json, and `git worktree remove`
# threw them away — so lesson scoring only ever reflected work done in the main
# tree. PROJECT_DIR overrides that resolution.
#
# Written to settings.local.json, which is globally gitignored, rather than
# symlinking .claude-recall/: those three files are TRACKED, and symlinking a
# tracked path is how a worktree clobbers it (#1107).
#
# Main-tree paths inside permissions are rewritten the same way
# compile_commands.json is above; PROJECT_DIR is set afterwards so it keeps
# pointing at the main tree rather than being rewritten with everything else.
if command -v python3 >/dev/null 2>&1; then
    mkdir -p "$WORKTREE_PATH/.claude"
    if MAIN_TREE="$MAIN_TREE" WORKTREE_PATH="$WORKTREE_PATH" python3 - <<'PYEOF'
import json, os, pathlib

main = os.environ["MAIN_TREE"]
wt = os.environ["WORKTREE_PATH"]
src = pathlib.Path(main, ".claude", "settings.local.json")
dst = pathlib.Path(wt, ".claude", "settings.local.json")

cfg = {}
if src.is_file():
    try:
        cfg = json.loads(src.read_text().replace(main, wt))
    except (json.JSONDecodeError, OSError):
        cfg = {}  # a malformed local file must not sink worktree setup

if not isinstance(cfg, dict):
    cfg = {}
env = cfg.setdefault("env", {})
if not isinstance(env, dict):
    env = cfg["env"] = {}
env["PROJECT_DIR"] = main
dst.write_text(json.dumps(cfg, indent=2) + "\n")
PYEOF
    then
        echo -e "  .claude/settings.local.json: ${GREEN}PROJECT_DIR -> main tree${RESET} (claude-recall stats)"
    else
        echo -e "  .claude/settings.local.json: ${YELLOW}skipped (could not write)${RESET}"
    fi
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
#
# lib/* hides the symlinks, but it would also hide the entries under lib/ that
# carry tracked content, so each of those is negated back in. Git cannot
# re-include a path whose parent directory is excluded, so the negation has to
# name the directory itself, not the files inside it.
#
# lib/mdns is negated with a TRAILING SLASH, which matches directories only.
# In the main tree it is a real directory and the negation applies, so tracked
# content there stays visible. In a worktree the path is a symlink, the
# negation does not apply, and lib/* keeps it hidden — otherwise every worktree
# reports a permanent `?? lib/mdns` for a symlink this script created. That
# stray entry is what `git add -A` once swept onto main as a blob replacing the
# tracked directory (restored in 3b0a8491b). lib/tuibox.h needs no such care:
# its symlink sits at the tracked path itself, and a path in the index is never
# reported as untracked.
EXCLUDES=(
    "# HelixScreen worktree setup - auto-generated excludes"
    "lib/*"
    "!lib/helix-xml"
    "!lib/mdns/"
    "!lib/minilzo"
    "!lib/quirc"
    "!lib/tuibox.h"
    "node_modules"
    ".venv"
    "build/"
    "compile_commands.json"
    ".fonts.stamp"
)

# Drop a legacy slashless "!lib/mdns", which would re-expose the worktree
# symlink and defeat the "!lib/mdns/" line below.
if [[ -f "$EXCLUDE_FILE" ]] && grep -qxF '!lib/mdns' "$EXCLUDE_FILE"; then
    EXCLUDE_TMP=$(mktemp)
    grep -vxF '!lib/mdns' "$EXCLUDE_FILE" > "$EXCLUDE_TMP" && mv "$EXCLUDE_TMP" "$EXCLUDE_FILE"
    echo -e "  ${YELLOW}replaced legacy !lib/mdns with !lib/mdns/${RESET}"
fi

# Add excludes if not already present. Match whole lines: a substring test
# reports "!lib/mdns/" as already present when only "lib/mdns" is there.
for exclude in "${EXCLUDES[@]}"; do
    if ! grep -qxF "$exclude" "$EXCLUDE_FILE" 2>/dev/null; then
        echo "$exclude" >> "$EXCLUDE_FILE"
    fi
done

# Mark symlinked paths as skip-worktree so git ignores typechanges
echo -e "${CYAN}Marking symlinks as skip-worktree...${RESET}"
cd "$WORKTREE_PATH"

# Symlinked submodules only. The mark exists to hide the symlink's typechange,
# and a private checkout has none — marking one would instead hide a real change
# of its pinned revision from `git status`, `git add` and the revision check
# below, which is the one thing that must stay visible.
for submod in $SUBMODULES; do
    if is_private_submodule "$submod"; then
        git update-index --no-skip-worktree "$submod" 2>/dev/null || true
        continue
    fi
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

# Step 9d: Make the private submodules carry THIS branch's patches
#
# patches/ is per-branch, and a private checkout is what lets two branches hold
# different patch sets at once. The copy above arrives carrying the main tree's
# patches, so it is already correct whenever the two trees agree on patches/ —
# and reapplying anyway is not free: build/.patches-applied is a prerequisite of
# the PCH, and therefore of every object, so re-stamping it turns a warm worktree
# cold. Reapply exactly when the copy cannot be trusted to describe this branch.
cd "$WORKTREE_PATH"
if [[ "$PRIVATE_SUBMODULES_NEED_PATCHES" != "true" ]] \
   && ! diff -rq "$MAIN_TREE/patches" "$WORKTREE_PATH/patches" >/dev/null 2>&1; then
    PRIVATE_SUBMODULES_NEED_PATCHES=true
    echo -e "${YELLOW}patches/ differs from the main tree's${RESET}"
fi
if [[ "$PRIVATE_SUBMODULES_NEED_PATCHES" == "true" ]]; then
    echo -e "${CYAN}Applying this branch's patches to the private submodules...${RESET}"
    if make reapply-patches; then
        echo -e "${GREEN}✓ Patches match this branch${RESET}"
    else
        echo -e "${RED}✗ make reapply-patches failed — this worktree's submodules do not match patches/${RESET}"
        exit 1
    fi
else
    echo -e "${GREEN}✓ Patches match the main tree's — private checkouts already carry them${RESET}"
fi

check_submodule_pins

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
echo -e "${YELLOW}Note: lib/lvgl, lib/libhv and lib/helix-xml are private to this worktree —"
echo -e "patches applied here reach no other tree. The remaining lib/ submodules are"
echo -e "symlinked from the main tree; un-symlink one before modifying it.${RESET}"
