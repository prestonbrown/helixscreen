#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The mod-owned competing-UI short-circuit (lib/installer/competing_uis.sh).
#
# On a host the mod profile recognized (HOST_OWNS_COMPETING_UIS=1: the probe
# found the mod's git tree), the mod owns its UI lifecycle. Stopping, killing,
# and de-execing its init scripts is the mod's business, not the standalone
# installer's - the generic sweep's chmod a-x on /opt/config/mod/.root/S*
# scripts is exactly the footprint the mod did not ask for. Our display
# takeover lives in configure_forgex_display instead. stop_competing_uis must
# return before ANY of the sweep runs.
#
# The unprobed ZMOD host keeps its own early return: ZMOD manages
# S80guppyscreen and friends (#314), and the probe does not recognize a ZMOD
# tree, so the flavor arm is still what protects it.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim

    # Temporary install directory for the .disabled_services state file
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # Redirect the production loop's absolute /etc/init.d paths at a mock
    # root (same substitution approach as test_sovol_competing_uis.bats) so
    # the plain-host control test below can run the real generic loop safely.
    # /opt/PROGRAM/ is the stock FlashForge UI's home, redirected for the
    # flavor-decoupled stock-UI kill tests below.
    export MOCK_ROOT="$BATS_TEST_TMPDIR/host"
    mkdir -p "$MOCK_ROOT/etc/init.d"

    local patched="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/opt/config/mod/.root/|$MOCK_ROOT/opt/config/mod/.root/|g" \
        -e "s|/opt/PROGRAM/|$MOCK_ROOT/opt/PROGRAM/|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh" > "$patched"
    unset _HELIX_COMPETING_UIS_SOURCED
    # shellcheck disable=SC1090
    . "$patched"

    # Fresh install in every case here, and nothing to kill in the sandbox.
    _is_self_update() { return 1; }
    export -f _is_self_update
    kill_process_by_name() { return 1; }
    export -f kill_process_by_name

    # systemctl reports nothing active: the control test's generic loop must
    # not touch the host's real services.
    mock_command_fail "systemctl"

    INIT_SYSTEM="systemd"
    MOD_FLAVOR=""
    AD5M_FIRMWARE=""
    K1_FIRMWARE=""
    HOST_OWNS_COMPETING_UIS=0
    platform=""
    PREVIOUS_UI_SCRIPT=""
}

# A kill_process_by_name that logs what it was asked to kill instead of
# killing, so a test can prove the REAL stock-UI kill path ran past its file
# guard.
_logging_kill() {
    echo "KILLED:$1"
    return 1
}

@test "mod-owned host: the competing-UI sweep never runs" {
    MOD_FLAVOR="forge_x"
    AD5M_FIRMWARE="forge_x"
    HOST_OWNS_COMPETING_UIS=1
    stop_forgex_competing_uis() { echo "SWEEP-RAN"; }
    stop_kmod_competing_uis() { echo "SWEEP-RAN"; }
    run stop_competing_uis
    [ "$status" -eq 0 ]
    [[ "$output" != *"SWEEP-RAN"* ]]
}

@test "plain host: the flavor handlers still dispatch" {
    # Control: without the probe's ownership answer, the forge_x handler and
    # the generic loop run exactly as before. Guards against over-skipping.
    MOD_FLAVOR="forge_x"
    AD5M_FIRMWARE="forge_x"
    HOST_OWNS_COMPETING_UIS=0
    stop_forgex_competing_uis() { echo "SWEEP-RAN"; }
    run stop_competing_uis
    [ "$status" -eq 0 ]
    [[ "$output" == *"SWEEP-RAN"* ]]
}

@test "unprobed ZMOD host keeps its flavor early-return" {
    # A ZMOD host the probe did not recognize (no forge-x tree to find):
    # the zmod arm must still skip the generic loop (#314).
    MOD_FLAVOR="zmod"
    AD5M_FIRMWARE="zmod"
    HOST_OWNS_COMPETING_UIS=0
    stop_forgex_competing_uis() { echo "SWEEP-RAN"; }
    run stop_competing_uis
    [ "$status" -eq 0 ]
    [[ "$output" != *"SWEEP-RAN"* ]]
}

