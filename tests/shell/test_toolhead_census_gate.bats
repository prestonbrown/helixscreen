#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_toolhead_census.py, the commit-time form of the
# [job_holds_machine] census in tests/unit/test_job_holds_machine.cpp.
#
# The catch half: a new control-bearing XML file outside both tables fails with
# the test's own instruction, and so do a stale row and a drifted guard count.
# The quiet half: a file with no control, a classified file, and an unstaged
# working-tree file (another session's WIP) say nothing. The checker reads its
# tables and markers from the real test file, so the first test also proves it
# still parses the shipped one.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"
GATE="$REPO_ROOT/scripts/check_toolhead_census.py"

# A miniature repo whose census test has the shipped file's shape.
setup() {
    FIX="$BATS_TEST_TMPDIR/census"
    mkdir -p "$FIX/ui_xml/components" "$FIX/tests/unit"
    cd "$FIX" || return 1
    git init -q .
    git config user.email t@example.com
    git config user.name tester
    cat > tests/unit/test_job_holds_machine.cpp <<'CPP'
constexpr const char* kSubject = "job_holds_machine";
constexpr const char* kDisabled = "state=\"disabled\"";
constexpr const char* kGuardAttribute = "moves_machine=\"true\"";
constexpr const char* kGuardAttributeName = "moves_machine=\"";
constexpr const char* kSocketGuard = "moves_machine=\"$";
constexpr GuardedFile kGuardedFiles[] = {
    {"ui_xml/motion.xml", 1},
};
constexpr const char* kNoMachineControlFiles[] = {
    "ui_xml/components/about.xml",
};
bool declares_a_control(const std::string& xml) {
    for (const char* marker :
         {"callback=\"", "clickable=\"true\"", "<lv_button"}) {
        if (xml.find(marker) != std::string::npos) {
            REQUIRE(bind.find("print_active") == std::string::npos);
CPP
    printf '<view><lv_button moves_machine="true"/></view>\n' > ui_xml/motion.xml
    printf '<view><lv_obj clickable="true"/></view>\n' > ui_xml/components/about.xml
    printf '<view><lv_label text="hi"/></view>\n' > ui_xml/plain.xml
    git add -A
    git commit -qm base --no-verify
}

run_staged() {
    run python3 "$GATE" --staged-only
}

@test "a classified tree passes" {
    run_staged
    [ "$status" -eq 0 ]
    contains "Toolhead-safety census" "$output"
}

@test "a new XML file declaring an event_cb in neither table fails with the test's instruction" {
    printf '<view><lv_obj><event_cb callback="on_tap"/></lv_obj></view>\n' > ui_xml/new_panel.xml
    git add ui_xml/new_panel.xml
    run_staged
    [ "$status" -eq 1 ]
    contains "ui_xml/new_panel.xml" "$output"
    contains "Add each to kGuardedFiles" "$output"
    contains "kNoMachineControlFiles" "$output"
}

@test "an existing file gaining its first control fails" {
    printf '<view><lv_label text="hi"/><lv_button/></view>\n' > ui_xml/plain.xml
    git add ui_xml/plain.xml
    run_staged
    [ "$status" -eq 1 ]
    contains "ui_xml/plain.xml" "$output"
}

@test "a row naming a file that no longer declares a control fails" {
    printf '<view/>\n' > ui_xml/components/about.xml
    git add ui_xml/components/about.xml
    run_staged
    [ "$status" -eq 1 ]
    contains "no longer declares a control" "$output"
    contains "ui_xml/components/about.xml" "$output"
}

@test "a guarded file whose guard count drifts from its row fails" {
    printf '<view><lv_button moves_machine="true"/><lv_button moves_machine="true"/></view>\n' \
        > ui_xml/motion.xml
    git add ui_xml/motion.xml
    run_staged
    [ "$status" -eq 1 ]
    contains "pins 1 toolhead guards and carries 2" "$output"
}

@test "a file with no control needs no row" {
    printf '<view><lv_label text="other"/></view>\n' > ui_xml/another_plain.xml
    git add ui_xml/another_plain.xml
    run_staged
    [ "$status" -eq 0 ]
}

@test "--staged-only ignores an unclassified file that is not staged" {
    printf '<view><lv_button/></view>\n' > ui_xml/peer_wip.xml
    run_staged
    [ "$status" -eq 0 ]
    lacks "peer_wip" "$output"
}

@test "a missing census test fails in both modes instead of passing" {
    git rm -q tests/unit/test_job_holds_machine.cpp
    run_staged
    [ "$status" -eq 1 ]
    contains "test_job_holds_machine.cpp is not in the index" "$output"
    run python3 "$GATE"
    [ "$status" -eq 1 ]
    contains "test_job_holds_machine.cpp is not in" "$output"
}

@test "an XML file naming the job_holds_machine subject fails" {
    printf '<view><lv_label text="x"><bind_flag_if_eq subject="job_holds_machine"/></lv_label></view>\n' \
        > ui_xml/plain.xml
    git add ui_xml/plain.xml
    run_staged
    [ "$status" -eq 1 ]
    contains "ui_xml/plain.xml names job_holds_machine 1 times" "$output"
}

@test "a bind_state that disables a control on print_active fails; another subject passes" {
    printf '<view><lv_button moves_machine="true"><bind_state_if_eq subject="print_active" state="disabled" ref_value="1"/></lv_button></view>\n' \
        > ui_xml/motion.xml
    git add ui_xml/motion.xml
    run_staged
    [ "$status" -eq 1 ]
    contains "ui_xml/motion.xml disables a control on print_active" "$output"

    printf '<view><lv_button moves_machine="true"><bind_state_if_eq subject="homed" state="disabled" ref_value="0"/></lv_button></view>\n' \
        > ui_xml/motion.xml
    git add ui_xml/motion.xml
    run_staged
    [ "$status" -eq 0 ]
}

@test "the gate is triggered by ui_xml XML and stays off for anything else" {
    cd "$REPO_ROOT" || return 1
    re="$(qc_trigger toolhead_census)"
    [ -n "$re" ]
    for p in ui_xml/new_panel.xml ui_xml/components/row.xml tests/unit/test_job_holds_machine.cpp; do
        echo "$p" | grep -qE "$re" || fail "$p should trigger"
    done
    for p in src/ui/ui_panel_motion.cpp docs/devel/README.md ui_xml/notes.txt; do
        if echo "$p" | grep -qE "$re"; then fail "$p should not trigger"; fi
    done
}
