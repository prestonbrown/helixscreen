#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make land-gate: the pre-landing gate. What CI judges a branch on, run before
# the branch reaches main, so a red CI run is found here instead of on main.
#
#   app-build         make (the app; tests/ui drives the real binary). Beside its log,
#                     app-build.why.log says what the build recompiled and why it
#                     might have: the objects make rebuilt (oldest first), the
#                     shared stamps every object depends on whose mtime moved, the
#                     flags stamp's text when it changed, and the ccache counters'
#                     delta (machine-wide, so other builds count too). Its first
#                     line is printed with the step's verdict
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
#   slow              make test-slow, right after ci-shards and where it ran: the
#                     [slow] tier, which CI runs only nightly. Nothing ties a [slow]
#                     test to the file a change touches, so this runs all of it
#                     (about 4 minutes in one process on zeus, less sharded).
#   bats              make test-shell BATS_REQUIRED=1 (a missing bats fails the row)
#                     in two halves (BATS_APP, scripts/bats_select.py --app): the
#                     files that do not drive the built app start with the gate,
#                     and the ones that do wait for app-build's row, since their
#                     ctl tests run the binary that build links. CI's bats job
#                     builds no app, so the first half needs none
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
#   no-mocks          check_no_mocks.py over the same range, after app-build: the
#                     branch's packaged units compiled with mocks off, their objects
#                     scanned for mock symbols. The release gate's cross build sees the
#                     linked result; this sees it before the branch lands
#   quality           make quality: the full quality-checks.sh sweep pre-push runs,
#                     after app-build, since its clang gate reads the build's
#                     compile commands, and after a local ci-shards, since it
#                     runs makes of its own in the tree. tests/python is left to
#                     the python step (HELIX_QC_SKIP_PYTHON_TESTS=1)
#
# A gate runs what the change can reach. scripts/gate_select.py plans each step
# from the paths that differ from the merge-base (committed, staged, unstaged,
# untracked) before any lane starts, and the plan prints above the lanes:
#
#   run     the step runs as listed above
#   select  a narrower run: ci-shards and slow get the change as
#           TEST_SELECT_CHANGED, so the sweep runs only the C++ cases whose
#           dependency files reach it (mk/tests.mk select_cases); bats runs
#           with BATS_GATE=1; python runs the files that read C++ source;
#           ui runs its smoke files and the ones the change reaches;
#           quality runs the gates a docs-only change's paths wake
#   full    every C++ case (or bats file): a safety valve fired
#   skip    nothing the step reads changed. Its row reads SKIP with the
#           reason and counts as a pass
#
# HELIX_GATE_FULL=1 runs everything, and so does a gate on a release/* or
# release-prep/* branch;
# the release procedure sets it. The full suites also run nightly and on every
# push to main, which catch what a selection leaves out. A step a failed
# app-build keeps from running reads BLOCKED and fails the gate.
#
# Each row keeps the plan it was judged under (<step>.plan beside it). A rerun
# refuses a step whose new plan is narrower than its row's, or selects other
# tests, so a selected PASS never replaces a full run's FAIL; a commit whose rows
# come from a full and a selected gate is never stamped. A selected gate's
# history row reads PASS(sel) or FAIL(sel), and each narrowed step <STATUS>(sel),
# so its times stay apart from full gates' in the ETA and in audits.
#
# After app-build, ui, plugin, no-mocks and (with a test-host C++ run) quality
# run side by side. patches/ is applied before any lane starts, so nothing a
# lane reads in a submodule is half patched.
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
# bats, ui and quality test the binaries app-build links (build/bin), so a
# rerun naming any of them or app-build reruns all four: a PASS can only ever
# come from a binary built from this commit. When that app-build fails, the
# other three are recorded SKIP.
#
# Once every step of a commit's results is a PASS, from the run or a rerun,
# the commit is stamped by full SHA (.githooks/lib/land-gate-stamp.sh), and
# pre-push skips the quality sweep when it pushes exactly that commit. A
# result set for the commit that is not all PASS removes its stamp.
#
# Outside this gate: [.] (the commit hook), the goldens (nightly and
# local-only), and build.yml's telemetry-worker vitest
# (server/telemetry-worker: `npm install && npm test`).
set -uo pipefail

