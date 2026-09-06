#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_vacuous_tests.py — the gate that reads a Catch2
# XML report and reports assertions that cannot fail.
#
# The gate is deliberately runtime-based. A static pass over assertion SYNTAX
# cannot separate a real test from a vacuous one: on this tree 2334 of 12758
# cases look "all-weak" by shape and the worst offenders are all good tests,
# because REQUIRE(is_valid(p)) and REQUIRE(ptr != nullptr) are the same shape.
#
# These tests pin both halves. The catch half is a case that asserted nothing
# and an assertion whose expansion is identical to its source. The quiet half
# is the larger risk and the reason the gate is narrow: REQUIRE_NOTHROW never
# decomposes its argument and so ALWAYS looks like a tautology, a skipped case
# asserts nothing legitimately, and a table-driven test that asserts inside a
# helper records its assertions against the calling case. Any of those firing
# would make the gate noise on every run and it would be switched off.

load helpers

GATE="scripts/check_vacuous_tests.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}"
}

report() {  # $1 = filename, stdin = TestCase elements
    { echo '<?xml version="1.0" encoding="UTF-8"?>'
      echo '<Catch2TestRun name="helix-tests">'
      cat
      echo '</Catch2TestRun>'; } > "$WORK/$1"
}

expr_elem() {  # $1 type, $2 original, $3 expanded
    echo "    <Expression success=\"true\" type=\"$1\" filename=\"tests/unit/t.cpp\" line=\"7\">"
    echo "      <Original>$2</Original><Expanded>$3</Expanded>"
    echo "    </Expression>"
}

# ---------------------------------------------------------------- catch half

@test "flags a test case that executed zero assertions" {
    report a.xml <<EOF
  <TestCase name="asserts nothing" filename="tests/unit/t.cpp" line="3">
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/a.xml" --list
    [ "$status" -eq 0 ]
    contains "no-assertion" "$output"
    [[ "$output" == *"asserts nothing"* ]]
}

@test "flags an assertion whose expansion is identical to its source" {
    report b.xml <<EOF
  <TestCase name="tautology" filename="tests/unit/t.cpp" line="3">
$(expr_elem REQUIRE "5 == 5" "5 == 5")
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/b.xml" --list
    [[ "$output" == *"literal-tautology"* ]]
}

@test "REQUIRE(true) is reported" {
    report c.xml <<EOF
  <TestCase name="always" filename="tests/unit/t.cpp" line="3">
$(expr_elem REQUIRE "true" "true")
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/c.xml" --list
    [[ "$output" == *"literal-tautology"* ]]
}

@test "--max-allowed makes the gate fail" {
    report d.xml <<EOF
  <TestCase name="asserts nothing" filename="tests/unit/t.cpp" line="3">
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/d.xml" --summary --max-allowed 0
    [ "$status" -eq 1 ]
    run python3 "$GATE" "$WORK/d.xml" --summary --max-allowed 1
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------- quiet half

@test "REQUIRE_NOTHROW is NOT a tautology even though it never expands" {
    report e.xml <<EOF
  <TestCase name="nothrow" filename="tests/unit/t.cpp" line="3">
$(expr_elem REQUIRE_NOTHROW "loader.is_available()" "loader.is_available()")
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/e.xml" --list
    lacks "literal-tautology" "$output"
    [[ "$output" != *"no-assertion"* ]]
}

@test "the _THROWS and _THAT families are also left alone" {
    report f.xml <<EOF
  <TestCase name="throws" filename="tests/unit/t.cpp" line="3">
$(expr_elem REQUIRE_THROWS_AS "parse(bad)" "parse(bad)")
$(expr_elem REQUIRE_THAT "name" "name")
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/f.xml" --list
    [[ "$output" != *"literal-tautology"* ]]
}

@test "a real assertion whose value differs from its source is quiet" {
    report g.xml <<EOF
  <TestCase name="real" filename="tests/unit/t.cpp" line="3">
$(expr_elem REQUIRE "slot.tool_id == 3" "3 == 3")
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/g.xml" --list
    [[ "$output" != *"literal-tautology"* ]]
}

@test "a SKIPped case asserts nothing legitimately and is not flagged" {
    report h.xml <<EOF
  <TestCase name="skipped" filename="tests/unit/t.cpp" line="3">
    <Skip filename="tests/unit/t.cpp" line="4">no hardware</Skip>
    <OverallResult success="true" skips="1"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/h.xml" --list
    [[ "$output" != *"no-assertion"* ]]
}

@test "a table-driven case asserting via a helper is not flagged" {
    # The assertions are recorded against the CALLING case, which is exactly why
    # this gate reads a run instead of the source.
    report i.xml <<EOF
  <TestCase name="corpus via helper" filename="tests/unit/t.cpp" line="3">
$(expr_elem REQUIRE "phase == \"load\"" "\"load\" == \"load\"")
$(expr_elem REQUIRE "phase == \"unload\"" "\"unload\" == \"unload\"")
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    run python3 "$GATE" "$WORK/i.xml" --list
    [[ "$output" != *"no-assertion"* ]]
}

@test "the baseline file exempts a named case" {
    report j.xml <<EOF
  <TestCase name="BusThread starts and stops cleanly" filename="tests/unit/t.cpp" line="3">
    <OverallResult success="true" skips="0"/>
  </TestCase>
EOF
    echo "BusThread starts and stops cleanly  # asserted by TSAN, not Catch2" > "$WORK/base.txt"
    run python3 "$GATE" "$WORK/j.xml" --baseline "$WORK/base.txt" --list
    [[ "$output" != *"no-assertion"* ]]
}

@test "a report polluted by test stdout is still parsed" {
    # Tests that shell out print ANSI status into the stream. The report stays
    # complete; the bytes are simply not legal XML.
    printf '<?xml version="1.0" encoding="UTF-8"?>\n<Catch2TestRun name="t">\n' > "$WORK/k.xml"
    printf '  <TestCase name="polluted" filename="tests/unit/t.cpp" line="3">\n' >> "$WORK/k.xml"
    printf '\033[0;32m[INFO]\033[0m noise from a subprocess\n' >> "$WORK/k.xml"
    printf '    <OverallResult success="true" skips="0"/>\n  </TestCase>\n</Catch2TestRun>\n' >> "$WORK/k.xml"
    run python3 "$GATE" "$WORK/k.xml" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"no-assertion"* ]]
}

@test "a truncated report is an error, not a clean bill of health" {
    printf '<?xml version="1.0"?>\n<Catch2TestRun name="t">\n  <TestCase name="x">\n' > "$WORK/l.xml"
    run python3 "$GATE" "$WORK/l.xml" --list
    [ "$status" -eq 2 ]
}
