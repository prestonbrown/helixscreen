#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# HelixScreen Worktree Teardown Script
# Removes a git worktree created by setup-worktree.sh, and optionally its branch

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

usage() {
    echo "Usage: $0 [OPTIONS] <worktree-name-or-path>"
    echo ""
    echo "Removes a worktree created by setup-worktree.sh and, by default, the"
    echo "branch it had checked out. Refuses to discard anything unique."
    echo ""
    echo "Arguments:"
    echo "  worktree-name-or-path   A name under .worktrees/, or a path."
    echo "                          'my-feature' resolves to .worktrees/my-feature."
    echo ""
    echo "Options:"
    echo "  --into <ref>      Branch the work must already be contained in"
    echo "                    (default: main). The branch is deleted only if its"
    echo "                    tip is an ancestor of this ref."
    echo "  --keep-branch     Remove the worktree, leave the branch alone"
    echo "  --force-branch    Delete the branch with -D even though git refused -d."
    echo "                    ONLY safe when this script has confirmed containment;"
    echo "                    it prints what it verified before doing it."
    echo "  --force           Remove even with uncommitted changes in the worktree."
    echo "                    Discards that work permanently."
    echo "  -n, --dry-run     Print what would happen, change nothing"
    echo "  -h, --help        Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 my-feature                 # remove worktree + branch, if merged to main"
    echo "  $0 my-feature --into devel    # containment checked against devel instead"
    echo "  $0 my-feature --keep-branch   # free the worktree, keep the branch"
    echo "  $0 my-feature -n              # show the plan"
    echo ""
    echo "Why this exists rather than 'git worktree remove':"
    echo "  setup-worktree.sh gives each worktree PRIVATE checkouts of lvgl, libhv"
    echo "  and helix-xml, and git refuses to remove a worktree containing"
    echo "  submodules. The removal is therefore a guarded rm -rf plus a prune."
    exit 0
}

INTO="main"
KEEP_BRANCH=0
FORCE_BRANCH=0
FORCE=0
DRY_RUN=0
TARGET=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --into)         INTO="${2:-}"; shift 2 ;;
        --keep-branch)  KEEP_BRANCH=1; shift ;;
        --force-branch) FORCE_BRANCH=1; shift ;;
        --force)        FORCE=1; shift ;;
        -n|--dry-run)   DRY_RUN=1; shift ;;
        -h|--help)      usage ;;
        -*)             echo -e "${RED}Unknown option: $1${RESET}"; exit 1 ;;
        *)              TARGET="$1"; shift ;;
    esac
done

[[ -n "$TARGET" ]] || usage

say()  { echo -e "$@"; }
run()  { if (( DRY_RUN )); then echo -e "  ${CYAN}would run:${RESET} $*"; else "$@"; fi; }

# The main tree is the one git calls the common dir's parent, so this works
# whether we are invoked from the main tree or from inside some other worktree.
MAIN_TREE="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
MAIN_TREE="${MAIN_TREE%/.git}"
if [[ -z "$MAIN_TREE" || ! -d "$MAIN_TREE" ]]; then
    say "${RED}Error: not inside a git repository.${RESET}"
    exit 1
fi

