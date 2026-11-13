# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/lvgl96_rename.py: the rewrite is exact, leaves what it cannot
prove alone, and is idempotent."""

import importlib.util
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("lvgl96_rename", REPO_ROOT / "scripts" / "lvgl96_rename.py")
lvgl96_rename = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lvgl96_rename)

RW = lvgl96_rename.Rewriter()


def rewrite(text):
    report = []
    text, _ = RW.rename(text)
    text, _ = RW.flags(text, "demo.cpp", report)
    return text, report


def test_single_literal_flags_map_onto_the_per_flag_functions():
    src = (
        "lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);\n"
        "lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);\n"
        "lv_obj_set_flag(c, LV_OBJ_FLAG_SCROLLABLE, on);\n"
        "if (!lv_obj_has_flag(d, LV_OBJ_FLAG_HIDDEN)) {}\n"
        "lv_obj_clear_flag(e, LV_OBJ_FLAG_SCROLLABLE);\n"
    )
    out, report = rewrite(src)
    assert out == (
        "lv_obj_set_hidden(a, true);\n"
        "lv_obj_set_clickable(b, false);\n"
        "lv_obj_set_scrollable(c, on);\n"
        "if (!lv_obj_is_hidden(d)) {}\n"
        "lv_obj_set_scrollable(e, false);\n"
    )
    assert report == []


def test_masks_variables_and_user_flags_are_left_and_reported():
    src = (
        "lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);\n"
        "lv_obj_add_flag(b, kMarker);\n"
        "lv_obj_has_flag(c, LV_OBJ_FLAG_USER_1);\n"
    )
    out, report = rewrite(src)
    assert out == src
    assert [r[1] for r in report] == [1, 2, 3]


def test_api_map_names_and_inline_functions_rename_outside_strings():
    src = 'lv_subject_copy_string(s, "lv_subject_copy_string");\nlv_obj_move_foreground(o);\n'
    out, _ = rewrite(src)
    assert out == 'lv_subject_set_string(s, "lv_subject_copy_string");\nlv_obj_move_to_index(o, -1);\n'


def test_live_config_names_in_the_map_are_not_renamed():
    assert "LV_DRAW_LAYER_SIMPLE_BUF_SIZE" not in RW.renames


def test_a_second_run_changes_nothing():
    src = "lv_obj_add_flag(\n    obj,\n    LV_OBJ_FLAG_HIDDEN);\nlv_obj_del(o);\n"
    once, _ = rewrite(src)
    twice, _ = rewrite(once)
    assert once == twice == "lv_obj_set_hidden(\n    obj, true);\nlv_obj_delete(o);\n"
