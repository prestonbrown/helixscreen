#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_esp32_app_srcs.py — the ESP32 firmware app_srcs
# manifest drift gate.
#
# The firmware compiles a hand-maintained SUBSET of src/ (the "v1 Core+AMS cut":
# camera, label printer, gcode/bed-mesh 3D, plugins, timelapse viewer,
# screensaver, calibration, sound, mocks, and the concrete libhv client are all
# gated off). The list was generated once from the native-audit 491-file
# Xtensa-compile manifest and has drifted twice since — ams_endless_spool.cpp
# and toolhead_homing.cpp both landed in main without making app_srcs.txt, and
# the firmware link broke ~25 min into esp32-build CI.
#
# Curation is unavoidable for a subset build; the gate's job is to make the
# drift loud at quality-check / PR time. Every src/**/*.{cpp,c} must be in
# app_srcs.txt (compile it) OR app_srcs_excluded.txt (don't).
#
# These tests pin both halves. The catch half is the whole point — a new file
# that nobody decided on must fail, and so must a manifest line that CMake would
# silently drop (the gate is only as good as its agreement with the CMake
# REGEX "^[^#].*\.(cpp|c)$" that actually consumes the file). The quiet half
# matters equally: a gate that fired on files the manifest legitimately excludes
# would be noise on every firmware commit and would get switched off, defeating
# the purpose.

load helpers

GATE="scripts/check_esp32_app_srcs.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    # Fixture: a tiny src/ tree + manifest + exclusions, isolated from the repo.
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/fw"
    mkdir -p "$ROOT/src/printer" "$ROOT/src/camera"
    touch "$ROOT/src/printer/compiled.cpp" \
          "$ROOT/src/printer/secretly_new.cpp" \
          "$ROOT/src/printer/excluded_one.cpp" \
          "$ROOT/src/camera/cam.cpp"
    # manifest: compiles compiled.cpp only
    printf 'src/printer/compiled.cpp\n' > "$ROOT/manifest.txt"
    # exclusions: one file + one whole dir
    printf 'src/printer/excluded_one.cpp  # reason\nsrc/camera/  # whole dir\n' > "$ROOT/excluded.txt"
}

run_gate() {
    run python3 "$GATE" \
        --manifest "$ROOT/manifest.txt" \
        --exclusions "$ROOT/excluded.txt" \
        --src-root "$ROOT/src"
}

# Decide the one undecided fixture file, so a test can start from a clean tree.
decide_all() {
    printf 'src/printer/compiled.cpp\nsrc/printer/secretly_new.cpp\n' > "$ROOT/manifest.txt"
}

# ----------------------------------------------------------- the catch half

@test "flags a src/ file in neither manifest nor exclusions (the drift case)" {
    run_gate
    [ "$status" -eq 1 ]
    contains "secretly_new.cpp" "$output"
    contains "app_srcs.txt" "$output"
    [[ "$output" == *"app_srcs_excluded.txt"* ]]
}

@test "flags a stale manifest line (src/ file that no longer exists)" {
    printf 'src/printer/compiled.cpp\nsrc/printer/deleted.cpp\n' > "$ROOT/manifest.txt"
    printf 'src/printer/excluded_one.cpp  # reason\nsrc/camera/  # whole dir\n' > "$ROOT/excluded.txt"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"stale"*"deleted.cpp"* ]]
}

@test "--list prints the undecided files" {
    run python3 "$GATE" --list \
        --manifest "$ROOT/manifest.txt" \
        --exclusions "$ROOT/excluded.txt" \
        --src-root "$ROOT/src"
    [ "$status" -eq 1 ]
    [[ "$output" == *"src/printer/secretly_new.cpp"* ]]
}

# --------------------------------------------- CMake-consumability
#
# components/helixapp/app_srcs.cmake reads the manifest: a line holding one
# .cpp/.c path, with optional surrounding whitespace and an optional trailing
# '# comment', is compiled; '#' lines are comments. The gate parses the same
# way, so it never blesses a line CMake drops and never rejects one CMake keeps.

READER="firmware/helixscreen-esp32/components/helixapp/app_srcs.cmake"

