#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# triage_shard.sh BIN DIR SHARD COUNT FILTER [ORDER_FLAGS...]
#
# Post-mortem for one unit-sweep shard that failed, crashed or timed out
# (`diagnose_shards` in mk/tests.mk calls it per suspect shard). DIR holds the
# shard's console log as DIR/SHARD.log and the binary's last progress record
# (HELIX_TEST_PROGRESS_FILE) as DIR/SHARD.progress; FILTER and ORDER_FLAGS are
# what the sweep passed, so `--shard-count COUNT --shard-index SHARD` selects
# the same cases in the same order.
#
# It names the failing case(s) and re-runs them in up to four ways, cheapest
# first, stopping at the first that fails:
#
#   alone              the failing cases with nothing else in the process
#   alone, pinned      the same on one CPU (taskset -c)
#   composition        the whole shard again, same cases and order
#   composition, pinned
#
# and prints one verdict:
#
#   fails alone                      a real bug in the case or the code it covers
#   fails only pinned                timing: it needs a slow or contended CPU
#   fails only in shard composition  order dependence or state leaked by an
#                                    earlier case in the shard
#   did not reproduce
#
# An idle machine with free cores is the condition a timing flake never fails
# under, which is why the pinned runs exist: one core makes every thread the
# test starts compete with it, as a loaded sweep does.
#
# Bounded: each run is capped by TRIAGE_TIMEOUT seconds (default 300, the
# sweep's SHARD_TIMEOUT) and this shard's triage by TRIAGE_BUDGET (default
# twice that); TRIAGE_DEADLINE (epoch seconds, set by the sweep from
# TRIAGE_TOTAL_BUDGET) caps every shard's triage together. A stage that would
# start past either is skipped and named. A run that hits the timeout is
# reported as timed out, never as a reproduction: a hang says nothing about
# whether the case fails. TRIAGE_RETRIES (default 3) runs per stage.
# TRIAGE_CPU picks the core (default: the least busy one this process may
# use); the verdict names the core and the load it ran under.
# Without taskset (macOS) the pinned stages are skipped and say so.
#
# Always exits 0: the sweep's own verdict decides whether the run failed.

set -u

if [ $# -lt 5 ]; then
    echo "usage: $0 BIN DIR SHARD COUNT FILTER [ORDER_FLAGS...]" >&2
    exit 2
fi
bin=$1 dir=$2 shard=$3 count=$4 filter=$5
shift 5
order=("$@")

here=$(cd "$(dirname "$0")" && pwd)
retries=${TRIAGE_RETRIES:-3}
run_timeout=${TRIAGE_TIMEOUT:-300}
budget=${TRIAGE_BUDGET:-$((2 * run_timeout))}
if [ -n "${TRIAGE_DEADLINE:-}" ]; then
    left=$((TRIAGE_DEADLINE - $(date +%s)))
    [ "$left" -gt 0 ] || left=0
    [ "$left" -ge "$budget" ] || budget=$left
fi
echo "   re-run timeout ${run_timeout}s, this shard's triage budget ${budget}s"
taskset_cmd=${TRIAGE_TASKSET:-taskset}
log="$dir/$shard.log"
started=$SECONDS

timeout_cmd=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

# Every CPU in an affinity list ("0-31", "3,5-7"), one per line.
expand_cpus() {
    local part
    for part in ${1//,/ }; do
        case $part in
            *-*) seq "${part%-*}" "${part#*-}" ;;
            *[!0-9]* | '') ;;
            *) echo "$part" ;;
        esac
    done
}

# Idle jiffies per CPU from a /proc/stat snapshot: "cpu idle" lines.
cpu_idle() { awk '$1 ~ /^cpu[0-9]+$/ { sub(/^cpu/, "", $1); print $1, $5 + $6 }' "$1" 2>/dev/null; }

