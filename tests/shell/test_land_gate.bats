#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make land-gate's wiring (scripts/land-gate.sh). make and test-host-run.sh are
# stubbed: each target exits with a verdict the test chooses and records that it
# ran, so these prove which steps run where and that any red step reds the gate
# and shows in the table, without running a real suite.

load helpers

REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
GATE="$REPO_ROOT/scripts/land-gate.sh"

ALL_STEPS="app-build ci-shards bats ui plugin python theme-tokens xml-schema lint-xml lvgl-event-codes esp32 quality"

# Every make here is a stub, and a stub must never reach the real make or the
# jobpool shim: JOBPOOL=0 keeps the shim out of any make that does run, and the
# stubs refuse to nest (FAKE_MAKE_DEPTH), so a stub that re-enters make fails
# loudly instead of forking without bound.
setup() {
    export JOBPOOL=0
    unset FAKE_MAKE_DEPTH LAND_GATE_ACTIVE
    export TMPDIR="$BATS_TEST_TMPDIR"
    export CALLS="$BATS_TEST_TMPDIR/calls"
    export LAND_GATE_LOGDIR="$BATS_TEST_TMPDIR/logs"
    export LAND_GATE_HISTORY="$BATS_TEST_TMPDIR/history.tsv"
    # A passing gate run in the real tree must never vouch for its HEAD to the
    # real pre-push hook.
    export HELIX_LAND_GATE_STAMP_DIR="$BATS_TEST_TMPDIR/stamps"
    export HELIX_LAND_GATE_SHARED_STAMP_DIR="$BATS_TEST_TMPDIR/shared-stamps"
    : >"$CALLS"
    # A target exits with $RC_<target, - as _>, default 0; no target is "app".
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; vars=""
for a in "$@"; do case "$a" in -*) ;; *=*) vars="$vars $a" ;; *) t=$a ;; esac; done
echo "make $t$vars" >> "$CALLS"
echo "start $t" >> "$CALLS.order"
[ "$t" != unit-sweep ] || [ -z "${UNIT_SWEEP_SECS:-}" ] || sleep "$UNIT_SWEEP_SECS"
trap '\''echo "end $t" >> "$CALLS.order"'\'' EXIT
[ "$t" != quality ] || [ -z "${MAKE_QUALITY_HOOK:-}" ] || eval "$MAKE_QUALITY_HOOK"
eval "hook=\${HOOK_$(echo "$t" | tr - _):-}"
[ -z "$hook" ] || eval "$hook"
echo "output of $t"
eval "rc=\${RC_$(echo "$t" | tr - _):-0}"
exit "$rc"'
    mock_command_script fake-test-host-run '
echo "host-run $*" >> "$CALLS"
case "$1" in
    sweep) echo "host sweep $*"; exit "${RC_host_ci:-0}" ;;
    --probe) exit 1 ;;
esac'
    # take exits $CLAIM_TAKE_RC (default 0); every call is recorded.
    mock_command_script fake-claim '
echo "claim $*" >> "$CALLS"
case "$1" in take) exit "${CLAIM_TAKE_RC:-0}" ;; esac'
    # The ESP32 check exits $RC_esp32 (default 0) and records its arguments.
    mock_command_script fake-esp32 '
echo "esp32 $*" >> "$CALLS"
echo "output of esp32"
exit "${RC_esp32:-0}"'
    export MAKE=fake-make TEST_HOST_RUN=fake-test-host-run HELIX_CLAIM=fake-claim
    export ESP32_CHECK=fake-esp32 LAND_GATE_BASE_REF=HEAD
    export HELIX_TEST_HOST=testhost.invalid HELIX_BUILD_HOSTS_FILE=/nonexistent
    unset CI_SHARDS CI_SHARD_TIMEOUT
    export TEST_HOST=0
}

