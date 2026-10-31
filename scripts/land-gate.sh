#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make land-gate: the pre-landing gate. What CI judges a branch on, run before
# the branch reaches main, so a red CI run is found here instead of on main.
#
#   app-build         make (the app; tests/ui drives the real binary)
#   ci-shards         make unit-sweep as CI_SHARDS shards (default 8: build.yml's
#                     layout, split across its two test legs), the gate's one
#                     C++ run. Which tests share a process decides what a
#                     use-after-free corrupts, so the layout is CI's. The
#                     local sweep's 96 shards are a grouping CI never runs,
#                     so the gate does not run them either.
#                     Each shard gets CI_SHARD_TIMEOUT seconds (default 600,
#                     build.yml's 10-minute test step): an eighth of the suite
#                     runs past the sweep's 300s SHARD_TIMEOUT on a slower host
#                     without hanging. SHARD_CRASH_FAILS=1 fails a shard that
#                     crashes after its assertions passed, as CI's step does.
#   bats              make test-shell BATS_REQUIRED=1 (a missing bats fails the row),
#                     after app-build: its ctl tests run the binary that build links
#   ui                make test-ui-ci: CI's exact tests/ui pytest slice, headless
#   plugin            make test-plugin: the moonraker plugin pytest, also in CI's job
#   python            make test-python: quality.yml's tests/python pytest step
#   theme-tokens      make check-theme-tokens: generated token table vs ui_xml/
#   xml-schema        make check-xml-schema: linter schema snapshot vs a regeneration
#   lint-xml          make lint-xml: CI's XML Lint job, against the committed snapshot
#   lvgl-event-codes  make check-lvgl-event-codes: crash worker table vs lv_event_code_t
#   esp32             esp32_syntax_check.py --require over the branch since its
#                     merge-base with LAND_GATE_BASE_REF (default origin/main):
#                     what esp32-build CI would refuse. A host that cannot run it
#                     (no docker, no espressif/idf image) fails the row rather
#                     than reading as a pass
#   quality           make quality: the full quality-checks.sh sweep pre-push runs,
#                     after app-build, since its clang gate reads the build's
#                     compile commands, and after a local ci-shards, since it
#                     runs makes of its own in the tree
#
# The C++ step runs on the test host when full-test-run's sweep would
# (scripts/lib/sweep_location.sh: TEST_HOST=1, 0 or unset for the probe).
# On macOS a gate whose C++ step would run here refuses instead.
# Everything else runs here, in parallel lanes beside it. Each step logs to $LAND_GATE_LOGDIR/<step>.log
# (default build/land-gate) and is killed after LAND_GATE_STEP_TIMEOUT seconds
# (default 3600, far above any healthy step), which its row reports as TIMEOUT.
# The table at the end names every step with its duration and the commit it
# judged, marked +dirty when the tree had uncommitted changes or untracked files
# git does not ignore (a new source the commit lacks); the exit status
# is non-zero unless every row passed. The run holds build:<tree>, so a second
# gate or a peer's make in the same tree is refused instead of racing it.
#
# A run on a clean tree that did not move keeps each step's verdict under
# $LAND_GATE_LOGDIR/results/<full HEAD SHA>/. A gate red on one flaky or
# environment step is completed by rerunning only that step on the same commit:
#
#   scripts/land-gate.sh --rerun ui[,bats...]     make land-gate STAGES=ui,bats
#
# A rerun refuses a step name the gate does not have, a dirty tree, and a HEAD
# with no recorded results; it runs the named steps one after another, merges
# their verdicts into that commit's results and prints the merged table.
#
# Once every step of a commit's results is a PASS, from the run or a rerun,
# the commit is stamped by full SHA (.githooks/lib/land-gate-stamp.sh), and
# pre-push skips the quality sweep when it pushes exactly that commit.
#
# Outside this gate: [.] (the commit hook), [slow] and the goldens (nightly and
# local-only), and build.yml's telemetry-worker vitest
# (server/telemetry-worker: `npm install && npm test`).
set -uo pipefail

