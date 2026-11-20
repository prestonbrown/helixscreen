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
[ "$t" != quality ] || [ -n "${HELIX_QC_CHANGED_FROM:-}" ] || echo "qc-only: all" >> "$CALLS"
[ "$t" != quality ] || [ -z "${HELIX_QC_CHANGED_FROM:-}" ] || echo "qc-changed: $HELIX_QC_CHANGED_FROM" >> "$CALLS"
[ "$t" != quality ] || [ -z "${HELIX_QC_GATE_CHANGED:-}" ] || echo "qc-scoped: $HELIX_QC_GATE_CHANGED" >> "$CALLS"
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
    grep -q "^qc-changed: " "$CALLS"
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
    grep -q "^make test-ui-ci UI_TESTS=tests/ui/test_detection_sources_boot.py " "$CALLS"
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

@test "a skipped step counts as a pass, and the history records SKIP" {
    change docs/devel/BUILD_SYSTEM.md
    run "$GATE"
    [ "$status" -eq 0 ]
    contains "LAND GATE: PASS" "$output"
    grep -q " ci-shards=SKIP:- " "$LAND_GATE_HISTORY"
}

@test "a gate judging a change from LAND_GATE_CHANGED_FILE never stamps the commit" {
    local repo
    repo=$(gate_clone)
    export LAND_GATE_LOGDIR="$repo/build/land-gate"
    change src/a.cpp
    cd "$repo"
    run ./scripts/land-gate.sh
    [ "$status" -eq 0 ] || fail "$output"
    contains "LAND_GATE_CHANGED_FILE" "$output"
    lacks "recorded: pushing" "$output"
    [ -z "$(ls -A "$HELIX_LAND_GATE_STAMP_DIR" 2>/dev/null)" ] || fail "stamped: $(ls "$HELIX_LAND_GATE_STAMP_DIR")"
    unset LAND_GATE_CHANGED_FILE
    run ./scripts/land-gate.sh
    contains "recorded: pushing" "$output"
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

ui_files_of() { # <changed path>...: the files the ui row selects, one line
    python3 "$SELECT" --root "$REPO_ROOT" plan --changed "$(IFS=,; echo "$*")" |
        awk -F'\t' '$1 == "ui" { print $4 }'
}

@test "plan: ui runs for anything the app is built from, not for scripts or tests" {
    [[ "$(plan_of ui ui_xml/home_panel.xml)" == select* ]] || fail "verdict differs"
    [[ "$(plan_of ui scripts/doc_anchors.py)" == skip* ]] || fail "verdict differs"
    [[ "$(plan_of ui tests/unit/test_version.cpp)" == skip* ]] || fail "verdict differs"
    [[ "$(plan_of ui tests/ui/test_freeze.py)" == select* ]] || fail "verdict differs"
}

@test "ui: the smoke files always run, plus the files whose screens the change reaches" {
    local smoke="tests/ui/test_detection_sources_boot.py tests/ui/test_navigation.py tests/ui/test_state.py tests/ui/test_text.py tests/ui/test_xml_expressions_boot.py"
    [ "$(ui_files_of src/printer/printer_state.cpp)" = "$smoke" ] || fail "$(ui_files_of src/printer/printer_state.cpp)"
    [ "$(ui_files_of include/ui_panel_bed_mesh.h)" = "$smoke" ] || fail "a panel's header is no framework header"
    [[ " $(ui_files_of include/ui_panel_motion.h) " == *" tests/ui/test_motion_small_screen_geometry.py "* ]] ||
        fail "a header reaches what its source's trigger does"
    [[ " $(ui_files_of ui_xml/motion_panel.xml) " == *" tests/ui/test_motion_small_screen_geometry.py "* ]] || fail "trigger"
    [[ " $(ui_files_of ui_xml/motion_panel.xml) " != *" tests/ui/test_modal_geometry.py "* ]] || fail "too wide"
    [[ " $(ui_files_of tests/ui/test_freeze.py) " == *" tests/ui/test_freeze.py "* ]] || fail "a changed file runs"
    # A file naming a changed component runs without a trigger row.
    [[ " $(ui_files_of ui_xml/action_prompt_modal.xml) " == *" tests/ui/test_language_switch_sweep.py "* ]] || fail "named"
    [[ " $(ui_files_of ui_xml/runout_guidance_modal.xml) " == *" tests/ui/test_reset.py "* ]] || fail "trigger row"
    [[ " $(ui_files_of ui_xml/history_list_row.xml) " == *" tests/ui/test_history_navigation.py "* ]]
}

@test "ui valve: the harness, globals.xml, the UI framework, startup, ctl, the mock printer and a building block (component or widget source) run every file" {
    local p
    for p in tests/ui/conftest.py tests/ui/helix/app.py ui_xml/globals.xml src/ui/ui_nav_manager.cpp include/ui_nav_manager.h \
        src/ui/theme/theme_responsive.cpp src/application/application.cpp src/remote/remote_control_server.cpp \
        src/mock/moonraker_client_mock.cpp src/api/moonraker_client_mock_gcode.cpp \
        src/printer/ams_backend_mock.cpp include/mock_persona.h assets/fonts/NotoSans-Regular.ttf ui_xml/icon.xml \
        src/ui/components/ui_button.cpp src/ui/settings/setting_group.cpp; do
        [[ "$(plan_of ui "$p")" == full* ]] || fail "$p: $(plan_of ui "$p")"
    done
}

@test "ui valve: a selection of most of the files runs every file" {
    [[ "$(plan_of ui ui_xml/home_panel.xml ui_xml/print_select_panel.xml ui_xml/history_list_panel.xml \
        ui_xml/motion_panel.xml ui_xml/runout_guidance_modal.xml ui_xml/ams_panel.xml)" == select* ]] ||
        fail "twelve of the ui files is a selection"
    [[ "$(plan_of ui ui_xml/home_panel.xml ui_xml/print_select_panel.xml ui_xml/history_list_panel.xml \
        ui_xml/motion_panel.xml ui_xml/runout_guidance_modal.xml ui_xml/ams_panel.xml \
        tests/ui/test_freeze.py tests/ui/test_wait_idle.py src/system/screenshot.cpp)" == "full 15 of "*" files reached" ]]
}

@test "a ui selection reaches make as UI_TESTS, and test-ui-ci keeps CI's other arguments" {
    change ui_xml/motion_panel.xml
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^make test-ui-ci UI_TESTS=tests/ui/test_detection_sources_boot.py tests/ui/test_motion_small_screen_geometry.py " "$CALLS"
    run run_mk_fragment "$REPO_ROOT/mk/tests.mk" '^UI_CI_PYTEST_ARGS :=' test-ui-ci -n test-ui-ci "UI_TESTS=tests/ui/test_a.py tests/ui/test_b.py"
    contains 'pytest tests/ui/test_a.py tests/ui/test_b.py -v --ignore=tests/ui/test_screens.py -m "not slow";' "$output"
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
    # Data a test reaches only through the code that loads it: src/loader.cpp
    # reads assets/config/db.json, test_e includes loader.h. src/packager.cpp
    # names every data file (an asset list), which makes it no one file's loader.
    mkdir -p "$root/assets/config"
    for u in db other1 other2 other3 other4 other5; do echo '{}' >"$root/assets/config/$u.json"; done
    echo 'open("db.json");' >"$root/src/loader.cpp"
    printf 'obj/loader.o: src/loader.cpp include/loader.h\n' >"$root/obj/loader.d"
    printf 'pack("db.json", "other1.json", "other2.json", "other3.json", "other4.json", "other5.json");\n' >"$root/src/packager.cpp"
    printf 'obj/packager.o: src/packager.cpp include/packager.h\n' >"$root/obj/packager.d"
    printf 'obj/tests/test_e.o: tests/unit/test_e.cpp include/loader.h include/core.h\n' >"$root/obj/tests/test_e.d"
    printf 'obj/tests/test_f.o: tests/unit/test_f.cpp include/packager.h include/core.h\n' >"$root/obj/tests/test_f.d"
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

# --------------------------------------------------------------------------
# mk/tests.mk's selected sweep, on a scratch Makefile: tests.mk's shard runner
# and select_cases, the real unit-sweep and test-slow recipes, a stub selector,
# a stub test binary and a live (stub) jobpool, which runs each batch of
# shards in a fresh bash that sees only exported variables.
# --------------------------------------------------------------------------

sweep_makefile() {
    local dir="$BATS_TEST_TMPDIR/sweep"
    mkdir -p "$dir/scripts" "$dir/bin"
    {
        printf 'SHELL := /bin/bash\nJOBS_CLAIMED := 1\nBUILD_DIR := build\nOBJ_DIR := build/obj\nTEST_BIN := %s\nECHO := @echo\n' "$dir/bin/helix-tests"
        printf 'SHARD_ARTIFACT_ROOT := %s\n' "$dir/shards"
        awk '/^# Test Dependency System/ { exit } { print }' "$REPO_ROOT/mk/tests.mk"
        awk '/^(unit-sweep|test-slow): test-build/ { on = 1; sub(/: test-build/, ":") } on { print } on && /^$/ { on = 0 }' "$REPO_ROOT/mk/tests.mk"
        printf '\ndefine run_slow_tiers\n\techo full slow tiers\nendef\n'
    } >"$dir/Makefile"
    cat >"$dir/scripts/gate_select.py" <<'PY'
import os, sys
out = sys.argv[sys.argv.index("--out") + 1]
os.makedirs(os.path.dirname(out), exist_ok=True)
n = int(os.environ.get("FAKE_CASES", "0"))
open(out, "w").write("".join('"case %d"\n' % i for i in range(n)))
print(("SELECT %d case(s)" % n) if n else "NONE nothing")
PY
    printf '#!/bin/sh\ncase "$1" in pool) exit 0 ;; esac\nexit 0\n' >"$dir/scripts/helix-claim"
    printf '#!/bin/sh\nexit 0\n' >"$dir/scripts/triage_shard.sh"
    printf '#!/bin/sh\n[ "$1" = with-token ] && shift 2\nexec "$@"\n' >"$dir/bin/fakejp"
    # Catch2 refuses a --shard-count without a number (exit 255); otherwise
    # shard i runs the input file's cases i, i+count, ...
    cat >"$dir/bin/helix-tests" <<'SH'
#!/bin/bash
file="" count=1 index=0
while [ $# -gt 0 ]; do
    case "$1" in
        -f) file=$2; shift ;;
        --shard-count) case "$2" in ''|-*) echo "error: --shard-count needs a number" >&2; exit 255 ;; esac; count=$2; shift ;;
        --shard-index) index=$2; shift ;;
    esac
    shift
