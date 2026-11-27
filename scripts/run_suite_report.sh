#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Produce the shared Catch2 report that the test-quality gates read, under a
# watch that says where the run is and stops it when it stops progressing.
#
# The report is written as the suite runs, one element per assertion, so its
# byte count is a live progress signal: a growing file means assertions are
# still completing, and a file that stops growing means the run is sitting
# inside a single test case. Without a watch the two are indistinguishable from
# outside — the run has no console output, because test stdout has to be
# discarded to keep ANSI status bytes out of the report — so a stuck case looks
# exactly like a slow suite and the gates that consume the report never run.
#
# Usage:
#   scripts/run_suite_report.sh <test-binary> <catch2-filter> <report-path>
#
# Environment:
#   SUITE_REPORT_STALL_SECS     stop the run after this long with no report
#                               growth (default 300)
#   SUITE_REPORT_MAX_SECS       stop the run after this much wall clock
#                               regardless of progress (default 4800)
#   SUITE_REPORT_PROGRESS_SECS  interval between progress lines (default 30)
#   HELIX_REPORT_DIRECT         1 writes the report to its path as the suite
#                               runs, with no stage in memory (default 0)
#
# Exit status:
#   0  the suite ran to the end and left a non-empty report
#   1  the run stalled or hit the wall-clock cap; the report is partial
#   2  no usable report was produced
#
# A non-zero exit from the suite itself is NOT an error here. A red suite is
# `make full-test-run`'s finding to report, and the gates still have something to say
# about the report a failing run produced.
#
# The suite runs in a process group of its own, and the run ends with that
# group: a stalled run takes everything the suite started with it, and a
# signal that ends this script ends the suite.
#
# The report is staged in memory. Catch2 flushes its --out stream after every
# insertion, so the file takes one write() per token, about one per two bytes.
# On a filesystem where a small write is dear (ZFS on the test host) those
# writes alone make the run several times slower than its tests. So the suite
# writes to a memory-backed file, the watch follows that, and its bytes reach
# the report path once the run has ended, however it ended; a SIGKILL to this
# script loses them. A memfd is a memory-backed file that no mount sizes, and
# only python3 can open one: the script starts itself again through python3
# with the stage on a descriptor, 9, which replaces whatever the caller had
# open there. Without python3 or a memfd, without /proc to reach the stage
# through, or when HELIX_REPORT_DIRECT=1, the suite writes the report path
# directly. A TERM or INT reaches the exit trap, which ends the suite before it
# publishes. A SIGKILL to this script cannot: it leaves the suite running, so
# the starting line names the suite's pid.
# scripts/check_test_order_dependence.py reads the same variable for the
# reports of the runs it starts.

set -uo pipefail

STALL_SECS="${SUITE_REPORT_STALL_SECS:-300}"
MAX_SECS="${SUITE_REPORT_MAX_SECS:-4800}"
PROGRESS_SECS="${SUITE_REPORT_PROGRESS_SECS:-30}"

# Nothing is observed between polls, so the poll can never be coarser than the
# progress interval it has to feed.
POLL_SECS=5
[ "$PROGRESS_SECS" -lt "$POLL_SECS" ] && POLL_SECS="$PROGRESS_SECS"

