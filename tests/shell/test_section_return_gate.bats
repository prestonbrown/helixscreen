#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Catch2 discovers a SECTION only when a run of the test case passes its macro.
# A return inside a section ends the run first, so every later sibling SECTION
# is never discovered and never runs, and the case still passes.
#
# Fixture tests of scripts/check_section_returns.py; the verdict over tests/ is
# qc_test_lint's (lint_section_returns in scripts/code_lint.sh).

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
}

section_return_offenders() {
    python3 scripts/check_section_returns.py "$@"
}

@test "the gate flags a return that hides a later sibling section" {
    local f="${BATS_TEST_TMPDIR}/hides.cpp"
    cat > "$f" <<'SHAPES'
TEST_CASE("flat") {
    SECTION("refused") {
        CHECK(a);
        return;
    }
    setup_more();
    SECTION("accepted") { CHECK(b); }
}
TEST_CASE("nested") {
    SECTION("outer") {
        SECTION("inner last") {
            if (x) { return; }
        }
    }
    SECTION("outer sibling") {}
}
TEST_CASE("behind a block") {
    WHEN("first") { return; }
    if (y) {
        THEN("second") {}
    }
}
TEST_CASE("dynamic") {
    DYNAMIC_SECTION("d" << 1) {
        return;
    }
    DYNAMIC_SECTION("d" << 2) {}
}
SHAPES
    run section_return_offenders "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 4 ] || fail "expected 4 findings, got: $output"
    contains "hides.cpp:4:" "$output"
    contains "hides.cpp:12:" "$output"
    contains "hides.cpp:18:" "$output"
    contains "hides.cpp:25:" "$output"
}

@test "the gate stays quiet on returns that hide no section" {
    local f="${BATS_TEST_TMPDIR}/quiet.cpp"
    cat > "$f" <<'SHAPES'
TEST_CASE("last section") {
    SECTION("a") { CHECK(a); }
    SECTION("b") {
        CHECK(b);
        return;
    }
    CHECK(tail);
}
TEST_CASE("outside sections") {
    if (!supported()) {
        return;
    }
    SECTION("a") {}
    SECTION("b") {}
}
TEST_CASE("lambdas and local functions") {
    SECTION("a") {
        auto f = [&](int v) -> bool { return v > 0; };
        auto g = [] { return 1; };
        struct Local { int get() const { return 2; } };
        each(items, [&](const Item& i) mutable {
            if (i.skip) return;
            use(i);
        });
    }
    SECTION("b") {}
}
TEST_CASE("text, not code") {
    SECTION("a return; in a name") {
        // return; in a comment
        log("return;");
    }
    SECTION("b") {}
}
TEST_CASE("separate cases") {
    SECTION("only") { return; }
}
TEST_CASE("next case") {
    SECTION("unrelated") {}
}
TEST_CASE("reviewed") {
    SECTION("a") {
        // SECTION_RETURN_OK: test fixture
        return;
    }
    SECTION("b") {}
}
SHAPES
    run section_return_offenders "$f"
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "expected no findings, got: $output"
}

@test "lint_section_returns fails on an offender and names it" {
    local root="${BATS_TEST_TMPDIR}/root"
    mkdir -p "$root/tests/unit"
    cat > "$root/tests/unit/test_x.cpp" <<'SHAPES'
TEST_CASE("x") {
    SECTION("a") { return; }
    SECTION("b") {}
}
SHAPES
    git -C "$root" init -q
    # shellcheck source=scripts/code_lint.sh
    . scripts/code_lint.sh
    cd "$root" || return 1
    run lint_section_returns
    [ "$status" -eq 1 ]
    contains "tests/unit/test_x.cpp:2:" "$output"
}
