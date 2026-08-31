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

    # Temporary install directory for the .disabled_services state file
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # Redirect the production loop's absolute /etc/init.d paths at a mock
    # root (same substitution approach as test_sovol_competing_uis.bats) so
    # the plain-host control test below can run the real generic loop safely.
    export MOCK_ROOT="$BATS_TEST_TMPDIR/host"
    mkdir -p "$MOCK_ROOT/etc/init.d"

    local patched="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/opt/config/mod/.root/|$MOCK_ROOT/opt/config/mod/.root/|g" \
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
