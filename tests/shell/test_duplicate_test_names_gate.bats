#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-test of scripts/check_duplicate_test_names.py: it fails on two
# TEST_CASEs sharing a name, and stays quiet on a commented-out twin and on
# the #if/#else branches of one test.

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    D="$BATS_TEST_TMPDIR/t"
    mkdir -p "$D/unit"
    printf 'TEST_CASE("one", "[a]") {}\n' > "$D/unit/a.cpp"
}

@test "two TEST_CASEs sharing a name fail, with both places named" {
    printf 'TEST_CASE_METHOD(Fx, "one",\n                 "[b]") {}\n' > "$D/unit/b.cpp"
    run python3 scripts/check_duplicate_test_names.py "$D"
    [ "$status" -eq 1 ] || fail "a duplicate must fail, got $status: $output"
    echo "$output" | grep -qF '"one"' || fail "did not name the duplicate: $output"
    echo "$output" | grep -qF "a.cpp:1" || fail "did not name a.cpp: $output"
    echo "$output" | grep -qF "b.cpp:1" || fail "did not name b.cpp: $output"
}

@test "a twin in a comment is not a test" {
    printf '// TEST_CASE("one", "[b]") {}\n/* TEST_CASE("one") */\nTEST_CASE("two") {}\n' > "$D/unit/b.cpp"
    run python3 scripts/check_duplicate_test_names.py "$D"
    [ "$status" -eq 0 ]
}

@test "the #if and #else branches of one test are one test" {
    printf '#ifdef __linux__\nTEST_CASE("three") {}\n#else\nTEST_CASE("three") {}\n#endif\n' > "$D/unit/c.cpp"
    run python3 scripts/check_duplicate_test_names.py "$D"
    [ "$status" -eq 0 ]
}

@test "a tree with no TEST_CASE fails rather than passing empty" {
    printf 'int x;\n' > "$D/unit/a.cpp"
    run python3 scripts/check_duplicate_test_names.py "$D"
    [ "$status" -eq 1 ] || fail "an empty scan must fail, got $status: $output"
    echo "$output" | grep -qF "no TEST_CASE found" || fail "did not say the scan was empty: $output"
}
