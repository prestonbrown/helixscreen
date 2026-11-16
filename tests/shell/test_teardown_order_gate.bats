#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pins the order of the key calls in PrinterSession::teardown_printer_scope(), the one
# ordered teardown behind both a printer switch and process exit, and of the exit tail
# Application::shutdown() runs after it.
#
# The order is the contract: observers detach before subjects deinit, the client outlives
# everything that unregisters from it, panels die before the subjects they observe, the
# display dies before the theme subjects its XML scopes observe. No unit test can run it
# (it ends in a full rebuild of global state), so this gate compares the call sequences to
# golden lists. A deliberate reorder edits the list in the same commit.
#
# Fixture tests of lint_teardown_call_order (scripts/code_lint.sh), on mutated
# copies of the two sources; the verdict over the real tree is qc_code_lint's.

load helpers

TEARDOWN_SRC="src/application/printer_session.cpp"
TEARDOWN_FN="PrinterSession::teardown_printer_scope"
TEARDOWN_GOLDEN="tests/shell/fixtures/teardown_printer_scope_order.txt"

TAIL_SRC="src/application/application.cpp"
TAIL_FN="Application::shutdown"
TAIL_GOLDEN="tests/shell/fixtures/shutdown_exit_tail_order.txt"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    # shellcheck source=scripts/code_lint.sh
    . scripts/code_lint.sh
}

@test "the teardown order gate fails when two calls swap" {
    local mutated="${BATS_TEST_TMPDIR}/swapped.cpp"
    # Move the panel reset after the subject reset.
    awk '
        /^    m_panels\.reset\(\);$/ { held = $0; next }
        held != "" && /^    m_subjects\.reset\(\);$/ { print; print held; held = ""; next }
        { print }
    ' "$TEARDOWN_SRC" > "$mutated"
    ! cmp -s "$mutated" "$TEARDOWN_SRC"

    run teardown_call_order "$mutated" "$TEARDOWN_FN" "$TEARDOWN_GOLDEN"
    [ "$status" -eq 0 ]
    ! diff <(printf '%s\n' "$output") "$TEARDOWN_GOLDEN" > /dev/null
}

@test "the exit tail gate fails when the display outlives the theme teardown" {
    local mutated="${BATS_TEST_TMPDIR}/tail_swapped.cpp"
    awk '
        /^    m_display\.reset\(\);$/ { held = $0; next }
        held != "" && /^    theme_manager_deinit\(\);$/ { print; print held; held = ""; next }
        { print }
    ' "$TAIL_SRC" > "$mutated"
    ! cmp -s "$mutated" "$TAIL_SRC"

    run teardown_call_order "$mutated" "$TAIL_FN" "$TAIL_GOLDEN"
    [ "$status" -eq 0 ]
    ! diff <(printf '%s\n' "$output") "$TAIL_GOLDEN" > /dev/null
}

@test "the teardown order gate fails when a function cannot be located" {
    local mutated="${BATS_TEST_TMPDIR}/renamed.cpp"
    sed -e 's@::teardown_printer_scope(@::teardown_renamed(@' "$TEARDOWN_SRC" > "$mutated"

    run teardown_call_order "$mutated" "$TEARDOWN_FN" "$TEARDOWN_GOLDEN"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not locate"* ]]
}

@test "the lint names the function whose order drifted and passes the one that held" {
    local root="$BATS_TEST_TMPDIR/root"
    mkdir -p "$root/src/application" "$root/tests/shell/fixtures"
    cp "$TEARDOWN_GOLDEN" "$TAIL_GOLDEN" "$root/tests/shell/fixtures/"
    cp "$TAIL_SRC" "$root/$TAIL_SRC"
    awk '
        /^    m_panels\.reset\(\);$/ { held = $0; next }
        held != "" && /^    m_subjects\.reset\(\);$/ { print; print held; held = ""; next }
        { print }
    ' "$TEARDOWN_SRC" > "$root/$TEARDOWN_SRC"
    cd "$root" || return 1

    run lint_teardown_call_order
    [ "$status" -eq 1 ]
    contains "PrinterSession::teardown_printer_scope() in $TEARDOWN_SRC" "$output"
    lacks "Application::shutdown()" "$output"
}
