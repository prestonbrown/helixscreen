#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Android build's precompiled header list, configured and compiled as it is
# written. Each entry sits in a $<COMPILE_LANGUAGE:CXX> generator expression,
# and a header name in angle brackets inside one needs $<ANGLE-R>: a bare '>'
# ends the expression early and the compiler is handed '>' as a file name.
# The scratch project stands in fake headers for every entry, so a missing or
# broken entry fails the compile here instead of in the Gradle build.

CMAKE="android/app/jni/CMakeLists.txt"

setup() {
    load helpers
    command -v cmake >/dev/null || skip "cmake is not installed"
    command -v c++ >/dev/null || skip "no C++ compiler"
    P="$BATS_TEST_TMPDIR/p"
    mkdir -p "$P/root/include" "$P/inc/spdlog" "$P/inc/hv" "$P/b"
    printf '#pragma once\n#define PCH_MAIN 1\n' > "$P/root/include/lvgl_pch.h"
    printf '#pragma once\n#define PCH_SPDLOG 1\n' > "$P/inc/spdlog/spdlog.h"
    printf '#pragma once\n#define PCH_JSON 1\n' > "$P/inc/hv/json.hpp"
    {
        printf '#if !defined(PCH_MAIN) || !defined(PCH_SPDLOG) || !defined(PCH_JSON)\n'
        printf '#error a precompiled header is missing\n#endif\nint f() { return 0; }\n'
    } > "$P/a.cpp"
    # C sources must compile without any of them.
    printf '#ifdef PCH_MAIN\n#error C got the C++ header\n#endif\nint g(void) { return 0; }\n' > "$P/b.c"
    {
        printf 'cmake_minimum_required(VERSION 3.14)\nproject(p C CXX)\n'
        printf 'set(PROJECT_ROOT "%s/root")\n' "$P"
        printf 'add_library(main STATIC a.cpp b.c)\n'
        printf 'target_include_directories(main PRIVATE "%s/inc")\n' "$P"
        sed -n '/^target_precompile_headers(main PRIVATE$/,/^)/p' "$CMAKE"
    } > "$P/CMakeLists.txt"
}

@test "the Android precompiled headers configure and compile" {
    grep -q 'spdlog/spdlog.h' "$P/CMakeLists.txt"
    grep -q 'hv/json.hpp' "$P/CMakeLists.txt"
    run cmake -S "$P" -B "$P/b"
    [ "$status" -eq 0 ] || { echo "$output"; false; }
    run cmake --build "$P/b"
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}
