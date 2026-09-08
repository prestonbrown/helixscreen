#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The QIDI stock screen unit is `makerbase-client` on firmware 1.1.1 and older
# and `qidi-client` on 01.01.02+. These cover the name list plus the two shapes
# it cannot reach: a unit found by the QIDI path in its ExecStart, and the
# `QD_Q2/bin/client` binary whose basename is too generic for pidof.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export DISABLED_SERVICES_FILE="$INSTALL_DIR/config/.disabled_services"

    # Mock root mirroring a QIDI Q2: the stock UI binary under a Klipper user's
    # home, and both systemd unit directories the discovery scan reads.
    export MOCK_ROOT="$BATS_TEST_TMPDIR/qidi"
    mkdir -p "$MOCK_ROOT/etc/init.d" \
             "$MOCK_ROOT/etc/systemd/system" \
             "$MOCK_ROOT/lib/systemd/system" \
             "$MOCK_ROOT/home/mks/QD_Q2/bin" \
             "$MOCK_ROOT/home/qidi/QD_Q2/bin"

    detect_init_system() { INIT_SYSTEM="systemd"; }
    export -f detect_init_system
    INIT_SYSTEM="systemd"
    AD5M_FIRMWARE=""
    K1_FIRMWARE=""
    platform="pi"
    PREVIOUS_UI_SCRIPT=""
    SERVICE_NAME="helixscreen"

    # Source the production module with the absolute paths it hardcodes
    # redirected into MOCK_ROOT, so the real functions run against a fake root.
    local patched="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        -e "s|/etc/systemd/system|$MOCK_ROOT/etc/systemd/system|g" \
        -e "s|/lib/systemd/system|$MOCK_ROOT/lib/systemd/system|g" \
        -e "s|/home/|$MOCK_ROOT/home/|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh" > "$patched"
    unset _HELIX_COMPETING_UIS_SOURCED
    # shellcheck disable=SC1090
    . "$patched"

    kill_process_by_name() { return 1; }
    export -f kill_process_by_name
}

# Write the stock UI binary for one Klipper user, and echo its path.
write_qd_client() {
    local user="${1:-mks}"
    local bin="$MOCK_ROOT/home/$user/QD_Q2/bin/client"
    cat > "$bin" <<'EOF'
#!/bin/sh
echo "stock qidi ui"
EOF
    chmod +x "$bin"
    echo "$bin"
}

# Write a systemd unit whose ExecStart points at $2, and echo the unit name.
write_unit() {
    local name="$1" execstart="$2" dir="${3:-$MOCK_ROOT/etc/systemd/system}"
    cat > "$dir/$name" <<EOF
[Unit]
Description=fake unit

[Service]
ExecStart=$execstart
EOF
    echo "$name"
}

# --- the name list ---

@test "qidi: makerbase-client is in the COMPETING_UIS list" {
    echo "$COMPETING_UIS" | grep -qw "makerbase-client"
}

@test "qidi: the 01.01.02+ unit and binary names are still in COMPETING_UIS" {
    echo "$COMPETING_UIS" | grep -qw "qidi-client"
    echo "$COMPETING_UIS" | grep -qw "qidiclient"
}

# --- stop_qidi_competing_uis(): the QD_Q2 binary (firmware 1.1.x) ---

@test "qidi handler: chmod-a-x's the QD_Q2 client binary and records it" {
    local bin
    bin="$(write_qd_client mks)"
    [ -x "$bin" ]

    mock_command_script "systemctl" "exit 1"

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    [ ! -x "$bin" ]
    grep -qF "sysv-chmod:$bin" "$DISABLED_SERVICES_FILE"
}

@test "qidi handler: finds the client binary under the renamed qidi user home" {
    local bin
    bin="$(write_qd_client qidi)"

    mock_command_script "systemctl" "exit 1"

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    [ ! -x "$bin" ]
    grep -qF "sysv-chmod:$bin" "$DISABLED_SERVICES_FILE"
}

@test "qidi handler: kills the running client by resolved path, not by basename" {
    local bin kill_log="$BATS_TEST_TMPDIR/killpath.log"
    bin="$(write_qd_client mks)"

    mock_command_script "systemctl" "exit 1"
    # Record what the handler asked to kill. A basename kill would pass `client`,
    # which on a general-purpose SBC can match anything.
    kill_process_by_path() { echo "$1" >> "$kill_log"; return 0; }
    export -f kill_process_by_path

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    grep -qF "$bin" "$kill_log"
    refute_grep "^client$" "$kill_log"
}

# --- stop_qidi_competing_uis(): unit discovery by ExecStart ---

@test "qidi handler: discovers a renamed unit by the QIDI path in its ExecStart" {
    local systemctl_log="$BATS_TEST_TMPDIR/systemctl.log"
    write_unit "qd-screen.service" "/home/mks/QD_Q2/bin/client --fullscreen" >/dev/null
    mock_command_script "systemctl" "echo \"\$@\" >> \"$systemctl_log\"; exit 0"

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    grep -q "stop qd-screen.service" "$systemctl_log"
    grep -q "disable qd-screen.service" "$systemctl_log"
    grep -qF "systemd:qd-screen.service" "$DISABLED_SERVICES_FILE"
}

