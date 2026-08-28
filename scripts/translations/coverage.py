#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Coverage module - calculates translation coverage statistics.
"""

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Set, Any

from .yaml_manager import load_yaml_file_readonly


@dataclass
class LanguageStats:
    """Statistics for a single language."""

    total: int
    translated: int
    missing: int
    percentage: float


def calculate_coverage(
    yaml_dir: Path, base_locale: str = "en"
) -> Dict[str, Dict[str, Any]]:
    """
    Calculate translation coverage for each language.

    Args:
        yaml_dir: Directory containing translation YAML files
        base_locale: The base language (considered 100% by definition)

    Returns:
        Dict mapping locale to stats dict with total, translated, missing, percentage
    """
    result: Dict[str, Dict[str, Any]] = {}

    # First, get all keys from base locale
    base_path = yaml_dir / f"{base_locale}.yml"
    if not base_path.exists():
        # Find any YAML file as base
        yaml_files = list(yaml_dir.glob("*.yml"))
        if not yaml_files:
            return result
        base_path = yaml_files[0]

    base_data = load_yaml_file_readonly(base_path)
    base_translations = base_data.get("translations", {})
    total_keys = len(base_translations) if base_translations else 0

    # Calculate stats for each language
    for yaml_path in yaml_dir.glob("*.yml"):
        data = load_yaml_file_readonly(yaml_path)
        locale = data.get("locale", yaml_path.stem)
        translations = data.get("translations", {})

        if locale == base_locale:
            # Base locale is 100% by definition
            result[locale] = {
                "total": total_keys,
                "translated": total_keys,
                "missing": 0,
                "percentage": 100.0,
            }
        else:
            # Count non-empty translations
            translated = 0
            if translations:
                for key in base_translations or {}:
                    value = translations.get(key, "")
                    if value and str(value).strip():
                        translated += 1

            missing = total_keys - translated
            percentage = (translated / total_keys * 100) if total_keys > 0 else 100.0

            result[locale] = {
                "total": total_keys,
                "translated": translated,
                "missing": missing,
                "percentage": round(percentage, 1),
            }

    return result


def get_missing_translations(
    yaml_dir: Path, base_locale: str = "en"
) -> Dict[str, List[str]]:
    """
    Get list of missing translations for each language.

    Args:
        yaml_dir: Directory containing translation YAML files
        base_locale: The base language

    Returns:
        Dict mapping locale to list of missing key names
    """
    result: Dict[str, List[str]] = {}

    # Get base locale keys
    base_path = yaml_dir / f"{base_locale}.yml"
    if not base_path.exists():
        return result

    base_data = load_yaml_file_readonly(base_path)
    base_translations = base_data.get("translations", {})

    if not base_translations:
        return result

    # Check each language
    for yaml_path in yaml_dir.glob("*.yml"):
        data = load_yaml_file_readonly(yaml_path)
        locale = data.get("locale", yaml_path.stem)

        if locale == base_locale:
            continue

        translations = data.get("translations", {})
        missing = []

        for key in base_translations:
            value = translations.get(key, "") if translations else ""
            if not value or not str(value).strip():
                missing.append(key)

        if missing:
            result[locale] = sorted(missing)

    return result


def generate_coverage_report(
    yaml_dir: Path, base_locale: str = "en", show_missing: bool = False
) -> str:
    """
    Generate a human-readable coverage report.

    Args:
        yaml_dir: Directory containing translation YAML files
        base_locale: The base language
        show_missing: Whether to list missing keys

    Returns:
        Formatted report string
    """
    stats = calculate_coverage(yaml_dir, base_locale)

    if not stats:
        return "No translation files found."

    lines = []
    lines.append("Translation Coverage Report")
    lines.append("=" * 40)
    lines.append("")

    # Sort by percentage (lowest first for attention)
    sorted_locales = sorted(stats.keys(), key=lambda k: stats[k]["percentage"])

    for locale in sorted_locales:
        s = stats[locale]
        bar_width = 20
        filled = int(s["percentage"] / 100 * bar_width)
        bar = "█" * filled + "░" * (bar_width - filled)

        lines.append(f"{locale:5} [{bar}] {s['percentage']:5.1f}%  ({s['translated']}/{s['total']})")

    lines.append("")
    total_translated = sum(s["translated"] for s in stats.values())
    total_possible = sum(s["total"] for s in stats.values())
    overall = (total_translated / total_possible * 100) if total_possible > 0 else 100
    lines.append(f"Overall: {overall:.1f}%")

    if show_missing:
        missing = get_missing_translations(yaml_dir, base_locale)
        if missing:
            lines.append("")
            lines.append("Missing Translations")
            lines.append("-" * 40)
            for locale, keys in sorted(missing.items()):
                lines.append(f"\n{locale} ({len(keys)} missing):")
                for key in keys:
                    lines.append(f"  - {key}")

    return "\n".join(lines)
