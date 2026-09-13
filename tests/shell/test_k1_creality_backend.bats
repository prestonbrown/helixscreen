#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The stock Creality backend on K1 series (master-server, app-server,
# web-server) serves Creality Print and Creality Cloud. Only Monitor and
# display-server contend for the framebuffer, so the kill lists in the install
# hook and the runtime hook stop at those two, and an init script we install
# (/etc/init.d/S99creality-backend) brings the trio up at boot
# (prestonbrown/helixscreen#1468).

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/assets/config/platform/hooks-k1.sh"
MODULE="$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh"
INIT_SRC="$WORKTREE_ROOT/config/creality-backend.init"

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

    # Source the production module with the absolute paths it hardcodes
    # redirected into MOCK_ROOT, so the real functions run against a fake root.
    local patched="$BATS_TEST_TMPDIR/competing_uis.sh"
    sed -e "s|/etc/init.d/|$MOCK_ROOT/etc/init.d/|g" \
        "$MODULE" > "$patched"
    unset _HELIX_COMPETING_UIS_SOURCED
    # shellcheck disable=SC1090
    . "$patched"

    kill_process_by_name() { return 1; }
    export -f kill_process_by_name

    # Neutralize the SSH-safety arm: it probes for dropbear by pidof and
    # starts the real binary when it finds one, which is off-topic here (and
    # pidof without a mock trips the suite sandbox).
    ensure_k1_ssh() { :; }
    export -f ensure_k1_ssh
}

capture_logs() {
    log_warn() { echo "WARN $*" >> "$BATS_TEST_TMPDIR/log"; }
    log_info() { echo "INFO $*" >> "$BATS_TEST_TMPDIR/log"; }
    export -f log_warn log_info
}

# The kill list of the single `for proc in ...` loop in $1.
extract_kill_list() {
    grep -m1 'for proc in ' "$1" | sed 's/.*for proc in //; s/; do.*//; s/ *\\$//'
}

# Write an executable stock S99start_app into the mock root.
write_stock_start_app() {
    local s="$MOCK_ROOT/etc/init.d/S99start_app"
    printf '#!/bin/sh\necho stock stop\n' > "$s"
    chmod +x "$s"
}

# A redirected copy of the shipped init script: every absolute path it touches
# (binaries, dirs, symlinks) lands under MOCK_ROOT, and the PATH hardening is
# dropped so mocked pidof/killall on the test PATH win over the host's.
redirected_init_script() {
    sed -e "s|/usr/sbin:/usr/bin:/sbin:/bin:||" \
        -e "s|/usr/bin/|$MOCK_ROOT/usr/bin/|g" \
        -e "s|/usr/share/frontend|$MOCK_ROOT/usr/share/frontend|g" \
        -e "s|/etc/sysConfig|$MOCK_ROOT/etc/sysConfig|g" \
        -e "s|/tmp/creality|$MOCK_ROOT/tmp/creality|g" \
        "$INIT_SRC"
}

# Fake backend binaries that record their launch.
write_fake_servers() {
    local name
    for name in master-server app-server web-server; do
        printf '#!/bin/sh\necho "launched %s" >> "%s/servers.log"\n' "$name" "$BATS_TEST_TMPDIR" \
            > "$MOCK_ROOT/usr/bin/$name"
        chmod +x "$MOCK_ROOT/usr/bin/$name"
    done
}

# --- the kill lists: framebuffer contenders only, in both copies ---

@test "k1: installer kill list stops at Monitor and display-server" {
    local list
    list="$(extract_kill_list "$MODULE")"
    [ "$list" = "Monitor display-server" ]
}

@test "k1: runtime hook kill list stops at Monitor and display-server" {
    local list
    list="$(extract_kill_list "$HOOK")"
    [ "$list" = "Monitor display-server" ]
}

@test "k1: hook and installer agree on the kill list" {
    # Two hand-written copies of one rule drift; this is the gate.
    [ "$(extract_kill_list "$MODULE")" = "$(extract_kill_list "$HOOK")" ]
}

@test "k1: kill lists leave the backend trio off entirely" {
    local list spared
    for list in "$(extract_kill_list "$MODULE")" "$(extract_kill_list "$HOOK")"; do
        for spared in master-server app-server web-server audio-server wifi-server upgrade-server; do
            if echo "$list" | grep -qw "$spared"; then
                echo "$spared must stay off the K1 kill list (got: $list)" >&2
                return 1
            fi
        done
    done
}

