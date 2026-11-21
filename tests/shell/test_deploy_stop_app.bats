#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The stop step of mk/cross.mk's deploy-common: scripts/deploy-stop-app-remote.sh,
# run on the device before anything is synced. A systemd unit with
# Restart=always that is not stopped through systemctl gets respawned from a
# half-synced bin/ the moment its process is killed, so a unit that cannot be
# stopped must end the deploy before the sync, and nothing may be killed.
#
# systemctl, sudo, id, killall, pidof and sleep are stubs. The systemctl stub
# keeps each unit's ActiveState in a file (user units under user@) and logs
# every call; stopping helixscreen re-arms helixscreen-update.path, as the
# unit's ExecStopPost does. pidof reports PIDOF_PIDS for PIDOF_TICKS calls.

setup() {
    load helpers
    REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    STOPPER="$REPO/scripts/deploy-stop-app-remote.sh"

    STATE_DIR="$BATS_TEST_TMPDIR/units"
    CALLS="$BATS_TEST_TMPDIR/calls.log"
    mkdir -p "$STATE_DIR"
    : > "$CALLS"
    export STATE_DIR CALLS

    # Defaults: a non-root ssh user, sudo that needs a password, no polkit rule.
    export FAKE_UID=1000 SUDO_OK=0 POLKIT_OK=0 USER_STOP_OK=1

    mock_command_script "systemctl" '
        echo "systemctl $*" >> "$CALLS"
        case "$1" in
            show) [ -f "$STATE_DIR/$4" ] && s=$(cat "$STATE_DIR/$4") || s=inactive
                  echo "ActiveState=$s"; exit 0 ;;
            --user)
                if [ "$2" = show ]; then
                    [ -f "$STATE_DIR/user@$5" ] && s=$(cat "$STATE_DIR/user@$5") || s=inactive
                    echo "ActiveState=$s"; exit 0
                fi
                [ "$2" = stop ] && [ "$USER_STOP_OK" = 1 ] && echo inactive > "$STATE_DIR/user@$3"
                exit 0 ;;
            --no-ask-password) [ "$POLKIT_OK" = 1 ] || exit 1; shift ;;
        esac
        if [ "$1" = stop ]; then
            [ "$FAKE_UID" = 0 ] || [ "$SUDO_ACTIVE" = 1 ] || [ "$POLKIT_OK" = 1 ] || exit 1
            echo inactive > "$STATE_DIR/$2"
            [ "$2" = helixscreen ] && echo active > "$STATE_DIR/helixscreen-update.path"
            exit 0
        fi
        exit 0
    '
    mock_command_script "sudo" '
        echo "sudo $*" >> "$CALLS"
        [ "$SUDO_OK" = 1 ] || { echo "sudo: a password is required" >&2; exit 1; }
        [ "$1" = -n ] && shift
        SUDO_ACTIVE=1 exec "$@"
    '
    mock_command_script "id" '
        case "$1" in -u) echo "$FAKE_UID" ;; -un) echo pi ;; *) echo "uid=$FAKE_UID" ;; esac
    '
    mock_command_script "killall" 'echo "killall $*" >> "$CALLS"; exit 0'
    export PIDOF_PIDS="" PIDOF_TICKS=0
    mock_command_script "pidof" '
        n=$(cat "$STATE_DIR/pidof.n" 2>/dev/null || echo 0)
        echo $((n + 1)) > "$STATE_DIR/pidof.n"
        [ -n "$PIDOF_PIDS" ] && [ "$n" -lt "$PIDOF_TICKS" ] || exit 1
        echo "$PIDOF_PIDS"
    '
    mock_command_script "sleep" 'exit 0'
}

unit_running() { echo active > "$STATE_DIR/$1"; }
unit_state() { cat "$STATE_DIR/$1" 2>/dev/null || echo inactive; }