MAKE=${MAKE:-make}
SELF="${BASH_SOURCE[0]}"
TEST_HOST_RUN=${TEST_HOST_RUN:-$(dirname "$SELF")/test-host-run.sh}
# A gate step has its own time limit and is the work the host is there for, so it
# does not wait behind whatever else is loading the host.
export HELIX_TEST_MAX_LOAD=${HELIX_TEST_MAX_LOAD:-0}
HELIX_CLAIM=${HELIX_CLAIM:-$(dirname "$SELF")/helix-claim}
ESP32_CHECK=${ESP32_CHECK:-$(dirname "$SELF")/esp32_syntax_check.py}
NO_MOCKS_CHECK=${NO_MOCKS_CHECK:-$(dirname "$SELF")/check_no_mocks.py}
GATE_SELECT=${GATE_SELECT:-$(dirname "$SELF")/gate_select.py}
LAND_GATE_BASE_REF=${LAND_GATE_BASE_REF:-origin/main}
CI_SHARDS=${CI_SHARDS:-8}
CI_SHARD_TIMEOUT=${CI_SHARD_TIMEOUT:-600}
LAND_GATE_LOGDIR=${LAND_GATE_LOGDIR:-build/land-gate}
LAND_GATE_STEP_TIMEOUT=${LAND_GATE_STEP_TIMEOUT:-3600}
# Every finished gate appends "<epoch> <seconds> <PASS|FAIL> <sweeps where>
# <judged SHA, +dirty when it was> <base SHA> <step>=<STATUS>:<seconds>...",
# machine-wide, so the next one can say how long it will take from what gates
# here actually took instead of anyone's guess, and each step's time survives
# the tree's teardown.
LAND_GATE_HISTORY=${LAND_GATE_HISTORY:-${XDG_STATE_HOME:-$HOME/.local/state}/helixscreen/land-gate-history.tsv}
ROWS_DIR="$LAND_GATE_LOGDIR/rows"
# What gate_select.py planned for this run, and the change it planned from.
PLAN="$LAND_GATE_LOGDIR/plan.tsv"
CHANGED_LIST="$LAND_GATE_LOGDIR/changed.txt"
RESULTS_DIR="$LAND_GATE_LOGDIR/results"

STEPS="app-build ci-shards slow bats ui plugin python theme-tokens xml-schema lint-xml lvgl-event-codes esp32 no-mocks quality"

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
                   # A step that times out its own parts has already said which.
                   grep -q '^killed after ' "$log" \
                       || echo "killed after ${LAND_GATE_STEP_TIMEOUT}s (LAND_GATE_STEP_TIMEOUT)" >>"$log" ;;
        *) status=FAIL ;;
    esac
    printf '%s %s %s\n' "$status" $((SECONDS - t0)) "$log" >"$ROWS_DIR/$name"
    plan_record "$name"
    echo "[$(date +%H:%M:%S)] $name: $status in $((SECONDS - t0))s"
    [ "$status" = PASS ]
}

# blocked <step> <why>: a step that could not run because one it needs failed.
blocked() { printf 'BLOCKED - %s\n' "$2" >"$ROWS_DIR/$1"; plan_record "$1"; }

