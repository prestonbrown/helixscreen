#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The unit sweep's shard runner (`run_tests_parallel` in mk/tests.mk). With a
# jobpool live it launches shards in batches of three, each batch under one
# `jobpool with-token` held for the batch's whole life, so every sweep on the
# box together runs at most three shards per pool token. Without a pool it
# runs each shard on its own. A shard that exits on a signal after its
# assertions passed warns, unless SHARD_CRASH_FAILS=1 (land-gate) fails it.
# A shard's budget is its timeout stretched by the load other work puts on the
# host (scripts/shard_budget.sh, at most twice), which the sweep's first line
# and each shard header say; the triage's budgets stretch with it.
#
# The test binary and the pool are fakes: the binary prints which token it ran
# under and fails, hangs or crashes on request, and the pool logs every token
# it hands out and takes back.

bats_require_minimum_version 1.5.0
load helpers

setup() {
    unset JOBPOOL
    export HELIX_JOBPOOL="$BATS_TEST_TMPDIR/fake-jobpool"
    export POOL_LOG="$BATS_TEST_TMPDIR/pool.log"
    FAKE_BIN="$BATS_TEST_TMPDIR/fake-tests"
    cat > "$FAKE_BIN" <<'EOF'
#!/bin/sh
i=""
while [ $# -gt 0 ]; do
    [ "$1" = --shard-index ] && i=$2
    shift
done
echo "ran under token=${FAKE_TOKEN:-none}"
[ "$i" = "${FAIL_SHARD:-x}" ] && { echo "test cases: 1 | 1 failed"; exit 1; }
[ -n "${HELIX_TEST_PROGRESS_FILE:-}" ] && echo "running case of shard $i" > "$HELIX_TEST_PROGRESS_FILE"
[ "$i" = "${HANG_SHARD:-x}" ] && sleep 5
echo "All tests passed"
[ "$i" = "${CRASH_SHARD:-x}" ] && exit 139
[ -n "${FAKE_TOKEN:-}" ] && echo "end $FAKE_TOKEN" >> "$POOL_LOG"
exit 0
EOF
    chmod +x "$FAKE_BIN"
}

# A live pool. with-token logs take/give around the command and names the
# token in FAKE_TOKEN so the shards can report which one they ran under.
live_pool() {
    cat > "$HELIX_JOBPOOL" <<'EOF'
#!/bin/sh
case "$1" in
    target) echo 4 ;;
    with-token)
        shift; [ "$1" = -- ] && shift
        n=$(date +%s%N)
        echo "take $n" >> "$POOL_LOG"
        FAKE_TOKEN=$n "$@"; rc=$?
        echo "give $n" >> "$POOL_LOG"
        exit $rc ;;
    *) exit 2 ;;
esac
EOF
    chmod +x "$HELIX_JOBPOOL"
}

# sweep SHARDS [make overrides...]: run the shard runner alone, no build.
sweep() {
    local n=$1; shift
    scratch_make TEST_BIN="$FAKE_BIN" NPROCS="$n" \
        SHARD_ARTIFACT_ROOT="$BATS_TEST_TMPDIR" SHARD_RETRIES=1 "$@" \
        --eval 'sweep-under-test: ; @$(call run_tests_parallel,"~[.]")' sweep-under-test
}

token_of() { # <shard> <output>
    printf '%s\n' "$2" | sed -n "s/^\[shard $1\] ran under token=//p"
}

@test "with a pool, shards run three to a token and every token comes back" {
    live_pool
    run sweep 7
    [ "$status" -eq 0 ]
    contains "batches of 3" "$output"
    [ "$(grep -c '^take' "$POOL_LOG")" -eq 3 ]
    [ "$(grep -c '^give' "$POOL_LOG")" -eq 3 ]
    local i
    for i in 0 1 2 3 4 5 6; do [ -n "$(token_of $i "$output")" ]; done
    [ "$(token_of 0 "$output")" = "$(token_of 2 "$output")" ]
    [ "$(token_of 3 "$output")" = "$(token_of 5 "$output")" ]
    [ "$(token_of 2 "$output")" != "$(token_of 3 "$output")" ]
    [ "$(token_of 6 "$output")" != "$(token_of 5 "$output")" ]
    [ "$(token_of 0 "$output")" != none ]
    # A token goes back only after every shard it covers has finished.
    [ "$(grep -c '^end' "$POOL_LOG")" -eq 7 ]
    run awk '$1 == "give" { gone[$2] = 1 } $1 == "end" && gone[$2] { print "late: " $0 }' "$POOL_LOG"
    [ -z "$output" ]
}

@test "a failing and a timed-out shard are reported, and their tokens come back" {
    live_pool
    FAIL_SHARD=1 HANG_SHARD=4 run sweep 6 SHARD_TIMEOUT=1
    [ "$status" -ne 0 ]
    contains "Shard 1 had test failures" "$output"
    contains "Shard 4 failed (exit 124)" "$output"
    contains "One or more test shards failed" "$output"
    [ "$(grep -c '^take' "$POOL_LOG")" -eq 2 ]
    [ "$(grep -c '^give' "$POOL_LOG")" -eq 2 ]
}

@test "a timed-out shard is named by the case it was running" {
    HANG_SHARD=2 run sweep 3 SHARD_TIMEOUT=1
    [ "$status" -ne 0 ]
    contains "Shard 2 failed (exit 124)" "$output"
    contains "died inside: case of shard 2" "$output"
    lacks "died inside: case of shard 1" "$output"
}

