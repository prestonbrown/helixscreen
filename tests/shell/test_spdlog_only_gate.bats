#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/qc/spdlog_only.sh and the private-LVGL-API half of
# scripts/qc/design_tokens.sh: src/ and include/ log through spdlog, never
# printf/cout/cerr/LV_LOG_*, and call no _lv_* internal. Each case builds a
# scratch tree and runs the gate function there.

load helpers

setup() {
    REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    ROOT="$BATS_TEST_TMPDIR/tree"
    mkdir -p "$ROOT/src/ui" "$ROOT/include" "$ROOT/scripts"
}

# run_gate <gate file stem> <function>
run_gate() {
    run bash -c 'cd "$1" && STAGED_ONLY=false && . "$2/scripts/qc/_lib.sh" && . "$2/scripts/qc/$3.sh" && "$4"' \
        _ "$ROOT" "$REPO" "$1" "$2"
}

@test "spdlog_only reports printf, std::cout, std::cerr and LV_LOG_ calls" {
    cat > "$ROOT/src/ui/bad.cpp" <<'CPP'
void f() {
    printf("x\n");
    std::cout << 1;
    std::cerr << 2;
    LV_LOG_WARN("w");
}
CPP
    run_gate spdlog_only qc_spdlog_only
    [ "$status" -eq 1 ]
    contains "src/ui/bad.cpp:2:" "$output"
    contains "src/ui/bad.cpp:3:" "$output"
    contains "src/ui/bad.cpp:4:" "$output"
    contains "src/ui/bad.cpp:5:" "$output"
}

@test "spdlog_only passes spdlog calls, snprintf and an allowlisted CLI file" {
    cat > "$ROOT/src/ui/good.cpp" <<'CPP'
void f() {
    spdlog::info("x {}", 1);
    char b[8];
    snprintf(b, sizeof b, "%d", 1);
}
CPP
    mkdir -p "$ROOT/src/system"
    printf 'void g() { printf("usage\\n"); }\n' > "$ROOT/src/system/cli_args.cpp"
    run_gate spdlog_only qc_spdlog_only
    [ "$status" -eq 0 ]
}

@test "design_tokens reports a private _lv_ call, except in the remote control server" {
    mkdir -p "$ROOT/src/remote"
    printf 'void f() { _lv_obj_foo(nullptr); }\n' > "$ROOT/src/ui/priv.cpp"
    printf 'void g() { _lv_xml_walk(nullptr); }\n' > "$ROOT/src/remote/remote_control_server.cpp"
    run_gate design_tokens qc_design_tokens
    [ "$status" -eq 1 ]
    contains "src/ui/priv.cpp" "$output"
    contains "Private LVGL API" "$output"
    [ "${output#*remote_control_server}" = "$output" ]
}

@test "both gates are in the commit hook's list" {
    run grep -qE 'QC_ALL=.*qc_spdlog_only.*qc_design_tokens|QC_ALL=.*qc_design_tokens.*qc_spdlog_only' "$REPO/scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
}