# No test here runs make in the real tree: a recipe is checked by copying its
# fragment of mk/tests.mk into a scratch Makefile (run_mk_fragment, helpers.bash).
run_fragment() { # <first-line-regex> <target> [make args...]
    run_mk_fragment "$REPO_ROOT/mk/tests.mk" "$@"
}

row() { # <status> <step>: the table row for step has that status
    printf '%s\n' "$output" | grep -Eq "^  $1 +$2 "
}

@test "all green: every step runs, every row passes, and the gate passes" {
    run "$GATE"
    [ "$status" -eq 0 ]
    for s in $ALL_STEPS; do row PASS "$s"; done
    contains "LAND GATE: PASS" "$output"
    grep -qx "make app" "$CALLS"
    grep -qx "make test-shell BATS_REQUIRED=1" "$CALLS"
    grep -qx "make test-ui-ci" "$CALLS"
    grep -qx "make test-plugin" "$CALLS"
    grep -qx "make test-python" "$CALLS"
    grep -qx "make check-theme-tokens" "$CALLS"
    grep -qx "make check-xml-schema" "$CALLS"
    grep -qx "make lint-xml" "$CALLS"
    grep -qx "make check-lvgl-event-codes" "$CALLS"
    grep -qx "make quality" "$CALLS"
    grep -qx "esp32 --require --base $(git rev-parse HEAD) --head HEAD" "$CALLS"
}

@test "the ESP32 check measures from the merge-base with the base ref; with none its row fails" {
    LAND_GATE_BASE_REF=refs/heads/no-such-branch-for-the-gate run "$GATE"
    [ "$status" -eq 1 ]
    row FAIL esp32
    contains "no merge-base between HEAD and refs/heads/no-such-branch-for-the-gate" "$output"
    refute_grep "^esp32 " "$CALLS"
}

@test "the C++ suite runs once, in CI's 8-shard layout, and CI_SHARDS changes it" {
    run "$GATE"
    [ "$status" -eq 0 ]
    [ "$(grep -c "unit-sweep" "$CALLS")" -eq 1 ]
    grep -qx "make unit-sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1" "$CALLS"
    : >"$CALLS"
    CI_SHARDS=12 run "$GATE"
    grep -qx "make unit-sweep NPROCS=12 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1" "$CALLS"
}

@test "a CI-layout shard gets CI's step budget, not the sweep's 300s, and CI_SHARD_TIMEOUT changes it" {
    [ "$(sed -n '/name: Run unit tests/,/run:/s/.*timeout-minutes: *//p' "$REPO_ROOT/.github/workflows/build.yml")" = 10 ]
    TEST_HOST=1 run "$GATE"
    grep -qx "host-run sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1" "$CALLS"
    : >"$CALLS"
    CI_SHARD_TIMEOUT=900 run "$GATE"
    grep -qx "make unit-sweep NPROCS=8 SHARD_TIMEOUT=900 SHARD_CRASH_FAILS=1" "$CALLS"
    refute_grep "make unit-sweep SHARD_TIMEOUT" "$CALLS"
}

