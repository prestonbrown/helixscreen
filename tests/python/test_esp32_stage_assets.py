#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Tests for the ESP32 LittleFS staging script's XML minifier: comments must be
stripped, inter-tag whitespace collapsed, and text content / attribute values
byte-preserved. Also round-trips a real ui_xml/ file through xml.etree to
confirm the minified output still parses and carries identical text/attrs.
"""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from esp32_stage_assets import minify_xml, stage_translations  # noqa: E402


def test_strips_single_line_comment():
    xml = '<component><!-- a comment --><widget/></component>'
    out = minify_xml(xml)
    assert "<!--" not in out
    assert "comment" not in out


def test_strips_multiline_comment():
    xml = "<component>\n  <!-- line one\n       line two -->\n  <widget/>\n</component>"
    out = minify_xml(xml)
    assert "<!--" not in out
    assert "line one" not in out
    assert "line two" not in out


def test_collapses_inter_tag_whitespace():
    xml = "<component>\n  <widget/>\n  <widget/>\n</component>"
    out = minify_xml(xml)
    assert ">\n" not in out
    assert "\n<" not in out
    assert out == "<component><widget/><widget/></component>"


def test_preserves_text_content_exactly():
    xml = '<label>Hello   World\n  with   odd  spacing</label>'
    out = minify_xml(xml)
    assert "Hello   World\n  with   odd  spacing" in out


def test_preserves_attribute_values_exactly():
    xml = '<widget style_pad_all="  12  " text="a    b">\n  <child/>\n</widget>'
    out = minify_xml(xml)
    assert 'style_pad_all="  12  "' in out
    assert 'text="a    b"' in out


def test_preserves_leading_trailing_whitespace_in_text_node():
    # Regression guard: text nodes sit in the same '>...<' position the
    # inter-tag collapse targets. A naive whitespace-only-blind collapse
    # would eat this text; the minifier must leave any non-whitespace-only
    # span alone.
    xml = "<a>  padded text  </a>"
    out = minify_xml(xml)
    assert out == xml


def test_does_not_touch_pure_whitespace_text_node_between_tags():
    # A genuinely empty/whitespace-only element body between two tags is
    # exactly what "inter-tag whitespace" collapse targets, distinct from
    # real (non-whitespace) text content.
    xml = "<a>\n   \n</a>"
    out = minify_xml(xml)
    assert out == "<a></a>"


def test_literal_gt_in_text_with_trailing_whitespace_not_corrupted():
    # Regression guard for the tag-boundary-unaware regex bug: a literal '>'
    # is legal unescaped in XML text content. A blind '>\s+<' regex reads the
    # literal '>' plus the trailing spaces plus the next tag's '<' as one
    # "whitespace between tags" span and silently eats the trailing spaces.
    # Compare exactly (no .strip()) — that's the whole point of the test.
    xml = "<label>value >   </label>"
    out = minify_xml(xml)
    assert out == xml


def test_literal_gt_in_attribute_value_not_corrupted():
    xml = '<widget text="a > b   ">\n  <child/>\n</widget>'
    out = minify_xml(xml)
    assert 'text="a > b   "' in out
    # the genuine inter-tag whitespace between the tags is still collapsed
    assert out == '<widget text="a > b   "><child/></widget>'


def test_globals_xml_round_trips_and_preserves_content():
    src_path = REPO_ROOT / "ui_xml" / "globals.xml"
    original = src_path.read_text(encoding="utf-8")
    minified = minify_xml(original)

    assert len(minified) < len(original), "minifier should shrink a real, comment-heavy file"
    assert "<!--" not in minified

    original_root = ET.fromstring(original)
    minified_root = ET.fromstring(minified)  # must still parse

    def normalize(value):
        # Whitespace-only (or absent) text/tail nodes are collapsed by the
        # minifier by design, so treat those as equivalent to None. Any
        # NON-empty text/tail must match byte-exactly — that's what would
        # catch tag-boundary corruption (e.g. a literal '>' in text losing
        # its trailing whitespace).
        if value is None or value.strip() == "":
            return None
        return value

    def collect(elem):
        return [
            (elem.tag, dict(elem.attrib), normalize(elem.text), normalize(elem.tail))
            for elem in elem.iter()
        ]

    assert collect(original_root) == collect(minified_root)


def test_stage_translations_hard_errors_when_en_does_not_fit(tmp_path):
    # The "en always ships" invariant must be a hard failure, not a silently
    # dropped language, if en.xml's minified size doesn't fit the remaining
    # on-flash budget.
    translations_dir = tmp_path / "ui_xml" / "translations"
    translations_dir.mkdir(parents=True)
    (translations_dir / "en.xml").write_text(
        "<translations><entry key=\"a\">Hello, World!</entry></translations>",
        encoding="utf-8",
    )

    with pytest.raises(SystemExit) as exc_info:
        stage_translations(tmp_path / "ui_xml", tmp_path / "out", remaining_budget=0)

    assert exc_info.value.code != 0
    # nothing should have been staged
    assert not (tmp_path / "out" / "ui_xml" / "translations" / "en.xml").exists()


def test_stage_translations_hard_errors_when_en_missing(tmp_path):
    translations_dir = tmp_path / "ui_xml" / "translations"
    translations_dir.mkdir(parents=True)
    (translations_dir / "fr.xml").write_text(
        "<translations><entry key=\"a\">Bonjour</entry></translations>",
        encoding="utf-8",
    )

    with pytest.raises(SystemExit) as exc_info:
        stage_translations(tmp_path / "ui_xml", tmp_path / "out", remaining_budget=1_000_000)

    assert exc_info.value.code != 0
