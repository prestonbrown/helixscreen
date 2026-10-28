# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/mdi_icon_codepoints.py: the icon fonts' codepoint list.

The list must hold every glyph the icon table or an ICON_* macro names, and an entry
whose bytes and comment disagree, or that is not one glyph, must stop the regen
instead of shipping a font without it.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import mdi_icon_codepoints  # noqa: E402

ALERT = r'"\xF3\xB0\x80\xA6"'  # U+F0026
HOME = r'"\xF3\xB0\x8B\x9C"'  # U+F02DC
SPOOLMAN = r'"\xF3\xBE\x80\x80"'  # U+FE000


def _tree(tmp_path, table_lines, macro_lines):
    (tmp_path / "include").mkdir()
    (tmp_path / "include" / "ui_icon_codepoints.h").write_text(
        "static const IconEntry kIconMap[] = {\n" + "\n".join(table_lines) + "\n};\n")
    (tmp_path / "include" / "ui_fonts.h").write_text("\n".join(macro_lines) + "\n")
    return tmp_path


def _run(capsys, root, flag):
    code = mdi_icon_codepoints.main([flag, "--root", str(root)])
    return code, capsys.readouterr()


def test_union_of_table_and_macros_split_by_block(tmp_path, capsys):
    root = _tree(
        tmp_path,
        [f'    {{"alert", {ALERT}}},  // F0026 alert',
         f'    {{"spoolman", {SPOOLMAN}}},  // FE000 spoolman'],
        [f"#define ICON_HOME {HOME} // U+F02DC home",
         f"#define ICON_ALERT_TOO {ALERT} // U+F0026 alert",
         "#define ICON_ALIAS ICON_HOME // alias, no glyph of its own"])
    assert _run(capsys, root, "--mdi") == (0, ("0xF0026,0xF02DC\n", ""))
    assert _run(capsys, root, "--custom")[1].out == "0xFE000\n"
    assert _run(capsys, root, "--table")[1].out == (
        "0xF0026 alert\n0xF02DC home\n0xFE000 spoolman\n")


def test_comment_disagreeing_with_the_bytes_fails(tmp_path, capsys):
    root = _tree(tmp_path, [f'    {{"alert", {ALERT}}},  // F0028 alert-circle'], [])
    code, out = _run(capsys, root, "--mdi")
    assert code == 1
    assert "F0028" in out.err and out.out == ""


def test_an_escape_that_is_not_one_glyph_fails(tmp_path, capsys):
    two = r'"\xF3\xB0\x80\xA6\xF3\xB0\x8B\x9C"'
    root = _tree(tmp_path, [f'    {{"two", {two}}},'], [])
    code, out = _run(capsys, root, "--mdi")
    assert code == 1
    assert "2 codepoints" in out.err


def test_a_glyph_outside_the_icon_blocks_fails(tmp_path, capsys):
    root = _tree(tmp_path, [], [r'#define ICON_A "\x41"'])
    code, out = _run(capsys, root, "--mdi")
    assert code == 1
    assert "outside the icon blocks" in out.err


def test_an_empty_list_fails(tmp_path, capsys):
    root = _tree(tmp_path, [f'    {{"spoolman", {SPOOLMAN}}},'], [])
    code, out = _run(capsys, root, "--mdi")
    assert code == 1
    assert "no codepoints" in out.err


def test_the_real_tree_covers_both_sources(capsys):
    code, out = _run(capsys, REPO_ROOT, "--mdi")
    assert code == 0
    listed = out.out.strip().split(",")
    assert listed == sorted(listed, key=lambda c: int(c, 16))
    assert "0xF0026" in listed  # alert: in the icon table
    assert "0xF030F" in listed  # keyboard-close: only an ICON_* macro names it
