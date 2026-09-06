#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_duplicate_xml_names.py — the duplicate widget
# `name=` gate.
#
# The gate exists because lv_obj_find_by_name() returns the FIRST depth-first
# match and warns about nothing (lib/lvgl/src/core/lv_obj_tree.c:631). Two
# elements sharing a name in one file means the second is built and then
# silently never configured. That was #1136: ams_panel.xml carried
# name="endless_arrows" twice.
#
# These tests pin both halves of the contract: the shape it must catch, and the
# idioms already all over ui_xml/ that it must stay quiet about. A gate that
# fires on <style>, on <if>/<else> branches, or on a `${i}`-indexed <repeat>
# body would be noise on every XML commit and would get switched off.

GATE="scripts/check_duplicate_xml_names.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/dup_names"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into a fixture .xml and run the gate over that file alone.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name.xml"
    run python3 "$GATE" "$FIXTURE_DIR/$name.xml"
}

# --- the shape that actually breaks the UI ---------------------------------

@test "flags the #1136 shape: one name on two elements" {
    run_gate endless_arrows '<?xml version="1.0"?>
<component>
  <view name="ams_panel" extends="lv_obj">
    <lv_obj name="left_column">
      <endless_spool_arrows name="endless_arrows" width="100%"/>
    </lv_obj>
    <lv_obj name="right_column">
      <endless_spool_arrows name="endless_arrows" width="50%"/>
    </lv_obj>
  </view>
</component>'
    [ "$status" -eq 1 ]
    contains 'endless_arrows' "$output"
    [[ "$output" == *'2 elements'* ]]
}

@test "flags a duplicate between sibling elements" {
    run_gate siblings '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_label name="status"/>
    <lv_label name="status"/>
  </view>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *'name="status"'* ]]
}

@test "flags a literal name inside a <repeat> body" {
    run_gate repeat_literal '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_obj name="rows">
      <repeat count="4">
        <ui_card name="slot_card"/>
      </repeat>
    </lv_obj>
  </view>
</component>'
    [ "$status" -eq 1 ]
    contains 'slot_card' "$output"
    [[ "$output" == *'<repeat>'* ]]
}

@test "reports every line of a duplicate so both sites are findable" {
    run_gate lines '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_label name="dup"/>
    <lv_obj>
      <lv_label name="dup"/>
    </lv_obj>
  </view>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *'lines 4, 6'* ]]
}

@test "the colon-suffixed LVGL state syntax does not blind the tokenizer" {
    # style_text_color:checked is an unbound namespace prefix — it makes every
    # strict XML parser bail, which is why the gate tokenizes by hand.
    run_gate colon_attrs '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <ui_button name="preset" style_bg_opa:checked="200" style_text_color:checked="#text"/>
    <ui_button name="preset" style_bg_opa:checked="100"/>
  </view>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *'name="preset"'* ]]
}

@test "flags a child that shadows the <view> root's own name" {
    # The <view> tag is parsed as whatever it extends, so its name= lands on the
    # root object — and 11 call sites search from lv_screen_active(), where the
    # root is reachable.
    run_gate view_root '<?xml version="1.0"?>
<component>
  <view name="wizard_subtitle" extends="lv_obj">
    <lv_label name="wizard_subtitle"/>
  </view>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *'wizard_subtitle'* ]]
}

# --- idioms already in ui_xml/ that must stay silent -----------------------

@test "accepts a file whose widget names are all unique" {
    run_gate clean '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_obj name="header">
      <lv_label name="title"/>
    </lv_obj>
    <lv_obj name="body">
      <lv_label name="subtitle"/>
    </lv_obj>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "accepts the same name in the two branches of <if>/<else>" {
    # Only one branch is ever materialized, so the pair cannot coexist.
    run_gate if_else '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <if cond="palette eq 1">
      <lv_obj name="swatches" flex_flow="row"/>
      <else/>
      <lv_obj name="swatches" flex_flow="column"/>
    </if>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "flags a duplicate inside a single <if> branch" {
    # The <if> exemption is per-branch, not a blanket pass for the subtree.
    run_gate if_same_branch '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <if cond="palette eq 1">
      <lv_obj name="swatches"/>
      <lv_obj name="swatches"/>
      <else/>
      <lv_obj name="other"/>
    </if>
  </view>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *'swatches'* ]]
}