MAKE=${MAKE:-make}
SELF="${BASH_SOURCE[0]}"
TEST_HOST_RUN=${TEST_HOST_RUN:-$(dirname "$SELF")/test-host-run.sh}
HELIX_CLAIM=${HELIX_CLAIM:-$(dirname "$SELF")/helix-claim}
ESP32_CHECK=${ESP32_CHECK:-$(dirname "$SELF")/esp32_syntax_check.py}
LAND_GATE_BASE_REF=${LAND_GATE_BASE_REF:-origin/main}
CI_SHARDS=${CI_SHARDS:-8}
CI_SHARD_TIMEOUT=${CI_SHARD_TIMEOUT:-600}
LAND_GATE_LOGDIR=${LAND_GATE_LOGDIR:-build/land-gate}
LAND_GATE_STEP_TIMEOUT=${LAND_GATE_STEP_TIMEOUT:-3600}
# Every finished gate appends "<epoch> <seconds> <PASS|FAIL> <sweeps where>",
# machine-wide, so the next one can say how long it will take from what gates
# here actually took instead of anyone's guess.
LAND_GATE_HISTORY=${LAND_GATE_HISTORY:-${XDG_STATE_HOME:-$HOME/.local/state}/helixscreen/land-gate-history.tsv}
ROWS_DIR="$LAND_GATE_LOGDIR/rows"
RESULTS_DIR="$LAND_GATE_LOGDIR/results"

STEPS="app-build ci-shards bats ui plugin python theme-tokens xml-schema lint-xml lvgl-event-codes esp32 quality"

TIMEOUT_BIN=$(command -v timeout || command -v gtimeout || true)

# step <name> <command...>: run it under the step timeout, log it, record its row.
step() {
    local name=$1 log t0 status rc=0
    shift
    log="$LAND_GATE_LOGDIR/$name.log"
    t0=$SECONDS
    echo "[$(date +%H:%M:%S)] $name: started"
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" -k 30 "$LAND_GATE_STEP_TIMEOUT" "$@" >"$log" 2>&1 || rc=$?
    else
        "$@" >"$log" 2>&1 || rc=$?
    fi
    case "$rc" in
        0) status=PASS ;;
        124 | 137) status=TIMEOUT
                   echo "killed after ${LAND_GATE_STEP_TIMEOUT}s (LAND_GATE_STEP_TIMEOUT)" >>"$log" ;;
        *) status=FAIL ;;
    esac
    printf '%s %s %s\n' "$status" $((SECONDS - t0)) "$log" >"$ROWS_DIR/$name"
    echo "[$(date +%H:%M:%S)] $name: $status in $((SECONDS - t0))s"
    [ "$status" = PASS ]
}

skip() { printf 'SKIP - %s\n' "$2" >"$ROWS_DIR/$1"; }

# The firmware units the branch reaches, measured from where it left
# LAND_GATE_BASE_REF, as pre-push measures a new branch.
esp32_check() {
    local base
    if ! base=$(git merge-base HEAD "$LAND_GATE_BASE_REF" 2>/dev/null); then
        echo "✗ no merge-base between HEAD and $LAND_GATE_BASE_REF: fetch it, or set LAND_GATE_BASE_REF"
        return 1
    fi
    "$ESP32_CHECK" --require --base "$base" --head HEAD
}

# run_step <name>: one step, by its table name. The gate's lanes and a rerun
# both run steps through here, so a rerun runs exactly what the gate ran.
run_step() {
    local mk=("$MAKE" --no-print-directory)
    case "$1" in
        app-build) step app-build "${mk[@]}" ;;
        ci-shards)
            if [ "$SWEEP_WHERE" = remote ]; then
                step ci-shards "$TEST_HOST_RUN" sweep "NPROCS=$CI_SHARDS" "SHARD_TIMEOUT=$CI_SHARD_TIMEOUT" SHARD_CRASH_FAILS=1
            else
                step ci-shards "${mk[@]}" unit-sweep "NPROCS=$CI_SHARDS" "SHARD_TIMEOUT=$CI_SHARD_TIMEOUT" SHARD_CRASH_FAILS=1
            fi ;;
        bats) step bats "${mk[@]}" test-shell BATS_REQUIRED=1 ;;
        ui) step ui "${mk[@]}" test-ui-ci ;;
        plugin) step plugin "${mk[@]}" test-plugin ;;
        python) step python "${mk[@]}" test-python ;;
        theme-tokens) step theme-tokens "${mk[@]}" check-theme-tokens ;;
        xml-schema) step xml-schema "${mk[@]}" check-xml-schema ;;
        lint-xml) step lint-xml "${mk[@]}" lint-xml ;;
        lvgl-event-codes) step lvgl-event-codes "${mk[@]}" check-lvgl-event-codes ;;
        esp32) step esp32 bash "$SELF" --esp32-check ;;
        quality) step quality "${mk[@]}" quality ;;
        *) echo "✗ unknown step '$1'" >&2; return 2 ;;
    esac
}