@test "each failing step fails the gate and shows FAIL in its own row" {
    local target step
    for pair in app:app-build unit-sweep:ci-shards test-shell:bats test-ui-ci:ui test-plugin:plugin test-python:python \
                check-theme-tokens:theme-tokens check-xml-schema:xml-schema lint-xml:lint-xml \
                check-lvgl-event-codes:lvgl-event-codes esp32:esp32; do
        target=${pair%%:*} step=${pair#*:}
        eval "export RC_$(echo "$target" | tr - _)=3"
        run "$GATE"
        eval "unset RC_$(echo "$target" | tr - _)"
        [ "$status" -eq 1 ] || { echo "red $target did not fail the gate"; return 1; }
        row FAIL "$step" || { echo "no FAIL row for $step"; return 1; }
        contains "LAND GATE: FAIL" "$output"
        contains "output of $target" "$output"
    done
}

@test "a red CI-layout run on the test host fails the gate" {
    TEST_HOST=1 RC_host_ci=1 run "$GATE"
    [ "$status" -eq 1 ]
    row FAIL ci-shards
}

@test "a failed app build skips the UI steps and fails the gate" {
    RC_app=2 run "$GATE"
    [ "$status" -eq 1 ]
    row FAIL app-build
    row SKIP ui
    row SKIP plugin
    row SKIP quality
    refute_grep "make test-ui-ci" "$CALLS"
    row PASS bats
}

# order_of <event>: its line number in the stubs' start/end log.
order_of() { grep -nx "$1" "$CALLS.order" | head -n 1 | cut -d: -f1; }

@test "a local C++ run finishes before quality starts its own makes in the tree" {
    UNIT_SWEEP_SECS=2 run "$GATE"
    [ "$status" -eq 0 ]
    row PASS quality
    [ "$(grep -c "^make quality" "$CALLS")" -eq 1 ]
    [ "$(order_of "end unit-sweep")" -lt "$(order_of "start quality")" ]
}

@test "bats starts after the app build, which relinks the binary its ctl tests run" {
    run "$GATE"
    [ "$status" -eq 0 ]
    row PASS bats
    [ "$(order_of "end app")" -lt "$(order_of "start test-shell")" ]
}

@test "bats still runs when the app build fails" {
    RC_app=2 run "$GATE"
    [ "$status" -eq 1 ]
    row PASS bats
}

@test "a local C++ run after a failed app build still skips quality" {
    RC_app=2 run "$GATE"
    [ "$status" -eq 1 ]
    row PASS ci-shards
    row SKIP quality
    refute_grep "make quality" "$CALLS"
}

@test "a test-host C++ run leaves quality in the app lane, beside it" {
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    row PASS quality
    [ "$(grep -c "^make quality" "$CALLS")" -eq 1 ]
}

@test "TEST_HOST=1 sends the C++ run to the test host, once, and runs none here" {
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    [ "$(grep -c "^host-run sweep" "$CALLS")" -eq 1 ]
    grep -qx "host-run sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1" "$CALLS"
    refute_grep "make unit-sweep" "$CALLS"
    grep -qx "make test-shell BATS_REQUIRED=1" "$CALLS"
    grep -qx "make test-ui-ci" "$CALLS"
}

@test "a bad TEST_HOST refuses before running anything" {
    TEST_HOST=maybe run "$GATE"
    [ "$status" -eq 2 ]
    [ ! -s "$CALLS" ]
}

# The recipe names $(MAKE), so make runs it even under -n: the script it calls
# is a stub in the scratch directory, never the real gate.
@test "make land-gate hands off to scripts/land-gate.sh with CI_SHARDS" {
    mkdir -p "$BATS_TEST_TMPDIR/mkfrag/scripts"
    printf '#!/bin/sh\necho "stub gate CI_SHARDS=$CI_SHARDS TEST_HOST=$TEST_HOST"\n' \
        >"$BATS_TEST_TMPDIR/mkfrag/scripts/land-gate.sh"
    chmod +x "$BATS_TEST_TMPDIR/mkfrag/scripts/land-gate.sh"
    run run_fragment '^CI_SHARDS [?]=' land-gate land-gate
    [ "$status" -eq 0 ]
    contains "stub gate CI_SHARDS=8 TEST_HOST=" "$output"
    run run_fragment '^CI_SHARDS [?]=' land-gate land-gate CI_SHARDS=12 TEST_HOST=1
    contains "stub gate CI_SHARDS=12 TEST_HOST=1" "$output"
}

# The gate is only worth its name while it runs what CI runs.
@test "test-ui-ci runs build.yml's exact tests/ui pytest invocation, headless" {
    local ci args
    ci=$(grep -E '^\s*run: pytest tests/ui/' "$REPO_ROOT/.github/workflows/build.yml" | sed 's/^ *run: pytest //')
    [ -n "$ci" ]
    args=$(sed -n 's/^UI_CI_PYTEST_ARGS := //p' "$REPO_ROOT/mk/tests.mk")
    [ "$args" = "$ci" ] || { printf 'CI:   %s\nmake: %s\n' "$ci" "$args"; return 1; }
    run run_fragment '^UI_CI_PYTEST_ARGS :=' test-ui-ci -n test-ui-ci
    [ "$status" -eq 0 ]
    contains "pytest $ci;" "$output"
    contains "HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy" "$output"
}

@test "CI's unit-test step uses the filter the sweep passes, with explicit shard flags" {
    local ci
    ci=$(grep -F 'helix-tests "~[.] ~[slow]" --shard-count $NPROCS --shard-index $i' \
        "$REPO_ROOT/.github/workflows/build.yml")
    [ -n "$ci" ]
    grep -qxF '        NPROCS=8' "$REPO_ROOT/.github/workflows/build.yml"
}

@test "a step past LAND_GATE_STEP_TIMEOUT is killed and reads TIMEOUT, failing the gate" {
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; for a in "$@"; do case "$a" in -*|*=*) ;; *) t=$a ;; esac; done
echo "make $t" >> "$CALLS"
[ "$t" = lint-xml ] && sleep 30
exit 0'
    LAND_GATE_STEP_TIMEOUT=1 run "$GATE"
    [ "$status" -eq 1 ]
    row TIMEOUT lint-xml
    row PASS bats
}

