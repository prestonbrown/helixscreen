#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A platform hook that names an unwritable cache root does not fail loudly. The
# cascade in helix_cache_dir.cpp skips any rung it cannot create and keeps going,
# so the cache silently lands several rungs down - on /tmp, which is tmpfs on
# every embedded target in the fleet. The app starts, the UI works, and the only
# symptom is memory pressure on the box least able to absorb it.
#
# CC1 is the worst case and the reason this file exists: 117 MB of RAM total,
# / is a read-only squashfs with no /opt at all, and the thumbnail cache alone
# is allowed 20 MB. These tests pin the writable partition for the platforms
# whose storage is fixed, and pin the absence of an override for the ones whose
# install root is discovered at runtime.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOKS_DIR="$WORKTREE_ROOT/assets/config/platform"
CACHE_DIR_CPP="$WORKTREE_ROOT/src/system/helix_cache_dir.cpp"

setup() {
    load helpers
}

# The value a hook exports for HELIX_CACHE_DIR, or empty when it sets none.
hook_cache_dir() {
    sed -n 's/.*export HELIX_CACHE_DIR="\([^"]*\)".*/\1/p' "$HOOKS_DIR/hooks-$1.sh"
}

# --------------------------------------------------------------------------
# CC1: read-only squashfs, no /opt, 117 MB RAM
# --------------------------------------------------------------------------

@test "cc1 caches on the writable ext4 partition" {
    run hook_cache_dir cc1
    [ "$status" -eq 0 ] || fail "could not read hooks-cc1.sh"
    [ "$output" = "/user-resource/helixscreen-state/cache" ] || \
        fail "cc1 cache is '$output'; /user-resource is the only writable bulk storage"
}

@test "cc1 does not cache under /opt, which does not exist on the device" {
    run hook_cache_dir cc1
    lacks "/opt/" "$output"
}

@test "cc1 compile-time rung agrees with its hook" {
    # The hook wins at runtime, but the compile-time rung is what a build with
    # no hooks deployed falls back to, so a disagreement here is a second copy
    # of the same fact drifting on its own.
    local from_hook
    from_hook="$(hook_cache_dir cc1)"
    run grep -A8 'defined(HELIX_PLATFORM_CC1)' "$CACHE_DIR_CPP"
    [ "$status" -eq 0 ] || fail "no CC1 rung in helix_cache_dir.cpp"
    contains "$from_hook" "$output"
}

@test "cc1 log and cache both land on the same writable partition" {
    run grep -E 'HELIX_(CACHE_DIR|LOG_FILE)=' "$HOOKS_DIR/hooks-cc1.sh"
    [ "$status" -eq 0 ] || fail "cc1 hook sets neither cache nor log"
    # Two separate decisions that must reach the same answer: the log was
    # reasoned onto flash and the cache has to be there too.
    local n
    n="$(echo "$output" | grep -c '/user-resource/')"
    [ "$n" -eq 2 ] || fail "expected cache and log both under /user-resource, got: $output"
}

# --------------------------------------------------------------------------
# Discovered install roots: an override can only be wrong
# --------------------------------------------------------------------------

@test "m1 sets no cache override" {
    run hook_cache_dir m1
    [ -z "$output" ] || \
        fail "m1 pins the cache to '$output'; it is a Debian SBC with a discovered install root"
}

@test "m1 does not name a Creality buildroot path" {
    run cat "$HOOKS_DIR/hooks-m1.sh"
    [ "$status" -eq 0 ] || fail "could not read hooks-m1.sh"
    lacks "/usr/data" "$output"
}

@test "pi sets no cache override" {
    run hook_cache_dir pi
    [ -z "$output" ] || fail "pi pins the cache to '$output'"
}

# --------------------------------------------------------------------------
# Every hook's cache, if pinned, is on that platform's own storage
# --------------------------------------------------------------------------

