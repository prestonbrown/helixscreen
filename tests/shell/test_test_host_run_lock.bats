#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test-host-run.sh serializes jobs on the remote workdir (scripts/test-host-run.sh).
#
# A job resets the checkout and rebuilds in $WORKDIR on the container host, so
# two runs in that tree corrupt each other. The remote half of the script
# therefore takes an flock on the host before touching anything, and a second
# run waits, printing who holds the lock and since when. A build orphaned in
# the container by a run whose ssh side died holds no lock at all, so the
# run also polls the container for a live make before touching git.
#
# These tests stub ssh so the remote heredoc runs locally, and stub git,
# sudo and docker so nothing leaves the sandbox and no build runs; what is
# under test is the lock protocol itself. HELIX_TEST_LOCK_DIR points the lock at
# the per-test sandbox, the same way HELIX_TEST_WORKDIR and TMPDIR do.
#
# The tsan cases at the end pin that job's make target and its refusal to
# call a run with no test output clean.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
SCRIPT="$WORKTREE_ROOT/scripts/test-host-run.sh"

setup() {
    load helpers
    export HELIX_TEST_HOST=testhost.invalid
    export HELIX_TEST_LOCK_DIR="$BATS_TEST_TMPDIR"
    export TMPDIR="$BATS_TEST_TMPDIR"
    export HELIX_TEST_WORKDIR="$BATS_TEST_TMPDIR/work/helixscreen"
    export MOCK_DOCKER_LOG="$BATS_TEST_TMPDIR/docker.log"
    # No jobpool on the test host unless a test installs the fake below.
    export HELIX_TEST_JOBPOOL="$BATS_TEST_TMPDIR/no-jobpool"
    # No ZFS on the test host unless a test installs the fake below.
    export HELIX_TEST_ARC_PARAM="$BATS_TEST_TMPDIR/no-zfs/zfs_arc_max"
    export HELIX_TEST_ARC_SYS_FREE="$BATS_TEST_TMPDIR/no-zfs/zfs_arc_sys_free"
    export HELIX_TEST_ARC_MARK="$BATS_TEST_TMPDIR/arc-mark"
    # mutate runs in a checkout per tree beside the mirrors; this one already
    # exists, so no case here copies one (test_test_host_run_seed.bats does).
    export HELIX_TEST_TREES_HOST="$BATS_TEST_TMPDIR/host/trees"
    mkdir -p "$BATS_TEST_TMPDIR/host/mutate/tree-x/.git"

    # ssh [-o opt]... <host> bash -se: drop the options and the host, and run
    # the heredoc locally.
    mock_command_script ssh '
while [ $# -gt 0 ]; do
    case "$1" in -o) shift 2 ;; -*) shift ;; *) break ;; esac
done
shift; exec "$@"'
    # sudo -n <cmd>: plain passthrough.
    mock_command_script sudo 'shift; exec "$@"'
    # The three local git calls: identity and the is-it-pushed check. A fake
    # sha keeps the test green on an unpushed HEAD.
    mock_command_script git '
case "$*" in
    "rev-parse HEAD") echo 0123456789abcdef0123456789abcdef01234567 ;;
    "rev-parse --short HEAD") echo 0123456 ;;
    "rev-parse --show-toplevel") echo "$BATS_TEST_TMPDIR/tree-x" ;;
    "branch -r --contains "*) echo origin/fake ;;
    *) echo "unhandled git call: $*" >&2; exit 1 ;;
esac'
    # docker: enough behavior for the container checks, and an exec that
    # records instead of running, so no git or make ever executes. The pgrep
    # probe is stateful when MOCK_PGREP_HITS names a file: the first poll
    # reports a running make, later polls report none, and the file counts
    # the polls. Without the knob every poll reports no make.
    mock_command_script docker '
