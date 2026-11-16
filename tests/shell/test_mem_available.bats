#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/lib/mem_available.sh: MemAvailable, bounded by the cgroup's memory
# limit, read from fixture files so no test depends on the host it runs on.

load helpers
bats_require_minimum_version 1.5.0

LIB="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/lib/mem_available.sh"
CLAIM="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/helix-claim"

setup() {
    export HELIX_MEMINFO="$BATS_TEST_TMPDIR/meminfo"
    export HELIX_CGROUP_ROOT="$BATS_TEST_TMPDIR/cgroup"
    mkdir -p "$HELIX_CGROUP_ROOT"
}

# meminfo KB: the host has KB available.
meminfo() { printf 'MemTotal:       263000000 kB\nMemAvailable:   %s kB\n' "$1" > "$HELIX_MEMINFO"; }
# cgroup MAX ANON: a limit of MAX bytes ("max" for none) holding ANON bytes.
cgroup() {
    printf '%s\n' "$1" > "$HELIX_CGROUP_ROOT/memory.max"
    printf 'anon %s\nfile 900000000000\nshmem 0\n' "$2" > "$HELIX_CGROUP_ROOT/memory.stat"
}
available() { sh -c '. "$1" && helix_mem_available_kb' _ "$LIB"; }

@test "with no limit the host's MemAvailable is the answer" {
    meminfo 167000000
    cgroup max 5000000000
    run available
    [ "$output" = "167000000" ]
}

@test "with no cgroup files the host's MemAvailable is the answer" {
    meminfo 167000000
    run available
    [ "$output" = "167000000" ]
}

@test "a limit smaller than the host's memory bounds the answer, less what the cgroup holds" {
    meminfo 167000000
    cgroup 103079215104 14000000000
    run available
    [ "$output" = "$(( (103079215104 - 14000000000) / 1024 ))" ]
}

@test "the cgroup's file cache does not count against it" {
    meminfo 167000000
    cgroup 10737418240 0
    run available
    [ "$output" = "10485760" ]
}

@test "a host with less free memory than the limit allows answers with the host's figure" {
    meminfo 2000000
    cgroup 103079215104 0
    run available
    [ "$output" = "2000000" ]
}

@test "a cgroup already over its limit answers zero" {
    meminfo 167000000
    cgroup 1073741824 2147483648
    run available
    [ "$output" = "0" ]
}

@test "the helper runs under BusyBox sh, as it does piped into the test container" {
    command -v busybox >/dev/null 2>&1 || skip "busybox not installed"
    meminfo 167000000
    cgroup 10737418240 0
    run busybox sh -c '. "$1" && helix_mem_available_kb' _ "$LIB"
    [ "$output" = "10485760" ]
}

@test "helix-claim jobs takes no more jobs than a container's limit allows" {
    export HELIX_JOBPOOL="$BATS_TEST_TMPDIR/no-jobpool"
    unset JOBPOOL
    meminfo 167000000
    cgroup 3221225472 0
    run --separate-stderr "$CLAIM" jobs -v
    [ "$status" -eq 0 ]
    contains "availGB=3 " "$stderr"
    ncpu=$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    expect=3
    [ "$ncpu" -lt 3 ] && expect=$ncpu
    [ "$output" = "$expect" ] || fail "jobs: $output (ncpu $ncpu)"
}
