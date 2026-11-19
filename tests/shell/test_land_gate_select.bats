#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What make land-gate runs for a change: scripts/gate_select.py's plan, the C++
# selection from the build's dependency files and its safety valves, and how
# scripts/land-gate.sh and mk/tests.mk act on them. make and the checks are
# stubbed as in test_land_gate.bats; the selector runs for real, on a change
# handed to it as a list of paths (LAND_GATE_CHANGED_FILE, --changed).

load helpers

REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
GATE="$REPO_ROOT/scripts/land-gate.sh"
SELECT="$REPO_ROOT/scripts/gate_select.py"

setup() {
    export JOBPOOL=0
    unset FAKE_MAKE_DEPTH LAND_GATE_ACTIVE STAGES HELIX_GATE_FULL
    export TMPDIR="$BATS_TEST_TMPDIR"
    export CALLS="$BATS_TEST_TMPDIR/calls"
    export LAND_GATE_LOGDIR="$BATS_TEST_TMPDIR/logs"
    export LAND_GATE_HISTORY="$BATS_TEST_TMPDIR/history.tsv"
    export HELIX_LAND_GATE_STAMP_DIR="$BATS_TEST_TMPDIR/stamps"
    export HELIX_LAND_GATE_SHARED_STAMP_DIR="$BATS_TEST_TMPDIR/shared-stamps"
    export LAND_GATE_CHANGED_FILE="$BATS_TEST_TMPDIR/changed"
    : >"$CALLS"
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; vars=""
for a in "$@"; do case "$a" in -*) ;; *=*) vars="$vars $a" ;; *) t=$a ;; esac; done
echo "make $t$vars" >> "$CALLS"
[ "$t" != quality ] || echo "qc-only: ${HELIX_QC_ONLY:-all}" >> "$CALLS"
echo "output of $t"
eval "rc=\${RC_$(echo "$t" | tr - _):-0}"
exit "$rc"'
    mock_command_script fake-test-host-run '
echo "host-run $*" >> "$CALLS"
case "$1" in --probe) exit 1 ;; esac'
    mock_command_script fake-claim '
echo "claim $*" >> "$CALLS"
exit 0'
    mock_command_script fake-esp32 'echo "esp32 $*" >> "$CALLS"'
    mock_command_script fake-no-mocks 'echo "no-mocks $*" >> "$CALLS"'
    export MAKE=fake-make TEST_HOST_RUN=fake-test-host-run HELIX_CLAIM=fake-claim
    export ESP32_CHECK=fake-esp32 NO_MOCKS_CHECK=fake-no-mocks LAND_GATE_BASE_REF=HEAD
    export HELIX_TEST_HOST=testhost.invalid HELIX_BUILD_HOSTS_FILE=/nonexistent
    unset CI_SHARDS CI_SHARD_TIMEOUT
    export TEST_HOST=0
}

change() { printf '%s\n' "$@" >"$LAND_GATE_CHANGED_FILE"; }

row() { # <status> <step>: the table row for step has that status
    printf '%s\n' "$output" | grep -Eq "^  $1 +$2 "
}

# --------------------------------------------------------------------------
# The gate acts on the plan
# --------------------------------------------------------------------------

@test "a docs-only change builds nothing, runs the doc gates alone, and passes" {
    change docs/devel/BUILD_SYSTEM.md README.md
    run "$GATE"
    [ "$status" -eq 0 ]
    for s in app-build ci-shards slow ui plugin python theme-tokens xml-schema lint-xml \
        lvgl-event-codes esp32 no-mocks; do
        row SKIP "$s"
    done
    row PASS quality
    row PASS bats
    grep -qx "qc-only: qc_doc_refs qc_doc_anchors qc_doc_images" "$CALLS"
    refute_grep "^make app$" "$CALLS"
    refute_grep "unit-sweep" "$CALLS"
    contains "→ plan: docs" "$output"
    contains "LAND GATE: PASS" "$output"
}

@test "a C++ source change hands the change to the sweep and the slow tier, and skips the plugin" {
    change src/ui/tour/tour_steps.cpp
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1 TEST_SELECT_CHANGED=src/ui/tour/tour_steps.cpp" "$CALLS"
    grep -qx "make test-slow SHARD_CRASH_FAILS=1 TEST_SELECT_CHANGED=src/ui/tour/tour_steps.cpp" "$CALLS"
    grep -q "^make test-python PYTHON_TESTS=tests/python/test_cpp_escape_decoding.py " "$CALLS"
    grep -qx "make test-ui-ci" "$CALLS"
    grep -qx "qc-only: all" "$CALLS"
    row SKIP plugin
    row PASS no-mocks
    grep -q "^make test-shell BATS_REQUIRED=1 BATS_APP=without BATS_GATE=1 BATS_BASE=" "$CALLS"
}

