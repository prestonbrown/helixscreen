# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/lvgl_event_name.py.

LVGL 9.6 renumbered lv_event_code_t, so the same code names different events
on 9.5 and 9.6 builds. The decoder must pick its table from the crash's
lvgl_version the way the crash worker does, and mask LV_EVENT_PREPROCESS.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from lvgl_event_name import event_name  # noqa: E402


def test_a_record_without_lvgl_version_decodes_with_the_9_5_table():
    assert event_name(42, "") == "LV_EVENT_DELETE"
    assert event_name(48, "") != "LV_EVENT_DELETE"


def test_a_9_6_record_decodes_with_the_9_6_table():
    assert event_name(48, "9.6") == "LV_EVENT_DELETE"
    assert event_name(42, "9.6") != "LV_EVENT_DELETE"


def test_a_later_lvgl_uses_the_current_table():
    assert event_name(48, "10.0") == "LV_EVENT_DELETE"


def test_the_preprocess_flag_is_masked_off():
    assert event_name(42 | 0x8000, "") == "LV_EVENT_DELETE"
    assert event_name(48 | 0x8000, "9.6") == "LV_EVENT_DELETE"


def test_an_unknown_code_decodes_to_nothing():
    assert event_name(9999, "9.6") == ""
