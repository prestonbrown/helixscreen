#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# `helix-claim jobs` is the -j a build takes when nothing else decides it: the
# Makefile's JOBS, the unit sweep's shard concurrency, the commit hook's build
# and the resource advisor all read it. With a jobpool daemon live it reports
# the machine pool; without jobpool (CI, a Mac, a contributor's box) it reports
# the cores capped by memory, and it always exits 0 with a usable number.
#
# The pool is always a fake named through HELIX_JOBPOOL; no test reaches the
# real one.

bats_require_minimum_version 1.5.0
load helpers

CLAIM="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/helix-claim"

setup() {
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    mkdir -p "$HELIX_CLAIM_DIR"
    unset JOBPOOL
    FAKE="$BATS_TEST_TMPDIR/fake-jobpool"
    export HELIX_JOBPOOL="$FAKE"
}

# fake_pool TARGET AVAILABLE: a jobpool whose daemon is up with these numbers.
# Every call is logged to $FAKE_LOG, so a test can say which commands ran.
fake_pool() {
    export FAKE_LOG="$BATS_TEST_TMPDIR/fake-jobpool.log"
    cat > "$FAKE" <<FAKE_EOF
#!/bin/sh
echo "\$1" >> "\$FAKE_LOG"
case "\$1" in
    ensure) echo /fake/fifo ;;
    target) echo $1 ;;
    status) echo '{"running":true,"pid":1,"target":$1,"available":$2,"consumers":3,"holders":3,"outstanding":5,"last_reset":null}' ;;
    *) exit 2 ;;
esac
FAKE_EOF
    chmod +x "$FAKE"
}

# A PATH holding no jobpool at all, for the box that never installed it.
path_without_jobpool() {
    local d p=""
    local IFS=:
    for d in $PATH; do
        [ -e "$d/jobpool" ] && continue
        p=${p:+$p:}$d
    done
    printf '%s' "$p"
}

@test "a live pool is the answer, and -v names it" {
    fake_pool 30 4
    run --separate-stderr "$CLAIM" jobs -v
    [ "$status" -eq 0 ]
    [ "$output" = "30" ]
    contains "pool target=30 available=4" "$stderr"
    contains "-> -j30" "$stderr"
    contains "availGB=" "$stderr"
}

@test "plain jobs asks only the pool's target, never status or ensure" {
    # status drains the FIFO to count it, and every make runs jobs.
    fake_pool 30 4
    run --separate-stderr "$CLAIM" jobs
    [ "$status" -eq 0 ]
    [ "$output" = "30" ]
    [ "$(cat "$FAKE_LOG")" = "target" ]
}

@test "jobs outside a make points other work at hold, and stdout stays the number" {
    fake_pool 30 4
    unset MAKEFLAGS
    run --separate-stderr "$CLAIM" jobs
    [ "$status" -eq 0 ]
    [ "$output" = "30" ]
    contains "jobs is make's -j (the whole pool, 30)" "$stderr"
    contains "scripts/helix-claim hold" "$stderr"
    contains "pool-docker.sh" "$stderr"
}

@test "jobs under a make's jobserver, with -v, or with no pool says nothing extra" {
    fake_pool 30 4
    MAKEFLAGS=" -j30 --jobserver-auth=fifo:/tmp/x" run --separate-stderr "$CLAIM" jobs
    [ "$output" = "30" ]
    [ -z "$stderr" ]
    MAKEFLAGS="rR -- --jobserver-fds=3,4 -j" run --separate-stderr "$CLAIM" jobs
    [ -z "$stderr" ]

    unset MAKEFLAGS
    run --separate-stderr "$CLAIM" jobs -v
    [ "$output" = "30" ]
    lacks "whole pool" "$stderr"

    JOBPOOL=0 run --separate-stderr "$CLAIM" jobs
    [ "$status" -eq 0 ]
    [ -z "$stderr" ]
}

