#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for two inline gates: the hardcoded-color ratchet of
# scripts/qc/design_tokens.sh, and the merge-conflict-marker check of
# scripts/qc/phase1.sh. Each case builds a scratch tree and runs the gate there.

load helpers

setup() {
    REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    ROOT="$BATS_TEST_TMPDIR/tree"
    mkdir -p "$ROOT/src/ui" "$ROOT/src/rendering" "$ROOT/include" "$ROOT/scripts"
    cp "$REPO/scripts/ratchet.py" "$ROOT/scripts/"
}

# colors <n> <file>: n hardcoded lv_color_hex(0x...) lines.
colors() {
    for i in $(seq 1 "$1"); do printf 'auto c%d = lv_color_hex(0x%06x);\n' "$i" "$i"; done > "$2"
}

tokens() {
    run bash -c 'cd "$1" && STAGED_ONLY=false && . "$2/scripts/qc/_lib.sh" && . "$2/scripts/qc/design_tokens.sh" && qc_design_tokens' \
        _ "$ROOT" "$REPO"
}

@test "tokens: hardcoded colors above the baseline fail and list each site" {
    colors 28 "$ROOT/src/ui/a.cpp"
    tokens
    [ "$status" -eq 1 ]
    contains "Hardcoded colors: 28 exceeds baseline (27)" "$output"
    contains "src/ui/a.cpp:1:" "$output"
}

@test "tokens: colors at the baseline pass" {
    colors 27 "$ROOT/src/ui/a.cpp"
    tokens
    [ "$status" -eq 0 ]
    contains "27 == baseline (27)" "$output"
}

@test "tokens: colors under the baseline pass and ask for the baseline to come down" {
    colors 5 "$ROOT/src/ui/a.cpp"
    tokens
    [ "$status" -eq 0 ]
    contains "ratchet down" "$output"
}

@test "tokens: the theme, renderer and canvas files are not counted" {
    colors 40 "$ROOT/src/rendering/r.cpp"
    colors 40 "$ROOT/src/ui/canvas_draw.cpp"
    tokens
    [ "$status" -eq 0 ]
    contains "0 hardcoded colors" "$output"
}

phase1() {
    run bash -c 'cd "$1" && STAGED_ONLY=false FILES="$3" && . "$2/scripts/qc/_lib.sh" && . "$2/scripts/qc/phase1.sh" && qc_phase1' \
        _ "$ROOT" "$REPO" "$1"
}

@test "phase1: each of the three conflict markers at a line start is reported" {
    printf 'a\n<<<<<<< HEAD\nb\n' > "$ROOT/src/ui/ours.cpp"
    printf 'a\n=======\nb\n' > "$ROOT/src/ui/mid.cpp"
    printf 'a\n>>>>>>> branch\n' > "$ROOT/src/ui/theirs.cpp"
    phase1 "$(printf '%s\n' src/ui/ours.cpp src/ui/mid.cpp src/ui/theirs.cpp)"
    [ "$status" -eq 1 ]
    contains "conflict markers found" "$output"
    contains "src/ui/ours.cpp" "$output"
    contains "src/ui/mid.cpp" "$output"
    contains "src/ui/theirs.cpp" "$output"
}

@test "phase1: a marker-like string that does not start the line is quiet" {
    printf 'auto s = "<<<<<<< x";\n  // ======= not at line start\nint a;\n' > "$ROOT/src/ui/ok.cpp"
    phase1 "src/ui/ok.cpp"
    contains "No merge conflict markers" "$output"
}