@test "a shard's budget is the timeout itself on an idle host" {
    HELIX_SHARD_HOST_LOAD=0 HELIX_SHARD_HOST_CORES=72 run scripts/shard_budget.sh 300
    [ "$status" -eq 0 ]
    [ "$output" = "300 100 0 72 0" ]
}

@test "a shard's budget stretches with the load other work puts on the host" {
    HELIX_SHARD_HOST_LOAD=11700 HELIX_SHARD_HOST_CORES=72 run scripts/shard_budget.sh 300
    [ "$output" = "492 164 11700 72 0" ]
    HELIX_SHARD_HOST_LOAD=600 HELIX_SHARD_HOST_CORES=4 run scripts/shard_budget.sh 300
    [ "$output" = "525 175 600 4 0" ]
}

@test "the stretch stops at twice, and a capped budget says so" {
    HELIX_SHARD_HOST_LOAD=700 HELIX_SHARD_HOST_CORES=4 run scripts/shard_budget.sh 300
    [ "$output" = "600 200 700 4 0" ]
    HELIX_SHARD_HOST_LOAD=7100 HELIX_SHARD_HOST_CORES=4 run scripts/shard_budget.sh 300
    [ "$output" = "600 200 7100 4 1" ]
    HELIX_SHARD_HOST_LOAD=40000 HELIX_SHARD_HOST_CORES=72 run scripts/shard_budget.sh 300
    [ "$output" = "600 200 40000 72 1" ]
    SHARD_BUDGET_CAP_X100=150 HELIX_SHARD_HOST_LOAD=11700 HELIX_SHARD_HOST_CORES=72 \
        run scripts/shard_budget.sh 300
    [ "$output" = "450 150 11700 72 1" ]
}

@test "without a named load the host's own one-minute average and core count are read" {
    unset HELIX_SHARD_HOST_LOAD HELIX_SHARD_HOST_CORES
    run scripts/shard_budget.sh 300
    [ "$status" -eq 0 ]
    [ "$(cut -d' ' -f4 <<< "$output")" = "$(nproc)" ]
    [[ "$output" =~ ^[0-9]+\ (1[0-9][0-9]|200)\ [0-9]+\ [1-9][0-9]*\ [01]$ ]]
}

@test "the sweep's first line and each shard header say when the host's load stretches the budget" {
    HELIX_SHARD_HOST_LOAD=0 HELIX_SHARD_HOST_CORES=72 run sweep 3
    [ "$status" -eq 0 ]
    contains "(timeout=300s)..." "$output"
    lacks "stretches" "$output"
    contains "=== shard 0/3 " "$output"
    contains "budget=300s" "$output"

    HELIX_SHARD_HOST_LOAD=11700 HELIX_SHARD_HOST_CORES=72 run sweep 3
    [ "$status" -eq 0 ]
    contains "(timeout=300s: host load 117.00 on 72 cores stretches it to 492s)..." "$output"
    contains "budget=492s" "$output"
    lacks "capped" "$output"

    HELIX_SHARD_HOST_LOAD=40000 HELIX_SHARD_HOST_CORES=72 run sweep 3
    [ "$status" -eq 0 ]
    contains "stretches it to 600s, capped at twice" "$output"
}

@test "a shard that only a loaded host starves gets the stretched budget" {
    HANG_SHARD=2 run sweep 3 SHARD_TIMEOUT=4
    [ "$status" -ne 0 ]
    contains "Shard 2 failed (exit 124)" "$output"

    HELIX_SHARD_HOST_LOAD=11700 HELIX_SHARD_HOST_CORES=72 HANG_SHARD=2 run sweep 3 SHARD_TIMEOUT=4
    [ "$status" -eq 0 ]
    contains "stretches it to 7s" "$output"
}

@test "the triage's budgets stretch with the shard's" {
    HELIX_SHARD_HOST_LOAD=11700 HELIX_SHARD_HOST_CORES=72 HANG_SHARD=2 run sweep 3 SHARD_TIMEOUT=1
    [ "$status" -ne 0 ]
    contains "Shard 2 failed (exit 124)" "$output"
    contains "triage: up to 2s per re-run, 1800s for every failed shard together" "$output"
    contains "re-run timeout 2s, this shard's triage budget 4s" "$output"
}

@test "a with-token that cannot start still runs its shards" {
    printf '#!/bin/sh\n[ "$1" = target ] && echo 4 && exit 0\nexit 1\n' > "$HELIX_JOBPOOL"
    chmod +x "$HELIX_JOBPOOL"
    run sweep 4
    [ "$status" -eq 0 ]
    local i
    for i in 0 1 2 3; do [ "$(token_of $i "$output")" = none ]; done
}

@test "without a pool every shard runs on its own, untouched" {
    live_pool
    JOBPOOL=0 run sweep 5
    [ "$status" -eq 0 ]
    lacks "batches of" "$output"
    [ ! -e "$POOL_LOG" ]
    local i
    for i in 0 1 2 3 4; do [ "$(token_of $i "$output")" = none ]; done
}

@test "a shard crashing after its assertions passed warns, and fails under SHARD_CRASH_FAILS=1" {
    JOBPOOL=0 CRASH_SHARD=2 run sweep 4
    [ "$status" -eq 0 ]
    contains "Shard 2 crashed after its assertions passed (signal 11)" "$output"
    lacks "One or more test shards failed" "$output"
    JOBPOOL=0 CRASH_SHARD=2 run sweep 4 SHARD_CRASH_FAILS=1
    [ "$status" -ne 0 ]
    contains "Shard 2 crashed after its assertions passed (signal 11)" "$output"
    contains "One or more test shards failed" "$output"
}
