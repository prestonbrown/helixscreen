#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A Catch2 assertion between fork() and the child's branch runs in BOTH
# processes, and one inside the child block runs in the child. Catch2's XML
# writer flushes after every element and the child shares the report's file
# description, so under --success (which reports passing assertions) the
# child's copy lands in the parent's report mid-element and the report stops
# parsing. Check the fork result after the child block.
#
# Fixture tests of fork_assertion_offenders (scripts/code_lint.sh); the verdict
# over tests/ is qc_test_lint's (lint_fork_assertions).

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    # shellcheck source=scripts/code_lint.sh
    . scripts/code_lint.sh
}

@test "the gate flags assertions a forked child would evaluate" {
    local f="${BATS_TEST_TMPDIR}/fork_shapes.cpp"
    cat > "$f" <<'SHAPES'
TEST_CASE("a") {
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        _exit(0);
    }
}
TEST_CASE("b") {
    const pid_t child = fork();
    CHECK_FALSE(child < 0);

    if (child == 0) {
        if (setsid() < 0) {
            _exit(98);
        }
        INFO("in the child");
        _exit(0);
    }
    REQUIRE(child > 0);
}
SHAPES
    run fork_assertion_offenders "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 3 ] || fail "expected 3 findings, got: $output"
    contains 'INFO("in the child")' "$output"
    contains "REQUIRE(pid >= 0)" "$output"
    contains "CHECK_FALSE(child < 0)" "$output"
}

@test "the gate stays quiet on assertions after the child block" {
    local f="${BATS_TEST_TMPDIR}/fork_ok.cpp"
    cat > "$f" <<'SHAPES'
TEST_CASE("a") {
    pid_t pid = fork();
    if (pid == 0) {
        if (setsid() < 0) {
            _exit(98);
        }
        _exit(0);
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
}
SHAPES
    run fork_assertion_offenders "$f"
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "unexpected findings: $output"
}

@test "the lint reads every fork() under tests/ and fails closed when it finds none" {
    local root="$BATS_TEST_TMPDIR/root"
    mkdir -p "$root/tests/unit"
    cd "$root" || return 1
    run lint_fork_assertions
    [ "$status" -eq 1 ]
    contains "found no fork()" "$output"

    printf 'TEST_CASE("a") {\n    pid_t pid = fork();\n    REQUIRE(pid >= 0);\n    if (pid == 0) {\n        _exit(0);\n    }\n}\n' \
        > tests/unit/test_forks.cpp
    run lint_fork_assertions
    [ "$status" -eq 1 ]
    contains "tests/unit/test_forks.cpp:3:" "$output"
}