@test "a test-host sweep gets the change on its make line too" {
    change src/ui/tour/tour_steps.cpp tests/unit/test_first_run_tour.cpp
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "host-run sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1 TEST_SELECT_CHANGED=src/ui/tour/tour_steps.cpp,tests/unit/test_first_run_tour.cpp" "$CALLS"
    grep -qx "host-run exec -- make --no-print-directory test-slow SHARD_CRASH_FAILS=1 TEST_SELECT_CHANGED=src/ui/tour/tour_steps.cpp,tests/unit/test_first_run_tour.cpp" "$CALLS"
}

@test "a change no C++ test can reach skips the sweep and the slow tier" {
    change moonraker-plugin/helix_print.py
    run "$GATE"
    [ "$status" -eq 0 ]
    row SKIP ci-shards
    row SKIP slow
    row PASS plugin
    refute_grep "unit-sweep" "$CALLS"
}

@test "HELIX_GATE_FULL=1 runs every step, the whole sweep and every bats file" {
    change docs/devel/BUILD_SYSTEM.md
    HELIX_GATE_FULL=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1" "$CALLS"
    grep -qx "make test-slow SHARD_CRASH_FAILS=1" "$CALLS"
    grep -qx "make test-shell BATS_REQUIRED=1 BATS_APP=without BATS_ALL=1" "$CALLS"
    grep -qx "qc-only: all" "$CALLS"
    refute_grep "SKIP" "$LAND_GATE_LOGDIR/rows/"*
}

@test "a build-system change runs the whole sweep: no TEST_SELECT_CHANGED" {
    change mk/tests.mk src/ui/tour/tour_steps.cpp
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1" "$CALLS"
    contains "mk/tests.mk changed (build system or submodule)" "$output"
}

@test "a skipped step counts as a pass: the commit is stamped and the history records SKIP" {
    change docs/devel/BUILD_SYSTEM.md
    git -C "$REPO_ROOT" diff --quiet HEAD && [ -z "$(git -C "$REPO_ROOT" status --porcelain)" ] ||
        skip "the tree is dirty, so no stamp is written"
    run "$GATE"
    [ "$status" -eq 0 ]
    contains "recorded: pushing" "$output"
    grep -q " ci-shards=SKIP:- " "$LAND_GATE_HISTORY"
}

@test "the table names why each skipped step was not reached" {
    change docs/devel/BUILD_SYSTEM.md
    run "$GATE"
    printf '%s\n' "$output" | grep -Eq '^  SKIP +ci-shards +- +not reached: no changed path reaches a C\+\+ test$'
}

@test "with no plan (the selector fails) everything runs" {
    change src/ui/tour/tour_steps.cpp
    GATE_SELECT=false run "$GATE"
    [ "$status" -eq 0 ]
    contains "→ no plan" "$output"
    grep -qx "make unit-sweep NPROCS=8 SHARD_TIMEOUT=600 SHARD_CRASH_FAILS=1" "$CALLS"
    grep -qx "make test-shell BATS_REQUIRED=1 BATS_APP=without BATS_ALL=1" "$CALLS"
}

# --------------------------------------------------------------------------
# The plan
# --------------------------------------------------------------------------

plan_of() { # <stage> <changed path>...: "<verdict> <why>" for the stage
    local stage=$1
    shift
    python3 "$SELECT" --root "$REPO_ROOT" plan --changed "$(IFS=,; echo "$*")" |
        awk -F'\t' -v s="$stage" '$1 == s { print $2 " " $3 }'
}

@test "plan: a test-only change runs the changed tests and quality, not esp32 or no-mocks" {
    [[ "$(plan_of ci-shards tests/unit/test_version.cpp)" == select* ]] || fail "verdict differs"
    [[ "$(plan_of quality tests/unit/test_version.cpp)" == run* ]] || fail "verdict differs"
    [[ "$(plan_of esp32 tests/unit/test_version.cpp)" == skip* ]] || fail "verdict differs"
    [[ "$(plan_of no-mocks tests/unit/test_version.cpp)" == skip* ]] || fail "verdict differs"
    python3 "$SELECT" --root "$REPO_ROOT" plan --changed tests/unit/test_version.cpp | grep -q "^# mode	tests"
}

