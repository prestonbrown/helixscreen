"""Every title_tag literal in ui_xml must have a non-empty translation in every
shipped locale.

A title_tag that never reaches the locale YAMLs leaves that section header in
English on every non-English device. Bundle CSLYH92R showed both failure
shapes: "Controllable Fans" / "Auto Fans" were never synced at all (title_tag
was missing from the extractor's attribute list), and the favorite-macro
dialog's three tags were synced but left as empty placeholders. This gate
fails on either shape."""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from translations.extractor import (  # noqa: E402
    I18N_SKIP_FILE_RE,
    _decode_xml_entities,
    should_skip_text,
)
from translations.yaml_manager import load_yaml_file_readonly  # noqa: E402

TITLE_TAG_RE = re.compile(r'(?<![\w])title_tag="([^"]*)"')


def _title_tags() -> set:
    tags = set()
    for xml in sorted((REPO_ROOT / "ui_xml").rglob("*.xml")):
        if "translations" in xml.parts:
            continue
        content = xml.read_text(encoding="utf-8")
        if I18N_SKIP_FILE_RE.search(content):
            continue
        for match in TITLE_TAG_RE.finditer(content):
            text = _decode_xml_entities(match.group(1))
            if text and not text.startswith(("$", "#")) and not should_skip_text(text):
                tags.add(text)
    return tags


def test_every_title_tag_translated_in_every_locale():
    tags = _title_tags()
    # The real population (checked 2026-08-31) is 17; the floor only guards
    # the scan against matching nothing.
    assert len(tags) >= 15, f"scan found too few title_tag literals: {sorted(tags)}"

    locales = sorted((REPO_ROOT / "translations").glob("*.yml"))
    assert len(locales) == 9, f"expected 9 locale files, found {[p.name for p in locales]}"

    missing = []
    for yml in locales:
        data = load_yaml_file_readonly(yml)
        entries = data["translations"]
        for tag in sorted(tags):
            if not entries.get(tag):
                missing.append(f"{yml.name}: {tag!r}")
    assert not missing, "title_tag keys missing or empty:\n  " + "\n  ".join(missing)
