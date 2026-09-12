#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The uninstall contract on K1: uninstall must leave the stock boot UI
# executable (prestonbrown/helixscreen#1618). The disabled-services ledger is
# the only path that chmod +x's S99start_app back — no PREVIOUS_UIS scan
# fallback names it — so these tests cover the states where the ledger is the
# install's to adopt rather than merely its own fresh write:
#   - an inherited disable (S99start_app already de-executed when the install
#     lands) must be recorded, or a later uninstall restores nothing
#   - a --clean cycle must carry the ledger across its user-config wipe, or
#     the /etc chmods it records outlive their own record
# The end-to-end cases run the standalone bundle the way the fleet did: a
# copied script, --force, against a redirected mock root.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
MODULE="$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export DISABLED_SERVICES_FILE="$INSTALL_DIR/config/.disabled_services"

    export MOCK_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/usr/bin"

    detect_init_system() { INIT_SYSTEM="sysv"; }
    export -f detect_init_system
    INIT_SYSTEM="sysv"
    K1_FIRMWARE="stock_klipper"
    platform="k1"
    PREVIOUS_UI_SCRIPT=""
    SERVICE_NAME="helixscreen"

    # Source the production module with its hardcoded /etc/init.d paths
    # redirected into MOCK_ROOT, so the real functions run against a fake
    # root (same harness as test_k1_creality_backend.bats).
    local patched="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -e "s|/etc/init.d|$MOCK_ROOT/etc/init.d|g" \
        "$MODULE" > "$patched"
    unset _HELIX_COMPETING_UIS_SOURCED
    # shellcheck disable=SC1090
    . "$patched"

    kill_process_by_name() { return 1; }
    export -f kill_process_by_name
    ensure_k1_ssh() { :; }
    export -f ensure_k1_ssh
}

# Write a stock S99start_app with the given mode into the mock root.
write_start_app() {
    local s="$MOCK_ROOT/etc/init.d/S99start_app"
    printf '#!/bin/sh\nexit 0\n' > "$s"
    chmod "$1" "$s"
}

# --- install side: adopting an inherited disable ---

@test "k1 install records an inherited S99start_app disable" {
    write_start_app 644   # de-executed before this install landed
    [ ! -x "$MOCK_ROOT/etc/init.d/S99start_app" ]

    found_any=false
    run stop_k1_stock_competing_uis
    [ "$status" -eq 0 ]

    [ ! -x "$MOCK_ROOT/etc/init.d/S99start_app" ]
    grep -qF "sysv-chmod:$MOCK_ROOT/etc/init.d/S99start_app" "$DISABLED_SERVICES_FILE"
}

@test "k1 install still stops and records an executable S99start_app" {
    write_start_app 755

    found_any=false
    run stop_k1_stock_competing_uis
    [ "$status" -eq 0 ]

    [ ! -x "$MOCK_ROOT/etc/init.d/S99start_app" ]
    grep -qF "sysv-chmod:$MOCK_ROOT/etc/init.d/S99start_app" "$DISABLED_SERVICES_FILE"
}

# --- end to end: the copied-script uninstaller against a mock K1 ---

# A K1 the bundle's own probes will recognize: buildroot os-release, /usr/data,
# and two K1 indicators, plus an installed payload whose config/.disabled_services
# is the printer_data per-file symlink (HELIX_USER_CONFIG_FILES layout).
build_mock_k1() {
    mkdir -p "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/usr/data/printer_data/config/helixscreen" \
             "$MOCK_ROOT/usr/data/pellcorp" "$MOCK_ROOT/usr/data/helixscreen/config" \
             "$MOCK_ROOT/run"
    printf 'ID=buildroot\n' > "$MOCK_ROOT/etc/os-release"
    printf '#!/bin/sh\nexit 0\n' > "$MOCK_ROOT/etc/init.d/S99helixscreen"
    chmod +x "$MOCK_ROOT/etc/init.d/S99helixscreen"
}

# Point the payload's ledger at the printer_data real file, the shipped layout.
link_ledger_into_payload() {
    mv "$MOCK_ROOT/usr/data/helixscreen/config/.disabled_services" \
       "$MOCK_ROOT/usr/data/printer_data/config/helixscreen/.disabled_services"
    ln -s "$MOCK_ROOT/usr/data/printer_data/config/helixscreen/.disabled_services" \
          "$MOCK_ROOT/usr/data/helixscreen/config/.disabled_services"
}

