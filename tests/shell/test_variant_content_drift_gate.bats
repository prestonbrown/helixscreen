#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_variant_content_drift.py — the variant/base
# content-drift gate.
#
# check_variant_parity.py compares WIRING between a ui_xml/<variant>/ override
# and its base — widget name=, subject bindings, event callbacks, <api> props —
# and deliberately not attributes, because reflow is the entire point of a
# variant. That leaves a gap: bump a font, swap an icon src=, or edit a
# translation_tag= in the base and forget the variant, and every existing gate
# passes. The two screens slowly stop saying the same thing.
#
# This gate closes that gap, but only for CONTENT-bearing attributes (src=,
# text=, translation_tag=) touched by the STAGED diff of one half of a pair
# while its sibling is not staged. It warns and never fails: additive
# divergence is legitimate (ui_xml/portrait/print_status_panel.xml carries
# three icons the base does not, because portrait has room for a temperature
# section landscape lacks), and a hard gate cannot tell that apart from rot.
#
# These tests pin both halves of the contract. The silent half matters as much
# as the loud half — a gate that fires on a legitimate landscape-only padding
# tweak is exactly the kind of gate this project's own philosophy says gets
# switched off.

load helpers

REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
GATE="$REPO_ROOT/scripts/check_variant_content_drift.py"

setup() {
    FIXTURE_DIR="$(mktemp -d "${BATS_TEST_TMPDIR:-${BATS_TMPDIR:-/tmp}}/content_drift-XXXXXX")"
    cd "$FIXTURE_DIR" || return 1
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test"
    mkdir -p ui_xml/portrait ui_xml/components ui_xml/translations
}

# Commit whatever is currently on disk as the "before" state, so the next edit
# produces a real staged diff instead of a from-nothing add.
commit_baseline() {
    git add -A
    git commit -q -m baseline
}

# ------------------------------------------------------- must warn (the rot)

@test "warns when a staged src= change in the base has no staged variant" {
    printf '<component>\n  <view name="x" extends="lv_obj">\n    <lv_image src="icon_fridge_industrial"/>\n  </view>\n</component>\n' > ui_xml/print_status_panel.xml
    printf '<component>\n  <view name="x" extends="lv_obj">\n    <lv_image src="icon_fridge_industrial"/>\n  </view>\n</component>\n' > ui_xml/portrait/print_status_panel.xml
    commit_baseline

    # One-character src= change in the base only.
    printf '<component>\n  <view name="x" extends="lv_obj">\n    <lv_image src="icon_fridge_industria2"/>\n  </view>\n</component>\n' > ui_xml/print_status_panel.xml
    git add ui_xml/print_status_panel.xml

    run "$GATE"
    contains "ui_xml/print_status_panel.xml" "$output"
    contains "ui_xml/portrait/print_status_panel.xml" "$output"
    [[ "$output" == *"src="* ]]
}

@test "warns when a staged translation_tag= change in the base has no staged variant" {
    printf '<component>\n  <view name="x" extends="lv_obj">\n    <text_body translation_tag="preparing_operation"/>\n  </view>\n</component>\n' > ui_xml/print_status_panel.xml
    printf '<component>\n  <view name="x" extends="lv_obj">\n    <text_body translation_tag="preparing_operation"/>\n  </view>\n</component>\n' > ui_xml/portrait/print_status_panel.xml
    commit_baseline

    printf '<component>\n  <view name="x" extends="lv_obj">\n    <text_body translation_tag="preparing_print"/>\n  </view>\n</component>\n' > ui_xml/print_status_panel.xml
    git add ui_xml/print_status_panel.xml

    run "$GATE"
    contains "ui_xml/print_status_panel.xml" "$output"
    contains "ui_xml/portrait/print_status_panel.xml" "$output"
    [[ "$output" == *"translation_tag="* ]]
}