# plan_record <step>: beside its row, the plan the row was judged under:
# "<width> <selection> <gate mode>". width is full (the whole step), select or
# skip; selection hashes what a select ran on (its argument and the change);
# the gate mode is full or selected. A rerun may replace a row only under a plan
# at least as wide, and a commit whose rows came from different gate modes is
# never stamped.
plan_record() {
    local v sel=-
    case "$(planned "$1")" in
        skip) v=skip ;;
        select) v=select
                sel=$({ planned "$1" 4; cat "$CHANGED_LIST" 2>/dev/null; } | cksum | cut -d' ' -f1) ;;
        *) v=full ;;
    esac
    printf '%s %s %s\n' "$v" "$sel" "$(gate_mode)" >"$ROWS_DIR/$1.plan"
}
plan_width() { case "$1" in skip) echo 0 ;; select) echo 1 ;; *) echo 2 ;; esac; }
gate_mode() { if [ "$(plan_mode)" = full ]; then echo full; else echo selected; fi; }

# planned <step> [field]: the plan's verdict for the step (field 2), its reason
# (3) or its argument (4). A step the plan does not name runs.
planned() {
    local v
    v=$(awk -F'\t' -v s="$1" -v f="${2:-2}" '$1 == s { print $f; exit }' "$PLAN" 2>/dev/null)
    if [ -z "$v" ] && [ "${2:-2}" = 2 ]; then v=run; fi
    printf '%s\n' "$v"
}
plan_mode() { awk -F'\t' '$1 == "# mode" { print $2; exit }' "$PLAN" 2>/dev/null; }

# unreached <step>: the plan skips it; its row says why and counts as a pass.
unreached() {
    printf 'SKIP - not reached: %s\n' "$(planned "$1" 3)" >"$ROWS_DIR/$1"
    plan_record "$1"
    echo "[$(date +%H:%M:%S)] $1: SKIP ($(planned "$1" 3))"
}

# The change as make's TEST_SELECT_CHANGED: every path, comma-separated.
test_select_args() {
    [ "$(planned "$1")" = select ] || return 0
    printf 'TEST_SELECT_CHANGED=%s\n' "$(paste -sd, "$CHANGED_LIST")"
}

# Where the branch left LAND_GATE_BASE_REF, as pre-push measures a new branch.
branch_base() {
    git merge-base HEAD "$LAND_GATE_BASE_REF" 2>/dev/null && return 0
    echo "✗ no merge-base between HEAD and $LAND_GATE_BASE_REF: fetch it, or set LAND_GATE_BASE_REF" >&2
    return 1
}

# The firmware units the branch reaches.
esp32_check() {
    local base
    base=$(branch_base) || return 1
    "$ESP32_CHECK" --require --base "$base" --head HEAD
}

# The packaged units the branch reaches.
no_mocks_check() {
    local base
    base=$(branch_base) || return 1
    "$NO_MOCKS_CHECK" --base "$base"
}

# The stamps every compiled object depends on: one that moves rebuilds all of them.
build_stamps() {
    local obj=$1 bdir=$2
    printf '%s\n' "$obj/.build-flags" "$bdir/.thirdparty-abi" "$bdir/.patches-applied" \
        "$bdir/.patch-markers-verified" "$bdir/lib/libhv.a" "$bdir/generated/helix_git_hash.h" \
        lv_conf.h lib/libhv/include/hv/json.hpp
}

# build_snapshot <file> <obj dir> <build dir>: each stamp's mtime, the flags
# stamp's text, and the ccache counters.
build_snapshot() {
    local out=$1 obj=$2 bdir=$3 f
    build_stamps "$obj" "$bdir" | while read -r f; do
        printf '%s\t%s\n' "$f" "$(stat -c %y "$f" 2>/dev/null || echo missing)"
    done >"$out.stamps"
    cat "$obj/.build-flags" >"$out.flags" 2>/dev/null || : >"$out.flags"
    "${LAND_GATE_CCACHE:-ccache}" --print-stats >"$out.ccache" 2>/dev/null || : >"$out.ccache"
}

