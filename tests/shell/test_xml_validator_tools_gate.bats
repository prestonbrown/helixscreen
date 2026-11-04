#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the qc_xml_tools gate, scripts/qc/xml_tools.sh.
#
# The constraint: qc_xml_const and qc_xml_attr guard on `[ -x build/bin/... ]`
# and skip when the binary is missing, and no ordinary build produces those
# binaries — `make` builds only the app. A gate whose tool nothing builds is a
# pass verdict that examines nothing, so the wiring that produces the tools is
# pinned here: one serial step, ahead of the parallel gate batch (two makes in
# one tree race on the object dir), failing red when the build fails.

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

@test "tool-build gate is registered in QC_ALL and QC_SERIAL" {
    run grep -q 'QC_ALL=.*qc_xml_tools' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
    run bash -c "grep -q '^QC_SERIAL=\"qc_xml_tools ' scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
}

@test "tool-build gate builds the attribute validator" {
    run bash -c "sed -n '/^qc_xml_tools() {/,/^}/p' scripts/qc/xml_tools.sh"
    [ "$status" -eq 0 ] || fail "qc_xml_tools not extractable"
    contains "validate-xml-attrs" "$output"
}

# validate-xml-constants links the whole app; building it from the hook makes
# the CI quality step a cold full build that overruns its time limit. Its check
# runs in the unit suite instead (#1698).
@test "tool-build gate does not build the constants validator" {
    run bash -c "sed -n '/^qc_xml_tools() {/,/^}/p' scripts/qc/xml_tools.sh | grep '^if .*make'"
    [ "$status" -eq 0 ] || fail "qc_xml_tools make line not found"
    lacks "validate-xml-constants" "$output"
}

@test "tool-build gate wakes on the files the validators inspect" {
    run qc_trigger xml_tools
    [ -n "$output" ] || fail "qc_xml_tools sets no trigger"
    contains '\.xml$' "$output"
    contains '^tools/validate_xml' "$output"
}

# A commit runs the attribute validator on staged XML only, so a staged UI source
# would pay for a tool link and validate nothing.
@test "tool-build gate sleeps through a staged UI source" {
    run qc_trigger xml_tools
    local trigger="$output"
    run bash -c 'printf "%s\n" src/ui/home/ui_panel_home.cpp | grep -qE "$1"' _ "$trigger"
    [ "$status" -eq 1 ] || fail "a staged .cpp woke the tool build: $trigger"
    run bash -c 'printf "%s\n" ui_xml/home_panel.xml | grep -qE "$1"' _ "$trigger"
    [ "$status" -eq 0 ] || fail "staged XML did not wake the tool build: $trigger"
}

# qc_xml_const runs no binary, so it must name where the constants check is
# enforced, and that test must exist, or the hook points at nothing.
@test "constants gate names the unit test that enforces it" {
    run bash -c "sed -n '/^qc_xml_const() {/,/^}/p' scripts/qc/xml_const.sh"
    [ "$status" -eq 0 ] || fail "qc_xml_const not extractable"
    contains 'ui_xml has no incomplete constant sets' "$output"
    lacks 'not enforced' "$output"
    run grep -q 'ui_xml has no incomplete constant sets' tests/unit/test_ui_theme_constants.cpp
    [ "$status" -eq 0 ] || fail "the named unit test does not exist"
}
