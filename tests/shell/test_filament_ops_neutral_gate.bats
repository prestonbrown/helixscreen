#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/check_filament_ops_neutral.py: the filament-op surfaces ask capability
# questions and never which backend they are on.

load helpers

@test "filament-op code carries no backend-type branch" {
    run python3 scripts/check_filament_ops_neutral.py
    [ "$status" -eq 0 ] || fail "$output"
}

@test "the filament-op gate catches get_type() and AmsType:: in its scope" {
    local root="$BATS_TEST_TMPDIR/root"
    mkdir -p "$root/src/ui"
    printf 'bool a(AmsBackend* b) { return b->get_type() == x; }\n' > "$root/src/ui/filament/ui_ams_sidebar.cpp"
    printf 'auto t = AmsType::SNAPMAKER;\n' > "$root/src/ui/filament/filament_op_execute.cpp"
    run python3 scripts/check_filament_ops_neutral.py --root "$root"
    [ "$status" -eq 1 ] || fail "violations passed: $output"
    contains "ui_ams_sidebar.cpp:1" "$output"
    contains "filament_op_execute.cpp:1" "$output"
}

@test "the filament-op gate ignores comments and files outside its scope" {
    local root="$BATS_TEST_TMPDIR/quiet"
    mkdir -p "$root/src/ui/wizard"
    printf '// never ask get_type() here\n/* AmsType::AFC\n   AmsType::CFS */\nint x;\n' \
        > "$root/src/ui/filament/ui_ams_sidebar.cpp"
    printf 'auto t = AmsType::SNAPMAKER;\n' > "$root/src/ui/wizard/ui_wizard_ams_identify.cpp"
    run python3 scripts/check_filament_ops_neutral.py --root "$root"
    [ "$status" -eq 0 ] || fail "quiet cases reported: $output"
}
