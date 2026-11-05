#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make full-test-run's split mode (scripts/full-test-run.sh).
#
# With TEST_HOST=1 the unit sweep runs on the test host while bats runs here, and the gate
# passes only when both do. make and test-host-run.sh are stubbed: each reports a
# verdict the test chooses and records that it ran.

GATE="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/full-test-run.sh"

setup() {
    load helpers
    # The make stub never reaches the real make or the jobpool shim, and refuses to nest.
    export JOBPOOL=0
    unset FAKE_MAKE_DEPTH
    export TMPDIR="$BATS_TEST_TMPDIR"
    export CALLS="$BATS_TEST_TMPDIR/calls"
    # <target> exits with $MAKE_<target>_RC (unit_sweep, test_shell), default 0.
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
for a in "$@"; do case "$a" in -*) ;; *) t=$a ;; esac; done
echo "make $t" >> "$CALLS"
case "$t" in
    unit-sweep) exit "${MAKE_unit_sweep_RC:-0}" ;;
    test-shell) exit "${MAKE_test_shell_RC:-0}" ;;
esac'
    mock_command_script fake-test-host-run '
echo "host-run $*" >> "$CALLS"
case "$1" in
    sweep) echo "test-host sweep output"; exit "${HOST_SWEEP_RC:-0}" ;;
    --probe) exit "${HOST_PROBE_RC:-1}" ;;
esac'
    export MAKE=fake-make TEST_HOST_RUN=fake-test-host-run
    export HELIX_TEST_HOST=testhost.invalid
    unset TEST_HOST HELIX_TEST_HOST_AUTO
}

@test "TEST_HOST=1: the sweep goes to the test host, bats runs here, and both green passes" {
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "host-run sweep" "$CALLS"
    grep -qx "make test-shell" "$CALLS"
    refute_grep "make unit-sweep" "$CALLS"
    contains "test-host unit sweep: passed" "$output"
    contains "local shell suite: passed" "$output"
}

@test "TEST_HOST=1: the sweep does not wait behind the test host load" {
    mock_command_script fake-test-host-run 'echo "max-load ${HELIX_TEST_MAX_LOAD-unset}" >> "$CALLS"'
    unset HELIX_TEST_MAX_LOAD
    TEST_HOST=1 run "$GATE"
    grep -qx "max-load 0" "$CALLS"
}

@test "TEST_HOST=1: a red test-host sweep with green bats fails the gate" {
    TEST_HOST=1 HOST_SWEEP_RC=1 run "$GATE"
    [ "$status" -ne 0 ]
    contains "test-host unit sweep: FAILED" "$output"
    contains "local shell suite: passed" "$output"
    contains "test-host sweep output" "$output"
}

@test "TEST_HOST=1: green test-host sweep with red bats fails the gate" {
    TEST_HOST=1 MAKE_test_shell_RC=2 run "$GATE"
    [ "$status" -ne 0 ]
    contains "test-host unit sweep: passed" "$output"
    contains "local shell suite: FAILED" "$output"
}

@test "TEST_HOST=0 runs both suites here and never asks the test host" {
    TEST_HOST=0 HELIX_TEST_HOST_AUTO=1 HOST_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep" "$CALLS"
    grep -qx "make test-shell" "$CALLS"
    refute_grep "host-run" "$CALLS"
}

@test "local mode: a red sweep stops the gate before bats" {
    TEST_HOST=0 MAKE_unit_sweep_RC=1 run "$GATE"
    [ "$status" -ne 0 ]
    refute_grep "make test-shell" "$CALLS"
}

@test "unset TEST_HOST with automatic offload switched off stays local without probing" {
    HELIX_TEST_HOST_AUTO=0 HOST_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep" "$CALLS"
    refute_grep "host-run" "$CALLS"
    contains "HELIX_TEST_HOST_AUTO=0" "$output"
}

@test "unset TEST_HOST follows the probe by default" {
    HOST_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "host-run --probe" "$CALLS"
    grep -qx "host-run sweep" "$CALLS"

    : > "$CALLS"
    HOST_PROBE_RC=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep" "$CALLS"
    refute_grep "host-run sweep" "$CALLS"
}

@test "the build-hosts file can switch automatic offload off" {
    echo "HELIX_TEST_HOST_AUTO=0" > "$HELIX_BUILD_HOSTS_FILE"
    HOST_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    refute_grep "host-run" "$CALLS"
}

@test "a TEST_HOST value other than 1, 0 or empty is refused" {
    TEST_HOST=yes run "$GATE"
    [ "$status" -eq 2 ]
    [ ! -e "$CALLS" ]
}

