#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Which systemd units the competing-UI sweep acts on.
#
# A screen UI competes for the display in two states, not one: running now, or
# enabled so it takes the display back at the next boot - and the installer asks
# for a reboot at the end. On a Pi/Voron host KlipperScreen is a plain systemd
# unit, so both states are what _unit_is_competing() has to answer for.
#
# The counterweight is over-reach. `systemctl is-enabled` exits 0 for `static`,
# `indirect` and `alias` as well, and none of those names a unit `disable` can
# turn off by itself, so acting on one records a promise uninstall's
# `systemctl enable` cannot keep.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export DISABLED_SERVICES_FILE="$INSTALL_DIR/config/.disabled_services"
    export SYSTEMCTL_LOG="$BATS_TEST_TMPDIR/systemctl.log"

    # A fake root for every absolute path the sweep reads, so a bare host scan
    # cannot reach the developer machine's own init scripts or units.
    export MOCK_ROOT="$BATS_TEST_TMPDIR/host"
    mkdir -p "$MOCK_ROOT/etc/init.d" \
             "$MOCK_ROOT/etc/systemd/system" \
             "$MOCK_ROOT/lib/systemd/system" \
             "$MOCK_ROOT/home" \
             "$MOCK_ROOT/opt/config/mod/.root"

    local patched="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/etc/systemd/system|$MOCK_ROOT/etc/systemd/system|g" \
        -e "s|/lib/systemd/system|$MOCK_ROOT/lib/systemd/system|g" \
        -e "s|/opt/config/mod/.root/|$MOCK_ROOT/opt/config/mod/.root/|g" \
        -e "s|/opt/PROGRAM/|$MOCK_ROOT/opt/PROGRAM/|g" \
        -e "s|/home/|$MOCK_ROOT/home/|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh" > "$patched"
    unset _HELIX_COMPETING_UIS_SOURCED
    # shellcheck disable=SC1090
    . "$patched"

    unset _HELIX_UNINSTALL_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"

    _is_self_update() { return 1; }
    export -f _is_self_update
    kill_process_by_name() { return 1; }
    export -f kill_process_by_name
    # The sweep waits on a UI it acted on; nothing here is real.
    sleep() { :; }
    export -f sleep

    INIT_SYSTEM="systemd"
    MOD_FLAVOR=""
    AD5M_FIRMWARE=""
    K1_FIRMWARE=""
    HOST_OWNS_COMPETING_UIS=0
    platform="pi"
    PREVIOUS_UI_SCRIPT=""
    SERVICE_NAME="helixscreen"
    SUDO=""
}

# Mock systemctl reporting one unit in one state and every other unit absent.
# Logs each invocation so a test can assert on stop/disable/enable.
# Args: $1 = unit name, $2 = is-active exit (0 running), $3 = is-enabled word
mock_unit_state() {
    local unit="$1" active_exit="$2" enabled_word="$3"
    local enabled_exit=1
    # systemd exits 0 for `static`, `indirect` and `alias` as well as the enabled
    # words, which is the trap the word match exists to avoid.
    case "$enabled_word" in
        enabled|enabled-runtime|static|indirect|alias) enabled_exit=0 ;;
    esac

    mock_command_script "systemctl" "
echo \"\$@\" >> '$SYSTEMCTL_LOG'
case \"\$1\" in
    is-active)
        case \"\$*\" in *' $unit'*|*'$unit ') exit $active_exit ;; esac
        exit 3
        ;;
    is-enabled)
        case \"\$*\" in *'$unit'*) echo '$enabled_word'; exit $enabled_exit ;; esac
        echo 'not-found'; exit 4
        ;;
esac
exit 0
"
}

# --- _unit_is_competing(): the decision on its own ---

@test "gate: a running unit competes" {
    mock_unit_state "KlipperScreen" 0 "enabled"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -eq 0 ]
}

@test "gate: an enabled but stopped unit competes" {
    mock_unit_state "KlipperScreen" 3 "enabled"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -eq 0 ]
}

@test "gate: an enabled-runtime unit competes" {
    mock_unit_state "KlipperScreen" 3 "enabled-runtime"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -eq 0 ]
}

@test "gate: a stopped, disabled unit does not compete" {
    mock_unit_state "KlipperScreen" 3 "disabled"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -ne 0 ]
}

@test "gate: a stopped static unit does not compete despite is-enabled exiting 0" {
    # `static` has no [Install] section, so `systemctl disable` cannot change it
    # and `systemctl enable` cannot restore it. Acting on exit status alone
    # would record a reversal that uninstall silently fails to perform.
    mock_unit_state "KlipperScreen" 3 "static"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -ne 0 ]
}

