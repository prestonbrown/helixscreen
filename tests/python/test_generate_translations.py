#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Tests for scripts/generate-translations.py

These tests define the expected behavior of the translation generator script.
They follow TDD principles - written before the implementation exists.

The generator script will:
1. Read YAML files from translations/*.yml
2. Generate LVGL XML files (ui_xml/translations/translations_*.xml)
3. Generate lv_i18n C code (src/generated/lv_i18n_translations.c and .h)
"""

import pytest
import sys
from pathlib import Path
from textwrap import dedent

# Add scripts directory to path for importing the module under test
scripts_dir = Path(__file__).parent.parent.parent / "scripts"
sys.path.insert(0, str(scripts_dir))


# =============================================================================
# Test Fixtures
# =============================================================================

@pytest.fixture
def simple_yaml_en():
    """Simple English YAML with basic key:value translations."""
    return dedent("""\
        locale: en
        translations:
          "Settings": Settings
          "Home": Home
          "Cancel": Cancel
          "Save": Save
    """)


@pytest.fixture
def simple_yaml_de():
    """Simple German YAML with basic key:value translations."""
    return dedent("""\
        locale: de
        translations:
          "Settings": Einstellungen
          "Home": Startseite
          "Cancel": Abbrechen
          "Save": Speichern
    """)


@pytest.fixture
def yaml_with_plurals_en():
    """English YAML with plural forms."""
    return dedent("""\
        locale: en
        translations:
          "file_count":
            one: "%d file"
            other: "%d files"
          "minute_count":
            one: "%d minute remaining"
            other: "%d minutes remaining"
    """)


@pytest.fixture
def yaml_with_plurals_ru():
    """Russian YAML with complex plural forms (one/few/many/other)."""
    return dedent("""\
        locale: ru
        translations:
          "file_count":
            one: "%d файл"
            few: "%d файла"
            many: "%d файлов"
            other: "%d файлов"
          "minute_count":
            one: "%d минута осталась"
            few: "%d минуты осталось"
            many: "%d минут осталось"
            other: "%d минут осталось"
    """)


@pytest.fixture
def yaml_with_missing_keys():
    """YAML file with some missing translations (compared to English)."""
    return dedent("""\
        locale: fr
        translations:
          "Settings": Paramètres
          "Home": Accueil
          # Missing: Cancel, Save
    """)


@pytest.fixture
def yaml_with_special_chars():
    """YAML with special characters that need escaping."""
    return dedent('''\
        locale: en
        translations:
          "quoted_string": "Temperature: 200°C"
          "with_ampersand": "Save & Exit"
          "with_quotes": 'He said "Hello"'
          "with_newline": "Line1\\nLine2"
    ''')


@pytest.fixture
def expected_xml_simple():
    """Expected XML output for simple translations."""
    return dedent("""\
        <?xml version="1.0" encoding="UTF-8"?>
        <translations languages="en de">
          <translation tag="Cancel" en="Cancel" de="Abbrechen"/>
          <translation tag="Home" en="Home" de="Startseite"/>
          <translation tag="Save" en="Save" de="Speichern"/>
          <translation tag="Settings" en="Settings" de="Einstellungen"/>
        </translations>
    """)


@pytest.fixture
def sample_translations_dict():
    """Sample parsed translations as Python dict."""
    return {
        "en": {
            "Settings": "Settings",
            "Home": "Home",
            "Cancel": "Cancel",
        },
        "de": {
            "Settings": "Einstellungen",
            "Home": "Startseite",
            "Cancel": "Abbrechen",
        },
    }


@pytest.fixture
def sample_plurals_dict():
    """Sample parsed plural translations as Python dict."""
    return {
        "en": {
            "file_count": {
                "one": "%d file",
                "other": "%d files",
            },
        },
        "ru": {
            "file_count": {
                "one": "%d файл",
                "few": "%d файла",
                "many": "%d файлов",
                "other": "%d файлов",
            },
        },
    }


# =============================================================================
# Test: YAML Parsing
# =============================================================================

class TestParseYamlSimpleStrings:
    """Test parsing simple key:value translations from YAML."""

    def test_parse_simple_yaml_returns_dict(self, simple_yaml_en):
        """Parsing YAML returns a dictionary with locale and translations."""
        from generate_translations import parse_yaml_content

        result = parse_yaml_content(simple_yaml_en)

        assert isinstance(result, dict)
        assert "locale" in result
        assert "translations" in result

    def test_parse_simple_yaml_extracts_locale(self, simple_yaml_en):
        """Locale is correctly extracted from YAML."""
        from generate_translations import parse_yaml_content

        result = parse_yaml_content(simple_yaml_en)

        assert result["locale"] == "en"

    def test_parse_simple_yaml_extracts_translations(self, simple_yaml_en):
        """Simple string translations are correctly extracted."""
        from generate_translations import parse_yaml_content

        result = parse_yaml_content(simple_yaml_en)

        assert result["translations"]["Settings"] == "Settings"
        assert result["translations"]["Home"] == "Home"
        assert result["translations"]["Cancel"] == "Cancel"
        assert result["translations"]["Save"] == "Save"

    def test_parse_german_yaml(self, simple_yaml_de):
        """German YAML is correctly parsed."""
        from generate_translations import parse_yaml_content

        result = parse_yaml_content(simple_yaml_de)

        assert result["locale"] == "de"
        assert result["translations"]["Settings"] == "Einstellungen"
        assert result["translations"]["Home"] == "Startseite"

    def test_parse_yaml_with_special_characters(self, yaml_with_special_chars):
        """Special characters are preserved in translations."""
        from generate_translations import parse_yaml_content

        result = parse_yaml_content(yaml_with_special_chars)

        assert "200°C" in result["translations"]["quoted_string"]
        assert "&" in result["translations"]["with_ampersand"]


class TestParseYamlPluralForms:
    """Test parsing plural forms from YAML."""

    def test_parse_plural_forms_english(self, yaml_with_plurals_en):
        """English plural forms (one/other) are correctly parsed."""
        from generate_translations import parse_yaml_content

        result = parse_yaml_content(yaml_with_plurals_en)
        file_count = result["translations"]["file_count"]

        assert isinstance(file_count, dict)
        assert file_count["one"] == "%d file"
        assert file_count["other"] == "%d files"

    def test_parse_plural_forms_russian(self, yaml_with_plurals_ru):
        """Russian plural forms (one/few/many/other) are correctly parsed."""
        from generate_translations import parse_yaml_content

        result = parse_yaml_content(yaml_with_plurals_ru)
        file_count = result["translations"]["file_count"]

        assert isinstance(file_count, dict)
        assert file_count["one"] == "%d файл"
        assert file_count["few"] == "%d файла"
        assert file_count["many"] == "%d файлов"
        assert file_count["other"] == "%d файлов"

# =============================================================================
# Test: XML Generation
# =============================================================================

class TestGenerateXmlOutput:
    """Test XML generation for LVGL translations format."""

    def test_generate_xml_has_correct_root_element(self, sample_translations_dict):
        """Generated XML has <translations> root with languages attribute."""
        from generate_translations import generate_lvgl_xml

        xml = generate_lvgl_xml(sample_translations_dict)

        assert "<translations" in xml
        assert 'languages="' in xml
        assert "</translations>" in xml

    def test_generate_xml_includes_all_languages(self, sample_translations_dict):
        """Languages attribute includes all provided languages."""
        from generate_translations import generate_lvgl_xml

        xml = generate_lvgl_xml(sample_translations_dict)

        # Should have both en and de in languages attribute
        assert 'languages="' in xml
        assert "en" in xml
        assert "de" in xml

    def test_generate_xml_translation_elements(self, sample_translations_dict):
        """Each translation has correct tag and language attributes."""
        from generate_translations import generate_lvgl_xml

        xml = generate_lvgl_xml(sample_translations_dict)

        assert '<translation tag="Settings"' in xml
        assert 'en="Settings"' in xml
        assert 'de="Einstellungen"' in xml

    def test_generate_xml_sorted_alphabetically(self, sample_translations_dict):
        """Translation tags are sorted alphabetically."""
        from generate_translations import generate_lvgl_xml

        xml = generate_lvgl_xml(sample_translations_dict)

        # Find positions of each tag
        cancel_pos = xml.find('tag="Cancel"')
        home_pos = xml.find('tag="Home"')
        settings_pos = xml.find('tag="Settings"')

        assert cancel_pos < home_pos < settings_pos

    def test_generate_xml_escapes_special_chars(self):
        """Special XML characters are properly escaped."""
        from generate_translations import generate_lvgl_xml

        translations = {
            "en": {
                "ampersand": "Save & Exit",
                "quotes": 'He said "Hello"',
                "angle": "Temperature < 100",
            },
        }

        xml = generate_lvgl_xml(translations)

        assert "&amp;" in xml
        assert "&quot;" in xml or "&#34;" in xml
        assert "&lt;" in xml

    def test_generate_xml_valid_format(self, sample_translations_dict):
        """Generated XML is valid and can be parsed."""
        from generate_translations import generate_lvgl_xml
        import xml.etree.ElementTree as ET

        xml = generate_lvgl_xml(sample_translations_dict)

        # Should not raise an exception
        root = ET.fromstring(xml)

        assert root.tag == "translations"


# =============================================================================
# Test: lv_i18n C Code Generation
# =============================================================================

class TestGenerateLvI18nCode:
    """Test C code generation for lv_i18n library."""

    def test_generate_c_creates_singulars_array(self, sample_translations_dict):
        """C code includes singulars array for each language."""
        from generate_translations import generate_lv_i18n_c

        c_code = generate_lv_i18n_c(sample_translations_dict, {})

        assert "static const char * en_singulars[]" in c_code
        assert "static const char * de_singulars[]" in c_code

    def test_generate_c_singulars_content(self, sample_translations_dict):
        """Singulars array contains correct translation strings."""
        from generate_translations import generate_lv_i18n_c

        c_code = generate_lv_i18n_c(sample_translations_dict, {})

        # English singulars should contain the English translations
        assert '"Settings"' in c_code
        assert '"Einstellungen"' in c_code

    def test_generate_c_creates_plural_arrays(self, sample_plurals_dict):
        """C code includes plural arrays for each form."""
        from generate_translations import generate_lv_i18n_c

        c_code = generate_lv_i18n_c({}, sample_plurals_dict)

        assert "plurals_one" in c_code
        assert "plurals_other" in c_code

    def test_generate_c_russian_plural_forms(self, sample_plurals_dict):
        """Russian plural forms include few and many."""
        from generate_translations import generate_lv_i18n_c

        c_code = generate_lv_i18n_c({}, sample_plurals_dict)

        assert "ru_plurals_few" in c_code
        assert "ru_plurals_many" in c_code

    def test_generate_c_language_struct(self, sample_translations_dict):
        """C code includes language struct definition."""
        from generate_translations import generate_lv_i18n_c

        c_code = generate_lv_i18n_c(sample_translations_dict, {})

        assert "lv_i18n_lang_t en_lang" in c_code or "static const lv_i18n_lang_t en_lang" in c_code
        assert ".locale_name" in c_code
        assert ".singulars" in c_code

    def test_generate_c_language_pack(self, sample_translations_dict):
        """C code includes language pack array."""
        from generate_translations import generate_lv_i18n_c

        c_code = generate_lv_i18n_c(sample_translations_dict, {})

        assert "lv_i18n_language_pack" in c_code
        assert "NULL" in c_code  # End marker

    def test_generate_c_plural_function(self, sample_plurals_dict):
        """C code includes plural function for languages with plurals."""
        from generate_translations import generate_lv_i18n_c

        c_code = generate_lv_i18n_c({}, sample_plurals_dict)

        assert "plural_fn" in c_code
        assert "LV_I18N_PLURAL_TYPE_ONE" in c_code

    def test_generate_c_escapes_strings(self):
        """C strings are properly escaped."""
        from generate_translations import generate_lv_i18n_c

        translations = {
            "en": {
                "quote": 'He said "Hello"',
                "backslash": "path\\to\\file",
                "newline": "Line1\nLine2",
            },
        }

        c_code = generate_lv_i18n_c(translations, {})

        assert r'\"' in c_code  # Escaped double quotes
        assert r'\\' in c_code  # Escaped backslashes

    def test_generate_header_file(self, sample_translations_dict):
        """Header file includes necessary declarations."""
        from generate_translations import generate_lv_i18n_h

        header = generate_lv_i18n_h(sample_translations_dict, {})

        assert "#ifndef" in header  # Include guard
        assert "#define" in header
        assert "#endif" in header
        assert "lv_i18n_language_pack" in header
        assert 'extern "C"' in header  # C++ compatibility


# =============================================================================
# Test: Validation and Warnings
# =============================================================================

class TestMissingTranslationWarning:
    """Test warning generation for missing translations."""

    def test_detect_missing_translations(self):
        """Detects when a language is missing translations present in English."""
        from generate_translations import find_missing_translations

        translations = {
            "en": {"Settings": "Settings", "Home": "Home", "Cancel": "Cancel"},
            "de": {"Settings": "Einstellungen", "Home": "Startseite"},
            # Missing: Cancel
        }

        missing = find_missing_translations(translations, base_locale="en")

        assert "de" in missing
        assert "Cancel" in missing["de"]

    def test_no_warnings_when_complete(self):
        """No warnings when all translations are present."""
        from generate_translations import find_missing_translations

        translations = {
            "en": {"Settings": "Settings", "Home": "Home"},
            "de": {"Settings": "Einstellungen", "Home": "Startseite"},
        }

        missing = find_missing_translations(translations, base_locale="en")

        assert len(missing.get("de", [])) == 0

    def test_warning_message_format(self):
        """Warning messages are properly formatted."""
        from generate_translations import format_missing_warnings

        missing = {
            "de": ["Cancel", "Save"],
            "fr": ["Cancel", "Save", "Home"],
        }

        warnings = format_missing_warnings(missing)

        assert warnings.splitlines() == [
            "Missing translations:",
            "  de: 2 missing - Cancel, Save",
            "  fr: 3 missing - Cancel, Save, Home",
        ]


class TestAllLanguagesHaveSameKeys:
    """Test consistency validation across language files."""

    def test_detect_inconsistent_keys(self):
        """Detects when languages have different key sets."""
        from generate_translations import validate_key_consistency

        translations = {
            "en": {"Settings": "Settings", "Home": "Home"},
            "de": {"Settings": "Einstellungen", "Extra": "Extra Key"},
            # de has "Extra" that en doesn't have
        }

        issues = validate_key_consistency(translations)

        assert len(issues) > 0
        assert any("Extra" in str(issue) for issue in issues)

    def test_detect_extra_keys_in_non_base_locale(self):
        """Detects keys in non-base locales that aren't in base locale."""
        from generate_translations import validate_key_consistency

        translations = {
            "en": {"A": "A", "B": "B"},
            "de": {"A": "A-de", "B": "B-de", "C": "C-de"},  # C is extra
        }

        issues = validate_key_consistency(translations, base_locale="en")

        assert any("C" in str(issue) and "de" in str(issue) for issue in issues)

    def test_all_keys_match_no_issues(self):
        """No issues when all languages have same keys."""
        from generate_translations import validate_key_consistency

        translations = {
            "en": {"Settings": "Settings", "Home": "Home"},
            "de": {"Settings": "Einstellungen", "Home": "Startseite"},
            "fr": {"Settings": "Paramètres", "Home": "Accueil"},
        }

        issues = validate_key_consistency(translations)

        assert len(issues) == 0


# =============================================================================
# Test: File Operations
# =============================================================================

class TestFileOperations:
    """Test file reading and writing operations."""

    def test_load_yaml_files_from_directory(self, tmp_path):
        """Load all YAML files from translations directory."""
        from generate_translations import load_translations_from_directory

        # Create test YAML files
        (tmp_path / "en.yml").write_text("locale: en\ntranslations:\n  Test: Test\n")
        (tmp_path / "de.yml").write_text("locale: de\ntranslations:\n  Test: Prüfung\n")

        result = load_translations_from_directory(tmp_path)

        assert "en" in result
        assert "de" in result
        assert result["en"]["Test"] == "Test"
        assert result["de"]["Test"] == "Prüfung"

    def test_write_xml_output(self, tmp_path):
        """Write generated XML to file."""
        from generate_translations import write_xml_file

        translations = {"en": {"Test": "Test"}}
        output_path = tmp_path / "translations.xml"

        write_xml_file(translations, output_path)

        assert output_path.exists()
        content = output_path.read_text()
        assert "<translations" in content

    def test_write_c_files(self, tmp_path):
        """Write generated C code to .c and .h files."""
        from generate_translations import write_lv_i18n_files

        translations = {"en": {"Test": "Test"}}
        output_dir = tmp_path

        write_lv_i18n_files(translations, {}, output_dir)

        c_file = output_dir / "lv_i18n_translations.c"
        h_file = output_dir / "lv_i18n_translations.h"

        assert c_file.exists()
        assert h_file.exists()


# =============================================================================
# Test: Integration / End-to-End
# =============================================================================

class TestIntegration:
    """Integration tests for the full translation generation pipeline."""

    def test_full_pipeline(self, tmp_path):
        """Test complete pipeline from YAML to XML and C code."""
        from generate_translations import generate_all

        # Setup: Create input YAML files
        yaml_dir = tmp_path / "translations"
        yaml_dir.mkdir()

        (yaml_dir / "en.yml").write_text(dedent("""\
            locale: en
            translations:
              "Settings": Settings
              "Home": Home
        """))

        (yaml_dir / "de.yml").write_text(dedent("""\
            locale: de
            translations:
              "Settings": Einstellungen
              "Home": Startseite
        """))

        xml_dir = tmp_path / "ui_xml" / "translations"
        c_dir = tmp_path / "src" / "generated"

        # Execute
        result = generate_all(
            yaml_dir=yaml_dir,
            xml_output_dir=xml_dir,
            c_output_dir=c_dir,
            emit_lv_i18n=True,  # opt in: asserts the legacy lv_i18n C/H artifacts
        )

        # Verify
        assert result.success
        assert (xml_dir / "translations.xml").exists()
        assert (c_dir / "lv_i18n_translations.c").exists()
        assert (c_dir / "lv_i18n_translations.h").exists()

    def test_pipeline_reports_warnings(self, tmp_path):
        """Pipeline reports warnings for missing translations."""
        from generate_translations import generate_all

        yaml_dir = tmp_path / "translations"
        yaml_dir.mkdir()

        (yaml_dir / "en.yml").write_text(dedent("""\
            locale: en
            translations:
              "Settings": Settings
              "Home": Home
              "Cancel": Cancel
        """))

        (yaml_dir / "de.yml").write_text(dedent("""\
            locale: de
            translations:
              "Settings": Einstellungen
              # Missing Home and Cancel
        """))

        result = generate_all(
            yaml_dir=yaml_dir,
            xml_output_dir=tmp_path / "xml",
            c_output_dir=tmp_path / "c",
        )

        assert len(result.warnings) > 0
        # BOTH missing keys, not "one of the two" -- and nothing about the key
        # de actually has.
        assert "Missing 'Home' in de" in result.warnings
        assert "Missing 'Cancel' in de" in result.warnings
        assert not any("Settings" in w for w in result.warnings)


# =============================================================================
# Per-Locale XML Output (for runtime lazy loading)
# =============================================================================


class TestPerLocaleXmlOutput:
    """Tests for write_per_locale_xml() — each locale gets its own XML file.

    Runtime reason: loading the combined translations.xml (all 9 langs) into
    lv_translation_pack_t costs ~500-700 KB of heap. Loading only the current
    locale costs ~60-80 KB. The generator emits one XML per locale so the app
    can load on demand.
    """

    def test_emits_one_file_per_locale(self, tmp_path):
        from generate_translations import write_per_locale_xml

        translations = {
            "en": {"Settings": "Settings", "Home": "Home"},
            "de": {"Settings": "Einstellungen", "Home": "Startseite"},
            "fr": {"Settings": "Paramètres", "Home": "Accueil"},
        }

        written = write_per_locale_xml(translations, tmp_path)

        assert len(written) == 3
        assert (tmp_path / "en.xml").exists()
        assert (tmp_path / "de.xml").exists()
        assert (tmp_path / "fr.xml").exists()

    def test_each_file_contains_only_its_locale(self, tmp_path):
        from generate_translations import write_per_locale_xml

        translations = {
            "en": {"Settings": "Settings"},
            "de": {"Settings": "Einstellungen"},
        }

        write_per_locale_xml(translations, tmp_path)

        de_content = (tmp_path / "de.xml").read_text(encoding="utf-8")
        en_content = (tmp_path / "en.xml").read_text(encoding="utf-8")

        # de.xml declares only the German language and contains only German
        assert 'languages="de"' in de_content
        assert "Einstellungen" in de_content
        # English strings must NOT leak into German file — leaking would
        # negate the heap savings of per-locale loading
        assert 'en="Settings"' not in de_content

        # Symmetric check for English
        assert 'languages="en"' in en_content
        assert "Einstellungen" not in en_content

    def test_creates_parent_directory(self, tmp_path):
        from generate_translations import write_per_locale_xml

        deep = tmp_path / "nested" / "dir"
        write_per_locale_xml({"en": {"Home": "Home"}}, deep)

        assert (deep / "en.xml").exists()

    def test_full_pipeline_emits_per_locale_xml(self, tmp_path):
        """The end-to-end generate_all() pipeline emits per-locale XMLs alongside
        the combined translations.xml."""
        from generate_translations import generate_all

        yaml_dir = tmp_path / "translations"
        yaml_dir.mkdir()
        (yaml_dir / "en.yml").write_text(dedent("""\
            locale: en
            translations:
              "Home": Home
        """))
        (yaml_dir / "de.yml").write_text(dedent("""\
            locale: de
            translations:
              "Home": Startseite
        """))

        xml_dir = tmp_path / "ui_xml" / "translations"
        result = generate_all(
            yaml_dir=yaml_dir,
            xml_output_dir=xml_dir,
            c_output_dir=tmp_path / "c",
        )

        assert result.success
        # Combined file still exists (backward compat + dev tooling)
        assert (xml_dir / "translations.xml").exists()
        # Per-locale files exist for each YAML locale
        assert (xml_dir / "en.xml").exists()
        assert (xml_dir / "de.xml").exists()


# =============================================================================
# Language Whitelist (HELIX_LANG filter)
# =============================================================================


class TestLanguageWhitelist:
    """Tests for the --languages CLI flag / `languages` parameter to generate_all().

    Purpose: allow platform Makefiles (e.g. memory-constrained device builds)
    to filter the compiled C translation table down to a subset of locales,
    reducing .rodata size. Base locale is always kept as a fallback.
    """

    def test_filters_to_requested_locales(self, tmp_path):
        from generate_translations import generate_all

        yaml_dir = tmp_path / "translations"
        yaml_dir.mkdir()
        for locale, greeting in [("en", "Hello"), ("de", "Hallo"),
                                 ("fr", "Bonjour"), ("ja", "こんにちは")]:
            (yaml_dir / f"{locale}.yml").write_text(dedent(f"""\
                locale: {locale}
                translations:
                  "Hello": {greeting}
            """))

        c_dir = tmp_path / "c"
        result = generate_all(
            yaml_dir=yaml_dir,
            xml_output_dir=tmp_path / "xml",
            c_output_dir=c_dir,
            languages=["en", "de"],
            emit_lv_i18n=True,  # whitelist asserted via the lv_i18n C table
        )

        assert result.success
        c_content = (c_dir / "lv_i18n_translations.c").read_text(encoding="utf-8")
        # Requested locales present
        assert "en_singulars" in c_content
        assert "de_singulars" in c_content
        # Excluded locales absent — this is the whole point of the filter
        assert "fr_singulars" not in c_content
        assert "ja_singulars" not in c_content

    def test_base_locale_always_kept(self, tmp_path):
        """Even if the whitelist omits the base locale, it's retained as
        fallback. Without this, non-English builds that request only 'de'
        would have no English strings to fall back to on missing keys."""
        from generate_translations import generate_all

        yaml_dir = tmp_path / "translations"
        yaml_dir.mkdir()
        (yaml_dir / "en.yml").write_text(dedent("""\
            locale: en
            translations:
              "Hello": Hello
        """))
        (yaml_dir / "de.yml").write_text(dedent("""\
            locale: de
            translations:
              "Hello": Hallo
        """))

        c_dir = tmp_path / "c"
        result = generate_all(
            yaml_dir=yaml_dir,
            xml_output_dir=tmp_path / "xml",
            c_output_dir=c_dir,
            languages=["de"],  # omits base_locale="en"
            emit_lv_i18n=True,  # whitelist asserted via the lv_i18n C table
        )

        assert result.success
        c_content = (c_dir / "lv_i18n_translations.c").read_text(encoding="utf-8")
        assert "en_singulars" in c_content  # forced in as fallback
        assert "de_singulars" in c_content

    def test_none_means_all_locales(self, tmp_path):
        """`languages=None` (the default) keeps all YAMLs — existing behavior
        for builds that don't set HELIX_LANG."""
        from generate_translations import generate_all

        yaml_dir = tmp_path / "translations"
        yaml_dir.mkdir()
        for locale in ["en", "de", "fr"]:
            (yaml_dir / f"{locale}.yml").write_text(dedent(f"""\
                locale: {locale}
                translations:
                  "Hello": Hello-{locale}
            """))

        c_dir = tmp_path / "c"
        result = generate_all(
            yaml_dir=yaml_dir,
            xml_output_dir=tmp_path / "xml",
            c_output_dir=c_dir,
            emit_lv_i18n=True,  # whitelist asserted via the lv_i18n C table
        )

        assert result.success
        c_content = (c_dir / "lv_i18n_translations.c").read_text(encoding="utf-8")
        assert "en_singulars" in c_content
        assert "de_singulars" in c_content
        assert "fr_singulars" in c_content

    def test_drop_reported_in_warnings(self, tmp_path):
        """Dropped locales are reported so builds log what they stripped."""
        from generate_translations import generate_all

        yaml_dir = tmp_path / "translations"
        yaml_dir.mkdir()
        for locale in ["en", "de", "fr", "ja"]:
            (yaml_dir / f"{locale}.yml").write_text(dedent(f"""\
                locale: {locale}
                translations:
                  "Hello": hello-{locale}
            """))

        result = generate_all(
            yaml_dir=yaml_dir,
            xml_output_dir=tmp_path / "xml",
            c_output_dir=tmp_path / "c",
            languages=["en"],
        )

        assert result.success
        assert any("Dropped" in w for w in result.warnings)
        dropped_msg = " ".join(result.warnings)
        assert "de" in dropped_msg
        assert "fr" in dropped_msg
        assert "ja" in dropped_msg