done
n=$(grep -c . "$file" 2>/dev/null || echo 0)
k=0
for ((i = index; i < n; i += count)); do k=$((k + 1)); done
[ -n "${FAKE_RUN_NONE:-}" ] && k=0
echo "All tests passed ($k assertions in $k test cases)"
exit "${FAKE_EXIT:-0}"
SH
    chmod +x "$dir/scripts/"* "$dir/bin/"*
    echo "$dir"
}

@test "a selected sweep under a live jobpool runs every selected case, sharded" {
    local dir
    dir=$(sweep_makefile)
    FAKE_CASES=5 HELIX_JOBPOOL="$dir/bin/fakejp" run make --no-print-directory -C "$dir" unit-sweep \
        TEST_SELECT_CHANGED=src/a.cpp NPROCS=8 SHARD_TIMEOUT=60
    [ "$status" -eq 0 ] || fail "$output"
    contains "Running 5 test shards" "$output"
    lacks "exit 255" "$output"
    contains "ran 5 of 5 selected case(s)" "$output"
}

@test "a selected sweep that runs fewer cases than it selected fails, crash setting or not" {
    local dir
    dir=$(sweep_makefile)
    FAKE_CASES=3 FAKE_RUN_NONE=1 HELIX_JOBPOOL="$dir/bin/fakejp" run make --no-print-directory -C "$dir" unit-sweep \
        TEST_SELECT_CHANGED=src/a.cpp NPROCS=8 SHARD_TIMEOUT=60 SHARD_CRASH_FAILS=0
    [ "$status" -ne 0 ] || fail "a sweep that ran nothing passed: $output"
    contains "selected 3 case(s), but the shards ran 0" "$output"
}

