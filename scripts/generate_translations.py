#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Translation generator script for HelixScreen.

This script reads YAML translation files and generates the LVGL XML
translation packs the app loads at runtime.

Usage:
    python generate_translations.py [--yaml-dir DIR] [--xml-dir DIR]
"""

import argparse
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

from translations.locale_variants import resolve_variants


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


# CLDR plural categories a YAML plural mapping may name.
PLURAL_CATEGORIES = ("zero", "one", "two", "few", "many", "other")


def plural_variant_tag(key: str, category: str) -> str:
    """The runtime tag carrying ``key``'s form for ``category``.

    The C++ twin is helix::ui::plural_variant_tag() (include/i18n_plural.h);
    tests/unit/test_i18n_plural.cpp pins the same spelling.
    """
    return key if category == "other" else f"{key}_{category}"


def expand_plural_entries(
    locale: str, translations: dict[str, Any]
) -> tuple[dict[str, str], list[str]]:
    """Flatten one locale's plural mappings into plain runtime tags.

    A plural entry is a mapping of CLDR categories to strings. Its ``other``
    form becomes the plain tag, so every lookup that knows nothing of plurals
    still finds a translation, and each remaining category becomes
    ``<key>_<category>`` (see plural_variant_tag), which
    helix::ui::tr_plural() looks up for a count in that category.

    Returns:
        (flat {tag: text} dict, list of error strings)
    """
    flat: dict[str, str] = {}
    errors: list[str] = []
    for key, value in translations.items():
        if not is_plural_entry(value):
            flat[key] = value
            continue
        unknown = sorted(set(value) - set(PLURAL_CATEGORIES))
        if unknown:
            errors.append(
                f"{locale}: plural entry '{key}' has unknown categor"
                f"{'y' if len(unknown) == 1 else 'ies'} {', '.join(unknown)}; "
                f"use {', '.join(PLURAL_CATEGORIES)}"
            )
            continue
        if not value.get("other"):
            errors.append(
                f"{locale}: plural entry '{key}' has no 'other' form, which every "
                f"count without its own form falls back to"
            )
            continue
        for category, text in value.items():
            if text:
                flat[plural_variant_tag(key, category)] = text
    return flat, errors


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
    base_locale: str = "en",
    languages: list[str] | None = None,
) -> GenerateResult:
    """
    Run the full translation generation pipeline.

    Args:
        yaml_dir: Directory containing YAML translation files
        xml_output_dir: Directory to write XML output
        base_locale: Base locale for comparison (default: "en")
        languages: Optional whitelist of locale codes. If None, all YAMLs are
                   included. Constrained-device builds pass a short list (e.g.
                   ["en"]) to shrink the shipped translation packs.

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

    # A variant's pack carries its base language's entries under its own
    # strings; resolved before the whitelist so a variant can ship without
    # its base.
    pack_entries, variant_errors = resolve_variants(translations)
    if variant_errors:
        result.success = False
        result.errors.extend(variant_errors)
        return result

    # Filter to whitelisted languages if requested. Base locale always stays
    # so the app has a fallback if the runtime-selected locale is absent.
    if languages:
        keep = set(languages) | {base_locale}
        dropped = sorted(set(translations.keys()) - keep)
        translations = {k: v for k, v in translations.items() if k in keep}
        pack_entries = {k: v for k, v in pack_entries.items() if k in keep}
        if dropped:
            result.warnings.append(
                f"Dropped locales (not in HELIX_LANG whitelist): {', '.join(dropped)}"
            )

    runtime_tags: dict[str, dict[str, str]] = {}

    for locale, locale_translations in pack_entries.items():
        runtime_tags[locale], plural_errors = expand_plural_entries(
            locale, locale_translations
        )
        result.errors.extend(plural_errors)

    if result.errors:
        result.success = False
        return result

    # Check for missing translations
    missing = find_missing_translations(translations, base_locale)
    if missing:
        for locale, keys in missing.items():
            for key in keys:
                result.warnings.append(f"Missing '{key}' in {locale}")

    # Validate key consistency
    issues = validate_key_consistency(translations, base_locale)
    result.warnings.extend(issues)

    # Generate XML
    write_xml_file(runtime_tags, xml_output_dir / "translations.xml")

    # Also emit per-locale XMLs so the app can load only the current language
    # at runtime. See write_per_locale_xml() for the rationale.
    write_per_locale_xml(runtime_tags, xml_output_dir)

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
        base_locale=args.base_locale,
        languages=lang_whitelist,
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
