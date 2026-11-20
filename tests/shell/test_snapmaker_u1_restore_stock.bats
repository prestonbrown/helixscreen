#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The U1 platform hook's restore-stock trigger: a file in Moonraker's config
# root (created from Mainsail/Fluidd, or by the Settings row) makes the next
# HelixScreen start hand the display back to the stock screen and reboot, with
# no SSH needed. The restore itself is the uninstaller's (u1_stock_ui.sh), run
# here against a mock rootfs.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/assets/config/platform/hooks-snapmaker-u1.sh"

setup() {
    load helpers

    export HELIX_U1_ROOT="$BATS_TEST_TMPDIR/u1"
    mkdir -p "$HELIX_U1_ROOT/oem" "$HELIX_U1_ROOT/etc/init.d" "$HELIX_U1_ROOT/usr/bin"

    # Installed state on stock firmware: gui disabled, the input-event-daemon
    # launcher taken over with the original parked as .stock, /oem/.debug ours.
    printf '#!/bin/sh\necho stock gui\n' > "$HELIX_U1_ROOT/usr/bin/gui"
    chmod a-x "$HELIX_U1_ROOT/usr/bin/gui"
    printf '#!/bin/sh\n# Modified by HelixScreen\n' > "$HELIX_U1_ROOT/etc/init.d/S99input-event-daemon"
    printf '#!/bin/sh\n# STOCK input-event-daemon\n' > "$HELIX_U1_ROOT/etc/init.d/S99input-event-daemon.stock"
    touch "$HELIX_U1_ROOT/oem/.debug" "$HELIX_U1_ROOT/oem/.debug.helixscreen"

    export HELIX_U1_RESTORE_TRIGGER="$BATS_TEST_TMPDIR/config/helixscreen-restore-stock"
    export HELIX_U1_RESTORE_LOG="$BATS_TEST_TMPDIR/logs/helixscreen-restore-stock.log"
    export HELIX_U1_RESTORE_LIB="$WORKTREE_ROOT/scripts/lib/installer/u1_stock_ui.sh"
    mkdir -p "$(dirname "$HELIX_U1_RESTORE_TRIGGER")" "$(dirname "$HELIX_U1_RESTORE_LOG")"

    export REBOOT_LOG="$BATS_TEST_TMPDIR/reboot.log"
    export LOGGER_LOG="$BATS_TEST_TMPDIR/logger.log"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\necho reboot >> "$REBOOT_LOG"\n' > "$BATS_TEST_TMPDIR/bin/fake-reboot"
    printf '#!/bin/sh\necho "$*" >> "$LOGGER_LOG"\n' > "$BATS_TEST_TMPDIR/bin/logger"
    chmod +x "$BATS_TEST_TMPDIR/bin/fake-reboot" "$BATS_TEST_TMPDIR/bin/logger"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    export HELIX_U1_REBOOT_CMD=fake-reboot
    export HELIX_U1_REBOOT_WAIT=0
}

# The installer's log functions (helpers.bash exports silent ones) do not
# exist where the hook runs.
run_trigger_check() {
    ( unset -f log_info log_warn; . "$HOOK"; u1_restore_stock_if_requested )
}

@test "u1 restore trigger: restores the stock screen, removes the trigger, logs, reboots" {
    touch "$HELIX_U1_RESTORE_TRIGGER"

    run run_trigger_check
    [ "$status" -eq 0 ]

    [ -x "$HELIX_U1_ROOT/usr/bin/gui" ] || fail "gui not re-enabled"
    grep -q STOCK "$HELIX_U1_ROOT/etc/init.d/S99input-event-daemon" || fail "launcher not restored"
    [ ! -e "$HELIX_U1_ROOT/oem/.debug" ] || fail "our /oem/.debug left behind"
    [ ! -e "$HELIX_U1_RESTORE_TRIGGER" ] || fail "trigger left behind"
    [ "$(cat "$REBOOT_LOG")" = reboot ] || fail "no reboot"
    contains "Restore stock screen requested" "$(cat "$HELIX_U1_RESTORE_LOG")"
    contains "Re-enabling stock UI binary" "$(cat "$HELIX_U1_RESTORE_LOG")"
    contains "-t helixscreen" "$(cat "$LOGGER_LOG")"
}

