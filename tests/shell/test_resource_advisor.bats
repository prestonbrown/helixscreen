#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/resource-advisor.sh runs as a PreToolUse hook on every Bash call. It
# never blocks: it answers with a suggestion, or with nothing. The share and the
# memory figure come from `helix-claim jobs -v`, stubbed here through
# HELIX_ADVISOR_JOBS_CMD so each case controls how busy the box looks.

load helpers

ADVISOR="scripts/resource-advisor.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    TEST_DIR="$(mktemp -d)"
    export HELIX_ADVISOR_JOBS_CMD="$TEST_DIR/jobs"
    export JOBS_CALLED="$TEST_DIR/jobs.called"
    HELIX_ADVISOR_HOSTS=$(hostname -s)
    export HELIX_ADVISOR_HOSTS
    roomy
}

teardown() {
    rm -rf "$TEST_DIR"
}

# A `helix-claim jobs -v` stand-in: the -v line on stderr, the share on stdout.
stub_jobs() {
    local share="$1" avail="$2" extra="${3:-}"
    cat > "$HELIX_ADVISOR_JOBS_CMD" <<EOF
#!/usr/bin/env bash
touch "$JOBS_CALLED"
echo "ncpu=32 availGB=$avail -> -j$share$extra" >&2
echo "$share"
EOF
    chmod +x "$HELIX_ADVISOR_JOBS_CMD"
}
roomy() { stub_jobs 16 60; }
narrow_share() { stub_jobs 6 60; }
tight_memory() { stub_jobs 16 12; }

advise() {
    local json
    json=$(jq -cn --arg c "$1" '{tool_name: "Bash", tool_input: {command: $c}}')
    run bash -c "printf '%s' \"\$1\" | $ADVISOR" _ "$json"
}

context() {
    printf '%s' "$output" | jq -r '.hookSpecificOutput.additionalContext'
}

# ---------------------------------------------------------------------------
# Silence and speed on ordinary commands
# ---------------------------------------------------------------------------

@test "an ordinary command gets no output and never reads the box" {
    advise "git status --short"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [ ! -e "$JOBS_CALLED" ]
}