# The allowed CPU that sat idle longest across a short sample, or the first
# allowed one when /proc/stat cannot say. TRIAGE_STAT_BEFORE/AFTER replace the
# two samples.
pick_cpu() {
    if [ -n "${TRIAGE_CPU:-}" ]; then
        echo "$TRIAGE_CPU"
        return
    fi
    local allowed before after
    allowed=$(expand_cpus "$("$taskset_cmd" -cp $$ 2>/dev/null | sed -n 's/.*: *//p')")
    [ -n "$allowed" ] || allowed=0
    before=${TRIAGE_STAT_BEFORE:-}
    after=${TRIAGE_STAT_AFTER:-}
    if [ -z "$before" ] && [ -r /proc/stat ]; then
        before=$(mktemp) after=$(mktemp)
        cp /proc/stat "$before"
        sleep 0.25
        cp /proc/stat "$after"
    fi
    local best=""
    if [ -n "$before" ] && [ -n "$after" ]; then
        best=$(awk 'FNR == NR { was[$1] = $2; next }
                    FILENAME == ARGV[2] { now[$1] = $2; next }
                    ($1 in now) && ($1 in was) { d = now[$1] - was[$1]; if (best == "" || d > bestd) { best = $1; bestd = d } }
                    END { print best }' \
            <(cpu_idle "$before") <(cpu_idle "$after") <(printf '%s\n' "$allowed"))
        [ -n "${TRIAGE_STAT_BEFORE:-}" ] || rm -f "$before" "$after"
    fi
    echo "${best:-$(printf '%s\n' "$allowed" | head -1)}"
}

# "loadavg L over N CPUs" for the verdict, or nothing when neither is known.
load_note() {
    local l n
    l=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null || sysctl -n vm.loadavg 2>/dev/null | tr -d '{}' | awk '{ print $1 }')
    n=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null)
    [ -n "$l" ] && echo "loadavg $l over ${n:-?} CPUs"
}

pinned=()
pin_note=""
if command -v "$taskset_cmd" >/dev/null 2>&1; then
    cpu=$(pick_cpu)
    pinned=("$taskset_cmd" -c "$cpu")
    pin_note=" (pinned to CPU $cpu$(l=$(load_note); [ -n "$l" ] && echo ", $l"))"
fi

shard_args=("$filter" ${order[@]+"${order[@]}"} --shard-count "$count" --shard-index "$shard")

# Print a command so it can be pasted back into a shell: plain words as they
# are, anything else single-quoted.
quote() {
    local a out=""
    for a in "$@"; do
        case $a in
            '' | *[!A-Za-z0-9_./:=,+@%-]*) a="'${a//\'/\'\\\'\'}'" ;;
        esac
        out+="$a "
    done
    printf '%s' "${out% }"
}

# --- what the shard ran, and what failed --------------------------------------
listing=$("$bin" "${shard_args[@]}" --list-tests --reporter xml 2>/dev/null)
printf '%s' "$listing" | python3 "$here/catch2_shard_tests.py" > "$dir/$shard.tests" 2>/dev/null || true
printf '%s' "$listing" | python3 "$here/catch2_shard_tests.py" --failed-in "$log" \
    > "$dir/$shard.failed" 2>/dev/null || : > "$dir/$shard.failed"
n=$(wc -l < "$dir/$shard.tests" | tr -d ' ')
nfail=$(wc -l < "$dir/$shard.failed" | tr -d ' ')

echo "  ran ${n:-?} test case(s) -> $dir/$shard.tests ($bin --input-file replays it)"
# Catch2 reports a fatal signal as a FAILED at the running case's TEST_CASE
# line. A SIGTERM there is the shard timeout landing mid-case: the case was
# running when the budget ran out, which says nothing about whether it fails.
failed_at() {
    awk '
        match($0, /^[A-Za-z0-9_\/.-]+\.cpp:[0-9]+: FAILED/) {
            if (loc != "") print loc, kind
            loc = substr($0, 1, RLENGTH - 8); kind = "assert"; seen = 0; next
        }
        loc != "" && ++seen <= 3 && /SIGTERM - Termination request signal/ { kind = "sigterm" }
        END { if (loc != "") print loc, kind }
    ' "$log" 2>/dev/null | awk -v want="$1" '$2 == want { print $1 }' | sort -u | tr '\n' ' '
}
asserts=$(failed_at assert)
killed=$(failed_at sigterm)
if [ -n "$asserts" ]; then
    echo "  failing assertion(s): $asserts"
fi
if [ -n "$killed" ]; then
    echo "  killed by the shard timeout (SIGTERM) while running: $killed"
    echo "     the case was running at the kill, not failing: look at the shard's total time"
fi
if [ -z "$asserts" ] && [ -z "$killed" ]; then
    # Catch2 reports a signal only inside a case body, so a kill or crash
    # anywhere else leaves no marker. The binary's progress record says where.
    progress=$(head -n 1 "$dir/$shard.progress" 2>/dev/null)
    case $progress in
        "running "*)
            echo "  no FAILED marker; died inside: ${progress#running }" ;;
        "after "*)
            echo "  no FAILED marker; died after ${progress#after } returned, in its post-case checks or before the next case" ;;
        run-ended)
            echo "  no FAILED marker; every case had finished: died in exit-time teardown (static destructors)" ;;
        *)
            echo "  no FAILED marker and no progress record ($dir/$shard.progress): cannot say where it died" ;;
    esac
fi
if [ "$nfail" -gt 0 ]; then
    echo "  failing case(s) -> $dir/$shard.failed:"
    sed 's/^/    /' "$dir/$shard.failed"
fi
echo "  shard repro: $(quote "$bin" "${shard_args[@]}")"

