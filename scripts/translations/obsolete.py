#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Obsolete module - detects and handles unused translation keys.
"""

import re
from pathlib import Path
from typing import Set, Dict, Any

from .extractor import (
    extract_strings_from_directory,
    extract_strings_from_cpp_directory,
    I18N_SKIP_FILE_RE,
)
from .yaml_manager import (
    load_yaml_file,
    require_ruamel,
    _entry_spans,
    _absorb_leading_comments,
    _render_entry_lines,
)

# Trees that can reference a translation key. `assets` matters because
# printer_database.json stores tags (e.g. "AI detection") as data; `include`
# matters because headers hold label text the extractor's src-only scan never
# saw.
REFERENCE_DIRS = ("src", "include", "ui_xml", "assets", "config", "tests", "lib/helix-xml")

REFERENCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".inc", ".xml", ".json", ".yml", ".yaml"}

# A reference is a *delimited* occurrence, never a loose substring: a complete
# C/JSON string literal, an XML attribute value, or an XML text node. Matching
# substrings instead would treat the word "Unlink" inside a code comment as a
# use of the "Unlink" key and hide genuinely dead entries.
_C_LITERAL_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
_XML_ATTR_RE = re.compile(r'="([^"\n]*)"|=\'([^\'\n]*)\'')
_XML_TEXT_RE = re.compile(r">([^<>\n]+)<")

# Escapes that appear in C/JSON literals; keys are stored unescaped in YAML.
_UNESCAPE = {r"\"": '"', r"\\": "\\", r"\t": "\t"}

# A RUN of adjacent C string literals ("a" "b" "c"), separated only by
# whitespace/comments-free spacing. Matches single literals too; the consumer
# only keeps the joined form when it is longer than the first piece.
_ADJACENT_LITERALS_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"(?:\s*"(?:[^"\\\n]|\\.)*")+')


def _unescape(text: str) -> str:
    for src, dst in _UNESCAPE.items():
        text = text.replace(src, dst)
    return text


def collect_referenced_strings(
    repo_root: Path, dirs=REFERENCE_DIRS, skip_translation_dir: Path = None
) -> Set[str]:
    """
    Collect every delimited string literal appearing anywhere in the source tree.

    This is the *usage oracle* for obsolete detection, and it is deliberately
    recall-oriented: it answers "is this key mentioned at all?", not "should this
    string be translated?". The extractor answers the latter and is tuned for
    precision (so `sync` doesn't add junk keys) — reusing it to decide deletions
    made `obsolete` report live keys, because keys reached indirectly are
    invisible to it: tags in struct initializers (src/ui/tour/tour_steps.cpp),
    tags stored as JSON data (assets/config/printer_database.json), and label
    text in include/.

    Over-reporting a reference (a key that also happens to appear in an
    unrelated log message) only leaves a stale entry behind. Under-reporting one
    deletes a string users see. This errs toward the former.

    XML files carrying an `i18n: skip-file` marker are excluded, so strings that
    live only in dev/test panels are still reported obsolete — that is what the
    marker asks for.
    """
    found: Set[str] = set()

    for rel in dirs:
        base = repo_root / rel
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in REFERENCE_SUFFIXES:
                continue
            # The generated translation packs/YAML list every key by definition.
            if skip_translation_dir and skip_translation_dir in path.parents:
                continue
            try:
                content = path.read_text(encoding="utf-8")
            except (IOError, UnicodeDecodeError):
                continue

            if path.suffix == ".xml":
                if I18N_SKIP_FILE_RE.search(content):
                    continue
                for m in _XML_ATTR_RE.finditer(content):
                    found.add(m.group(1) if m.group(1) is not None else m.group(2))
                for m in _XML_TEXT_RE.finditer(content):
                    found.add(m.group(1).strip())
            else:
                for m in _C_LITERAL_RE.finditer(content):
                    found.add(_unescape(m.group(1)))
                # C++ concatenates adjacent literals: "foo " "bar" is the
                # single string "foo bar" at compile time. clang-format wraps
                # long literals this way, so a long key's reference must be
                # matched in joined form too (tour bodies in
                # src/ui/tour/tour_steps.cpp). Adding joins only widens the
                # candidate set — consistent with the recall-oriented design:
                # a false reference leaves a stale entry, a missed one deletes
                # a string users see.
                if path.suffix in {".c", ".cpp", ".h", ".hpp", ".inc"}:
                    for m in _ADJACENT_LITERALS_RE.finditer(content):
                        pieces = _C_LITERAL_RE.findall(m.group(0))
                        joined = "".join(pieces)
                        if len(joined) > len(pieces[0]):
                            found.add(_unescape(joined))

    return found


def find_obsolete_keys(
    xml_dir: Path,
    yaml_dir: Path,
    base_locale: str = "en",
    cpp_dir: Path = None,
    repo_root: Path = None,
) -> Set[str]:
    """
    Find translation keys that are not referenced anywhere in the source tree.

    Args:
        xml_dir: Directory containing XML files to scan
        yaml_dir: Directory containing translation YAML files
        base_locale: The base language to check keys from
        cpp_dir: Optional directory containing C++ source files
        repo_root: Repository root for the reference scan (defaults to the
            parent of xml_dir, i.e. the checkout containing ui_xml/)

    Returns:
        Set of obsolete key names
    """
    # Extract all strings used in XML
    used_strings = extract_strings_from_directory(xml_dir, recursive=True)

    # Also extract from C++ if directory provided
    if cpp_dir and cpp_dir.exists():
        cpp_strings = extract_strings_from_cpp_directory(cpp_dir, recursive=True)
        used_strings.update(cpp_strings)

    # Union in the recall-oriented reference scan. Without this, keys that are
    # only reached indirectly look unused and get deleted.
    root = repo_root if repo_root is not None else xml_dir.parent
    if root.exists():
        used_strings.update(
            collect_referenced_strings(
                root, skip_translation_dir=(xml_dir / "translations")
            )
        )

    # Get all keys from base locale YAML
    base_path = yaml_dir / f"{base_locale}.yml"
    if not base_path.exists():
        return set()

    base_data = load_yaml_file(base_path)
    base_translations = base_data.get("translations", {})

    if not base_translations:
        return set()

    # Find keys in YAML that aren't referenced anywhere
    yaml_keys = set(base_translations.keys())
    obsolete = yaml_keys - used_strings

    return obsolete


def report_obsolete_keys(obsolete_keys: Set[str]) -> None:
    """
    Print a report of obsolete keys.

    Args:
        obsolete_keys: Set of obsolete key names
    """
    if not obsolete_keys:
        print("No obsolete keys found.")
        return

    print(f"Found {len(obsolete_keys)} obsolete keys:")
    print()

    for key in sorted(obsolete_keys):
        print(f"  - {key}")


def mark_obsolete_keys(
    yaml_dir: Path, obsolete_keys: Set[str], dry_run: bool = False
) -> int:
    """
    Mark obsolete keys with a DEPRECATED comment in YAML files.

    Args:
        yaml_dir: Directory containing translation YAML files
        obsolete_keys: Set of keys to mark
        dry_run: If True, don't modify files

    Returns:
        Number of keys marked
    """
    if not obsolete_keys:
        return 0

    if not dry_run:
        require_ruamel("Marking obsolete keys")

    marked = 0

    for yaml_path in yaml_dir.glob("*.yml"):
        data = load_yaml_file(yaml_path)
        translations = data.get("translations")

        if not translations:
            continue

        # Collect the value changes for keys that aren't already deprecated.
        changes = {}
        for key in obsolete_keys:
            if key in translations:
                value = translations[key]
                if not str(value).startswith("[DEPRECATED]"):
                    changes[key] = f"[DEPRECATED] {value}"
                    marked += 1

        if changes and not dry_run:
            raw = yaml_path.read_text(encoding="utf-8").splitlines(keepends=True)
            spans = _entry_spans(translations, len(raw))
            # Replace each changed entry's lines bottom-up so indices stay valid.
            for key in sorted(changes, key=lambda k: spans.get(k, (-1,))[0], reverse=True):
                if key not in spans:
                    continue
                start, end = spans[key]
                raw[start:end] = _render_entry_lines(key, changes[key])
            yaml_path.write_text("".join(raw), encoding="utf-8")

    return marked


def delete_obsolete_keys(
    yaml_dir: Path, obsolete_keys: Set[str], dry_run: bool = False
) -> int:
    """
    Delete obsolete keys from all YAML files.

    Args:
        yaml_dir: Directory containing translation YAML files
        obsolete_keys: Set of keys to delete
        dry_run: If True, don't modify files

    Returns:
        Number of keys deleted
    """
    if not obsolete_keys:
        return 0

    if not dry_run:
        require_ruamel("Deleting obsolete keys")

    deleted = 0

    for yaml_path in yaml_dir.glob("*.yml"):
        data = load_yaml_file(yaml_path)
        translations = data.get("translations")

        if not translations:
            continue

        to_delete = [k for k in translations if k in obsolete_keys]
        if not to_delete:
            continue

        if dry_run:
            deleted += len(to_delete)
            continue

        raw = yaml_path.read_text(encoding="utf-8").splitlines(keepends=True)
        spans = _entry_spans(translations, len(raw))
        # Collect the lines to drop as a SET, then splice once.
        #
        # Deleting each span bottom-up looks safe but is not, because the spans
        # OVERLAP. An entry's span runs to the next entry's start line, so a
        # `# Source:` comment sitting between two entries falls inside the
        # earlier entry's span -- and _absorb_leading_comments then pulls that
        # same line into the later entry's span too. Two deletions then claim
        # one line, the second `del` runs against a list the first already
        # shortened, and it over-reaches by exactly that many lines, silently
        # taking whatever live key follows. Deleting the adjacent obsolete keys
        # `0h` and `0m` destroyed the live key between the next pair, and the
        # count still reported 2. A set of indices cannot double-count, so
        # overlap becomes harmless and order stops mattering.
        #
        # Count spliced entries rather than matched ones: _entry_spans needs
        # ruamel's line numbers, and under plain PyYAML it returns nothing, so
        # counting matches reported a full delete for a file left untouched.
        drop: Set[int] = set()
        for key in to_delete:
            if key not in spans:
                continue
            start, end = spans[key]
            start = _absorb_leading_comments(raw, start)
            # A trailing comment run documents the NEXT entry, so it is not this
            # entry's to remove -- leaving it keeps a surviving neighbour's
            # `# Source:` line attached to it.
            while end - 1 > start and raw[end - 1].lstrip().startswith("#"):
                end -= 1
            drop.update(range(start, end))
            deleted += 1
        if drop:
            raw = [line for i, line in enumerate(raw) if i not in drop]
        yaml_path.write_text("".join(raw), encoding="utf-8")

    return deleted
