#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# detect_k1_firmware decides whether the installer manages the stock Creality
# stack (stock_klipper, guilouz) or leaves it to Simple AF (simple_af). Only
# the Simple AF tree proves Simple AF: the Guilouz Helper Script installs the
# same GuppyScreen paths while leaving the stock stack in place, so a Helper
# Script printer read as simple_af gets S99start_app de-executed and no
# S99creality-backend.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export MOCK_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$MOCK_ROOT/usr/data" "$MOCK_ROOT/etc/init.d"

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export DISABLED_SERVICES_FILE="$INSTALL_DIR/config/.disabled_services"

    KLIPPER_USER="" KLIPPER_HOME="" INIT_SCRIPT_DEST="" PREVIOUS_UI_SCRIPT=""
    AD5M_FIRMWARE="" K1_FIRMWARE="" platform="k1" INIT_SYSTEM="sysv"
    SERVICE_NAME="helixscreen" HOST_OWNS_COMPETING_UIS=""

    # The production modules with their absolute paths redirected into
    # MOCK_ROOT, so the real detector and the real stop loop run against a
    # fake root.
    local platform_sh="$BATS_TEST_TMPDIR/platform.sh"
    sed -e "s|/usr/data/|$MOCK_ROOT/usr/data/|g" \
        -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/platform.sh" > "$platform_sh"
    local competing="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/opt/|$MOCK_ROOT/opt/|g" \
        -e "s|/home/|$MOCK_ROOT/home/|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh" > "$competing"
    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED \
          _HELIX_COMPETING_UIS_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    # shellcheck disable=SC1090
    . "$platform_sh"
    # shellcheck disable=SC1090
    . "$competing"
    load helpers

    mock_command_script pidof 'exit 1'
    kill_process_by_name() { return 1; }
    ensure_k1_ssh() { :; }
    _is_self_update() { return 1; }
    sleep() { :; }
    export -f kill_process_by_name ensure_k1_ssh _is_self_update sleep
}

# The GuppyScreen footprint both mods leave: the tree and its init script.
write_guppyscreen() {
    mkdir -p "$MOCK_ROOT/usr/data/guppyscreen"
    printf '#!/bin/sh\nexit 0\n' > "$MOCK_ROOT/etc/init.d/S99guppyscreen"
    chmod +x "$MOCK_ROOT/etc/init.d/S99guppyscreen"
}

write_stock_start_app() {
    printf '#!/bin/sh\nexit 0\n' > "$MOCK_ROOT/etc/init.d/S99start_app"
    chmod +x "$MOCK_ROOT/etc/init.d/S99start_app"
}

# --- classification ---

@test "k1 firmware: the Simple AF tree alone is simple_af" {
    mkdir -p "$MOCK_ROOT/usr/data/pellcorp"
    [ "$(detect_k1_firmware)" = "simple_af" ]
}

@test "k1 firmware: Simple AF with its GuppyScreen is simple_af" {
    mkdir -p "$MOCK_ROOT/usr/data/pellcorp"
    write_guppyscreen
    [ "$(detect_k1_firmware)" = "simple_af" ]
}

@test "k1 firmware: Helper Script with GuppyScreen is guilouz, not simple_af" {
    mkdir -p "$MOCK_ROOT/usr/data/helper-script"
    write_guppyscreen
    [ "$(detect_k1_firmware)" = "guilouz" ]
}

@test "k1 firmware: Helper Script alone is guilouz" {
    mkdir -p "$MOCK_ROOT/usr/data/helper-script"
    [ "$(detect_k1_firmware)" = "guilouz" ]
}

@test "k1 firmware: GuppyScreen with neither mod tree is stock_klipper" {
    write_guppyscreen
    [ "$(detect_k1_firmware)" = "stock_klipper" ]
}

@test "k1 firmware: no markers is stock_klipper" {
    [ "$(detect_k1_firmware)" = "stock_klipper" ]
}

@test "k1 firmware: the Simple AF tree outranks a leftover Helper Script tree" {
    mkdir -p "$MOCK_ROOT/usr/data/pellcorp" "$MOCK_ROOT/usr/data/helper-script"
    [ "$(detect_k1_firmware)" = "simple_af" ]
}

# --- what the verdict drives on a Helper Script + GuppyScreen printer ---

@test "k1 guilouz: the stop step takes down GuppyScreen and the stock UI, records both" {
    mkdir -p "$MOCK_ROOT/usr/data/helper-script"
    write_guppyscreen
    write_stock_start_app
    K1_FIRMWARE=$(detect_k1_firmware)
    set_install_paths "k1" "$K1_FIRMWARE"
    [ -z "$PREVIOUS_UI_SCRIPT" ]
    # set_install_paths moved INSTALL_DIR under the mock root; the ledger follows it.
    DISABLED_SERVICES_FILE="$INSTALL_DIR/config/.disabled_services"
    mkdir -p "$INSTALL_DIR/config"

    run stop_competing_uis
    [ "$status" -eq 0 ]

    [ ! -x "$MOCK_ROOT/etc/init.d/S99guppyscreen" ]
    grep -qF "sysv-chmod:$MOCK_ROOT/etc/init.d/S99guppyscreen" "$DISABLED_SERVICES_FILE"
    [ ! -x "$MOCK_ROOT/etc/init.d/S99start_app" ]
    grep -qF "sysv-chmod:$MOCK_ROOT/etc/init.d/S99start_app" "$DISABLED_SERVICES_FILE"
}

@test "k1 guilouz: the backend init script is installed" {
    mkdir -p "$MOCK_ROOT/usr/data/helper-script"
    write_guppyscreen
    K1_FIRMWARE=$(detect_k1_firmware)
    printf '#!/bin/sh\nexit 0\n' > "$INSTALL_DIR/config/creality-backend.init"

    run install_k1_creality_backend
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/etc/init.d/S99creality-backend" ]
    grep -qF "sysv-created:$MOCK_ROOT/etc/init.d/S99creality-backend" "$DISABLED_SERVICES_FILE"
}

@test "k1 simple_af: the backend init script stays out" {
    mkdir -p "$MOCK_ROOT/usr/data/pellcorp"
    write_guppyscreen
    K1_FIRMWARE=$(detect_k1_firmware)
    printf '#!/bin/sh\nexit 0\n' > "$INSTALL_DIR/config/creality-backend.init"

    run install_k1_creality_backend
    [ "$status" -eq 0 ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99creality-backend" ]
}
