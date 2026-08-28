#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
CLI module - command-line interface for translation sync operations.
"""

from dataclasses import dataclass, field
from pathlib import Path
from typing import Set, Dict, Any, List

from .extractor import (
    extract_strings_from_directory,
    extract_strings_with_all_locations,
    extract_strings_from_cpp_directory,
)
from .yaml_manager import (
    merge_new_keys,
    merge_new_keys_with_sources,
    load_yaml_file_readonly,
)
from .coverage import calculate_coverage, generate_coverage_report
from .obsolete import find_obsolete_keys, report_obsolete_keys, mark_obsolete_keys, delete_obsolete_keys


@dataclass
class SyncResult:
    """Result of a sync operation."""

    new_keys_found: int = 0
    keys_added: int = 0
    files_modified: int = 0
    obsolete_keys_found: int = 0


@dataclass
class ExtractResult:
    """Result of an extract operation."""

    strings: Set[str] = field(default_factory=set)
    total_count: int = 0


@dataclass
class CoverageResult:
    """Result of a coverage check."""

    passed: bool = True
    stats: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    min_coverage: float = 0.0


@dataclass
class ObsoleteResult:
    """Result of an obsolete detection operation."""

    obsolete_keys: Set[str] = field(default_factory=set)
    would_delete: int = 0
    deleted: int = 0


def run_sync(
    xml_dir: Path,
    yaml_dir: Path,
    dry_run: bool = True,
    with_sources: bool = False,
    base_locale: str = "en",
    cpp_dir: Path = None,
) -> SyncResult:
    """
    Run the full sync workflow: extract from XML and C++, merge to YAML.

    Args:
        xml_dir: Directory containing XML files
        yaml_dir: Directory containing YAML translation files
        dry_run: If True, don't modify files
        with_sources: If True, add source file comments
        base_locale: Base locale for comparison
        cpp_dir: Optional directory containing C++ source files

    Returns:
        SyncResult with statistics
    """
    result = SyncResult()

    # Extract strings from XML
    if with_sources:
        strings_with_sources = extract_strings_with_all_locations(xml_dir, recursive=True)
        new_keys = set(strings_with_sources.keys())
    else:
        new_keys = extract_strings_from_directory(xml_dir, recursive=True)

    # Also extract from C++ if directory provided
    if cpp_dir and cpp_dir.exists():
        cpp_strings = extract_strings_from_cpp_directory(cpp_dir, recursive=True)
        new_keys.update(cpp_strings)

    # Get existing keys from YAML
    base_path = yaml_dir / f"{base_locale}.yml"
    if base_path.exists():
        base_data = load_yaml_file_readonly(base_path)
        existing_keys = set((base_data.get("translations") or {}).keys())
    else:
        existing_keys = set()

    # Find truly new keys
    truly_new = new_keys - existing_keys
    result.new_keys_found = len(truly_new)

    # Merge new keys
    if truly_new:
        if with_sources:
            # Filter to only new keys
            new_sources = {k: v for k, v in strings_with_sources.items() if k in truly_new}
            merge_result = merge_new_keys_with_sources(yaml_dir, new_sources, dry_run=dry_run)
        else:
            merge_result = merge_new_keys(yaml_dir, truly_new, dry_run=dry_run)

        result.keys_added = merge_result.keys_added
        result.files_modified = merge_result.files_modified

    # Ensure all base locale keys exist in every other language file.
    # Keys can be present in en.yml but missing from other files if they were
    # added manually or by a previous sync that only checked the base locale.
    all_base_keys = existing_keys | truly_new
    for yaml_path in yaml_dir.glob("*.yml"):
        if yaml_path.stem == base_locale:
            continue
        lang_data = load_yaml_file_readonly(yaml_path)
        lang_translations = lang_data.get("translations") or {}
        lang_keys = set(lang_translations.keys())
        missing_in_lang = all_base_keys - lang_keys
        if missing_in_lang:
            backfill_result = merge_new_keys(yaml_dir, missing_in_lang, dry_run=dry_run)
            result.keys_added += backfill_result.keys_added
            if backfill_result.files_modified > 0:
                result.files_modified += backfill_result.files_modified

    # Check for obsolete keys (include C++ sources for accurate count).
    # new_keys is already the extractor's verdict on these same two trees and is
    # not mutated above, so hand it over rather than paying for the scan twice.
    # The with_sources branch built it from a different extractor entry point,
    # so that path keeps re-scanning.
    obsolete = find_obsolete_keys(
        xml_dir,
        yaml_dir,
        base_locale,
        cpp_dir=cpp_dir,
        extracted=None if with_sources else set(new_keys),
    )
    result.obsolete_keys_found = len(obsolete)

    return result


def run_extract(xml_dir: Path, recursive: bool = True) -> ExtractResult:
    """
    Extract and list all translatable strings from XML files.

    Args:
        xml_dir: Directory containing XML files
        recursive: Whether to scan subdirectories

    Returns:
        ExtractResult with strings found
    """
    strings = extract_strings_from_directory(xml_dir, recursive=recursive)

    return ExtractResult(strings=strings, total_count=len(strings))


def run_coverage(
    yaml_dir: Path,
    fail_under: float = 0.0,
    base_locale: str = "en",
) -> CoverageResult:
    """
    Check translation coverage.

    Args:
        yaml_dir: Directory containing YAML translation files
        fail_under: Minimum coverage percentage required (0-100)
        base_locale: Base locale for comparison

    Returns:
        CoverageResult with stats and pass/fail status
    """
    stats = calculate_coverage(yaml_dir, base_locale)

    # Check if all languages meet minimum
    passed = True
    min_coverage = 100.0

    for locale, locale_stats in stats.items():
        if locale == base_locale:
            continue  # Skip base locale

        pct = locale_stats["percentage"]
        min_coverage = min(min_coverage, pct)

        if pct < fail_under:
            passed = False

    return CoverageResult(passed=passed, stats=stats, min_coverage=min_coverage)


def run_obsolete(
    xml_dir: Path,
    yaml_dir: Path,
    action: str = "report",
    dry_run: bool = True,
    base_locale: str = "en",
    cpp_dir: Path = None,
) -> ObsoleteResult:
    """
    Detect and optionally handle obsolete translation keys.

    Args:
        xml_dir: Directory containing XML files
        yaml_dir: Directory containing YAML translation files
        action: One of "report", "mark", "delete"
        dry_run: If True, don't modify files (for delete/mark actions)
        base_locale: Base locale for comparison
        cpp_dir: Optional directory containing C++ source files

    Returns:
        ObsoleteResult with obsolete keys and action results
    """
    obsolete = find_obsolete_keys(xml_dir, yaml_dir, base_locale, cpp_dir=cpp_dir)

    result = ObsoleteResult(obsolete_keys=obsolete)

    if action == "report":
        report_obsolete_keys(obsolete)

    elif action == "mark":
        marked = mark_obsolete_keys(yaml_dir, obsolete, dry_run=dry_run)
        if dry_run:
            result.would_delete = marked
        else:
            result.deleted = marked

    elif action == "delete":
        # Count how many would be deleted
        result.would_delete = len(obsolete) * len(list(yaml_dir.glob("*.yml")))

        if not dry_run:
            deleted = delete_obsolete_keys(yaml_dir, obsolete, dry_run=False)
            result.deleted = deleted

    return result