@test "no hook caches under a path another platform owns" {
    # /opt is real on the Pi and on AD5M Forge-X, and absent or read-only on
    # CC1. A hook naming a root its own device does not have is the shape of
    # this whole class of bug, so check each one against its own install tree.
    local failures=""
    for f in "$HOOKS_DIR"/hooks-*.sh; do
        local plat dir
        plat="$(basename "$f" .sh)"
        plat="${plat#hooks-}"
        dir="$(hook_cache_dir "$plat")"
        [ -n "$dir" ] || continue
        # Either beside the payload (<mount>/helixscreen-state/cache) or on a
        # different mount that carries the platform's durable state.
        case "$dir" in
            /*/helixscreen-state/cache|/*/*/helixscreen-state/cache) ;;
            /*/helixscreen/cache|/*/*/helixscreen/cache) ;;
            /*/mod_data/helixscreen/cache) ;;
            *) failures="$failures $plat:$dir" ;;
        esac
    done
    [ -z "$failures" ] || fail "cache roots outside the expected shape:$failures"
}

# --------------------------------------------------------------------------
# The install-root consumers must READ the shared list, not restate it
# --------------------------------------------------------------------------
#
# test_install_roots.cpp proves helix::kInstallRoots covers every platform the
# manifest declares. It cannot prove anything about who reads it, so reverting a
# consumer back to its own hand-written literals leaves that test green and
# re-opens the bug: a debug bundle from a CC1 or U1 with crash.txt silently
# absent. These cases pin the wiring.

CONSUMERS="src/system/log_collector.cpp src/system/debug_bundle_collector.cpp src/system/update_checker.cpp"

@test "every install-root consumer reads the shared list" {
    for f in $CONSUMERS; do
        run grep -c "kInstallRoots" "$f"
        [ "$output" != "0" ] || fail "$f no longer reads helix::kInstallRoots"
    done
}

@test "no consumer has re-grown its own root literals" {
    # These three roots exist ONLY in the shared header. A consumer spelling one
    # itself is a hand-kept list coming back, which is how the gap opened.
    for f in $CONSUMERS; do
        for root in "/srv/helixscreen" "/userdata/helixscreen" "/user-resource/helixscreen"; do
            run grep -c -- "\"$root" "$f"
            [ "$output" = "0" ] || fail "$f spells $root itself; it should come from kInstallRoots"
        done
    done
}

@test "the shared list is the only place the roots are written" {
    run grep -c -- '"/userdata/helixscreen"' include/helix_install_roots.h
    [ "$output" != "0" ] || fail "the shared header no longer carries the Snapmaker U1 root"
}

# --------------------------------------------------------------------------
# State lives beside the payload, never inside it
# --------------------------------------------------------------------------
#
# The install root is what an update replaces, and not only by our own hand: a
# Moonraker `type: web` entry does shutil.rmtree(path) before extracting, and
# that path IS the install root. Cache and logs kept under it are destroyed on
# every update - logs vanish exactly when someone needs them, and the thumbnail
# cache is rebuilt from nothing.

@test "no hook puts cache or logs inside that platform's install root" {
    # The roots each hook's platform installs to, from set_install_paths.
    declare -A ROOT=(
        [k1]=/usr/data/helixscreen
        [k2]=/opt/helixscreen
        [cc1]=/user-resource/helixscreen
        [ad5x]=/srv/helixscreen
        [snapmaker-u1]=/userdata/helixscreen
    )
    local failures=""
    for plat in "${!ROOT[@]}"; do
        local f="$HOOKS_DIR/hooks-$plat.sh"
        [ -f "$f" ] || continue
        local root="${ROOT[$plat]}"
        while read -r path; do
            [ -n "$path" ] || continue
            case "$path" in
                "$root"/*|"$root") failures="$failures $plat:$path" ;;
            esac
        done <<< "$(sed -n 's/.*export HELIX_\(CACHE_DIR\|LOG_FILE\)="\([^"]*\)".*/\2/p' "$f")"
    done
    [ -z "$failures" ] || fail "state inside the payload, which updates delete:$failures"
}

@test "k1 keeps its cache and logs off the payload" {
    run hook_cache_dir k1
    [ "$output" = "/usr/data/helixscreen-state/cache" ] || fail "k1 cache is '$output'"
    run grep -c 'HELIX_LOG_FILE="/usr/data/helixscreen-state/logs/helix.log"' "$HOOKS_DIR/hooks-k1.sh"
    [ "$output" = "1" ] || fail "k1 log is not on the state tree"
}

@test "cc1 keeps its cache and logs off the payload" {
    run hook_cache_dir cc1
    [ "$output" = "/user-resource/helixscreen-state/cache" ] || fail "cc1 cache is '$output'"
}

@test "ad5x keeps its log where the mod archiver looks, not on the state tree" {
    # ZMOD's TAR_CONFIG collects /opt/config/ and never /data or /srv, so this
    # one is deliberately NOT co-located with the cache.
    run grep -c 'HELIX_LOG_FILE="/opt/config/mod_data/log/helix.log"' "$HOOKS_DIR/hooks-ad5x.sh"
    [ "$output" = "1" ] || fail "ad5x log moved off the mod's archive path"
    run hook_cache_dir ad5x
    [ "$output" = "/srv/helixscreen-state/cache" ] || fail "ad5x cache is '$output'"
}

@test "a log tail can still find the moved logs" {
    # Moving the logs without teaching the search is how a debug bundle ends up
    # with the field silently absent.
    for d in /usr/data/helixscreen-state /user-resource/helixscreen-state \
             /userdata/helixscreen-state /srv/helixscreen-state; do
        run grep -c -- "\"$d\"" "$WORKTREE_ROOT/include/helix_install_roots.h"
        [ "$output" != "0" ] || fail "$d is not in kStateRoots; its logs are unreachable"
    done
}

@test "uninstall sweeps the state dirs and the legacy in-payload ones" {
    run bash -c "export SUDO=''; . '$WORKTREE_ROOT/scripts/lib/installer/common.sh'; helix_state_sweep_paths"
    [ "$status" -eq 0 ] || fail "helix_state_sweep_paths failed: $output"
    contains "/usr/data/helixscreen-state/cache" "$output"
    contains "/usr/data/helixscreen-state/logs" "$output"
    # The legacy location an older install still has must stay swept, or an
    # upgrade-then-uninstall leaves it behind.
    contains "/usr/data/helixscreen/cache" "$output"
    contains "/user-resource/helixscreen/cache" "$output"
}