@test "the gate holds build:<tree> for the run and releases it; a held tree refuses" {
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^claim take build:" "$CALLS"
    grep -q "^claim release-if-owned-by .* build:" "$CALLS"
    : >"$CALLS"
    CLAIM_TAKE_RC=1 run "$GATE"
    [ "$status" -eq 2 ]
    contains "is held" "$output"
    refute_grep "^make" "$CALLS"
}

@test "a tree claimed by the session running the gate is taken as that session" {
    # take succeeds only for the pid named in $CLAIM_OWNER, as helix-claim
    # exempts the holder of the sibling worktree: claim.
    mock_command_script fake-claim '
echo "claim $*" >> "$CALLS"
case "$1" in take) [ "$5" = "$CLAIM_OWNER" ] ;; esac'
    CLAIM_OWNER=$$ run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^claim take build:.* --pid $$\$" "$CALLS"
    grep -q "^claim release-if-owned-by $$ build:" "$CALLS"
}

@test "the verdict names the commit, marked +dirty when the tree has uncommitted changes" {
    local T="$BATS_TEST_TMPDIR/tree"
    git init -q "$T"
    echo a >"$T/f"; git -C "$T" add f
    git -C "$T" -c user.email=t@t -c user.name=t commit -q -m init
    local sha; sha=$(git -C "$T" rev-parse --short HEAD)
    run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    contains "Land gate on $sha," "$output"
    lacks "+dirty" "$output"
    echo b >"$T/f"
    run bash -c "cd '$T' && '$GATE'"
    contains "Land gate on $sha+dirty" "$output"
    contains "LAND GATE: PASS ($sha+dirty)" "$output"
}

# A one-commit repo the gate runs in, so its HEAD and cleanliness are the test's.
stamp_tree() {
    T="$BATS_TEST_TMPDIR/tree"
    git init -q "$T"
    echo a >"$T/f"; git -C "$T" add f
    git -C "$T" -c user.email=t@t -c user.name=t commit -q -m init
    SHA=$(git -C "$T" rev-parse HEAD)
}

@test "a clean PASS records the full HEAD SHA for pre-push" {
    stamp_tree
    run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    [ -f "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    [ "$(ls "$HELIX_LAND_GATE_STAMP_DIR")" = "$SHA" ]
    contains "recorded: pushing ${SHA:0:7}" "$output"
}

@test "a failed step or a dirty tree records nothing" {
    stamp_tree
    RC_quality=1 run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 1 ]
    row FAIL quality
    RC_lint_xml=1 run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 1 ]
    echo b >"$T/f"
    run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    contains "+dirty" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    lacks "recorded:" "$output"
}

