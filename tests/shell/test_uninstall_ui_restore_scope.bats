#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# WHICH screen UI the standalone uninstaller puts back, and what it says when
# it cannot. These drive the GENERATED scripts/uninstall.sh through
# reenable_disabled_services() and reenable_previous_ui() in the order main()
# calls them.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim

    export MOCK_ROOT="$BATS_TEST_TMPDIR/host"
    mkdir -p "$MOCK_ROOT/etc/init.d"

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export STATE_FILE="$INSTALL_DIR/config/.disabled_services"
    export SYSTEMCTL_LOG="$BATS_TEST_TMPDIR/systemctl.log"
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
# started - and what was not. FAIL_ENABLE=1 makes every enable fail.
mock_host_units() {
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '%s\n' "$@" > "$BATS_TEST_TMPDIR/units"
    cat > "$BATS_TEST_TMPDIR/bin/systemctl" <<EOF
#!/bin/sh
echo "\$@" >> "$SYSTEMCTL_LOG"
if [ "\$1" = "list-unit-files" ]; then
    grep -qx "\${2%.service}" "$BATS_TEST_TMPDIR/units" || exit 1
fi
[ "\$1" = "enable" ] && [ -n "\${FAIL_ENABLE:-}" ] && exit 1
exit 0
EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/systemctl"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
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

# --- the QIDI stock screen ---

@test "restore: a recorded QIDI stock screen unit is enabled and started" {
    # The shape stop_qidi_competing_uis records. Enabled but not started, the
    # stock screen stays on its boot splash until the next reboot.
    printf 'systemd:qidi-client.service\n' > "$STATE_FILE"
    mock_host_units qidi-client

    run run_restore
    [ "$status" -eq 0 ]

    grep -q "^enable qidi-client.service$" "$SYSTEMCTL_LOG"
    grep -Eq "^start qidi-client(\.service)?$" "$SYSTEMCTL_LOG"
}

@test "restore: a failed enable is reported with the command that fixes it" {
    printf 'systemd:qidi-client.service\n' > "$STATE_FILE"
    mock_host_units qidi-client
    export FAIL_ENABLE=1

    run run_restore
    [ "$status" -eq 0 ]

    contains "[WARN] Could not re-enable qidi-client.service" "$output"
    contains "sudo systemctl enable --now qidi-client.service" "$output"
}

@test "fallback: with no state file a QIDI stock screen unit is restored" {
    rm -f "$STATE_FILE"
    mock_host_units qidi-client

    run run_restore
    [ "$status" -eq 0 ]

    grep -q "^enable qidi-client$" "$SYSTEMCTL_LOG"
    grep -q "^start qidi-client$" "$SYSTEMCTL_LOG"
}

@test "fallback: a failed enable is reported with the command that fixes it" {
    rm -f "$STATE_FILE"
    mock_host_units makerbase-client
    export FAIL_ENABLE=1

    run run_restore
    [ "$status" -eq 0 ]

    contains "[WARN] Could not re-enable makerbase-client" "$output"
    contains "sudo systemctl enable --now makerbase-client" "$output"
}