@test "k1: Monitor is killed before display-server" {
    # Monitor is a watchdog that respawns display-server; killing
    # display-server first lets Monitor resurrect it between the two kills.
    local list
    list="$(extract_kill_list "$MODULE")"
    [ "${list%% *}" = "Monitor" ]
}

@test "k1: runtime hook still kills boot_display" {
    grep -q 'killall boot_display' "$HOOK"
}

@test "k1: runtime hook still disables S99start_app" {
    grep -q 'chmod a-x /etc/init.d/S99start_app' "$HOOK"
}

# --- stop_k1_stock_competing_uis ---

@test "k1 stop: kills only Monitor and display-server, spares the trio" {
    local kills="$BATS_TEST_TMPDIR/kills.log"
    write_stock_start_app
    kill_process_by_name() { echo "$1" >> "$kills"; return 0; }
    export -f kill_process_by_name

    found_any=false
    run stop_k1_stock_competing_uis
    [ "$status" -eq 0 ]

    grep -qx "Monitor" "$kills"
    grep -qx "display-server" "$kills"
    [ "$(wc -l < "$kills")" -eq 2 ]
}

@test "k1 stop: disables and records S99start_app" {
    write_stock_start_app

    found_any=false
    run stop_k1_stock_competing_uis
    [ "$status" -eq 0 ]

    [ ! -x "$MOCK_ROOT/etc/init.d/S99start_app" ]
    grep -qF "sysv-chmod:$MOCK_ROOT/etc/init.d/S99start_app" "$DISABLED_SERVICES_FILE"
}

@test "k1 stop: warns that the backend is kept" {
    write_stock_start_app
    capture_logs

    found_any=false
    run stop_k1_stock_competing_uis
    [ "$status" -eq 0 ]

    grep -q "WARN.*backend servers.*kept running" "$BATS_TEST_TMPDIR/log"
    grep -q "Creality Print" "$BATS_TEST_TMPDIR/log"
}

@test "k1 stop: backend-off warning names what the user loses" {
    write_stock_start_app
    capture_logs
    K1_CREALITY_BACKEND_ENABLED=0

    found_any=false
    run stop_k1_stock_competing_uis
    [ "$status" -eq 0 ]

    grep -q "WARN.*no longer reach this printer" "$BATS_TEST_TMPDIR/log"
}

@test "k1 stop: no warning when there was no stock stack to disable" {
    capture_logs

    found_any=false
    run stop_k1_stock_competing_uis
    [ "$status" -eq 0 ]

    [ ! -f "$BATS_TEST_TMPDIR/log" ] || refute_grep "WARN.*backend" "$BATS_TEST_TMPDIR/log"
}

# --- install_k1_creality_backend ---

@test "k1 install: installs, enables, records and starts the backend script" {
    write_fake_servers
    redirected_init_script > "$INSTALL_DIR/config/creality-backend.init"
    mock_command_script "pidof" "exit 1"

    run install_k1_creality_backend
    [ "$status" -eq 0 ]

    local dest="$MOCK_ROOT/etc/init.d/S99creality-backend"
    [ -x "$dest" ]
    grep -qF "sysv-created:$dest" "$DISABLED_SERVICES_FILE"
    # Started within the install: the fake trio recorded their launch.
    for name in master-server app-server web-server; do
        grep -q "launched $name" "$BATS_TEST_TMPDIR/servers.log"
    done
}

@test "k1 install: no-op when the enable flag is off" {
    redirected_init_script > "$INSTALL_DIR/config/creality-backend.init"
    K1_CREALITY_BACKEND_ENABLED=0

    run install_k1_creality_backend
    [ "$status" -eq 0 ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99creality-backend" ]
    [ ! -f "$DISABLED_SERVICES_FILE" ]
}

@test "k1 install: no-op on Simple AF firmware" {
    redirected_init_script > "$INSTALL_DIR/config/creality-backend.init"
    K1_FIRMWARE="simple_af"

    run install_k1_creality_backend
    [ "$status" -eq 0 ]
    [ ! -e "$MOCK_ROOT/etc/init.d/S99creality-backend" ]
}

@test "k1 install: warns and continues when the asset is missing" {
    capture_logs
    mock_command_script "pidof" "exit 1"

    run install_k1_creality_backend
    [ "$status" -eq 0 ]
    grep -q "WARN.*creality-backend.init missing" "$BATS_TEST_TMPDIR/log"
    [ ! -e "$MOCK_ROOT/etc/init.d/S99creality-backend" ]
}

@test "k1 install: reached from the install flow on platform k1" {
    # The function is worthless if nothing invokes it. Pin the call site.
    grep -q 'install_k1_creality_backend' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
}

# --- the init script asset itself ---

@test "k1 init script passes sh syntax check" {
    sh -n "$INIT_SRC"
}

@test "k1 init script passes shellcheck" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    shellcheck -s sh "$INIT_SRC"
}