@test "a shard exiting 255 (Catch2 refusing its arguments, no signal) fails the sweep, not just warns" {
    local dir
    dir=$(sweep_makefile)
    FAKE_CASES=3 FAKE_EXIT=255 HELIX_JOBPOOL="$dir/bin/fakejp" run make --no-print-directory -C "$dir" unit-sweep \
        TEST_SELECT_CHANGED=src/a.cpp NPROCS=8 SHARD_TIMEOUT=60 SHARD_CRASH_FAILS=0
    [ "$status" -ne 0 ] || fail "$output"
    contains "failed (exit 255)" "$output"
    # A real signal after passing assertions still only warns without SHARD_CRASH_FAILS.
    FAKE_CASES=3 FAKE_EXIT=139 HELIX_JOBPOOL="$dir/bin/fakejp" run make --no-print-directory -C "$dir" unit-sweep \
        TEST_SELECT_CHANGED=src/a.cpp NPROCS=8 SHARD_TIMEOUT=60 SHARD_CRASH_FAILS=0
    [ "$status" -eq 0 ]
    contains "crashed after its assertions passed (signal 11)" "$output"
}

@test "the selected slow tier under a live jobpool runs every selected case" {
    local dir
    dir=$(sweep_makefile)
    FAKE_CASES=4 HELIX_JOBPOOL="$dir/bin/fakejp" run make --no-print-directory -C "$dir" test-slow \
        TEST_SELECT_CHANGED=src/a.cpp SLOW_SHARDS=16 SHARD_TIMEOUT=60
    [ "$status" -eq 0 ] || fail "$output"
    contains "ran 4 of 4 selected case(s)" "$output"
}

