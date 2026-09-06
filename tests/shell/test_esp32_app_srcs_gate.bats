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

# --------------------------------------------- CMake-consumability (the E1 class)
#
# CMakeLists.txt reads the manifest with REGEX "^[^#].*\.(cpp|c)$". A line that
# does not END at the suffix never reaches the build, and the gate must not
# bless a line CMake will throw away — otherwise the gate goes green and the
# firmware link fails 25 minutes later.

@test "rejects a manifest path with a trailing '# reason' comment (CMake drops it)" {
    printf 'src/printer/compiled.cpp\nsrc/printer/secretly_new.cpp  # keep, AMS needs it\n' \
        > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "CMake will NOT compile" "$output"
    contains "line 2" "$output"
    contains "secretly_new.cpp" "$output"
    # and it must NOT be counted as compiled: the file is not silently "decided"
    [[ "$output" == *"DROPPED"* ]]
}

@test "rejects a manifest path with trailing whitespace (CMake drops it)" {
    printf 'src/printer/compiled.cpp\nsrc/printer/secretly_new.cpp   \n' > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "CMake will NOT compile" "$output"
    [[ "$output" == *"trailing whitespace"* ]]
}

@test "rejects a manifest path with leading whitespace (CMake builds a bogus path)" {
    printf 'src/printer/compiled.cpp\n  src/printer/secretly_new.cpp\n' > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "CMake will NOT compile" "$output"
    [[ "$output" == *"leading whitespace"* ]]
}

@test "rejects a manifest line whose suffix is not .cpp/.c" {
    printf 'src/printer/compiled.cpp\nsrc/printer/secretly_new.cpp\nsrc/printer/oops.h\n' \
        > "$ROOT/manifest.txt"
    run_gate
    [ "$status" -eq 1 ]
    contains "oops.h" "$output"
    [[ "$output" == *"does not end in .cpp/.c"* ]]
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