# build_why <before> <after> <marker> <obj dir>: the report app-build.why.log holds.
build_why() {
    local before=$1 after=$2 marker=$3 obj=$4 n
    n=$(find "$obj" -name '*.o' -newer "$marker" 2>/dev/null | wc -l)
    echo "app-build recompiled $n object(s)"
    if [ "$n" -gt 0 ]; then
        echo "first rebuilt (oldest first):"
        find "$obj" -name '*.o' -newer "$marker" -printf '%T@ %p\n' 2>/dev/null | sort -n | head -n 20 | sed 's/^[^ ]* /  /'
    fi
    paste "$before.stamps" "$after.stamps" | awk -F'\t' '$2 != $4 { print "moved: " $1 " (" $2 " -> " $4 ")" }'
    if ! cmp -s "$before.flags" "$after.flags"; then
        echo "flags stamp text changed:"
        echo "- $(cat "$before.flags")"
        echo "+ $(cat "$after.flags")"
    fi
    if [ -s "$before.ccache" ] && [ -s "$after.ccache" ]; then
        printf 'ccache delta (machine-wide):'
        awk 'NR == FNR { b[$1] = $2; next }
             $1 ~ /^(direct_cache_hit|preprocessed_cache_hit|cache_miss|cleanups_performed|files_in_cache)$/ {
                 d = $2 - b[$1]; printf " %s %s%d", $1, (d < 0 ? "" : "+"), d }' \
            "$before.ccache" "$after.ccache"
        echo
    fi
}

# app_build: the app build, with what it recompiled and why written beside its log.
app_build() {
    local obj=${LAND_GATE_OBJ_DIR:-build/obj} bdir=${LAND_GATE_BUILD_DIR:-build}
    local snap rc=0
    snap=$(mktemp -d) || return 1
    build_snapshot "$snap/before" "$obj" "$bdir"
    : >"$snap/marker"
    "$MAKE" --no-print-directory || rc=$?
    build_snapshot "$snap/after" "$obj" "$bdir"
    build_why "$snap/before" "$snap/after" "$snap/marker" "$obj" >"$LAND_GATE_LOGDIR/app-build.why.log"
    rm -f "$snap"/before.* "$snap"/after.* "$snap/marker"
    rmdir "$snap"
    return "$rc"
}

# bats_half <needs|without>: one half under the step timeout, so the wait
# between the halves never counts against bats.
bats_half() {
    local mk=("$MAKE" --no-print-directory test-shell BATS_REQUIRED=1 "BATS_APP=$1")
    case "$(planned bats)" in
        full) mk+=(BATS_ALL=1) ;;
        select) mk+=(BATS_GATE=1 "BATS_BASE=$GATE_BASE") ;;
    esac
    local budget=${LAND_GATE_BATS_HALF_TIMEOUT:-$LAND_GATE_STEP_TIMEOUT} rc=0
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" -k 30 "$budget" "${mk[@]}" || rc=$?
        case "$rc" in
            124 | 137) echo "killed after ${budget}s (LAND_GATE_STEP_TIMEOUT, bats half $1)" ;;
        esac
        return "$rc"
    else
        "${mk[@]}"
    fi
}

# bats_halves: the files that need no app first, then, once app-build has a
# row (or its lane died), the files that drive the binary it links. Both halves
# run whatever the other did; either red reds the step. A failed half outranks a
# timed-out one, so the step reads TIMEOUT only when no half failed.
bats_halves() {
    local without=0 needs=0
    bats_half without || without=$?
    if [ -n "${APP_LANE_PID:-}" ]; then
        while [ ! -f "$ROWS_DIR/app-build" ] && kill -0 "$APP_LANE_PID" 2>/dev/null; do sleep 1; done
    fi
    bats_half needs || needs=$?
    local rc
    for rc in "$without" "$needs"; do
        case "$rc" in 0 | 124 | 137) ;; *) return 1 ;; esac
    done
    for rc in "$without" "$needs"; do
        case "$rc" in 124 | 137) return 124 ;; esac
    done
    return 0
}