@test "stop succeeds through sudo -n: every unit ends inactive, then stragglers are killed" {
    SUDO_OK=1
    unit_running helixscreen
    unit_running helixscreen-update.path

    run sh "$STOPPER"

    [ "$status" -eq 0 ]
    grep -qF "sudo -n systemctl stop helixscreen" "$CALLS"
    [ "$(unit_state helixscreen)" = inactive ]
    # Re-armed by helixscreen's ExecStopPost, so it must be stopped again after.
    [ "$(unit_state helixscreen-update.path)" = inactive ]
    # The kill comes only after the units are down.
    last_stop=$(grep -n "systemctl stop" "$CALLS" | tail -1 | cut -d: -f1)
    first_kill=$(grep -n "^killall" "$CALLS" | head -1 | cut -d: -f1)
    [ -n "$first_kill" ] && [ "$first_kill" -gt "$last_stop" ]
}

@test "stop fails (sudo needs a password): exit 3, the unit is named, nothing is killed" {
    unit_running helixscreen

    run sh "$STOPPER"

    [ "$status" -eq 3 ]
    contains "helixscreen is still running" "$output"
    grep -qF "sudo -n systemctl stop helixscreen" "$CALLS"
    ! grep -q "^killall" "$CALLS"
    [ "$(unit_state helixscreen)" = active ]
}

@test "sudo never runs without -n, so it cannot hang on a prompt" {
    unit_running helixscreen

    run sh "$STOPPER"

    [ "$status" -eq 3 ]
    ! grep -E "^sudo " "$CALLS" | grep -vqE "^sudo -n "
}

@test "a polkit rule that lets the user stop the unit is enough without sudo" {
    POLKIT_OK=1
    unit_running helixscreen

    run sh "$STOPPER"

    [ "$status" -eq 0 ]
    [ "$(unit_state helixscreen)" = inactive ]
    grep -q "^killall" "$CALLS"
}

@test "root host: units stopped with plain systemctl, sudo never touched" {
    FAKE_UID=0
    unit_running helixscreen

    run sh "$STOPPER"

    [ "$status" -eq 0 ]
    grep -qxF "systemctl stop helixscreen" "$CALLS"
    ! grep -q "^sudo" "$CALLS"
    [ "$(unit_state helixscreen)" = inactive ]
    grep -q "^killall" "$CALLS"
}

@test "no unit running (a hand-started launcher): no sudo needed, stragglers are killed" {
    run sh "$STOPPER"

    [ "$status" -eq 0 ]
    ! grep -q "^sudo" "$CALLS"
    grep -q "^killall helix-watchdog helix-screen" "$CALLS"
}

@test "host without systemd (SysV/BusyBox init): straight to the kill, exit 0" {
    # A PATH holding only what the script needs plus the stubs, minus systemctl.
    local bin="$BATS_TEST_TMPDIR/nosysd"
    mkdir -p "$bin"
    for t in id killall pidof sleep sudo; do ln -s "$BATS_TEST_TMPDIR/bin/$t" "$bin/$t"; done
    ln -s "$(command -v sed)" "$bin/sed"

    run env PATH="$bin" "$(command -v sh)" "$STOPPER"

    [ "$status" -eq 0 ]
    grep -q "^killall helix-watchdog helix-screen" "$CALLS"
    ! grep -q "^sudo\|^systemctl" "$CALLS"
}

@test "a user unit that stops is confirmed inactive and the deploy goes on" {
    echo active > "$STATE_DIR/user@helix-screen"

    run sh "$STOPPER"

    [ "$status" -eq 0 ]
    [ "$(cat "$STATE_DIR/user@helix-screen")" = inactive ]
    grep -q "^killall" "$CALLS"
}

@test "a user unit that will not stop: exit 3, the unit is named, nothing is killed" {
    USER_STOP_OK=0
    echo active > "$STATE_DIR/user@helix-screen"

    run sh "$STOPPER"

    [ "$status" -eq 3 ]
    contains "user unit helix-screen is still running" "$output"
    ! grep -q "^killall" "$CALLS"
}

@test "a process that outlives the kill: exit 4 after a bounded wait, naming pid and owner" {
    # This shell stands in for the stuck instance, so /proc has a real owner.
    PIDOF_PIDS=$$ PIDOF_TICKS=1000

    run sh "$STOPPER"

    [ "$status" -eq 4 ]
    contains "pid $$ " "$output"
    contains "(owner $(stat -c %U /proc/$$))" "$output"
    [ "$(cat "$STATE_DIR/pidof.n")" -lt 100 ]
}

