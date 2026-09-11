#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# WHICH screen UI the standalone uninstaller puts back.
#
# The uninstaller restores the UI HelixScreen displaced. What it must not do is
# hand the host back a UI the user had already switched off before installing:
# on a Pi that is a KlipperScreen the owner deliberately disabled, switched on
# again by an uninstall that only ever scanned the host for UI-shaped names.
#
# ${INSTALL_DIR}/config/.disabled_services is the only thing that tells the two
# cases apart, and it is gone by the time reenable_previous_ui() runs - the
# install directory is removed first - so reenable_disabled_services() publishes
# what it read. These drive the GENERATED scripts/uninstall.sh through both
# functions in the order main() calls them, so the handoff is under test too.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim

    export MOCK_ROOT="$BATS_TEST_TMPDIR/host"
    mkdir -p "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/home/sovol/printer_data/build"

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export STATE_FILE="$INSTALL_DIR/config/.disabled_services"
    export SYSTEMCTL_LOG="$BATS_TEST_TMPDIR/systemctl.log"
    export START_LOG="$BATS_TEST_TMPDIR/started.log"
    : > "$SYSTEMCTL_LOG"

    # Redirect the bundle's absolute host paths into MOCK_ROOT, and drop the
    # trailing dispatch so sourcing only defines functions.
    BUNDLE="$BATS_TEST_TMPDIR/bundle_under_test.sh"
    sed -e "s|/etc/init\.d|$MOCK_ROOT/etc/init.d|g" \
        -e "s|/usr/bin/update-cosmos|$MOCK_ROOT/usr/bin/update-cosmos|g" \
        -e "s|/mnt/UDISK|$MOCK_ROOT/mnt/UDISK|g" \
        "$WORKTREE_ROOT/scripts/uninstall.sh" \
        | sed '/^case "\${0##\*\/}" in$/,+2d' > "$BUNDLE"
    export BUNDLE
}