@test "a single-tag test run is not heavy" {
    tight_memory
    advise "make t F='[ams]'"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "a command already on the test host, or over ssh, is left alone" {
    tight_memory
    advise "scripts/test-host-run.sh mutate --tests '[ams]'"
    [ -z "$output" ]
    advise "ssh buildbox.invalid make full-test-run"
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# Always on the test host, however quiet thelio is
# ---------------------------------------------------------------------------

@test "addr2line against helix-tests is sent to the test host on a roomy box" {
    advise "addr2line -f -C -e build/bin/helix-tests 0x69125f5"
    [ "$status" -eq 0 ]
    contains "test host" "$(context)"
    contains "addr2line" "$(context)"
    contains "test-host-run.sh exec --build --" "$(context)"
}

@test "gdb against a helix binary points at coredumpctl" {
    advise "gdb -batch -ex bt build/bin/helix-screen core.123"
    contains "coredumpctl info" "$(context)"
    contains "test-host-run.sh exec --build --" "$(context)"
}

# A test-host-run.sh stand-in offering exactly the modes named.
host_modes() {
    { printf '#!/usr/bin/env bash\ncase "$1" in\n'
      for m in "$@"; do printf '    %s)\n        ;;\n' "$m"; done
      printf 'esac\n'; } > "$TEST_DIR/test-host-run.sh"
    export HELIX_ADVISOR_TEST_HOST_RUN="$TEST_DIR/test-host-run.sh"
}

@test "unit-sweep goes to the test host's sweep and says nothing about bats" {
    host_modes sweep
    advise "make unit-sweep"
    contains "test-host-run.sh sweep" "$(context)"
    lacks "make test-shell" "$(context)"
}

@test "full-test-run sends the C++ half to the test host and keeps bats on thelio" {
    host_modes sweep
    advise "make -j full-test-run"
    contains "test-host-run.sh sweep" "$(context)"
    contains "make test-shell" "$(context)"
    lacks "test-host-run.sh full" "$(context)"
}

# full-test-run offloads its own sweep when a test host is configured and
# automatic offload is on; advice to move it would only send it twice.
@test "full-test-run is left alone when it will offload its own sweep" {
    host_modes sweep
    echo "HELIX_TEST_HOST=testhost.invalid" > "$HELIX_BUILD_HOSTS_FILE"
    advise "make full-test-run"
    [ -z "$output" ]
    HELIX_TEST_HOST=fromenv.invalid advise "make -j full-test-run"
    [ -z "$output" ]
}

@test "full-test-run still gets the advice when it will not offload" {
    host_modes sweep
    echo "HELIX_TEST_HOST=testhost.invalid" > "$HELIX_BUILD_HOSTS_FILE"
    HELIX_TEST_HOST_AUTO=0 advise "make full-test-run"
    contains "test-host-run.sh sweep" "$(context)"
    advise "make full-test-run TEST_HOST=0"
    contains "test-host-run.sh sweep" "$(context)"
}

@test "a bare unit-sweep gets the advice even with a test host configured" {
    host_modes sweep
    echo "HELIX_TEST_HOST=testhost.invalid" > "$HELIX_BUILD_HOSTS_FILE"
    advise "make unit-sweep"
    contains "test-host-run.sh sweep" "$(context)"
}

@test "a sweep suggestion points at helix-claim resources" {
    host_modes sweep
    advise "make unit-sweep"
    contains "scripts/helix-claim resources" "$(context)"
}

@test "a full gate is sent to the test host's sweep mode on a roomy box" {
    host_modes sweep
    advise "make -j full-test-run"
    contains "test-host-run.sh sweep" "$(context)"
    contains "mirrors this tree" "$(context)"
}

@test "a full gate is silent while test-host-run has no sweep mode" {
    host_modes test
    advise "make unit-sweep"
    [ -z "$output" ]
}

@test "a mutation run is sent to the test host" {
    advise "make mutate-diff"
    contains "test-host-run.sh mutate" "$(context)"
}

# ---------------------------------------------------------------------------
# Only when thelio is tight
# ---------------------------------------------------------------------------

@test "an explicit -j above the box's -j is flagged" {
    narrow_share
    advise "make -j24"
    contains '-j$(scripts/helix-claim jobs)' "$(context)"
    contains "-j6" "$(context)"
}

@test "an explicit -j at or under the box's -j is not flagged" {
    narrow_share
    advise "make -j4 test"
    [ -z "$output" ]
}

@test "a docker cross build is sent to the test host only when thelio is tight" {
    advise "make snapmaker-u1-docker"
    [ -z "$output" ]
    tight_memory
    advise "make snapmaker-u1-docker"
    contains "test host" "$(context)"
    contains "scripts/helix-claim resources" "$(context)"
}

@test "an idf build in docker is heavy when thelio is tight" {
    tight_memory
    advise "docker run --rm -v \$PWD:/src espressif/idf idf.py build"
    contains "test host" "$(context)"
}

@test "a test binary in a loop is flagged only when thelio is tight" {
    local loop='for i in $(seq 1 200); do ./build/bin/helix-tests "[x]" || f=$((f+1)); done'
    advise "$loop"
    [ -z "$output" ]
    tight_memory
    advise "$loop"
    contains "loop" "$(context)"
}

@test "memory under 16GB counts as tight even with a wide share" {
    tight_memory
    advise "make -j24"
    contains "12GB" "$(context)"
}

# ---------------------------------------------------------------------------
# Contract with Claude Code and with helix-claim
# ---------------------------------------------------------------------------

@test "a suggestion is a PreToolUse context block that does not decide permission" {
    advise "make mutate-diff"
    [ "$status" -eq 0 ]
    [ "$(printf '%s' "$output" | jq -r '.hookSpecificOutput.hookEventName')" = "PreToolUse" ]
    [ "$(printf '%s' "$output" | jq -r '.hookSpecificOutput.permissionDecision // "none"')" = "none" ]
}

@test "extra trailing fields on the jobs line are ignored" {
    stub_jobs 6 60 " shards=3 nice=10"
    advise "make -j24"
    contains "-j6" "$(context)"
}

@test "a missing helix-claim still exits 0 and still sends symbolizers to the test host" {
    export HELIX_ADVISOR_JOBS_CMD="$TEST_DIR/does-not-exist"
    advise "make -j24"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    advise "addr2line -e build/bin/helix-tests 0x1"
    [ "$status" -eq 0 ]
    contains "test host" "$(context)"
}

@test "garbage on stdin exits 0 with no output" {
    run bash -c "printf 'not json' | $ADVISOR"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    # Past the fast path, so jq itself sees the broken input.
    run bash -c "printf 'mutate-diff {' | $ADVISOR"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "without jq it exits 0 with no output" {
    mkdir -p "$TEST_DIR/bin"
    local t
    for t in bash cat sed grep hostname dirname nproc timeout; do
        ln -s "$(command -v "$t")" "$TEST_DIR/bin/$t"
    done
    run env PATH="$TEST_DIR/bin" bash "$ADVISOR" <<< '{"tool_input":{"command":"make mutate-diff"}}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "on a host the advice is not about it is silent" {
    export HELIX_ADVISOR_HOSTS="some-other-box"
    advise "make mutate-diff"
    [ -z "$output" ]
}

@test "a hung helix-claim is cut off and fails open" {
    printf '#!/usr/bin/env bash\nsleep 30\n' > "$HELIX_ADVISOR_JOBS_CMD"
    local start=$SECONDS
    advise "make -j40 x"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [ $((SECONDS - start)) -lt 6 ] || fail "took $((SECONDS - start))s"
}

@test "the jobs line is read the same from a path with spaces" {
    mkdir -p "$TEST_DIR/a b"
    export HELIX_ADVISOR_JOBS_CMD="$TEST_DIR/a b/jobs"
    stub_jobs 6 60
    advise "make -j40 x"
    contains "above the -j6 this box takes" "$(context)"
}

# ---------------------------------------------------------------------------
# Boundaries
# ---------------------------------------------------------------------------

@test "-j equal to the share is fine and one above is flagged" {
    narrow_share
    advise "make -j6 x"
    [ -z "$output" ]
    advise "make -j7 x"
    contains "above the -j6 this box takes" "$(context)"
}

@test "with no pool a narrow -j alone is not tight" {
    # Without a pool the -j is cores capped by memory, so memory already
    # carries the signal; a small -j on a box with room is a small box.
    stub_jobs 2 60
    advise 'for i in 1 2; do ./build/bin/helix-tests x; done'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "15GB available is tight and 16GB is not" {
    local loop='for i in 1 2; do ./build/bin/helix-tests x; done'
    stub_jobs 16 15
    advise "$loop"
    contains "15GB" "$(context)"
    stub_jobs 16 16
    advise "$loop"
    [ -z "$output" ]
}

@test "a plain test-binary run does not read the box" {
    advise "./build/bin/helix-tests '[ams]'"
    [ -z "$output" ]
    [ ! -e "$JOBS_CALLED" ]
}

# ---------------------------------------------------------------------------
# Mentions are not invocations
# ---------------------------------------------------------------------------

@test "a commit message naming heavy commands is silent" {
    tight_memory
    advise 'git commit -m x -m "mutation: make mutate-diff reverted it; make full-test-run green; gdb on helix-tests"'
    [ -z "$output" ]
}

@test "a grep for a heavy command is silent" {
    tight_memory
    advise "grep -rn addr2line scripts/helix-claim && grep -n 'make full-test-run' CLAUDE.md"
    [ -z "$output" ]
}

@test "a loop that only greps bats files is silent" {
    tight_memory
    advise 'for f in tests/shell/*.bats; do grep -c @test $f; done'
    [ -z "$output" ]
}

@test "a heavy command after a harmless one in the same line is caught" {
    advise "cd build && make -j mutate-diff"
    contains "test-host-run.sh mutate" "$(context)"
}

# ---------------------------------------------------------------------------
# Spellings of the real offenders
# ---------------------------------------------------------------------------

@test "a symbolizer by full path or eu- prefix is sent to the test host" {
    advise "/usr/bin/addr2line -e build/bin/helix-tests 0x1"
    contains "test host" "$(context)"
    advise "eu-addr2line -e build/bin/helix-tests 0x1"
    contains "test host" "$(context)"
}

@test "the native ASAN forms are sent to the test host" {
    advise "make SANITIZE=address test"
    contains "test-host-run.sh asan" "$(context)"
    advise "make test-asan"
    contains "test-host-run.sh asan" "$(context)"
}

@test "an ASAN build for another board is not an ASAN run here" {
    tight_memory
    advise "make deploy-pi-asan"
    [ -z "$output" ]
}

@test "every spelling of an oversized -j is flagged" {
    narrow_share
    # The advisor expands $(nproc) on the host, so pin it: a 4-core runner's
    # -j$(nproc) sits under the share of 6 and would not be oversized there.
    mkdir -p "$TEST_DIR/nproc-bin"
    printf '#!/bin/sh\necho 32\n' > "$TEST_DIR/nproc-bin/nproc"
    chmod +x "$TEST_DIR/nproc-bin/nproc"
    PATH="$TEST_DIR/nproc-bin:$PATH"
    for c in 'make -j$(nproc)' 'make -j 32 test' 'make --jobs=32' 'make --jobs 32'; do
        advise "$c"
        contains "above the -j6 this box takes" "$(context)" || fail "missed: $c"
    done
}

@test "a computed or bare -j is left to its source" {
    narrow_share
    for c in 'make -j"$(scripts/helix-claim jobs)"' 'make -j$(scripts/helix-claim jobs) test' 'make -j'; do
        advise "$c"
        [ -z "$output" ] || fail "flagged: $c"
    done
}

@test "an explicit -j above the share is flagged even on a roomy box" {
    advise "make -j24 test"
    contains "above the -j16 this box takes" "$(context)"
}

@test "xargs over the test binary is a loop" {
    tight_memory
    advise "seq 200 | xargs -I{} ./build/bin/helix-tests '[x]'"
    contains "loop" "$(context)"
}

@test "a docker toolchain target is a container build" {
    tight_memory
    advise "make docker-toolchain-k1"
    contains "test host" "$(context)"
}

@test "a huge command returns quickly" {
    local big
    big=$(printf 'make x %.0s' $(seq 1 15000))   # ~105KB, under the 128KB single-argument limit
    local start=$SECONDS
    advise "$big"
    [ "$status" -eq 0 ]
    [ $((SECONDS - start)) -lt 5 ] || fail "took $((SECONDS - start))s"
}

# ---------------------------------------------------------------------------
# With a jobpool: the make shim strips -j, and tightness is the pool's free tokens
# ---------------------------------------------------------------------------

stub_pool() {
    local target="$1" free="$2" avail="$3"
    cat > "$HELIX_ADVISOR_JOBS_CMD" <<EOF
#!/usr/bin/env bash
touch "$JOBS_CALLED"
echo "pool target=$target available=$free availGB=$avail -> -j$target" >&2
echo "$target"
EOF
    chmod +x "$HELIX_ADVISOR_JOBS_CMD"
}

@test "with a live pool an explicit -j says nothing, since the shim strips it" {
    stub_pool 30 0 60
    advise "make -j64"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "with a live pool a container build is tight when few tokens are free" {
    stub_pool 30 3 60
    advise "make pi-docker"
    contains "the build pool has 3 of 30 tokens free" "$(context)"
    contains "Container builds are heavy" "$(context)"
}

@test "with a live pool a heavy runner outside it is named, tight or not" {
    stub_pool 30 20 60
    advise "bats --jobs 32 --no-parallelize-within-files tests/shell/"
    contains "bats --jobs sizes itself to the machine and runs outside the jobpool (20 of 30 tokens free)" "$(context)"
    contains "scripts/helix-claim hold --" "$(context)"
    advise "cd firmware && docker run --rm -v \$PWD:/src espressif/idf:v5.5.5 idf.py build"
    contains "a docker run of idf.py sizes itself" "$(context)"
    contains "scripts/pool-docker.sh" "$(context)"
    advise "ninja -C build"
    contains "ninja sizes itself" "$(context)"
    advise "ls | parallel -j 8 gzip"
    contains "GNU parallel sizes itself" "$(context)"
}

@test "a heavy runner already under the pool, or with no pool live, is silent" {
    stub_pool 30 20 60
    advise "scripts/helix-claim hold -- sh -c 'bats --jobs \"\$JOBPOOL_SLOTS\" tests/shell/'"
    [ -z "$output" ]
    advise "scripts/pool-docker.sh docker run --rm espressif/idf:v5.5.5 idf.py build"
    [ -z "$output" ]
    advise "bats tests/shell/test_foo.bats"
    [ -z "$output" ]
    roomy
    advise "bats --jobs 32 tests/shell/"
    [ -z "$output" ]
}

@test "with a live pool and tokens to spare a container build is silent" {
    stub_pool 30 20 60
    advise "make pi-docker"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "a pool with free tokens is still tight on memory" {
    stub_pool 30 20 12
    advise "for i in 1 2 3; do ./build/bin/helix-tests '[ams]'; done"
    contains "12GB available" "$(context)"
}

@test "without jobpool installed the real helix-claim still judges -j" {
    unset HELIX_ADVISOR_JOBS_CMD HELIX_JOBPOOL
    unset -f jobpool 2>/dev/null || true
    local d p=""
    local IFS=:
    for d in $PATH; do [ -e "$d/jobpool" ] || p=${p:+$p:}$d; done
    unset IFS
    local json
    json=$(jq -cn --arg c "make -j9999" '{tool_name: "Bash", tool_input: {command: $c}}')
    run env -u JOBPOOL PATH="$p" bash -c "printf '%s' \"\$1\" | $ADVISOR" _ "$json"
    [ "$status" -eq 0 ]
    contains "-j9999 is above the -j" "$(context)"
}

# ---------------------------------------------------------------------------
# `helix-claim jobs` is make's -j, never a size for other runners
# ---------------------------------------------------------------------------

@test "a non-make runner sized from helix-claim jobs is pointed at hold" {
    for c in \
        'docker run --rm --cpus $(scripts/helix-claim jobs) helixscreen/toolchain-pi make' \
        'docker run --rm --cpus "$(helix-claim jobs)" img' \
        'ninja -C build -j$(scripts/helix-claim jobs)' \
        'ls | parallel -j "$(scripts/helix-claim jobs)" gzip' \
        'find . -name x | xargs -P $(helix-claim jobs) -n1 gzip' \
        'cd firmware && idf.py -j $(scripts/helix-claim jobs) build' \
        'bats --jobs "$(scripts/helix-claim jobs)" tests/shell/'; do
        advise "$c"
        [ "$status" -eq 0 ] || fail "status $status: $c"
        contains "is make's -j" "$(context)" || fail "missed: $c"
        contains "scripts/helix-claim hold [--min M] --" "$(context)" || fail "no hold: $c"
    done
    advise 'docker run --rm --cpus $(scripts/helix-claim jobs) img'
    contains "docker --cpus sized from it" "$(context)"
    contains "scripts/pool-docker.sh" "$(context)"
}

@test "make sized from helix-claim jobs, and a mention of it, are silent" {
    for c in \
        'make -j$(scripts/helix-claim jobs)' \
        'make -j"$(helix-claim jobs)" test' \
        'make pi-docker NPROC_DOCKER_RUN=$(scripts/helix-claim jobs)' \
        'git commit -m "never use docker --cpus $(helix-claim jobs)"' \
        "grep -n 'ninja -j\$(helix-claim jobs)' notes.txt" \
        'scripts/helix-claim jobs'; do
        advise "$c"
        [ -z "$output" ] || fail "flagged: $c"
    done
}
