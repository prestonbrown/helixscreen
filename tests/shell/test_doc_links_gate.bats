#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/gen_doc_links.py — the generator that turns the
# architecture guide's backticked citations into links — and for the unwrap
# that keeps check_doc_refs.py's drift checks working through those links.
#
# The generator runs against the current working directory, so each test builds
# a miniature repo in BATS_TEST_TMPDIR and cds into it.

load helpers

setup() {
    GEN="$BATS_TEST_DIRNAME/../../scripts/gen_doc_links.py"
    CHECK="$BATS_TEST_DIRNAME/../../scripts/check_doc_refs.py"
    REPO="$BATS_TEST_TMPDIR/repo"
    DOC="$REPO/docs/devel/architecture/ch.md"
    mkdir -p "$REPO/docs/devel/architecture" "$REPO/src/printer" "$REPO/include"
    for i in $(seq 60); do echo "line $i"; done > "$REPO/src/printer/zz_state.cpp"
    echo "int x;" > "$REPO/include/zz_state.h"
}

@test "generator: a path citation and a :line citation both become links" {
    cat > "$DOC" <<'EOF'
See `src/printer/zz_state.cpp` and `src/printer/zz_state.cpp:42`.
EOF
    cd "$REPO"
    run python3 "$GEN" docs/devel/architecture
    [ "$status" -eq 0 ]
    grep -q '\[`src/printer/zz_state.cpp`\](../../../src/printer/zz_state.cpp)' "$DOC"
    grep -q '\[`src/printer/zz_state.cpp:42`\](../../../src/printer/zz_state.cpp#L42)' "$DOC"
}

@test "generator: a bare basename resolves to its one match" {
    echo 'The header `zz_state.h` declares it.' > "$DOC"
    cd "$REPO"
    run python3 "$GEN" docs/devel/architecture
    [ "$status" -eq 0 ]
    grep -q '\[`zz_state.h`\](../../../include/zz_state.h)' "$DOC"
}

@test "generator: an ambiguous basename stays a plain code span" {
    mkdir -p "$REPO/src/ui"
    echo "int y;" > "$REPO/src/printer/zz_dup.h"   # two matches in the same
    echo "int z;" > "$REPO/src/ui/zz_dup.h"        # root — no tie-break exists
    echo 'The header `zz_dup.h` declares it.' > "$DOC"
    cd "$REPO"
    run python3 "$GEN" docs/devel/architecture
    [ "$status" -eq 0 ]
    refute_grep '](' "$DOC"
}

@test "generator: submodule paths under lib/ are never linked" {
    mkdir -p "$REPO/lib/lvgl/src"
    echo "void f(void);" > "$REPO/lib/lvgl/src/zz_observer.c"
    echo 'See `lib/lvgl/src/zz_observer.c` for the walk.' > "$DOC"
    cd "$REPO"
    run python3 "$GEN" docs/devel/architecture
    [ "$status" -eq 0 ]
    refute_grep '](' "$DOC"
}

@test "generator: citations inside a fenced code block are left alone" {
    cat > "$DOC" <<'EOF'
Prose cites `src/printer/zz_state.cpp`.

```bash
grep foo `src/printer/zz_state.cpp`
```
EOF
    cd "$REPO"
    run python3 "$GEN" docs/devel/architecture
    [ "$status" -eq 0 ]
    [ "$(grep -c '](' "$DOC")" -eq 1 ]
    grep -q 'grep foo `src/printer/zz_state.cpp`$' "$DOC"
}

@test "generator: a citation shown literally in a ``double-backtick`` span is left alone" {
    cat > "$DOC" <<'EOF'
Write `` `src/printer/zz_state.cpp:42` ``, never the link yourself.
EOF
    cd "$REPO"
    run python3 "$GEN" docs/devel/architecture
    [ "$status" -eq 0 ]
    refute_grep '](' "$DOC"
}

@test "generator: rewriting is idempotent" {
    echo 'See `src/printer/zz_state.cpp:42`.' > "$DOC"
    cd "$REPO"
    python3 "$GEN" docs/devel/architecture
    cp "$DOC" "$BATS_TEST_TMPDIR/once.md"
    python3 "$GEN" docs/devel/architecture
    diff "$BATS_TEST_TMPDIR/once.md" "$DOC"
}

@test "generator: a hand-edited URL is repaired from the citation text" {
    printf 'See [`src/printer/zz_state.cpp:42`](../../../src/WRONG.cpp#L999).\n' > "$DOC"
    cd "$REPO"
    run python3 "$GEN" docs/devel/architecture
    [ "$status" -eq 0 ]
    grep -q '(../../../src/printer/zz_state.cpp#L42)' "$DOC"
    refute_grep 'WRONG' "$DOC"
}

@test "generator: --check fails on a stale doc and passes once regenerated" {
    echo 'See `src/printer/zz_state.cpp`.' > "$DOC"
    cd "$REPO"
    run python3 "$GEN" --check docs/devel/architecture
    [ "$status" -eq 1 ]
    [[ "$output" == *"regen-doc-links"* ]]
    python3 "$GEN" docs/devel/architecture
    run python3 "$GEN" --check docs/devel/architecture
    [ "$status" -eq 0 ]
}

@test "gate: a past-EOF cite still fails when the cite is wrapped in a link" {
    mkdir -p "$BATS_TEST_TMPDIR/devel" "$BATS_TEST_TMPDIR/src"
    printf 'int f(void);\n' > "$BATS_TEST_TMPDIR/src/zz_eof.cpp"
    printf 'Boom at [`src/zz_eof.cpp:900`](../src/zz_eof.cpp#L900).\n' \
        > "$BATS_TEST_TMPDIR/devel/eof.md"
    cd "$BATS_TEST_TMPDIR"
    run python3 "$CHECK" --devel devel/eof.md
    [ "$status" -eq 1 ]
    [[ "$output" == *"past the end"* ]]
    [[ "$output" == *"zz_eof.cpp:900"* ]]
}

@test "gate: symbol-cite drift is still caught through a link wrapper" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    { echo "void zz_target(void) {}"; for i in $(seq 60); do echo "// filler $i"; done; } \
        > "$BATS_TEST_TMPDIR/src/zz_drift.cpp"
    mkdir -p "$BATS_TEST_TMPDIR/devel"
    printf '`zz_target()` ([`src/zz_drift.cpp:55`](../src/zz_drift.cpp#L55)) does the work.\n' \
        > "$BATS_TEST_TMPDIR/devel/drift.md"
    cd "$BATS_TEST_TMPDIR"
    run python3 "$CHECK" --devel devel/drift.md
    [ "$status" -eq 1 ]
    [[ "$output" == *"Symbol-cite drift"* ]]
}

@test "gate: a linked cite that is correct still passes" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    { for i in $(seq 54); do echo "// filler $i"; done; echo "void zz_target(void) {}"; } \
        > "$BATS_TEST_TMPDIR/src/zz_ok.cpp"
    mkdir -p "$BATS_TEST_TMPDIR/devel"
    printf '`zz_target()` ([`src/zz_ok.cpp:55`](../src/zz_ok.cpp#L55)) does the work.\n' \
        > "$BATS_TEST_TMPDIR/devel/ok.md"
    cd "$BATS_TEST_TMPDIR"
    run python3 "$CHECK" --devel devel/ok.md
    [ "$status" -eq 0 ]
}