@test "plan: python runs whole for scripts/, the C++ readers for src/, nothing for tests/unit or a doc" {
    [[ "$(plan_of python scripts/doc_anchors.py)" == run* ]] || fail "verdict differs"
    [[ "$(plan_of python src/app_globals.cpp)" == select* ]] || fail "verdict differs"
    [[ "$(plan_of python tests/unit/test_version.cpp)" == skip* ]] || fail "verdict differs"
    [[ "$(plan_of python scripts/CLAUDE.md)" == skip* ]] || fail "verdict differs"
}

@test "plan: ui runs for anything the app is built from, not for scripts or tests" {
    [[ "$(plan_of ui ui_xml/home_panel.xml)" == run* ]] || fail "verdict differs"
    [[ "$(plan_of ui scripts/doc_anchors.py)" == skip* ]] || fail "verdict differs"
    [[ "$(plan_of ui tests/unit/test_version.cpp)" == skip* ]] || fail "verdict differs"
    [[ "$(plan_of ui tests/ui/test_navigation.py)" == run* ]] || fail "verdict differs"
}

@test "valve: the test harness, a build step or a submodule runs the whole C++ suite" {
    [[ "$(plan_of ci-shards tests/test_main.cpp)" == "full tests/test_main.cpp changed (test harness"* ]] || fail "verdict differs"
    [[ "$(plan_of ci-shards tests/lvgl_ui_test_fixture.cpp)" == full* ]] || fail "verdict differs"
    [[ "$(plan_of ci-shards scripts/gen-git-hash.sh)" == full* ]] || fail "verdict differs"
    [[ "$(plan_of ci-shards lib/lvgl)" == full* ]] || fail "verdict differs"
    [[ "$(plan_of ci-shards patches/lvgl_x.patch)" == full* ]] || fail "verdict differs"
    [[ "$(plan_of ci-shards lv_conf.h)" == full* ]] || fail "verdict differs"
    [[ "$(plan_of ci-shards Makefile)" == full* ]] || fail "verdict differs"
    # A header beside the harness is followed through the dependency files.
    [[ "$(plan_of ci-shards tests/lvgl_ui_test_fixture.h)" == select* ]] || fail "verdict differs"
}

@test "valve: more than 200 changed paths runs the whole C++ suite" {
    local many=() i
    for i in $(seq 1 201); do many+=("src/f$i.cpp"); done
    [[ "$(plan_of ci-shards "${many[@]}")" == "full 201 changed paths, over 200" ]] || fail "verdict differs"
    [[ "$(plan_of ci-shards "${many[@]:1}")" == select* ]] || fail "verdict differs"
}

@test "valve: a path that cannot travel as one word of TEST_SELECT_CHANGED runs the whole C++ suite" {
    [[ "$(plan_of ci-shards 'src/a b.cpp')" == "full 'src/a b.cpp' cannot travel in TEST_SELECT_CHANGED" ]] || fail "verdict differs"
    [[ "$(plan_of ci-shards 'src/a$b.cpp')" == full* ]] || fail "verdict differs"
}

@test "valve: no change at all runs everything" {
    python3 "$SELECT" --root "$REPO_ROOT" plan --changed "" | grep -q "^# mode	full"
    [[ "$(plan_of ui)" == "run no change to select from" ]] || fail "verdict differs"
}

@test "valve: HELIX_GATE_FULL=1 and a release branch run everything" {
    [[ "$(HELIX_GATE_FULL=1 plan_of ci-shards docs/x.md)" == "full HELIX_GATE_FULL=1" ]] || fail "verdict differs"
    [[ "$(HELIX_GATE_FULL=1 plan_of ui docs/x.md)" == run* ]] || fail "verdict differs"
    local repo="$BATS_TEST_TMPDIR/rel"
    git init -q -b release/9.9 "$repo"
    [[ "$(python3 "$SELECT" --root "$repo" plan --changed docs/x.md | awk -F'\t' '$1 == "ci-shards" { print $2 " " $3 }')" == "full the branch is release/9.9" ]] || fail "verdict differs"
}

# --------------------------------------------------------------------------
# The C++ selection, on a fixture build: dependency files and a test binary
# --------------------------------------------------------------------------