lane() {
    case "$1" in
        unit)
            run_step ci-shards
            if [ "$SWEEP_WHERE" = local ]; then
                # quality starts makes of its own in this tree (an incremental app
                # build, the XML tools), so it waits for the local sweep to link
                # and finish. This lane starts after the app build's row exists.
                if grep -q '^PASS ' "$ROWS_DIR/app-build" 2>/dev/null; then
                    run_step quality
                else
                    skip quality "app-build failed"
                fi
            fi ;;
        app)
            if run_step app-build; then
                run_step ui
                run_step plugin
                [ "$SWEEP_WHERE" = local ] || run_step quality
            else
                skip ui "app-build failed"
                skip plugin "app-build failed"
                [ "$SWEEP_WHERE" = local ] || skip quality "app-build failed"
            fi ;;
        bats) run_step bats ;;
        fresh)
            run_step python
            run_step theme-tokens
            run_step xml-schema
            run_step lint-xml
            run_step lvgl-event-codes
            run_step esp32 ;;
        *) echo "✗ unknown lane '$1'" >&2; return 2 ;;
    esac
    return 0
}

if [ "${1:-}" = --lane ]; then
    lane "$2"
    exit $?
fi
# A step runs under timeout, which can only start a program, not a function.
if [ "${1:-}" = --esp32-check ]; then
    esp32_check
    exit $?
fi

# --rerun takes step names, comma- or space-separated, and refuses any the
# gate does not have before anything runs.
RERUN=""
if [ "${1:-}" = --rerun ]; then
    IFS=', ' read -r -a rerun_names <<<"${2:-}"
    for n in "${rerun_names[@]}"; do
        case " $STEPS " in
            *" $n "*) RERUN+=" $n" ;;
            *) echo "✗ land-gate has no step '$n' (steps: $STEPS)" >&2; exit 2 ;;
        esac
    done
    if [ -z "$RERUN" ]; then
        echo "✗ --rerun needs step names (steps: $STEPS)" >&2
        exit 2
    fi
elif [ $# -gt 0 ]; then
    echo "✗ usage: land-gate.sh [--rerun STEP[,STEP...]]" >&2
    exit 2
fi
reruns() { case "$RERUN " in *" $1 "*) return 0 ;; esac; return 1; }

# A step that ends up running the gate again (a make target, a stub) would fan
# out every lane once more at each level. Lanes inherit this, so a nested gate
# refuses at once.
if [ -n "${LAND_GATE_ACTIVE:-}" ]; then
    echo "✗ land-gate is already running (pid $LAND_GATE_ACTIVE): refusing to nest" >&2
    exit 2
fi
export LAND_GATE_ACTIVE=$$

# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "$SELF")/lib/build_hosts.sh"
# shellcheck source-path=SCRIPTDIR source=lib/sweep_location.sh
. "$(dirname "$SELF")/lib/sweep_location.sh"
# shellcheck source-path=SCRIPTDIR source=../.githooks/lib/land-gate-stamp.sh
. "$(dirname "$SELF")/../.githooks/lib/land-gate-stamp.sh"
resolve_sweep_location || exit $?
# macOS is a development platform, and the C++ suite has known failures there
# on the environment alone (#1776): a local run there reds every gate.
if [ "$SWEEP_WHERE" = local ] && [ "$(uname -s)" = Darwin ] && { [ -z "$RERUN" ] || reruns ci-shards; }; then
    if [ -n "${HELIX_TEST_HOST:-}" ]; then
        echo "✗ land-gate does not run the C++ suite on macOS: TEST_HOST=1 make land-gate sends it to $HELIX_TEST_HOST" >&2
    else
        echo "✗ land-gate does not run the C++ suite on macOS: set HELIX_TEST_HOST in ~/.config/helixscreen/build-hosts.env (docs/devel/BUILD_SYSTEM.md), or gate on Linux" >&2
    fi
    exit 2
fi
export SWEEP_WHERE MAKE TEST_HOST_RUN CI_SHARDS CI_SHARD_TIMEOUT LAND_GATE_LOGDIR LAND_GATE_STEP_TIMEOUT ROWS_DIR

judged="$(git rev-parse --short HEAD 2>/dev/null || echo '?')"
judged_sha="$(git rev-parse HEAD 2>/dev/null || true)"
tree_clean() { [ -z "$(git status --porcelain --untracked-files=normal 2>/dev/null)" ]; }
# The verdicts name judged_sha only while HEAD is still it and nothing is
# uncommitted: a commit or an edit during the run means something else was judged.
still_judged() { [ -n "$judged_sha" ] && [ "$(git rev-parse HEAD 2>/dev/null)" = "$judged_sha" ] && tree_clean; }