# Resolve a bare name against .worktrees/, a path as given.
if [[ "$TARGET" == */* || -d "$TARGET" ]]; then
    WORKTREE_PATH="$TARGET"
else
    WORKTREE_PATH="$MAIN_TREE/.worktrees/$TARGET"
fi

if [[ ! -d "$WORKTREE_PATH" ]]; then
    say "${RED}Error: no such directory: $WORKTREE_PATH${RESET}"
    exit 1
fi

WT_ABS="$(cd "$WORKTREE_PATH" && pwd -P)"
MAIN_ABS="$(cd "$MAIN_TREE" && pwd -P)"

# Guard: never the main tree. Both resolved with -P so a symlinked path cannot
# slip past the compare. Deleting the main tree would take every other
# worktree's lib/ symlink target with it.
if [[ "$WT_ABS" == "$MAIN_ABS" ]]; then
    say "${RED}Error: that is the main tree, not a worktree:${RESET}"
    say "  $MAIN_ABS"
    exit 1
fi

# Guard: it must be a worktree git knows about. Without this the script is an
# rm -rf with a friendly name on any directory the user names.
#
# The list is captured before matching rather than piped into grep -q: under
# `set -o pipefail`, grep -q closes the pipe on its first hit, git dies of
# SIGPIPE, and the pipeline reports failure on the very input that matched.
WORKTREE_LIST="$(git -C "$MAIN_ABS" worktree list --porcelain)"
if ! grep -qxF "worktree $WT_ABS" <<<"$WORKTREE_LIST"; then
    say "${RED}Error: git does not list that path as a worktree of this repo:${RESET}"
    say "  $WT_ABS"
    say "Refusing to delete a directory git is not tracking as a worktree."
    exit 1
fi

# Guard: do not pull the floor out from under a running process.
BUSY=""
for p in $(pgrep -x -d' ' 'make|cc1plus|helix-tests|helix-screen|git' 2>/dev/null || true); do
    cwd="$(readlink "/proc/$p/cwd" 2>/dev/null || true)"
    [[ "$cwd" == "$WT_ABS"* ]] && BUSY+=" $p"
done
if [[ -n "$BUSY" ]]; then
    say "${RED}Error: processes are running inside that worktree:${RESET}$BUSY"
    say "A build or test run there will fail in confusing ways if the tree vanishes."
    say "Wait for them, or kill those PIDs by number."
    exit 1
fi

BRANCH="$(git -C "$WT_ABS" branch --show-current 2>/dev/null || true)"

say "${BOLD}${CYAN}HelixScreen Worktree Teardown${RESET}"
say "  worktree: $WT_ABS"
say "  branch:   ${BRANCH:-<detached>}"
say "  contained in: $INTO"
say ""

# --- what would be lost -------------------------------------------------------

DIRTY="$( { git -C "$WT_ABS" status --porcelain 2>/dev/null || true; } | wc -l | tr -d ' ')"
if [[ "$DIRTY" != "0" ]]; then
    if (( FORCE )); then
        say "${YELLOW}! $DIRTY uncommitted change(s) will be DISCARDED (--force).${RESET}"
    else
        say "${RED}Error: $DIRTY uncommitted change(s) in the worktree.${RESET}"
        { git -C "$WT_ABS" status --short || true; } | head -10 | sed 's/^/    /'
        say "Commit them, or pass ${CYAN}--force${RESET} to discard permanently."
        exit 1
    fi
fi

# The private submodules are the ones that can hold work nothing else has.
# lvgl and libhv are routinely dirty from patches/ and that is reproducible;
# helix-xml is our own repo and is edited directly, so unpushed commits there
# are real work that this script must not silently delete.
for sub in helix-xml libhv lvgl; do
    [[ -d "$WT_ABS/lib/$sub/.git" || -f "$WT_ABS/lib/$sub/.git" ]] || continue
    # No upstream configured is a git fatal, not an error here: a submodule with
    # no remote tracking branch simply has nothing that could be unpushed.
    unpushed="$( { git -C "$WT_ABS/lib/$sub" log --oneline '@{u}..' 2>/dev/null || true; } | wc -l | tr -d ' ')"
    if [[ "$unpushed" != "0" ]]; then
        say "${RED}Error: lib/$sub has $unpushed unpushed commit(s).${RESET}"
        { git -C "$WT_ABS/lib/$sub" log --oneline '@{u}..' 2>/dev/null || true; } | head -5 | sed 's/^/    /'
        say "Push them from ${CYAN}$WT_ABS/lib/$sub${RESET} first; this script will not discard them."
        exit 1
    fi
done

# --- branch containment -------------------------------------------------------

DELETE_BRANCH=0
if [[ -n "$BRANCH" ]] && (( ! KEEP_BRANCH )); then
    if ! git -C "$MAIN_ABS" rev-parse --verify --quiet "$INTO" >/dev/null; then
        say "${YELLOW}! '$INTO' does not resolve; keeping the branch.${RESET}"
    elif git -C "$MAIN_ABS" merge-base --is-ancestor "$BRANCH" "$INTO" 2>/dev/null; then
        AHEAD="$( { git -C "$MAIN_ABS" log --oneline "$INTO..$BRANCH" 2>/dev/null || true; } | wc -l | tr -d ' ')"
        say "${GREEN}Branch tip is an ancestor of $INTO ($AHEAD commits outstanding).${RESET}"
        DELETE_BRANCH=1
    else
        say "${YELLOW}! '$BRANCH' is NOT contained in '$INTO' - keeping it.${RESET}"
        { git -C "$MAIN_ABS" log --oneline "$INTO..$BRANCH" 2>/dev/null || true; } | head -5 | sed 's/^/    /'
    fi
fi

# --- remove -------------------------------------------------------------------

say ""
say "${BOLD}Removing the worktree${RESET}"
say "  lib/ holds symlinks into the main tree plus private checkouts of lvgl,"
say "  libhv and helix-xml. rm -rf removes a symlink, never its target, so the"
say "  main tree's copies are untouched."
run rm -rf "$WT_ABS"
run git -C "$MAIN_ABS" worktree prune

# The claim, if any, outlives the directory and would read LIVE forever.
if [[ -x "$MAIN_ABS/scripts/helix-claim" ]]; then
    run "$MAIN_ABS/scripts/helix-claim" release "worktree:$(basename "$WT_ABS")" >/dev/null 2>&1 || true
fi

# --- branch -------------------------------------------------------------------

if (( DELETE_BRANCH )); then
    say ""
    say "${BOLD}Deleting branch '$BRANCH'${RESET}"
    if (( DRY_RUN )); then
        echo -e "  ${CYAN}would run:${RESET} git branch -d $BRANCH"
    elif git -C "$MAIN_ABS" branch -d "$BRANCH" 2>/dev/null; then
        say "${GREEN}Deleted.${RESET}"
    else
        # git -d also wants the branch merged into its UPSTREAM, which for a
        # local-only merge it is not: the work is on local $INTO but not on
        # origin/$INTO. Containment was verified above, so -D discards nothing.
        UPSTREAM="$(git -C "$MAIN_ABS" rev-parse --abbrev-ref "$BRANCH@{u}" 2>/dev/null || echo '<none>')"
        say "${YELLOW}git refused -d.${RESET} Its check includes the branch's upstream ($UPSTREAM),"
        say "which is usually behind when $INTO has been merged locally but not pushed."
        if (( FORCE_BRANCH )); then
            say "Verified: tip is an ancestor of $INTO, so -D discards nothing."
            run git -C "$MAIN_ABS" branch -D "$BRANCH"
            say "${GREEN}Deleted.${RESET}"
        else
            say "Push $INTO, or re-run with ${CYAN}--force-branch${RESET}."
        fi
    fi
fi

say ""
if (( DRY_RUN )); then
    say "${CYAN}Dry run - nothing changed.${RESET}"
else
    say "${GREEN}${BOLD}Teardown complete.${RESET}"
fi
