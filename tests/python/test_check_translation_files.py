#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Locale files stay in the shape Weblate writes, and our own edits keep them there.

Weblate commits a locale file by re-serializing all of it. These tests pin that
canonical_text() is that serialization, that translation-sync's line splices
produce entries already in it, the plural and locale checks of
scripts/check_translation_files.py, and the glossary's TBX export for Weblate.
"""

import io
import subprocess
import sys
from pathlib import Path
from textwrap import dedent

import pytest

SCRIPTS = Path(__file__).resolve().parent.parent.parent / "scripts"
sys.path.insert(0, str(SCRIPTS))

pytest.importorskip("ruamel.yaml")

import check_translation_files as gate  # noqa: E402
from translations.yaml_manager import (  # noqa: E402
    canonical_text,
    merge_new_keys_with_sources,
)

LONG_KEY = "A sentence long enough to pass ruamel's 128-character simple-key limit, " + (
    "which Weblate then writes in the explicit question-mark form."
)

CANONICAL_DE = dedent(f"""\
    locale: de
    translations:
      '%d min': '%d Min'
      ? {LONG_KEY}
      : Ein langer Satz.
      Cancel: Abbrechen
      Settings: ''
""")


def test_folded_value_is_unfolded():
    folded = dedent("""\
        locale: de
        translations:
          '- Ensure ADXL345 is connected': '- Stellen Sie sicher, dass der ADXL345 angeschlossen
            ist'
    """)
    assert canonical_text(folded) == dedent("""\
        locale: de
        translations:
          '- Ensure ADXL345 is connected': '- Stellen Sie sicher, dass der ADXL345 angeschlossen ist'
    """)


def test_canonical_file_is_a_fixed_point():
    assert len(LONG_KEY) > 128
    assert canonical_text(CANONICAL_DE) == CANONICAL_DE


def test_spliced_entries_are_already_canonical(tmp_path):
    """translation-sync's splice and Weblate's dump must agree line for line."""
    d = tmp_path / "translations"
    d.mkdir()
    (d / "de.yml").write_text(CANONICAL_DE, encoding="utf-8")
    other_long = LONG_KEY.replace("A sentence", "Another sentence")
    merge_new_keys_with_sources(d, {
        "Bed: 60°C 'hot'": [("panel.xml", 3)],
        other_long: [("panel.xml", 9)],
    })
    text = (d / "de.yml").read_text(encoding="utf-8")
    assert "  # Source: panel.xml:9\n  ? " + other_long in text
    assert canonical_text(text) == text


def test_matches_translate_toolkit_with_weblates_parameters():
    """The writer Weblate runs: translate-toolkit's YAMLFile with the component's
    "YAML indentation 2 / No line wrapping / UNIX line breaks" parameters."""
    yamlstore = pytest.importorskip("translate.storage.yaml")
    store = yamlstore.YAMLFile()
    store.dump_args.update(indent=2, width=65535, line_break="\n")
    folded = CANONICAL_DE.replace("Ein langer Satz.", "Ein " + "sehr " * 30 + "langer Satz.")
    store.parse(folded.encode("utf-8"))
    out = io.BytesIO()
    store.serialize(out)
    assert out.getvalue().decode("utf-8") == canonical_text(folded)


def test_collapsed_plural_is_reported_and_others_are_not():
    base = {
        "%d gates": {"one": "%d Gate", "other": "%d Gates"},
        "%d lanes": {"one": "%d voie", "other": "%d voies"},
        "Cancel": "Annuler",
    }
    head = {
        "%d gates": "%d Gates",                                   # collapsed
        "%d lanes": {"one": "%d voie", "few": "x", "other": "y"},  # still a mapping
        "Cancel": "Abandonner",                                    # flat stays flat
    }
    assert gate.collapsed_plurals(base, head) == ["%d gates"]


def test_removed_plural_key_is_not_a_collapse():
    assert gate.collapsed_plurals({"%d gates": {"other": "x"}}, {}) == []


def test_locale_field_must_match_file_name(tmp_path):
    good = tmp_path / "fr.yml"
    good.write_text("locale: fr\ntranslations: {}\n")
    bad = tmp_path / "de.yml"
    bad.write_text("locale: en\ntranslations: {}\n")
    messages = gate.check_locale_field([good, bad])
    assert len(messages) == 1 and "de.yml" in messages[0]


def _git(root, *args):
    subprocess.run(["git", *args], cwd=root, check=True, capture_output=True,
                   env={"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                        "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
                        "PATH": "/usr/bin:/bin", "HOME": str(root)})


def test_plural_check_reads_the_base_commit(tmp_path):
    d = tmp_path / "translations"
    d.mkdir()
    fr = d / "fr.yml"
    fr.write_text(dedent("""\
        locale: fr
        translations:
          '%d gates':
            one: '%d Gate'
            other: '%d Gates'
    """))
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "add", "translations/fr.yml")
    _git(tmp_path, "commit", "-q", "-m", "base")
    assert gate.check_plurals([fr], tmp_path, "HEAD") == []

    fr.write_text("locale: fr\ntranslations:\n  '%d gates': '%d Gates (weblate)'\n")
    messages = gate.check_plurals([fr], tmp_path, "HEAD")
    assert len(messages) == 1 and "'%d gates'" in messages[0]


def test_glossary_tbx_carries_every_locale_and_skips_partial_terms(tmp_path, monkeypatch):
    """The TBX upload holds the same selection GLOSSARY.md does."""
    import xml.etree.ElementTree as ET

    from translations import glossary

    monkeypatch.setattr(glossary, "GLOSSARY_GROUPS", {"Group": ["Bed & Nozzle", "Spool"]})
    for loc in glossary.LOCALES:
        spool = "" if loc == "ja" else f"Spool-{loc}"
        (tmp_path / f"{loc}.yml").write_text(
            f"locale: {loc}\ntranslations:\n  'Bed & Nozzle': '<{loc}> & co'\n"
            f"  Spool: '{spool}'\n", encoding="utf-8")

    root = ET.fromstring(glossary.generate_glossary_tbx(tmp_path))
    entries = root.findall("./text/body/termEntry")
    assert len(entries) == 1  # Spool is untranslated in ja, so it is skipped
    lang = "{http://www.w3.org/XML/1998/namespace}lang"
    terms = {ls.get(lang): ls.find("./tig/term").text for ls in entries[0].findall("langSet")}
    assert terms["en"] == "Bed & Nozzle"
    assert terms["de"] == "<de> & co"
    assert set(terms) == {"en", *glossary.LOCALES}