@test "an untracked file git does not ignore records nothing; an ignored one does not count" {
    stamp_tree
    echo build/ >"$T/.gitignore"
    git -C "$T" add .gitignore
    git -C "$T" -c user.email=t@t -c user.name=t commit -q -m ignore
    SHA=$(git -C "$T" rev-parse HEAD)
    mkdir "$T/build"; echo o >"$T/build/x.o"
    echo new >"$T/new.cpp"
    run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    contains "+dirty" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    rm "$T/new.cpp"
    run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    lacks "+dirty" "$output"
    [ -f "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a gate whose tree is edited during the run records nothing" {
    stamp_tree
    MAKE_QUALITY_HOOK="echo edited >f" run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    lacks "+dirty" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a tree dirty at the start records nothing, even if clean at the end" {
    stamp_tree
    echo b >"$T/f"
    MAKE_QUALITY_HOOK="echo a >f" run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    contains "+dirty" "$output"
    [ -z "$(git -C "$T" status --porcelain --untracked-files=no)" ]
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a gate whose HEAD moves during the run records nothing" {
    stamp_tree
    # The quality step commits, as a peer committing in the tree mid-gate would.
    RC_quality=0 MAKE_QUALITY_HOOK="git -c user.email=t@t -c user.name=t commit -q --allow-empty -m moved" \
        run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    [ "$(git -C "$T" rev-parse HEAD)" != "$SHA" ]
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$(git -C "$T" rev-parse HEAD)" ]
}

gate_in_tree() { run bash -c 'cd "$1" && shift && "$@"' _ "$T" "$GATE" "$@"; }

@test "a red step completed by a passing rerun of it on the same commit stamps the commit" {
    stamp_tree
    RC_lint_xml=1 gate_in_tree
    [ "$status" -eq 1 ]
    row FAIL lint-xml
    contains "make land-gate STAGES=lint-xml" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    : >"$CALLS"
    gate_in_tree --rerun lint-xml
    [ "$status" -eq 0 ]
    for s in $ALL_STEPS; do row PASS "$s"; done
    contains "LAND GATE: PASS (${SHA:0:7}" "$output"
    [ -f "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    [ -f "$HELIX_LAND_GATE_SHARED_STAMP_DIR/$SHA" ]
    # Only the named step ran: lint-xml tests nothing app-build links.
    [ "$(grep '^make' "$CALLS")" = "make lint-xml" ]
}

@test "on macOS a rerun runs unless it names the C++ suite" {
    stamp_tree
    RC_lint_xml=1 gate_in_tree
    [ "$status" -eq 1 ]
    : >"$CALLS"
    mock_command_script uname 'echo Darwin'
    gate_in_tree --rerun ci-shards
    [ "$status" -eq 2 ]
    contains "land-gate does not run the C++ suite on macOS" "$output"
    [ ! -s "$CALLS" ]
    gate_in_tree --rerun lint-xml
    [ "$status" -eq 0 ]
    [ "$(grep '^make' "$CALLS")" = "make lint-xml" ]
}

@test "a rerun of a step that tests the app build rebuilds the app and reruns every such step" {
    stamp_tree
    RC_test_ui_ci=1 gate_in_tree
    [ "$status" -eq 1 ]
    : >"$CALLS"
    gate_in_tree --rerun ui
    [ "$status" -eq 0 ]
    [ "$(grep '^make' "$CALLS")" = "$(printf 'make app\nmake test-shell BATS_REQUIRED=1\nmake test-ui-ci\nmake quality')" ]
    [ -f "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a rerun whose app build fails records its dependents SKIP and stamps nothing" {
    stamp_tree
    RC_test_ui_ci=1 gate_in_tree
    [ "$status" -eq 1 ]
    : >"$CALLS"
    RC_app=2 gate_in_tree --rerun ui
    [ "$status" -eq 1 ]
    row FAIL app-build
    row SKIP ui
    row SKIP bats
    row SKIP quality
    refute_grep "make test-ui-ci" "$CALLS"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a rerun of the app build reruns its dependents, so a PASS from a failed build never counts" {
    stamp_tree
    # bats runs after a failed build, against whatever binary is on disk.
    RC_app=2 gate_in_tree
    [ "$status" -eq 1 ]
    row PASS bats
    : >"$CALLS"
    RC_test_shell=1 gate_in_tree --rerun app-build,plugin
    [ "$status" -eq 1 ]
    grep -qx "make test-shell BATS_REQUIRED=1" "$CALLS"
    row PASS app-build
    row FAIL bats
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    gate_in_tree --rerun app-build,plugin
    [ "$status" -eq 0 ]
    [ -f "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a rerun whose tree changes while it runs merges nothing" {
    stamp_tree
    RC_lint_xml=1 gate_in_tree
    [ "$status" -eq 1 ]
    HOOK_lint_xml="echo edited >f" gate_in_tree --rerun lint-xml
    [ "$status" -eq 1 ]
    contains "nothing was merged" "$output"
    grep -q "^FAIL " "$LAND_GATE_LOGDIR/results/$SHA/lint-xml"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a red full run of a stamped commit removes its stamp, here and in the shared record" {
    stamp_tree
    gate_in_tree
    [ "$status" -eq 0 ]
    [ -f "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    [ -f "$HELIX_LAND_GATE_SHARED_STAMP_DIR/$SHA" ]
    RC_lint_xml=1 gate_in_tree
    [ "$status" -eq 1 ]
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    [ ! -e "$HELIX_LAND_GATE_SHARED_STAMP_DIR/$SHA" ]
}

# kill_lane_hook <lane>: shell that, run from inside a step, kill the lane running it, as a crash
# would; the lane then never writes the rows of its remaining steps.
kill_lane_hook() {
    printf '%s' 'p=$PPID; while [ "${p:-1}" -gt 1 ]; do case "$(ps -o args= -p "$p")" in *"--lane '"$1"'"*) kill -9 "$p"; break ;; esac; p=$(ps -o ppid= -p "$p" | tr -d " "); done'
}

@test "a second run of a commit whose lane dies leaves no earlier PASS standing" {
    stamp_tree
    gate_in_tree
    [ "$status" -eq 0 ]
    HOOK_lint_xml="$(kill_lane_hook fresh)" gate_in_tree
    [ "$status" -eq 1 ]
    contains "no result: its lane died" "$output"
    [ ! -e "$LAND_GATE_LOGDIR/results/$SHA/lint-xml" ]
    gate_in_tree --rerun python
    [ "$status" -eq 1 ]
    row FAIL lint-xml
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a rerun on another commit or a dirty tree is refused and writes nothing" {
    stamp_tree
    RC_test_ui_ci=1 gate_in_tree
    [ "$status" -eq 1 ]
    echo b >"$T/f"
    : >"$CALLS"
    gate_in_tree --rerun ui
    [ "$status" -eq 2 ]
    contains "needs a clean tree" "$output"
    git -C "$T" -c user.email=t@t -c user.name=t commit -qam moved
    gate_in_tree --rerun ui
    [ "$status" -eq 2 ]
    contains "no gate results for HEAD $(git -C "$T" rev-parse --short HEAD)" "$output"
    refute_grep "^make" "$CALLS"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$(git -C "$T" rev-parse HEAD)" ]
}

@test "a gate on a tree dirty at the start records no results, even if clean at the end" {
    stamp_tree
    echo b >"$T/f"
    MAKE_QUALITY_HOOK="echo a >f" RC_test_ui_ci=1 gate_in_tree
    [ "$status" -eq 1 ]
    lacks "STAGES=" "$output"
    [ -z "$(git -C "$T" status --porcelain)" ]
    gate_in_tree --rerun ui
    [ "$status" -eq 2 ]
    contains "no gate results for HEAD" "$output"
}

@test "a rerun whose step still fails, or that leaves another step red, writes nothing" {
    stamp_tree
    RC_test_ui_ci=1 RC_lint_xml=1 gate_in_tree
    [ "$status" -eq 1 ]
    RC_test_ui_ci=1 gate_in_tree --rerun ui
    [ "$status" -eq 1 ]
    row FAIL ui
    contains "LAND GATE: FAIL" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    gate_in_tree --rerun ui
    [ "$status" -eq 1 ]
    row PASS ui
    row FAIL lint-xml
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
    gate_in_tree --rerun lint-xml
    [ "$status" -eq 0 ]
    [ -f "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "a rerun of a step the gate does not have is refused before anything runs" {
    stamp_tree
    gate_in_tree
    [ "$status" -eq 0 ]
    rm -f "$HELIX_LAND_GATE_STAMP_DIR/$SHA"
    : >"$CALLS"
    gate_in_tree --rerun ui,nope
    [ "$status" -eq 2 ]
    contains "land-gate has no step 'nope'" "$output"
    gate_in_tree --rerun ""
    [ "$status" -eq 2 ]
    contains "--rerun needs step names" "$output"
    [ ! -s "$CALLS" ]
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$SHA" ]
}

@test "make land-gate STAGES= hands the steps to the script as --rerun" {
    mkdir -p "$BATS_TEST_TMPDIR/mkfrag/scripts"
    printf '#!/bin/sh\necho "stub gate args=[$*]"\n' >"$BATS_TEST_TMPDIR/mkfrag/scripts/land-gate.sh"
    chmod +x "$BATS_TEST_TMPDIR/mkfrag/scripts/land-gate.sh"
    run run_fragment '^CI_SHARDS [?]=' land-gate land-gate
    [ "$status" -eq 0 ]
    contains "stub gate args=[]" "$output"
    run run_fragment '^CI_SHARDS [?]=' land-gate land-gate STAGES=ui,bats
    contains "stub gate args=[--rerun ui,bats]" "$output"
}

@test "test-shell with BATS_REQUIRED=1 fails when bats is missing; without it, skips" {
    run run_fragment '^BATS_BIN [?]=' test-shell test-shell BATS_BIN=no-such-bats BATS_REQUIRED=1
    [ "$status" -ne 0 ]
    contains "BATS_REQUIRED=1" "$output"
    run run_fragment '^BATS_BIN [?]=' test-shell test-shell BATS_BIN=no-such-bats
    [ "$status" -eq 0 ]
    contains "skipping shell tests" "$output"
}

# A stubbed step that re-enters the gate (a make target running land-gate, or a
# stub calling make again) must end in a FAIL row, never in unbounded recursion.
@test "stubbed lanes cannot recurse: a nested gate refuses and a re-entering stub stops" {
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; for a in "$@"; do case "$a" in -*|*=*) ;; *) t=$a ;; esac; done
echo "make $t" >> "$CALLS"
case "$t" in
    test-shell) "$NESTED_GATE"; exit $? ;;
    lint-xml) fake-make --no-print-directory again; exit $? ;;
esac
exit 0'
    NESTED_GATE="$GATE" run /usr/bin/timeout 120 "$GATE"
    [ "$status" -eq 1 ]
    row FAIL bats
    row FAIL lint-xml
    grep -q "land-gate is already running" "$LAND_GATE_LOGDIR/bats.log"
    grep -q "fake-make re-entered" "$LAND_GATE_LOGDIR/lint-xml.log"
    [ "$(grep -c '^make' "$CALLS")" -lt 20 ]
}

# A gate launched detached (`( make land-gate ) &` from a shell that exits) is
# reparented to init, so the session holding worktree:<tree> is no ancestor of
# it; only the session id it inherited ties it to that claim. The holder here is
# no ancestor of the gate either, so only the id can match it.
detached_gate_fixture() {
    T="$BATS_TEST_TMPDIR/tree"
    git init -q "$T"
    echo a >"$T/f"; git -C "$T" add f
    git -C "$T" -c user.email=t@t -c user.name=t commit -q -m init
    export HELIX_CLAIM="$REPO_ROOT/scripts/helix-claim" HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    # The session's own claim: taken with --session by a stand-in `claude`
    # process that outlives the take.
    mkdir -p "$BATS_TEST_TMPDIR/fakebin"
    ln -sf "$(command -v bash)" "$BATS_TEST_TMPDIR/fakebin/claude"
    (cd "$T" && exec env HELIX_SESSION_ID=sess-a "$BATS_TEST_TMPDIR/fakebin/claude" -c \
        '"$1" take worktree:tree "session edit" --session >/dev/null || exit 1; exec sleep 120' \
        _ "$HELIX_CLAIM") &
    HOLDER=$!
    local i
    for i in $(seq 100); do
        (cd "$T" && "$HELIX_CLAIM" check worktree:tree >/dev/null 2>&1) || return 0
        sleep 0.05
    done
    return 1
}

teardown() {
    [ -n "${HOLDER:-}" ] && kill "$HOLDER" 2>/dev/null
    return 0
}

@test "a detached gate runs under its own session's worktree: claim" {
    detached_gate_fixture
    run env HELIX_SESSION_ID=sess-a bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    contains "LAND GATE: PASS" "$output"
}

@test "a detached gate outside any session is refused, and says why" {
    detached_gate_fixture
    run env -u HELIX_SESSION_ID -u CLAUDE_CODE_SESSION_ID bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 2 ]
    contains "is held by another session" "$output"
    contains "outside a session" "$output"
    refute_grep "^make" "$CALLS"
}

@test "with no history the gate says it has no measured time, and records one" {
    run "$GATE"
    [ "$status" -eq 0 ]
    contains "no measured land-gate time yet for local sweeps" "$output"
    read -r _ secs verdict where <"$LAND_GATE_HISTORY"
    [[ "$secs" =~ ^[0-9]+$ ]] || fail "seconds not recorded: $secs"
    [ "$verdict" = PASS ]
    [ "$where" = local ]
}

@test "the ETA is the median and longest of past gates that swept in the same place" {
    printf '%s\n' "1 600 PASS local" "2 1200 FAIL local" "3 900 PASS local" \
        "4 5000 PASS remote" >"$LAND_GATE_HISTORY"
    run "$GATE"
    [ "$status" -eq 0 ]
    contains "expect ~15m (median of the last 3), up to 20m" "$output"
}

@test "a failing gate is recorded as FAIL" {
    RC_lint_xml=1 run "$GATE"
    [ "$status" -ne 0 ]
    [ "$(awk '{print $3}' "$LAND_GATE_HISTORY")" = FAIL ]
}

@test "on macOS a local C++ run refuses in one line naming the test host" {
    mock_command_script uname 'echo Darwin'
    run "$GATE"
    [ "$status" -eq 2 ]
    [ "${#lines[@]}" -eq 1 ]
    contains "TEST_HOST=1 make land-gate sends it to testhost.invalid" "$output"
    [ ! -s "$CALLS" ]
}

@test "on macOS with no test host configured the refusal says how to name one" {
    mock_command_script uname 'echo Darwin'
    HELIX_TEST_HOST= run "$GATE"
    [ "$status" -eq 2 ]
    contains "set HELIX_TEST_HOST in ~/.config/helixscreen/build-hosts.env" "$output"
}

@test "on macOS a C++ run already routed to the test host runs the gate" {
    mock_command_script uname 'echo Darwin'
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "host-run sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1" "$CALLS"
}