# Regenerate the standalone uninstaller from the worktree, redirect every
# absolute path it probes into MOCK_ROOT, and run it from a copied path with
# --force — the fleet invocation from the issue.
run_copied_uninstaller() {
    local gen="$BATS_TEST_TMPDIR/uninstall-gen.sh"
    "$WORKTREE_ROOT/scripts/bundle-uninstaller.sh" -o "$gen" >/dev/null

    local copy_dir="$BATS_TEST_TMPDIR/uninstall-copy"
    mkdir -p "$copy_dir"
    sed -e "s|/etc/init.d|$MOCK_ROOT/etc/init.d|g" \
        -e "s|/usr/data|$MOCK_ROOT/usr/data|g" \
        -e "s|/etc/os-release|$MOCK_ROOT/etc/os-release|g" \
        -e "s|/run/systemd/system|$MOCK_ROOT/run/systemd/system|g" \
        "$gen" > "$copy_dir/uninstall.sh"

    # k1 requires root (check_permissions) and the process sweep needs pidof.
    mock_command_script "id" "echo 0"
    mock_command_script "pidof" "exit 1"

    export HELIX_STATE_VAR_LIB="$BATS_TEST_TMPDIR/var/lib/helixscreen"
    export HELIX_STATE_ROOT_HOME="$BATS_TEST_TMPDIR/root/.helixscreen"
    run sh "$copy_dir/uninstall.sh" --force
}

@test "uninstaller from a copied script restores an inherited S99start_app disable the install adopted" {
    build_mock_k1
    write_start_app 644   # inherited: de-executed before this install
    [ ! -x "$MOCK_ROOT/etc/init.d/S99start_app" ]

    # The install half, against the payload the uninstaller will sweep.
    INSTALL_DIR="$MOCK_ROOT/usr/data/helixscreen"
    found_any=false
    stop_k1_stock_competing_uis
    [ -f "$INSTALL_DIR/config/.disabled_services" ]
    link_ledger_into_payload

    run_copied_uninstaller
    [ "$status" -eq 0 ]
    contains "HelixScreen has been removed" "$output"

    [ -x "$MOCK_ROOT/etc/init.d/S99start_app" ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99helixscreen" ]
    [ ! -d "$MOCK_ROOT/usr/data/helixscreen" ]
}

@test "uninstaller from a copied script restores the stock UI the install itself disabled" {
    build_mock_k1
    write_start_app 755

    INSTALL_DIR="$MOCK_ROOT/usr/data/helixscreen"
    found_any=false
    stop_k1_stock_competing_uis
    [ ! -x "$MOCK_ROOT/etc/init.d/S99start_app" ]
    link_ledger_into_payload

    run_copied_uninstaller
    [ "$status" -eq 0 ]

    [ -x "$MOCK_ROOT/etc/init.d/S99start_app" ]
}

@test "uninstaller output matches the fleet report when nothing re-enables S99start_app" {
    # With no ledger entry the uninstaller completes "successfully" and
    # reports no UI found — the silence is the trap. The inherited-disable
    # adoption above is what keeps this state from arising; this test pins
    # that the uninstaller itself never re-enables an unrecorded target.
    build_mock_k1
    write_start_app 644
    # No install half: no ledger anywhere.

    run_copied_uninstaller
    [ "$status" -eq 0 ]
    contains "No previous screen UI found to re-enable" "$output"
    [ ! -x "$MOCK_ROOT/etc/init.d/S99start_app" ]
}

# --- --clean: the ledger rides out the user-config wipe ---

@test "clean install mode wipes user config but carries the disabled-services ledger" {
    unset _HELIX_UNINSTALL_SOURCED _HELIX_COMMON_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"

    local pd_config="$BATS_TEST_TMPDIR/printer_data/config"
    local pd_helix="$pd_config/helixscreen"
    mkdir -p "$pd_helix"
    echo '{"theme":"dark"}' > "$pd_helix/settings.json"
    echo "sysv-chmod:$MOCK_ROOT/etc/init.d/S99start_app" > "$pd_helix/.disabled_services"

    stop_service() { :; }
    export -f stop_service
    host_mod_destruct_blocked() { return 1; }
    export -f host_mod_destruct_blocked
    # Echoes the exported tmpdir, not a local: an exported function runs
    # without the test body's locals in scope.
    klipper_config_dir() { echo "$BATS_TEST_TMPDIR/printer_data/config"; }
    export -f klipper_config_dir
    ASSUME_YES=true
    HELIX_INSTALL_DIRS=""
    HELIX_INIT_SCRIPTS=""
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR"
    export HELIX_STATE_VAR_LIB="$BATS_TEST_TMPDIR/var/lib/helixscreen"
    export HELIX_STATE_ROOT_HOME="$BATS_TEST_TMPDIR/root/.helixscreen"
    install_sudo_rm_shim "$BATS_TEST_TMPDIR"

    run clean_old_installation k1
    [ "$status" -eq 0 ]

    [ ! -f "$pd_helix/settings.json" ]
    [ -f "$pd_helix/.disabled_services" ]
    grep -qF "sysv-chmod:$MOCK_ROOT/etc/init.d/S99start_app" "$pd_helix/.disabled_services"
}
