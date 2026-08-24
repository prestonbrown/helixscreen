#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The K2 keeps its bulk storage on /mnt/UDISK (27.5GB). Both /opt (where
# HelixScreen installs) and /usr/data are on the root overlay, which is ~240MB
# and 68% full before we add anything. Anything of ours that grows must live on
# /mnt/UDISK, and the cache we used to write to /usr/data must be reclaimed.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
PLATFORM_SH="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
RELEASE_SH="$WORKTREE_ROOT/scripts/lib/installer/release.sh"
APP_GLOBALS="$WORKTREE_ROOT/src/app_globals.cpp"

setup() {
    load helpers

    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED _HELIX_RELEASE_SOURCED
    export SUDO=""
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/release.sh"

    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }
}

# ---------------------------------------------------------------------------
# The app's cache root (compile-time branch — gated statically)
#
# get_helix_cache_dir()'s K2 arm is behind #if defined(HELIX_PLATFORM_K2), so a
# native test build cannot reach it and a functional assertion here would pass
# whatever the branch says. Gate the source instead; the runtime behaviour is
# verified on the device by the "[App Globals] Cache dir (K2)" log line.
# ---------------------------------------------------------------------------

@test "K2 cache branch prefers /mnt/UDISK over the root overlay" {
    local branch
    branch=$(awk '/#elif defined\(HELIX_PLATFORM_K2\)/,/#elif defined\(HELIX_PLATFORM_MIPS\)/' "$APP_GLOBALS")
    [ -n "$branch" ]
    echo "$branch" | grep -q '/mnt/UDISK'
    # /usr/data may remain only as a fallback, never as the first choice.
    echo "$branch" | grep -q 'mnt/UDISK".*"/usr/data\|"/mnt/UDISK", "/usr/data"'
}

@test "K1 (MIPS) keeps /usr/data — it is the large partition there" {
    local branch
    branch=$(awk '/#elif defined\(HELIX_PLATFORM_MIPS\)/,/#elif defined\(HELIX_PLATFORM_ANDROID\)/' "$APP_GLOBALS")
    [ -n "$branch" ]
    echo "$branch" | grep -q '/usr/data/helixscreen/cache'
    echo "$branch" | refute_grep '/mnt/UDISK'
}

@test "K2 and MIPS are no longer a shared cache branch" {
    # They disagree about what /usr/data is; sharing the arm is what put the
    # K2's cache on its 240MB overlay.
    refute_grep 'HELIX_PLATFORM_MIPS) || defined(HELIX_PLATFORM_K2)' "$APP_GLOBALS"
}

# ---------------------------------------------------------------------------
# Reclaiming the cache the old builds left on the overlay
# ---------------------------------------------------------------------------

@test "k2 declares the stale cache dir for cleanup" {
    run bash -c "grep -A 25 '\"k2\"' '$PLATFORM_SH' | grep STALE_CACHE_DIRS"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q '/usr/data/helixscreen/cache'
}

@test "cleanup removes a declared stale cache dir" {
    local stale="$BATS_TEST_TMPDIR/usr/data/helixscreen/cache"
    mkdir -p "$stale/helix_thumbs"
    echo thumb > "$stale/helix_thumbs/a.png"

    STALE_CACHE_DIRS="$stale"
    cleanup_stale_cache_dirs

    [ ! -d "$stale" ]
}

@test "cleanup refuses a path that is not a cache dir" {
    # Guard shape copied from the off-partition rollback cleanup: only ever
    # remove a path whose final component is exactly "cache". A past incident
    # wiped a K2's /mnt/UDISK mount root via an unguarded rm -rf.
    local victim="$BATS_TEST_TMPDIR/mnt/UDISK"
    mkdir -p "$victim/printer_data"
    echo important > "$victim/printer_data/printer.cfg"

    STALE_CACHE_DIRS="$victim"
    cleanup_stale_cache_dirs

    [ -d "$victim" ]
    [ -f "$victim/printer_data/printer.cfg" ]
}