case "$1" in
    ps) echo helix-test ;;
    start) exit 0 ;;
    inspect) [ -n "${MOCK_MOUNTS:-}" ] && echo "$MOCK_MOUNTS" ;;
    exec)
        case "$*" in
            *pgrep*)
                [ -n "${MOCK_PGREP_LOG:-}" ] && echo "$*" >> "$MOCK_PGREP_LOG"
                if [ -n "${MOCK_PGREP_HITS:-}" ]; then
                    n=$(cat "$MOCK_PGREP_HITS" 2>/dev/null || echo 0)
                    n=$((n + 1))
                    echo "$n" > "$MOCK_PGREP_HITS"
                    [ "$n" -eq 1 ] && exit 0
                fi
                exit 1
                ;;
            *"base64 -d | sh"*)
                echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG"
                [ -z "${MOCK_CONTAINER_MEM_KB:-}" ] || echo "$MOCK_CONTAINER_MEM_KB"
                ;;
            *) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG" ;;
        esac ;;
    *) echo "unhandled docker call: $*" >&2; exit 1 ;;
esac
exit 0'
}

# Pins the --commit lock name: directory overridable, file named after the
# checkout with the helix-zeus-run- prefix, the one name every client of that
# checkout takes. If the script renames it, these tests must follow on purpose.
lock_file() {
    printf '%s/helix-zeus-run-%s.lock\n' "$HELIX_TEST_LOCK_DIR" "$(basename "$HELIX_TEST_WORKDIR")"
}

wait_for_file() { # <path>
    local _
    for _ in $(seq 1 200); do [ -e "$1" ] && return 0; sleep 0.05; done
    return 1
}

wait_for_line() { # <substring> <file>
    local _
    for _ in $(seq 1 200); do grep -qF "$1" "$2" 2>/dev/null && return 0; sleep 0.05; done
    return 1
}

@test "a solo run takes the workdir lock, records itself, and releases it" {
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    # No contention: the run never announces a holder other than itself.
    lacks "busy:" "$output"

    wait_for_line "held by pid" "$(lock_file)"
    grep -Eq 'held by pid [0-9]+: test 0123456 since ' "$(lock_file)"

    # The lock is advisory state, not liveness: the file outlives the run,
    # but an immediate exclusive flock must succeed.
    flock -n "$(lock_file)" -c true

    # Successive runs rewrite the holder line rather than appending.
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    [ "$(wc -l < "$(lock_file)")" -eq 1 ]
}

@test "a second run waits for the workdir lock and names the holder" {
    local lock ready out holder runner
    lock="$(lock_file)"
    ready="$BATS_TEST_TMPDIR/holder-ready"
    out="$BATS_TEST_TMPDIR/waiting.log"

    # A stand-in holder: takes the lock, records a holder line in the format
    # the script writes, then sleeps. fd 9 is closed for the sleep so killing
    # the holder releases the lock instead of leaving it to the child.
    (
        exec 9>>"$lock"
        flock 9
        printf 'held by pid %s: mutate 0123456 since 2026-09-24 12:00:00\n' "$$" >&9
        touch "$ready"
        sleep 30 9>&-
    ) &
    holder=$!
    wait_for_file "$ready"

    "$SCRIPT" --commit test >"$out" 2>&1 &
    runner=$!

    # The run names the holder from the lock file and stays blocked: every
    # line that follows the lock (job sizing onward) is still absent.
    wait_for_line "busy:" "$out"
    grep -q "busy: held by pid.*: mutate 0123456 since " "$out"
    refute_grep "available to helix-test" "$out"

    kill "$holder"
    wait "$holder" 2>/dev/null || true
    wait "$runner"
    grep -q "free; continuing" "$out"
}

@test "a run waits out an orphaned container build before touching the tree" {
    # A build left running in the container by a run whose ssh side died
    # holds no lock; the next run must not reset the tree under it.
    export HELIX_TEST_ORPHAN_POLL_SECS=0
    export MOCK_PGREP_HITS="$BATS_TEST_TMPDIR/pgrep-hits"

    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    contains "orphaned build still running in helix-test; waiting" "$output"
    # Two polls: the first sees the make, the second sees it gone.
    [ "$(cat "$MOCK_PGREP_HITS")" -eq 2 ]
    # The git sequence ran, and only after the wait cleared.
    grep -qF "git reset" "$MOCK_DOCKER_LOG"
}

@test "the orphan probe counts only live makes, never a zombie" {
    # An unreaped zombie make never exits, so a probe that matched it would
    # block every later run.
    export MOCK_PGREP_LOG="$BATS_TEST_TMPDIR/pgrep-log"

    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    grep -qF "pgrep -x -r R,S,D,T,t make" "$MOCK_PGREP_LOG"
}

