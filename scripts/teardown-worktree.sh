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

# File owner as a login name, GNU stat's -c first (BSD rejects that flag
# outright), BSD's -f second. Unknown either way rather than aborting -
# these calls only ever inform a leftover-files report, never a decision.
file_owner()       { stat -c '%U'    "$1" 2>/dev/null || stat -f '%Su'      "$1" 2>/dev/null || echo '?';   }
file_owner_group() { stat -c '%U:%G' "$1" 2>/dev/null || stat -f '%Su:%Sg' "$1" 2>/dev/null || echo '?:?'; }

# The main tree is the one git calls the common dir's parent, so this works
# whether we are invoked from the main tree or from inside some other worktree.
MAIN_TREE="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
MAIN_TREE="${MAIN_TREE%/.git}"
if [[ -z "$MAIN_TREE" || ! -d "$MAIN_TREE" ]]; then
    say "${RED}Error: not inside a git repository.${RESET}"
    exit 1
fi

MAIN_ABS="$(cd "$MAIN_TREE" && pwd -P)"

source "$(dirname -- "${BASH_SOURCE[0]}")/lib/worktree_lib.sh"

# Resolve a bare name against .worktrees/, a path as given.
if [[ "$TARGET" == */* || -d "$TARGET" ]]; then
    CANDIDATE_ABS="$(canonicalize_path "$TARGET")"
else
    CANDIDATE_ABS="$(canonicalize_path "$MAIN_TREE/.worktrees/$TARGET")"
fi

# Guard: it must be a worktree git knows about, resolved from git's own
# registry rather than a filesystem check. A directory existing on disk
# proves nothing - it could be an unrelated folder - and a directory NOT
# existing doesn't mean there is nothing to clean up: git may still hold a
# stale, prunable registration for it that this script can still finish.
#
# The list is captured before matching rather than piped into grep -q: under
# `set -o pipefail`, grep -q closes the pipe on its first hit, git dies of
# SIGPIPE, and the pipeline reports failure on the very input that matched.
WORKTREE_LIST="$(git -C "$MAIN_ABS" worktree list --porcelain)"
WT_ABS=""
if grep -qxF "worktree $CANDIDATE_ABS" <<<"$WORKTREE_LIST"; then
    WT_ABS="$CANDIDATE_ABS"
fi

if [[ -z "$WT_ABS" ]]; then
    say "${RED}Error: git does not list that path as a worktree of this repo:${RESET}"
    say "  $CANDIDATE_ABS"
    say "Refusing to delete a directory git is not tracking as a worktree."
    exit 1
fi

# Guard: never the main tree. Deleting it would take every other worktree's
# lib/ symlink target with it.
if [[ "$WT_ABS" == "$MAIN_ABS" ]]; then
    say "${RED}Error: that is the main tree, not a worktree:${RESET}"
    say "  $MAIN_ABS"
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

# Guard: a live claim outranks an idle-looking tree. The process scan above sees
# a build, and sees nothing at all while a tree's owner is reading code to work
# out what a bug is - a phase with no compiler, no commits and a clean status.
# An advisory claim is the only signal that phase leaves behind.
if [[ -x "$MAIN_ABS/scripts/helix-claim" ]]; then
    WT_NAME="$(basename "$WT_ABS")"
    for res in "worktree:$WT_NAME" "build:$WT_NAME"; do
        # check exits non-zero only for LIVE; FREE and STALE pass.
        "$MAIN_ABS/scripts/helix-claim" check "$res" >/dev/null 2>&1 && continue
        if (( FORCE )); then
            say "${YELLOW}! $res is claimed by a live owner; removing anyway (--force).${RESET}"
            continue
        fi
        say "${RED}Error: $res is claimed and its owner is alive.${RESET}"
        { "$MAIN_ABS/scripts/helix-claim" check "$res" 2>&1 || true; } | sed 's/^/    /'
        say "If the claim is yours, release it and rerun:"
        say "  ${CYAN}scripts/helix-claim release $res${RESET}"
        say "Otherwise leave the tree alone, or pass ${CYAN}--force${RESET}."
        exit 1
    done
fi

# Guard: a worktree's ".git" is a pointer file into the main repo's admin
# data. Once it is gone - a previous teardown that got partway through
# removing this same tree before root-owned leftovers (a docker build, say)
# stopped it - every `git -C "$WT_ABS"` command below would discover upward
# past the missing pointer and silently answer for whatever repository it
# finds next, which for a worktree is the main tree. Handle this case on its
# own rather than let the checks below report on the wrong repository.
GIT_POINTER_OK=1
[[ -e "$WT_ABS/.git" ]] || GIT_POINTER_OK=0

if (( ! GIT_POINTER_OK )); then
    say "${YELLOW}! $WT_ABS has no .git of its own.${RESET}"
    say "Its git pointer is missing, so any git command scoped to this path would"
    say "silently fall through to the main tree instead - those checks are skipped."
    say ""
    say "${BOLD}What is left on disk:${RESET}"
    ME="$(id -un)"
    FOUND=0
    while IFS= read -r -d '' f; do
        FOUND=1
        OWNER="$(file_owner "$f")"
        if [[ "$OWNER" == "$ME" ]]; then
            say "  $OWNER  $f"
        else
            say "  ${RED}$OWNER${RESET}  $f  (not yours - needs sudo to remove)"
        fi
    done < <(find "$WT_ABS" -mindepth 1 -print0 2>/dev/null)
    (( FOUND )) || say "  (nothing left - only the worktree's registration remains)"
    say ""
    if (( ! FORCE )); then
        say "Remove the files above yourself (root-owned ones need sudo), or rerun"
        say "with ${CYAN}--force${RESET} to let this script finish the removal."
        exit 1
    fi
    say "${YELLOW}--force given: skipping branch cleanup.${RESET}"
    BRANCH=""
    DELETE_BRANCH=0
else
    BRANCH="$(git -C "$WT_ABS" branch --show-current 2>/dev/null || true)"
fi

say "${BOLD}${CYAN}HelixScreen Worktree Teardown${RESET}"
say "  worktree: $WT_ABS"
say "  branch:   ${BRANCH:-<detached>}"
say "  contained in: $INTO"
say ""

if (( GIT_POINTER_OK )); then
    # --- what would be lost ---------------------------------------------------

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
fi

# --- branch containment -------------------------------------------------------

DELETE_BRANCH=0
UNCONTAINED=0
if [[ -n "$BRANCH" ]] && (( ! KEEP_BRANCH )); then
    if ! git -C "$MAIN_ABS" rev-parse --verify --quiet "$INTO" >/dev/null; then
        say "${YELLOW}! '$INTO' does not resolve; keeping the branch.${RESET}"
    elif git -C "$MAIN_ABS" merge-base --is-ancestor "$BRANCH" "$INTO" 2>/dev/null; then
        say "${GREEN}Branch tip is an ancestor of $INTO.${RESET}"
        DELETE_BRANCH=1
    elif (( FORCE_BRANCH )); then
        say "${YELLOW}! '$BRANCH' is NOT contained in '$INTO'.${RESET}"
        { git -C "$MAIN_ABS" log --oneline "$INTO..$BRANCH" 2>/dev/null || true; } | head -5 | sed 's/^/    /'
        say "${YELLOW}--force-branch given: deleting anyway, discarding the commits above.${RESET}"
        UNCONTAINED=1
        DELETE_BRANCH=1
    else
        say "${YELLOW}! '$BRANCH' is NOT contained in '$INTO' - keeping it.${RESET}"
        { git -C "$MAIN_ABS" log --oneline "$INTO..$BRANCH" 2>/dev/null || true; } | head -5 | sed 's/^/    /'
        say "  Re-run with ${CYAN}--force-branch${RESET} to delete it and discard them."
    fi
fi

# --- remove -------------------------------------------------------------------

say ""
say "${BOLD}Removing the worktree${RESET}"
say "  lib/ holds symlinks into the main tree plus private checkouts of lvgl,"
say "  libhv and helix-xml. rm -rf removes a symlink, never its target, so the"
say "  main tree's copies are untouched."
if (( DRY_RUN )); then
    say "  ${CYAN}would run:${RESET} rm -rf $WT_ABS (contents first, .git pointer last)"
else
    # Contents first, .git pointer last: if a leftover (root-owned docker
    # build output, say) stops this short, the worktree still has its own
    # .git and keeps identifying itself to git on a rerun, instead of a git
    # command scoped to it silently falling through to the main tree.
    find "$WT_ABS" -mindepth 1 -maxdepth 1 -not -name '.git' -exec rm -rf {} + 2>/dev/null || true
    LEFTOVER="$(find "$WT_ABS" -mindepth 1 -not -name '.git' 2>/dev/null || true)"
    if [[ -n "$LEFTOVER" ]]; then
        say ""
        say "${RED}Error: could not fully remove $WT_ABS - files remain:${RESET}"
        while IFS= read -r f; do
            say "  $(file_owner_group "$f")  $f"
        done <<<"$LEFTOVER"
        say "Remove the listed files (root-owned ones need sudo) and rerun this script."
        exit 1
    fi
    rm -rf "$WT_ABS/.git"
    rmdir "$WT_ABS" 2>/dev/null || true
fi
run git -C "$MAIN_ABS" worktree prune

# --- shared submodule pointers -------------------------------------------------
say ""
say "${BOLD}Restoring shared submodule pointers${RESET}"
restore_shared_module_pointers "$MAIN_ABS" "$WT_ABS" run

# The claim, if any, outlives the directory and would read LIVE forever.
if [[ -x "$MAIN_ABS/scripts/helix-claim" ]]; then
    # --force: the tree is being deleted, so its claim goes with it even when the
    # session that took it is still alive somewhere.
    run "$MAIN_ABS/scripts/helix-claim" release --force "worktree:$(basename "$WT_ABS")" >/dev/null 2>&1 || true
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
        # git -d also wants the branch merged into its UPSTREAM. setup-worktree.sh
        # branches from the tracked upstream, so a branch made here tracks
        # origin/<default> - a ref that moves only on a fetch, or on a push made
        # by remote NAME. A push by URL, or one made from the other machine,
        # leaves it behind while the remote already has the work, and the branch
        # then reads as unpushed when it is not.
        UPSTREAM="$(git -C "$MAIN_ABS" rev-parse --abbrev-ref "$BRANCH@{u}" 2>/dev/null || echo '<none>')"
        DELETED_AFTER_FETCH=0
        if [[ "$UPSTREAM" == */* ]] && (( ! UNCONTAINED )); then
            say "Refreshing $UPSTREAM before reporting the branch as unpushed..."
            if git -C "$MAIN_ABS" fetch --quiet "${UPSTREAM%%/*}" "${UPSTREAM#*/}" 2>/dev/null &&
               git -C "$MAIN_ABS" branch -d "$BRANCH" 2>/dev/null; then
                # Retried rather than reasoned about: git re-applies its own
                # check against the ref as it now stands, so nothing here has to
                # decide the branch is safe to delete.
                say "${GREEN}Deleted.${RESET} ($UPSTREAM was stale; ${UPSTREAM%%/*} already had the work.)"
                DELETED_AFTER_FETCH=1
            fi
        fi
        if (( ! DELETED_AFTER_FETCH )); then
            if (( UNCONTAINED )); then
                say "${YELLOW}git refused -d${RESET} because the branch holds commits $INTO does not."
            else
                say "${YELLOW}git refused -d.${RESET} Its check includes the branch's upstream ($UPSTREAM),"
                say "which is behind when $INTO has been merged locally but not pushed."
            fi
            say "  git said: $(git -C "$MAIN_ABS" branch -d "$BRANCH" 2>&1 | head -2 | tr '\n' ' ')"
            if (( FORCE_BRANCH )); then
                if (( UNCONTAINED )); then
                    say "${YELLOW}-D discards the commits listed above; no other ref holds them.${RESET}"
                else
                    # Containment in $INTO was verified above, so -D discards nothing.
                    say "Verified: tip is an ancestor of $INTO, so -D discards nothing."
                fi
                run git -C "$MAIN_ABS" branch -D "$BRANCH"
                say "${GREEN}Deleted.${RESET}"
            else
                say "Push $INTO, or re-run with ${CYAN}--force-branch${RESET}."
            fi
        fi
    fi
fi

say ""
if (( DRY_RUN )); then
    say "${CYAN}Dry run - nothing changed.${RESET}"
else
    say "${GREEN}${BOLD}Teardown complete.${RESET}"
fi