# A tree of ten test units. test_a includes a.h, test_b b.h, and every unit
# includes core.h; src/a.cpp implements a.h and src/a_extra.cpp extends it.
# test_c names ui_xml/c_panel.xml's component and test_d the fixture d.json.
# The fake helix-tests lists one case per unit, and a [slow] one for test_a.
cxx_fixture() {
    local root="$BATS_TEST_TMPDIR/tree" u
    mkdir -p "$root/tests/unit" "$root/obj/tests" "$root/include" "$root/src" "$root/bin"
    for u in a b c d e f g h i j; do
        printf '// test %s\n' "$u" >"$root/tests/unit/test_$u.cpp"
        printf 'obj/tests/test_%s.o: tests/unit/test_%s.cpp include/core.h \\\n  tests/unit/../catch_amalgamated.hpp\n' \
            "$u" "$u" >"$root/obj/tests/test_$u.d"
    done
    printf 'obj/tests/test_a.o: tests/unit/test_a.cpp include/a.h include/core.h\n' >"$root/obj/tests/test_a.d"
    printf 'obj/tests/test_b.o: tests/unit/test_b.cpp include/b.h include/core.h\n' >"$root/obj/tests/test_b.d"
    printf 'obj/a.o: src/a.cpp include/a.h include/core.h\n' >"$root/obj/a.d"
    printf 'obj/a_extra.o: src/a_extra.cpp include/a.h\n' >"$root/obj/a_extra.d"
    printf 'obj/core.o: src/core.cpp include/core.h\n' >"$root/obj/core.d"
    echo 'auto* p = create("c_panel");' >"$root/tests/unit/test_c.cpp"
    echo 'load_fixture("d.json");' >"$root/tests/unit/test_d.cpp"
    cat >"$root/bin/helix-tests" <<'SH'
#!/bin/sh
spec="$1"
echo '<?xml version="1.0" encoding="UTF-8"?>'
echo '<MatchingTests>'
for u in a b c d e f g h i j; do
    case "$spec" in
        "[slow]"*) [ "$u" = a ] || continue; name="slow, [$u] \"case\"" ;;
        *) name="case $u" ;;
    esac
    printf '<TestCase><Name>%s</Name><SourceInfo><File>tests/unit/test_%s.cpp</File></SourceInfo></TestCase>\n' "$name" "$u"
done
echo '</MatchingTests>'
SH
    chmod +x "$root/bin/helix-tests"
    echo "$root"
}

cxx() { # <root> <filter> <changed,...>: run the selector; the input file is $root/out.txt
    python3 "$SELECT" --root "$1" cxx --obj-dir "$1/obj" --bin "$1/bin/helix-tests" \
        --filter "$2" --changed "$3" --out "$1/out.txt"
}

@test "cxx: a changed header selects the units whose dependency files list it" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "~[.] ~[slow]" include/b.h
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "SELECT 1 case(s) in 1 file(s): 1 of 10 test units reached" ]
    [ "$(cat "$root/out.txt")" = '"case b"' ]
}

@test "cxx: a changed .cpp selects the units including the header it implements" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "~[.] ~[slow]" src/a.cpp
    [ "${lines[0]}" = "SELECT 1 case(s) in 1 file(s): 1 of 10 test units reached" ]
    contains "tests/unit/test_a.cpp: includes include/a.h, which src/a.cpp implements" "$output"
    # a_extra.cpp implements a.h by its longest stem prefix.
    run cxx "$root" "~[.] ~[slow]" src/a_extra.cpp
    [ "$(cat "$root/out.txt")" = '"case a"' ]
}

@test "cxx: a changed test unit selects itself; data selects the units that name it" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "~[.] ~[slow]" tests/unit/test_e.cpp,ui_xml/c_panel.xml,tests/fixtures/d.json
    [ "$(sort "$root/out.txt" | tr '\n' ' ')" = '"case c" "case d" "case e" ' ]
}

@test "cxx: names with Catch2's special characters are escaped in the input file" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "[slow] ~[serial]" include/a.h
    [ "$(cat "$root/out.txt")" = '"slow\, \[a\] \"case\""' ]
}

@test "cxx: a unit with no case matching the filter selects nothing" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "[slow] ~[serial]" include/b.h
    [ "${lines[0]}" = "NONE 1 of 10 test units reached, but none has a case matching [slow] ~[serial]" ]
    [ ! -s "$root/out.txt" ]
}

@test "cxx: a header most units include selects them all, which the selection cap turns into everything" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "~[.] ~[slow]" include/core.h
    [ "${lines[0]}" = "FULL 10 of 10 test units reached, over 50%" ] || fail "${lines[0]}"
    run cxx "$root" "~[.] ~[slow]" src/core.cpp
    [ "${lines[0]}" = "FULL 10 of 10 test units reached, over 50%" ]
}

