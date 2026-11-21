#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The boot delegate autostart writes over S99input-event-daemon keeps the stock
# screen reachable without SSH and without /userdata/helixscreen:
#   - a helixscreen-restore-stock file in Moonraker's config folder (created
#     from Mainsail/Fluidd, or by the Settings row) hands the display back at
#     boot, through the overlay copy of the uninstaller's restore, and
#     HelixScreen is not started;
#   - with no executable HelixScreen launcher, /usr/bin/gui is switched back on.
# The setup runs the real autostart against a mock rootfs with a package-shaped
# install dir, then boots the delegate it rendered.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/assets/config/platform/hooks-snapmaker-u1.sh"

setup() {
    load helpers

    export MOCK_ROOT="$BATS_TEST_TMPDIR/u1"
    mkdir -p "$MOCK_ROOT/oem" "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/usr/bin"
    printf '#!/bin/sh\necho stock gui\n' > "$MOCK_ROOT/usr/bin/gui"
    chmod +x "$MOCK_ROOT/usr/bin/gui"
    printf '#!/bin/sh\n# STOCK input-event-daemon\nexit 0\n' > "$MOCK_ROOT/etc/init.d/S99input-event-daemon"
    chmod +x "$MOCK_ROOT/etc/init.d/S99input-event-daemon"

    # Package-shaped install where the delegate looks for it: the init script,
    # the platform hook under assets/, and scripts/u1_stock_ui.sh as the U1
    # package ships it (REL_SCRIPTS copies it flat into scripts/).
    DEPLOY_DIR="$MOCK_ROOT/userdata/helixscreen"
    mkdir -p "$DEPLOY_DIR/config" "$DEPLOY_DIR/scripts" "$DEPLOY_DIR/assets/config/platform" \
        "$DEPLOY_DIR/bin"
    export INIT_LOG="$BATS_TEST_TMPDIR/init.log"
    printf '#!/bin/sh\necho "$1" >> "%s"\n' "$INIT_LOG" > "$DEPLOY_DIR/config/helixscreen.init"
    printf '#!/bin/sh\nexit 0\n' > "$DEPLOY_DIR/bin/helix-screen"
    chmod +x "$DEPLOY_DIR/config/helixscreen.init" "$DEPLOY_DIR/bin/helix-screen"
    cp "$HOOK" "$DEPLOY_DIR/assets/config/platform/"
    cp "$WORKTREE_ROOT/scripts/lib/installer/u1_stock_ui.sh" "$DEPLOY_DIR/scripts/"

    export HELIX_U1_RESTORE_TRIGGER="$BATS_TEST_TMPDIR/config/helixscreen-restore-stock"
    export HELIX_U1_RESTORE_LOG="$BATS_TEST_TMPDIR/logs/helixscreen-restore-stock.log"
    mkdir -p "$(dirname "$HELIX_U1_RESTORE_TRIGGER")" "$(dirname "$HELIX_U1_RESTORE_LOG")"

    export LOGGER_LOG="$BATS_TEST_TMPDIR/logger.log"
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\necho "$*" >> "$LOGGER_LOG"\n' > "$BATS_TEST_TMPDIR/bin/logger"
    chmod +x "$BATS_TEST_TMPDIR/bin/logger"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"

    # Install: PAXX-shaped SSH (no S50dropbear), so the pre-flight passes.
    HELIX_SETUP_ROOT="$MOCK_ROOT" HELIX_DROPBEAR_INIT="$MOCK_ROOT/etc/init.d/S50dropbear" \
        bash "$WORKTREE_ROOT/scripts/snapmaker-u1-setup-autostart.sh" "$DEPLOY_DIR" >/dev/null
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ] || fail "install did not disable gui"
    grep -q HelixScreen "$MOCK_ROOT/etc/init.d/S99input-event-daemon" || fail "delegate not installed"
    [ -f "$MOCK_ROOT/etc/helixscreen/u1_stock_ui.sh" ] || fail "restore not copied into the overlay"
}

# rcS runs the delegate with `start` at boot, rcK with `stop` at shutdown.
boot_delegate() {
    HELIX_U1_ROOT="$MOCK_ROOT" ${DELEGATE_SH:-sh} "$MOCK_ROOT/etc/init.d/S99input-event-daemon" "${1:-start}"
}

@test "u1 boot delegate: no trigger -> HelixScreen starts, stock screen stays off" {
    run boot_delegate
    [ "$status" -eq 0 ]
    [ "$(cat "$INIT_LOG")" = start ] || fail "init calls: $(cat "$INIT_LOG" 2>/dev/null)"
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
}

@test "u1 boot delegate: trigger -> stock screen restored from the overlay copy, HelixScreen not started" {
    touch "$HELIX_U1_RESTORE_TRIGGER"

    run boot_delegate
    [ "$status" -eq 0 ]

    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui not re-enabled"
    grep -q STOCK "$MOCK_ROOT/etc/init.d/S99input-event-daemon" || fail "launcher not restored"
    [ ! -e "$MOCK_ROOT/oem/.debug" ] || fail "our /oem/.debug left behind"
    [ ! -e "$MOCK_ROOT/etc/helixscreen" ] || fail "overlay copy of the restore left behind"
    [ ! -e "$HELIX_U1_RESTORE_TRIGGER" ] || fail "trigger left behind"
    lacks start "$(cat "$INIT_LOG" 2>/dev/null)"
    contains "Restore stock screen requested" "$(cat "$HELIX_U1_RESTORE_LOG")"
    contains "Re-enabling stock UI binary" "$(cat "$HELIX_U1_RESTORE_LOG")"
    contains "-t helixscreen" "$(cat "$LOGGER_LOG")"
}

