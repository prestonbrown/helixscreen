#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_shipped_spacing_tokens.py — the gate on spacing
# tokens that shipping code reads and a packaged release does not carry.
#
# theme_manager_get_spacing("foo") resolves a const registered from the
# foo_small/foo_medium/foo_large triplet found in the top level of ui_xml/. When
# the const is absent lv_xml_get_const_silent() returns nullptr, get_spacing
# returns 0, and the caller takes its hardcoded fallback with no warning on
# either side.
#
# release-copy-xml-config (mk/cross.mk) stages ui_xml/ and then deletes the
# DEV_PANEL_XML files, so a token declared only in a dev-only panel is present
# in the development tree and absent on every device. The failure is invisible
# in development, and invisible on the tier whose value happens to equal the
# fallback.
#
# The silent half matters as much as the loud one: border_radius and
# button_height come from C++ tables per breakpoint and have no XML declaration
# at all. A gate that demanded XML for those would fire on every release and get
# switched off.

load helpers

GATE="scripts/check_shipped_spacing_tokens.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/shipped_tokens"
    mkdir -p "$FIXTURE/ui_xml" "$FIXTURE/src" "$FIXTURE/mk"
    printf 'DEV_PANEL_XML := step_test_panel.xml test_panel.xml\n' > "$FIXTURE/mk/cross.mk"
}

# Write $2 into $FIXTURE/ui_xml/$1.xml.
xml() {
    printf '%s\n' "$2" > "$FIXTURE/ui_xml/$1.xml"
}

# Write $2 into $FIXTURE/src/$1.cpp.
cpp() {
    printf '%s\n' "$2" > "$FIXTURE/src/$1.cpp"
}

run_gate() {
    run python3 "$GATE" --ui-xml "$FIXTURE/ui_xml" --src "$FIXTURE/src" \
        --cross-mk "$FIXTURE/mk/cross.mk"
}

# ------------------------------------------------ the shape that ships broken

@test "flags a triplet declared only in a dev-only panel" {
    xml step_test_panel '<component>
  <consts>
    <px name="step_indicator_small" value="16"/>
    <px name="step_indicator_medium" value="20"/>
    <px name="step_indicator_large" value="24"/>
  </consts>
</component>'
    cpp widget 'int32_t s = theme_manager_get_spacing("step_indicator");'
    run_gate
    [ "$status" -eq 1 ]
    contains "step_indicator_small" "$output"
    [[ "$output" == *"step_test_panel.xml"* ]]
}

@test "names the calling site so the affected widget is findable" {
    xml test_panel '<component>
  <consts>
    <px name="gizmo_small" value="4"/>
    <px name="gizmo_medium" value="6"/>
    <px name="gizmo_large" value="8"/>
  </consts>
</component>'
    cpp gizmo '// leading line
int32_t g = theme_manager_get_spacing("gizmo");'
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"gizmo.cpp:2"* ]]
}

@test "flags a single tier stranded in a dev-only panel" {
    # The other two tiers shipping does not save the token: the release loses
    # _large and every large panel silently falls back.
    xml globals '<component>
  <consts>
    <px name="gizmo_small" value="4"/>
    <px name="gizmo_medium" value="6"/>
  </consts>
</component>'
    xml step_test_panel '<component>
  <consts>
    <px name="gizmo_large" value="8"/>
  </consts>
</component>'
    cpp gizmo 'int32_t g = theme_manager_get_spacing("gizmo");'
    run_gate
    [ "$status" -eq 1 ]
    contains "gizmo_large" "$output"
    [[ "$output" != *"gizmo_small"* ]]
}

@test "flags a bare non-responsive token declared only in a dev-only panel" {
    xml test_panel '<component>
  <consts>
    <px name="ghost_opacity" value="51"/>
  </consts>
</component>'
    cpp ghost 'int32_t o = theme_manager_get_spacing("ghost_opacity");'
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"ghost_opacity"* ]]
}

