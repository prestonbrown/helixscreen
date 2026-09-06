#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_responsive_token_scope.py — the responsive-token
# location gate.
#
# theme_manager_find_xml_files() skips subdirectories outright
# (src/ui/theme_manager.cpp: `if (entry->d_type == DT_DIR) continue;`), so token
# auto-discovery reads only the top-level ui_xml/*.xml. A responsive token
# declared in ui_xml/components/, ui_xml/portrait/, ui_xml/micro/ — anywhere but
# the top level — is never registered, and every `#token` referencing it
# silently resolves to nothing. No warning fires on either side (#1211).
#
# Recursing was the obvious fix and is the wrong one: discovery is last-wins by
# alphabetical order, so a portrait-only token would start shadowing its base
# token globally rather than only while that variant is active. Discovery stays
# top-level-only and this gate makes the constraint loud.
#
# These tests pin both halves. The silent half matters as much as the loud one:
# component-local <consts> that are NOT responsive are an established idiom in
# ui_xml/components/ (grid_gap, pin_dot_size, tray_default_height) and resolve
# fine through the component's own scope. A gate that fired on those would be
# noise on every component commit and would get switched off.

load helpers

GATE="scripts/check_responsive_token_scope.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/token_scope"
    mkdir -p "$FIXTURE_DIR"
}

# Write $3 into $FIXTURE_DIR/ui_xml/$1/$2.xml and run the gate over that file.
# $1 may be "." for the top level.
run_gate() {
    local subdir="$1" name="$2" body="$3"
    mkdir -p "$FIXTURE_DIR/ui_xml/$subdir"
    printf '%s\n' "$body" > "$FIXTURE_DIR/ui_xml/$subdir/$name.xml"
    run python3 "$GATE" "$FIXTURE_DIR/ui_xml/$subdir/$name.xml"
}

# ------------------------------------------------- the shape that breaks the UI

@test "flags a <px> responsive token declared in ui_xml/components/" {
    run_gate components card '<component>
  <consts>
    <px name="card_gutter_small" value="8"/>
  </consts>
  <view name="card" extends="lv_obj"/>
</component>'
    [ "$status" -eq 1 ]
    contains "card_gutter_small" "$output"
    contains "card.xml" "$output"
    [[ "$output" == *"globals.xml"* ]]
}

@test "flags a <string> responsive token declared in a layout variant dir" {
    run_gate portrait navigation_bar '<component>
  <consts>
    <string name="nav_label_medium" value="Print"/>
  </consts>
  <view name="navigation_bar" extends="lv_obj"/>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"nav_label_medium"* ]]
}

@test "flags every responsive suffix, including the optional tiers" {
    for suffix in micro tiny small medium large xlarge xxlarge; do
        run_gate micro "tok_$suffix" "<component>
  <consts>
    <px name=\"thing_$suffix\" value=\"4\"/>
  </consts>
</component>"
        [ "$status" -eq 1 ]
        [[ "$output" == *"thing_$suffix"* ]]
    done
}

@test "flags a token nested deeper than one level under ui_xml/" {
    run_gate components/nested widget '<component>
  <consts>
    <px name="widget_pad_large" value="16"/>
  </consts>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"widget_pad_large"* ]]
}

@test "reports the line so the declaration is findable" {
    run_gate micro_portrait lines '<component>
  <consts>
    <px name="row_height_small" value="24"/>
  </consts>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"lines.xml:3"* ]]
}

@test "explains why, not just that" {
    run_gate tiny_portrait why '<component>
  <consts>
    <px name="pad_small" value="2"/>
  </consts>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"top-level"* ]]
}

# ---------------------------------------------- idioms that must stay silent

@test "silent on a responsive token at the top level of ui_xml/" {
    # This is where they belong — globals.xml and the per-feature token files
    # (ams_tokens.xml, fan_tokens.xml) all sit at the top level.
    run_gate . globals '<component>
  <consts>
    <px name="space_lg_small" value="12"/>
    <px name="space_lg_medium" value="16"/>
    <px name="space_lg_large" value="20"/>
  </consts>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on a component-local const with no responsive suffix" {
    # ui_xml/components/color_swatch_grid.xml and friends do exactly this; those
    # resolve through the component's own <consts> scope, not auto-discovery.
    run_gate components swatch_grid '<component>
  <consts>
    <px name="grid_gap" value="6"/>
    <px name="grid_swatch_size" value="40"/>
  </consts>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on a name that merely contains a tier word" {
    run_gate components nearmiss '<component>
  <consts>
    <px name="smallish_pad" value="4"/>
    <px name="largest_gap" value="8"/>
    <px name="small_print_row" value="9"/>
  </consts>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on a widget name ending in a tier word" {
    # name= on a widget is a lookup handle, not a token declaration.
    run_gate micro widgets '<component>
  <view name="panel" extends="lv_obj">
    <lv_label name="status_small"/>
    <text_small name="hint_large"/>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on a reference to a responsive token from a subdirectory" {
    # Referencing #space_lg from a variant file is the whole point; only the
    # DECLARATION has to live at the top level.
    run_gate portrait consumer '<component>
  <view name="panel" extends="lv_obj" style_pad_all="#space_lg"
        height="#row_height_small"/>
</component>'
    [ "$status" -eq 0 ]
}

@test "a declaration inside a comment is not a declaration" {
    run_gate components commented '<component>
  <consts>
    <!-- <px name="old_pad_small" value="4"/> -->
    <px name="pad" value="4"/>
  </consts>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on an <api> prop whose name ends in a tier word" {
    run_gate components api_prop '<component>
  <api>
    <prop name="font_small" type="string"/>
  </api>
</component>'
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------- the real tree

@test "the committed ui_xml/ tree is clean" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}

@test "the whole-tree run says what it checked" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"✅"* || "$output" == *"✓"* ]]
}
