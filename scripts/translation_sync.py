#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Translation Sync Tool - manage translation string lifecycle.

Usage:
    python translation_sync.py sync [--dry-run] [--with-sources]
    python translation_sync.py extract
    python translation_sync.py coverage [--fail-under PCT]
    python translation_sync.py obsolete [--action ACTION] [--dry-run]
    python translation_sync.py glossary [--output PATH]

Commands:
    sync        Extract strings from XML, merge new keys to YAML files
    extract     List all translatable strings found in XML files
    coverage    Show translation coverage statistics
    obsolete    Find/handle translation keys not used in XML
    glossary    Regenerate translations/GLOSSARY.md (canonical terms) from YAML

Options:
    --dry-run        Don't modify files, just show what would change
    --with-sources   Add source file comments when merging
    --fail-under     Fail if coverage below threshold (default: 0)
    --action         For obsolete: report, mark, or delete (default: report)
"""

import argparse
import sys
from pathlib import Path

# Add scripts directory to path for relative imports
scripts_dir = Path(__file__).parent
sys.path.insert(0, str(scripts_dir))

from translations.extractor import extract_strings_from_directory
from translations.yaml_manager import merge_new_keys
from translations.coverage import generate_coverage_report, calculate_coverage
from translations.obsolete import find_obsolete_keys, report_obsolete_keys
from translations.cli import run_sync, run_extract, run_coverage, run_obsolete
from translations.glossary import generate_glossary_tbx, write_glossary

# Default paths relative to project root
PROJECT_ROOT = Path(__file__).parent.parent
DEFAULT_XML_DIR = PROJECT_ROOT / "ui_xml"
DEFAULT_YAML_DIR = PROJECT_ROOT / "translations"
# lv_tr() strings live in src/ and in headers under include/; a root missing
# here means its keys are never added by a sync (obsolete detection still sees
# them through the repo-root reference scan).
DEFAULT_CPP_DIRS = [PROJECT_ROOT / "src", PROJECT_ROOT / "include"]
DEFAULT_GLOSSARY = PROJECT_ROOT / "translations" / "GLOSSARY.md"


def cmd_sync(args):
    """Run sync command."""
    xml_dir = Path(args.xml_dir)
    yaml_dir = Path(args.yaml_dir)
    cpp_dirs = [Path(p) for p in args.cpp_dir] if args.cpp_dir else DEFAULT_CPP_DIRS

    if not xml_dir.exists():
        print(f"Error: XML directory not found: {xml_dir}")
        return 1

    if not yaml_dir.exists():
        print(f"Error: YAML directory not found: {yaml_dir}")
        return 1

    print(f"Syncing translations...")
    print(f"  XML source:  {xml_dir}")
    for cpp_dir in cpp_dirs:
        print(f"  C++ source:  {cpp_dir}")
    print(f"  YAML target: {yaml_dir}")
    print(f"  Dry run:     {args.dry_run}")
    print()

    result = run_sync(
        xml_dir,
        yaml_dir,
        dry_run=args.dry_run,
        with_sources=args.with_sources,
        cpp_dirs=cpp_dirs,
    )

    if result.new_keys_found == 0:
        print("✓ All XML strings already in YAML files.")
    else:
        verb = "Would add" if args.dry_run else "Added"
        print(f"{verb} {result.new_keys_found} new keys to {result.files_modified} files.")
        if not args.dry_run:
            print("→ Translating these? Consult translations/GLOSSARY.md and reuse "
                  "the canonical term per locale — don't coin new words.")

    if result.obsolete_keys_found > 0:
        print(f"\n⚠ Found {result.obsolete_keys_found} obsolete keys (run 'obsolete' command for details)")

    return 0


def cmd_glossary(args):
    """Regenerate translations/GLOSSARY.md from the committed YAML."""
    yaml_dir = Path(args.yaml_dir)
    out = Path(args.output)
    print(f"Regenerating glossary from {yaml_dir} ...")
    write_glossary(yaml_dir, out)
    print(f"✓ Wrote {out}")
    if args.tbx:
        tbx = Path(args.tbx)
        tbx.write_text(generate_glossary_tbx(yaml_dir), encoding="utf-8")
        print(f"✓ Wrote {tbx} (upload it to the Weblate glossary)")
    return 0


def cmd_extract(args):
    """Run extract command."""
    xml_dir = Path(args.xml_dir)

    if not xml_dir.exists():
        print(f"Error: XML directory not found: {xml_dir}")
        return 1

    result = run_extract(xml_dir)

    print(f"Found {result.total_count} translatable strings in {xml_dir}:")
    print()

    for s in sorted(result.strings):
        print(f"  {s}")

    return 0


def cmd_coverage(args):
    """Run coverage command."""
    yaml_dir = Path(args.yaml_dir)

    if not yaml_dir.exists():
        print(f"Error: YAML directory not found: {yaml_dir}")
        return 1

    report = generate_coverage_report(yaml_dir, show_missing=args.show_missing)
    print(report)

    if args.fail_under > 0:
        result = run_coverage(yaml_dir, fail_under=args.fail_under)
        if not result.passed:
            print(f"\n✗ Coverage {result.min_coverage:.1f}% below threshold {args.fail_under}%")
            return 1
        print(f"\n✓ Coverage meets threshold ({args.fail_under}%)")

    return 0


def cmd_obsolete(args):
    """Run obsolete command."""
    xml_dir = Path(args.xml_dir)
    yaml_dir = Path(args.yaml_dir)
    cpp_dirs = [Path(p) for p in args.cpp_dir] if args.cpp_dir else DEFAULT_CPP_DIRS

    if not xml_dir.exists():
        print(f"Error: XML directory not found: {xml_dir}")
        return 1

    if not yaml_dir.exists():
        print(f"Error: YAML directory not found: {yaml_dir}")
        return 1

    result = run_obsolete(
        xml_dir,
        yaml_dir,
        action=args.action,
        dry_run=args.dry_run,
        cpp_dirs=cpp_dirs,
    )

    if len(result.obsolete_keys) == 0:
        print("✓ No obsolete keys found.")
        return 0

    if args.action == "report":
        # Already printed by run_obsolete
        pass
    elif args.action == "mark":
        if args.dry_run:
            print(f"Would mark {result.would_delete} keys as deprecated.")
        else:
            print(f"Marked {result.deleted} keys as deprecated.")
    elif args.action == "delete":
        if args.dry_run:
            print(f"Would delete {len(result.obsolete_keys)} keys from all YAML files.")
        else:
            print(f"Deleted {result.deleted} key instances from YAML files.")

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Translation string management tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    parser.add_argument(
        "--xml-dir",
        default=str(DEFAULT_XML_DIR),
        help=f"XML source directory (default: {DEFAULT_XML_DIR})",
    )
    parser.add_argument(
        "--yaml-dir",
        default=str(DEFAULT_YAML_DIR),
        help=f"YAML translations directory (default: {DEFAULT_YAML_DIR})",
    )
    parser.add_argument(
        "--cpp-dir",
        action="append",
        default=None,
        help="C++ source directory; repeatable (default: src/ and include/)",
    )

    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # sync command
    sync_parser = subparsers.add_parser("sync", help="Extract and merge new strings")
    sync_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Don't modify files",
    )
    sync_parser.add_argument(
        "--with-sources",
        action="store_true",
        help="Add source file comments",
    )

    # extract command
    extract_parser = subparsers.add_parser("extract", help="List all translatable strings")

    # coverage command
    coverage_parser = subparsers.add_parser("coverage", help="Show coverage statistics")
    coverage_parser.add_argument(
        "--fail-under",
        type=float,
        default=0,
        help="Fail if coverage below this percentage",
    )
    coverage_parser.add_argument(
        "--show-missing",
        action="store_true",
        help="List missing translation keys",
    )

    # obsolete command
    obsolete_parser = subparsers.add_parser("obsolete", help="Find unused keys")
    obsolete_parser.add_argument(
        "--action",
        choices=["report", "mark", "delete"],
        default="report",
        help="What to do with obsolete keys",
    )
    obsolete_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Don't modify files (for mark/delete)",
    )

    # glossary command
    glossary_parser = subparsers.add_parser(
        "glossary", help="Regenerate translations/GLOSSARY.md from the YAML"
    )
    glossary_parser.add_argument(
        "--output", default=str(DEFAULT_GLOSSARY), help="Glossary output path"
    )
    glossary_parser.add_argument(
        "--tbx", help="Also write the glossary as multilingual TBX to this path"
    )

    args = parser.parse_args()

    if args.command is None:
        parser.print_help()
        return 1

    commands = {
        "sync": cmd_sync,
        "extract": cmd_extract,
        "coverage": cmd_coverage,
        "obsolete": cmd_obsolete,
        "glossary": cmd_glossary,
    }

    return commands[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