@test "competing_uis.sh gates the sweep on HOST_OWNS_COMPETING_UIS" {
    grep -qF '[ "$HOST_OWNS_COMPETING_UIS" = "1" ] && return 0' \
        "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh"
}

@test "install.sh (bundled) carries the mod-owned short-circuit" {
    grep -qF '[ "$HOST_OWNS_COMPETING_UIS" = "1" ] && return 0' \
        "$WORKTREE_ROOT/scripts/install.sh"
}

# ============================================================================
# Stock-UI guards are keyed on the stock UI's own files, not the mod flavor.
#
# The forge_x-flavored default used to carry these two along implicitly; with
# the honest `stock` flavor a mod-less AD5M must STILL kill and disable the
# stock FlashForge UI when its files are present - that UI fights us for the
# framebuffer whatever firmware is on the box.
# ============================================================================

# Source configure_platform (main.sh) with forgex.sh's stock-UI file paths
# redirected at MOCK_ROOT. main.sh's source-time traps (ERR/EXIT) are stripped
# from the copy: an ERR trap left armed inside a bats test fires on the first
# failing assertion into an undefined error_handler and swallows bats' result
# line for that test.
_load_configure_platform() {
    local forgex_patched="$BATS_TEST_TMPDIR/forgex.sh"
    sed -e "s|auto_run=\"/opt/auto_run.sh\"|auto_run=\"$MOCK_ROOT/opt/auto_run.sh\"|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh" > "$forgex_patched"
    local main_patched="$BATS_TEST_TMPDIR/main.sh"
    sed -e "/^trap /d" \
        "$WORKTREE_ROOT/scripts/lib/installer/main.sh" > "$main_patched"
    unset _HELIX_FORGEX_SOURCED _HELIX_MAIN_SOURCED
    # shellcheck disable=SC1090
    . "$forgex_patched"
    # shellcheck disable=SC1090
    . "$main_patched"
}

@test "mod-less AD5M (flavor stock): the stock-UI kill fires when ffstartup-arm is present" {
    MOD_FLAVOR="stock"
    AD5M_FIRMWARE="stock"
    HOST_OWNS_COMPETING_UIS=0
    mkdir -p "$MOCK_ROOT/opt/PROGRAM"
    touch "$MOCK_ROOT/opt/PROGRAM/ffstartup-arm"
    kill_process_by_name() { _logging_kill "$@"; }
    export -f kill_process_by_name
    run stop_competing_uis
    [ "$status" -eq 0 ]
    [[ "$output" == *"KILLED:firmwareExe"* ]]
}

@test "mod-less AD5M (flavor stock): no ffstartup-arm, no stock-UI kill" {
    MOD_FLAVOR="stock"
    AD5M_FIRMWARE="stock"
    HOST_OWNS_COMPETING_UIS=0
    kill_process_by_name() { _logging_kill "$@"; }
    export -f kill_process_by_name
    run stop_competing_uis
    [ "$status" -eq 0 ]
    [[ "$output" != *"KILLED:firmwareExe"* ]]
}

@test "mod-less AD5M (flavor stock): auto_run.sh stock UI line still gets disabled" {
    _load_configure_platform
    MOD_FLAVOR="stock"
    AD5M_FIRMWARE="stock"
    mkdir -p "$MOCK_ROOT/opt"
    printf '#!/bin/sh\n/opt/PROGRAM/ffstartup-arm &\n' > "$MOCK_ROOT/opt/auto_run.sh"

    run configure_platform
    [ "$status" -eq 0 ]
    grep -q "^# Disabled by HelixScreen: /opt/PROGRAM/ffstartup-arm" \
        "$MOCK_ROOT/opt/auto_run.sh"
}

@test "mod-less AD5M (flavor stock): auto_run.sh without the stock UI line is untouched" {
    _load_configure_platform
    MOD_FLAVOR="stock"
    AD5M_FIRMWARE="stock"
    mkdir -p "$MOCK_ROOT/opt"
    printf '#!/bin/sh\necho boot\n' > "$MOCK_ROOT/opt/auto_run.sh"

    run configure_platform
    [ "$status" -eq 0 ]
    ! grep -q "Disabled by HelixScreen" "$MOCK_ROOT/opt/auto_run.sh"
}
