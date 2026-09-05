#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_clang_diagnostics.py - the clang/GCC divergence
# gate. It ran without a meta-suite, which is exactly how its .c
# misclassification shipped: generated font data under assets/fonts/ compiles
# as C (-std=c11), the gate replayed those command lines through clang++, and
# the resulting "invalid argument '-std=c11' not allowed with 'C++'" wall only
# surfaced at push time - the pre-commit hook deliberately skips this gate as
# too slow, so nothing else sees it fail. These pin both halves of its
# contract: the files it must stay quiet about, and the TU it must still fail
# on.
#
# The fixtures build their own compile database in bats tmp space via
# --compile-db-dir. Earlier versions put forged .ccj fragments under build/obj
# (the production glob root): a bats run killed between setup and teardown
# left them there, and every later --all audit failed on the deliberately
# broken TU - plus a make completing inside the window could bake them into
# compile_commands.json. A private database also means these tests need no
# prior build, so CI's bare checkout runs them instead of skipping.

load helpers

GATE="scripts/check_clang_diagnostics.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/clang-gate-db"
    rm -rf "$FIXTURE_DIR"
    mkdir -p "$FIXTURE_DIR"
    CURRENT_VERSION="$(cat VERSION.txt)"
}

# A TU whose only declaration sits behind a feature macro. Compiled with the
# -D the build passes today it is clean; compiled without it every use is an
# unknown type. That is the shape of a compile command recorded before the
# flag existed.
write_guarded_tu() {
    cat > "$FIXTURE_DIR/guarded.cpp" <<'EOF'
#ifdef HELIX_HAS_FIXTURE_FEATURE
struct FixtureFeature {
    int value;
};
#endif
int main() {
    FixtureFeature f{};
    return f.value;
}
EOF
}

# $1 = fragment basename, $2 = recorded version, $3... = extra flags
write_fragment() {
    local name="$1" version="$2"
    shift 2
    cat > "$FIXTURE_DIR/$name.ccj" <<EOF
{"directory": "$FIXTURE_DIR", "file": "guarded.cpp", "command": "g++ -std=c++17 -DHELIX_VERSION=\"$version\" -DHELIX_VERSION_PATCH=${version##*.} $* -c guarded.cpp -o $name.o"}
EOF
}

teardown() {
    rm -rf "$FIXTURE_DIR"
}

# Generated C font data is not a C++ TU: the gate must ignore it, not replay
# its -std=c11 command line through clang++. This is the exact regression that
# blocked a push: the spoolman merge added mdi_icons_*.c and the gate turned
# them into four failing "TUs". The fragment is forged because the regression
# only bites when the .c file IS in the compile database - an unknown .c would
# be skipped with a note either way, and a test relying on that passes with
# the bug present (found by mutation: the first version of this test survived
# re-adding .c to TU_EXTENSIONS).
@test "gate ignores .c files - generated font data is not a C++ TU" {
    cat > "$FIXTURE_DIR/font_data.c" <<'EOF'
const unsigned char test_font_data[] = {0x00, 0x01};
EOF
    cat > "$FIXTURE_DIR/font_data.ccj" <<EOF
{"directory": "$FIXTURE_DIR", "file": "font_data.c", "command": "gcc -std=c11 -c font_data.c"}
EOF
    run python3 "$GATE" --compile-db-dir "$FIXTURE_DIR" "$FIXTURE_DIR/font_data.c"
    if grep -qF "SKIP:" <<<"$output"; then
        skip "clang unavailable"
    fi
    [ "$status" -eq 0 ]
    refute grep -qF "invalid argument" <<<"$output"
    refute grep -qF "failing" <<<"$output"
}

# The catch half: a TU clang cannot parse must fail the gate, not pass
# quietly. The forged .ccj (what mk/rules.mk's emit-compile-command writes per
# TU at compile time) points the gate at a deliberately broken .cpp.
@test "gate fails on a TU clang cannot parse" {
    cat > "$FIXTURE_DIR/broken.cpp" <<'EOF'
int main() { this is not c++ }
EOF
    cat > "$FIXTURE_DIR/broken.ccj" <<EOF
{"directory": "$FIXTURE_DIR", "file": "broken.cpp", "command": "g++ -std=c++17 -c broken.cpp"}
EOF
    run python3 "$GATE" --compile-db-dir "$FIXTURE_DIR" "$FIXTURE_DIR/broken.cpp"
    if grep -qF "SKIP:" <<<"$output"; then
        skip "clang unavailable"
    fi
    [ "$status" -ne 0 ]
}

# A .ccj is a byproduct of compiling, and nothing rewrites it when the command
# changes without the source changing. An entry recorded by an older build
# describes flags the tree no longer passes, so replaying it produces
# diagnostics about the command rather than about the code. The gate must not
# report those as findings: it cannot trust the command, which is a different
# thing from the TU being broken.
@test "gate skips a TU whose compile command predates the current version" {
    write_guarded_tu
    write_fragment stale 0.99.1

    run python3 "$GATE" --compile-db-dir "$FIXTURE_DIR" "$FIXTURE_DIR/guarded.cpp"
    if grep -qF "SKIP:" <<<"$output"; then
        skip "clang unavailable"
    fi
    [ "$status" -eq 0 ]
    refute grep -qF "unknown type name" <<<"$output"
    grep -qF "stale compile command" <<<"$output"
    # quality-checks.sh shows only the last line on a pass, so the count has to
    # be on it: a run that checked nothing must not read as a clean bill.
    grep -qF "1 skipped (stale compile command)" <<<"$output"
}