# The paths CMake's own reader takes from a manifest, one per line.
cmake_reads() {
    command -v cmake >/dev/null || skip "cmake is not installed"
    cat > "$BATS_TEST_TMPDIR/read.cmake" <<CMAKE
include("$PWD/$READER")
helix_read_app_srcs("$1" out)
foreach(p IN LISTS out)
    message("\${p}")
endforeach()
CMAKE
    cmake -P "$BATS_TEST_TMPDIR/read.cmake" 2>&1
}

@test "a trailing '# reason', trailing or leading whitespace still compiles the path" {
    printf 'src/printer/compiled.cpp  # keep, AMS needs it\n  src/printer/secretly_new.cpp   \n' \
        > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 0 ] || fail "$output"
}

@test "CMake's reader takes the same paths the gate does" {
    printf '# heading; with [ an unbalanced bracket\nsrc/a.cpp  # reason\n  src/b.c\t\n# MOCK: src/mock.cpp\nsrc/c.cpp\n' \
        > "$BATS_TEST_TMPDIR/m.txt"
    run cmake_reads "$BATS_TEST_TMPDIR/m.txt"
    [ "$status" -eq 0 ] || fail "$output"
    [ "$output" = "$(printf 'src/a.cpp\nsrc/b.c\nsrc/c.cpp')" ] || fail "$output"
}

@test "a path line carrying ; or [ is refused, since CMake would split or merge it" {
    printf 'src/printer/compiled.cpp\nsrc/printer/secretly_new.cpp  # needs a; b\n' > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "line 2" "$output"
}

@test "rejects a manifest line whose suffix is not .cpp/.c" {
    printf 'src/printer/compiled.cpp\nsrc/printer/secretly_new.cpp\nsrc/printer/oops.h\n' \
        > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "oops.h" "$output"
    [[ "$output" == *"does not name one .cpp/.c path"* ]]
}