@test "-j is sized from what the container says it may allocate" {
    export MOCK_CONTAINER_MEM_KB=$(( 6 * 1048576 ))
    run "$SCRIPT" --commit test '[x]'
    [ "$status" -eq 0 ]
    contains "6GB available to helix-test" "$output"
}

@test "the container's memory probe carries lib/mem_available.sh and runs under sh" {
    run "$SCRIPT" --commit test '[x]'
    [ "$status" -eq 0 ]
    b64=$(sed -n 's/.*echo \([A-Za-z0-9+\/=]*\) | base64 -d | sh.*/\1/p' "$MOCK_DOCKER_LOG" | head -1)
    [ -n "$b64" ] || fail "no probe in: $(cat "$MOCK_DOCKER_LOG")"
    mkdir -p "$BATS_TEST_TMPDIR/cg"
    printf '10737418240\n' > "$BATS_TEST_TMPDIR/cg/memory.max"
    printf 'anon 0\n' > "$BATS_TEST_TMPDIR/cg/memory.stat"
    printf 'MemAvailable: 167000000 kB\n' > "$BATS_TEST_TMPDIR/meminfo"
    run env HELIX_CGROUP_ROOT="$BATS_TEST_TMPDIR/cg" HELIX_MEMINFO="$BATS_TEST_TMPDIR/meminfo" \
        sh -c 'echo "$1" | base64 -d | sh' _ "$b64"
    [ "$output" = "10485760" ]
}

@test "every docker exec names the container and a command" {
    # The heredoc is unquoted, so a backtick or $( ) left unescaped in it runs
    # on the caller's machine before ssh starts.
    run "$SCRIPT" --commit mutate
    [ "$status" -eq 0 ]
    lacks "requires at least" "$output"
    [ -s "$MOCK_DOCKER_LOG" ]
    run grep -v ' helix-test bash -lc ' "$MOCK_DOCKER_LOG"
    [ "$status" -eq 1 ]
}

@test "safe.directory reaches every exec's git without a global config write" {
    # Concurrent runs share one container; a `git config --global` write from
    # each races on /root/.gitconfig.lock and fails one of them.
    run "$SCRIPT" --commit mutate
    [ "$status" -eq 0 ]
    lacks "git config" "$(cat "$MOCK_DOCKER_LOG")"
    # The ccache settings exec (ccache-shared-config.sh, carried base64) runs no git.
    grep -qF "base64 -d" "$MOCK_DOCKER_LOG"
    run sh -c 'grep -vF "base64 -d" "$1" | grep -vF -- "-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0=* "' _ \
        "$MOCK_DOCKER_LOG"
    [ "$status" -eq 1 ]
}

@test "the checkout's local main is brought level with origin/main before the reset" {
    # mutate_diff.py's default base reads the local main; a stale one yields a
    # base that refuses the run.
    run "$SCRIPT" --commit mutate
    [ "$status" -eq 0 ]
    grep -qF "git update-ref refs/heads/main FETCH_HEAD" "$MOCK_DOCKER_LOG"
    local sync_line reset_line
    sync_line=$(grep -nF "git update-ref refs/heads/main" "$MOCK_DOCKER_LOG" | head -1 | cut -d: -f1)
    reset_line=$(grep -nF "git reset" "$MOCK_DOCKER_LOG" | head -1 | cut -d: -f1)
    [ "$sync_line" -lt "$reset_line" ]
}

@test "patches are reapplied after the checkout, before the job runs" {
    # A submodule already at its pin keeps an earlier job's patches; a commit
    # that edits a patch would otherwise fail the build's drift check.
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    local reset_line reapply_line
    reset_line=$(grep -nF "git reset" "$MOCK_DOCKER_LOG" | head -1 | cut -d: -f1)
    reapply_line=$(grep -nF "make reapply-patches" "$MOCK_DOCKER_LOG" | head -1 | cut -d: -f1)
    [ -n "$reapply_line" ]
    [ "$reset_line" -lt "$reapply_line" ]
}

