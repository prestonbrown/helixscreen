#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_todo_markers.py - the ratchet on work markers.
#
# The failure mode it exists for: quality-checks.sh printed the markers as
# informational output, cut the list at 20 lines, and matched bare XXX, so the
# list was never read whole, the count was free to climb, and several entries
# turned out to be user-facing controls that did nothing (#1373).
#
# Both halves are pinned. The catch half proves a marker in a comment turns the
# gate red. The quiet half matters as much: a checker that fired on the
# `mod_XXXXXX` placeholder strings or on an echo that names the words would be
# noise nobody could ratchet down.

GATE="scripts/check_todo_markers.py"

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIX="$BATS_TEST_TMPDIR/fix"
    mkdir -p "$FIX"
}

# Write $2 to a fixture named $1, echo its path.
fixture() {
    printf '%s\n' "$2" > "$FIX/$1"
    echo "$FIX/$1"
}

# The words are assembled so this file stays out of the count too.
T="TO""DO"
F="FIX""ME"

# --- catch ---

@test "a marker in a C++ line comment is flagged" {
    f=$(fixture probe.cpp "int x = 1; // $T: make this configurable")
    run python3 "$GATE" --list "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"probe.cpp:1:"* ]]
}

@test "the other marker word and a block comment are flagged too" {
    f=$(fixture probe.h "/* $F: this leaks */
 * $T later")
    run python3 "$GATE" --summary "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"TOTAL           2"* ]]
}

@test "a marker in a shell comment is flagged" {
    f=$(fixture probe.sh "#!/bin/sh
# $T(#986): still open
echo hi")
    run python3 "$GATE" --list "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"probe.sh:2:"* ]]
}

@test "an issue-cited marker still counts - the baseline is what says it is accounted for" {
    f=$(fixture probe.cpp "// $T(#1503): occupied cell rejects the drop")
    run python3 "$GATE" "$f"
    [ "$status" -eq 1 ]
}

# --- quiet ---

@test "a placeholder in code, not a comment, is not flagged" {
    f=$(fixture probe.cpp 'std::string id = "mod_XXXXXX";
const char* cls = "XXX--text";')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "the word inside an identifier or string is not a marker" {
    f=$(fixture probe.cpp "void undoTODOList(); // clears the undo list
const char* s = \"$T\";")
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "a shell echo that names the words is not a comment" {
    f=$(fixture probe.sh "#!/bin/sh
echo \"Checking for $T/$F markers\"")
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "a file type the gate does not scan is ignored" {
    f=$(fixture probe.py "# $T: python is not in the scan set")
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

# --- ratchet semantics ---

@test "--max-allowed passes at the baseline and fails above it" {
    f=$(fixture probe.cpp "// $T one
// $T two")
    run python3 "$GATE" --max-allowed 2 "$f"
    [ "$status" -eq 0 ]
    run python3 "$GATE" --max-allowed 1 "$f"
    [ "$status" -eq 1 ]
}

@test "coming in under the baseline passes and says to ratchet down" {
    f=$(fixture probe.cpp "int x; // nothing to see")
    run python3 "$GATE" --max-allowed 5 "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ratchet the baseline down"* ]]
}

# --- wiring ---
#
# The gate has to run from quality-checks.sh, not only from here: that is what
# the pre-commit hook, the pre-push hook and the Code Quality workflow call.

@test "gate is wired into quality-checks.sh" {
    run grep -q "check_todo_markers.py" scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "gate section is registered in the quality-checks section list" {
    run grep -q 'QC_ALL=.*qc_todo_markers' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "gate wakes on C++ and shell sources" {
    run bash -c "sed -n '/qc_todo_markers)/,/;;/p' scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
    contains "cpp" "$output"
    contains "sh" "$output"
}

@test "the quality-checks baseline matches the tree" {
    # A baseline that drifted above the real count silently stops ratcheting:
    # the gate would pass while new markers accumulate underneath it.
    baseline=$(grep -oE 'check_todo_markers.py --max-allowed [0-9]+' scripts/quality-checks.sh | grep -oE '[0-9]+')
    actual=$(python3 "$GATE" --summary | awk '/TOTAL/{print $2}')
    [ -n "$baseline" ]
    [ "$actual" -le "$baseline" ]
}

@test "adding a marker to the tree turns the wired gate red" {
    # The mutation the ratchet exists for: one more marker than the baseline.
    baseline=$(grep -oE 'check_todo_markers.py --max-allowed [0-9]+' scripts/quality-checks.sh | grep -oE '[0-9]+')
    actual=$(python3 "$GATE" --summary | awk '/TOTAL/{print $2}')
    [ "$actual" -eq "$baseline" ] || skip "tree is under the baseline; ratchet it down first"
    extra=$(fixture extra.cpp "// $T: one more")
    run python3 "$GATE" --max-allowed "$baseline" src include scripts "$extra"
    [ "$status" -eq 1 ]
}