@test "gate: a stopped indirect unit does not compete despite is-enabled exiting 0" {
    mock_unit_state "KlipperScreen" 3 "indirect"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -ne 0 ]
}

@test "gate: a stopped alias does not compete despite is-enabled exiting 0" {
    mock_unit_state "KlipperScreen" 3 "alias"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -ne 0 ]
}

@test "gate: a masked unit does not compete" {
    mock_unit_state "KlipperScreen" 3 "masked"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -ne 0 ]
}

@test "gate: an absent unit does not compete" {
    mock_unit_state "SomethingElse" 3 "enabled"
    run _unit_is_competing "KlipperScreen"
    [ "$status" -ne 0 ]
}

# --- stop_competing_uis(): the Pi/Voron KlipperScreen unit ---

@test "pi: a running KlipperScreen is stopped, disabled and recorded" {
    mock_unit_state "KlipperScreen" 0 "enabled"

    run stop_competing_uis
    [ "$status" -eq 0 ]

    grep -q "stop KlipperScreen" "$SYSTEMCTL_LOG"
    grep -q "disable KlipperScreen" "$SYSTEMCTL_LOG"
    grep -qF "systemd:KlipperScreen" "$DISABLED_SERVICES_FILE"
}

@test "pi: an enabled but stopped KlipperScreen is disabled and recorded" {
    # The reboot case: nothing to stop today, but it owns the display tomorrow.
    mock_unit_state "KlipperScreen" 3 "enabled"

    run stop_competing_uis
    [ "$status" -eq 0 ]

    grep -q "disable KlipperScreen" "$SYSTEMCTL_LOG"
    grep -qF "systemd:KlipperScreen" "$DISABLED_SERVICES_FILE"
}

@test "pi: a KlipperScreen the user already disabled is left alone" {
    mock_unit_state "KlipperScreen" 3 "disabled"

    run stop_competing_uis
    [ "$status" -eq 0 ]

    refute_grep "disable KlipperScreen" "$SYSTEMCTL_LOG"
    [ ! -f "$DISABLED_SERVICES_FILE" ] || refute_grep "KlipperScreen" "$DISABLED_SERVICES_FILE"
}

@test "pi: a stopped static unit is left alone" {
    mock_unit_state "KlipperScreen" 3 "static"

    run stop_competing_uis
    [ "$status" -eq 0 ]

    refute_grep "disable KlipperScreen" "$SYSTEMCTL_LOG"
    [ ! -f "$DISABLED_SERVICES_FILE" ] || refute_grep "KlipperScreen" "$DISABLED_SERVICES_FILE"
}

@test "pi: the sweep never touches the helixscreen unit" {
    mock_unit_state "KlipperScreen" 0 "enabled"

    run stop_competing_uis
    [ "$status" -eq 0 ]

    refute_grep "helixscreen" "$SYSTEMCTL_LOG"
}

# --- stop_wayland_compositors(): the same gate on the DRM master holder ---

@test "compositor: an enabled but stopped cage@tty1 is disabled and recorded" {
    # KlipperScreen under cage: the compositor owns /dev/dri/card0, so a cage
    # unit left enabled takes the DRM master back at the next boot.
    mock_unit_state "cage@tty1" 3 "enabled"

    found_any=false
    run stop_wayland_compositors
    [ "$status" -eq 0 ]

    grep -q "disable cage@tty1" "$SYSTEMCTL_LOG"
    grep -qF "systemd:cage@tty1" "$DISABLED_SERVICES_FILE"
}

@test "compositor: a stopped, disabled weston is left alone" {
    mock_unit_state "weston" 3 "disabled"

    found_any=false
    run stop_wayland_compositors
    [ "$status" -eq 0 ]

    refute_grep "disable weston" "$SYSTEMCTL_LOG"
    [ ! -f "$DISABLED_SERVICES_FILE" ] || refute_grep "weston" "$DISABLED_SERVICES_FILE"
}

# --- round trip: what the sweep records, uninstall re-enables ---

@test "round trip: an enabled but stopped KlipperScreen is re-enabled on uninstall" {
    mock_unit_state "KlipperScreen" 3 "enabled"

    run stop_competing_uis
    [ "$status" -eq 0 ]
    grep -qF "systemd:KlipperScreen" "$DISABLED_SERVICES_FILE"

    : > "$SYSTEMCTL_LOG"
    run reenable_disabled_services
    [ "$status" -eq 0 ]

    grep -q "enable KlipperScreen" "$SYSTEMCTL_LOG"
}
