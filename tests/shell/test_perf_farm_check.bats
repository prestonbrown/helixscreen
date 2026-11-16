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

# Moonraker answers each query with the next word of $STATES, then repeats the last.
moonraker_says_in_turn() {
    export STATES="$1" TURN="$BATS_TEST_TMPDIR/turn"
    : >"$TURN"
    mock_command_script curl '
echo x >> "$TURN"
n=$(wc -l < "$TURN")
set -- $STATES
while [ "$n" -gt 1 ] && [ "$#" -gt 1 ]; do shift; n=$((n - 1)); done
printf "{\"status\": {\"print_stats\": {\"state\": \"%s\"}}}" "$1"'
}

@test "a print started just before the stop keeps its UI" {
    moonraker_says_in_turn "standby printing"
    run "$CHECK"
    contains "the printer is busy (print_stats.state=printing)" "$output"
    [ ! -e "$STOP_MARK" ]
}

@test "a state it does not know is refused" {
    moonraker_says_in_turn "startup"
    run "$CHECK"
    contains "unknown print_stats.state=startup" "$output"
    [ ! -e "$STOP_MARK" ]
}

@test "a print started during the run ends it and brings the UI back" {
    export START_MARK="$BATS_TEST_TMPDIR/started"
    sed -i.bak "s|\"service_start\":\"true\"|\"service_start\":\"touch $START_MARK\"|" "$BUDGETS"
    mock_command_script ps 'echo "123 root 0:00 /nonexistent/bin/helix-screen"'
    moonraker_says_in_turn "standby standby printing"
    run "$CHECK"
    [ -e "$STOP_MARK" ]
    [ -e "$START_MARK" ]
    contains "the printer is busy (print_stats.state=printing)" "$output"
    lacks "SERVICE DID NOT RESTART" "$output"
}

# The device answers the measuring call with these lines; the reachability probe succeeds.
device_answers() {
    export ANSWER="$1"
    mock_command_script ssh '
for last; do :; done
case "$last" in true) exit 0 ;; esac
cat >/dev/null
printf "%b\n" "$ANSWER"'
}

@test "PERF_RESULTS keeps one record per device: status, samples, budgets and version" {
    device_answers 'VERSION HelixScreen 1.2.0-dev\nRESULT MEASURED hz=100 idle= 3.6 3.5 print= 30.0 31.0 printoff= 6.0 6.2\nRESTORED ok'
    sed -i.bak 's/"idle_budget_pct":10/"idle_budget_pct":10,"print_budget_pct":22,"print_anim_off_budget_pct":10/' "$BUDGETS"
    PERF_RESULTS="$BATS_TEST_TMPDIR/r.jsonl" PERF_RUN_TS=2026-11-11T05:00:00Z run "$CHECK"
    [ "$status" -eq 1 ]
    [ "$(wc -l < "$BATS_TEST_TMPDIR/r.jsonl")" -eq 1 ]
    jq -e '[.ts, .device, .status, .version, .idle, .print, .print_anim_off, .budget.print]
           == ["2026-11-11T05:00:00Z", "rig", "over", "HelixScreen 1.2.0-dev", [3.6, 3.5], [30, 31], [6, 6.2], 22]' \
        "$BATS_TEST_TMPDIR/r.jsonl"
}

@test "a device that gives no sample is still recorded, by why" {
    mock_command_script ssh 'exit 255'
    PERF_RESULTS="$BATS_TEST_TMPDIR/r.jsonl" run "$CHECK"
    [ "$(jq -r .status "$BATS_TEST_TMPDIR/r.jsonl")" = unreachable ]

    moonraker_says printing
    mock_command_script ssh 'printf "ssh %s\n" "$*" >> "$SSH_CALLS"; for last; do :; done; exec sh -c "$last"'
    PERF_RESULTS="$BATS_TEST_TMPDIR/s.jsonl" run "$CHECK"
    [ "$(jq -r .status "$BATS_TEST_TMPDIR/s.jsonl")" = skipped ]
    contains "printer is busy" "$(jq -r .detail "$BATS_TEST_TMPDIR/s.jsonl")"
}

@test "the scheduled run appends records, keeps the newest logs, and never overlaps itself" {
    SCHED="$BATS_TEST_DIRNAME/../../scripts/perf-farm-scheduled.sh"
    export PERF_FARM_DIR="$BATS_TEST_TMPDIR/farm" PERF_FARM_KEEP_LOGS=2
    mkdir -p "$PERF_FARM_DIR/runs"
    touch "$PERF_FARM_DIR/runs/20200101T000000Z.log" "$PERF_FARM_DIR/runs/20200102T000000Z.log"
    device_answers 'RESULT MEASURED hz=100 idle= 3.6 3.5 print= printoff= (no mock in this build)\nRESTORED ok'

    run "$SCHED"
    [ "$status" -eq 0 ]
    [ "$(wc -l < "$PERF_FARM_DIR/results.jsonl")" -eq 1 ]
    [ "$(find "$PERF_FARM_DIR/runs" -name '*.log' | wc -l)" -eq 2 ]
    [ ! -e "$PERF_FARM_DIR/runs/20200101T000000Z.log" ]
    newest=$(find "$PERF_FARM_DIR/runs" -name '*.log' | sort | tail -n 1)
    grep -q "rig ok" "$newest"
    grep -q "^exit 0$" "$newest"

    exec 8>"$PERF_FARM_DIR/lock"
    flock -n 8
    run "$SCHED"
    exec 8>&-
    [ "$status" -eq 3 ]
    contains "another perf-farm run holds" "$output"
    [ "$(wc -l < "$PERF_FARM_DIR/results.jsonl")" -eq 1 ]
}

@test "the trend shows each device's last N runs as phase medians" {
    SCHED="$BATS_TEST_DIRNAME/../../scripts/perf-farm-scheduled.sh"
    export PERF_FARM_DIR="$BATS_TEST_TMPDIR/farm"
    mkdir -p "$PERF_FARM_DIR"
    for row in '1 ok [9.0] 1.0' '2 ok [5,3,4] 1.1' '3 over [3.5,3.6] 1.2'; do
        set -- $row
        printf '{"ts":"t%s","device":"rig","status":"%s","version":"%s","idle":%s,"print":[],"print_anim_off":[]}\n' \
            "$1" "$2" "$4" "$3" >>"$PERF_FARM_DIR/results.jsonl"
    done
    printf '{"ts":"t9","device":"other","status":"ok","version":"","idle":[1],"print":[],"print_anim_off":[]}\n' \
        >>"$PERF_FARM_DIR/results.jsonl"

    run "$SCHED" --trend rig 2
    [ "$status" -eq 0 ]
    lacks "t1" "$output"
    lacks "other" "$output"
    [ "$(sed -n 2p <<<"$output" | tr -s ' ')" = "t2 rig ok 4 - - 1.1" ]
    [ "$(sed -n 3p <<<"$output" | tr -s ' ')" = "t3 rig over 3.5 - - 1.2" ]

    run "$SCHED" --trend "" 0
    [ "$status" -eq 2 ]
}
