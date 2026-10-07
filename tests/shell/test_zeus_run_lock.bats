#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# zeus-run.sh serializes jobs on the remote workdir (scripts/zeus-run.sh).
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
# under test is the lock protocol itself. ZEUS_LOCK_DIR points the lock at
# the per-test sandbox, the same way ZEUS_WORKDIR and TMPDIR do.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
SCRIPT="$WORKTREE_ROOT/scripts/zeus-run.sh"

setup() {
    load helpers
    export ZEUS_LOCK_DIR="$BATS_TEST_TMPDIR"
    export TMPDIR="$BATS_TEST_TMPDIR"
    export ZEUS_WORKDIR="$BATS_TEST_TMPDIR/work/helixscreen"
    export MOCK_DOCKER_LOG="$BATS_TEST_TMPDIR/docker.log"

    # ssh <host> bash -se: drop the host argument and run the heredoc locally.
    mock_command_script ssh 'shift; exec "$@"'
    # sudo -n <cmd>: plain passthrough.
    mock_command_script sudo 'shift; exec "$@"'
    # The three local git calls: identity and the is-it-pushed check. A fake
    # sha keeps the test green on an unpushed HEAD.
    mock_command_script git '
case "$*" in
    "rev-parse HEAD") echo 0123456789abcdef0123456789abcdef01234567 ;;
    "rev-parse --short HEAD") echo 0123456 ;;
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
    ps) echo helix-tsan ;;
    start) exit 0 ;;
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
            *) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG" ;;
        esac ;;
    *) echo "unhandled docker call: $*" >&2; exit 1 ;;
esac
exit 0'
}

# Pins the lock naming rule: directory overridable, file named after the
# workdir. If the script renames its lock, these tests must follow on purpose.
lock_file() {
    printf '%s/helix-zeus-run-%s.lock\n' "$ZEUS_LOCK_DIR" "$(basename "$ZEUS_WORKDIR")"
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
    run "$SCRIPT" test
    [ "$status" -eq 0 ]
    # No contention: the run never announces a holder other than itself.
    lacks "busy:" "$output"

    wait_for_line "held by pid" "$(lock_file)"
    grep -Eq 'held by pid [0-9]+: test 0123456 since ' "$(lock_file)"

    # The lock is advisory state, not liveness: the file outlives the run,
    # but an immediate exclusive flock must succeed.
    flock -n "$(lock_file)" -c true

    # Successive runs rewrite the holder line rather than appending.
    run "$SCRIPT" test
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

    "$SCRIPT" test >"$out" 2>&1 &
    runner=$!

    # The run names the holder from the lock file and stays blocked: every
    # line that follows the lock (job sizing onward) is still absent.
    wait_for_line "busy:" "$out"
    grep -q "busy: held by pid.*: mutate 0123456 since " "$out"
    refute_grep "MemAvailable" "$out"

    kill "$holder"
    wait "$holder" 2>/dev/null || true
    wait "$runner"
    grep -q "free; continuing" "$out"
}

@test "a run waits out an orphaned container build before touching the tree" {
    # A build left running in the container by a run whose ssh side died
    # holds no lock; the next run must not reset the tree under it.
    export ZEUS_ORPHAN_POLL_SECS=0
    export MOCK_PGREP_HITS="$BATS_TEST_TMPDIR/pgrep-hits"

    run "$SCRIPT" test
    [ "$status" -eq 0 ]
    contains "orphaned build still running in helix-tsan; waiting" "$output"
    # Two polls: the first sees the make, the second sees it gone.
    [ "$(cat "$MOCK_PGREP_HITS")" -eq 2 ]
    # The git sequence ran, and only after the wait cleared.
    grep -qF "git reset" "$MOCK_DOCKER_LOG"
}

@test "the orphan probe counts only live makes, never a zombie" {
    # An unreaped zombie make never exits, so a probe that matched it would
    # block every later run.
    export MOCK_PGREP_LOG="$BATS_TEST_TMPDIR/pgrep-log"

    run "$SCRIPT" test
    [ "$status" -eq 0 ]
    grep -qF "pgrep -x -r R,S,D,T,t make" "$MOCK_PGREP_LOG"
}

@test "the checkout's local main is brought level with origin/main before the reset" {
    # mutate_diff.py's default base reads the local main; a stale one yields a
    # base that refuses the run.
    run "$SCRIPT" mutate
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
    run "$SCRIPT" test
    [ "$status" -eq 0 ]
    local reset_line reapply_line
    reset_line=$(grep -nF "git reset" "$MOCK_DOCKER_LOG" | head -1 | cut -d: -f1)
    reapply_line=$(grep -nF "make reapply-patches" "$MOCK_DOCKER_LOG" | head -1 | cut -d: -f1)
    [ -n "$reapply_line" ]
    [ "$reset_line" -lt "$reapply_line" ]
}

@test "tsan with a tag runs the single-test target" {
    run "$SCRIPT" tsan '[ams]'
    grep -qF 'make test-tsan-one TEST="[ams]"' "$MOCK_DOCKER_LOG"
}

@test "tsan with no tag runs the full target" {
    run "$SCRIPT" tsan
    grep -qE 'make test-tsan -j' "$MOCK_DOCKER_LOG"
    ! grep -q 'test-tsan-one' "$MOCK_DOCKER_LOG"
}

@test "a tsan run with no Catch2 summary is not a pass" {
    run "$SCRIPT" tsan '[ams]'
    [ "$status" -eq 1 ]
    has "not a clean TSAN result" "$output"
}
