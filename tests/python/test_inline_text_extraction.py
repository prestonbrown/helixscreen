"""Inline XML text extraction — parity with lib/helix-xml/src/xml/lv_xml.c PCDATA handling."""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from translations.extractor import (  # noqa: E402
    collapse_whitespace,
    extract_strings_from_xml,
    extract_strings_with_locations,
)

# Shared collapse-parity table — keep in sync with the C tests in
# lib/helix-xml/tests/cases/test_inline_text.c (inputs are post-entity-decode).
COLLAPSE_TABLE = [
    ("Hello world", "Hello world"),
    ("  Hello  world  ", "Hello world"),
    ("\n    Hello\n    world\n  ", "Hello world"),
    ("Tabs\there\tand\rthere", "Tabs here and there"),
    ("Hello\nworld", "Hello world"),
    ("   \n\t  ", ""),
    ("", ""),
]


def test_collapse_whitespace_parity_table():
    for raw, expected in COLLAPSE_TABLE:
        assert collapse_whitespace(raw) == expected, f"input: {raw!r}"


def _extract(tmp_path, xml: str):
    f = tmp_path / "sample.xml"
    f.write_text(xml, encoding="utf-8")
    return extract_strings_from_xml(f)


def test_inline_text_extracted(tmp_path):
    xml = """<component>
  <view extends="lv_obj">
    <text_muted name="msg">Print speed</text_muted>
  </view>
</component>"""
    assert "Print speed" in _extract(tmp_path, xml)


def test_inline_text_collapsed_before_keying(tmp_path):
    xml = """<component>
  <view extends="lv_obj">
    <text_body name="msg">
      Multi line
      copy here
    </text_body>
  </view>
</component>"""
    result = _extract(tmp_path, xml)
    assert "Multi line copy here" in result
    assert not any("\n" in s for s in result)


def test_inline_entities_decoded_then_collapsed(tmp_path):
    # &#10; decodes to a newline, which then collapses to a space — this
    # mirrors expat (decode) + the C collapse, in that order.
    xml = '<view><text_muted name="m">Fish &amp; chips&#10;tonight</text_muted></view>'
    assert "Fish & chips tonight" in _extract(tmp_path, xml)


def test_inline_skips_bind_text_elements(tmp_path):
    xml = '<view><text_muted name="m" bind_text="some_subject">fallback junk</text_muted></view>'
    assert "fallback junk" not in _extract(tmp_path, xml)


def test_inline_skips_conflicting_text_attr(tmp_path):
    # Parser drops inline text when text= is present; extractor must too
    # (the text= value itself is still extracted by the existing attr pass).
    xml = '<view><text_muted name="m" text="Kept attr">dropped inline</text_muted></view>'
    result = _extract(tmp_path, xml)
    assert "dropped inline" not in result
    assert "Kept attr" in result


def test_inline_skips_prop_and_const_tokens(tmp_path):
    xml = """<view>
    <text_muted name="a">$title</text_muted>
    <text_muted name="b">#space_md</text_muted>
</view>"""
    result = _extract(tmp_path, xml)
    assert "$title" not in result
    assert "#space_md" not in result


def test_inline_whitespace_only_ignored(tmp_path):
    xml = '<view><text_muted name="m">\n    </text_muted></view>'
    assert "" not in _extract(tmp_path, xml)


def test_inline_text_locations_line_number(tmp_path):
    # A two-line XML comment sits above the inline-text element. This locks
    # the _blank_xml_comments invariant: comment bodies are blanked to
    # same-length filler (newlines preserved), so match offsets/line numbers
    # are computed against the ORIGINAL content, not a comment-stripped one.
    xml = """<component>
  <!-- comment line one
       comment line two -->
  <view extends="lv_obj">
    <text_muted name="msg">Print speed</text_muted>
  </view>
</component>"""
    f = tmp_path / "sample.xml"
    f.write_text(xml, encoding="utf-8")
    result = extract_strings_with_locations(f)
    assert "Print speed" in result
    # Line count (1-indexed), hand-counted from the string above:
    # 1 <component>  2 <!-- comment line one  3 comment line two -->
    # 4 <view ...>  5 <text_muted ...>Print speed</text_muted>
    assert result["Print speed"] == [("sample.xml", 5)]


def test_inline_mixed_content_leading_text(tmp_path):
    # Text before a child element ("mixed content") must still be caught —
    # INLINE_TEXT_RE's [^<]+ stops at the next "<", whether that's the
    # closing tag or a child element's opening tag.
    xml = '<view><text_muted name="m">Leading text<lv_obj name="child"/></text_muted></view>'
    assert "Leading text" in _extract(tmp_path, xml)


def test_title_tag_extracted(tmp_path):
    # title_tag names a section-header key (setting_group_header and friends)
    # and pairs with title= exactly the way label_tag pairs with label=. A
    # header carrying title= for the design-time fallback plus title_tag= for
    # the runtime lookup is asking for the tag to be synced.
    xml = '<setting_group_header title="CONTROLLABLE FANS" title_tag="Controllable Fans"/>'
    assert "Controllable Fans" in _extract(tmp_path, xml)


def test_title_tag_param_reference_skipped(tmp_path):
    # A $param is a component parameter forwarded at instantiation time, not a
    # literal key - harvesting it would sync a "$title_tag" key.
    xml = '<setting_group_header title="X" title_tag="$title_tag"/>'
    assert "$title_tag" not in _extract(tmp_path, xml)