@test "cleanup refuses a bare /cache at the filesystem root" {
    STALE_CACHE_DIRS="/cache"
    run cleanup_stale_cache_dirs
    # Must not have attempted removal of a top-level directory.
    echo "$output" | grep -qi 'refus'
}

@test "cleanup tolerates an absent stale dir" {
    STALE_CACHE_DIRS="$BATS_TEST_TMPDIR/not/here/cache"
    run cleanup_stale_cache_dirs
    [ "$status" -eq 0 ]
}

@test "cleanup handles multiple declared dirs" {
    local a="$BATS_TEST_TMPDIR/one/cache" b="$BATS_TEST_TMPDIR/two/cache"
    mkdir -p "$a" "$b"

    STALE_CACHE_DIRS="$a $b"
    cleanup_stale_cache_dirs

    [ ! -d "$a" ]
    [ ! -d "$b" ]
}

@test "unset STALE_CACHE_DIRS is a no-op" {
    unset STALE_CACHE_DIRS
    run cleanup_stale_cache_dirs
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Reclaiming scratch dirs leaked before cleanup was armed on EXIT
# ---------------------------------------------------------------------------

@test "cleanup removes a leaked installer scratch dir" {
    local stale="$BATS_TEST_TMPDIR/usr/data/helixscreen-install"
    mkdir -p "$stale"
    dd if=/dev/zero of="$stale/helixscreen.zip" bs=1024 count=8 2>/dev/null

    STALE_CACHE_DIRS="$stale"
    TMP_DIR=""
    cleanup_stale_cache_dirs

    [ ! -d "$stale" ]
}

@test "cleanup removes a dot-prefixed scratch dir" {
    local stale="$BATS_TEST_TMPDIR/opt/.helixscreen-install"
    mkdir -p "$stale"

    STALE_CACHE_DIRS="$stale"
    TMP_DIR=""
    cleanup_stale_cache_dirs

    [ ! -d "$stale" ]
}

@test "cleanup NEVER removes the scratch dir this run is staging into" {
    # The declared list can name the very dir detect_tmp_dir just picked; wiping
    # it mid-install would delete the tree we are about to swap in.
    local active="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-install"
    mkdir -p "$active/helixscreen/bin"
    echo binary > "$active/helixscreen/bin/helix-screen"

    STALE_CACHE_DIRS="$active"
    TMP_DIR="$active"
    cleanup_stale_cache_dirs

    [ -d "$active" ]
    [ -f "$active/helixscreen/bin/helix-screen" ]
}

@test "cleanup tolerates a trailing slash on the active TMP_DIR" {
    local active="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-install"
    mkdir -p "$active"

    STALE_CACHE_DIRS="$active"
    TMP_DIR="$active/"
    cleanup_stale_cache_dirs

    [ -d "$active" ]
}

@test "cleanup still refuses a non-cache non-scratch path" {
    local victim="$BATS_TEST_TMPDIR/mnt/UDISK/printer_data"
    mkdir -p "$victim"
    echo cfg > "$victim/printer.cfg"

    STALE_CACHE_DIRS="$victim"
    TMP_DIR=""
    run cleanup_stale_cache_dirs

    [ -d "$victim" ]
    [ -f "$victim/printer.cfg" ]
    echo "$output" | grep -qi 'refus'
}

@test "k2 declares the leaked scratch dirs for cleanup" {
    run bash -c "grep -A 28 '\"k2\"' '$PLATFORM_SH' | grep STALE_CACHE_DIRS"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'helixscreen-install'
}

@test "cleanup mixes cache and scratch entries in one declaration" {
    local c="$BATS_TEST_TMPDIR/usr/data/helixscreen/cache"
    local s="$BATS_TEST_TMPDIR/usr/data/helixscreen-install"
    mkdir -p "$c" "$s"

    STALE_CACHE_DIRS="$c $s"
    TMP_DIR=""
    cleanup_stale_cache_dirs

    [ ! -d "$c" ]
    [ ! -d "$s" ]
}