if [ $# -ne 3 ]; then
    echo "usage: $0 <test-binary> <catch2-filter> <report-path>" >&2
    exit 2
fi

TEST_BIN="$1"
FILTER="$2"
REPORT="$3"

if [ ! -x "$TEST_BIN" ]; then
    echo "run_suite_report: $TEST_BIN is not an executable" >&2
    exit 2
fi

ts() { date '+%Y-%m-%dT%H:%M:%S'; }
say() { echo "$(ts) [suite-report] $*"; }

# BSD stat and GNU stat spell the size flag differently. -L, because the stage
# is reached through /dev/fd, a symlink.
if stat -f %z . >/dev/null 2>&1; then
    file_size() { stat -Lf %z "$1" 2>/dev/null || echo 0; }
else
    file_size() { stat -Lc %s "$1" 2>/dev/null || echo 0; }
fi

mkdir -p "$(dirname "$REPORT")"
rm -f "$REPORT"
RUN_LOG="$REPORT.stdout"

# --- the stage ---------------------------------------------------------------
# python3 runs this script again with the stage open on STAGE_FD. The suite
# writes its report to that descriptor's path; fd 7 is this script's own read
# end on the same file. LIVE is what the watch follows.
STAGE_FD="${_SUITE_REPORT_STAGE_FD:-}"

if [ -z "$STAGE_FD" ] && [ "${HELIX_REPORT_DIRECT:-0}" != 1 ] && [ -e /proc/self/fd ] &&
   python3 -c 'import os; os.memfd_create("suite-report")' 2>/dev/null; then
    export _SUITE_REPORT_STAGE_FD=9
    # The stage is inheritable, so the suite started below reports through it.
    exec python3 -c 'import os, sys
try:
    fd = int(os.environ["_SUITE_REPORT_STAGE_FD"])
    os.dup2(os.memfd_create("suite-report"), fd)
    os.set_inheritable(fd, True)
    os.execvp("bash", ["bash"] + sys.argv[1:])
except OSError as e:
    print(f"run_suite_report: no stage in memory: {e}", file=sys.stderr)
    sys.exit(2)' "$0" "$@"
fi

LIVE="$REPORT"
if [ -n "$STAGE_FD" ]; then
    if exec 7</proc/self/fd/"$STAGE_FD"; then
        LIVE="/dev/fd/7"
    else
        # Open, but nothing here can reach it: the suite writes the report path.
        STAGE_FD=""
    fi
fi

published=""
run_pid=""
stop_suite() {
    # The suite, and in its own process group whatever it started: a case that
    # hung may have left a helper behind, and nothing of the run outlives it.
    [ -n "$run_pid" ] || return 0
    kill -9 "$run_pid" 2>/dev/null
    kill -9 -- "-$run_pid" 2>/dev/null
    wait "$run_pid" 2>/dev/null
}
publish() {
    # A staged run's bytes reach the report path here, once. A direct run has
    # written them there already.
    [ -n "$STAGE_FD" ] && [ -z "$published" ] || return 0
    published=1
    if cat <&7 > "$REPORT"; then
        return 0
    fi
    # A copy that failed leaves nothing for a gate to read as a whole report.
    rm -f "$REPORT" 2>/dev/null
    say "cannot write $REPORT: the staged report did not reach its path"
    exit 2
}
# However this script ends (a verdict, a signal, its last line), the group ends
# before the report is published: a stalled run takes everything the suite
# started with it, and nothing writes to the stage while it is copied. A
# signal cannot reach the suite itself, which runs in another process group.
trap 'stop_suite; publish' EXIT

if [ -n "$STAGE_FD" ]; then
    say "starting $TEST_BIN '$FILTER' (stall ${STALL_SECS}s, cap ${MAX_SECS}s, suite pid $run_pid, report in memory)"
else
    say "starting $TEST_BIN '$FILTER' (stall ${STALL_SECS}s, cap ${MAX_SECS}s, suite pid $run_pid, report at $REPORT)"
fi

# --out is not optional: tests that shell out print ANSI status to stdout, and
# with the report on stdout too the XML comes back complete but unparseable.
# Job control on for this one start puts the suite in a process group of its
# own, so the run can be ended with everything it started. A shell without job
# control reads a background job's input from /dev/null; with it on, that is
# the suite's by hand.
set -m
if [ -n "$STAGE_FD" ]; then
    "$TEST_BIN" "$FILTER" --reporter xml --success --out "/proc/self/fd/$STAGE_FD" \
        >"$RUN_LOG" 2>&1 </dev/null &
else
    "$TEST_BIN" "$FILTER" --reporter xml --success --out "$REPORT" >"$RUN_LOG" 2>&1 </dev/null &
fi
set +m
run_pid=$!

start=$(date +%s)
scanned=0          # bytes of the report already searched for case names
last_size=0
last_growth=$start
next_progress=$((start + PROGRESS_SECS))
current_case="(none started)"
verdict=""

# Names the case the run is inside by scanning only the bytes added since the
# last look, so a single case larger than any fixed window is still attributed.
refresh_current_case() {
    local found
    found=$(tail -c "+$((scanned + 1))" "$LIVE" 2>/dev/null |
        grep -ao '<TestCase name="[^"]*"' | tail -1)
    if [ -n "$found" ]; then
        found="${found#*name=\"}"
        current_case="${found%\"}"
    fi
    scanned="$last_size"
}

while kill -0 "$run_pid" 2>/dev/null; do
    sleep "$POLL_SECS"
    now=$(date +%s)
    size=$(file_size "$LIVE")

    if [ "$size" -gt "$last_size" ]; then
        last_size="$size"
        last_growth="$now"
    fi

    if [ "$now" -ge "$next_progress" ]; then
        refresh_current_case
        say "$((now - start))s  $((last_size / 1048576)) MB  in: $current_case"
        next_progress=$((now + PROGRESS_SECS))
    fi

    if [ $((now - last_growth)) -ge "$STALL_SECS" ]; then
        verdict="stall"
        break
    fi
    if [ $((now - start)) -ge "$MAX_SECS" ]; then
        verdict="cap"
        break
    fi
done

if [ -n "$verdict" ]; then
    refresh_current_case
    elapsed=$(( $(date +%s) - start ))
    if [ "$verdict" = "stall" ]; then
        say "STALLED: the report stopped growing ${STALL_SECS}s ago (elapsed ${elapsed}s)"
    else
        say "TIMED OUT: no result after ${MAX_SECS}s"
    fi
    say "  stalled inside test case: $current_case"
    say "  partial report: $REPORT ($((last_size / 1048576)) MB)"
    say "  suite stdout:   $RUN_LOG"
    say "  re-run that case alone with: $TEST_BIN \"$current_case\""
    exit 1
fi

wait "$run_pid"
suite_rc=$?
elapsed=$(( $(date +%s) - start ))
last_size=$(file_size "$LIVE")

if [ "$last_size" -eq 0 ]; then
    say "no report produced (suite exit $suite_rc); see $RUN_LOG"
    exit 2
fi

# Read from where the suite wrote: the exit trap ends the group, then puts the
# report at its path.
cases=$(grep -ao '<TestCase name=' "$LIVE" | wc -l | tr -d ' ')
say "done in ${elapsed}s: $((last_size / 1048576)) MB, ${cases:-0} cases, suite exit $suite_rc"
exit 0
