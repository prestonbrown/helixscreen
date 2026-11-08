# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/source_strip.py, the comment and literal blanker every lint gate shares."""

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from source_strip import blank_cxx, blank_cxx_lines, blank_xml_comments  # noqa: E402

SAMPLE = 'a(); // tail\n/* x\ny */ b("s//t", \'c\');\nR"d(raw // )" no)d" z;\n'


@pytest.mark.parametrize("mode", ["blank", "body", "keep"])
def test_every_mode_preserves_length_and_newlines(mode):
    out = blank_cxx(SAMPLE, strings=mode)
    assert len(out) == len(SAMPLE)
    assert [i for i, c in enumerate(out) if c == "\n"] == [i for i, c in enumerate(SAMPLE) if c == "\n"]


def test_comments_are_blanked_in_every_mode():
    for mode in ("blank", "body", "keep"):
        out = blank_cxx(SAMPLE, strings=mode)
        assert "tail" not in out and "x" not in out.split("\n")[1]
        assert "a();" in out and "z;" in out


def test_blank_mode_clears_literals_quotes_and_all():
    out = blank_cxx('f("abc", \'d\');')
    assert out == "f(     ,    );"


def test_body_mode_keeps_the_quotes():
    out = blank_cxx('f("abc", \'d\');', strings="body")
    assert out == 'f("   ", \' \');'


def test_keep_mode_leaves_literals_and_still_skips_slashes_in_them():
    src = 'f({"x", "https://a"}, 1); // tail\ng();'
    out = blank_cxx(src, strings="keep")
    assert '"https://a"}, 1);' in out
    assert "tail" not in out


def test_escaped_quote_does_not_end_a_literal():
    out = blank_cxx(r'f("a\"b // no", 1); // yes')
    assert "no" not in out and "yes" not in out and "1);" in out


def test_digit_separator_is_not_a_char_literal():
    src = "int n = 1'000'000; // tail\nint m = 0xFF'FF;"
    out = blank_cxx(src)
    assert "1'000'000;" in out and "0xFF'FF;" in out
    assert "tail" not in out


def test_prefixed_char_literal_is_still_a_literal():
    assert blank_cxx("c = L'x'; d;") == "c = L   ; d;"


def test_raw_string_ends_only_at_its_delimiter():
    src = 'auto s = R"d(a "quote" // not a comment )" still )d"; tail();'
    out = blank_cxx(src)
    assert "quote" not in out and "still" not in out
    assert out.endswith("; tail();")


def test_raw_string_after_an_identifier_is_not_raw():
    out = blank_cxx('FOOR"(x)";')
    assert "FOOR" in out


def test_unterminated_string_stops_at_end_of_line():
    out = blank_cxx("bad(\"oops\nkeep(); // c")
    assert "keep();" in out and "c" not in out.split("\n")[1].replace("keep", "")


def test_unterminated_block_comment_runs_to_eof():
    out = blank_cxx("a; /* never closed\nb;")
    assert out.strip() == "a;"


def test_preprocessor_lines_blanked_with_continuations():
    src = "#define M(x) \\\n  { x }\nint f() {\n  #if A\n}\n"
    out = blank_cxx(src, preprocessor=True)
    lines = out.split("\n")
    assert lines[0].strip() == "" and lines[1].strip() == "" and lines[3].strip() == ""
    assert lines[2] == "int f() {"


def test_hash_mid_line_is_not_a_directive():
    out = blank_cxx("x = a # b;\n", preprocessor=True)
    assert out == "x = a # b;\n"


def test_unknown_mode_is_refused():
    with pytest.raises(ValueError):
        blank_cxx("x", strings="drop")


def test_lines_variant_keeps_the_line_count():
    lines = ["a; /* x", "y */ b;", 'c("//");']
    out = blank_cxx_lines(lines, strings="body")
    assert len(out) == 3 and out[1].strip() == "b;" and out[2] == 'c("  ");'


def test_xml_comments_blanked_keeping_offsets():
    src = '<a x="1"/><!-- <b y="2"/>\nmore -->\n<c/>'
    out = blank_xml_comments(src)
    assert len(out) == len(src) and out.count("\n") == 2
    assert 'y="2"' not in out and "<c/>" in out and '<a x="1"/>' in out