@test "u1 restore trigger: no trigger -> nothing restored, no reboot" {
    run run_trigger_check
    [ "$status" -eq 0 ]
    [ ! -x "$HELIX_U1_ROOT/usr/bin/gui" ]
    [ ! -e "$REBOOT_LOG" ]
}

@test "u1 restore trigger: a trigger that cannot be removed -> no restore, no reboot loop" {
    # A directory in the trigger's place: rm -f cannot remove it.
    mkdir -p "$HELIX_U1_RESTORE_TRIGGER/x"

    run run_trigger_check
    [ "$status" -eq 0 ]
    [ ! -x "$HELIX_U1_ROOT/usr/bin/gui" ]
    [ ! -e "$REBOOT_LOG" ]
    contains "cannot be removed" "$(cat "$HELIX_U1_RESTORE_LOG")"
}

@test "u1 restore trigger: restore script missing -> trigger kept, no reboot" {
    touch "$HELIX_U1_RESTORE_TRIGGER"
    export HELIX_U1_RESTORE_LIB="$BATS_TEST_TMPDIR/nope/u1_stock_ui.sh"

    run run_trigger_check
    [ "$status" -eq 0 ]
    [ -e "$HELIX_U1_RESTORE_TRIGGER" ]
    [ ! -e "$REBOOT_LOG" ]
    contains "is missing" "$(cat "$HELIX_U1_RESTORE_LOG")"
}

@test "u1 restore trigger: platform_pre_start acts on it before anything else" {
    touch "$HELIX_U1_RESTORE_TRIGGER"

    run bash -c '
        . "'"$HOOK"'"
        ensure_lmd_running() { echo "lmd step ran"; }
        ensure_wifi_associated() { :; }
        ensure_root_ssh() { :; }
        start_remote_screen() { :; }
        platform_pre_start
    '
    [ "$status" -eq 0 ]
    [ "$(cat "$REBOOT_LOG")" = reboot ] || fail "no reboot"
    # Reached only because the stubbed reboot returned; the restore came first.
    [[ "$(head -1 <<< "$output")" == *"Restore stock screen requested"* ]] || fail "$output"
}

# The Settings row is offered only where the app sees the trigger path.
pre_start_exports() {
    bash -c '
        . "'"$HOOK"'"
        ensure_lmd_running() { :; }
        ensure_wifi_associated() { :; }
        ensure_root_ssh() { :; }
        start_remote_screen() { :; }
        platform_pre_start
        env | grep "^HELIX_U1_RESTORE_TRIGGER=" || echo "not exported"
    '
}

@test "u1 restore trigger: stock firmware exports the trigger path to the app" {
    export HELIX_DROPBEAR_INIT="$BATS_TEST_TMPDIR/S50dropbear"
    printf '#!/bin/sh\n[ x"$(custom_misc vertype 2>/dev/null)" = x"dbg" ] || exit 0\n' > "$HELIX_DROPBEAR_INIT"
    export -n HELIX_U1_RESTORE_TRIGGER

    run pre_start_exports
    [ "$status" -eq 0 ]
    contains "HELIX_U1_RESTORE_TRIGGER=/home/lava/printer_data/config/helixscreen-restore-stock" "$output"
}

@test "u1 restore trigger: PAXX firmware (no --force gate) does not export it" {
    export HELIX_DROPBEAR_INIT="$BATS_TEST_TMPDIR/S50dropbear"
    printf '#!/bin/sh\n# [ x"$(custom_misc vertype 2>/dev/null)" = x"dbg" ] || exit 0\n' > "$HELIX_DROPBEAR_INIT"
    export -n HELIX_U1_RESTORE_TRIGGER

    run pre_start_exports
    [ "$status" -eq 0 ]
    contains "not exported" "$output"
}