# The stand-in test-host-run.sh starts a child in place of its job ssh. A real
# background job ignores the terminal's Ctrl-C and, once setsid has detached
# it, the terminal's hangup too, so only the gate itself is signalled here:
# what stops the test-host run must be the gate's own cleanup.
gate_signal_takes_the_run_down() { # <signal>
    mock_command_script fake-test-host-run '
sleep 60 & echo $! > "$BATS_TEST_TMPDIR/child.pid"
echo $$ > "$BATS_TEST_TMPDIR/host.pid"
wait'
    mock_command_script fake-make 'sleep 2'
    TEST_HOST=1 "$GATE" > "$BATS_TEST_TMPDIR/out" 2>&1 &
    local gate=$!
    for _ in $(seq 1 200); do [ -s "$BATS_TEST_TMPDIR/child.pid" ] && break; sleep 0.05; done
    [ -s "$BATS_TEST_TMPDIR/child.pid" ]
    kill "-$1" "$gate"
    for _ in $(seq 1 200); do kill -0 "$gate" 2>/dev/null || break; sleep 0.05; done
    local z c alive=0
    z=$(cat "$BATS_TEST_TMPDIR/host.pid"); c=$(cat "$BATS_TEST_TMPDIR/child.pid")
    for _ in $(seq 1 100); do kill -0 "$c" 2>/dev/null || break; sleep 0.05; done
    kill -0 "$z" 2>/dev/null && alive=1
    kill -0 "$c" 2>/dev/null && alive=1
    kill "$z" "$c" 2>/dev/null || true
    [ "$alive" -eq 0 ]
}

@test "an interrupted TEST_HOST=1 gate takes its test-host run down with it, ssh included" {
    gate_signal_takes_the_run_down TERM
}

@test "a TEST_HOST=1 gate whose terminal hangs up takes its test-host run down too" {
    gate_signal_takes_the_run_down HUP
}

@test "TEST_HOST=1 with no test host configured refuses in one line, naming the file" {
    unset HELIX_TEST_HOST
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 2 ]
    [ "${#lines[@]}" -eq 1 ]
    contains "HELIX_TEST_HOST is not set" "$output"
    contains "$HELIX_BUILD_HOSTS_FILE" "$output"
    [ ! -e "$CALLS" ]
}

@test "automatic mode with no test host configured runs here, silently, and never probes" {
    unset HELIX_TEST_HOST
    mock_command_script ssh 'touch "$BATS_TEST_TMPDIR/ssh-called"; exit 255'
    HELIX_TEST_HOST_AUTO=1 HOST_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep" "$CALLS"
    grep -qx "make test-shell" "$CALLS"
    refute_grep "host-run" "$CALLS"
    lacks "test host" "$output"
    lacks "test-host" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-called" ]
}

@test "the build-hosts file names the test host, and the environment beats it" {
    unset HELIX_TEST_HOST
    printf '# comment\nHELIX_TEST_HOST=fromfile.invalid\n' > "$HELIX_BUILD_HOSTS_FILE"
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    contains "unit sweep on fromfile.invalid" "$output"
    HELIX_TEST_HOST=fromenv.invalid TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    contains "unit sweep on fromenv.invalid" "$output"
}

@test "a candidate-list test host is resolved once, and the gate works on the pick" {
    mock_command_script ssh 'for a; do [ "$a" = true ] && break; h=$a; done
echo "$h" >> "$BATS_TEST_TMPDIR/ssh-log"
case "$h" in up.*) exit 0 ;; *) exit 255 ;; esac'
    HELIX_TEST_HOST=dead.invalid,up.invalid HOST_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    contains "→ HELIX_TEST_HOST=up.invalid" "$output"
    contains "unit sweep on up.invalid" "$output"
    grep -qx "host-run sweep" "$CALLS"
    [ "$(cat "$BATS_TEST_TMPDIR/ssh-log")" = "$(printf 'dead.invalid\nup.invalid')" ]
}

@test "automatic mode with no candidate answering runs here, without probing the link" {
    mock_command_script ssh 'exit 255'
    HELIX_TEST_HOST=dead.invalid,dead2.invalid HOST_PROBE_RC=0 run "$GATE"
    [ "$status" -eq 0 ]
    contains "no candidate answered" "$output"
    contains "the test host is unreachable" "$output"
    grep -qx "make unit-sweep" "$CALLS"
    refute_grep "host-run" "$CALLS"
}
