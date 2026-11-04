#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_hand_rolled_swatch.py: the ratchet keeping colour
# swatches on the one swatch painter (docs/devel/COLOR_SWATCHES.md).
#
# The quiet half matters as much as the loud one: circles are everywhere in this
# UI (badges, status dots, the swatch components themselves), and a gate that
# fired on them would be switched off.

load helpers

GATE="scripts/check_hand_rolled_swatch.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src/ui" "$ROOT/ui_xml/components"
    : > "$ROOT/base.txt"
}

run_gate() {
    run python3 "$GATE" --root "$ROOT" --baseline "$ROOT/base.txt"
}

# ------------------------------------------------------------- the loud half

@test "catches a circle whose fill is bound to a subject" {
    cat > "$ROOT/ui_xml/x.xml" <<'XML'
<component><view extends="lv_obj">
  <lv_obj name="spool" width="20" height="20" style_radius="9999" style_bg_opa="255">
    <bind_style_prop prop="bg_color" subject="slot_color"/>
  </lv_obj>
</view></component>
XML
    run_gate
    [ "$status" -eq 1 ] || fail "bound circle passed: $output"
    contains "x.xml:xml-bound-circle" "$output"
}

@test "catches a circle whose fill is a component prop" {
    cat > "$ROOT/ui_xml/x.xml" <<'XML'
<component><view extends="lv_obj">
  <lv_obj name="disc" style_radius="9999" style_bg_color="$color"/>
</view></component>
XML
    run_gate
    [ "$status" -eq 1 ] || fail "prop-filled circle passed: $output"
    contains "x.xml:xml-bound-circle" "$output"
}

@test "catches a circle named like a swatch that C++ colours" {
    printf '<component><view extends="lv_obj"><lv_obj name="seated_swatch" style_radius="9999" style_bg_opa="255"/></view></component>\n' \
        > "$ROOT/ui_xml/x.xml"
    run_gate
    [ "$status" -eq 1 ] || fail "named swatch passed: $output"
    contains "x.xml:xml-named-swatch" "$output"
}

@test "catches a C++ circle filled with a slot colour" {
    cat > "$ROOT/src/ui/x.cpp" <<'CPP'
void paint(lv_layer_t* layer, const SlotInfo& slot, lv_area_t a) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(slot.color_rgb);
    dsc.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &dsc, &a);
}
CPP
    run_gate
    [ "$status" -eq 1 ] || fail "C++ circle passed: $output"
    contains "x.cpp:cpp-circle-fill" "$output"
}

@test "catches the lv_obj form in C++" {
    printf 'void f(lv_obj_t* o, uint32_t filament_rgb) {\n    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);\n    lv_obj_set_style_bg_color(o, lv_color_hex(filament_rgb), 0);\n}\n' \
        > "$ROOT/src/ui/x.cpp"
    run_gate
    [ "$status" -eq 1 ] || fail "lv_obj circle passed: $output"
}

@test "catches an LED colour, and not a word that merely contains led" {
    printf 'void f(lv_obj_t* o, lv_color_t led_color) {\n    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);\n    lv_obj_set_style_bg_color(o, led_color, 0);\n}\n' \
        > "$ROOT/src/ui/x.cpp"
    run_gate
    [ "$status" -eq 1 ] || fail "LED circle passed: $output"
    printf 'void f(lv_obj_t* o) {\n    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);\n    lv_obj_set_style_bg_color(o, theme_manager_get_color("disabled"), 0);\n}\n' \
        > "$ROOT/src/ui/x.cpp"
    run_gate
    [ "$status" -eq 0 ] || fail "a 'disabled' theme colour was counted: $output"
}

# ------------------------------------------------------------- the quiet half