@test "u1 boot delegate: trigger with /userdata/helixscreen gone -> still restored" {
    touch "$HELIX_U1_RESTORE_TRIGGER"
    rm -rf "$MOCK_ROOT/userdata/helixscreen"

    run boot_delegate
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui not re-enabled"
    grep -q STOCK "$MOCK_ROOT/etc/init.d/S99input-event-daemon" || fail "launcher not restored"
}

@test "u1 boot delegate: trigger at shutdown (stop) -> left for the next boot" {
    touch "$HELIX_U1_RESTORE_TRIGGER"

    run boot_delegate stop
    [ "$status" -eq 0 ]
    [ -e "$HELIX_U1_RESTORE_TRIGGER" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
    [ "$(cat "$INIT_LOG")" = stop ]
}

@test "u1 boot delegate: a trigger that cannot be removed -> no restore, HelixScreen starts" {
    mkdir -p "$HELIX_U1_RESTORE_TRIGGER/x"

    run boot_delegate
    [ "$status" -eq 0 ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
    [ "$(cat "$INIT_LOG")" = start ]
    contains "cannot be removed" "$(cat "$HELIX_U1_RESTORE_LOG")"
}

@test "u1 boot delegate: trigger with the overlay restore missing -> gui re-enabled, HelixScreen not started" {
    touch "$HELIX_U1_RESTORE_TRIGGER"
    rm -rf "$MOCK_ROOT/etc/helixscreen"

    run boot_delegate
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui not re-enabled"
    lacks start "$(cat "$INIT_LOG" 2>/dev/null)"
}

@test "u1 boot delegate: install directory gone -> stock screen switched back on" {
    rm -rf "$MOCK_ROOT/userdata/helixscreen"

    run boot_delegate
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui still disabled"
    contains "No usable HelixScreen launcher found" "$(cat "$HELIX_U1_RESTORE_LOG")"
}

@test "u1 boot delegate: launcher not executable -> stock screen switched back on" {
    chmod a-x "$MOCK_ROOT/userdata/helixscreen/config/helixscreen.init"

    run boot_delegate
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui still disabled"
    [ ! -e "$INIT_LOG" ]
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

@test "u1 boot delegate: the real helixscreen.init without its binary -> stock screen switched back on" {
    # helixscreen.init exits 0 before the platform hook (and SSH) when
    # bin/helix-screen is not executable, so it must not count as a launcher.
    cp "$WORKTREE_ROOT/config/helixscreen.init" "$MOCK_ROOT/userdata/helixscreen/config/helixscreen.init"
    chmod +x "$MOCK_ROOT/userdata/helixscreen/config/helixscreen.init"
    rm -f "$MOCK_ROOT/userdata/helixscreen/bin/helix-screen"

    run boot_delegate
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui still disabled: $output"
}

@test "u1 boot delegate: HelixScreen start fails -> stock screen switched back on" {
    printf '#!/bin/sh\necho "$1" >> "%s"\nexit 1\n' "$INIT_LOG" \
        > "$MOCK_ROOT/userdata/helixscreen/config/helixscreen.init"

    run boot_delegate
    [ "$status" -eq 0 ]
    [ "$(cat "$INIT_LOG")" = start ]
    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui still disabled"
    contains "HelixScreen failed to start" "$(cat "$HELIX_U1_RESTORE_LOG")"
}

@test "u1 boot delegate: stock screen already on -> HelixScreen not started beside it" {
    chmod +x "$MOCK_ROOT/usr/bin/gui"

    run boot_delegate
    [ "$status" -eq 0 ]
    [ ! -e "$INIT_LOG" ] || fail "init called: $(cat "$INIT_LOG")"
}

@test "u1 boot delegate: restore without the overlay script -> next boot does not start HelixScreen" {
    touch "$HELIX_U1_RESTORE_TRIGGER"
    rm -rf "$MOCK_ROOT/etc/helixscreen"
    run boot_delegate
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui not re-enabled"
    rm -f "$INIT_LOG"

    # The delegate is still in place, but gui now owns the screen.
    run boot_delegate
    [ "$status" -eq 0 ]
    [ ! -e "$INIT_LOG" ] || fail "init called: $(cat "$INIT_LOG")"
}

@test "u1 boot delegate: busybox sh, as rcS runs it -> trigger restores, otherwise HelixScreen starts" {
    command -v busybox >/dev/null || skip "busybox not installed"
    # busybox runs its own logger applet ahead of PATH, so this case reads the
    # restore log the delegate writes itself.
    run env DELEGATE_SH="busybox sh" bash -c "$(declare -f boot_delegate); boot_delegate"
    [ "$status" -eq 0 ]
    [ "$(cat "$INIT_LOG")" = start ] || fail "init calls: $(cat "$INIT_LOG" 2>/dev/null)"

    touch "$HELIX_U1_RESTORE_TRIGGER"
    run env DELEGATE_SH="busybox sh" bash -c "$(declare -f boot_delegate); boot_delegate"
    [ "$status" -eq 0 ]
    [ -x "$MOCK_ROOT/usr/bin/gui" ] || fail "gui not re-enabled"
    grep -q STOCK "$MOCK_ROOT/etc/init.d/S99input-event-daemon" || fail "launcher not restored"
    contains "Re-enabling stock UI binary" "$(cat "$HELIX_U1_RESTORE_LOG")"
}