# --------------------------------------------------------------------------
# Release gates, reruns and stamps
# --------------------------------------------------------------------------

@test "valve: a release-prep branch runs everything" {
    local repo="$BATS_TEST_TMPDIR/relprep"
    git init -q -b release-prep/v9.9.0 "$repo"
    [[ "$(python3 "$SELECT" --root "$repo" plan --changed docs/x.md | awk -F'\t' '$1 == "ci-shards" { print $2 " " $3 }')" == "full the branch is release-prep/v9.9.0" ]]
}

@test "each result row records the plan it ran under, and the history marks a selected gate" {
    change src/ui/tour/tour_steps.cpp
    run "$GATE"
    [ "$status" -eq 0 ]
    [ "$(cut -d' ' -f1,3 "$LAND_GATE_LOGDIR/rows/ci-shards.plan")" = "select selected" ] ||
        fail "$(cat "$LAND_GATE_LOGDIR/rows/ci-shards.plan")"
    [ "$(cut -d' ' -f1,3 "$LAND_GATE_LOGDIR/rows/plugin.plan")" = "skip selected" ] || fail "skip row"
    grep -Eq '^[0-9]+ [0-9]+ PASS\(sel\) ' "$LAND_GATE_HISTORY" || fail "$(cat "$LAND_GATE_HISTORY")"
    grep -q " ci-shards=PASS(sel):" "$LAND_GATE_HISTORY"
}

# A clean commit in a scratch clone, so results are recorded and a rerun can
# complete them. The selector is the real one, copied in with the gate.
gate_clone() {
    local repo="$BATS_TEST_TMPDIR/clone"
    git init -q -b main "$repo"
    mkdir -p "$repo/scripts/lib" "$repo/.githooks/lib"
    cp "$REPO_ROOT/scripts/land-gate.sh" "$REPO_ROOT/scripts/gate_select.py" "$repo/scripts/"
    cp "$REPO_ROOT/scripts/lib/build_hosts.sh" "$REPO_ROOT/scripts/lib/sweep_location.sh" "$repo/scripts/lib/"
    cp "$REPO_ROOT/.githooks/lib/land-gate-stamp.sh" "$repo/.githooks/lib/"
    mkdir -p "$repo/src" && echo x >"$repo/src/a.cpp"
    printf 'build/\n' >"$repo/.gitignore"
    git -C "$repo" add -A && git -C "$repo" -c user.email=t@e -c user.name=t commit -qm base
    echo "$repo"
}

