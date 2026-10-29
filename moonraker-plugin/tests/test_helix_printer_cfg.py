# SPDX-License-Identifier: GPL-3.0-or-later
"""
printer.cfg include lines the plugin adds (helix_printer_cfg.py): power-loss
recovery's mesh include and the Klipper companions' include share one set of
rules, so each feature's lines are found, added and removed without touching
the other's or the user's.

Run with: pytest tests/test_helix_printer_cfg.py -v
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

import helix_printer_cfg as pc

A_MARK, A_LINE = "# Added by HelixScreen: a", "[include a/*.cfg]"
B_MARK, B_LINE = "# Added by HelixScreen: b", "[include b/*.cfg]"
USER = "[printer]\nkinematics: corexy\n"


def test_add_appends_marker_and_line_once():
    text = pc.add_include(USER, A_MARK, A_LINE)
    assert text == USER + A_MARK + "\n" + A_LINE + "\n"
    assert pc.add_include(text, A_MARK, A_LINE) is None


def test_add_to_a_file_without_a_final_newline():
    assert pc.add_include("[printer]", A_MARK, A_LINE).startswith("[printer]\n" + A_MARK)


def test_two_features_keep_their_own_lines():
    text = pc.add_include(pc.add_include(USER, A_MARK, A_LINE), B_MARK, B_LINE)
    stripped, removed = pc.strip_include(text, A_MARK, A_LINE)
    assert removed == 2
    assert pc.include_present(stripped, B_LINE)
    assert not pc.include_present(stripped, A_LINE)
    assert stripped.startswith(USER)


def test_a_users_own_spelling_is_never_ours():
    text = USER + "[include a/*.cfg] # mine\n"
    assert pc.strip_include(text, A_MARK, A_LINE) == (text, 0)


def test_write_change_backs_up_the_old_text(tmp_path):
    cfg = tmp_path / "printer.cfg"
    cfg.write_text(USER)
    pc.write_change(cfg, USER, USER + A_LINE + "\n")
    assert cfg.read_text() == USER + A_LINE + "\n"
    assert (tmp_path / ("printer.cfg" + pc.BACKUP_SUFFIX)).read_text() == USER
