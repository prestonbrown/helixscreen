#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

setup() {
    load helpers
    CHECK="$BATS_TEST_DIRNAME/../../scripts/perf-farm-check.sh"
    export STOP_MARK="$BATS_TEST_TMPDIR/stopped"
    export SSH_CALLS="$BATS_TEST_TMPDIR/ssh-calls"
    export BUDGETS="$BATS_TEST_TMPDIR/budgets.json"
    cat >"$BUDGETS" <<EOF
{"devices":[{"name":"rig","ssh":"root@rig.invalid","binary":"/nonexistent/bin/helix-screen",
  "service_stop":"touch $STOP_MARK","service_start":"true","idle_budget_pct":10}]}
EOF
    # The device is this machine: ssh runs its last argument here, reading the script from stdin.
    mock_command_script ssh '
printf "ssh %s\n" "$*" >> "$SSH_CALLS"
for last; do :; done
exec sh -c "$last"'
    mock_command_script sshpass 'printf "sshpass SSHPASS=%s\n" "$SSHPASS" >> "$SSH_CALLS"; shift; exec "$@"'
    mock_command_script wget 'exit 1'
}

moonraker_says() {
    mock_command_script curl "printf '%s' '{\"result\": {\"status\": {\"print_stats\": {\"state\": \"$1\"}}}}'"
}

@test "a printer it cannot ask about is left alone, never stopped" {
    mock_command_script curl 'exit 7'
    run "$CHECK"
    [ "$status" -eq 2 ]
    contains "could not read print_stats.state" "$output"
    [ ! -e "$STOP_MARK" ]
}

@test "a printing or paused printer is left alone" {
    for state in printing paused; do
        moonraker_says "$state"
        run "$CHECK"
        [ "$status" -eq 2 ]
        contains "the printer is busy (print_stats.state=$state)" "$output"
        [ ! -e "$STOP_MARK" ]
    done
}

@test "wget answers where a board has no curl" {
    mock_command_script curl 'exit 127'
    mock_command_script wget "printf '%s' '{\"status\": {\"print_stats\": {\"state\": \"printing\"}}}'"
    run "$CHECK"
    contains "the printer is busy (print_stats.state=printing)" "$output"
    [ ! -e "$STOP_MARK" ]
}

@test "PERF_SSH_OPTS reaches ssh, and keys are the default" {
    moonraker_says printing
    PERF_SSH_OPTS="-o ProxyJump=me@jump.invalid" run "$CHECK"
    grep -q "ProxyJump=me@jump.invalid" "$SSH_CALLS"
    grep -q "BatchMode=yes" "$SSH_CALLS"
}

@test "a device naming password_env uses sshpass only when the variable is set" {
    moonraker_says printing
    sed -i.bak 's/"name":"rig",/"name":"rig","password_env":"RIG_PASSWORD",/' "$BUDGETS"

    run "$CHECK"
    refute_grep "sshpass" "$SSH_CALLS"

    : >"$SSH_CALLS"
    RIG_PASSWORD=hunter2 run "$CHECK"
    grep -q "sshpass SSHPASS=hunter2" "$SSH_CALLS"
    refute_grep "BatchMode=yes" "$SSH_CALLS"
}

@test "an idle-only run scores idle alone, and a build without a mock is never read as over budget" {
    mock_command_script ssh '
for last; do :; done
case "$last" in true) exit 0 ;; esac
cat >/dev/null
echo "RESULT MEASURED hz=100 idle= 3.6 3.5 print= printoff= (no mock in this build)"
echo "RESTORED ok"'
    sed -i.bak 's/"idle_budget_pct":10/"idle_budget_pct":10,"print_budget_pct":22,"print_anim_off_budget_pct":10/' "$BUDGETS"

    IDLE_ONLY=true run "$CHECK"
    [ "$status" -eq 0 ]
    contains "rig ok" "$output"
    lacks "print-off" "$output"

    run "$CHECK"
    [ "$status" -eq 2 ]
    lacks "OVER" "$output"
    contains "rig INCOMPLETE" "$output"
}