@test "a narrower rerun cannot replace a row a full run recorded, and no stamp comes of it" {
    local repo
    repo=$(gate_clone)
    export LAND_GATE_LOGDIR="$repo/build/land-gate"
    change src/a.cpp
    cd "$repo"
    HELIX_GATE_FULL=1 RC_unit_sweep=1 run ./scripts/land-gate.sh
    [ "$status" -ne 0 ]
    contains "HELIX_GATE_FULL=1 make land-gate STAGES=ci-shards" "$output"
    run ./scripts/land-gate.sh --rerun ci-shards
    [ "$status" -ne 0 ] || fail "a selected rerun replaced a full row: $output"
    contains "narrower than the full run" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$(git rev-parse HEAD)" ]
    HELIX_GATE_FULL=1 run ./scripts/land-gate.sh --rerun ci-shards
    [ "$status" -eq 0 ] || fail "$output"
}

@test "rows recorded under different plans never stamp the commit" {
    local repo sha
    repo=$(gate_clone)
    export LAND_GATE_LOGDIR="$repo/build/land-gate"
    change src/a.cpp
    cd "$repo"
    sha=$(git rev-parse HEAD)
    RC_unit_sweep=1 run ./scripts/land-gate.sh
    [ "$status" -ne 0 ]
    HELIX_GATE_FULL=1 run ./scripts/land-gate.sh --rerun ci-shards
    [ "$status" -ne 0 ] || fail "a merge of a full row into a selected run stamped: $output"
    contains "recorded under different plans" "$output"
    [ ! -e "$HELIX_LAND_GATE_STAMP_DIR/$sha" ]
}

# --------------------------------------------------------------------------
# Docs-only quality and python triggers
# --------------------------------------------------------------------------

@test "a docs-only change's quality sweep gets the changed paths, so every gate a .md wakes runs" {
    change docs/devel/BUILD_SYSTEM.md
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^qc-changed: .*changed.txt$" "$CALLS" || fail "$(cat "$CALLS")"
}

@test "HELIX_QC_CHANGED_FROM runs the gates whose trigger matches a path, and the untriggered ones" {
    local list="$BATS_TEST_TMPDIR/paths"
    echo docs/devel/BUILD_SYSTEM.md >"$list"
    run bash -c 'eval "$(sed -n "/^qc_trigger_re() {/,/^}/p; /^qc_wanted() {/,/^}/p" scripts/quality-checks.sh)"
        QC_TRIGGER_qc_code_lint="\.md$" QC_TRIGGER_qc_xml_tools="^ui_xml/"
        HELIX_QC_CHANGED_FROM="$1" STAGED_ONLY=false
        qc_wanted qc_code_lint && echo "code_lint runs"
        qc_wanted qc_xml_tools || echo "xml_tools skipped"
        qc_wanted qc_phase1 && echo "untriggered runs"' _ "$list"
    contains "code_lint runs" "$output"
    contains "xml_tools skipped" "$output"
    contains "untriggered runs" "$output"
    hash_with() {
        HELIX_QC_CHANGED_FROM="$1" bash -c \
            'eval "$(sed -n "/^qc_state_hash() {/,/^}/p" scripts/quality-checks.sh)"; qc_state_hash'
    }
    [ "$(hash_with "$list")" != "$(hash_with "")" ] || fail "a partial sweep shares the full sweep's stamp"
    echo ui_xml/home_panel.xml >"$BATS_TEST_TMPDIR/other"
    [ "$(hash_with "$list")" != "$(hash_with "$BATS_TEST_TMPDIR/other")" ] || fail "two partial sweeps share a stamp"
}

@test "a code change's quality sweep runs every gate and hands the path-scoped ones the change" {
    change src/a.cpp
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^qc-only: all$" "$CALLS" || fail "the sweep was narrowed: $(cat "$CALLS")"
    grep -q "^qc-scoped: .*changed.txt$" "$CALLS" || fail "no change handed over: $(cat "$CALLS")"
}

