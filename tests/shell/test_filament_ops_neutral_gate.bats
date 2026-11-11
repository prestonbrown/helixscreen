#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/check_filament_ops_neutral.py: the filament-op surfaces ask capability
# questions and never which backend they are on.

load helpers

# The verdict over the real tree is qc_filament_ops_neutral's; these cases pin
# the script on a fixture root.
scope_file() { # <path under the root> <body>
    mkdir -p "$BATS_TEST_TMPDIR/root/$(dirname "$1")"
    printf '%s\n' "$2" > "$BATS_TEST_TMPDIR/root/$1"
}

@test "a get_type() call or an AmsType name in filament-op code fails, named by line" {
    scope_file src/ui/filament/ui_ams_sidebar.cpp $'void f() {\n    if (backend->get_type () == x) {}\n}'
    scope_file include/ui_panel_filament.h 'constexpr auto kType = AmsType::AFC;'
    run python3 scripts/check_filament_ops_neutral.py --root "$BATS_TEST_TMPDIR/root"
    [ "$status" -eq 1 ]
    contains "src/ui/filament/ui_ams_sidebar.cpp:2:" "$output"
    contains "include/ui_panel_filament.h:1:" "$output"
    contains "2 backend-type reference(s)" "$output"
}

@test "comments, capability questions and files outside the scope pass" {
    scope_file src/ui/filament/ui_ams_sidebar.cpp $'// never call get_type() here\n/* nor AmsType::AFC */\nbool ok = backend->head_filament_ops();'
    scope_file src/ui/ui_panel_ams.cpp 'if (backend->get_type() == AmsType::AFC) {}'
    run python3 scripts/check_filament_ops_neutral.py --root "$BATS_TEST_TMPDIR/root"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "the filament-op gate catches get_type() and AmsType:: in its scope" {
    local root="$BATS_TEST_TMPDIR/root"
    mkdir -p "$root/src/ui" "$root/src/ui/filament" "$root/src/ui/wizard"
    printf 'bool a(AmsBackend* b) { return b->get_type() == x; }\n' > "$root/src/ui/filament/ui_ams_sidebar.cpp"
    printf 'auto t = AmsType::SNAPMAKER;\n' > "$root/src/ui/filament/filament_op_execute.cpp"
    run python3 scripts/check_filament_ops_neutral.py --root "$root"
    [ "$status" -eq 1 ] || fail "violations passed: $output"
    contains "ui_ams_sidebar.cpp:1" "$output"
    contains "filament_op_execute.cpp:1" "$output"
}

@test "the filament-op gate ignores comments and files outside its scope" {
    local root="$BATS_TEST_TMPDIR/quiet"
    mkdir -p "$root/src/ui/wizard" "$root/src/ui/filament"
    printf '// never ask get_type() here\n/* AmsType::AFC\n   AmsType::CFS */\nint x;\n' \
        > "$root/src/ui/filament/ui_ams_sidebar.cpp"
    printf 'auto t = AmsType::SNAPMAKER;\n' > "$root/src/ui/wizard/ui_wizard_ams_identify.cpp"
    run python3 scripts/check_filament_ops_neutral.py --root "$root"
    [ "$status" -eq 0 ] || fail "quiet cases reported: $output"
}