@test "tsan with tags runs test-tsan-one with the tags as one TEST argument" {
    # Stub the Catch2 summary a real tagged run tees into the log.
    mock_command_script docker '
case "$1" in
    ps) echo helix-test ;;
    exec)
        case "$*" in
            *pgrep*) exit 1 ;;
            *test-tsan-one*) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG"; echo "All tests passed (1 assertion in 1 test case)" ;;
            *) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG" ;;
        esac ;;
esac
exit 0'
    run "$SCRIPT" --commit tsan '[ams],[spoolman]'
    [ "$status" -eq 0 ]
    grep -qF 'make test-tsan-one TEST="[ams],[spoolman]"' "$MOCK_DOCKER_LOG"
    refute_grep 'make test-tsan ' "$MOCK_DOCKER_LOG"
}

@test "tsan with no tag runs the sharded suite" {
    mock_command_script docker '
case "$1" in
    ps) echo helix-test ;;
    exec)
        case "$*" in
            *pgrep*) exit 1 ;;
            *"make test-tsan "*) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG"; echo "✓ TSAN clean — no sanitizer reports" ;;
            *) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG" ;;
        esac ;;
esac
exit 0'
    run "$SCRIPT" --commit tsan
    [ "$status" -eq 0 ]
    grep -qF 'make test-tsan $HELIX_JFLAG' "$MOCK_DOCKER_LOG"
    grep -qE 'HELIX_JFLAG=-j[0-9]+ ' "$MOCK_DOCKER_LOG"
    refute_grep 'test-tsan-one' "$MOCK_DOCKER_LOG"
}

@test "a tsan run that printed no Catch2 summary is not reported clean" {
    # The default docker stub runs nothing, so the log carries no summary.
    run "$SCRIPT" --commit tsan '[ams]'
    [ "$status" -eq 1 ]
    contains "not a clean TSAN result" "$output"
}

# A jobpool on the test host whose state dir is $BATS_TEST_TMPDIR/mnt/.jobpool. It
# records exec, and container-env prints a marker naming the dir it was given.
fake_jobpool() {
    export HELIX_TEST_JOBPOOL="$BATS_TEST_TMPDIR/fake-jobpool"
    export MOCK_JOBPOOL_LOG="$BATS_TEST_TMPDIR/jobpool.log"
    cat > "$HELIX_TEST_JOBPOOL" <<'FAKE'
#!/bin/sh
case "$1" in
    ensure) echo "$BATS_TEST_TMPDIR/mnt/.jobpool/fifo" ;;
    container-env) echo "POOLENV:$2" ;;
    status) echo '{"running":true,"target":30,"available":30}' ;;
    exec) echo "jobpool exec" >> "$MOCK_JOBPOOL_LOG"; shift 2; exec "$@" ;;
    *) exit 2 ;;
esac
FAKE
    chmod +x "$HELIX_TEST_JOBPOOL"
}

@test "without jobpool on the test host the job sizes -j from memory" {
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    contains "using -j" "$output"
    grep -qE 'HELIX_JFLAG=-j[0-9]+ helix-test bash -lc make test \$HELIX_JFLAG' "$MOCK_DOCKER_LOG"
}

@test "test hands its arguments to helix-tests, in a run with no mirror" {
    run "$SCRIPT" --commit test -# '[#test_x]'
    [ "$status" -eq 0 ]
    grep -qF './build/bin/helix-tests -# ' "$MOCK_DOCKER_LOG"
    grep -qF 'test_x' "$MOCK_DOCKER_LOG"
}

@test "a quote in an argument survives the jobpool branch's quoting" {
    fake_jobpool
    export MOCK_MOUNTS="$BATS_TEST_TMPDIR/mnt /work"
    run "$SCRIPT" --commit test "it's"
    [ "$status" -eq 0 ]
    [ "$(cat "$MOCK_JOBPOOL_LOG")" = "jobpool exec" ]
    grep -qF "helix-tests it\\'s" "$MOCK_DOCKER_LOG"
}

