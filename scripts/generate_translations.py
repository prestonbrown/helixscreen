#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Translation generator script for HelixScreen.

This script reads YAML translation files and generates:
1. LVGL XML files for the declarative UI system
2. lv_i18n C code for runtime translation lookups

Usage:
    python generate_translations.py [--yaml-dir DIR] [--xml-dir DIR] [--c-dir DIR]
"""

import argparse
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml


# =============================================================================
# YAML Parsing
# =============================================================================


def parse_yaml_content(yaml_string: str) -> dict[str, Any]:
    """
    Parse YAML content and return dict with 'locale' and 'translations'.

    Args:
        yaml_string: YAML content as a string

    Returns:
        Dictionary with 'locale' (str) and 'translations' (dict) keys
    """
    data = yaml.safe_load(yaml_string)
    return {
        "locale": data.get("locale", ""),
        "translations": data.get("translations", {}),
    }


def is_plural_entry(value: Any) -> bool:
    """
    Check if a translation value is a plural form (dict) vs singular (string).

    Args:
        value: The translation value to check

    Returns:
        True if value is a dict (plural form), False otherwise
    """
    return isinstance(value, dict)


# =============================================================================
# XML Generation
# =============================================================================


def escape_xml_attr(text: str) -> str:
    """Escape special characters for XML attributes.

    Newline, tab and carriage return MUST go out as numeric character
    references. XML attribute-value normalization (XML 1.0 s3.3.3) replaces a
    literal one with a space, but a character reference is appended to the
    normalized value as-is - so `&#10;` is the only way a key or translation
    containing a real newline survives the parse. helix-xml reads the packs with
    expat (lib/helix-xml/src/xml/lv_xml_translation.c), which conforms.
    """
    # Order matters - ampersand first
    text = text.replace("&", "&amp;")
    text = text.replace("<", "&lt;")
    text = text.replace(">", "&gt;")
    text = text.replace('"', "&quot;")
    text = text.replace("'", "&apos;")
    text = text.replace("\n", "&#10;")
    text = text.replace("\t", "&#9;")
    text = text.replace("\r", "&#13;")
    return text


def generate_lvgl_xml(translations_dict: dict[str, dict[str, str]]) -> str:
    """
    Generate LVGL XML format from translations dictionary.

    Args:
        translations_dict: Dict of {locale: {key: value, ...}, ...}

    Returns:
        XML string in LVGL translations format
    """
    # Get all languages and sort them
    languages = sorted(translations_dict.keys())
    languages_str = " ".join(languages)

    # Collect all keys (only singular translations, not plural dicts)
    all_keys = set()
    for locale_translations in translations_dict.values():
        for key, value in locale_translations.items():
            if not is_plural_entry(value):
                all_keys.add(key)

    # Sort keys alphabetically
    sorted_keys = sorted(all_keys)

    # Build XML
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<translations languages="{languages_str}">',
    ]

    for key in sorted_keys:
        attrs = [f'tag="{escape_xml_attr(key)}"']
        for lang in languages:
            value = translations_dict.get(lang, {}).get(key, "")
            if value and not is_plural_entry(value):
                attrs.append(f'{lang}="{escape_xml_attr(value)}"')
        lines.append(f'  <translation {" ".join(attrs)}/>')

    lines.append("</translations>")

    return "\n".join(lines) + "\n"


# =============================================================================
# lv_i18n C Code Generation
# =============================================================================

# Plural rules for different languages
# Based on CLDR plural rules: https://unicode-org.github.io/cldr-staging/charts/latest/supplemental/language_plural_rules.html
PLURAL_RULES = {
    "en": """
static uint8_t {locale}_plural_fn(int32_t num)
{{
    uint32_t n = op_n(num); UNUSED(n);
    uint32_t i = op_i(n); UNUSED(i);
    uint32_t v = op_v(n); UNUSED(v);

    if ((i == 1 && v == 0)) return LV_I18N_PLURAL_TYPE_ONE;
    return LV_I18N_PLURAL_TYPE_OTHER;
}}
""",
    "de": """
