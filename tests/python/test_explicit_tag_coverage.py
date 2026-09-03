"""Every explicit *_tag literal in ui_xml must have a non-empty translation in
every shipped locale.

A tag that never reaches the locale YAMLs leaves that string in English on
every non-English device. Bundle CSLYH92R showed both failure shapes:
"Controllable Fans" / "Auto Fans" were never synced at all (title_tag was
missing from the extractor's attribute list), and the favorite-macro dialog's
three tags were synced but left as empty placeholders. This gate fails on
either shape.

It iterates EXPLICIT_TAG_ATTRIBUTES rather than naming one attribute, so
teaching the extractor a new forwarding prop also puts it under this gate --
hardcoding title_tag is how description_tag stayed uncovered while carrying 139
uses and one string ("Suppress periodic temperature status lines") that reached
no locale at all.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from translations.extractor import (  # noqa: E402
    EXPLICIT_TAG_ATTRIBUTES,
    I18N_SKIP_FILE_RE,
    _decode_xml_entities,
    should_skip_text,
)
from translations.yaml_manager import load_yaml_file_readonly  # noqa: E402

# (?<![\w]) matches the extractor's own guard: without it `text_tag` would also
# match inside `action_button_text_tag`, attributing one literal to two props.
TAG_RES = {
    attr: re.compile(rf'(?<![\w]){attr}="([^"]*)"') for attr in EXPLICIT_TAG_ATTRIBUTES
}

# Floor per attribute, only to guard the scan against silently matching
# nothing -- a typo'd attribute name would otherwise make this gate vacuous.
# Real populations checked 2026-09-01; the floors sit below them.
MIN_TAGS = {
    "translation_tag": 500,
    "label_tag": 100,
    "title_tag": 15,
    "description_tag": 50,
    "action_button_text_tag": 3,
    "action_button_2_text_tag": 1,
    "primary_tag": 3,
    "secondary_tag": 2,
    "tertiary_tag": 1,
    "close_text_tag": 1,
    "text_tag": 1,
}


# Tags that ARE synced into every locale but still carry an empty value, so the
# device renders the English key. A ratchet, not an exemption: the assertion
# below fails on a new untranslated tag AND on a listed one that has since been
# translated, so this list can only shrink.
#
# Both entries belong to the Spoolman archive feature, which is untranslated as
# a cluster -- "Archive Spool?", "Spool archived", "Failed to archive spool" and
# "It can be un-archived in Spoolman's web UI." are empty in all nine locales
# too, and those are plain text= so they sit outside this gate. Translating two
# of the cluster in isolation would leave one dialog half-localized; the cluster
# wants one pass.
KNOWN_UNTRANSLATED: set[str] = set()


def _tags_by_attribute() -> dict:
    found = {attr: set() for attr in EXPLICIT_TAG_ATTRIBUTES}
    for xml in sorted((REPO_ROOT / "ui_xml").rglob("*.xml")):
        if "translations" in xml.parts:
            continue
        content = xml.read_text(encoding="utf-8")
        if I18N_SKIP_FILE_RE.search(content):
            continue
        for attr, pattern in TAG_RES.items():
            for match in pattern.finditer(content):
                text = _decode_xml_entities(match.group(1))
                if text and not text.startswith(("$", "#")) and not should_skip_text(text):
                    found[attr].add(text)
    return found


def test_every_explicit_tag_attribute_has_a_floor():
    """A new entry in EXPLICIT_TAG_ATTRIBUTES must arrive with a floor, or the
    coverage assertion below would pass it without ever scanning for it."""
    assert set(MIN_TAGS) == set(EXPLICIT_TAG_ATTRIBUTES), (
        "MIN_TAGS is out of sync with EXPLICIT_TAG_ATTRIBUTES: "
        f"{sorted(set(MIN_TAGS) ^ set(EXPLICIT_TAG_ATTRIBUTES))}"
    )


def test_every_explicit_tag_translated_in_every_locale():
    found = _tags_by_attribute()

    thin = [
        f"{attr}: found {len(tags)}, expected at least {MIN_TAGS[attr]}"
        for attr, tags in found.items()
        if len(tags) < MIN_TAGS[attr]
    ]
    assert not thin, "scan found too few literals:\n  " + "\n  ".join(thin)

    locales = sorted((REPO_ROOT / "translations").glob("*.yml"))
    assert len(locales) == 9, f"expected 9 locale files, found {[p.name for p in locales]}"

    missing = []
    translated_after_all = set()
    for yml in locales:
        data = load_yaml_file_readonly(yml)
        entries = data["translations"]
        for attr, tags in sorted(found.items()):
            for tag in sorted(tags):
                value = entries.get(tag)
                if tag in KNOWN_UNTRANSLATED:
                    # en.yml holds the key as its own value, so it is never
                    # evidence that a tag left the baseline.
                    if value and yml.stem != "en":
                        translated_after_all.add(tag)
                    continue
                if not value:
                    missing.append(f"{yml.name}: {attr}={tag!r}")
    assert not missing, "explicit tag keys missing or empty:\n  " + "\n  ".join(missing)
    assert not translated_after_all, (
        "these are translated now -- drop them from KNOWN_UNTRANSLATED:\n  "
        + "\n  ".join(sorted(translated_after_all))
    )


def test_known_untranslated_entries_are_still_reachable():
    """A baselined tag that no longer appears in ui_xml is dead weight: it would
    sit in the list forever, silently excusing the string if it ever came back."""
    live = set()
    for tags in _tags_by_attribute().values():
        live |= tags
    stale = KNOWN_UNTRANSLATED - live
    assert not stale, (
        "KNOWN_UNTRANSLATED names tags no explicit *_tag attribute uses any more:\n  "
        + "\n  ".join(sorted(stale))
    )
