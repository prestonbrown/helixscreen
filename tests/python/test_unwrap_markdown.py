# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/unwrap-markdown.py: soft-wrapped lines are joined for publishing."""

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from importlib import import_module  # noqa: E402

unwrap = import_module("unwrap-markdown").unwrap


def test_wrapped_paragraph_is_joined():
    assert unwrap("one two\nthree four\nfive\n") == "one two three four five\n"


def test_paragraphs_stay_separate():
    assert unwrap("a\nb\n\nc\nd\n") == "a b\n\nc d\n"


def test_wrapped_bullet_continuations_join_onto_the_item():
    src = "- first item that\n  keeps going\n  and going\n- second\n"
    assert unwrap(src) == "- first item that keeps going and going\n- second\n"


def test_numbered_items_and_lazy_continuation():
    src = "1. alpha\n   beta\n2. gamma\ndelta\n"
    assert unwrap(src) == "1. alpha beta\n2. gamma delta\n"


def test_nested_bullets_stay_nested():
    src = "- parent text\n  more parent\n  - child text\n    more child\n- next\n"
    assert unwrap(src) == "- parent text more parent\n  - child text more child\n- next\n"


def test_fenced_code_block_is_untouched():
    src = "intro\nline\n```\nkeep\n  this   wrapped\n- not a bullet\n```\nafter\n"
    assert unwrap(src) == "intro line\n```\nkeep\n  this   wrapped\n- not a bullet\n```\nafter\n"


def test_tilde_fence_and_longer_fence():
    src = "~~~\na\nb\n~~~\n````\nx\ny\n````\n"
    assert unwrap(src) == src


def test_table_is_untouched():
    src = "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n"
    assert unwrap(src) == src


def test_headings_stand_alone():
    src = "# Title\ntext one\ntext two\n## Next\n- item\n  wrapped\n"
    assert unwrap(src) == "# Title\ntext one text two\n## Next\n- item wrapped\n"


def test_text_after_a_table_does_not_join_onto_it():
    src = "| a |\n|---|\nafter\n"
    assert unwrap(src) == src


def test_blockquote_lines_are_untouched():
    src = "> quoted\n> more\ntext\n"
    assert unwrap(src) == "> quoted\n> more\ntext\n"


def test_hard_break_is_kept():
    assert unwrap("one  \ntwo\n") == "one  \ntwo\n"


def test_blank_lines_and_missing_trailing_newline():
    assert unwrap("a\n\n\nb") == "a\n\n\nb"


def test_cli_reads_stdin():
    out = subprocess.run(
        [sys.executable, str(REPO_ROOT / "scripts" / "unwrap-markdown.py")],
        input="a\nb\n",
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    assert out == "a b\n"