# The container half run for real: docker exec of the job exports its -e
# variables and runs the bash -lc script here, against a make that prints what
# it was given. container-env opens a real FIFO, so its mode decides whether
# the job joins.
run_container_job() {
    export POOL_FIFO="$BATS_TEST_TMPDIR/pool-fifo"
    mkfifo -m 600 "$POOL_FIFO"
    cat > "$HELIX_TEST_JOBPOOL" <<'FAKE'
#!/bin/sh
case "$1" in
    ensure) echo "$BATS_TEST_TMPDIR/mnt/.jobpool/fifo" ;;
    container-env) echo "exec 3<>$POOL_FIFO 4<>$POOL_FIFO && export MAKEFLAGS=POOLED:$2" ;;
    status) echo '{"running":true,"target":30,"available":30}' ;;
    exec) echo "jobpool exec" >> "$MOCK_JOBPOOL_LOG"; shift 2; exec "$@" ;;
    *) exit 2 ;;
esac
FAKE
    mock_command_script make 'echo "MAKE: $* MAKEFLAGS=${MAKEFLAGS:-}"'
    mock_command_script docker '
case "$1" in
    ps) echo helix-test ;;
    inspect) echo "$MOCK_MOUNTS" ;;
    exec)
        case "$*" in
            *pgrep*) exit 1 ;;
            *unit-sweep*)
                # A container starts from its own environment.
                unset MAKEFLAGS MFLAGS MAKELEVEL
                shift
                while [ "$1" != bash ]; do
                    [ "$1" = -e ] && export "$2"
                    shift
                done
                exec bash -c "$3" ;;
        esac ;;
esac
exit 0'
    export MOCK_MOUNTS="$BATS_TEST_TMPDIR/mnt /work"
}

@test "with jobpool on the test host the container joins it and make gets no -j" {
    fake_jobpool
    run_container_job
    run "$SCRIPT" --commit sweep
    [ "$status" -eq 0 ]
    contains "joining jobpool: target 30" "$output"
    contains "MAKE: unit-sweep NPROCS=96 MAKEFLAGS=POOLED:/work/.jobpool" "$output"
    [ "$(cat "$MOCK_JOBPOOL_LOG")" = "jobpool exec" ]
}

@test "a container uid that cannot open the FIFO runs with its own -j" {
    [ "$(id -u)" != 0 ] || skip "root opens a mode-000 FIFO anyway"
    fake_jobpool
    run_container_job
    chmod 000 "$POOL_FIFO"
    run "$SCRIPT" --commit sweep
    [ "$status" -eq 0 ]
    contains "cannot open the jobpool FIFO; using -j" "$output"
    printf '%s\n' "$output" | grep -qE '^MAKE: unit-sweep NPROCS=96 -j[0-9]+ MAKEFLAGS=$'
}

@test "a pool whose state the container cannot see is not joined" {
    fake_jobpool
    export MOCK_MOUNTS="/elsewhere /data"
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    contains "is not mounted in helix-test; sizing -j from memory" "$output"
    contains "using -j" "$output"
    refute_grep "POOLENV" "$MOCK_DOCKER_LOG"
    [ ! -e "$MOCK_JOBPOOL_LOG" ]
}

@test "a jobpool that will not start is named, and the job sizes -j from memory" {
    fake_jobpool
    printf '#!/bin/sh\nexit 1\n' > "$HELIX_TEST_JOBPOOL"
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    contains "jobpool ensure failed" "$output"
    contains "using -j" "$output"
}

# --- mutate yields to every other job ---------------------------------------

# docker exec runs the mutate job's bash -lc script here for real, against a
# python3 that prints the scheduling it inherited and the jobserver it was handed.
run_mutate_for_real() {
    mock_command_script make 'echo "MAKE: $*" >> "$BATS_TEST_TMPDIR/make.log"'
    mock_command_script python3 'echo "POLICY: $(chrt -p $$)"; echo "NICE: $(nice)"; echo "MAKEFLAGS=${MAKEFLAGS:-}"'
    mock_command_script docker '
case "$1" in
    ps) echo helix-test ;;
    inspect) [ -n "${MOCK_MOUNTS:-}" ] && echo "$MOCK_MOUNTS" ;;
    exec)
        case "$*" in
            *pgrep*) exit 1 ;;
            *mutate_diff*)
                unset MAKEFLAGS MFLAGS MAKELEVEL
                shift
                while [ "$1" != bash ]; do
                    [ "$1" = -e ] && export "$2"
                    shift
                done
                exec bash -c "$3" ;;
            *) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG" ;;
        esac ;;
esac
exit 0'
}