@test "k1 init script has start/stop/restart/status cases" {
    for case_name in start stop restart status; do
        grep -qE "^    ${case_name}\)|^    ${case_name}\|" "$INIT_SRC"
    done
}

@test "k1 init script runs exactly the backend trio" {
    local servers
    servers=$(sed -n 's/^BACKEND_SERVERS="\(.*\)"$/\1/p' "$INIT_SRC")
    [ "$servers" = "master-server app-server web-server" ]
}

@test "k1 init script: start launches each server exactly once" {
    write_fake_servers
    local dest="$MOCK_ROOT/etc/init.d/S99creality-backend"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "pidof" "exit 1"

    run "$dest" start
    [ "$status" -eq 0 ]
    [ "$(grep -c "launched" "$BATS_TEST_TMPDIR/servers.log")" -eq 3 ]

    # A second start with everything reported running launches nothing new.
    mock_command_script "pidof" "echo 123; exit 0"
    run "$dest" start
    [ "$status" -eq 0 ]
    [ "$(grep -c "launched" "$BATS_TEST_TMPDIR/servers.log")" -eq 3 ]
}

@test "k1 init script: start recreates the stock preamble dirs and symlinks" {
    write_fake_servers
    local dest="$MOCK_ROOT/etc/init.d/S99creality-backend"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "pidof" "exit 1"

    run "$dest" start
    [ "$status" -eq 0 ]

    [ -d "$MOCK_ROOT/tmp/creality/local_gcode/thumbnail" ]
    [ -d "$MOCK_ROOT/tmp/creality/original" ]
    [ -L "$MOCK_ROOT/usr/share/frontend/downloads/thumbnail" ]
    [ -L "$MOCK_ROOT/usr/share/frontend/downloads/original" ]
    [ -d "$MOCK_ROOT/etc/sysConfig/defData" ]
}

@test "k1 init script: start skips a missing binary without failing" {
    # A firmware variant without one of the trio must still get the other two.
    write_fake_servers
    rm -f "$MOCK_ROOT/usr/bin/app-server"
    local dest="$MOCK_ROOT/etc/init.d/S99creality-backend"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "pidof" "exit 1"

    run "$dest" start
    [ "$status" -eq 0 ]
    grep -q "launched master-server" "$BATS_TEST_TMPDIR/servers.log"
    grep -q "launched web-server" "$BATS_TEST_TMPDIR/servers.log"
    refute grep -q "launched app-server" "$BATS_TEST_TMPDIR/servers.log"
}

@test "k1 init script: stop kills only the trio" {
    local dest="$MOCK_ROOT/etc/init.d/S99creality-backend" kills="$BATS_TEST_TMPDIR/kills.log"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "killall" "echo \"\$@\" >> '$kills'"

    run "$dest" stop
    [ "$status" -eq 0 ]

    for name in master-server app-server web-server; do
        grep -qx "$name" "$kills"
    done
    [ "$(wc -l < "$kills")" -eq 3 ]
}

@test "k1 init script: status reports a running backend" {
    local dest="$MOCK_ROOT/etc/init.d/S99creality-backend"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "pidof" "echo 4242; exit 0"

    run "$dest" status
    [ "$status" -eq 0 ]
    contains "master-server is running (PID 4242)" "$output"
}

@test "k1 init script: status fails when nothing is running" {
    local dest="$MOCK_ROOT/etc/init.d/S99creality-backend"
    redirected_init_script > "$dest"
    chmod +x "$dest"
    mock_command_script "pidof" "exit 1"

    run "$dest" status
    [ "$status" -ne 0 ]
    contains "master-server is not running" "$output"
}