@test "jobs -v lets status finish when it is killed mid-count" {
    # A status interrupted between draining and refilling loses the tokens;
    # the advisor runs jobs -v under timeout 2.
    cat > "$FAKE" <<'FAKE_EOF'
#!/bin/sh
case "$1" in
    target) echo 30 ;;
    status) sleep 1; echo '{"available":4}'; touch "$BATS_TEST_TMPDIR/status-done" ;;
esac
FAKE_EOF
    chmod +x "$FAKE"
    run timeout 0.3 "$CLAIM" jobs -v
    [ "$status" -eq 124 ]
    local _
    for _ in $(seq 30); do [ -e "$BATS_TEST_TMPDIR/status-done" ] && break; sleep 0.1; done
    [ -e "$BATS_TEST_TMPDIR/status-done" ]
}

@test "the pool subcommand prints the target, and exits 1 with none" {
    fake_pool 12 4
    run "$CLAIM" pool
    [ "$status" -eq 0 ]
    [ "$output" = "12" ]
    JOBPOOL=0 run "$CLAIM" pool
    [ "$status" -eq 1 ]
    [ -z "$output" ]
    printf '#!/bin/sh\nexit 1\n' > "$FAKE"
    run "$CLAIM" pool
    [ "$status" -eq 1 ]
}

@test "JOBPOOL=0 bypasses the pool here as it does in the shim" {
    fake_pool 999 4
    JOBPOOL=0 run --separate-stderr "$CLAIM" jobs -v
    [ "$status" -eq 0 ]
    [ "$output" != "999" ]
    contains "ncpu=" "$stderr"
}

@test "a pool that will not start falls back to the cores" {
    printf '#!/bin/sh\nexit 1\n' > "$FAKE"; chmod +x "$FAKE"
    run --separate-stderr "$CLAIM" jobs -v
    [ "$status" -eq 0 ]
    contains "ncpu=" "$stderr"
    [ "$output" -ge 1 ]
}

@test "a target it cannot read falls back to the cores" {
    printf '#!/bin/sh\necho not a number\n' > "$FAKE"; chmod +x "$FAKE"
    run --separate-stderr "$CLAIM" jobs -v
    [ "$status" -eq 0 ]
    contains "ncpu=" "$stderr"
}

@test "without jobpool installed it exits 0 with a sane -j" {
    unset HELIX_JOBPOOL
    unset -f jobpool 2>/dev/null || true
    run --separate-stderr env -u JOBPOOL PATH="$(path_without_jobpool)" "$CLAIM" jobs -v
    [ "$status" -eq 0 ]
    [ "$output" -ge 1 ]
    [ "$output" -le "$(nproc)" ]
    contains "ncpu=$(nproc) " "$stderr"
    contains "-> -j$output" "$stderr"
}

@test "the fallback is bounded by the cores" {
    mock_command nproc 3
    JOBPOOL=0 run "$CLAIM" jobs
    [ "$status" -eq 0 ]
    [ "$output" -le 3 ]
    [ "$output" -ge 2 ]
}

@test "a one-core box gets -j1, not the floor of 2" {
    mock_command nproc 1
    JOBPOOL=0 run "$CLAIM" jobs
    [ "$status" -eq 0 ]
    [ "$output" = "1" ]
}

# --- The callers read the same decision -------------------------------------

REPO="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"

qc_jobs() {
    (cd "$REPO" && . scripts/qc/_lib.sh && qc_build_jobs)
}

@test "the commit hook's -j is the pool when one is live" {
    fake_pool 30 4
    run qc_jobs
    [ "$status" -eq 0 ]
    [ "$output" = "30" ]
    HELIX_QC_JOBS=3 run qc_jobs
    [ "$output" = "3" ]
}

@test "the commit hook's -j without jobpool installed is a sane number" {
    unset HELIX_JOBPOOL
    unset -f jobpool 2>/dev/null || true
    PATH="$(path_without_jobpool)" run qc_jobs
    [ "$status" -eq 0 ]
    [ "$output" -ge 1 ]
    [ "$output" -le "$(nproc)" ]
}