@test "cxx valve: reaching more than half the units runs everything" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "~[.] ~[slow]" tests/unit/test_a.cpp,tests/unit/test_b.cpp,tests/unit/test_c.cpp,tests/unit/test_d.cpp,tests/unit/test_e.cpp
    [[ "${lines[0]}" == SELECT* ]] || fail "verdict differs"
    run cxx "$root" "~[.] ~[slow]" tests/unit/test_a.cpp,tests/unit/test_b.cpp,tests/unit/test_c.cpp,tests/unit/test_d.cpp,tests/unit/test_e.cpp,tests/unit/test_f.cpp
    [ "${lines[0]}" = "FULL 6 of 10 test units reached, over 50%" ]
}

@test "cxx valve: no test dependency files (tests not built) runs everything" {
    local root
    root=$(cxx_fixture)
    rm "$root"/obj/tests/*.d
    run cxx "$root" "~[.] ~[slow]" include/b.h
    [ "${lines[0]}" = "FULL no test dependency files to read (tests not built)" ]
}

@test "cxx: a change that reaches no unit selects nothing" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "~[.] ~[slow]" include/nobody.h
    [ "${lines[0]}" = "NONE 1 changed path(s), none reached by a test unit" ]
}

# --------------------------------------------------------------------------
# mk/tests.mk's select_cases
# --------------------------------------------------------------------------

# A scratch Makefile with select_cases and a target that prints what it set,
# beside a stub selector that records its arguments and answers $FAKE_VERDICT.
select_cases_makefile() {
    local dir="$BATS_TEST_TMPDIR/selmk"
    mkdir -p "$dir/scripts"
    {
        printf 'NPROCS := 8\nBUILD_DIR := build\nOBJ_DIR := build/obj\nTEST_BIN := build/bin/helix-tests\n'
        awk '/^TEST_SELECT_CHANGED [?]=/ { on = 1 } on { print } on && /^endef/ { exit }' "$REPO_ROOT/mk/tests.mk"
        printf 'probe:\n\t@$(call select_cases,~[.] ~[slow],fast); echo "verdict=$$sel_verdict file=$$sel_file shards=$$sel_shards"\n'
    } >"$dir/Makefile"
    cat >"$dir/scripts/gate_select.py" <<'PY'
import os, sys
open(os.environ["CALLS"], "a").write("select " + " ".join(sys.argv[1:]) + "\n")
out = sys.argv[sys.argv.index("--out") + 1]
os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, "w").write("".join('"case %d"\n' % i for i in range(int(os.environ.get("FAKE_CASES", "0")))))
print(os.environ["FAKE_VERDICT"] + " why")
PY
    echo "$dir"
}

@test "select_cases: no TEST_SELECT_CHANGED is the whole filter, and runs no selector" {
    local dir
    dir=$(select_cases_makefile)
    run make --no-print-directory -C "$dir" probe
    [ "$status" -eq 0 ]
    contains "verdict=FULL file= shards=8" "$output"
    [ ! -s "$CALLS" ]
}

@test "select_cases: the selector's verdict, its input file, and one shard per case up to NPROCS" {
    local dir
    dir=$(select_cases_makefile)
    FAKE_VERDICT=SELECT FAKE_CASES=3 run make --no-print-directory -C "$dir" probe TEST_SELECT_CHANGED=src/a.cpp,include/b.h
    [ "$status" -eq 0 ]
    contains "verdict=SELECT file=build/test-select/fast.txt shards=3" "$output"
    contains "[test-select fast] SELECT why" "$output"
    grep -q -- "cxx --changed src/a.cpp,include/b.h --obj-dir build/obj --bin build/bin/helix-tests --filter ~\[.\] ~\[slow\] --out build/test-select/fast.txt" "$CALLS"
    FAKE_VERDICT=SELECT FAKE_CASES=30 run make --no-print-directory -C "$dir" probe TEST_SELECT_CHANGED=src/a.cpp
    contains "shards=8" "$output"
    FAKE_VERDICT=NONE run make --no-print-directory -C "$dir" probe TEST_SELECT_CHANGED=src/a.cpp
    contains "verdict=NONE" "$output"
}

@test "select_cases: a selector that fails fails the recipe" {
    local dir
    dir=$(select_cases_makefile)
    printf 'import sys\nsys.exit(2)\n' >"$dir/scripts/gate_select.py"
    run make --no-print-directory -C "$dir" probe TEST_SELECT_CHANGED=src/a.cpp
    [ "$status" -ne 0 ]
    lacks "verdict=" "$output"
}
