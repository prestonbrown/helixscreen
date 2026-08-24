#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for check_doc_refs.py --devel: the docs/devel dead-path and
# dead-link gate added for the architecture overhaul.

setup() {
    CHECK="$BATS_TEST_DIRNAME/../../scripts/check_doc_refs.py"
    FIX="$BATS_TEST_TMPDIR/devel"
    mkdir -p "$FIX"
}

@test "devel: clean doc with file ref, :line ref, and live link passes" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    # 50 lines so the doc's :42 cite lands inside the file (past-EOF check)
    for i in $(seq 50); do echo "line $i"; done > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/good.md" <<'EOF'
See `src/zz_fixture_real.cpp` and `src/zz_fixture_real.cpp:42` and
[the other doc](other.md). Glob `src/*.cpp` and `<placeholder>` are skipped.
EOF
    echo x > "$FIX/other.md"
    run python3 "$CHECK" --devel "$FIX/good.md"
    [ "$status" -eq 0 ]
}

@test "devel: dead file path fails with file:line report" {
    cat > "$FIX/bad.md" <<'EOF'
Cites `src/zz_fixture_missing_thing.cpp` which does not exist.
EOF
    run python3 "$CHECK" --devel "$FIX/bad.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_fixture_missing_thing.cpp"* ]]
}

@test "devel: dead relative markdown link fails" {
    echo x > "$FIX/target.md"
    cat > "$FIX/badlink.md" <<'EOF'
See [gone](nope.md) and [anchor ok](target.md#section).
EOF
    run python3 "$CHECK" --devel "$FIX/badlink.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"nope.md"* ]]
}

@test "devel: plans and printer-research subdirs are exempt" {
    mkdir -p "$FIX/plans"
    cat > "$FIX/plans/old.md" <<'EOF'
`src/zz_fixture_missing_thing.cpp`
EOF
    run python3 "$CHECK" --devel "$FIX"
    [ "$status" -eq 0 ]
}

@test "devel: C++ lambda with unnamed pointer param is not a link" {
    cat > "$FIX/lambda.md" <<'EOF'
```cpp
.on_destroy = [](lv_obj_t*) {
    g_api->log_debug("cleanup");
}
```
EOF
    run python3 "$CHECK" --devel "$FIX/lambda.md"
    [ "$status" -eq 0 ]
    [[ "$output" != *"lv_obj_t*"* ]]
}

@test "devel: cite past end of file fails" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    printf 'int foo(void);\n' > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/eof.md" <<'EOF'
See `src/zz_fixture_real.cpp:9999` for the declaration.
EOF
    run python3 "$CHECK" --devel "$FIX/eof.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"past the end of the file"* ]]
}

@test "devel: symbol-near-cite, symbol first form, drift fails" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    printf 'int other(void);\nint other2(void);\n' > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/cite_a.md" <<'EOF'
`foo()` (`src/zz_fixture_real.cpp:1`) does the thing.
EOF
    run python3 "$CHECK" --devel "$FIX/cite_a.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Symbol-cite drift"* ]]
    [[ "$output" == *"`foo()` cited at `src/zz_fixture_real.cpp:1`"* ]]
}

@test "devel: symbol-near-cite, cite first form, drift fails" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    printf 'int aaa(void);\n' > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/cite_b.md" <<'EOF'
`src/zz_fixture_real.cpp:1` — `bar` is the entry point.
EOF
    run python3 "$CHECK" --devel "$FIX/cite_b.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Symbol-cite drift"* ]]
    [[ "$output" == *"`bar` cited at `src/zz_fixture_real.cpp:1`"* ]]
}

@test "devel: symbol present near cited line passes, including window edges" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    { printf 'noise\nnoise\n'; printf 'int widget_attach(void);\n'; printf 'noise\n'; } \
        > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/cite_ok.md" <<'EOF'
`attach()` (`src/zz_fixture_real.cpp:3`) and
`src/zz_fixture_real.cpp:3` — `attach` both resolve: the symbol is on the
exact line. A bare `src/zz_fixture_real.cpp:1` cite makes no symbol claim,
so whatever sits on that line is none of the gate's business.
EOF
    run python3 "$CHECK" --devel "$FIX/cite_ok.md"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Symbol cites"* ]]
}

@test "devel: qualified symbol matches its final component" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    printf 'int Nav::shutdown(void);\n' > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/cite_qual.md" <<'EOF'
`NavigationManager::shutdown()` (`src/zz_fixture_real.cpp:1`) runs last.
EOF
    run python3 "$CHECK" --devel "$FIX/cite_qual.md"
    [ "$status" -eq 0 ]
}

@test "stale: report mode runs clean on a doc with no file cites" {
    cat > "$FIX/nocites.md" <<'EOF'
Plain prose, no backticked paths at all.
EOF
    run python3 "$CHECK" --stale --devel "$FIX/nocites.md"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No citation is older than its doc"* ]]
}