# run_step <name>: one step, by its table name. The gate's lanes and a rerun
# both run steps through here, so a rerun runs exactly what the gate ran.
run_step() {
    local mk=("$MAKE" --no-print-directory) sel=()
    if [ "$(planned "$1")" = skip ]; then
        unreached "$1"
        return 0
    fi
    case "$1" in
        app-build) step app-build bash "$SELF" --app-build ;;
        ci-shards)
            mapfile -t sel < <(test_select_args ci-shards)
            if [ "$SWEEP_WHERE" = remote ]; then
                step ci-shards "$TEST_HOST_RUN" sweep "NPROCS=$CI_SHARDS" "SHARD_TIMEOUT=$CI_SHARD_TIMEOUT" SHARD_CRASH_FAILS=1 "${sel[@]}"
            else
                step ci-shards "${mk[@]}" unit-sweep "NPROCS=$CI_SHARDS" "SHARD_TIMEOUT=$CI_SHARD_TIMEOUT" SHARD_CRASH_FAILS=1 "${sel[@]}"
            fi ;;
        slow)
            mapfile -t sel < <(test_select_args slow)
            if [ "$SWEEP_WHERE" = remote ]; then
                step slow "$TEST_HOST_RUN" exec -- make --no-print-directory test-slow SHARD_CRASH_FAILS=1 "${sel[@]}"
            else
                step slow "${mk[@]}" test-slow SHARD_CRASH_FAILS=1 "${sel[@]}"
            fi ;;
        # Each half has the step timeout of its own; this one, three of them,
        # only backs those up across the wait for app-build between them.
        bats)
            local half=$LAND_GATE_STEP_TIMEOUT backstop=$((LAND_GATE_STEP_TIMEOUT * 3))
            LAND_GATE_BATS_HALF_TIMEOUT=$half LAND_GATE_STEP_TIMEOUT=$backstop step bats bash "$SELF" --bats ;;
        ui)
            [ "$(planned ui)" != select ] || sel=("UI_TESTS=$(planned ui 4)")
            step ui "${mk[@]}" test-ui-ci "${sel[@]}" ;;
        plugin) step plugin "${mk[@]}" test-plugin ;;
        python)
            [ "$(planned python)" != select ] || sel=("PYTHON_TESTS=$(planned python 4)")
            step python "${mk[@]}" test-python "${sel[@]}" ;;
        theme-tokens) step theme-tokens "${mk[@]}" check-theme-tokens ;;
        xml-schema) step xml-schema "${mk[@]}" check-xml-schema ;;
        lint-xml) step lint-xml "${mk[@]}" lint-xml ;;
        lvgl-event-codes) step lvgl-event-codes "${mk[@]}" check-lvgl-event-codes ;;
        esp32) step esp32 bash "$SELF" --esp32-check ;;
        no-mocks) step no-mocks bash "$SELF" --no-mocks-check ;;
        quality)
            # A docs-only change: the gates whose triggers its paths match.
            [ "$(planned quality)" != select ] || sel=("HELIX_QC_CHANGED_FROM=$(cd "$(dirname "$CHANGED_LIST")" && pwd)/$(basename "$CHANGED_LIST")")
            step quality env HELIX_LINK_BOUNDARY_REQUIRED=1 HELIX_QC_SKIP_PYTHON_TESTS=1 "${sel[@]}" "${mk[@]}" quality ;;
        *) echo "✗ unknown step '$1'" >&2; return 2 ;;
    esac
}

