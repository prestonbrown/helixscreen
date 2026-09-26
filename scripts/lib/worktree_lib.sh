#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared helpers for setup-worktree.sh and teardown-worktree.sh.
#
# Usage: source "$SCRIPT_DIR/lib/worktree_lib.sh"

# Canonicalize $1 (resolve symlinks) without requiring it to exist, so a
# worktree a previous run already emptied - but never pruned - can still be
# found and finished. GNU realpath's -m does exactly this; BSD/macOS
# realpath has no -m at all (it fails outright), so a `-m` probe decides
# which path this run takes rather than assuming.
# `cd "$dir" && pwd -P` (portable everywhere) then handles the part of $1
# that already exists, and the remaining, not-yet-created tail is appended
# as given - resolving nothing further, but never failing.
#
# Probed against "/", which always exists: BSD realpath rejects the -m flag
# itself regardless of its argument, so the probe's target doesn't matter,
# only that it can never fail for a reason unrelated to -m support.
HAVE_REALPATH_M=1
realpath -m -- "/" >/dev/null 2>&1 || HAVE_REALPATH_M=0

canonicalize_path() {
    local p="$1"
    if (( HAVE_REALPATH_M )); then
        realpath -m -- "$p"
        return
    fi
    if [[ -d "$p" ]]; then
        (cd -- "$p" && pwd -P)
        return
    fi
    local parent base
    parent="$(dirname -- "$p")"
    base="$(basename -- "$p")"
    if [[ -d "$parent" ]]; then
        printf '%s/%s\n' "$(cd -- "$parent" && pwd -P)" "$base"
    else
        printf '%s\n' "$p"
    fi
}

# .git/modules/<name> is common to every worktree, so initializing a submodule
# inside one (which is what --unlink's empty directories invite) repoints that
# shared core.worktree at it. Left there, the main tree and every worktree
# symlinking that submodule depend on this one tree existing; once it is gone
# they all fail `git status` with "cannot chdir" (prestonbrown/helixscreen#1621).
#
# Repoints each shared module whose core.worktree resolves inside $2 at the same
# path under the main tree $1. Private per-worktree modules live under
# .git/worktrees/<n>/modules and are never visited. $3=1 reports what would be
# restored and writes nothing.
#
# Only the pointer's parent is resolved physically. The last component is the
# submodule directory, which --relink has already turned back into a symlink
# into the main tree; following it would hide that the pointer names this tree.
restore_shared_module_pointers() {
    local main wt dry_run="${3:-0}" cfg target p resolved gitdir depth up new restored=0
    main="$(canonicalize_path "$1")"
    wt="$(canonicalize_path "$2")"
    for cfg in "$main"/.git/modules/*/config "$main"/.git/modules/lib/*/config; do
        [[ -f "$cfg" ]] || continue
        target="$(git config --file "$cfg" --get core.worktree 2>/dev/null || true)"
        [[ -n "$target" ]] || continue
        gitdir="$(dirname -- "$cfg")"
        if [[ "$target" == /* ]]; then p="$target"; else p="$gitdir/$target"; fi
        resolved="$(canonicalize_path "$(dirname -- "$p")")/$(basename -- "$p")"
        [[ "$resolved" == "$wt"/* ]] || continue
        # One ../ per component of .git/modules/<name> climbs back to the main
        # tree, whatever depth the worktree itself sits at.
        depth="${gitdir#"$main"/}"
        up="$(printf '%s' "$depth" | sed -E 's#[^/]+#..#g')/"
        new="${up}${resolved#"$wt"/}"
        if (( dry_run )); then
            echo "  $(basename -- "$gitdir"): points into this worktree, would restore to $new"
        else
            git config --file "$cfg" core.worktree "$new"
            echo "  $(basename -- "$gitdir"): pointed into this worktree, restored to the main tree"
        fi
        restored=$((restored + 1))
    done
    if (( restored == 0 )); then
        echo "  none pointed into this worktree"
    fi
}