@test "a manifest comment line is still a comment, not a malformed path" {
    # The manifest carries its whole derivation ledger as '#' lines, including
    # commented-out '# MOCK:' paths. Flagging those would make the gate unusable.
    printf '# ===== section =====\n# MOCK: src/printer/ams_backend_mock.cpp\n  # indented note\nsrc/printer/compiled.cpp\nsrc/printer/secretly_new.cpp\n' \
        > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

# ------------------------------------------- exclusion rot (the E2 class)

@test "flags a stale exclusion entry (excluded file that no longer exists)" {
    decide_all
    printf 'src/printer/excluded_one.cpp  # reason\nsrc/printer/deleted_long_ago.cpp  # reason\nsrc/camera/  # whole dir\n' \
        > "$ROOT/excluded.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "stale app_srcs_excluded.txt" "$output"
    contains "deleted_long_ago.cpp" "$output"
    [[ "$output" == *"file no longer exists"* ]]
}

@test "flags a stale dir-level exclusion (no src/ files left beneath it)" {
    decide_all
    printf 'src/printer/excluded_one.cpp  # reason\nsrc/camera/  # whole dir\nsrc/gone/  # subsystem deleted\n' \
        > "$ROOT/excluded.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "stale app_srcs_excluded.txt" "$output"
    contains "src/gone/" "$output"
    [[ "$output" == *"no src/ files remain"* ]]
}

# --------------------------------------- manifest/exclusion overlap (the E3 class)

@test "flags a file listed in BOTH the manifest and the exclusions" {
    decide_all
    printf 'src/printer/excluded_one.cpp  # reason\nsrc/printer/compiled.cpp  # also excluded?!\nsrc/camera/  # whole dir\n' \
        > "$ROOT/excluded.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "BOTH app_srcs.txt and app_srcs_excluded.txt" "$output"
    [[ "$output" == *"src/printer/compiled.cpp"* ]]
}

@test "flags a manifest file swallowed by a dir-level exclusion" {
    decide_all
    # cam.cpp is compiled AND under the src/camera/ blanket exclusion
    printf 'src/printer/compiled.cpp\nsrc/printer/secretly_new.cpp\nsrc/camera/cam.cpp\n' \
        > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "BOTH app_srcs.txt and app_srcs_excluded.txt" "$output"
    contains "src/camera/cam.cpp" "$output"
    [[ "$output" == *"via the 'src/camera/' directory entry"* ]]
}

# ----------------------------------------------------------- the quiet half

@test "flags a firmware-compiled file that includes a std::locale-pulling header" {
    decide_all
    printf '#include <string>\n  #include <sstream>\n' > "$ROOT/src/printer/compiled.cpp"
    run_gate
    [ "$status" -eq 1 ]
    contains "src/printer/compiled.cpp:2: <sstream>" "$output"
    contains "core/text_io.h" "$output"
}

@test "a locale-pulling include in an excluded file is not the firmware's problem" {
    decide_all
    printf '#include <regex>\n#include <filesystem>\n' > "$ROOT/src/printer/excluded_one.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

@test "flags each call that aborts a firmware built without exceptions" {
    decide_all
    cat > "$ROOT/src/printer/compiled.cpp" <<'CPP'
int a = j.value("speed", 0);
auto b = j.at("name");
auto c = json::parse(text);
int d = std::stoi(s);
float e = std::any_cast<float>(value);
CPP
    run_gate
    [ "$status" -eq 1 ]
    contains "src/printer/compiled.cpp:1: json .value" "$output"
    contains "src/printer/compiled.cpp:2: json .at" "$output"
    contains "src/printer/compiled.cpp:3: one-argument json::parse" "$output"
    contains "src/printer/compiled.cpp:4: std::sto*" "$output"
    contains "src/printer/compiled.cpp:5: value-form std::any_cast" "$output"
}

@test "the non-throwing forms, comments and excluded files pass the aborting-call check" {
    decide_all
    cat > "$ROOT/src/printer/compiled.cpp" <<'CPP'
auto c = json::parse(text, nullptr, false);
const float* e = std::any_cast<float>(&value);
auto o = maybe.value();
// j.value("speed", 0) would throw here
CPP
    printf 'int d = std::stoi(s);\n' > "$ROOT/src/printer/excluded_one.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

@test "flags try, catch and throw in a firmware-compiled file" {
    decide_all
    cat > "$ROOT/src/printer/compiled.cpp" <<'CPP'
void f() {
    try {
        g();
    } catch (const std::exception& e) {
        throw std::runtime_error("x");
    }
}
CPP
    run_gate
    [ "$status" -eq 1 ]
    contains "src/printer/compiled.cpp:2: try" "$output"
    contains "src/printer/compiled.cpp:4: catch" "$output"
    contains "src/printer/compiled.cpp:5: throw" "$output"
    contains "exception_policy.h" "$output"
}

@test "a desktop-only net inside __cpp_exceptions passes; its #else branch is checked" {
    decide_all
    cat > "$ROOT/src/printer/compiled.cpp" <<'CPP'
#if defined(__cpp_exceptions)
    try {
        g();
    } catch (...) {
    }
#else
    g();
#endif
#ifdef __cpp_exceptions
    throw std::runtime_error("desktop");
#else
    try {
#endif
CPP
    run_gate
    [ "$status" -eq 1 ]
    contains "src/printer/compiled.cpp:12: try" "$output"
    lacks "compiled.cpp:2:" "$output"
    lacks "compiled.cpp:10:" "$output"
}

@test "branches the firmware does not compile pass, decided by its own compile definitions" {
    decide_all
    cat > "$ROOT/CMakeLists.txt" <<'CMAKE'
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # comment lines are skipped
    HELIX_HAS_VIEWER=0
    HELIX_HAS_ACE=1)
CMAKE
    cat > "$ROOT/src/printer/compiled.cpp" <<'CPP'
#if defined(ESP_PLATFORM)
    v = esp_random();
#else
    try { v = rd(); } catch (...) {}
#endif
#if HELIX_HAS_VIEWER
    try { build(); } catch (...) {}
#endif
#if !HELIX_HAS_ACE
    throw 1;
#endif
    helix::throw_or_abort(std::runtime_error("never"));
    log("do not throw here"); // a throw in a comment
CPP
    run_gate
    [ "$status" -eq 0 ]
}

@test "a gate the firmware leaves at its include/helix_features.h default is decided by that default" {
    decide_all
    mkdir -p "$ROOT/include"
    printf '#ifndef HELIX_HAS_ACE\n#define HELIX_HAS_ACE 1\n#endif\n#ifndef HELIX_HAS_VIEWER\n#define HELIX_HAS_VIEWER 1\n#endif\n' \
        > "$ROOT/include/helix_features.h"
    printf 'target_compile_definitions(${COMPONENT_LIB} PRIVATE\n    HELIX_HAS_VIEWER=0)\n' > "$ROOT/CMakeLists.txt"
    cat > "$ROOT/src/printer/compiled.cpp" <<'CPP'
#if !HELIX_HAS_ACE
    throw 1;
#endif
#if HELIX_HAS_VIEWER
    try { build(); } catch (...) {}
#endif
#if HELIX_HAS_ACE
    try { load(); } catch (...) {}
#endif
CPP
    run_gate
    [ "$status" -eq 1 ]
    contains "src/printer/compiled.cpp:8: try" "$output"
    lacks "compiled.cpp:2:" "$output"
    lacks "compiled.cpp:5:" "$output"
}

@test "a branch under a macro the gate cannot evaluate is checked" {
    decide_all
    cat > "$ROOT/src/printer/compiled.cpp" <<'CPP'
#if SOME_UNKNOWN_FEATURE && OTHER
    try { g(); } catch (...) {}
#endif
CPP
    run_gate
    [ "$status" -eq 1 ]
    contains "src/printer/compiled.cpp:2: try" "$output"
}

@test "headers are checked too: any of them can reach the firmware through an include" {
    decide_all
    mkdir -p "$ROOT/include"
    printf 'inline int n(const std::string& s) { return std::stoi(s); }\n' > "$ROOT/include/sorting.h"
    run_gate
    [ "$status" -eq 1 ]
    contains "include/sorting.h:1: std::sto*" "$output"
}

@test "an #else after a compiled #if branch is not compiled, whatever an #elif said" {
    decide_all
    cat > "$ROOT/src/printer/compiled.cpp" <<'CPP'
#if defined(ESP_PLATFORM)
    v = 1;
#elif SOMETHING
    v = 2;
#else
    try { g(); } catch (...) {}
#endif
CPP
    run_gate
    [ "$status" -eq 0 ]
}

@test "passes when every src/ file is in the manifest or exclusions" {
    # decide the new file: add it to the manifest
    decide_all
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "a dir-level exclusion (trailing /) covers every file beneath it" {
    decide_all
    run_gate
    # camera/cam.cpp is covered by the src/camera/ dir exclusion, so the tree is
    # fully decided — no findings at all, and cam.cpp is named nowhere.
    [ "$status" -eq 0 ]
    contains "OK" "$output"
    [[ "$output" != *"cam.cpp"* ]]
}

@test "ignores manifest entries outside src/ (e.g. lib/ sources)" {
    printf 'src/printer/compiled.cpp\nsrc/printer/secretly_new.cpp\nlib/lv_markdown/src/x.c\n' > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" != *"lv_markdown"* ]]
}

@test "an exclusion entry outside src/ is not reported as stale" {
    decide_all
    printf 'src/printer/excluded_one.cpp  # reason\nsrc/camera/  # whole dir\nlib/some/thing.c  # not our universe\n' \
        > "$ROOT/excluded.txt"
    run_gate
    [ "$status" -eq 0 ]
}

# --------------------------------------------- the failure message (the E5 class)

@test "drift guidance leads with app_srcs.txt, not the bulk exclusion tool" {
    run_gate
    [ "$status" -eq 1 ]
    # The remedy for a file you just added is the manifest. --write-exclusions
    # answers "exclude it" for every undecided file at once, which is the wrong
    # answer here; the message must steer away from it, not toward it.
    contains "Add the bare path to" "$output"
    [[ "$output" == *"Do NOT reach for --write-exclusions"* ]]
}

# ----------------------------------------------------------- the seed tooling

@test "--write-exclusions seeds a baseline covering all undecided files" {
    rm -f "$ROOT/excluded.txt"
    run python3 "$GATE" --write-exclusions \
        --manifest "$ROOT/manifest.txt" \
        --exclusions "$ROOT/excluded.txt" \
        --src-root "$ROOT/src"
    [ "$status" -eq 0 ]
    # now the gate passes with the freshly-seeded baseline
    run_gate
    [ "$status" -eq 0 ]
}

@test "--write-exclusions refuses to overwrite a baseline without --force" {
    printf '# hand-written baseline\nsrc/printer/excluded_one.cpp\n' > "$ROOT/excluded.txt"
    run python3 "$GATE" --write-exclusions \
        --manifest "$ROOT/manifest.txt" \
        --exclusions "$ROOT/excluded.txt" \
        --src-root "$ROOT/src"
    [ "$status" -eq 1 ]
    contains "--force" "$output"
    # the hand-written baseline is untouched
    [[ "$(cat "$ROOT/excluded.txt")" == *"hand-written baseline"* ]]
}

@test "--write-exclusions --force merges: existing entries and reasons survive" {
    # The destructive shape this replaces: with nothing undecided, a regenerate
    # emitted a header and nothing else, wiping the whole baseline.
    printf 'src/printer/excluded_one.cpp  # AMS-only, needs libhv\nsrc/camera/  # camera is gated off\n' \
        > "$ROOT/excluded.txt"
    run python3 "$GATE" --write-exclusions --force \
        --manifest "$ROOT/manifest.txt" \
        --exclusions "$ROOT/excluded.txt" \
        --src-root "$ROOT/src"
    [ "$status" -eq 0 ]
    written="$(cat "$ROOT/excluded.txt")"
    [[ "$written" == *"src/printer/excluded_one.cpp"*"AMS-only, needs libhv"* ]] \
        || fail "excluded_one.cpp is not listed with its reason: $written"
    [[ "$written" == *"src/camera/"*"camera is gated off"* ]] \
        || fail "src/camera/ is not listed with its reason: $written"
    # the undecided file was added
    contains "src/printer/secretly_new.cpp" "$written"
    run_gate
    [ "$status" -eq 0 ]
}

@test "--write-exclusions compresses to the SHALLOWEST whole-undecided directory" {
    mkdir -p "$ROOT/src/gated/sub/deeper"
    touch "$ROOT/src/gated/top.cpp" \
          "$ROOT/src/gated/sub/mid.cpp" \
          "$ROOT/src/gated/sub/deeper/leaf.cpp"
    rm -f "$ROOT/excluded.txt"
    run python3 "$GATE" --write-exclusions \
        --manifest "$ROOT/manifest.txt" \
        --exclusions "$ROOT/excluded.txt" \
        --src-root "$ROOT/src"
    [ "$status" -eq 0 ]
    written="$(cat "$ROOT/excluded.txt")"
    # one line for the whole tree, not one per subdirectory
    contains "src/gated/" "$written"
    lacks "src/gated/sub/" "$written"
    lacks "src/gated/sub/deeper/" "$written"
    # and the count describes files beneath, not the number of choosers
    contains "all 3 src/ files beneath" "$written"
    run_gate
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------- --link (object level)
# compiled.cpp is listed and excluded_one.cpp is excluded; the objects are real,
# built here, so nm sees exactly what a native build would.

build_link_fixture() {
    printf 'int only_excluded_defines();\n' > "$ROOT/src/printer/excluded_one.h"
    printf '#include "excluded_one.h"\nint caller() { %s }\n' "$1" \
        > "$ROOT/src/printer/compiled.cpp"
    printf 'int only_excluded_defines() { return 7; }\n' > "$ROOT/src/printer/excluded_one.cpp"
    mkdir -p "$ROOT/obj/printer" "$ROOT/fwroot"
    # The native build has every subsystem on; the firmware CMakeLists below does not.
    c++ -DHELIX_HAS_CAMERA=1 -c "$ROOT/src/printer/compiled.cpp" -o "$ROOT/obj/printer/compiled.o"
    c++ -c "$ROOT/src/printer/excluded_one.cpp" -o "$ROOT/obj/printer/excluded_one.o"
    printf 'target_compile_definitions(${COMPONENT_LIB} PRIVATE\n    HELIX_HAS_CAMERA=0)\n' \
        > "$ROOT/CMakeLists.txt"
    printf 'max-edges: 0\n' > "$ROOT/link_baseline.txt"
}

EDGE="src/printer/compiled.cpp -> src/printer/excluded_one.cpp"

run_link_gate() {
    run python3 "$GATE" --link \
        --manifest "$ROOT/manifest.txt" \
        --exclusions "$ROOT/excluded.txt" \
        --src-root "$ROOT/src" \
        --obj-root "$ROOT/obj" \
        --firmware-root "$ROOT/fwroot" \
        --baseline "$ROOT/link_baseline.txt"
}

@test "--link fails when a listed file calls a symbol only an excluded file defines" {
    build_link_fixture 'return only_excluded_defines();'
    run_link_gate
    [ "$status" -eq 1 ]
    contains "src/printer/compiled.cpp -> src/printer/excluded_one.cpp" "$output"
    contains "only_excluded_defines()" "$output"
}

@test "--link passes when the firmware's own sources stub the symbol" {
    build_link_fixture 'return only_excluded_defines();'
    printf 'int only_excluded_defines() { return 0; }\n' > "$ROOT/fwroot/stubs.cpp"
    run_link_gate
    [ "$status" -eq 0 ]
}

@test "--link passes a call inside a branch the firmware does not compile" {
    build_link_fixture $'\n#if HELIX_HAS_CAMERA\n    return only_excluded_defines();\n#else\n    return 0;\n#endif\n'
    run_link_gate
    [ "$status" -eq 0 ]
}

@test "--link exits 2 when a listed file has no object to read" {
    build_link_fixture 'return only_excluded_defines();'
    rm "$ROOT/obj/printer/compiled.o"
    run_link_gate
    [ "$status" -eq 2 ]
    contains "build first" "$output"
}

@test "--link: a firmware source that only mentions the name and its class does not stub it" {
    printf 'struct SoundMgr { static int start(); };\n' > "$ROOT/src/printer/excluded_one.h"
    printf '#include "excluded_one.h"\nint caller() { return SoundMgr::start(); }\n' \
        > "$ROOT/src/printer/compiled.cpp"
    printf '#include "excluded_one.h"\nint SoundMgr::start() { return 1; }\n' \
        > "$ROOT/src/printer/excluded_one.cpp"
    mkdir -p "$ROOT/obj/printer" "$ROOT/fwroot"
    c++ -I"$ROOT/src/printer" -c "$ROOT/src/printer/compiled.cpp" -o "$ROOT/obj/printer/compiled.o"
    c++ -I"$ROOT/src/printer" -c "$ROOT/src/printer/excluded_one.cpp" \
        -o "$ROOT/obj/printer/excluded_one.o"
    printf 'max-edges: 0\n' > "$ROOT/link_baseline.txt"
    # Both words appear, and Other::start has a body, but nothing defines SoundMgr::start.
    printf '// SoundMgr is not built here\nstruct Other { int start(); };\nint Other::start() { return SoundMgr::start(); }\n' \
        > "$ROOT/fwroot/stubs.cpp"
    run_link_gate
    [ "$status" -eq 1 ]
    contains "SoundMgr::start()" "$output"
}

@test "--link: a baselined edge passes" {
    build_link_fixture 'return only_excluded_defines();'
    printf 'max-edges: 1\n%s\n' "$EDGE" > "$ROOT/link_baseline.txt"
    run_link_gate
    [ "$status" -eq 0 ]
}

@test "--link: a baselined edge that no longer occurs fails" {
    build_link_fixture 'return 0;'
    printf 'max-edges: 1\n%s\n' "$EDGE" > "$ROOT/link_baseline.txt"
    run_link_gate
    [ "$status" -eq 1 ]
    contains "no longer occur" "$output"
    contains "$EDGE" "$output"
}

@test "--link: a baseline holding more entries than max-edges fails" {
    build_link_fixture 'return only_excluded_defines();'
    printf 'max-edges: 0\n%s\n' "$EDGE" > "$ROOT/link_baseline.txt"
    run_link_gate
    [ "$status" -eq 1 ]
    contains "against max-edges: 0" "$output"
}

@test "--link passes a call guarded by a flag only the native Makefile sets" {
    build_link_fixture $'\n#ifdef HELIX_HAS_BUZZER\n    return only_excluded_defines();\n#endif\n    return 0;\n'
    c++ -DHELIX_HAS_BUZZER -c "$ROOT/src/printer/compiled.cpp" -o "$ROOT/obj/printer/compiled.o"
    printf 'CXXFLAGS += -DHELIX_HAS_BUZZER\n' > "$ROOT/Makefile"
    run_link_gate
    [ "$status" -eq 0 ]
}

@test "--link: a Makefile flag a header also #defines may be set on the firmware" {
    build_link_fixture $'\n#ifdef HELIX_HAS_BUZZER\n    return only_excluded_defines();\n#endif\n    return 0;\n'
    c++ -DHELIX_HAS_BUZZER -c "$ROOT/src/printer/compiled.cpp" -o "$ROOT/obj/printer/compiled.o"
    printf 'CXXFLAGS += -DHELIX_HAS_BUZZER\n' > "$ROOT/Makefile"
    printf '#define HELIX_HAS_BUZZER 1\n' > "$ROOT/src/printer/platform.h"
    run_link_gate
    [ "$status" -eq 1 ]
    contains "$EDGE" "$output"
}

# The source never spells only_excluded_defines: the call comes from an inline
# in the header it includes.
build_inline_fixture() {
    printf '#pragma once\nint only_excluded_defines();\ninline int wrap() { return only_excluded_defines(); }\n' \
        > "$ROOT/src/printer/excluded_one.h"
    printf '%s\nint caller() { return wrap(); }\n%s\n' "$1" "$2" > "$ROOT/src/printer/compiled.cpp"
    c++ -DHELIX_HAS_CAMERA=1 -c "$ROOT/src/printer/compiled.cpp" -o "$ROOT/obj/printer/compiled.o"
}

@test "--link fails a header inline's reference when the header is included live" {
    build_link_fixture 'return 0;'
    build_inline_fixture '#include "excluded_one.h"' ''
    run_link_gate
    [ "$status" -eq 1 ]
    contains "$EDGE" "$output"
}

@test "--link passes a header inline's reference when the header is included only in a dead branch" {
    build_link_fixture 'return 0;'
    build_inline_fixture $'#if HELIX_HAS_CAMERA\n#include "excluded_one.h"' '#endif'
    run_link_gate
    [ "$status" -eq 0 ]
}

@test "--link fails a header inline's reference when a live header reaches the header a dead branch includes" {
    build_link_fixture 'return 0;'
    printf '#include "excluded_one.h"\n' > "$ROOT/src/printer/live.h"
    build_inline_fixture $'#include "live.h"\n#if HELIX_HAS_CAMERA\n#include "excluded_one.h"' '#endif'
    run_link_gate
    [ "$status" -eq 1 ]
    contains "$EDGE" "$output"
}

@test "--link fails a live member call even when its class is named only in a dead branch" {
    printf 'struct Pwm { void update(); };\ninline Pwm& get_pwm() { static Pwm p; return p; }\n' \
        > "$ROOT/src/printer/excluded_one.h"
    printf '#include "excluded_one.h"\nvoid Pwm::update() {}\n' > "$ROOT/src/printer/excluded_one.cpp"
    printf '#include "excluded_one.h"\n#if HELIX_HAS_CAMERA\nPwm unused;\n#endif\nvoid caller() { get_pwm().update(); }\n' \
        > "$ROOT/src/printer/compiled.cpp"
    mkdir -p "$ROOT/obj/printer" "$ROOT/fwroot"
    c++ -DHELIX_HAS_CAMERA=1 -I"$ROOT/src/printer" -c "$ROOT/src/printer/compiled.cpp" -o "$ROOT/obj/printer/compiled.o"
    c++ -I"$ROOT/src/printer" -c "$ROOT/src/printer/excluded_one.cpp" -o "$ROOT/obj/printer/excluded_one.o"
    printf 'target_compile_definitions(${COMPONENT_LIB} PRIVATE\n    HELIX_HAS_CAMERA=0)\n' > "$ROOT/CMakeLists.txt"
    printf 'max-edges: 0\n' > "$ROOT/link_baseline.txt"
    run_link_gate
    [ "$status" -eq 1 ]
    contains "Pwm::update()" "$output"
}

@test "--link: a Makefile flag a firmware sdkconfig mentions may be set on the firmware" {
    build_link_fixture $'\n#ifdef HELIX_HAS_BUZZER\n    return only_excluded_defines();\n#endif\n    return 0;\n'
    c++ -DHELIX_HAS_BUZZER -c "$ROOT/src/printer/compiled.cpp" -o "$ROOT/obj/printer/compiled.o"
    printf 'CXXFLAGS += -DHELIX_HAS_BUZZER\n' > "$ROOT/Makefile"
    mkdir -p "$ROOT/firmware/board"
    printf 'HELIX_HAS_BUZZER=y\n' > "$ROOT/firmware/board/sdkconfig.defaults"
    run_link_gate
    [ "$status" -eq 1 ]
    contains "$EDGE" "$output"
}
