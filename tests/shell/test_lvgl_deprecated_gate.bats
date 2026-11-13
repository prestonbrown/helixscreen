#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the lvgl_deprecated quality gate: scripts/lvgl96_rename.py --check.
#
# LV_DISABLE_API_MAPPING turns a renamed LVGL name into a compile error, but the
# generic flag calls (lv_obj_add_flag, lv_obj_has_flag, ...) are real functions LVGL
# only marks deprecated, and the build keeps deprecation a warning. This gate is what
# fails a new one. It also keeps helix::ui::obj_set_flags()/obj_has_flags() to the
# flags LVGL gives no per-flag function, so the wrapper cannot hide a hidden-flag
# toggle from the declarative-UI gates.

GATE="scripts/lvgl96_rename.py"

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}"
    FILE="$DIR/demo.cpp"
}

@test "a deprecated generic flag call fails the check" {
    printf '%s\n' 'void f(lv_obj_t* w) {' '    lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);' '}' > "$FILE"
    run python3 $GATE --check "$FILE"
    [ "$status" -eq 1 ]
    contains "demo.cpp:2: lv_obj_add_flag" "$output"
}

@test "an API-map name fails the check" {
    printf '%s\n' 'void f(lv_subject_t* s) { lv_subject_copy_string(s, "x"); }' > "$FILE"
    run python3 $GATE --check "$FILE"
    [ "$status" -eq 1 ]
    contains "lv_subject_copy_string" "$output"
}

@test "the wrapper with a built-in flag that has its own setter fails the check" {
    printf '%s\n' 'void f(lv_obj_t* w, bool v) {' \
        '    helix::ui::obj_set_flags(w, LV_OBJ_FLAG_HIDDEN, v);' \
        '    if (obj_has_flags(w, LV_OBJ_FLAG_CLICKABLE)) return;' '}' > "$FILE"
    run python3 $GATE --check "$FILE"
    [ "$status" -eq 1 ]
    contains "demo.cpp:2: obj_set_flags" "$output"
    contains "demo.cpp:3: obj_has_flags" "$output"
}

@test "the wrapper with a marker flag, a USER_n flag or a run-time mask passes" {
    printf '%s\n' 'void f(lv_obj_t* w, lv_obj_flag_t m) {' \
        '    helix::ui::obj_set_flags(w, kPanelWidgetTileFlag, true);' \
        '    helix::ui::obj_set_flags(w, LV_OBJ_FLAG_USER_4, true);' \
        '    if (helix::ui::obj_has_flags(w, m)) return;' '}' > "$FILE"
    run python3 $GATE --check "$FILE"
    [ "$status" -eq 0 ]
}

@test "new names, comments, strings and the opt-out pass" {
    printf '%s\n' 'void f(lv_obj_t* w) {' \
        '    lv_obj_set_hidden(w, true);' \
        '    // lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN) was the old spelling' \
        '    log("lv_obj_add_flag");' \
        '    lv_obj_has_flag(w, f); // LV_DEPRECATED_OK: generic helper' '}' > "$FILE"
    run python3 $GATE --check "$FILE"
    [ "$status" -eq 0 ]
}

@test "quality-checks.sh runs the gate" {
    run grep -c 'qc_lvgl_deprecated' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
    run grep -c -- 'lvgl96_rename.py --check' scripts/qc/lvgl_deprecated.sh
    [ "$status" -eq 0 ]
}
