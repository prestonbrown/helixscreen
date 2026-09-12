#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# An install already on disk outranks the platform default.
#
# The per-platform roots in set_install_paths are defaults for a FIRST install.
# Once a tree exists, choosing a different root orphans it along with the user
# config inside it, so what is on disk decides.
#
# The one exception is a root the platform declares superseded in
# storage.previous_root, which is migrated rather than adopted;
# tests/shell/test_k2_root_migration.bats covers that path.
#
# detect_pi_install_dir has always done this for the Pi branch. These cases pin
# it for the embedded platforms, which are the ones whose roots a relocation
# would actually move.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    unset INSTALL_DIR _USER_INSTALL_DIR HOST_INSTALL_ROOT STANDALONE_INSTALL
    export SUDO=""
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }

    FAKE_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$FAKE_ROOT"
}

# Put an executable helix-screen at <root>, and point the known-dirs list at the
# sandbox so the probe never sees the real machine.
seed_install() {
    local dir="$FAKE_ROOT$1"
    mkdir -p "$dir/bin"
    printf '#!/bin/sh\nexit 0\n' > "$dir/bin/helix-screen"
    chmod +x "$dir/bin/helix-screen"
    printf '%s\n' "$dir"
}

# Rebind the probe to the sandbox: the real list holds absolute device paths.
use_sandbox_dirs() {
    _HELIX_KNOWN_INSTALL_DIRS=""
    for d in /opt/helixscreen /srv/helixscreen /usr/data/helixscreen \
             /user-resource/helixscreen /userdata/helixscreen \
             /mnt/UDISK/helixscreen; do
        _HELIX_KNOWN_INSTALL_DIRS="$_HELIX_KNOWN_INSTALL_DIRS $FAKE_ROOT$d"
    done
}

@test "k2: an existing install at a non-default root wins over the default" {
    use_sandbox_dirs
    local existing
    existing="$(seed_install /mnt/UDISK/helixscreen)"

    set_install_paths k2
    [ "$INSTALL_DIR" = "$existing" ] || \
        fail "k2 chose '$INSTALL_DIR'; the tree on disk is at '$existing'"
}

@test "k2: with nothing on disk the platform default stands" {
    use_sandbox_dirs
    set_install_paths k2
    [ "$INSTALL_DIR" = "/mnt/UDISK/helixscreen" ] || \
        fail "k2 first install chose '$INSTALL_DIR', expected the platform default"
}

@test "cc1: an existing install wins" {
    use_sandbox_dirs
    local existing
    existing="$(seed_install /opt/helixscreen)"

    set_install_paths cc1
    [ "$INSTALL_DIR" = "$existing" ] || \
        fail "cc1 chose '$INSTALL_DIR', ignoring the tree at '$existing'"
}

@test "k1: an existing install wins" {
    use_sandbox_dirs
    local existing
    existing="$(seed_install /srv/helixscreen)"

    set_install_paths k1
    [ "$INSTALL_DIR" = "$existing" ] || fail "k1 chose '$INSTALL_DIR'"
}

@test "snapmaker-u1: the init script follows the tree it adopted" {
    # U1 is the only platform whose init script lives inside the install tree,
    # so a stale INIT_SCRIPT_DEST would point the service at the orphan.
    use_sandbox_dirs
    local existing
    existing="$(seed_install /opt/helixscreen)"

    set_install_paths snapmaker-u1
    [ "$INSTALL_DIR" = "$existing" ] || fail "u1 chose '$INSTALL_DIR'"
    [ "$INIT_SCRIPT_DEST" = "$existing/config/helixscreen.init" ] || \
        fail "u1 init script is '$INIT_SCRIPT_DEST', not inside the adopted tree"
}

@test "an explicit INSTALL_DIR outranks a tree on disk" {
    use_sandbox_dirs
    seed_install /srv/helixscreen >/dev/null
    _USER_INSTALL_DIR="$FAKE_ROOT/custom/helixscreen"

    set_install_paths k2
    [ "$INSTALL_DIR" != "$FAKE_ROOT/srv/helixscreen" ] || \
        fail "an explicit request was overridden by a tree found on disk"
}

@test "a directory without the binary is not an install" {
    # The probe requires an executable bin/helix-screen. An empty directory left
    # by a failed run or a stale mount must not capture the install.
    use_sandbox_dirs
    mkdir -p "$FAKE_ROOT/srv/helixscreen/bin"

    set_install_paths k2
    [ "$INSTALL_DIR" = "/mnt/UDISK/helixscreen" ] || \
        fail "an empty directory was treated as an install: '$INSTALL_DIR'"
}

@test "the pi branch keeps doing its own detection exactly once" {
    # detect_pi_install_dir has its own cascade; the embedded check must not
    # run a second time and log a contradicting line.
    use_sandbox_dirs
    export KLIPPER_HOME="$FAKE_ROOT/home/pi"
    mkdir -p "$KLIPPER_HOME"
    local existing
    existing="$(seed_install /opt/helixscreen)"

    run set_install_paths pi
    [ "$status" -eq 0 ] || fail "set_install_paths pi failed: $output"
    local n
    n="$(echo "$output" | grep -c "existing install" || true)"
    [ "$n" -le 1 ] || fail "the existing-install decision was logged $n times"
}
