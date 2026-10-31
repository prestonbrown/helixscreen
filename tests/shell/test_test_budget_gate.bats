#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_test_budget.py: a branch may not grow the C++
# test suite, TestAccess use or friend declarations past its fork point, and
# only test lines take a Test-budget grant. Each measure is proven to fail on
# growth, to pass when the branch pays for what it adds, and to ignore what it
# does not measure.

load helpers

GATE="$BATS_TEST_DIRNAME/../../scripts/check_test_budget.py"

setup() {
    REPO="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    mkdir -p "$REPO/tests/unit" "$REPO/src" "$REPO/include" "$REPO/tests/shell"
    git -C "$REPO" init -q -b main
    git -C "$REPO" config user.email t@example.com
    git -C "$REPO" config user.name t
    printf 'TEST_CASE("a") {\n    CHECK(1 == 1);\n}\n' > "$REPO/tests/unit/test_a.cpp"
    printf 'class Widget {\n    friend class WidgetTestAccess;\n};\n' > "$REPO/include/widget.h"
    printf 'int f() { return 1; }\n' > "$REPO/src/widget.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm base
    BASE=$(git -C "$REPO" rev-parse HEAD)
    git -C "$REPO" checkout -qb feature
}

commit_all() {
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "$1"
}

budget() {
    run python3 "$GATE" --root "$REPO" --base "$BASE"
}

@test "a branch that adds test lines fails" {
    printf 'TEST_CASE("b") {\n    CHECK(2 == 2);\n}\n' > "$REPO/tests/unit/test_b.cpp"
    commit_all "add a test"
    budget
    [ "$status" -eq 1 ]
    contains "test lines grew by 3" "$output"
}

@test "a branch that pays for its new test with a cut passes" {
    printf 'TEST_CASE("b") {\n    CHECK(2 == 2);\n}\n' > "$REPO/tests/unit/test_b.cpp"
    rm "$REPO/tests/unit/test_a.cpp"
    commit_all "replace a test"
    budget
    [ "$status" -eq 0 ]
    contains "test lines +0" "$output"
}

@test "a Test-budget line in a branch commit grants that many lines" {
    printf 'TEST_CASE("b") {\n    CHECK(2 == 2);\n}\n' > "$REPO/tests/unit/test_b.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "add a test" -m "Test-budget: +3 the new subsystem has no tests to trade"
    budget
    [ "$status" -eq 0 ]
    contains "budget 3" "$output"
}

@test "a grant smaller than the growth still fails, and a grant without a reason grants nothing" {
    printf 'TEST_CASE("b") {\n    CHECK(2 == 2);\n}\n' > "$REPO/tests/unit/test_b.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "add a test" -m "Test-budget: +2 almost enough"
    budget
    [ "$status" -eq 1 ]
    contains "2 granted by Test-budget" "$output"
    git -C "$REPO" commit -q --amend -m "add a test" -m "Test-budget: +3"
    budget
    [ "$status" -eq 1 ]
}

@test "a new TestAccess reference fails even with a Test-budget grant" {
    printf 'void g() { WidgetTestAccess::poke(w); }\n' >> "$REPO/src/widget.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "reach in" -m "Test-budget: +100 unrelated"
    budget
    [ "$status" -eq 1 ]
    contains "TestAccess references grew by 1" "$output"
}

@test "a new friend declaration fails" {
    printf 'class Gadget {\n    friend struct GadgetPeek;\n};\n' > "$REPO/include/gadget.h"
    commit_all "befriend"
    budget
    [ "$status" -eq 1 ]
    contains "friend declarations grew by 1" "$output"
}

@test "moving a friend between headers nets zero" {
    printf 'class Widget {\n};\n' > "$REPO/include/widget.h"
    printf 'class Moved {\n    friend class WidgetTestAccess;\n};\n' > "$REPO/include/moved.h"
    commit_all "move it"
    budget
    [ "$status" -eq 0 ]
    contains "friend +0" "$output"
}

@test "shell tests and a branch with no commits are not measured" {
    printf '@test "x" {\n  true\n}\n' > "$REPO/tests/shell/test_x.bats"
    commit_all "a shell test"
    budget
    [ "$status" -eq 0 ]
    contains "test lines +0" "$output"
    run python3 "$GATE" --root "$REPO" --base HEAD
    [ "$status" -eq 0 ]
    contains "no committed work" "$output"
}

@test "with no upstream to fork from, the auto base refuses instead of passing" {
    printf 'TEST_CASE("b") {\n    CHECK(2 == 2);\n}\n' > "$REPO/tests/unit/test_b.cpp"
    commit_all "add a test"
    git -C "$REPO" branch -qD main
    run python3 "$GATE" --root "$REPO" --base auto
    [ "$status" -eq 1 ]
    contains "no fork point to measure from" "$output"
}

qc_budget() {
    run bash -c '
        STAGED_ONLY=false; QC_TMP="$(mktemp -d)"; QC_COUNTS="$QC_TMP/counts"
        . "$1/scripts/qc/_lib.sh"
        . "$1/scripts/qc/test_budget.sh"
        cd "$2" && mkdir -p scripts && ln -sf "$1/scripts/check_test_budget.py" scripts/
        qc_test_budget' _ "$BATS_TEST_DIRNAME/../.." "$REPO"
}

@test "the qc wrapper measures the branch from its fork point and fails on growth" {
    printf 'TEST_CASE("b") {\n    CHECK(2 == 2);\n}\n' > "$REPO/tests/unit/test_b.cpp"
    commit_all "add a test"
    qc_budget
    [ "$status" -eq 1 ]
    contains "test lines grew by 3" "$output"
}

@test "the qc wrapper passes a branch that pays for what it adds" {
    printf 'TEST_CASE("b") {\n    CHECK(2 == 2);\n}\n' > "$REPO/tests/unit/test_b.cpp"
    rm "$REPO/tests/unit/test_a.cpp"
    commit_all "replace a test"
    qc_budget
    [ "$status" -eq 0 ]
    contains "Test budget" "$output"
}

@test "the qc wrapper triggers on C++ under tests, src and include and on its script" {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    re="$(qc_trigger test_budget)"
    [ -n "$re" ]
    for p in tests/unit/test_x.cpp src/ui/ui_x.cpp include/x.h scripts/check_test_budget.py; do
        echo "$p" | grep -qE "$re" || fail "$p should trigger"
    done
    for p in docs/devel/README.md tests/shell/test_x.bats ui_xml/globals.xml; do
        if echo "$p" | grep -qE "$re"; then fail "$p should not trigger"; fi
    done
}
