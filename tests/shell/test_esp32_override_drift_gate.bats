#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_esp32_override_drift.py — the drift gate for the
# ESP32 native-audit overrides/ forks.
#
# firmware/native-audit/overrides/ holds ten hand-maintained COPIES of src/
# files; app_srcs.txt makes CMake compile the copy INSTEAD of the twin. Each
# fork exists for a narrow reason (on Xtensa int32_t is long, so std::max's
# template argument needs spelling out; the renderer's whole-file
# HELIX_HAS_GCODE_VIEWER guard is stripped for size attribution). Everything
# else that differs is drift, and drift here means a fix that landed in src/ is
# silently reintroduced as a bug on the ESP32 build.
#
# Nothing else can catch it: the audit tree is not a make target, CI never
# compiles it, and no test links it (#1427).
#
# The catch half is the whole point: a twin that moves without its fork must go
# red. The quiet half matters as much — a gate that fired on the deliberate
# Xtensa divergence would be red on every commit and would get switched off.

GATE="scripts/check_esp32_override_drift.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    # Fixture: a tiny src/ tree with a matching overrides/ fork of each file.
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/audit"
    mkdir -p "$ROOT/src/ui" "$ROOT/src/printer" "$ROOT/overrides"

    # synced.cpp — fork carries one MARKED divergence and nothing else.
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\n' \
        > "$ROOT/src/ui/synced.cpp"
    printf 'int a() { return 1; }\nint b() { return std::max<uint32_t>(1u, x); } // AUDIT OVERRIDE: int32_t=long on Xtensa\nint c() { return 3; }\n' \
        > "$ROOT/overrides/synced.cpp"

    # clone.cpp — fork is byte-identical to its twin (a redundant fork).
    printf 'int only() { return 0; }\n' > "$ROOT/src/printer/clone.cpp"
    printf 'int only() { return 0; }\n' > "$ROOT/overrides/clone.cpp"

    # Baseline: both at zero unmarked drift.
    printf '0 synced.cpp\n0 clone.cpp\n' > "$ROOT/base.txt"
}

run_gate() {
    run python3 "$GATE" \
        --overrides "$ROOT/overrides" \
        --src-root "$ROOT/src" \
        --baseline "$ROOT/base.txt"
}

# ----------------------------------------------------------- the catch half
#
# This is the #1427 shape: a commit edits a src/ file and does not edit its
# fork. Every one of these must be red.

@test "flags a twin that gained a fix the fork did not (the drift case)" {
    # src/ grows a guard the fork does not have.
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\nvoid fixed() { entry.disable_and_unplace(); }\n' \
        > "$ROOT/src/ui/synced.cpp"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"drifted further from their src/ twin"* ]]
    [[ "$output" == *"synced.cpp"* ]]
    [[ "$output" == *"baseline 0"* ]]
}

@test "flags drift even when the fork already carries a marked divergence" {
    # The marker on line 2 must not bless an unrelated change elsewhere —
    # otherwise one AUDIT OVERRIDE comment would silence the whole file.
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\nint d() { return 4; }\nint e() { return 5; }\nint f() { return 6; }\nint g() { return 7; }\nint h() { return 8; }\nint NEW() { return 9; }\n' \
        > "$ROOT/src/ui/synced.cpp"
    printf 'int a() { return 1; }\nint b() { return std::max<uint32_t>(1u, x); } // AUDIT OVERRIDE: int32_t=long on Xtensa\nint c() { return 3; }\nint d() { return 4; }\nint e() { return 5; }\nint f() { return 6; }\nint g() { return 7; }\nint h() { return 8; }\n' \
        > "$ROOT/overrides/synced.cpp"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"synced.cpp"* ]]
}

@test "flags an override with no src/ twin at all" {
    printf 'int orphan() { return 0; }\n' > "$ROOT/overrides/orphan.cpp"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"cannot be paired"* ]]
    [[ "$output" == *"orphan.cpp"* ]]
    [[ "$output" == *"no src/ file with this basename"* ]]
}

@test "flags an override whose basename is ambiguous in src/" {
    # Two src/ files share the name: picking one would check the fork against
    # the wrong twin and report a clean file, which is worse than failing.
    printf 'int x() { return 0; }\n' > "$ROOT/src/ui/dupe.cpp"
    printf 'int x() { return 0; }\n' > "$ROOT/src/printer/dupe.cpp"
    printf 'int x() { return 0; }\n' > "$ROOT/overrides/dupe.cpp"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"AMBIGUOUS"* || "$output" == *"share this basename"* ]]
    [[ "$output" == *"src/printer/dupe.cpp"* ]]
    [[ "$output" == *"src/ui/dupe.cpp"* ]]
}

@test "flags a baseline entry for an override that no longer exists" {
    printf '0 synced.cpp\n0 clone.cpp\n7 deleted_fork.cpp\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"no longer exist"* ]]
    [[ "$output" == *"deleted_fork.cpp"* ]]
}