# --- re-runs ------------------------------------------------------------------
# stage NAME -- CMD...: run CMD up to $retries times. Prints the number of
# failing runs; keeps the last failing run's output as $dir/$shard.<NAME>.log.
# A run that selected no test case is not a failure of the case, and stops
# the stage (Catch2 says "No test cases matched"). A run the timeout stopped
# (exit 124, or 137 after the -k kill) is counted apart, as timed out.
stage_hits=0
stage_ran=0
stage_timeouts=0
total_timeouts=0
timeout_at=""
stage() {
    local name=$1
    shift 2
    stage_hits=0
    stage_ran=0
    stage_timeouts=0
    local out="$dir/$shard.$name.run.log" i rc limit
    for ((i = 1; i <= retries; i++)); do
        limit=$((budget - (SECONDS - started)))
        [ "$limit" -gt 0 ] || break
        [ "$limit" -lt "$run_timeout" ] || limit=$run_timeout
        if [ -n "$timeout_cmd" ]; then
            "$timeout_cmd" -k 5 "$limit" "$@" > "$out" 2>&1
        else
            "$@" > "$out" 2>&1
        fi
        rc=$?
        if grep -q "No test cases matched" "$out" 2>/dev/null; then
            echo "  (could not select the failing case(s) on their own; $name skipped)"
            break
        fi
        stage_ran=$((stage_ran + 1))
        if [ -n "$timeout_cmd" ] && { [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; }; then
            stage_timeouts=$((stage_timeouts + 1))
            total_timeouts=$((total_timeouts + 1))
            timeout_at=$limit
            mv -f "$out" "$dir/$shard.$name.timeout.log"
        elif [ "$rc" -ne 0 ]; then
            stage_hits=$((stage_hits + 1))
            mv -f "$out" "$dir/$shard.$name.log"
        fi
    done
    rm -f "$out"
    if [ "$stage_ran" -gt 0 ]; then
        if [ "$stage_timeouts" -gt 0 ]; then
            echo "  $name: failed $stage_hits/$stage_ran, $stage_timeouts timed out at ${timeout_at}s"
        else
            echo "  $name: failed $stage_hits/$stage_ran"
        fi
    fi
}

verdict=""
repro=""
repro_log=""
skipped=()
try_stage() { # name description -- cmd...
    local name=$1 desc=$2
    shift 2
    if [ -n "$verdict" ]; then
        return
    fi
    if [ $((SECONDS - started)) -ge "$budget" ]; then
        skipped+=("$name (budget)")
        return
    fi
    stage "$name" "$@"
    if [ "$stage_hits" -gt 0 ]; then
        verdict=$desc
        shift
        repro=$(quote "$@")
        repro_log="$dir/$shard.$name.log"
    fi
}

alone=("$bin" --input-file "$dir/$shard.failed")
whole=("$bin" "${shard_args[@]}")

if [ "$nfail" -gt 0 ]; then
    try_stage alone "fails alone: a real bug in the case or the code it covers" -- "${alone[@]}"
fi
if [ "$nfail" -gt 0 ] && [ ${#pinned[@]} -gt 0 ]; then
    try_stage alone-pinned "fails only pinned: timing, it needs a slow or contended CPU$pin_note" -- \
        "${pinned[@]}" "${alone[@]}"
fi
try_stage composition \
    "fails only in shard composition: order dependence or state leaked by an earlier case" -- \
    "${whole[@]}"
if [ ${#pinned[@]} -gt 0 ]; then
    try_stage composition-pinned \
        "fails only pinned: timing, it needs a slow or contended CPU and this shard's cases$pin_note" -- \
        "${pinned[@]}" "${whole[@]}"
fi

[ ${#pinned[@]} -gt 0 ] || skipped+=("pinned stages ($taskset_cmd not available)")

if [ -n "$verdict" ]; then
    echo "  verdict: $verdict"
    echo "  repro: $repro"
    echo "  repro log: $repro_log"
    tail -25 "$repro_log" 2>/dev/null | sed 's/^/     | /'
else
    echo "  verdict: did not reproduce ($retries run(s) per stage, $((SECONDS - started))s)"
    echo "     An intermittent fault can pass every re-run: this does not prove the diff"
    echo "     is clean. Raise TRIAGE_RETRIES or TRIAGE_BUDGET to sample harder."
fi
if [ "$total_timeouts" -gt 0 ]; then
    echo "  timed out: $total_timeouts run(s) at ${timeout_at}s, counted as neither pass nor fail (a hang;"
    echo "     its output is in $dir/$shard.<stage>.timeout.log)"
fi
if [ ${#skipped[@]} -gt 0 ]; then
    echo "  skipped: ${skipped[*]}"
fi
exit 0
