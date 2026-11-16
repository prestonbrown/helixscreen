#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# install.sh --source: the channel that installed this copy, kept as one
# allowlisted word in config/.install_source for the opt-in telemetry.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB="$WORKTREE_ROOT/scripts/lib/installer"

setup() {
    load helpers
    local bats_traps
    bats_traps="$(trap -p EXIT INT TERM HUP)"
    for m in common logo host_profile platform permissions requirements competing_uis desktop release service moonraker kiauh printer_seed plan uninstall main; do
        . "$LIB/$m.sh"
    done
    trap - ERR EXIT INT TERM HUP
    eval "$bats_traps"
    export SUDO=""
    INSTALL_DIR="$BATS_TEST_TMPDIR/install"
    mkdir -p "$INSTALL_DIR/config"
    MARKER="$INSTALL_DIR/config/.install_source"
    update_mode=false
    INSTALL_SOURCE_ARG=""
}

@test "every installer source is on the app's and the worker's allowlists" {
    run install_source_allowlist
    [ "$status" -eq 0 ]
    [ -n "$output" ]
    for s in $output; do
        grep -q "\"$s\"" "$WORKTREE_ROOT/src/system/install_provenance.cpp"
        grep -q "\"$s\"" "$WORKTREE_ROOT/server/telemetry-worker/src/schema.ts"
    done
}

@test "--source keeps a listed name, maps any other to other, and requires a value" {
    parse_installer_args --source kiauh
    [ "$INSTALL_SOURCE_ARG" = kiauh ]
    parse_installer_args --source '/home/alice/my script'
    [ "$INSTALL_SOURCE_ARG" = other ]
    parse_installer_args --source KIAUH
    [ "$INSTALL_SOURCE_ARG" = other ]
    run parse_installer_args --source
    [ "$status" -ne 0 ]
    [[ "$output" == *"--source requires"* ]]
}

@test "a fresh install writes the named source, and curl when none is named" {
    INSTALL_SOURCE_ARG=helper-script
    record_install_source
    [ "$(cat "$MARKER")" = helper-script ]

    rm "$MARKER"
    INSTALL_SOURCE_ARG=""
    record_install_source
    [ "$(cat "$MARKER")" = curl ]
}

@test "--update keeps the marker, even against another --source" {
    printf 'kiauh\n' > "$MARKER"
    update_mode=true
    record_install_source
    [ "$(cat "$MARKER")" = kiauh ]
    INSTALL_SOURCE_ARG=curl
    record_install_source
    [ "$(cat "$MARKER")" = kiauh ]
}

@test "--update on an install with no marker does not guess one" {
    update_mode=true
    record_install_source
    [ ! -e "$MARKER" ]
}

@test "--update with a named source fills in a missing or invalid marker" {
    update_mode=true
    INSTALL_SOURCE_ARG=kiauh
    record_install_source
    [ "$(cat "$MARKER")" = kiauh ]
    printf 'not-a-source\n' > "$MARKER"
    INSTALL_SOURCE_ARG=curl
    record_install_source
    [ "$(cat "$MARKER")" = curl ]
}

@test "a reinstall keeps the marker unless it names a source" {
    printf 'kiauh\n' > "$MARKER"
    record_install_source
    [ "$(cat "$MARKER")" = kiauh ]
    INSTALL_SOURCE_ARG=helix-installer
    record_install_source
    [ "$(cat "$MARKER")" = helix-installer ]
}

@test "the write follows a marker relocated into printer_data" {
    local pd="$BATS_TEST_TMPDIR/printer_data_install_source"
    printf 'not-a-source\n' > "$pd"
    ln -s "$pd" "$MARKER"
    INSTALL_SOURCE_ARG=kiauh
    record_install_source
    [ -L "$MARKER" ]
    [ "$(cat "$pd")" = kiauh ]
}

@test "the marker is relocated with the install's other user config" {
    case " $HELIX_USER_CONFIG_FILES " in
        *" .install_source "*) ;;
        *) false ;;
    esac
}

@test "an unwritable config dir warns and does not fail the install" {
    INSTALL_DIR="$BATS_TEST_TMPDIR/does-not-exist"
    INSTALL_SOURCE_ARG=kiauh
    run record_install_source
    [ "$status" -eq 0 ]
    [ ! -e "$INSTALL_DIR/config/.install_source" ]
}
