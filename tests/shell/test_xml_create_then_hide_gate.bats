#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_xml_create_then_hide.py: an lv_xml_create() result
# hidden right after its build should have been built with
# helix::ui::create_xml_hidden() instead.

load helpers

GATE="scripts/check_xml_create_then_hide.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FILE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/demo.cpp"
}

@test "a create followed by a hide of the same object is reported" {
    cat > "$FILE" <<'SHAPES'
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "demo_overlay", nullptr));
    find_required(overlay_root_, "x", "y");
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
SHAPES
    run python3 "$GATE" "$FILE"
    [ "$status" -eq 1 ]
    contains "demo.cpp:1: overlay_root_" "$output"
}

@test "create_xml_hidden, a hide of another object and the opt-out are quiet" {
    cat > "$FILE" <<'SHAPES'
    auto* a = helix::ui::create_xml_hidden(parent, "demo");
    lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
    auto* b = static_cast<lv_obj_t*>(lv_xml_create(parent, "row", nullptr));
    lv_obj_add_flag(other, LV_OBJ_FLAG_HIDDEN);
    auto* c = static_cast<lv_obj_t*>(lv_xml_create(parent, "row", nullptr));
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN); // XML_HIDDEN_OK: pooled row
SHAPES
    run python3 "$GATE" "$FILE"
    [ "$status" -eq 0 ]
}

@test "quality-checks.sh runs the gate over the tree" {
    run grep -c 'python3 scripts/check_xml_create_then_hide.py >' scripts/qc/decl_ui.sh
    [ "$status" -eq 0 ]
    [ "$output" -ge 1 ]
}