lane() {
    case "$1" in
        unit)
            run_step ci-shards
            run_step slow
            if [ "$SWEEP_WHERE" = local ]; then
                # quality starts makes of its own in this tree (an incremental app
                # build, the XML tools), so it waits for the local sweep to link
                # and finish. This lane starts after the app build's row exists.
                if grep -Eq '^(PASS|SKIP) ' "$ROWS_DIR/app-build" 2>/dev/null; then
                    run_step quality
                else
                    blocked quality "app-build failed"
                fi
            fi ;;
        app)
            if run_step app-build; then
                [ "$(planned app-build)" = skip ] ||
                    head -n 1 "$LAND_GATE_LOGDIR/app-build.why.log" 2>/dev/null | sed "s/^/[$(date +%H:%M:%S)] /"
                # Each writes only its own row and log, and none rebuilds what
                # another reads, so they share the tree's wait for nothing.
                run_step ui &
                run_step plugin &
                run_step no-mocks &
                if [ "$SWEEP_WHERE" != local ]; then run_step quality & fi
                wait
            else
                head -n 1 "$LAND_GATE_LOGDIR/app-build.why.log" 2>/dev/null | sed "s/^/[$(date +%H:%M:%S)] /"
                blocked ui "app-build failed"
                blocked plugin "app-build failed"
                blocked no-mocks "app-build failed"
                [ "$SWEEP_WHERE" = local ] || blocked quality "app-build failed"
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
if [ "${1:-}" = --no-mocks-check ]; then
    no_mocks_check
    exit $?
fi
if [ "${1:-}" = --app-build ]; then
    app_build
    exit $?
fi
if [ "${1:-}" = --bats ]; then
    bats_halves
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

# The steps that test what app-build links into build/bin.
BUILD_DEPENDENTS="bats ui quality"
if [ -n "$RERUN" ]; then
    for n in app-build $BUILD_DEPENDENTS; do
        if reruns "$n"; then
            for d in app-build $BUILD_DEPENDENTS; do reruns "$d" || RERUN+=" $d"; done
            break
        fi
    done
fi

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
if [ "$SWEEP_WHERE" = local ] && [ "$(uname -s)" = Darwin ] && { [ -z "$RERUN" ] || reruns ci-shards || reruns slow; }; then
    if [ -n "${HELIX_TEST_HOST:-}" ]; then
        echo "✗ land-gate does not run the C++ suite on macOS: TEST_HOST=1 make land-gate sends it to $HELIX_TEST_HOST" >&2
    else
        echo "✗ land-gate does not run the C++ suite on macOS: set HELIX_TEST_HOST in ~/.config/helixscreen/build-hosts.env (docs/devel/BUILD_SYSTEM.md), or gate on Linux" >&2
    fi
    exit 2
fi
APP_LANE_PID=
export SWEEP_WHERE MAKE TEST_HOST_RUN CI_SHARDS CI_SHARD_TIMEOUT LAND_GATE_LOGDIR LAND_GATE_STEP_TIMEOUT ROWS_DIR APP_LANE_PID
export PLAN CHANGED_LIST GATE_SELECT

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

# The plan: what the change since the merge-base can reach. With no merge-base
# there is no change to read, so everything runs. LAND_GATE_CHANGED_FILE hands
# the planner a list of paths instead of a diff (the gate's own tests).
GATE_BASE=$(branch_base 2>/dev/null) || GATE_BASE=
export GATE_BASE
plan_src=()
if [ -n "${LAND_GATE_CHANGED_FILE:-}" ]; then
    plan_src=(--changed-from "$LAND_GATE_CHANGED_FILE")
elif [ -n "$GATE_BASE" ]; then
    plan_src=(--base "$GATE_BASE")
fi
plan_ok=0
if [ ${#plan_src[@]} -gt 0 ] && "$GATE_SELECT" changed "${plan_src[@]}" >"$CHANGED_LIST" 2>"$PLAN.err" &&
    "$GATE_SELECT" plan "${plan_src[@]}" >"$PLAN" 2>>"$PLAN.err"; then
    plan_ok=1
fi
if [ "$plan_ok" = 0 ]; then
    [ -n "$GATE_BASE" ] || echo "no merge-base with $LAND_GATE_BASE_REF" >>"$PLAN.err"
    : >"$CHANGED_LIST"
    {
        printf '# mode\tfull\t\t\n'
        for s in $STEPS; do
            case "$s" in ci-shards | slow | bats) v=full ;; *) v=run ;; esac
            printf '%s\t%s\t%s\t\n' "$s" "$v" "no plan, so everything runs"
        done
    } >"$PLAN"
    echo "→ no plan ($(tail -n 1 "$PLAN.err" 2>/dev/null)): everything runs"
