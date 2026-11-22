#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/check_header_actions.py: a header's actions slot holds header_action and
# nothing else, and a header_action leaves its look to the component.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR}/tree"
    mkdir -p "$ROOT/ui_xml/components"
}

screen() {
    printf '%s\n' "<component><view name=\"screen\" extends=\"lv_obj\">" \
        "  <header_bar title=\"T\"><header_bar-actions>" \
        "$1" \
        "  </header_bar-actions></header_bar>" \
        "</view></component>" > "$ROOT/ui_xml/${2:-screen}.xml"
}

@test "the gate passes header_actions with bindings, and a binding on the slot itself" {
    screen '<bind_flag_if cond="ui_breakpoint lt 2" flag="hidden"/>
      <header_action name="btn_save" text="Save" role="primary" callback="on_save">
        <bind_state_if_eq subject="dirty" state="disabled" ref_value="0"/>
      </header_action>
      <header_action name="btn_more" icon="dots_vertical" callback="on_more"/>'
    run python3 scripts/check_header_actions.py --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$output" != *warning* ]] || fail "$output"
}

@test "the gate fires on a hand-built button in the slot" {
    screen '<ui_button name="btn_refresh" icon="refresh"/>'
    run python3 scripts/check_header_actions.py --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "<ui_button> in <header_bar-actions>" "$output"
}

@test "the gate fires on a wrapper in an overlay_panel slot, in a components file" {
    printf '%s\n' '<component><view extends="overlay_panel"><overlay_panel-actions>' \
        '<lv_obj name="row"><header_action icon="plus" callback="x"/></lv_obj>' \
        '</overlay_panel-actions></view></component>' > "$ROOT/ui_xml/components/editor.xml"
    run python3 scripts/check_header_actions.py --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "ui_xml/components/editor.xml:2: <lv_obj> in <overlay_panel-actions>" "$output"
}

@test "the gate fires on a header_action given a size, an unknown role or no content" {
    screen '<header_action name="btn_a" text="A" height="40" callback="a"/>
      <header_action name="btn_b" text="B" role="secondary" callback="b"/>
      <header_action name="btn_c" collapse="sometimes" callback="c"/>'
    run python3 scripts/check_header_actions.py --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "btn_a sets height=" "$output"
    contains "btn_b role=\"secondary\"" "$output"
    contains "btn_c collapse=\"sometimes\"" "$output"
    contains "btn_c has neither icon nor text" "$output"
}

@test "the gate fires on a widget inside a header_action, outside a slot too" {
    printf '%s\n' '<component><view extends="lv_obj">' \
        '<header_action name="btn_gear" icon="settings" callback="g"><lv_label text="x"/></header_action>' \
        '</view></component>' > "$ROOT/ui_xml/custom_header.xml"
    run python3 scripts/check_header_actions.py --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "<lv_label> inside <header_action>" "$output"
}

@test "the gate warns, and passes, on four actions or two primaries" {
    screen '<header_action icon="a" callback="a" role="primary"/>
      <header_action icon="b" callback="b" role="primary"/>
      <header_action icon="c" callback="c"/>
      <header_action icon="d" callback="d"/>'
    run python3 scripts/check_header_actions.py --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
    contains "has 4 actions" "$output"
    contains "has 2 primary actions" "$output"
}

@test "the gate ignores markup in comments" {
    screen '<!-- <ui_button name="old"/> -->
      <header_action icon="refresh" callback="r"/>'
    run python3 scripts/check_header_actions.py --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "quality-checks runs the gate on XML and on the checker itself" {
    contains "qc_header_actions" "$(grep '^QC_ALL=' scripts/quality-checks.sh)"
    trigger="$(qc_trigger header_actions)"
    [[ "ui_xml/spoolman_panel.xml" =~ $trigger ]] || fail "XML does not trigger the gate"
    [[ "scripts/check_header_actions.py" =~ $trigger ]] || fail "the checker does not trigger the gate"
    ! [[ "src/ui/ui_nav.cpp" =~ $trigger ]]
}
