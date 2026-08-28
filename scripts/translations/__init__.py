# SPDX-License-Identifier: GPL-3.0-or-later
"""
Translation sync tooling - extract strings from XML and C++, manage YAML translations.
"""

from .extractor import (
    extract_strings_from_xml,
    extract_strings_with_locations,
    extract_strings_from_directory,
    extract_strings_from_cpp,
    extract_strings_from_cpp_directory,
)
from .yaml_manager import (
    merge_new_keys,
    merge_new_keys_with_sources,
    load_yaml_file,
    load_yaml_file_readonly,
)
from .coverage import (
    calculate_coverage,
    get_missing_translations,
    generate_coverage_report,
)
from .obsolete import (
    find_obsolete_keys,
    report_obsolete_keys,
    mark_obsolete_keys,
    delete_obsolete_keys,
)
from .cli import run_sync, run_extract, run_coverage, run_obsolete

__all__ = [
    "extract_strings_from_xml",
    "extract_strings_with_locations",
    "extract_strings_from_directory",
    "extract_strings_from_cpp",
    "extract_strings_from_cpp_directory",
    "merge_new_keys",
    "merge_new_keys_with_sources",
    "load_yaml_file",
    "load_yaml_file_readonly",
    "calculate_coverage",
    "get_missing_translations",
    "generate_coverage_report",
    "find_obsolete_keys",
    "report_obsolete_keys",
    "mark_obsolete_keys",
    "delete_obsolete_keys",
    "run_sync",
    "run_extract",
    "run_coverage",
    "run_obsolete",
]