# Mock systemctl. Units named in $@ exist on the host (list-unit-files finds
# them); every invocation is logged so a test can assert what was enabled or
# started - and what was not.
mock_host_units() {
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '%s\n' "$@" > "$BATS_TEST_TMPDIR/units"
    cat > "$BATS_TEST_TMPDIR/bin/systemctl" <<EOF
#!/bin/sh
echo "\$@" >> "$SYSTEMCTL_LOG"
if [ "\$1" = "list-unit-files" ]; then
    grep -qx "\${2%.service}" "$BATS_TEST_TMPDIR/units" || exit 1
fi
exit 0
EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/systemctl"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

# An init script that records being run, so "was it started" is a fact.
write_init_script() {
    local path="$MOCK_ROOT/etc/init.d/$1"
    cat > "$path" <<EOF
#!/bin/sh
echo "$1 \$1" >> "$START_LOG"
EOF
    # Disabled the way the installer leaves it.
    chmod a-x "$path"
    echo "$path"
}

# Drive the shipped entry points in main()'s order.
run_restore() {
    bash -c '
        log_info(){ :; }
        log_warn(){ :; }
        log_success(){ :; }
        log_error(){ :; }
        . "$BUNDLE" 2>/dev/null
        platform=pi
        INIT_SYSTEM=systemd
        SUDO=""
        AD5M_FIRMWARE=""
        PREVIOUS_UI_SCRIPT=""
        reenable_disabled_services
        reenable_previous_ui
    '
}

# --- the record is what comes back ---

@test "restore: a recorded KlipperScreen unit is enabled and started" {
    printf 'systemd:KlipperScreen\n' > "$STATE_FILE"
    mock_host_units KlipperScreen

    run run_restore
    [ "$status" -eq 0 ]

    grep -q "^enable KlipperScreen$" "$SYSTEMCTL_LOG"
    grep -q "^start KlipperScreen$" "$SYSTEMCTL_LOG"
}

@test "restore: a UI the user disabled before installing stays off" {
    # The bug. The record says this install displaced a compositor and nothing
    # else, so the KlipperScreen sitting on the host was already off and is not
    # ours to switch on.
    printf 'systemd:cage@tty1\n' > "$STATE_FILE"
    mock_host_units KlipperScreen cage@tty1

    run run_restore
    [ "$status" -eq 0 ]

    refute_grep "KlipperScreen" "$SYSTEMCTL_LOG"
}

@test "restore: an unrecorded UI is left alone even beside a recorded one" {
    printf 'systemd:KlipperScreen\n' > "$STATE_FILE"
    mock_host_units KlipperScreen FeatherScreen

    run run_restore
    [ "$status" -eq 0 ]

    grep -q "^start KlipperScreen$" "$SYSTEMCTL_LOG"
    refute_grep "FeatherScreen" "$SYSTEMCTL_LOG"
}

@test "restore: a recorded compositor is re-enabled but not started" {
    # cage comes back for the next boot; the UI service that runs it is what
    # starts now, and starting both would fight over the DRM master.
    printf 'systemd:cage@tty1\nsystemd:KlipperScreen\n' > "$STATE_FILE"
    mock_host_units KlipperScreen cage@tty1

    run run_restore
    [ "$status" -eq 0 ]

    grep -q "^enable cage@tty1$" "$SYSTEMCTL_LOG"
    refute_grep "^start cage@tty1$" "$SYSTEMCTL_LOG"
    grep -q "^start KlipperScreen$" "$SYSTEMCTL_LOG"
}

@test "restore: only the first recorded UI is started" {
    printf 'systemd:KlipperScreen\nsystemd:FeatherScreen\n' > "$STATE_FILE"
    mock_host_units KlipperScreen FeatherScreen

    run run_restore
    [ "$status" -eq 0 ]

    grep -q "^enable FeatherScreen$" "$SYSTEMCTL_LOG"
    [ "$(grep -c '^start ' "$SYSTEMCTL_LOG")" -eq 1 ]
}

# --- recorded init scripts, and what must never be run as one ---

@test "restore: a recorded UI init script gets its execute bit and a start" {
    local script
    script="$(write_init_script S80featherscreen)"
    printf 'sysv-chmod:%s\n' "$script" > "$STATE_FILE"
    mock_host_units

    run run_restore
    [ "$status" -eq 0 ]

    [ -x "$script" ]
    grep -q "^S80featherscreen start$" "$START_LOG"
}

@test "restore: a recorded bare binary is re-enabled but never invoked" {
    # The record also carries the Sovol and QIDI stock client executables, which
    # take no `start` argument. Running one is not a restore.
    local bin="$MOCK_ROOT/home/sovol/printer_data/build/mksclient"
    cat > "$bin" <<EOF
#!/bin/sh
echo "mksclient \$*" >> "$START_LOG"
EOF
    chmod a-x "$bin"
    printf 'sysv-chmod:%s\n' "$bin" > "$STATE_FILE"
    mock_host_units

    run run_restore
    [ "$status" -eq 0 ]

    [ -x "$bin" ]
    [ ! -f "$START_LOG" ] || refute_grep "mksclient" "$START_LOG"
}

@test "restore: a recorded non-UI init script is not started either" {
    local script
    script="$(write_init_script S40xorg)"
    printf 'sysv-chmod:%s\n' "$script" > "$STATE_FILE"
    mock_host_units

    run run_restore
    [ "$status" -eq 0 ]

    [ -x "$script" ]
    [ ! -f "$START_LOG" ] || refute_grep "S40xorg start" "$START_LOG"
}

# --- no record: the host scan is still the fallback ---

@test "fallback: with no state file the host scan still restores a UI" {
    # An install predating the state file, or one whose directory was removed by
    # hand. Nothing tells us what we displaced, so a UI-shaped unit is the best
    # guess available and the old behaviour is what the user needs.
    rm -f "$STATE_FILE"
    mock_host_units KlipperScreen

    run run_restore
    [ "$status" -eq 0 ]

    grep -q "^enable KlipperScreen$" "$SYSTEMCTL_LOG"
    grep -q "^start KlipperScreen$" "$SYSTEMCTL_LOG"
}

@test "fallback: with no state file a UI init script is restored too" {
    # S80klipperscreen is deliberately not the name here: restore_previous_ui_platform
    # claims that one for Klipper Mod before the scan runs, which would prove the
    # platform arm rather than the fallback.
    rm -f "$STATE_FILE"
    write_init_script S80featherscreen >/dev/null
    mock_host_units

    run run_restore
    [ "$status" -eq 0 ]

    [ -x "$MOCK_ROOT/etc/init.d/S80featherscreen" ]
    grep -q "^S80featherscreen start$" "$START_LOG"
}

@test "fallback: an empty record is a record, not a missing one" {
    # The install disabled nothing, so there is nothing to put back. Falling
    # through to the scan here is what would resurrect the user's own choice.
    : > "$STATE_FILE"
    mock_host_units KlipperScreen

    run run_restore
    [ "$status" -eq 0 ]

    refute_grep "KlipperScreen" "$SYSTEMCTL_LOG"
}

# --- the handoff itself ---

@test "handoff: reenable_disabled_services publishes what it read" {
    printf 'systemd:KlipperScreen\nsysv-chmod:%s\n' \
        "$MOCK_ROOT/etc/init.d/S80featherscreen" > "$STATE_FILE"
    write_init_script S80featherscreen >/dev/null
    mock_host_units

    run bash -c '
        log_info(){ :; }; log_warn(){ :; }; log_success(){ :; }; log_error(){ :; }
        . "$BUNDLE" 2>/dev/null
        SUDO=""
        reenable_disabled_services
        echo "FOUND=$HELIX_DISABLED_RECORD_FOUND"
        echo "UNITS=$HELIX_REENABLED_UNITS"
        echo "SCRIPTS=$HELIX_REENABLED_SCRIPTS"
    '
    [ "$status" -eq 0 ]

    contains "FOUND=1" "$output"
    contains "UNITS= KlipperScreen" "$output"
    contains "S80featherscreen" "$output"
}

@test "handoff: no state file reports no record" {
    rm -f "$STATE_FILE"

    run bash -c '
        log_info(){ :; }; log_warn(){ :; }; log_success(){ :; }; log_error(){ :; }
        . "$BUNDLE" 2>/dev/null
        SUDO=""
        reenable_disabled_services
        echo "FOUND=$HELIX_DISABLED_RECORD_FOUND"
    '
    [ "$status" -eq 0 ]

    contains "FOUND=0" "$output"
}