# The distinction the skip must not collapse: at the current version the
# command describes today's build, so clang's verdict is about the code and a
# failure is real.
@test "gate still fails a broken TU whose compile command is current" {
    cat > "$FIXTURE_DIR/guarded.cpp" <<'EOF'
int main() { this is not c++ }
EOF
    write_fragment current "$CURRENT_VERSION" -DHELIX_HAS_FIXTURE_FEATURE=1

    run python3 "$GATE" --compile-db-dir "$FIXTURE_DIR" "$FIXTURE_DIR/guarded.cpp"
    if grep -qF "SKIP:" <<<"$output"; then
        skip "clang unavailable"
    fi
    [ "$status" -ne 0 ]
    refute grep -qF "stale compile command" <<<"$output"
}

# Every object tree emits its own fragment for the same source, so obj/,
# obj-asan/, obj-O0/ and obj-tsan/ each contribute an entry and only the tree
# built most recently carries today's flags. The gate must pick the entry that
# describes the current build, not whichever the glob yielded last.
@test "gate picks the current-version entry when several describe one file" {
    write_guarded_tu
    write_fragment aaa_current "$CURRENT_VERSION" -DHELIX_HAS_FIXTURE_FEATURE=1
    write_fragment zzz_stale 0.99.1
    # Recency only breaks ties the version stamp cannot, so the stale fragment
    # is dated later than the current one: nothing but the stamp can pick the
    # right entry here, and fragments written in one tick would otherwise leave
    # the choice to glob order.
    touch -t 200001010000 "$FIXTURE_DIR/aaa_current.ccj"

    run python3 "$GATE" --compile-db-dir "$FIXTURE_DIR" "$FIXTURE_DIR/guarded.cpp"
    if grep -qF "SKIP:" <<<"$output"; then
        skip "clang unavailable"
    fi
    [ "$status" -eq 0 ]
    refute grep -qF "unknown type name" <<<"$output"
    refute grep -qF "stale compile command" <<<"$output"
    grep -qF "1 TU(s)" <<<"$output"
}

# Deleting a source leaves its fragment behind, and clang answers a command
# naming a missing file with "no such file or directory" - an error about the
# database, not about any code under review.
@test "gate drops an entry whose source file no longer exists" {
    # A surviving TU keeps the database non-empty: an empty one makes the gate
    # print SKIP for want of any compile database, and this test would pass
    # having checked nothing.
    write_guarded_tu
    write_fragment live "$CURRENT_VERSION" -DHELIX_HAS_FIXTURE_FEATURE=1
    cat > "$FIXTURE_DIR/ghost.ccj" <<EOF
{"directory": "$FIXTURE_DIR", "file": "deleted_source.cpp", "command": "g++ -std=c++17 -c deleted_source.cpp"}
EOF
    run python3 "$GATE" --compile-db-dir "$FIXTURE_DIR" --all
    if grep -qF "SKIP:" <<<"$output"; then
        skip "clang unavailable"
    fi
    [ "$status" -eq 0 ]
    refute grep -qF "no such file or directory" <<<"$output"
    grep -qF "1 TU(s)" <<<"$output"
}

# A changed header is resolved to its dependent TUs through the build's .d
# files, and the fan-out is capped. Trust has to be settled before the cap
# applies: a widely included header whose dependents are mostly stale would
# otherwise spend the whole budget on TUs that get skipped, and the ones it
# could have checked never make the list.
@test "header fan-out spends its cap on TUs with a usable compile command" {
    cat > "$FIXTURE_DIR/feature.h" <<'EOF'
#pragma once
EOF
    # Dependents are taken in path order, so the stale one is named to sort
    # first: with the cap at one, a gate that ranks before it filters spends
    # the whole budget there and checks nothing.
    for name in aaa_stale_dep zzz_good_dep; do
        cat > "$FIXTURE_DIR/$name.cpp" <<EOF
#include "feature.h"
#ifdef HELIX_HAS_FIXTURE_FEATURE
struct FixtureFeature { int value; };
#endif
int ${name}_entry() { FixtureFeature f{}; return f.value; }
EOF
        cat > "$FIXTURE_DIR/$name.d" <<EOF
$FIXTURE_DIR/$name.o: $FIXTURE_DIR/$name.cpp $FIXTURE_DIR/feature.h
EOF
    done
    cat > "$FIXTURE_DIR/aaa_stale_dep.ccj" <<EOF
{"directory": "$FIXTURE_DIR", "file": "aaa_stale_dep.cpp", "command": "g++ -std=c++17 -DHELIX_VERSION=\"0.99.1\" -c aaa_stale_dep.cpp -o aaa_stale_dep.o"}
EOF
    cat > "$FIXTURE_DIR/zzz_good_dep.ccj" <<EOF
{"directory": "$FIXTURE_DIR", "file": "zzz_good_dep.cpp", "command": "g++ -std=c++17 -DHELIX_VERSION=\"$CURRENT_VERSION\" -DHELIX_HAS_FIXTURE_FEATURE=1 -c zzz_good_dep.cpp -o zzz_good_dep.o"}
EOF

    run python3 "$GATE" --compile-db-dir "$FIXTURE_DIR" --max-header-tus 1 "$FIXTURE_DIR/feature.h"
    if grep -qF "SKIP:" <<<"$output"; then
        skip "clang unavailable"
    fi
    [ "$status" -eq 0 ]
    refute grep -qF "unknown type name" <<<"$output"
    grep -qF "checked 1 TU(s)" <<<"$output"
    grep -qF "1 skipped (stale compile command)" <<<"$output"
}