@test "--list names the exact unmarked lines" {
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\nvoid ported() { }\n' \
        > "$ROOT/src/ui/synced.cpp"
    run python3 "$GATE" --list \
        --overrides "$ROOT/overrides" \
        --src-root "$ROOT/src" \
        --baseline "$ROOT/base.txt"
    [ "$status" -eq 1 ]
    [[ "$output" == *"overrides/synced.cpp:"* ]]
}

@test "the failure message says port the change, not raise the baseline" {
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\nvoid ported() { }\n' \
        > "$ROOT/src/ui/synced.cpp"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"Port the missing changes"* ]]
    [[ "$output" == *"Do not raise the baseline to pass"* ]]
}

# ----------------------------------------------------------- the quiet half
#
# The deliberate Xtensa divergence is the reason these files exist. If the gate
# fired on it, it would be noise on every commit and would get turned off.

@test "passes when the only divergence carries an AUDIT OVERRIDE marker" {
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "a marker covers a deletion it stands in place of" {
    # The renderer fork strips a whole-file #if guard; the closing #endif is a
    # pure deletion with no fork line of its own to mark. A comment left where
    # it was must count, or that fork can never be clean.
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\n#endif // HELIX_HAS_GCODE_VIEWER\n' \
        > "$ROOT/src/ui/synced.cpp"
    printf 'int a() { return 1; }\nint b() { return std::max<uint32_t>(1u, x); } // AUDIT OVERRIDE: int32_t=long on Xtensa\nint c() { return 3; }\n// AUDIT OVERRIDE: closing half of the stripped HELIX_HAS_GCODE_VIEWER guard.\n' \
        > "$ROOT/overrides/synced.cpp"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "a byte-identical fork is reported as redundant but does NOT fail" {
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"byte-identical"* ]]
    [[ "$output" == *"clone.cpp"* ]]
    [[ "$output" == *"app_srcs.txt"* ]]
}

@test "existing debt at its baseline passes; the same debt +1 line fails" {
    # The ratchet: five of the ten real forks are hundreds of lines behind. A
    # hard zero would be red on arrival and would get switched off, so the
    # baseline freezes today's debt and only a RISE fails.
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\nint stale1() { return 0; }\nint stale2() { return 0; }\n' \
        > "$ROOT/src/ui/synced.cpp"
    printf '2 synced.cpp\n0 clone.cpp\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 0 ]

    # one more line lands in src/ and not in the fork
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\nint stale1() { return 0; }\nint stale2() { return 0; }\nint stale3() { return 0; }\n' \
        > "$ROOT/src/ui/synced.cpp"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"synced.cpp"* ]]
}

@test "drift below the baseline passes and offers the ratchet down" {
    printf '9 synced.cpp\n0 clone.cpp\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"ratchet down"* ]]
    [[ "$output" == *"synced.cpp"* ]]
}

# ----------------------------------------------------------- the seed tooling

@test "--write-baseline freezes today's counts so the gate then passes" {
    printf 'int a() { return 1; }\nint b() { return std::max(1u, x); }\nint c() { return 3; }\nint newly() { return 0; }\n' \
        > "$ROOT/src/ui/synced.cpp"
    run_gate
    [ "$status" -eq 1 ]
    run python3 "$GATE" --write-baseline \
        --overrides "$ROOT/overrides" \
        --src-root "$ROOT/src" \
        --baseline "$ROOT/base.txt"
    [ "$status" -eq 0 ]
    run_gate
    [ "$status" -eq 0 ]
}

# --------------------------------------------- the real tree (pins the port)
#
# The audit tree compiles src/ directly: an ESP-specific need is expressed in
# src/ behind a guard, or in the audit build's CMake, never as a forked copy.
# A fork cannot drift if it does not exist, so these pin the absence.

@test "the real overrides/ tree is within its committed baseline" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}

@test "no src/ file is forked into overrides/" {
    run bash -c 'ls firmware/native-audit/overrides/*.cpp 2>/dev/null | wc -l'
    [ "$output" -eq 0 ]
}

@test "every app_srcs.txt entry resolves to a file in the repo" {
    # Repointing a manifest line at the wrong src/ path drops the TU from the
    # audit silently: CMake globs what it is given and never sees the gap.
    run bash -c '
        miss=0
        while read -r rel; do
            case "$rel" in ""|\#*) continue ;; esac
            [ -f "$rel" ] || { echo "missing: $rel"; miss=1; }
        done < firmware/native-audit/components/helixapp/app_srcs.txt
        exit $miss'
    [ "$status" -eq 0 ]
}

@test "the #1414 coordinate clear reaches ESP32 through src/" {
    # A bare `.enabled = false` leaves stale col/row on disk. The audit build
    # now compiles this file directly, so the guard belongs here.
    run grep -c "disable_and_unplace" src/ui/grid_edit_mode.cpp
    [ "$status" -eq 0 ]
    [ "$output" -ge 1 ]
}
