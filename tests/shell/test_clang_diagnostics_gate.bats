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

GATE="scripts/check_clang_diagnostics.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/clang-gate-db"
    rm -rf "$FIXTURE_DIR"
    mkdir -p "$FIXTURE_DIR"
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
    if [[ "$output" == *"SKIP:"* ]]; then
        skip "clang unavailable"
    fi
    [ "$status" -eq 0 ]
    [[ "$output" != *"invalid argument"* ]]
    [[ "$output" != *"failing"* ]]
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
    if [[ "$output" == *"SKIP:"* ]]; then
        skip "clang unavailable"
    fi
    [ "$status" -ne 0 ]
}