@test "the swatch components and a fixed-token circle are quiet" {
    cat > "$ROOT/ui_xml/x.xml" <<'XML'
<component><view extends="lv_obj">
  <color_swatch index="0" color_subject="led_swatch_color_0"/>
  <color_dot name="spool_swatch" color_subject="slot_color"/>
  <swatch_paint name="bar" bind_color="slot_color"/>
  <lv_obj name="badge" style_radius="9999" style_bg_color="#primary"/>
  <lv_obj name="status" style_radius="9999">
    <bind_style_if_eq name="ok" subject="homed" ref_value="1"/>
  </lv_obj>
</view></component>
XML
    run_gate
    [ "$status" -eq 0 ] || fail "quiet cases reported: $output"
}

@test "a circle with a theme colour in C++ is quiet" {
    printf 'void f(lv_obj_t* o) {\n    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);\n    lv_obj_set_style_bg_color(o, theme_manager_get_color("primary"), 0);\n}\n' \
        > "$ROOT/src/ui/x.cpp"
    run_gate
    [ "$status" -eq 0 ] || fail "themed circle reported: $output"
}

@test "commented-out markup is not counted" {
    printf '<component><view extends="lv_obj"><!-- <lv_obj name="old_swatch" style_radius="9999"/> --></view></component>\n' \
        > "$ROOT/ui_xml/x.xml"
    run_gate
    [ "$status" -eq 0 ] || fail "a comment was counted: $output"
}

@test "SWATCH_OK on or above the line opts a site out" {
    cat > "$ROOT/ui_xml/x.xml" <<'XML'
<component><view extends="lv_obj">
  <!-- SWATCH_OK: a homing dot -->
  <lv_obj name="axis_dot" style_radius="9999" style_bg_opa="255"/>
</view></component>
XML
    printf 'void f(lv_obj_t* o, uint32_t filament_rgb) {\n    // SWATCH_OK: test\n    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);\n    lv_obj_set_style_bg_color(o, lv_color_hex(filament_rgb), 0);\n}\n' \
        > "$ROOT/src/ui/x.cpp"
    run_gate
    [ "$status" -eq 0 ] || fail "SWATCH_OK ignored: $output"
}

@test "the painter and the swatch components are exempt" {
    printf '<component><view extends="lv_obj"><lv_obj name="color_swatch_disc" style_radius="9999" style_bg_color="$color"/></view></component>\n' \
        > "$ROOT/ui_xml/components/color_swatch.xml"
    printf 'void f(lv_obj_t* o, uint32_t filament_rgb) {\n    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);\n    lv_obj_set_style_bg_color(o, lv_color_hex(filament_rgb), 0);\n}\n' \
        > "$ROOT/src/ui/components/ui_swatch.cpp"
    run_gate
    [ "$status" -eq 0 ] || fail "exempt files reported: $output"
}

# ------------------------------------------------------------- the ratchet

@test "fails above a nonzero baseline, passes at or below it" {
    printf '<component><view extends="lv_obj"><lv_obj name="a_swatch" style_radius="9999"/><lv_obj name="b_swatch" style_radius="9999"/></view></component>\n' \
        > "$ROOT/ui_xml/x.xml"
    printf '1 ui_xml/x.xml:xml-named-swatch\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 1 ] || fail "two sites over a baseline of one passed"
    printf '2 ui_xml/x.xml:xml-named-swatch\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 0 ] || fail "two sites at a baseline of two failed: $output"
    printf '3 ui_xml/x.xml:xml-named-swatch\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 0 ] || fail "below baseline failed: $output"
    contains "lowered" "$output"
}

@test "the gate runs for staged sources, XML and its own files" {
    local re
    re="$(qc_trigger hand_rolled_swatch)"
    [[ "src/ui/ui_x.cpp" =~ $re ]] || fail "a source file does not trigger the gate"
    [[ "include/ui_x.h" =~ $re ]] || fail "a header does not trigger the gate"
    [[ "ui_xml/components/x.xml" =~ $re ]] || fail "an XML file does not trigger the gate"
    [[ "scripts/hand_rolled_swatch_baseline.txt" =~ $re ]] || fail "the baseline does not trigger it"
    [[ ! "docs/devel/COLOR_SWATCHES.md" =~ $re ]] || fail "a doc triggers the gate"
}
