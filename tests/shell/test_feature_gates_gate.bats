#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_feature_gates.py, the gate keeping every
# HELIX_HAS_* feature gate's default in include/helix_features.h.
#
# An undefined identifier in `#if` evaluates to 0, so a build that forgets to
# pass a gate compiles the feature out with no error. The header gives every
# gate its default and each build overrides only what it changes; the gate
# makes that the only place a default can live.

load helpers

GATE="scripts/check_feature_gates.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/include" "$ROOT/src/ui" "$ROOT/mk" "$ROOT/android/app/jni" \
        "$ROOT/firmware/helixscreen-esp32/components/helixapp"
    cat > "$ROOT/include/helix_features.h" <<'EOF'
#pragma once
// Label printing.
#ifndef HELIX_HAS_LABEL_PRINTER
#define HELIX_HAS_LABEL_PRINTER 1
#endif
EOF
    cat > "$ROOT/src/ui/panel.cpp" <<'EOF'
// HELIX_HAS_NOT_A_GATE in prose is not a use.
#if HELIX_HAS_LABEL_PRINTER
int label = 1;
#endif
#ifdef HELIX_HAS_SOUND
int sound = 1;
#endif
EOF
    : > "$ROOT/Makefile"
    : > "$ROOT/android/app/jni/CMakeLists.txt"
    : > "$ROOT/firmware/helixscreen-esp32/components/helixapp/CMakeLists.txt"
}

@test "a clean fixture passes, and prose and probed presence flags stay quiet" {
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "a gate used in #if but declared nowhere is named with its file and line" {
    printf '#if HELIX_HAS_WIDGET_X\nint x;\n#endif\n' > "$ROOT/src/ui/widget.cpp"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "src/ui/widget.cpp:1" "$output"
    contains "HELIX_HAS_WIDGET_X" "$output"
}

@test "a defined() test of an undeclared gate is caught too" {
    printf '#if defined(HELIX_HAS_WIDGET_Y) && 1\nint y;\n#endif\n' > "$ROOT/src/ui/widget.cpp"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "HELIX_HAS_WIDGET_Y" "$output"
}

@test "a flag a header defines for itself stays quiet" {
    printf '#define HELIX_HAS_FONT_96 (TIER >= 6)\n' > "$ROOT/include/fonts.h"
    printf '#if HELIX_HAS_FONT_96\nint f;\n#endif\n' > "$ROOT/src/ui/font_use.cpp"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "a default the header sets without an #ifndef guard is refused" {
    printf '#pragma once\n#define HELIX_HAS_LABEL_PRINTER 1\n' > "$ROOT/include/helix_features.h"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "#ifndef" "$output"
}

@test "a second default for a declared gate outside the header is refused" {
    printf '#ifndef HELIX_HAS_LABEL_PRINTER\n#define HELIX_HAS_LABEL_PRINTER 0\n#endif\n' > "$ROOT/src/ui/local.cpp"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "src/ui/local.cpp" "$output"
}

@test "a build file passing a gate the header does not declare is named" {
    printf 'CXXFLAGS += -DHELIX_HAS_LABEL_PRINTR=0\n' > "$ROOT/Makefile"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "HELIX_HAS_LABEL_PRINTR" "$output"
    contains "Makefile" "$output"
}

@test "a CMake definition of an undeclared gate is named" {
    printf 'target_compile_definitions(lib PRIVATE\n    HELIX_HAS_CAMRA=0)\n' \
        > "$ROOT/firmware/helixscreen-esp32/components/helixapp/CMakeLists.txt"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "HELIX_HAS_CAMRA" "$output"
}

@test "a missing header fails closed instead of passing an empty check" {
    rm "$ROOT/include/helix_features.h"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 2 ] || fail "$output"
}

@test "a presence flag passed as =0 is refused, since #ifdef still sees it" {
    printf 'target_compile_definitions(lib PRIVATE\n    HELIX_HAS_SOUND=0)\n' \
        > "$ROOT/firmware/helixscreen-esp32/components/helixapp/CMakeLists.txt"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "HELIX_HAS_SOUND" "$output"
    contains "#ifdef" "$output"
}

@test "a presence flag passed bare stays quiet" {
    printf 'target_compile_definitions(lib PRIVATE\n    HELIX_HAS_SOUND)\n' > "$ROOT/android/app/jni/CMakeLists.txt"
    printf 'SOUND_CXXFLAGS := -DHELIX_HAS_SOUND\n' > "$ROOT/Makefile"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "a header default not in the exact form the Makefile reads is refused" {
    printf '#pragma once\n#ifndef HELIX_HAS_LABEL_PRINTER\n#define HELIX_HAS_LABEL_PRINTER 1 // on\n#endif\n' \
        > "$ROOT/include/helix_features.h"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "#define HELIX_HAS_LABEL_PRINTER" "$output"
    contains "Makefile" "$output"
}

@test "an unguarded redefinition of a declared gate outside the header is refused" {
    printf '#define HELIX_HAS_LABEL_PRINTER 0\n' > "$ROOT/src/ui/local.cpp"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "src/ui/local.cpp:1" "$output"
}

@test "#ifdef on a declared gate is refused, since every build defines it" {
    printf '#ifdef HELIX_HAS_LABEL_PRINTER\nint x;\n#endif\n' > "$ROOT/src/ui/ifdef.cpp"
    run python3 "$GATE" --root "$ROOT"
    [ "$status" -eq 1 ] || fail "$output"
    contains "src/ui/ifdef.cpp:1" "$output"
    contains "#if" "$output"
}