# A rerun completes the results one commit already has, so it needs that
# commit checked out and nothing else in the tree.
if [ -n "$RERUN" ]; then
    if [ -z "$judged_sha" ]; then
        echo "✗ --rerun: not in a git tree, so there is no commit to complete" >&2
        exit 2
    fi
    if ! tree_clean; then
        echo "✗ --rerun needs a clean tree: uncommitted changes are not $judged, whose results a rerun completes" >&2
        exit 2
    fi
    if [ ! -d "$RESULTS_DIR/$judged_sha" ]; then
        echo "✗ --rerun: no gate results for HEAD $judged to complete; a make land-gate on a clean tree at this commit records them" >&2
        exit 2
    fi
fi

# build:<tree> is taken for this run's pid. The two claims exclude each other
# across owners, so a worktree:<tree> holder running the gate must read as the
# same owner. helix-claim matches a session-owned holder (taken with --session)
# recorded under the session id this gate inherited, which survives a detached
# launch reparented to init. A caller with no session id (a plain shell that
# took the claim with --pid $$) is matched by retrying the take as each
# ancestor in turn; a peer's claim matches neither and still refuses.
tree=$(basename "$(git rev-parse --show-toplevel 2>/dev/null || pwd)")
claim_pid=$$
until "$HELIX_CLAIM" take "build:$tree" "make land-gate" --pid "$claim_pid" >/dev/null 2>&1; do
    claim_pid=$(ps -o ppid= -p "$claim_pid" 2>/dev/null | tr -d ' ')
    if [ -z "$claim_pid" ] || [ "$claim_pid" -le 1 ]; then
        echo "✗ build:$tree is held by another session: a gate or build is running in this tree" >&2
        if [ -z "${CLAUDE_CODE_SESSION_ID:-${HELIX_SESSION_ID:-}}" ]; then
            echo "  If that claim is your own: a gate launched detached outside a session" >&2
            echo "  is no descendant of the holder and carries no session id, so it cannot" >&2
            echo "  be matched to it. Run it in the foreground, or set HELIX_SESSION_ID." >&2
        fi
        "$HELIX_CLAIM" check "build:$tree" >&2
        exit 2
    fi
done
trap '"$HELIX_CLAIM" release-if-owned-by "$claim_pid" "build:$tree" >/dev/null 2>&1' EXIT

