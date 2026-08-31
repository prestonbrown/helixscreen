#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# helixconfig.json.backup is the pre-settings.json rolling backup, superseded but
# still read as the lowest-priority entry in Config::init's restore chain. Uninstall
# took it along with the whole state dir; install never swept it, so it sat on the
# smallest partition on the box forever. Retiring it is only safe once the current
# backup exists beside it -- otherwise it is the last copy of the user's settings.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    export SUDO=""
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"

    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }

    export HELIX_STATE_VAR_LIB="$BATS_TEST_TMPDIR/var-lib"
    export HELIX_STATE_ROOT_HOME="$BATS_TEST_TMPDIR/root-home"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/klipper"
    mkdir -p "$HELIX_STATE_VAR_LIB" "$HELIX_STATE_ROOT_HOME" "$KLIPPER_HOME/.helixscreen"
}

seed() {  # <dir> <files...>
    local dir=$1; shift
    mkdir -p "$dir"
    local f; for f in "$@"; do echo "{}" > "$dir/$f"; done
}

@test "retires the legacy backup when the current one exists beside it" {
    seed "$HELIX_STATE_VAR_LIB" helixconfig.json.backup settings.json.backup

    run retire_legacy_config_backups
    [ "$status" -eq 0 ]
    [ ! -f "$HELIX_STATE_VAR_LIB/helixconfig.json.backup" ]
    # The current backup is never touched.
    [ -f "$HELIX_STATE_VAR_LIB/settings.json.backup" ]
}

@test "KEEPS the legacy backup when it is the only one -- it is the last copy" {
    seed "$HELIX_STATE_VAR_LIB" helixconfig.json.backup

    run retire_legacy_config_backups
    [ "$status" -eq 0 ]
    [ -f "$HELIX_STATE_VAR_LIB/helixconfig.json.backup" ]
}

@test "sweeps every tier, not just the systemd one" {
    seed "$HELIX_STATE_VAR_LIB"   helixconfig.json.backup settings.json.backup
    seed "$HELIX_STATE_ROOT_HOME" helixconfig.json.backup settings.json.backup
    seed "$KLIPPER_HOME/.helixscreen" helixconfig.json.backup settings.json.backup

    run retire_legacy_config_backups
    [ "$status" -eq 0 ]
    [ ! -f "$HELIX_STATE_VAR_LIB/helixconfig.json.backup" ]
    [ ! -f "$HELIX_STATE_ROOT_HOME/helixconfig.json.backup" ]
    [ ! -f "$KLIPPER_HOME/.helixscreen/helixconfig.json.backup" ]
}

@test "each tier is judged on its own -- a bare tier survives beside a retired one" {
    seed "$HELIX_STATE_VAR_LIB"   helixconfig.json.backup settings.json.backup
    seed "$HELIX_STATE_ROOT_HOME" helixconfig.json.backup

    run retire_legacy_config_backups
    [ "$status" -eq 0 ]
    [ ! -f "$HELIX_STATE_VAR_LIB/helixconfig.json.backup" ]
    [ -f "$HELIX_STATE_ROOT_HOME/helixconfig.json.backup" ]
}

@test "never touches the env backup" {
    seed "$HELIX_STATE_VAR_LIB" helixconfig.json.backup settings.json.backup helixscreen.env.backup

    run retire_legacy_config_backups
    [ "$status" -eq 0 ]
    [ -f "$HELIX_STATE_VAR_LIB/helixscreen.env.backup" ]
}

@test "no-op on a clean machine with no state dirs at all" {
    export HELIX_STATE_VAR_LIB="$BATS_TEST_TMPDIR/absent-a"
    export HELIX_STATE_ROOT_HOME="$BATS_TEST_TMPDIR/absent-b"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/absent-c"

    run retire_legacy_config_backups
    [ "$status" -eq 0 ]
}

@test "the install path actually calls it" {
    grep -q 'retire_legacy_config_backups' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    # And it survives into the bundle users curl.
    grep -q 'retire_legacy_config_backups()' "$WORKTREE_ROOT/scripts/install.sh"
}
