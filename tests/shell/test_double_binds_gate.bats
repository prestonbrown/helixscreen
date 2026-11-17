#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for `scripts/scan_double_binds.py --check`, gated as qc_double_binds:
# tests/unit/double_bind_widgets.inc must be what ui_xml/ generates. A widget
# gaining a second binding, a hand edit of the table, and a regenerated but
# unstaged table each fail with the regenerate command; a regenerated commit
# and unstaged working-tree edits pass.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"
SCAN="$REPO_ROOT/scripts/scan_double_binds.py"
INC="tests/unit/double_bind_widgets.inc"

setup() {
    FIX="$BATS_TEST_TMPDIR/binds"
    mkdir -p "$FIX/ui_xml" "$FIX/tests/unit"
    cd "$FIX" || return 1
    git init -q .
    git config user.email t@example.com
    git config user.name tester
    cat > ui_xml/panel.xml <<'XML'
<component><view>
  <lv_button name="go_btn">
    <bind_state_if_eq subject="busy" state="disabled" ref_value="1"/>
    <bind_state_if_eq subject="offline" state="disabled" ref_value="1"/>
  </lv_button>
  <lv_button name="stop_btn">
    <bind_state_if_eq subject="busy" state="disabled" ref_value="0"/>
  </lv_button>
</view></component>
XML
    python3 "$SCAN" --emit-cpp >/dev/null
    git add -A
    git commit -qm base --no-verify
}

@test "the real tree's table is current" {
    cd "$REPO_ROOT"
    run python3 "$SCAN" --check
    [ "$status" -eq 0 ]
    contains "matches ui_xml/" "$output"
}

@test "an unchanged tree passes, staged and working" {
    run python3 "$SCAN" --check
    [ "$status" -eq 0 ]
    contains "1 double-bind groups" "$output"
    run python3 "$SCAN" --check --staged-only
    [ "$status" -eq 0 ]
}

@test "a widget gaining a second binding without regenerating fails and names the command" {
    sed -i 's|<bind_state_if_eq subject="busy" state="disabled" ref_value="0"/>|&\n    <bind_state_if_eq subject="homing" state="disabled" ref_value="1"/>|' ui_xml/panel.xml
    git add ui_xml/panel.xml
    run python3 "$SCAN" --check --staged-only
    [ "$status" -eq 1 ]
    contains '+    {"panel", "stop_btn"' "$output"
    contains "scan_double_binds.py --emit-cpp" "$output"
}

@test "a hand-edited table fails" {
    sed -i 's|"go_btn"|"go_button"|' "$INC"
    run python3 "$SCAN" --check
    [ "$status" -eq 1 ]
    contains '-    {"panel", "go_button"' "$output"
}

@test "a regenerated but unstaged table still fails the staged check" {
    sed -i 's|<bind_state_if_eq subject="offline" state="disabled" ref_value="1"/>||' ui_xml/panel.xml
    git add ui_xml/panel.xml
    python3 "$SCAN" --emit-cpp >/dev/null
    run python3 "$SCAN" --check
    [ "$status" -eq 0 ]
    run python3 "$SCAN" --check --staged-only
    [ "$status" -eq 1 ]
    git add "$INC"
    run python3 "$SCAN" --check --staged-only
    [ "$status" -eq 0 ]
}

@test "unstaged working-tree edits do not decide the staged verdict" {
    echo "garbage" > "$INC"
    run python3 "$SCAN" --check --staged-only
    [ "$status" -eq 0 ]
}

@test "the gate triggers on ui_xml, the table and the scanner" {
    cd "$REPO_ROOT"
    re="$(qc_trigger double_binds)"
    [ -n "$re" ]
    for p in ui_xml/home_panel.xml ui_xml/components/x.xml "$INC" scripts/scan_double_binds.py; do
        printf '%s\n' "$p" | grep -qE "$re" || fail "trigger misses $p"
    done
    run grep -qE "$re" <<< "src/ui/ui_panel_home.cpp"
    [ "$status" -eq 1 ]
}