fi
echo "→ plan: $(plan_mode) ($(wc -l <"$CHANGED_LIST" | tr -d ' ') changed path(s) since ${GATE_BASE:0:10}; HELIX_GATE_FULL=1 runs everything)"
awk -F'\t' '$1 !~ /^#/ { printf "    %-17s %-6s  %s\n", $1, $2, $3 }' "$PLAN" 2>/dev/null

# The app build applies patches/ to the submodules while bats, the freshness
# checks and a test-host C++ run's copy of the tree already read them. Applied
# first, every lane sees fully patched submodules; with the stamp current it is
# a no-op. A rerun runs its steps one after another behind app-build, so only a
# test-host C++ rerun needs it.
if [ -z "$RERUN" ] || { [ "$SWEEP_WHERE" = remote ] && { reruns ci-shards || reruns slow; }; }; then
    if ! "$MAKE" --no-print-directory apply-patches >"$LAND_GATE_LOGDIR/apply-patches.log" 2>&1; then
        echo "✗ applying patches/ failed, so the lanes would read unpatched submodules:"
        tail -n 25 "$LAND_GATE_LOGDIR/apply-patches.log"
        echo "LAND GATE: FAIL ($judged)"
        exit 1
    fi
fi

# print_table <rows dir>: one row per step with its duration and log, then the
# tail of each red log. Sets failed unless every step has a PASS row.
print_table() {
    local dir=$1 s status secs log dur
    failed=0 unpassed=""
    for s in $STEPS; do
        if [ -f "$dir/$s" ]; then
            read -r status secs log <"$dir/$s"
        else
            status=FAIL secs=- log="no result: its lane died"
        fi
        case "$status" in PASS | SKIP) ;; *) failed=1; unpassed+=",$s" ;; esac
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
    if ! plans_agree "$RESULTS_DIR/$judged_sha"; then
        land_gate_stamp_remove "$judged_sha"
        echo "✗ $judged's rows were recorded under different plans (a full and a selected gate), so nothing is stamped"
        echo "  run the whole gate once on this commit: $(full_prefix)make land-gate"
        return 1
    fi
    # The test hook hands the planner a made-up change: its verdicts judge
    # that list, not the commit, so they never vouch for it to pre-push.
    if [ -n "${LAND_GATE_CHANGED_FILE:-}" ]; then
        echo "  not stamped: the plan came from LAND_GATE_CHANGED_FILE, not from $judged's own change"
        return 0
    fi
    if still_judged && land_gate_stamp_write "$judged_sha"; then
        echo "  recorded: pushing $judged skips pre-push's quality sweep"
    fi
}

# plans_agree <results dir>: every step has a plan record, all from one gate mode.
plans_agree() {
    local s modes=""
    for s in $STEPS; do
        [ -f "$1/$s.plan" ] || return 1
        modes+="$(cut -d' ' -f3 "$1/$s.plan")"$'\n'
    done
    [ "$(printf '%s' "$modes" | sort -u | grep -c .)" -eq 1 ]
}

# full_prefix: HELIX_GATE_FULL=1 when this run had it, for a command it prints.
full_prefix() { [ "${HELIX_GATE_FULL:-}" != 1 ] || printf 'HELIX_GATE_FULL=1 '; }

# results_red: judged_sha's results have a step that is not a PASS, so any
# stamp it holds no longer agrees with them.
results_red() {
    land_gate_stamp_remove "$judged_sha"
    echo "LAND GATE: FAIL ($judged)"
    echo "  complete it on this commit by rerunning the steps still to pass: $(full_prefix)make land-gate STAGES=${unpassed#,}"
}

