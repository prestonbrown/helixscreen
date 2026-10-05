"""A component prop default forwarded into a translation key is itself a key."""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from translations.extractor import (  # noqa: E402
    extract_strings_from_xml,
    extract_strings_with_locations,
)

XML = """<component>
  <api>
    <prop name="message" type="string" default="Nothing is loaded. What now?"/>
    <prop name="raw" type="string" default="Rendered verbatim"/>
    <!-- <prop name="gone" default="Commented out"/> -->
  </api>
  <view extends="lv_obj">
    <text_body text="$message" translation_tag="$message"/>
    <text_body text="$raw"/>
    <text_body text="$gone" translation_tag="$gone"/>
  </view>
</component>"""


def _write(tmp_path):
    f = tmp_path / "sample.xml"
    f.write_text(XML, encoding="utf-8")
    return f


def test_tag_forwarded_prop_default_extracted(tmp_path):
    strings = extract_strings_from_xml(_write(tmp_path))
    assert "Nothing is loaded. What now?" in strings
    assert "Rendered verbatim" not in strings
    assert "Commented out" not in strings


def test_tag_forwarded_prop_default_located(tmp_path):
    locs = extract_strings_with_locations(_write(tmp_path))
    assert locs["Nothing is loaded. What now?"] == [("sample.xml", 3)]
    assert "Rendered verbatim" not in locs
