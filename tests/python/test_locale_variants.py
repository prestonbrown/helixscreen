# SPDX-License-Identifier: GPL-3.0-or-later
"""Regional variants (en-GB, pt-BR): the pack the generator writes for one, and
how coverage counts its empty values.

A variant's empty value inherits its base language. The generator resolves that
inheritance into the variant's pack, and coverage counts an inherited key as
translated, while a plain language's empty value stays a missing translation.
"""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from textwrap import dedent, indent

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from generate_translations import generate_all  # noqa: E402
from translations.coverage import calculate_coverage, get_missing_translations  # noqa: E402
from translations.locale_variants import base_locale, merge_variant  # noqa: E402


def _write(yaml_dir: Path, locale: str, body: str) -> None:
    (yaml_dir / f"{locale}.yml").write_text(
        f"locale: {locale}\ntranslations:\n" + indent(dedent(body), "  "), encoding="utf-8"
    )


def _pack(xml_dir: Path, locale: str) -> dict:
    root = ET.parse(xml_dir / f"{locale}.xml").getroot()
    assert root.get("languages") == locale
    return {t.get("tag"): t.get(locale) for t in root.iter("translation")}


def _catalogs(tmp_path: Path) -> Path:
    yaml_dir = tmp_path / "translations"
    yaml_dir.mkdir()
    _write(yaml_dir, "en", """\
          Color: Color
          Save: Save
          Cancel: Cancel
          "{} prints": "{} prints"
        """)
    _write(yaml_dir, "en-GB", """\
          Color: Colour
          Save: ''
          Cancel: ''
          "{} prints": ''
        """)
    _write(yaml_dir, "pt", """\
          Color: Cor
          Save: Salvar
          Cancel: ''
          "{} prints":
            one: "{} impressão"
            many: "{} de impressões"
            other: "{} impressões"
        """)
    _write(yaml_dir, "pt-BR", """\
          Color: ''
          Save: Gravar
          Cancel: ''
          "{} prints": "{} impressões BR"
        """)
    return yaml_dir


def test_base_locale_names_the_language_a_variant_inherits():
    assert base_locale("en-GB") == "en"
    assert base_locale("pt-BR") == "pt"
    assert base_locale("en_GB") == "en"
    assert base_locale("en") is None


def test_variant_value_wins_and_empty_value_inherits_the_base(tmp_path):
    yaml_dir = _catalogs(tmp_path)
    xml_dir = tmp_path / "xml"
    result = generate_all(yaml_dir=yaml_dir, xml_output_dir=xml_dir)
    assert result.success, result.errors

    pt_br = _pack(xml_dir, "pt-BR")
    assert pt_br["Save"] == "Gravar"  # the variant's own value
    assert pt_br["Color"] == "Cor"  # empty in the variant: the base's value
    assert "Cancel" not in pt_br  # empty in both: LVGL falls back to the tag

    # A flat variant string replaces the base's plural entry whole: no base
    # category tag may outlive it and answer a count in that category.
    assert pt_br["{} prints"] == "{} impressões BR"
    assert "{} prints_many" not in pt_br
    assert "{} prints_one" not in pt_br


def test_variant_of_the_identity_locale_ships_only_its_own_strings(tmp_path):
    yaml_dir = _catalogs(tmp_path)
    xml_dir = tmp_path / "xml"
    assert generate_all(yaml_dir=yaml_dir, xml_output_dir=xml_dir).success

    # en maps every key to itself, which the tag already says, so en-GB's
    # pack holds nothing but the strings it respells.
    assert _pack(xml_dir, "en-GB") == {"Color": "Colour"}


def test_variant_whitelisted_without_its_base_still_inherits(tmp_path):
    yaml_dir = _catalogs(tmp_path)
    xml_dir = tmp_path / "xml"
    result = generate_all(yaml_dir=yaml_dir, xml_output_dir=xml_dir, languages=["pt-BR"])
    assert result.success
    assert not (xml_dir / "pt.xml").exists()
    assert _pack(xml_dir, "pt-BR")["Color"] == "Cor"


def test_variant_without_a_base_file_fails_generation(tmp_path):
    yaml_dir = tmp_path / "translations"
    yaml_dir.mkdir()
    _write(yaml_dir, "en", "  Save: Save\n")
    _write(yaml_dir, "fr-CA", "  Save: Enregistrer\n")
    result = generate_all(yaml_dir=yaml_dir, xml_output_dir=tmp_path / "xml")
    assert not result.success
    assert any("fr-CA" in e and "fr.yml" in e for e in result.errors)


def test_merge_variant_drops_identity_entries_of_the_base():
    assert merge_variant({"Save": "Save", "Color": "Cor"}, {"Save": ""}) == {"Color": "Cor"}


def test_coverage_counts_an_inherited_key_as_translated(tmp_path):
    yaml_dir = _catalogs(tmp_path)
    stats = calculate_coverage(yaml_dir)
    missing = get_missing_translations(yaml_dir)

    # en-GB leaves three of four keys empty and inherits all of them from en.
    assert stats["en-GB"]["translated"] == 4
    assert stats["en-GB"]["overrides"] == 1
    assert "en-GB" not in missing

    # pt-BR inherits Color from pt; Cancel is empty in both, so it is missing.
    assert missing["pt-BR"] == ["Cancel"]

    # A plain language's empty value is still a missing translation.
    assert missing["pt"] == ["Cancel"]
    assert "overrides" not in stats["pt"]


def test_shipped_en_gb_catalog_carries_every_en_key():
    import yaml

    en = yaml.safe_load((REPO_ROOT / "translations" / "en.yml").read_text(encoding="utf-8"))
    gb = yaml.safe_load((REPO_ROOT / "translations" / "en-GB.yml").read_text(encoding="utf-8"))
    assert gb["locale"] == "en-GB"
    assert list(gb["translations"]) == list(en["translations"])
