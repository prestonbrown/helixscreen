#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/run_suite_report.sh — the watch around the Catch2 run
# that make test-vacuous and make test-order-dependence both consume.
#
# The report file is the only progress signal that run has. Test stdout must be
# discarded (ANSI status bytes from tests that shell out are not legal XML, and
# they land in the report if the two streams share a destination), so from
# outside, a suite sitting inside one test case and a suite that is merely slow
# look identical: no console output, one process, a file that will finish
# eventually. Every gate downstream waits on it either way.
#
# So the guard is the check, and these tests are about the guard going off. A
# fake suite stands in for the real one, which keeps a contract about a
# 7-minute run testable in seconds. Each case wraps the script in `timeout` for
# a reason: with the guard removed the run never returns, and a test that hangs
# reads as infrastructure trouble rather than as the regression it is. The
# timeout turns that into exit 124, which is not the 1 the assertions want.

load helpers

SCRIPT="scripts/run_suite_report.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/suite-report"
    rm -rf "$WORK"; mkdir -p "$WORK"
    REPORT="$WORK/report.xml"
    BIN="$WORK/fake-suite"
    # A loaded machine can take seconds just to start a fake suite, and a stall
    # verdict then preempts every other outcome. Only the cases about stalls
    # shorten this, through stall_after_secs.
    export SUITE_REPORT_STALL_SECS=30
    export SUITE_REPORT_MAX_SECS=60
    export SUITE_REPORT_PROGRESS_SECS=1
}

# stdin = the fake suite's body, with $OUT bound to the --out path it was given.
fake_suite() {
    {
        echo '#!/usr/bin/env bash'
        echo 'OUT=""'
        echo 'while [ $# -gt 0 ]; do case "$1" in --out) OUT="$2"; shift 2;; *) shift;; esac; done'
        cat
    } > "$BIN"
    chmod +x "$BIN"
}

guard() { run timeout -s KILL 60 bash "$SCRIPT" "$BIN" "~[.]" "$REPORT"; }

stall_after_secs() { export SUITE_REPORT_STALL_SECS="$1"; }

# ------------------------------------------------------------- the guard fires

@test "a run that stops inside a case is killed and the case is named" {
    stall_after_secs 2
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="the case that never returns" filename="t.cpp">\n' > "$OUT"
sleep 300
EOF
    guard
    [ "$status" -eq 1 ]
    contains "STALLED" "$output"
    contains "the case that never returns" "$output"
}

@test "the stalled case is named in progress output before the verdict" {
    stall_after_secs 2
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="slow case" filename="t.cpp">\n' > "$OUT"
sleep 300
EOF
    guard
    [ "$status" -eq 1 ]
    contains "in: slow case" "$output"
}

@test "a run that keeps writing but never ends is stopped at the wall clock" {
    export SUITE_REPORT_STALL_SECS=60
    export SUITE_REPORT_MAX_SECS=3
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="grinds forever" filename="t.cpp">\n' > "$OUT"
while true; do printf '<Expression success="true"/>\n' >> "$OUT"; sleep 0.2; done
EOF
    guard
    [ "$status" -eq 1 ]
    contains "TIMED OUT" "$output"
    contains "grinds forever" "$output"
}

@test "a case larger than one poll window is still the one named" {
    stall_after_secs 2
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="early case" filename="t.cpp">\n' > "$OUT"
printf '<TestCase name="the fat one" filename="t.cpp">\n' >> "$OUT"
i=0; while [ $i -lt 40000 ]; do printf '<Expression success="true"><Original>x</Original></Expression>\n' >> "$OUT"; i=$((i+1)); done
sleep 300
EOF
    guard
    [ "$status" -eq 1 ]
    contains "the fat one" "$output"
}

@test "a run that produces no report at all fails as unusable" {
    fake_suite <<'EOF'
exit 3
EOF
    guard
    [ "$status" -eq 2 ]
    contains "no report produced" "$output"
}

# ------------------------------------------------------- the guard stays quiet