static uint8_t {locale}_plural_fn(int32_t num)
{{
    uint32_t n = op_n(num); UNUSED(n);
    uint32_t i = op_i(n); UNUSED(i);
    uint32_t v = op_v(n); UNUSED(v);

    if ((i == 1 && v == 0)) return LV_I18N_PLURAL_TYPE_ONE;
    return LV_I18N_PLURAL_TYPE_OTHER;
}}
""",
    "es": """
static uint8_t {locale}_plural_fn(int32_t num)
{{
    uint32_t n = op_n(num); UNUSED(n);
    uint32_t i = op_i(n); UNUSED(i);
    uint32_t v = op_v(n); UNUSED(v);

    if ((i == 1 && v == 0)) return LV_I18N_PLURAL_TYPE_ONE;
    return LV_I18N_PLURAL_TYPE_OTHER;
}}
""",
    "fr": """
static uint8_t {locale}_plural_fn(int32_t num)
{{
    uint32_t n = op_n(num); UNUSED(n);
    uint32_t i = op_i(n); UNUSED(i);

    if ((i == 0 || i == 1)) return LV_I18N_PLURAL_TYPE_ONE;
    return LV_I18N_PLURAL_TYPE_OTHER;
}}
""",
    "ru": """
static uint8_t {locale}_plural_fn(int32_t num)
{{
    uint32_t n = op_n(num); UNUSED(n);
    uint32_t v = op_v(n); UNUSED(v);
    uint32_t i = op_i(n); UNUSED(i);
    uint32_t i10 = i % 10;
    uint32_t i100 = i % 100;
    if ((v == 0 && i10 == 1 && i100 != 11)) return LV_I18N_PLURAL_TYPE_ONE;
    if ((v == 0 && (2 <= i10 && i10 <= 4) && (!(12 <= i100 && i100 <= 14)))) return LV_I18N_PLURAL_TYPE_FEW;
    if ((v == 0 && i10 == 0) || (v == 0 && (5 <= i10 && i10 <= 9)) || (v == 0 && (11 <= i100 && i100 <= 14))) return LV_I18N_PLURAL_TYPE_MANY;
    return LV_I18N_PLURAL_TYPE_OTHER;
}}
""",
}


def escape_c_string(text: str) -> str:
    """Escape special characters for C strings.

    Handles both real control characters and literal escape sequences from YAML.
    YAML single-quoted strings store \\n as two chars (backslash + n), which must
    stay as \\n in C (the compiler interprets it as a real newline at runtime).
    """
    result = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\n":
            result.append("\\n")
        elif ch == "\r":
            result.append("\\r")
        elif ch == "\t":
            result.append("\\t")
        elif ch == '"':
            result.append('\\"')
        elif ch == "\\" and i + 1 < len(text):
            next_ch = text[i + 1]
            if next_ch in ("n", "r", "t", "\\", '"'):
                # Recognized C escape sequence — preserve as-is
                result.append("\\")
                result.append(next_ch)
                i += 1
            else:
                result.append("\\\\")
        elif ch == "\\":
            result.append("\\\\")
        else:
            result.append(ch)
        i += 1
    return "".join(result)


def get_plural_forms_for_locale(locale: str) -> list[str]:
    """Get the plural forms used by a locale."""
    # Most languages use one/other
    # Russian uses one/few/many/other
    if locale == "ru":
        return ["one", "few", "many", "other"]
    else:
        return ["one", "other"]


def generate_lv_i18n_c(
    singulars: dict[str, dict[str, str]],
    plurals: dict[str, dict[str, dict[str, str]]],
) -> str:
    """
    Generate C code for lv_i18n library.

    Args:
        singulars: Dict of {locale: {key: value, ...}, ...} for singular translations
        plurals: Dict of {locale: {key: {form: value, ...}, ...}, ...} for plural translations

    Returns:
        C source code string
    """
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Auto-generated by generate_translations.py - DO NOT EDIT",
        "",
        '#include "lv_i18n_translations.h"',
        "#include <stddef.h>  // For NULL",
        "#include <string.h>  // For strcmp",
        "",
        "////////////////////////////////////////////////////////////////////////////////",
        "// Define plural operands",
        "// http://unicode.org/reports/tr35/tr35-numbers.html#Operands",
        "",
        "#define UNUSED(x) (void)(x)",
        "",
        "static inline uint32_t op_n(int32_t val) { return (uint32_t)(val < 0 ? -val : val); }",
        "static inline uint32_t op_i(uint32_t val) { return val; }",
        "static inline uint32_t op_v(uint32_t val) { UNUSED(val); return 0; }",
        "static inline uint32_t op_w(uint32_t val) { UNUSED(val); return 0; }",
        "static inline uint32_t op_f(uint32_t val) { UNUSED(val); return 0; }",
        "static inline uint32_t op_t(uint32_t val) { UNUSED(val); return 0; }",
        "static inline uint32_t op_e(uint32_t val) { UNUSED(val); return 0; }",
        "",
    ]

    # Collect all locales
    all_locales = set(singulars.keys()) | set(plurals.keys())
    sorted_locales = sorted(all_locales)

    # Collect all singular keys (sorted)
    all_singular_keys = set()
    for locale_data in singulars.values():
        all_singular_keys.update(locale_data.keys())
    sorted_singular_keys = sorted(all_singular_keys)

    # Collect all plural keys (sorted)
    all_plural_keys = set()
    for locale_data in plurals.values():
        all_plural_keys.update(locale_data.keys())
    sorted_plural_keys = sorted(all_plural_keys)

    # Generate singulars arrays for each locale
    for locale in sorted_locales:
        locale_singulars = singulars.get(locale, {})
        if locale_singulars or locale in singulars:
            lines.append(f"static const char * {locale}_singulars[] = {{")
            for i, key in enumerate(sorted_singular_keys):
                value = locale_singulars.get(key)
                if value is not None:
                    lines.append(f'    "{escape_c_string(value)}", // {i}="{key}"')
                else:
                    lines.append(f'    NULL, // {i}="{key}"')
            lines.append("};")
            lines.append("")

    # Generate plurals arrays for each locale and plural form
    for locale in sorted_locales:
        locale_plurals = plurals.get(locale, {})
        if not locale_plurals:
            continue

        # Determine which plural forms this locale uses
        forms_used = set()
        for key_plurals in locale_plurals.values():
            forms_used.update(key_plurals.keys())

        for form in ["one", "two", "few", "many", "other"]:
            if form not in forms_used:
                continue

            lines.append(f"static const char * {locale}_plurals_{form}[] = {{")
            for i, key in enumerate(sorted_plural_keys):
                key_plurals = locale_plurals.get(key, {})
                value = key_plurals.get(form)
                if value is not None:
                    lines.append(f'    "{escape_c_string(value)}", // {i}="{key}"')
                else:
                    lines.append(f'    NULL, // {i}="{key}"')
            lines.append("};")
            lines.append("")

    # Generate plural functions for each locale that has plurals
    for locale in sorted_locales:
        if locale not in plurals or not plurals[locale]:
            continue

        # Use the locale-specific plural rule or fall back to English
        rule_template = PLURAL_RULES.get(locale, PLURAL_RULES["en"])
        lines.append(rule_template.format(locale=locale))

    # Generate language structs
    for locale in sorted_locales:
        locale_singulars = singulars.get(locale, {})
        locale_plurals = plurals.get(locale, {})

        lines.append(f"static const lv_i18n_lang_t {locale}_lang = {{")
        lines.append(f'    .locale_name = "{locale}",')

        if locale_singulars:
            lines.append(f"    .singulars = {locale}_singulars,")

        if locale_plurals:
            forms_used = set()
            for key_plurals in locale_plurals.values():
                forms_used.update(key_plurals.keys())

            for form in ["one", "two", "few", "many", "other"]:
                if form in forms_used:
                    plural_type = f"LV_I18N_PLURAL_TYPE_{form.upper()}"
                    lines.append(f"    .plurals[{plural_type}] = {locale}_plurals_{form},")

            lines.append(f"    .locale_plural_fn = {locale}_plural_fn")
        lines.append("};")
        lines.append("")

    # Generate language pack
    lines.append("const lv_i18n_language_pack_t lv_i18n_language_pack[] = {")
    for locale in sorted_locales:
        lines.append(f"    &{locale}_lang,")
    lines.append("    NULL // End mark")
    lines.append("};")
    lines.append("")

    # Generate singular and plural index arrays (for runtime lookup)
    lines.append("#ifndef LV_I18N_OPTIMIZE")
    lines.append("")

    if sorted_singular_keys:
        lines.append("static const char * singular_idx[] = {")
        for key in sorted_singular_keys:
            lines.append(f'    "{escape_c_string(key)}",')
        lines.append("};")
        lines.append("")

    if sorted_plural_keys:
        lines.append("static const char * plural_idx[] = {")
        for key in sorted_plural_keys:
            lines.append(f'    "{escape_c_string(key)}",')
        lines.append("};")
        lines.append("")

    lines.append("#endif")
    lines.append("")

    # Add runtime functions
    lines.extend([
        "////////////////////////////////////////////////////////////////////////////////",
        "// Runtime API",
        "////////////////////////////////////////////////////////////////////////////////",
        "",
        "// Internal state",
        "static const lv_i18n_language_pack_t * current_lang_pack = NULL;",
        "static const lv_i18n_lang_t * current_lang = NULL;",
        "",
        "int lv_i18n_init(const lv_i18n_language_pack_t * langs)",
        "{",
        "    if (langs == NULL) return -1;",
        "    if (langs[0] == NULL) return -1;",
        "",
        "    current_lang_pack = langs;",
        "    current_lang = langs[0];  // Default to first language",
        "    return 0;",
        "}",
        "",
        "int lv_i18n_set_locale(const char * l_name)",
        "{",
        "    if (current_lang_pack == NULL) return -1;",
        "    if (l_name == NULL) return -1;",
        "",
        "    for (int i = 0; current_lang_pack[i] != NULL; i++) {",
        "        if (strcmp(current_lang_pack[i]->locale_name, l_name) == 0) {",
        "            current_lang = current_lang_pack[i];",
        "            return 0;",
        "        }",
        "    }",
        "",
        "    return -1;  // Locale not found",
        "}",
        "",
        "const char * lv_i18n_get_current_locale(void)",
        "{",
        "    if (current_lang == NULL) return NULL;",
        "    return current_lang->locale_name;",
        "}",
        "",
    ])

    return "\n".join(lines)


def generate_lv_i18n_h(
    singulars: dict[str, dict[str, str]],
    plurals: dict[str, dict[str, dict[str, str]]],
) -> str:
    """
    Generate header file for lv_i18n library.

    Args:
        singulars: Dict of {locale: {key: value, ...}, ...} for singular translations
        plurals: Dict of {locale: {key: {form: value, ...}, ...}, ...} for plural translations

    Returns:
        C header code string
    """
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Auto-generated by generate_translations.py - DO NOT EDIT",
        "",
        "#ifndef LV_I18N_TRANSLATIONS_H",
        "#define LV_I18N_TRANSLATIONS_H",
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "#include <stdint.h>",
        "",
        "////////////////////////////////////////////////////////////////////////////////",
        "",
        "typedef enum {",
        "    LV_I18N_PLURAL_TYPE_ZERO,",
        "    LV_I18N_PLURAL_TYPE_ONE,",
        "    LV_I18N_PLURAL_TYPE_TWO,",
        "    LV_I18N_PLURAL_TYPE_FEW,",
        "    LV_I18N_PLURAL_TYPE_MANY,",
        "    LV_I18N_PLURAL_TYPE_OTHER,",
        "    _LV_I18N_PLURAL_TYPE_NUM,",
        "} lv_i18n_plural_type_t;",
        "",
        "typedef struct {",
        "    const char * locale_name;",
        "    const char * * singulars;",
        "    const char * * plurals[_LV_I18N_PLURAL_TYPE_NUM];",
        "    uint8_t (*locale_plural_fn)(int32_t num);",
        "} lv_i18n_lang_t;",
        "",
        "typedef const lv_i18n_lang_t * lv_i18n_language_pack_t;",
        "",
        "extern const lv_i18n_language_pack_t lv_i18n_language_pack[];",
        "",
        "/**",
        " * Initialize the lv_i18n translation system with a language pack.",
        " * @param langs Pointer to the array of languages (last element must be NULL)",
        " * @return 0 on success, -1 on error",
        " */",
        "int lv_i18n_init(const lv_i18n_language_pack_t * langs);",
        "",
        "/**",
        " * Change the current locale (language).",
        " * @param l_name Name of the locale to use (e.g., \"en\", \"de\", \"fr\")",
        " * @return 0 on success, -1 if locale not found",
        " */",
        "int lv_i18n_set_locale(const char * l_name);",
        "",
        "/**",
        " * Get the name of the currently active locale.",
        " * @return Locale name string, or NULL if not initialized",
        " */",
        "const char * lv_i18n_get_current_locale(void);",
        "",
        "#ifdef __cplusplus",
        "} /* extern \"C\" */",
        "#endif",
        "",
        "#endif /* LV_I18N_TRANSLATIONS_H */",
        "",
    ]

    return "\n".join(lines)


# =============================================================================
# Validation and Warnings
# =============================================================================


def find_missing_translations(
    translations: dict[str, dict[str, Any]],
    base_locale: str = "en",
) -> dict[str, list[str]]:
    """
    Find translations missing from non-base locales.

    Args:
        translations: Dict of {locale: {key: value, ...}, ...}
        base_locale: The base locale to compare against (default: "en")

    Returns:
        Dict of {locale: [missing_key, ...], ...}
    """
    if base_locale not in translations:
        return {}

    base_keys = set(translations[base_locale].keys())
    missing = {}

    for locale, locale_translations in translations.items():
        if locale == base_locale:
            continue

        locale_keys = set(locale_translations.keys())
        missing_keys = base_keys - locale_keys

        if missing_keys:
            missing[locale] = sorted(missing_keys)

    return missing


def format_missing_warnings(missing: dict[str, list[str]]) -> str:
    """
    Format missing translation warnings as a human-readable string.

    Args:
        missing: Dict of {locale: [missing_key, ...], ...}

    Returns:
        Warning message string
    """
    if not missing:
        return ""

    lines = ["Missing translations:"]

    for locale in sorted(missing.keys()):
        keys = missing[locale]
        count = len(keys)
        lines.append(f"  {locale}: {count} missing - {', '.join(keys)}")

    return "\n".join(lines)


def validate_key_consistency(
    translations: dict[str, dict[str, Any]],
    base_locale: str = "en",
) -> list[str]:
    """
    Validate that all locales have consistent keys.

    Args:
        translations: Dict of {locale: {key: value, ...}, ...}
        base_locale: The base locale to compare against (default: "en")

    Returns:
        List of issue descriptions
    """
    issues = []

    if base_locale not in translations:
        # Use first locale as base if base_locale not present
        if translations:
            base_locale = sorted(translations.keys())[0]
        else:
            return []

    base_keys = set(translations[base_locale].keys())

    for locale, locale_translations in translations.items():
        if locale == base_locale:
            continue

        locale_keys = set(locale_translations.keys())

        # Find extra keys in this locale
        extra_keys = locale_keys - base_keys
        for key in extra_keys:
            issues.append(f"Extra key '{key}' in {locale} not in {base_locale}")

    return issues


# =============================================================================
# File Operations
# =============================================================================


def load_translations_from_directory(path: Path) -> dict[str, dict[str, Any]]:
    """
    Load all YAML translation files from a directory.

    Args:
        path: Path to directory containing .yml files

    Returns:
        Dict of {locale: {key: value, ...}, ...}
    """
    result = {}
    path = Path(path)

    for yaml_file in path.glob("*.yml"):
        content = yaml_file.read_text(encoding="utf-8")
        parsed = parse_yaml_content(content)
        locale = parsed["locale"]
        result[locale] = parsed["translations"]

    return result


def write_xml_file(translations: dict[str, dict[str, str]], output_path: Path) -> None:
    """
    Write translations to an XML file.

    Args:
        translations: Dict of {locale: {key: value, ...}, ...}
        output_path: Path to write the XML file
    """
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    xml_content = generate_lvgl_xml(translations)
    output_path.write_text(xml_content, encoding="utf-8")


def write_per_locale_xml(
    translations: dict[str, dict[str, str]], output_dir: Path
) -> list[Path]:
    """
    Emit one XML file per locale so runtime can load only the current locale's
    translations instead of parsing the combined 678 KB translations.xml (which
    balloons to ~500-700 KB of heap when LVGL parses all 9 languages at once).

    Each per-locale file declares `languages="<lang>"` and contains only that
    locale's translations. Runtime loads `<saved_lang>.xml` at startup and
    additional files as the user switches languages (packs accumulate because
    LVGL's translation system has no remove API).

    Args:
        translations: Dict of {locale: {key: value, ...}, ...}
        output_dir: Directory to write `<locale>.xml` into

    Returns:
        List of paths written
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for locale in sorted(translations.keys()):
        single = {locale: translations[locale]}
        path = output_dir / f"{locale}.xml"
        path.write_text(generate_lvgl_xml(single), encoding="utf-8")
        written.append(path)
    return written


def write_lv_i18n_files(
    singulars: dict[str, dict[str, str]],
    plurals: dict[str, dict[str, dict[str, str]]],
    output_dir: Path,
) -> None:
    """
    Write lv_i18n C source and header files.

    Args:
        singulars: Dict of {locale: {key: value, ...}, ...} for singular translations
        plurals: Dict of {locale: {key: {form: value, ...}, ...}, ...} for plural translations
        output_dir: Directory to write the files
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    c_content = generate_lv_i18n_c(singulars, plurals)
    h_content = generate_lv_i18n_h(singulars, plurals)

    (output_dir / "lv_i18n_translations.c").write_text(c_content, encoding="utf-8")
    (output_dir / "lv_i18n_translations.h").write_text(h_content, encoding="utf-8")


# =============================================================================
# Full Pipeline
# =============================================================================


@dataclass
class GenerateResult:
    """Result of the generate_all function."""

    success: bool = True
    warnings: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


def generate_all(
    yaml_dir: Path,
    xml_output_dir: Path,
    c_output_dir: Path,
    base_locale: str = "en",
    languages: list[str] | None = None,
    emit_lv_i18n: bool = False,
) -> GenerateResult:
    """
    Run the full translation generation pipeline.

    Args:
        yaml_dir: Directory containing YAML translation files
        xml_output_dir: Directory to write XML output
        c_output_dir: Directory to write C output (only used if emit_lv_i18n)
        base_locale: Base locale for comparison (default: "en")
        languages: Optional whitelist of locale codes. If None, all YAMLs are
                   included. Constrained-device builds pass a short list (e.g.
                   ["en"]) to shrink the compiled translation table.
        emit_lv_i18n: When True, also write the legacy lv_i18n C/H translation
                   table. Default False — that table is NOT compiled or linked
                   (runtime uses the XML packs via LVGL's lv_translation_* API),
                   so emitting it only produces a large churn diff. Opt in only
                   if the lv_i18n subsystem is ever re-enabled.

    Returns:
        GenerateResult with success status and any warnings/errors
    """
    result = GenerateResult()

    # Load translations
    translations = load_translations_from_directory(yaml_dir)

    if not translations:
        result.success = False
        result.errors.append(f"No translation files found in {yaml_dir}")
        return result

    # Filter to whitelisted languages if requested. Base locale always stays
    # so the app has a fallback if the runtime-selected locale is absent.
    if languages:
        keep = set(languages) | {base_locale}
        dropped = sorted(set(translations.keys()) - keep)
        translations = {k: v for k, v in translations.items() if k in keep}
        if dropped:
            result.warnings.append(
                f"Dropped locales (not in HELIX_LANG whitelist): {', '.join(dropped)}"
            )

    # Separate singulars and plurals
    singulars: dict[str, dict[str, str]] = {}
    plurals: dict[str, dict[str, dict[str, str]]] = {}

    for locale, locale_translations in translations.items():
        singulars[locale] = {}
        plurals[locale] = {}

        for key, value in locale_translations.items():
            if is_plural_entry(value):
                plurals[locale][key] = value
            else:
                singulars[locale][key] = value

    # Check for missing translations
    missing = find_missing_translations(singulars, base_locale)
    if missing:
        for locale, keys in missing.items():
            for key in keys:
                result.warnings.append(f"Missing '{key}' in {locale}")

    # Validate key consistency
    issues = validate_key_consistency(translations, base_locale)
    result.warnings.extend(issues)

    # Generate XML
    write_xml_file(singulars, xml_output_dir / "translations.xml")

    # Also emit per-locale XMLs so the app can load only the current language
    # at runtime. See write_per_locale_xml() for the rationale.
    write_per_locale_xml(singulars, xml_output_dir)

    # Legacy lv_i18n C/H table: not compiled or linked (see emit_lv_i18n note).
    # Skipped by default to avoid a large churn diff on every regeneration.
    if emit_lv_i18n:
        write_lv_i18n_files(singulars, plurals, c_output_dir)

    return result


# =============================================================================
# CLI Entry Point
# =============================================================================


def main():
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Generate LVGL translations from YAML files"
    )
    parser.add_argument(
        "--yaml-dir",
        type=Path,
        default=Path("translations"),
        help="Directory containing YAML translation files",
    )
    parser.add_argument(
        "--xml-dir",
        type=Path,
        default=Path("ui_xml/translations"),
        help="Output directory for XML files",
    )
    parser.add_argument(
        "--c-dir",
        type=Path,
        default=Path("src/generated"),
        help="Output directory for the legacy lv_i18n C files (only used with "
             "--emit-lv-i18n)",
    )
    parser.add_argument(
        "--emit-lv-i18n",
        action="store_true",
        help="Also emit the legacy lv_i18n C/H translation table. Off by "
             "default — it is not compiled or linked (runtime uses the XML "
             "packs), so emitting it just produces churn.",
    )
    parser.add_argument(
        "--base-locale",
        default="en",
        help="Base locale for translation comparison (default: en)",
    )
    parser.add_argument(
        "--languages",
        default=None,
        help="Comma-separated whitelist of locales to include (e.g. 'en,zh'). "
             "If unset, all YAMLs are included. Base locale is always kept.",
    )

    args = parser.parse_args()

    lang_whitelist = None
    if args.languages:
        lang_whitelist = [
            s.strip() for s in args.languages.split(",") if s.strip()
        ]

    result = generate_all(
        yaml_dir=args.yaml_dir,
        xml_output_dir=args.xml_dir,
        c_output_dir=args.c_dir,
        base_locale=args.base_locale,
        languages=lang_whitelist,
        emit_lv_i18n=args.emit_lv_i18n,
    )

    for warning in result.warnings:
        print(f"WARNING: {warning}")

    for error in result.errors:
        print(f"ERROR: {error}")

    if not result.success:
        return 1

    print("Translation generation complete!")
    return 0


if __name__ == "__main__":
    exit(main())