@test "warns when the staged half is the variant and the base is untouched" {
    # Drift is symmetric: forgetting to update the base after editing the
    # variant is the same rot in the other direction.
    printf '<component>\n  <view name="x" extends="lv_obj">\n    <lv_image src="icon_a"/>\n  </view>\n</component>\n' > ui_xml/print_status_panel.xml
    printf '<component>\n  <view name="x" extends="lv_obj">\n    <lv_image src="icon_a"/>\n  </view>\n</component>\n' > ui_xml/portrait/print_status_panel.xml
    commit_baseline

    printf '<component>\n  <view name="x" extends="lv_obj">\n    <lv_image src="icon_b"/>\n  </view>\n</component>\n' > ui_xml/portrait/print_status_panel.xml
    git add ui_xml/portrait/print_status_panel.xml

    run "$GATE"
    contains "ui_xml/portrait/print_status_panel.xml" "$output"
    [[ "$output" == *"ui_xml/print_status_panel.xml"* ]]
}

@test "never fails the build, even when it warns" {
    printf '<component><view name="x" extends="lv_obj"><lv_image src="icon_a"/></view></component>\n' > ui_xml/print_status_panel.xml
    printf '<component><view name="x" extends="lv_obj"><lv_image src="icon_a"/></view></component>\n' > ui_xml/portrait/print_status_panel.xml
    commit_baseline

    printf '<component><view name="x" extends="lv_obj"><lv_image src="icon_z"/></view></component>\n' > ui_xml/print_status_panel.xml
    git add ui_xml/print_status_panel.xml

    run "$GATE"
    [ "$status" -eq 0 ]
}

# ------------------------------------------------ must stay silent (reflow)

@test "silent when only style_pad_all/width/flex_flow change in the base" {
    printf '<component>\n  <view name="x" extends="lv_obj" width="200" style_pad_all="4" flex_flow="row"/>\n</component>\n' > ui_xml/print_status_panel.xml
    printf '<component>\n  <view name="x" extends="lv_obj" width="100" style_pad_all="8" flex_flow="column"/>\n</component>\n' > ui_xml/portrait/print_status_panel.xml
    commit_baseline

    printf '<component>\n  <view name="x" extends="lv_obj" width="240" style_pad_all="6" flex_flow="column"/>\n</component>\n' > ui_xml/print_status_panel.xml
    git add ui_xml/print_status_panel.xml

    run "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" != *"⚠"* ]]
}

@test "silent when base and variant are staged together" {
    printf '<component><lv_image src="icon_a"/></component>\n' > ui_xml/print_status_panel.xml
    printf '<component><lv_image src="icon_a"/></component>\n' > ui_xml/portrait/print_status_panel.xml
    commit_baseline

    printf '<component><lv_image src="icon_b"/></component>\n' > ui_xml/print_status_panel.xml
    printf '<component><lv_image src="icon_b"/></component>\n' > ui_xml/portrait/print_status_panel.xml
    git add ui_xml/print_status_panel.xml ui_xml/portrait/print_status_panel.xml

    run "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" != *"⚠"* ]]
}

@test "silent on a file with no variant sibling at all" {
    printf '<component><lv_image src="icon_a"/></component>\n' > ui_xml/lonely_panel.xml
    commit_baseline

    printf '<component><lv_image src="icon_b"/></component>\n' > ui_xml/lonely_panel.xml
    git add ui_xml/lonely_panel.xml

    run "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" != *"⚠"* ]]
}

@test "silent on a staged file under ui_xml/components/" {
    printf '<component><lv_image src="icon_a"/></component>\n' > ui_xml/components/some_card.xml
    commit_baseline

    printf '<component><lv_image src="icon_b"/></component>\n' > ui_xml/components/some_card.xml
    git add ui_xml/components/some_card.xml

    run "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" != *"⚠"* ]]
}

@test "silent on a staged file under ui_xml/translations/" {
    printf '<strings><s k="a" v="hello"/></strings>\n' > ui_xml/translations/en.xml
    commit_baseline

    printf '<strings><s k="a" v="goodbye"/></strings>\n' > ui_xml/translations/en.xml
    git add ui_xml/translations/en.xml

    run "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" != *"⚠"* ]]
}

@test "silent when nothing at all is staged" {
    printf '<component><lv_image src="icon_a"/></component>\n' > ui_xml/print_status_panel.xml
    printf '<component><lv_image src="icon_a"/></component>\n' > ui_xml/portrait/print_status_panel.xml
    commit_baseline

    run "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" != *"⚠"* ]]
}

# -------------------------------------------------------------- the real tree

@test "the real repo tree is silent with nothing staged" {
    cd "$REPO_ROOT" || return 1
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" != *"⚠"* ]]
}