if [ -n "$RERUN" ]; then
    # A rerun replaces rows; a plan narrower than the one a row was judged
    # under would turn a full run's FAIL into a selected PASS.
    for s in $RERUN; do
        rec="$RESULTS_DIR/$judged_sha/$s.plan"
        if [ -f "$rec" ]; then read -r rw rsel _ <"$rec"; else rw=full rsel=-; fi
        plan_record "$s"
        read -r nw nsel _ <"$ROWS_DIR/$s.plan"
        rm -f "$ROWS_DIR/$s.plan"
        if [ "$(plan_width "$nw")" -lt "$(plan_width "$rw")" ] ||
            { [ "$nw" = select ] && [ "$rw" = select ] && [ "$nsel" != "$rsel" ]; }; then
            echo "✗ --rerun $s: this run's plan ($nw) is narrower than the $rw run that recorded its row" >&2
            [ "$rw" != full ] || echo "  rerun it as wide: HELIX_GATE_FULL=1 make land-gate STAGES=${RERUN# }" >&2
            exit 2
        fi
    done
    echo "→ rerunning$RERUN on $judged, one after another (logs: $LAND_GATE_LOGDIR)"
    for s in $STEPS; do
        reruns "$s" || continue
        case " $BUILD_DEPENDENTS " in
            *" $s "*) if ! grep -Eq '^(PASS|SKIP) ' "$ROWS_DIR/app-build" 2>/dev/null; then
                          blocked "$s" "app-build failed"
                          continue
                      fi ;;
        esac
        run_step "$s"
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
        results_red
        exit 1
    fi
    if ! stamp_if_complete; then
        echo "LAND GATE: FAIL ($judged)"
        exit 1
    fi
    echo "LAND GATE: PASS ($judged)"
    exit 0
fi
# The expected wall time: the median and the longest of the last ten gates
# whose C++ sweeps ran in the same place, full gates for a full one and
# selected gates (a verdict marked (sel)) for a selected one.
gate_eta() {
    local secs narrow=0
    [ "$(gate_mode)" = full ] || narrow=1
    secs=$(awk -v w="$SWEEP_WHERE" -v sel="$narrow" '$4 == w && (($3 ~ /\(sel\)$/) == sel) { print $2 }' \
        "$LAND_GATE_HISTORY" 2>/dev/null | tail -n 10 | sort -n)
    if [ -z "$secs" ]; then
        echo "→ no measured land-gate time yet for $(gate_mode) gates with $SWEEP_WHERE sweeps; this run will record one"
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
APP_LANE_PID=${PIDS[-1]}
start_lane bats

# A local C++ run waits for the app build: two makes in one tree race on its
# objects and stamps.
while [ ! -f "$ROWS_DIR/app-build" ] && kill -0 "$APP_LANE_PID" 2>/dev/null; do sleep 1; done
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
history_steps=""
for s in $STEPS; do
    if [ -f "$ROWS_DIR/$s" ]; then
        read -r status secs _ <"$ROWS_DIR/$s"
    else
        status=FAIL secs=-
    fi
    # A narrowed step's time is not a full step's: the ETA and the audits
    # read (sel) apart.
    [ "$(planned "$s")" != select ] || status+="(sel)"
    history_steps+=" $s=$status:$secs"
done
history_sha=${judged_sha:--}
[[ "$judged" != *+dirty ]] || history_sha+="+dirty"
history_base=$(branch_base 2>/dev/null) || history_base=-
if mkdir -p "$(dirname "$LAND_GATE_HISTORY")" 2>/dev/null; then
    history_verdict=$verdict
    [ "$(gate_mode)" = full ] || history_verdict+="(sel)"
    echo "$(date +%s) $((SECONDS - gate_t0)) $history_verdict $SWEEP_WHERE $history_sha $history_base$history_steps" >>"$LAND_GATE_HISTORY"
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
    if [ -n "$recorded" ]; then
        results_red
    else
        echo "LAND GATE: FAIL ($judged)"
    fi
    exit 1
fi
echo "LAND GATE: PASS ($judged)"
[ -z "$recorded" ] || stamp_if_complete