mkdir -p "$ROWS_DIR" || exit 2
rm -f "$ROWS_DIR"/*
gate_t0=$SECONDS

# print_table <rows dir>: one row per step with its duration and log, then the
# tail of each red log. Sets failed unless every step has a PASS row.
print_table() {
    local dir=$1 s status secs log dur
    failed=0
    for s in $STEPS; do
        if [ -f "$dir/$s" ]; then
            read -r status secs log <"$dir/$s"
        else
            status=FAIL secs=- log="no result: its lane died"
        fi
        [ "$status" = PASS ] || failed=1
        if [ "$secs" = - ]; then dur=-; else dur="${secs}s"; fi
        printf '  %-7s  %-17s %7s  %s\n' "$status" "$s" "$dur" "$log"
    done
    for s in $STEPS; do
        [ -f "$dir/$s" ] || continue
        read -r status _ log <"$dir/$s"
        case "$status" in FAIL | TIMEOUT) ;; *) continue ;; esac
        echo
        echo "--- $s: last 25 lines of $log ---"
        tail -n 25 "$log"
    done
}

# stamp_if_complete: called once every step of judged_sha's results is a PASS;
# records the commit for pre-push while the tree still holds exactly it.
stamp_if_complete() {
    if still_judged && land_gate_stamp_write "$judged_sha"; then
        echo "  recorded: pushing $judged skips pre-push's quality sweep"
    fi
}

if [ -n "$RERUN" ]; then
    echo "→ rerunning$RERUN on $judged, one after another (logs: $LAND_GATE_LOGDIR)"
    for s in $STEPS; do
        reruns "$s" && run_step "$s"
    done
    echo
    if ! still_judged; then
        echo "✗ HEAD moved or the tree changed during the rerun: its verdicts name no commit, so nothing was merged"
        print_table "$ROWS_DIR"
        echo "LAND GATE: FAIL ($judged)"
        exit 1
    fi
    cp "$ROWS_DIR"/* "$RESULTS_DIR/$judged_sha/" && touch "$RESULTS_DIR/$judged_sha"
    echo "Land gate on $judged, C++ sweeps: $SWEEP_WHERE; rerun:$RERUN, merged with the commit's earlier results"
    print_table "$RESULTS_DIR/$judged_sha"
    echo
    if [ "$failed" -ne 0 ]; then
        echo "LAND GATE: FAIL ($judged)"
        exit 1
    fi
    echo "LAND GATE: PASS ($judged)"
    stamp_if_complete
    exit 0
fi
# The expected wall time: the median and the longest of the last ten gates
# whose C++ sweeps ran in the same place.
gate_eta() {
    local secs
    secs=$(awk -v w="$SWEEP_WHERE" '$4 == w { print $2 }' "$LAND_GATE_HISTORY" 2>/dev/null | tail -n 10 | sort -n)
    if [ -z "$secs" ]; then
        echo "→ no measured land-gate time yet for $SWEEP_WHERE sweeps; this run will record one"
        return
    fi
    local n median longest
    n=$(printf '%s\n' "$secs" | wc -l)
    median=$(printf '%s\n' "$secs" | sed -n "$(((n + 1) / 2))p")
    longest=$(printf '%s\n' "$secs" | tail -n 1)
    echo "→ expect ~$(((median + 59) / 60))m (median of the last $n), up to $(((longest + 59) / 60))m; finishes around $(date -d "+${median} seconds" +%H:%M 2>/dev/null || echo '?')"
}
gate_eta
tree_clean || judged+="+dirty"

# Each lane is its own process group, so one signal reaches everything under
# it: a test-host ssh, a build, a pytest and its app, a bats run. A lane left
# behind keeps a mirror lock and a run nobody is waiting for.
PIDS=()
start_lane() {
    if command -v setsid >/dev/null 2>&1; then
        setsid bash "$SELF" --lane "$1" &
    else
        bash "$SELF" --lane "$1" &
    fi
    PIDS+=("$!")
}
stop_lanes() {
    local p
    for p in "${PIDS[@]}"; do kill -TERM -- "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null; done
    exit 130
}
trap stop_lanes INT TERM HUP

if [ "$SWEEP_WHERE" = remote ]; then
    echo "→ C++ suite on $HELIX_TEST_HOST, everything else here (logs: $LAND_GATE_LOGDIR)"
    start_lane unit
else
    echo "→ every step runs here (logs: $LAND_GATE_LOGDIR)"
fi
start_lane fresh
start_lane app
app_pid=${PIDS[-1]}

# bats and a local C++ run wait for the app build. Two makes in one tree race on
# its objects and stamps, and bats drives build/bin/helix-screen, which the app
# build relinks under a running ctl test. A failed build still lets bats run.
while [ ! -f "$ROWS_DIR/app-build" ] && kill -0 "$app_pid" 2>/dev/null; do sleep 1; done
start_lane bats
[ "$SWEEP_WHERE" = local ] && start_lane unit

for p in "${PIDS[@]}"; do wait "$p"; done
trap - INT TERM HUP

echo
echo "Land gate on $judged, C++ sweeps: $SWEEP_WHERE"
[[ "$judged" != *+dirty ]] || echo "  +dirty: uncommitted changes were judged, so this verdict names no commit"
print_table "$ROWS_DIR"
echo
echo "  total $((SECONDS - gate_t0))s"
if [ "$failed" -ne 0 ]; then verdict=FAIL; else verdict=PASS; fi
if mkdir -p "$(dirname "$LAND_GATE_HISTORY")" 2>/dev/null; then
    echo "$(date +%s) $((SECONDS - gate_t0)) $verdict $SWEEP_WHERE" >>"$LAND_GATE_HISTORY"
fi
# This run's verdicts replace the commit's results when they name it. Results
# untouched for 30 days are pruned, as stamps are.
recorded=""
if [[ "$judged" != *+dirty ]] && still_judged && mkdir -p "$RESULTS_DIR"; then
    find "$RESULTS_DIR" -mindepth 1 -maxdepth 1 -type d -mtime +30 -exec rm -rf {} + 2>/dev/null
    rm -rf "${RESULTS_DIR:?}/$judged_sha"
    if mkdir -p "$RESULTS_DIR/$judged_sha" && cp "$ROWS_DIR"/* "$RESULTS_DIR/$judged_sha/"; then
        recorded=1
    fi
fi
if [ "$failed" -ne 0 ]; then
    echo "LAND GATE: FAIL ($judged)"
    [ -z "$recorded" ] || echo "  complete it on this commit by rerunning the red steps: make land-gate STAGES=<step>[,<step>]"
    exit 1
fi
echo "LAND GATE: PASS ($judged)"
[ -z "$recorded" ] || stamp_if_complete
