#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The filament-ops-neutral rule of scripts/check_source_rules.py: the filament-op surfaces ask capability
# questions and never which backend they are on.

load helpers

# The verdict over the real tree is qc_filament_ops_neutral's; these cases pin
# the script on a fixture root that holds every file in its SCOPE, empty.
setup() {
    python3 -c 'import sys; sys.path.insert(0, "scripts"); import check_source_rules as g; print("\n".join(g.FILAMENT_OPS_SCOPE))' |
        while IFS= read -r rel; do
            mkdir -p "$BATS_TEST_TMPDIR/root/$(dirname "$rel")"
            : > "$BATS_TEST_TMPDIR/root/$rel"
        done
}

scope_file() { # <path under the root> <body>
    mkdir -p "$BATS_TEST_TMPDIR/root/$(dirname "$1")"
    printf '%s\n' "$2" > "$BATS_TEST_TMPDIR/root/$1"
}

@test "a get_type() call or an AmsType name in filament-op code fails, named by line" {
    scope_file src/ui/filament/ui_ams_sidebar.cpp $'void f() {\n    if (backend->get_type () == x) {}\n}'
    scope_file include/ui_panel_filament.h 'constexpr auto kType = AmsType::AFC;'
    run python3 scripts/check_source_rules.py --rule filament-ops-neutral --root "$BATS_TEST_TMPDIR/root"
    [ "$status" -eq 1 ]
    contains "src/ui/filament/ui_ams_sidebar.cpp:2:" "$output"
    contains "include/ui_panel_filament.h:1:" "$output"
    contains "2 backend-type reference(s)" "$output"
}

@test "comments, capability questions and files outside the scope pass" {
    scope_file src/ui/filament/ui_ams_sidebar.cpp $'// never call get_type() here\n/* nor AmsType::AFC\n   nor AmsType::CFS */\nbool ok = backend->head_filament_ops();'
    scope_file src/ui/ui_panel_ams.cpp 'if (backend->get_type() == AmsType::AFC) {}'
    run python3 scripts/check_source_rules.py --rule filament-ops-neutral --root "$BATS_TEST_TMPDIR/root"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "a scope file that no longer exists fails, naming it, instead of passing unchecked" {
    rm "$BATS_TEST_TMPDIR/root/src/ui/filament/ui_ams_sidebar.cpp"
    run python3 scripts/check_source_rules.py --rule filament-ops-neutral --root "$BATS_TEST_TMPDIR/root"
    [ "$status" -eq 1 ]
    contains "src/ui/filament/ui_ams_sidebar.cpp" "$output"
    contains "SCOPE" "$output"
}