@test "mutate hands each argument through whole, spaces included" {
    run_mutate_for_real
    mock_command_script python3 'case "$*" in *import*) exit 0 ;; esac; printf "ARG<%s>\n" "$@"'
    cd "$BATS_TEST_TMPDIR" || return 1
    run "$SCRIPT" mutate --tests-for 'src/a b.cpp=[one case]'
    [ "$status" -eq 0 ]
    contains "ARG<src/a b.cpp=[one case]>" "$output"
}

@test "mutate makes the checkout's venv when it cannot import pytest" {
    run_mutate_for_real
    # The job's script runs in this directory: one with no venv of its own.
    cd "$BATS_TEST_TMPDIR" || return 1
    run "$SCRIPT" mutate --tests '[x]'
    [ "$status" -eq 0 ]
    run cat "$BATS_TEST_TMPDIR/make.log"
    contains "MAKE: venv-setup" "$output"
}

@test "mutate leaves a checkout venv that imports pytest alone" {
    run_mutate_for_real
    cd "$BATS_TEST_TMPDIR" || return 1
    mkdir -p .venv/bin
    printf '#!/bin/sh\nexit 0\n' > .venv/bin/python3
    chmod +x .venv/bin/python3
    run "$SCRIPT" mutate --tests '[x]'
    [ "$status" -eq 0 ]
    [ ! -e "$BATS_TEST_TMPDIR/make.log" ] || fail "venv-setup ran: $(cat "$BATS_TEST_TMPDIR/make.log")"
}

@test "mutate beside a jobpool runs outside it, at SCHED_IDLE and nice 19" {
    fake_jobpool
    export MOCK_MOUNTS="$BATS_TEST_TMPDIR/mnt /work"
    run_mutate_for_real
    run "$SCRIPT" mutate --tests '[x]'
    [ "$status" -eq 0 ]
    contains "background job: SCHED_IDLE, outside the jobpool" "$output"
    contains "SCHED_IDLE" "$output"
    contains "NICE: 19" "$output"
    printf '%s\n' "$output" | grep -qx 'MAKEFLAGS='
    lacks "could not lower" "$output"
    [ ! -e "$MOCK_JOBPOOL_LOG" ]
}

@test "mutate beside a jobpool takes half the -j free memory allows" {
    fake_jobpool
    export MOCK_MOUNTS="$BATS_TEST_TMPDIR/mnt /work"
    run_mutate_for_real
    run "$SCRIPT" mutate --tests '[x]'
    [ "$status" -eq 0 ]
    full=$(printf '%s\n' "$output" | sed -n 's/.*half of the -j\([0-9]*\) free memory allows.*/\1/p')
    used=$(printf '%s\n' "$output" | sed -n 's/.*using -j\([0-9]*\)$/\1/p')
    [ -n "$full" ]
    [ -n "$used" ]
    [ "$used" -eq $((full / 2)) ]
}

@test "a refused chrt still leaves mutate at nice 19, and is named" {
    run_mutate_for_real
    mock_command_script chrt 'exit 1'
    run "$SCRIPT" mutate --tests '[x]'
    [ "$status" -eq 0 ]
    contains "NICE: 19" "$output"
    contains "could not lower this background job with: chrt;" "$output"
}

@test "mutate beside a jobpool skips the global lock that sizes unpooled runs" {
    fake_jobpool
    export MOCK_MOUNTS="$BATS_TEST_TMPDIR/mnt /work"
    run_mutate_for_real
    exec 9>>"$HELIX_TEST_LOCK_DIR/helix-test-host-run.global.lock"
    flock -n 9
    run timeout 30 "$SCRIPT" mutate --tests '[x]'
    exec 9>&-
    [ "$status" -eq 0 ]
    lacks "waiting for the run ahead" "$output"
    contains "SCHED_IDLE" "$output"
}

@test "mutate on a host without a jobpool still drops to SCHED_IDLE" {
    run_mutate_for_real
    run "$SCRIPT" mutate --tests '[x]'
    [ "$status" -eq 0 ]
    contains "using -j" "$output"
    contains "SCHED_IDLE" "$output"
    contains "NICE: 19" "$output"
}