@test "a run that finishes reports its size and passes" {
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="one" filename="t.cpp"/>\n<TestCase name="two" filename="t.cpp"/>\n</Catch2TestRun>\n' > "$OUT"
EOF
    guard
    [ "$status" -eq 0 ]
    contains "2 cases" "$output"
}

# A red suite is `make full-test-run`'s finding. The gates still have something to
# say about the report a failing run left behind, so this must not be an error.
@test "a failing suite still yields a usable report" {
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="one" filename="t.cpp"/>\n</Catch2TestRun>\n' > "$OUT"
exit 1
EOF
    guard
    [ "$status" -eq 0 ]
    contains "suite exit 1" "$output"
}

@test "a missing test binary is refused before anything runs" {
    run timeout -s KILL 60 bash "$SCRIPT" "$WORK/not-built" "~[.]" "$REPORT"
    [ "$status" -eq 2 ]
    contains "not an executable" "$output"
}

# ------------------------------------------------------------ the stage in memory

# What the host offers the script: python3 and a memfd.
can_stage() { python3 -c 'import os; os.memfd_create("suite-report")' 2>/dev/null; }

# A fake suite that notes, into $FAKE_SEES.saw, whether the report already
# stands at its path while the suite is running.
fake_suite_that_looks() {
    export FAKE_SEES="$REPORT"
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="one" filename="t.cpp"/>\n' > "$OUT"
if [ -e "$FAKE_SEES" ]; then echo present > "$FAKE_SEES.saw"; else echo absent > "$FAKE_SEES.saw"; fi
printf '<TestCase name="two" filename="t.cpp"/>\n</Catch2TestRun>\n' >> "$OUT"
EOF
}

@test "the report stands at its path only once the run is over" {
    can_stage || skip "no memfd on this host"
    fake_suite_that_looks
    guard
    [ "$status" -eq 0 ]
    contains "report in memory" "$output"
    # While the suite wrote, nothing at the path could be mistaken for its report.
    contains "absent" "$(cat "$REPORT.saw")"
    contains "2 cases" "$output"
    grep -q 'TestCase name="one"' "$REPORT"
    grep -q 'TestCase name="two"' "$REPORT"
}

@test "a stalled run's partial report reaches its path too" {
    can_stage || skip "no memfd on this host"
    stall_after_secs 2
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="the case that never returns" filename="t.cpp">\n<Expression success="true"/>\n' > "$OUT"
sleep 300
EOF
    guard
    [ "$status" -eq 1 ]
    contains "STALLED" "$output"
    grep -q 'TestCase name="the case that never returns"' "$REPORT"
    grep -q '<Expression success="true"/>' "$REPORT"
}

@test "HELIX_REPORT_DIRECT=1 writes the report to its path as the suite runs" {
    export HELIX_REPORT_DIRECT=1
    fake_suite_that_looks
    guard
    [ "$status" -eq 0 ]
    contains "present" "$(cat "$REPORT.saw")"
    contains "2 cases" "$output"
}

@test "with no python3 to open a stage the suite writes the report path itself" {
    mock_command_script python3 'exit 127'
    fake_suite_that_looks
    guard
    [ "$status" -eq 0 ]
    contains "present" "$(cat "$REPORT.saw")"
    contains "2 cases" "$output"
}

@test "a stage that cannot be reached sends the suite to the report path" {
    # The descriptor number the second start trusts, with nothing open on it:
    # what a process without /proc to reach it through has.
    export _SUITE_REPORT_STAGE_FD=9
    fake_suite_that_looks
    guard
    [ "$status" -eq 0 ]
    contains "present" "$(cat "$REPORT.saw")"
    contains "2 cases" "$output"
}

@test "a staged report that cannot reach its path fails as unusable" {
    can_stage || skip "no memfd on this host"
    # A directory stands where the report should be written.
    mkdir "$REPORT"
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="one" filename="t.cpp"/>\n</Catch2TestRun>\n' > "$OUT"
EOF
    guard
    [ "$status" -eq 2 ]
    contains "cannot write" "$output"
}