@test "a process that exits during the wait: exit 0" {
    PIDOF_PIDS=4242 PIDOF_TICKS=5

    run sh "$STOPPER"

    [ "$status" -eq 0 ]
    lacks "still running" "$output"
}

# --- the make recipe around it ----------------------------------------------

deploy_setup() {
    cd "$(scratch_tree)" || return 1
    # ssh runs the stop half locally against the stubs above; every other
    # remote command is only logged. rsync only logs.
    # K2_SYSTEMD answers the K2 deploy's "does systemd run HelixScreen" probe.
    mock_command_script "ssh" '
        echo "ssh $*" >> "$CALLS"
        [ "$#" -eq 2 ] && [ "$2" = "sh -s" ] && exec sh -s
        case "$2" in "[ -d /run/systemd/system ]"*) exit "${K2_SYSTEMD:-1}" ;; esac
        exit 0
    '
    mock_command_script "rsync" 'echo "rsync $*" >> "$CALLS"; exit 0'
    mkdir -p build/pi/bin build/k2/bin build/assets/images/prerendered build/assets/images/printers/prerendered
    touch build/pi/bin/helix-screen build/pi/bin/helix-splash build/k2/bin/helix-screen build/k2/bin/helix-splash
    touch build/assets/images/prerendered/splash-logo-medium.bin
    touch build/assets/images/prerendered/benchy_thumbnail_white.bin
    touch build/assets/images/prerendered/splash-3d-dark-small.bin
    touch build/assets/images/printers/prerendered/dummy.bin
}

deploy_teardown() {
    rm -rf build/pi build/k2 build/assets/images/prerendered build/assets/images/printers/prerendered
}

@test "deploy-pi refuses before any sync when the service cannot be stopped" {
    deploy_setup
    unit_running helixscreen

    run make deploy-pi PI_HOST=fake-pi-host PI_DEPLOY_DIR=/home/pi/helixscreen
    deploy_teardown

    [ "$status" -ne 0 ]
    contains "Refusing to deploy" "$output"
    contains "NOPASSWD" "$output"
    ! grep -q "^rsync" "$CALLS"
    ! grep -q "^killall" "$CALLS"
}

@test "deploy-pi syncs once the service is stopped" {
    deploy_setup
    SUDO_OK=1
    unit_running helixscreen

    run make deploy-pi PI_HOST=fake-pi-host PI_DEPLOY_DIR=/home/pi/helixscreen
    deploy_teardown

    [ "$status" -eq 0 ]
    lacks "Refusing to deploy" "$output"
    grep -q "^rsync .*helix-screen" "$CALLS"
    [ "$(unit_state helixscreen)" = inactive ]
}

@test "deploy-k2-bin refuses before any copy when the service cannot be stopped" {
    deploy_setup
    unit_running helixscreen

    run make deploy-k2-bin K2_HOST=fake-k2
    deploy_teardown

    [ "$status" -ne 0 ]
    contains "Refusing to deploy" "$output"
    ! grep -q "bin/helix-screen" "$CALLS"
    ! grep -q "^killall" "$CALLS"
}

@test "deploy-k2 on systemd firmware stops and starts the unit, and installs no init script" {
    deploy_setup
    SUDO_OK=1
    unit_running helixscreen
    export K2_SYSTEMD=0

    run make deploy-k2 K2_HOST=fake-k2
    deploy_teardown

    [ "$status" -eq 0 ]
    [ "$(unit_state helixscreen)" = inactive ]
    contains "leaving its unit as it is" "$output"
    ! grep -q "S99helixscreen" "$CALLS"
}

@test "deploy-k2 on stock firmware installs the init script and procd shim" {
    deploy_setup
    export K2_SYSTEMD=1

    run make deploy-k2 K2_HOST=fake-k2
    deploy_teardown

    [ "$status" -eq 0 ]
    lacks "leaving its unit as it is" "$output"
    grep -q "S99helixscreen" "$CALLS"
}