# make, with no jobpool shim in front of it, printing what the sweep and the
# build would size from.
sizes() {
    (cd "$REPO" && env PATH="$(path_without_jobpool)" make --no-print-directory \
        --eval 'jp-sizes: ; @echo SHARDS=$(SHARD_CONCURRENCY) JOBS=$(JOBS)' jp-sizes)
}

@test "the sweep runs 3 shards per pool token when a pool is live" {
    fake_pool 7 7
    run sizes
    [ "$status" -eq 0 ]
    contains "SHARDS=21 JOBS=7" "$output"
}

@test "the sweep and the build size themselves without jobpool installed" {
    unset HELIX_JOBPOOL
    unset -f jobpool 2>/dev/null || true
    run sizes
    [ "$status" -eq 0 ]
    local j=${output##*JOBS=}
    [ "$j" -ge 1 ]
    [ "$j" -le "$(nproc)" ]
    contains "SHARDS=$((j * 3)) " "$output"
}

# make's two-phase re-invoke (mk/rules.mk `all:`, mk/tests.mk `$(TEST_BIN)`),
# dry-run with the sub-make replaced by an echo of its command line, and clear
# of any jobserver the suite itself runs under (`make test-shell`).
reinvoke() {
    (cd "$REPO" && env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL PATH="$(path_without_jobpool)" \
        make -n --no-print-directory MAKE='echo SUBMAKE' "$@" 2>&1)
}

@test "a make that already has a jobserver never asks helix-claim for a -j" {
    fake_pool 7 7
    run reinvoke -j3 all
    [ "$status" -eq 0 ]
    contains "SUBMAKE _PARALLEL_CHECKED=1 all" "$output"
    run reinvoke -j3 build/bin/helix-tests
    [ "$status" -eq 0 ]
    contains "SUBMAKE _PARALLEL_GUARD=1 --no-print-directory build/bin/helix-tests" "$output"
    [ ! -e "$FAKE_LOG" ]
}

@test "a make with no jobserver takes its -j from helix-claim, or from JOBS" {
    fake_pool 7 7
    run reinvoke all
    contains "SUBMAKE _PARALLEL_CHECKED=1 -j7 all" "$output"
    run reinvoke build/bin/helix-tests
    contains "SUBMAKE _PARALLEL_GUARD=1 --no-print-directory -j7 build/bin/helix-tests" "$output"
    rm -f "$FAKE_LOG"
    run reinvoke JOBS=5 all
    contains "SUBMAKE _PARALLEL_CHECKED=1 -j5 all" "$output"
    [ ! -e "$FAKE_LOG" ]
}

# The Makefile's JOBS_QUERY shell text, run in a scratch tree whose
# scripts/helix-claim prints $1 (a printf format) and exits $2.
jobs_query_with() {
    local dir="$BATS_TEST_TMPDIR/query" q
    mkdir -p "$dir/scripts"
    printf '#!/bin/sh\nprintf '\''%s'\''\nexit %s\n' "$1" "$2" > "$dir/scripts/helix-claim"
    chmod +x "$dir/scripts/helix-claim"
    q=$(cd "$REPO" && env PATH="$(path_without_jobpool)" make --no-print-directory -n \
        --eval 'jq-show: ; $(info JOBS_QUERY:$(JOBS_QUERY))' jq-show 2>/dev/null |
        sed -n 's/^JOBS_QUERY://p')
    [ -n "$q" ]
    (cd "$dir" && sh -c "$q")
}

@test "the Makefile takes helix-claim's -j only when it is one positive integer" {
    local cores
    cores=$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    run jobs_query_with '7\n' 0
    [ "$output" = "7" ]
    # A number and then a failure, as from a script its shell cannot parse:
    # a second word after -jN is a goal to make.
    run jobs_query_with '7\n' 1
    [ "$output" = "$cores" ]
    run jobs_query_with '7\n7\n' 0
    [ "$output" = "$cores" ]
    run jobs_query_with '' 1
    [ "$output" = "$cores" ]
    run jobs_query_with '0\n' 0
    [ "$output" = "$cores" ]
}