@test "a TERM to the watch ends the suite before it publishes what the suite had written" {
    can_stage || skip "no memfd on this host"
    export FAKE_PID="$WORK/suite.pid"
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="slow case" filename="t.cpp">\n' > "$OUT"
echo $$ > "$FAKE_PID"
sleep 300
EOF
    # By hand, without `run`: the signal is this test's to send.
    bash "$SCRIPT" "$BIN" "~[.]" "$REPORT" >"$WORK/watch.log" 2>&1 &
    watch=$!
    for _ in $(seq 1 100); do
        [ -s "$FAKE_PID" ] && break
        sleep 0.1
    done
    suite=$(cat "$FAKE_PID")
    kill -TERM "$watch"
    # A watch ended by a signal reports that signal as its status.
    if wait "$watch" 2>/dev/null; then watch_status=0; else watch_status=$?; fi
    [ "$watch_status" -eq 143 ]
    if kill -0 "$suite" 2>/dev/null; then
        kill -9 "$suite" 2>/dev/null
        fail "the suite $suite outlived the watch it was started by"
    fi
    # What the suite had written is there, whole: nothing wrote to it after.
    contains "slow case" "$(cat "$REPORT")"
    contains "report in memory" "$(cat "$WORK/watch.log")"
}

# ------------------------------------------------------------ the run's process group

# Gone: the kill has landed and a parentless child has been reaped. A killed
# child is a zombie until then, and `kill -0` still finds a zombie.
child_is_gone() { # <pid>
    for _ in $(seq 1 50); do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
    done
    return 1
}

# A fake suite that starts a child of its own, records its pid in $FAKE_CHILD,
# and never returns.
fake_suite_with_child() {
    export FAKE_CHILD="$WORK/child.pid"
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="the case that never returns" filename="t.cpp">\n' > "$OUT"
sleep 300 &
echo $! > "$FAKE_CHILD"
wait
EOF
}

# A fake suite that leaves a child behind, records its pid in $FAKE_CHILD, and
# ends.
fake_suite_that_leaves_a_child() {
    export FAKE_CHILD="$WORK/child.pid"
    fake_suite <<'EOF'
printf '<Catch2TestRun name="t">\n<TestCase name="one" filename="t.cpp"/>\n' > "$OUT"
sleep 300 &
echo $! > "$FAKE_CHILD"
EOF
}

@test "a run that ends takes what the suite left behind" {
    fake_suite_that_leaves_a_child
    guard
    [ "$status" -eq 0 ]
    contains "1 cases" "$output"
    grep -q 'TestCase name="one"' "$REPORT"
    child=$(cat "$FAKE_CHILD")
    if ! child_is_gone "$child"; then
        kill "$child" 2>/dev/null
        fail "the suite's child $child outlived the run"
    fi
}

@test "a stalled run ends what the suite started" {
    fake_suite_with_child
    stall_after_secs 2
    guard
    [ "$status" -eq 1 ]
    contains "STALLED" "$output"
    child=$(cat "$FAKE_CHILD")
    if ! child_is_gone "$child"; then
        kill "$child" 2>/dev/null
        fail "the suite's child $child outlived the stalled run"
    fi
}

@test "a signal to the watch ends what the suite started too" {
    fake_suite_with_child
    # By hand, without `run`: the signal is this test's to send.
    bash "$SCRIPT" "$BIN" "~[.]" "$REPORT" >"$WORK/watch.log" 2>&1 &
    watch=$!
    for _ in $(seq 1 100); do
        [ -s "$FAKE_CHILD" ] && break
        sleep 0.1
    done
    child=$(cat "$FAKE_CHILD")
    kill -TERM "$watch"
    # A watch ended by a signal reports that signal as its status.
    if wait "$watch" 2>/dev/null; then :; fi
    if ! child_is_gone "$child"; then
        kill "$child" 2>/dev/null
        fail "the suite's child $child outlived the signalled watch"
    fi
}
