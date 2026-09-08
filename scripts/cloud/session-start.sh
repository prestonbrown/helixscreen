#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# SessionStart hook — runs at the top of every session, local and cloud.
#
# A laptop or thelio never wrote the cloud env-setup marker, so this exits
# silently on them: it must never add work or output to a machine that never
# asked for it. On a cloud VM whose setup script (scripts/cloud/env-setup.sh)
# completed, it finishes the per-session pieces that script cannot do before
# the repo exists — wiring the seed clone's objects into this checkout,
# initializing submodules, and pointing .venv at the prebuilt one.
#
# HELIX_CLOUD_ENV_DIR / HELIX_CLOUD_SEED let tests point this at a scratch
# directory instead of the real /opt paths.
set -uo pipefail

CLOUD_ENV_DIR="${HELIX_CLOUD_ENV_DIR:-/opt/helix-cloud-env}"
CLOUD_SEED="${HELIX_CLOUD_SEED:-/opt/helixscreen-seed}"

[ -f "$CLOUD_ENV_DIR/READY" ] || exit 0

REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
[ -n "$REPO_ROOT" ] || exit 0
cd "$REPO_ROOT" || exit 0

FAILED_STEP=""

# a. Wire the seed clone's objects in as an alternate, so `git submodule
# update` below fetches nothing for any submodule the seed already carries —
# it borrows those objects instead of downloading them again. Skipped in a
# linked worktree, where .git is a file rather than the object store itself.
if [ -d .git ] && [ -d "$CLOUD_SEED/.git/objects" ]; then
    mkdir -p .git/objects/info 2>/dev/null
    ALTERNATES_FILE=".git/objects/info/alternates"
    if [ ! -f "$ALTERNATES_FILE" ] || ! grep -Fxq "$CLOUD_SEED/.git/objects" "$ALTERNATES_FILE" 2>/dev/null; then
        printf '%s\n' "$CLOUD_SEED/.git/objects" >> "$ALTERNATES_FILE"
    fi
    git config submodule.alternateLocation superproject
    git config submodule.alternateErrorStrategy info
fi

# b. Submodules — synchronous, timed for the banner below.
SUBMODULE_START=$(date +%s)
if ! git submodule update --init --recursive --jobs 8; then
    FAILED_STEP="submodules"
fi
SUBMODULE_ELAPSED=$(( $(date +%s) - SUBMODULE_START ))

# c. Point .venv at the prebuilt venv, then reconcile requirements. Cheap
# when the venv already satisfies requirements.txt, so this runs every
# session rather than only on the symlink's first creation.
if [ ! -e .venv ] && [ -x /opt/helix-venv/bin/python ]; then
    ln -s /opt/helix-venv .venv 2>/dev/null || true
fi
if ! make venv-setup >/tmp/helix-venv-setup.log 2>&1; then
    [ -z "$FAILED_STEP" ] && FAILED_STEP="venv-setup"
fi

# d. One short banner. SessionStart stdout becomes context for the agent, so
# this stays terse rather than replaying either log.
ccache_summary() {
    command -v ccache >/dev/null 2>&1 || { printf 'not installed'; return; }
    local tarball size
    # What env-setup.sh recorded at provisioning time is the authority on whether
    # a cache was ever fetched. `ccache -s` can only describe the cache now, and
    # says the least when it matters most: ccache 4.9.1 omits the "Files in cache"
    # line entirely for an empty cache, so treating its absence as anything but
    # empty reports a cold box as warm.
    tarball=$(sed -n 's/^ccache_tarball: *//p' "$CLOUD_ENV_DIR/READY" 2>/dev/null | head -1)
    size=$(ccache -s 2>/dev/null | grep -E '^ *Cache size' | grep -oE '[0-9]+(\.[0-9]+)?' | head -1)
    [ -n "$size" ] || size=0
    case "$size" in
        0 | 0.0 | 0.00) printf 'EMPTY — %s' "${tarball:-provisioning status unknown}" ;;
        *) printf '%s GB — %s (hit rate resets per session)' "$size" "${tarball:-provisioning status unknown}" ;;
    esac
}

if [ -n "$FAILED_STEP" ]; then
    printf '[cloud-env] ready with issues: %s failed — submodules %ss, ccache %s\n' \
        "$FAILED_STEP" "$SUBMODULE_ELAPSED" "$(ccache_summary)"
else
    printf '[cloud-env] ready: submodules %ss, ccache %s, venv ok\n' \
        "$SUBMODULE_ELAPSED" "$(ccache_summary)"
fi
printf '[cloud-env] build with plain `make -j4` and `make test -j4` (default OPT; no OPT=0), never two makes in one tree\n'

exit 0