@test "a forced-full gate sweeps the path-scoped gates whole" {
    change src/a.cpp
    HELIX_GATE_FULL=1 run "$GATE"
    [ "$status" -eq 0 ]
    ! grep -q "^qc-scoped:" "$CALLS" || fail "a full gate scoped its gates: $(cat "$CALLS")"
}

@test "HELIX_QC_GATE_CHANGED runs a path-scoped gate only for its paths, and leaves the rest" {
    local list="$BATS_TEST_TMPDIR/paths"
    echo src/a.cpp >"$list"
    run bash -c 'eval "$(grep "^QC_GATE_PATH_SCOPED=" scripts/quality-checks.sh)"
        eval "$(sed -n "/^qc_trigger_re() {/,/^}/p; /^qc_wanted() {/,/^}/p" scripts/quality-checks.sh)"
        QC_TRIGGER_qc_link_parity="^mk/" QC_TRIGGER_qc_shellcheck="\.cpp$" QC_TRIGGER_qc_l081="^mk/"
        HELIX_QC_GATE_CHANGED="$1" STAGED_ONLY=false
        qc_wanted qc_link_parity || echo "link_parity skipped"
        qc_wanted qc_shellcheck && echo "shellcheck runs"
        qc_wanted qc_l081 && echo "l081 runs"' _ "$list"
    contains "link_parity skipped" "$output"
    contains "shellcheck runs" "$output"
    contains "l081 runs" "$output"
    hash_with() {
        HELIX_QC_GATE_CHANGED="$1" bash -c \
            'eval "$(sed -n "/^qc_state_hash() {/,/^}/p" scripts/quality-checks.sh)"; qc_state_hash'
    }
    [ "$(hash_with "$list")" != "$(hash_with "")" ] || fail "a scoped sweep shares the full sweep's stamp"
}

@test "a path-scoped gate's own script wakes it, so a change to the check is judged on the whole tree" {
    run bash -c 'for g in link_parity translation_files shellcheck; do
            . "scripts/qc/$g.sh"; re=$(eval echo "\$QC_TRIGGER_qc_$g")
            echo "scripts/qc/$g.sh" | grep -qE "$re" || echo "MISSING $g"
        done; echo done'
    [ "$status" -eq 0 ]
    lacks "MISSING" "$output"
}

@test "plan: python runs for the doc-screenshot manifest and for the workflows its tests read" {
    [[ "$(plan_of python docs/images/doc-screenshots.yml)" == run* ]] || fail "manifest"
    [[ "$(plan_of python .github/workflows/ci-watch.yml)" == run* ]] || fail "workflows"
    [[ "$(plan_of python docs/devel/TESTING.md)" == skip* ]]
}

@test "a rerun whose selection differs from the one its row ran refuses" {
    local repo
    repo=$(gate_clone)
    export LAND_GATE_LOGDIR="$repo/build/land-gate"
    change src/a.cpp
    cd "$repo"
    RC_unit_sweep=1 run ./scripts/land-gate.sh
    [ "$status" -ne 0 ]
    change src/b.cpp
    run ./scripts/land-gate.sh --rerun ci-shards
    [ "$status" -ne 0 ] || fail "a different selection replaced the row: $output"
    contains "narrower than the select run" "$output"
}

@test "a selected gate's ETA comes from selected gates only" {
    change src/ui/tour/tour_steps.cpp
    printf '%s 600 PASS local x y\n' 1 2 3 >"$LAND_GATE_HISTORY"
    run "$GATE"
    contains "no measured land-gate time yet for selected gates with local sweeps" "$output"
    printf '%s 90 PASS(sel) local x y\n' 1 2 3 >"$LAND_GATE_HISTORY"
    run "$GATE"
    contains "expect ~2m (median of the last 3)" "$output"
}

@test "cxx: a data file reaches the tests that include its loader's header, not an asset list's" {
    local root
    root=$(cxx_fixture)
    run cxx "$root" "~[.] ~[slow]" assets/config/db.json
    [ "$(cat "$root/out.txt")" = '"case e"' ] || fail "$output"
    contains "tests/unit/test_e.cpp: includes include/loader.h, whose src/loader.cpp loads assets/config/db.json" "$output"
}
