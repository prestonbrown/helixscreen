#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_variant_parity.py — the variant/base wiring
# parity gate. It FAILS the build, so the silent half of the contract matters
# as much as the loud half: a false positive on legitimate reflow is the defect
# that gets a gate like this switched off.
#
# Wiring means: widget names, subject bindings, event callbacks, <api> <prop>
# declarations, and cond=/[*]_cond= attribute presence. Props are compared by
# NAME ONLY — a variant may legitimately narrow a width floor or pick a
# smaller font for its tier, so a defaults comparison would fail on correct
# code. Conditions are compared by PRESENCE
# keyed element@attribute; the expression text is free to differ.
#
# The script roots itself at its own location (Path(__file__).parent.parent)
# rather than the CWD or git, so each case copies it into the fixture tree and
# runs the copy against the fixture's ui_xml/. The copy is made from this repo
# at run time, so it is always the version under test.

load helpers

REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
GATE="$REPO_ROOT/scripts/check_variant_parity.py"

setup() {
    FIXTURE_DIR="$(mktemp -d "${BATS_TEST_TMPDIR:-${BATS_TMPDIR:-/tmp}}/parity-XXXXXX")"
    mkdir -p "$FIXTURE_DIR/scripts" "$FIXTURE_DIR/ui_xml/micro"
    cp "$GATE" "$FIXTURE_DIR/scripts/check_variant_parity.py"
}

# Write a base/variant pair of ui_xml/demo_panel.xml. The filename matches no
# ALLOWED_OMISSIONS entry, so nothing is exempted behind the test's back.
write_pair() {  # write_pair <base-body> <variant-body>
    printf '%s\n' "$1" > "$FIXTURE_DIR/ui_xml/demo_panel.xml"
    printf '%s\n' "$2" > "$FIXTURE_DIR/ui_xml/micro/demo_panel.xml"
}

run_gate() {
    run python3 "$FIXTURE_DIR/scripts/check_variant_parity.py"
}

# ------------------------------------------------------- must fail (the rot)

@test "fails when a prop exists in the base and not the variant" {
    write_pair \
        '<component><api><prop name="title" type="string"/><prop name="z_clamp_max" type="string" default="0"/></api></component>' \
        '<component><api><prop name="title" type="string"/></api></component>'
    run_gate
    [ "$status" -eq 1 ]
    contains "missing api prop: z_clamp_max" "$output"
}

@test "fails when a cond= exists in the base and not the variant" {
    write_pair \
        '<component><view name="root" extends="lv_obj"><lv_image name="wifi_icon" src="icon_wifi" cond="network_up"/><lv_button name="calibrate" disabled_cond="busy"/></view></component>' \
        '<component><view name="root" extends="lv_obj"><lv_image name="wifi_icon" src="icon_wifi"/><lv_button name="calibrate" disabled_cond="busy"/></view></component>'
    run_gate
    [ "$status" -eq 1 ]
    contains "missing condition: lv_image@cond" "$output"
    # disabled_cond is present on both sides, so it must not be reported.
    [[ "$output" != *"lv_button@disabled_cond"* ]]
}

# ------------------------------------------- must stay silent (legitimate)

@test "silent when the variant carries a different default for the same prop" {
    # The false-positive case this gate must never fire on: the micro header
    # narrows action_button_2_min_width to 72 where the base sets 90.
    write_pair \
        '<component><api><prop name="action_button_2_min_width" type="string" default="90"/></api></component>' \
        '<component><api><prop name="action_button_2_min_width" type="string" default="72"/></api></component>'
    run_gate
    [ "$status" -eq 0 ]
    contains "1 override(s)" "$output"
}

@test "silent when a variant differs only in sizes, padding, flex and text" {
    # secondary_text contains "cond" as a substring (se-COND-ary) while being
    # plain content, so a variant dropping it must not read as a lost
    # condition — only attr == "cond" or a *_cond suffix is one.
    write_pair \
        '<component><view name="root" extends="lv_obj" width="200" style_pad_all="4" flex_flow="row"><lv_label name="status" bind_text="printer_status" secondary_text="Hello"/></view></component>' \
        '<component><view name="root" extends="lv_obj" width="100" style_pad_all="8" flex_flow="column"><lv_label name="status" bind_text="printer_status"/></view></component>'
    run_gate
    [ "$status" -eq 0 ]
    contains "1 override(s)" "$output"
}

@test "silent when base and variant agree" {
    write_pair \
        '<component><api><prop name="title" type="string"/></api><view name="root" extends="lv_obj"><lv_label name="status" bind_text="printer_status" cond="online"/><lv_button name="go" clicked_callback="on_go" hidden_cond="offline"/></view></component>' \
        '<component><api><prop name="title" type="string"/></api><view name="root" extends="lv_obj"><lv_label name="status" bind_text="printer_status" cond="online"/><lv_button name="go" clicked_callback="on_go" hidden_cond="offline"/></view></component>'
    run_gate
    [ "$status" -eq 0 ]
    contains "1 override(s)" "$output"
}

@test "silent on a file with no variant sibling" {
    printf '<component><view name="root" extends="lv_obj"><lv_label name="status" bind_text="printer_status" cond="online"/></view></component>\n' \
        > "$FIXTURE_DIR/ui_xml/lonely_panel.xml"
    run_gate
    [ "$status" -eq 0 ]
    contains "0 override(s)" "$output"
}