@test "explains the consequence, not just the location" {
    xml step_test_panel '<component>
  <consts>
    <px name="gizmo_small" value="4"/>
    <px name="gizmo_medium" value="6"/>
    <px name="gizmo_large" value="8"/>
  </consts>
</component>'
    cpp gizmo 'int32_t g = theme_manager_get_spacing("gizmo");'
    run_gate
    [ "$status" -eq 1 ]
    contains "falls back" "$output"
    [[ "$output" == *"globals.xml"* ]]
}

@test "the dev-panel list comes from the makefile, not from the script" {
    xml step_test_panel '<component>
  <consts>
    <px name="gizmo_small" value="4"/>
    <px name="gizmo_medium" value="6"/>
    <px name="gizmo_large" value="8"/>
  </consts>
</component>'
    cpp gizmo 'int32_t g = theme_manager_get_spacing("gizmo");'
    printf 'DEV_PANEL_XML := glyphs_panel.xml\n' > "$FIXTURE/mk/cross.mk"
    run_gate
    [ "$status" -eq 0 ]
}

@test "refuses to pass when DEV_PANEL_XML cannot be read" {
    # A gate that finds no dev panels would report every tree clean.
    printf 'RELEASE_DIR := build/release\n' > "$FIXTURE/mk/cross.mk"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"DEV_PANEL_XML"* ]]
}

# ---------------------------------------------- idioms that must stay silent

@test "silent when the triplet lives in a shipped file" {
    xml globals '<component>
  <consts>
    <px name="step_indicator_small" value="16"/>
    <px name="step_indicator_medium" value="20"/>
    <px name="step_indicator_large" value="24"/>
  </consts>
</component>'
    cpp widget 'int32_t s = theme_manager_get_spacing("step_indicator");'
    run_gate
    [ "$status" -eq 0 ]
}

@test "silent on a token XML never declares" {
    # border_radius and button_height are registered from C++ tables per
    # breakpoint; packaging cannot remove what XML never provided.
    cpp radius 'int32_t r = theme_manager_get_spacing("border_radius");'
    run_gate
    [ "$status" -eq 0 ]
}

@test "silent when a shipped file and a dev panel both declare the token" {
    xml globals '<component>
  <consts>
    <px name="gizmo_small" value="4"/>
    <px name="gizmo_medium" value="6"/>
    <px name="gizmo_large" value="8"/>
  </consts>
</component>'
    xml step_test_panel '<component>
  <consts>
    <px name="gizmo_small" value="4"/>
    <px name="gizmo_medium" value="6"/>
    <px name="gizmo_large" value="8"/>
  </consts>
</component>'
    cpp gizmo 'int32_t g = theme_manager_get_spacing("gizmo");'
    run_gate
    [ "$status" -eq 0 ]
}

@test "a declaration inside an XML comment is not a declaration" {
    xml globals '<component>
  <consts>
    <px name="gizmo_small" value="4"/>
    <px name="gizmo_medium" value="6"/>
    <px name="gizmo_large" value="8"/>
  </consts>
</component>'
    xml step_test_panel '<component>
  <consts>
    <!-- <px name="gizmo_large" value="99"/> -->
  </consts>
</component>'
    cpp gizmo 'int32_t g = theme_manager_get_spacing("gizmo");'
    run_gate
    [ "$status" -eq 0 ]
}

@test "a call inside a C++ comment is not a call" {
    xml step_test_panel '<component>
  <consts>
    <px name="gizmo_small" value="4"/>
    <px name="gizmo_medium" value="6"/>
    <px name="gizmo_large" value="8"/>
  </consts>
</component>'
    cpp docs '// Example: theme_manager_get_spacing("gizmo")
/* also theme_manager_get_spacing("gizmo") */'
    run_gate
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------- the real tree

@test "the committed tree ships every token its C++ reads" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"✓"* ]]
}