@test "only mutate yields: a sweep beside the same pool joins it at its own priority" {
    fake_jobpool
    export MOCK_MOUNTS="$BATS_TEST_TMPDIR/mnt /work"
    run "$SCRIPT" --commit sweep
    [ "$status" -eq 0 ]
    lacks "background job" "$output"
    [ "$(cat "$MOCK_JOBPOOL_LOG")" = "jobpool exec" ]
    refute_grep "chrt" "$MOCK_DOCKER_LOG"
}

# --- ZFS: leftover cap marker, zfs_arc_sys_free check ----------------------

fake_zfs() { # <zfs_arc_max> <zfs_arc_sys_free>
    mkdir -p "$BATS_TEST_TMPDIR/zfs"
    export HELIX_TEST_ARC_PARAM="$BATS_TEST_TMPDIR/zfs/zfs_arc_max"
    export HELIX_TEST_ARC_SYS_FREE="$BATS_TEST_TMPDIR/zfs/zfs_arc_sys_free"
    echo "$1" > "$HELIX_TEST_ARC_PARAM"
    echo "$2" > "$HELIX_TEST_ARC_SYS_FREE"
}

SYS_FREE=$((64 * 1024 * 1024 * 1024))

@test "a run leaves zfs_arc_max alone" {
    fake_zfs 269272276992 "$SYS_FREE"
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    [ "$(cat "$HELIX_TEST_ARC_PARAM")" = 269272276992 ]
    lacks "zfs_arc" "$output"
}

@test "a cap marker left by a dead run is restored to the bytes it recorded" {
    local dead
    true & dead=$!; wait "$dead"
    fake_zfs 68719476736 "$SYS_FREE"
    echo "$dead 123456789" > "$HELIX_TEST_ARC_MARK"
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    contains "restored zfs_arc_max to 123456789" "$output"
    [ "$(cat "$HELIX_TEST_ARC_PARAM")" = 123456789 ]
    [ ! -e "$HELIX_TEST_ARC_MARK" ]
}

@test "a cap marker whose run is still live is left to that run" {
    fake_zfs 68719476736 "$SYS_FREE"
    echo "$$ 123456789" > "$HELIX_TEST_ARC_MARK"
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    [ "$(cat "$HELIX_TEST_ARC_PARAM")" = 68719476736 ]
    [ -e "$HELIX_TEST_ARC_MARK" ]
}

@test "a cap marker with no bytes to restore stays and is named" {
    local dead
    true & dead=$!; wait "$dead"
    fake_zfs 68719476736 "$SYS_FREE"
    echo "$dead 0" > "$HELIX_TEST_ARC_MARK"
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    contains "holds no bytes to restore" "$output"
    [ -e "$HELIX_TEST_ARC_MARK" ]
    [ "$(cat "$HELIX_TEST_ARC_PARAM")" = 68719476736 ]
}

@test "zfs_arc_sys_free under the floor is warned about" {
    fake_zfs 269272276992 0
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    contains "zfs_arc_sys_free is 0 bytes" "$output"
}

@test "zfs_arc_sys_free at 64 GiB, or no ZFS at all, is not warned about" {
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    lacks "zfs_arc_sys_free" "$output"
    fake_zfs 269272276992 "$SYS_FREE"
    run "$SCRIPT" --commit test
    [ "$status" -eq 0 ]
    lacks "zfs_arc_sys_free" "$output"
}

@test "--commit excludes a client that still runs zeus-run.sh against the same checkout" {
    # zeus-run.sh locks helix-zeus-run-<checkout>.lock; both reset the checkout.
    local lock="$HELIX_TEST_LOCK_DIR/helix-zeus-run-helixscreen.lock" out runner holder
    (
        exec 9>>"$lock"
        flock 9
        printf 'held by pid %s: mutate abc1234 since 2026-10-08 12:00:00\n' "$$" >&9
        touch "$BATS_TEST_TMPDIR/ready"
        sleep 30 9>&-
    ) &
    holder=$!
    wait_for_file "$BATS_TEST_TMPDIR/ready"
    out="$BATS_TEST_TMPDIR/waiting.log"
    "$SCRIPT" --commit test >"$out" 2>&1 &
    runner=$!
    wait_for_line "busy:" "$out"
    grep -q "mutate abc1234" "$out"
    kill "$holder"; wait "$holder" 2>/dev/null || true
    wait "$runner"
}