@test "qidi handler: discovers a unit in /lib/systemd/system too" {
    local systemctl_log="$BATS_TEST_TMPDIR/systemctl.log"
    write_unit "qidiclient.service" "/usr/bin/qidiclient" \
        "$MOCK_ROOT/lib/systemd/system" >/dev/null
    mock_command_script "systemctl" "echo \"\$@\" >> \"$systemctl_log\"; exit 0"

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    grep -q "stop qidiclient.service" "$systemctl_log"
    grep -qF "systemd:qidiclient.service" "$DISABLED_SERVICES_FILE"
}

@test "qidi handler: matches an ExecStart whose capitalisation differs" {
    local systemctl_log="$BATS_TEST_TMPDIR/systemctl.log"
    write_unit "Qidi-client.service" "/usr/bin/Qidi-client" >/dev/null
    mock_command_script "systemctl" "echo \"\$@\" >> \"$systemctl_log\"; exit 0"

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    grep -q "disable Qidi-client.service" "$systemctl_log"
    grep -qF "systemd:Qidi-client.service" "$DISABLED_SERVICES_FILE"
}

@test "qidi handler: never touches the helixscreen unit" {
    local systemctl_log="$BATS_TEST_TMPDIR/systemctl.log"
    # Our own unit legitimately mentions a QIDI-looking install path.
    write_unit "helixscreen.service" "/home/mks/QD_Q2/../helixscreen/bin/helix-launcher.sh" >/dev/null
    mock_command_script "systemctl" "echo \"\$@\" >> \"$systemctl_log\"; exit 0"

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    [ ! -f "$systemctl_log" ] || refute_grep "helixscreen" "$systemctl_log"
    [ ! -f "$DISABLED_SERVICES_FILE" ] || refute_grep "helixscreen" "$DISABLED_SERVICES_FILE"
}

@test "qidi handler: is a no-op on a host with no QIDI markers" {
    local systemctl_log="$BATS_TEST_TMPDIR/systemctl.log"
    mock_command_script "systemctl" "echo \"\$@\" >> \"$systemctl_log\"; exit 0"

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    [ ! -f "$systemctl_log" ] || refute_grep "disable" "$systemctl_log"
    [ ! -f "$DISABLED_SERVICES_FILE" ]
}

# --- the empty-sweep report ---
#
# log_warn is a no-op stub from helpers, so these capture it locally to assert
# which arm ran.

capture_logs() {
    log_warn() { echo "WARN $*" >> "$BATS_TEST_TMPDIR/log"; }
    log_info() { echo "INFO $*" >> "$BATS_TEST_TMPDIR/log"; }
    export -f log_warn log_info
}

@test "empty sweep warns on a QIDI-class host" {
    mock_command_script "systemctl" "exit 1"
    capture_logs
    _is_qidi_class_sbc() { return 0; }
    export -f _is_qidi_class_sbc

    run stop_competing_uis
    [ "$status" -eq 0 ]
    grep -q "WARN No competing UIs found, but this device normally ships one." "$BATS_TEST_TMPDIR/log"
}

@test "empty sweep warns on a vendor-firmware platform" {
    mock_command_script "systemctl" "exit 1"
    capture_logs
    _is_qidi_class_sbc() { return 1; }
    export -f _is_qidi_class_sbc
    platform="k1"

    run stop_competing_uis
    [ "$status" -eq 0 ]
    grep -q "WARN No competing UIs found" "$BATS_TEST_TMPDIR/log"
}

@test "empty sweep stays quiet on a generic pi" {
    mock_command_script "systemctl" "exit 1"
    capture_logs
    _is_qidi_class_sbc() { return 1; }
    export -f _is_qidi_class_sbc
    platform="pi"

    run stop_competing_uis
    [ "$status" -eq 0 ]
    grep -q "INFO No competing UIs found" "$BATS_TEST_TMPDIR/log"
    refute_grep "WARN" "$BATS_TEST_TMPDIR/log"
}

# --- wiring ---

@test "qidi: stop_competing_uis reaches the handler" {
    local marker="$BATS_TEST_TMPDIR/handler.log"
    mock_command_script "systemctl" "exit 1"

    stop_qidi_competing_uis() { echo ran >> "$marker"; }
    export -f stop_qidi_competing_uis

    run stop_competing_uis
    [ "$status" -eq 0 ]
    grep -q ran "$marker"
}

@test "qidi: the stock UI binary survives an install on a non-QIDI host" {
    # The handler needs no hostname gate because nothing it matches exists off a
    # QIDI box. A same-named binary somewhere else must not be touched.
    local decoy="$MOCK_ROOT/home/pi/printer_data/client"
    mkdir -p "$(dirname "$decoy")"
    echo '#!/bin/sh' > "$decoy"
    chmod +x "$decoy"

    mock_command_script "systemctl" "exit 1"

    found_any=false
    run stop_qidi_competing_uis
    [ "$status" -eq 0 ]

    [ -x "$decoy" ]
}