@test "accepts a <repeat> body whose name carries the loop index" {
    run_gate repeat_indexed '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_obj name="rows">
      <repeat count="macro_row_count">
        <ui_card name="slot_${i}"/>
      </repeat>
    </lv_obj>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "accepts style declarations and applications sharing a name" {
    # <style> and <bind_style*> name a STYLE, not a widget — a different
    # namespace, and one that is meant to be referenced repeatedly.
    run_gate styles '<?xml version="1.0"?>
<component>
  <styles>
    <style name="card_style" bg_color="#card_bg"/>
  </styles>
  <view name="panel" extends="lv_obj">
    <lv_obj name="a">
      <style name="card_style"/>
      <bind_style name="card_style" subject="is_active" ref_value="1"/>
    </lv_obj>
    <lv_obj name="b">
      <style name="card_style"/>
      <bind_style_if_eq name="card_style" subject="is_active" ref_value="1"/>
    </lv_obj>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "accepts an <api> prop, a <const> and a <subject> named like a widget" {
    # Those live outside <view>; only the widget tree shares a name space.
    run_gate other_namespaces '<?xml version="1.0"?>
<component>
  <api>
    <prop name="value" type="string"/>
  </api>
  <consts>
    <px name="value" value="10"/>
  </consts>
  <subjects>
    <subject name="value" type="int" value="0"/>
  </subjects>
  <view name="panel" extends="lv_obj">
    <lv_label name="value"/>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "accepts param-interpolated names, which resolve per instantiation" {
    run_gate dollar_param '<?xml version="1.0"?>
<component>
  <view name="form_field" extends="lv_obj">
    <text_input name="$input_name"/>
    <status_pill name="$badge_name"/>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "accepts trailing-# names, which LVGL auto-indexes per sibling group" {
    run_gate auto_index '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_obj name="slot_#"/>
    <lv_obj name="slot_#"/>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "a duplicate name inside a comment is not a duplicate" {
    run_gate commented '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_label name="status"/>
    <!-- was: <lv_label name="status"/> -->
  </view>
</component>'
    [ "$status" -eq 0 ]
}

# --- opt-out ---------------------------------------------------------------

@test "DUPLICATE_NAME_OK on the element suppresses it" {
    run_gate opt_out_inline '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_label name="value"/>
    <lv_label name="value"/> <!-- DUPLICATE_NAME_OK: nothing looks this up -->
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "DUPLICATE_NAME_OK on the line above suppresses it" {
    run_gate opt_out_above '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_label name="value"/>
    <!-- DUPLICATE_NAME_OK: row-scoped lookup, never searched from the root -->
    <lv_label name="value"/>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "the opt-out is per-element, not per-file" {
    # Suppressing one of three leaves two that still collide.
    run_gate opt_out_partial '<?xml version="1.0"?>
<component>
  <view name="panel" extends="lv_obj">
    <lv_label name="value"/>
    <lv_label name="value"/> <!-- DUPLICATE_NAME_OK: deliberate -->
    <lv_label name="value"/>
  </view>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *'2 elements'* ]]
}

# --- the real tree ---------------------------------------------------------

@test "ui_xml/ is at or below the recorded baseline" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *'✅'* ]]
}

@test "--list enumerates the baselined sites and exits 0" {
    run python3 "$GATE" --list
    [ "$status" -eq 0 ]
    contains 'about_settings_overlay.xml' "$output"
    [[ "$output" == *'[baselined]'* ]]
}

@test "a name absent from the baseline fails even when its file is baselined" {
    # Guards the ratchet: the baseline is keyed per name, so about_settings_overlay
    # being listed must not license a NEW duplicate in that same file.
    run_gate baseline_scope '<?xml version="1.0"?>
<component>
  <view name="about_settings_overlay" extends="overlay_panel">
    <lv_label name="brand_new_dup"/>
    <lv_label name="brand_new_dup"/>
  </view>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *'brand_new_dup'* ]]
}
