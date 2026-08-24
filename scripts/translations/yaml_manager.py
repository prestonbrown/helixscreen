#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
YAML Manager module - handles merging new keys into translation YAML files.

Insertion is **surgical**: new entries are spliced into the raw text at the
correct alphabetical position and existing lines are left byte-for-byte
untouched. The whole file is never re-serialized. A full round-trip dump (even
with ruamel's round-trip mode) does not reproduce the committed files
byte-for-byte — the locale files use a fold width and quoting style that no
single dump config reproduces — so re-dumping to add one key reformatted
thousands of unrelated lines (wrapping/unwrapping folded scalars, requoting).
Reading uses ruamel only to obtain each key's source line number (`.lc`); the
edit itself is a line splice.
"""

from dataclasses import dataclass, field
from io import StringIO
from pathlib import Path
from typing import Set, Dict, List, Tuple, Optional, Any

try:
    from ruamel.yaml import YAML
    from ruamel.yaml.comments import CommentedMap
    from ruamel.yaml.constructor import DuplicateKeyError

    RUAMEL_AVAILABLE = True
except ImportError:
    RUAMEL_AVAILABLE = False
    import yaml as pyyaml


@dataclass
class MergeResult:
    """Result of a merge operation."""

    keys_added: int = 0
    files_modified: int = 0
    keys_per_file: Dict[str, int] = field(default_factory=dict)


def get_yaml_instance():
    """Get a configured YAML instance for round-trip reading."""
    if not RUAMEL_AVAILABLE:
        return None

    yaml = YAML()
    yaml.preserve_quotes = True
    yaml.indent(mapping=2, sequence=2, offset=2)
    yaml.width = 4096  # Don't wrap lines
    return yaml


def require_ruamel(operation: str) -> None:
    """
    Refuse a mutating operation when ruamel.yaml is missing.

    Every edit path splices raw lines at positions taken from ruamel's ``.lc``
    line numbers. Under the PyYAML fallback those positions do not exist, so a
    splice removes nothing (or appends out of alphabetical order) while the
    caller still reports the full count as edited. Failing here keeps a missing
    dependency from reading as a completed edit.
    """
    if not RUAMEL_AVAILABLE:
        raise RuntimeError(
            f"{operation} needs ruamel.yaml to locate each key's source line. "
            "Run it with .venv/bin/python3 (create the venv with 'make venv-setup')."
        )


class DuplicateTranslationKey(Exception):
    """A locale file defines the same key twice."""


def _duplicate_key_message(yaml_path: Path, exc: "DuplicateKeyError") -> str:
    """Turn ruamel's multi-line parse error into one actionable line."""
    # exc.problem already reads: found duplicate key "X" with value "Y"
    # (original value: "Z"). problem_mark is 0-based.
    where = ""
    if exc.problem_mark is not None:
        where = f" at line {exc.problem_mark.line + 1}"
    return (
        f"{yaml_path}: {exc.problem}{where}. Keep the translated entry and delete "
        f"the empty one; a branch that ran translation-sync before the locales were "
        f"filled, then merged, is the usual cause — YAML files merge textually, so "
        f"both lines survive."
    )


def _strict_pyyaml_loader():
    """A SafeLoader subclass that refuses a duplicate key instead of keeping the last.

    Built on demand rather than at import: ``pyyaml`` is only imported on the
    ruamel-less path, so a module-level subclass would have to name a base class
    that does not exist in the common case — and the usual dodge (inherit from
    ``object`` when ruamel is present) leaves a class whose ``super()`` call is
    broken if anything ever reaches it.
    """

    class StrictLoader(pyyaml.SafeLoader):
        def construct_mapping(self, node, deep=False):
            seen = set()
            for key_node, _ in node.value:
                key = self.construct_object(key_node, deep=deep)
                if key in seen:
                    raise DuplicateTranslationKey(
                        f"duplicate key {key!r} at line {key_node.start_mark.line + 1}"
                    )
                seen.add(key)
            return super().construct_mapping(node, deep)

    return StrictLoader


def load_yaml_file(yaml_path: Path) -> Dict[str, Any]:
    """
    Load a YAML translation file.

    Rejects a file that defines a key twice, in terms a person can act on.
    Neither backend handles that acceptably on its own: ruamel raises
    DuplicateKeyError with a traceback that buries the cause under a dozen frames
    of constructor internals, and pyyaml.safe_load silently keeps the LAST value.

    Detection is left to the parsers rather than a regex over the text. A locale
    key may be quoted either way, may double an inner quote, and — for the long
    ones — is folded across lines or written in YAML's explicit ``? key`` form;
    a line-oriented scan missed 59 keys per file, which would be exactly the
    blind spot a duplicate hides in.

    Not hypothetical: a branch cut before the locales were filled ran
    `translation-sync`, which correctly added empty placeholders for keys that did
    not yet exist on it. Merging brought main's translated entries in beside them,
    and since YAML files merge textually both lines survived. "Print cancelled"
    and "Print did not start" then shipped untranslated in all eight non-English
    locales.

    Args:
        yaml_path: Path to the YAML file

    Returns:
        Dict with 'locale' and 'translations' keys

    Raises:
        DuplicateTranslationKey: the file defines a key more than once
    """
    text = yaml_path.read_text(encoding="utf-8")

    if RUAMEL_AVAILABLE:
        yaml = get_yaml_instance()
        try:
            data = yaml.load(text)
        except DuplicateKeyError as exc:
            raise DuplicateTranslationKey(_duplicate_key_message(yaml_path, exc)) from None
    else:
        try:
            data = pyyaml.load(text, Loader=_strict_pyyaml_loader())
        except DuplicateTranslationKey as exc:
            raise DuplicateTranslationKey(f"{yaml_path}: {exc}") from None

    return data or {"locale": "", "translations": {}}


def save_yaml_file(yaml_path: Path, data: Dict[str, Any]) -> None:
    """
    Serialize an entire data structure back to YAML.

    NOTE: this re-serializes the whole document and does NOT preserve the
    committed files' exact folding/quoting. It is retained only for callers
    that build a document from scratch. The key-lifecycle operations in this
    module (insert/delete/mark) use surgical line splicing instead — see the
    module docstring.
    """
    if RUAMEL_AVAILABLE:
        yaml = get_yaml_instance()
        with open(yaml_path, "w", encoding="utf-8") as f:
            yaml.dump(data, f)
    else:
        with open(yaml_path, "w", encoding="utf-8") as f:
            pyyaml.dump(data, f, allow_unicode=True, sort_keys=True)


# ---------------------------------------------------------------------------
# Surgical line-based editing helpers
# ---------------------------------------------------------------------------


def _render_entry_lines(key: str, value: str) -> List[str]:
    """
    Render a single ``  key: value`` translation entry as raw text lines.

    The entry is produced by dumping ``{"translations": {key: value}}`` and
    keeping everything after the ``translations:`` header line, so the key gets
    the same 2-space indent and quoting-when-required that ruamel uses for the
    rest of the file. Each returned line ends in a newline.

    A very wide dump width is used so neither key nor value is folded across
    lines: at a small width ruamel folds a long *plain-scalar key* without
    switching to ``? key`` complex-key notation, which is invalid YAML (a
    block-mapping key cannot span lines as a plain scalar). Keeping each new
    entry on a single ``key: value`` line is always valid and only affects the
    inserted lines — untouched lines are never reflowed.
    """
    yaml = YAML()
    yaml.preserve_quotes = True
    yaml.indent(mapping=2, sequence=2, offset=2)
    yaml.width = 4096  # never fold key or value onto a continuation line
    doc = CommentedMap()
    inner = CommentedMap()
    inner[key] = value
    doc["translations"] = inner

    buf = StringIO()
    yaml.dump(doc, buf)
    lines = buf.getvalue().splitlines()
    # lines[0] is "translations:"; the rest is the indented entry.
    return [line + "\n" for line in lines[1:]]


def _translations_header_index(raw_lines: List[str]) -> int:
    """Return the index of the ``translations:`` line, or -1 if absent."""
    for i, line in enumerate(raw_lines):
        if line.rstrip("\n") == "translations:" or line.startswith("translations:"):
            return i
    return -1


def _key_start_lines(translations) -> Dict[str, int]:
    """Map each key to the 0-based line where its entry begins (via ruamel .lc)."""
    lc = getattr(translations, "lc", None)
    if lc is None or getattr(lc, "data", None) is None:
        return {}
    return {k: lc.data[k][0] for k in translations if k in lc.data}


def _surgical_insert(
    yaml_path: Path,
    translations,
    entries: List[Tuple[str, str, Optional[str]]],
) -> None:
    """
    Splice new entries into the raw file text at their alphabetical position.

    Args:
        yaml_path: file to edit
        translations: the ruamel CommentedMap already loaded from the file
        entries: list of (key, value, comment) to insert; keys must not exist
    """
    if not entries:
        return

    raw = yaml_path.read_text(encoding="utf-8").splitlines(keepends=True)
    existing = list(translations.keys()) if translations else []
    start_line = _key_start_lines(translations) if translations else {}

    # Anchor for keys that sort after everything / empty files: end of the
    # translations block. Since translations is the last mapping in the file,
    # that is end-of-file; for an empty block, just after the header line.
    if start_line:
        append_at = len(raw)
    else:
        header = _translations_header_index(raw)
        append_at = header + 1 if header >= 0 else len(raw)

    plan: List[Tuple[int, str, str, Optional[str]]] = []
    for key, value, comment in entries:
        successor = None
        for ek in existing:
            if ek > key and (successor is None or ek < successor):
                successor = ek
        idx = start_line[successor] if successor is not None else append_at
        plan.append((idx, key, value, comment))

    # Apply bottom-up so earlier splices don't shift later indices. Ties at the
    # same anchor are emitted so the final on-disk order ascends.
    plan.sort(key=lambda t: (t[0], t[1]), reverse=True)
    for idx, key, value, comment in plan:
        new_lines = _render_entry_lines(key, value)
        if comment:
            new_lines = [f"  #{comment}\n"] + new_lines
        if idx >= len(raw) and raw and not raw[-1].endswith("\n"):
            raw[-1] = raw[-1] + "\n"
        raw[idx:idx] = new_lines

    yaml_path.write_text("".join(raw), encoding="utf-8")


def _entry_spans(translations, n_lines: int) -> Dict[str, Tuple[int, int]]:
    """
    Map each key to the [start, end) raw-line range its entry occupies.

    An entry runs from its own start line up to the next entry's start line
    (end-of-file for the last entry). Used by delete/mark to remove or replace
    exactly one entry's lines without disturbing neighbours.
    """
    starts = _key_start_lines(translations)
    ordered = sorted(starts.items(), key=lambda kv: kv[1])
    spans: Dict[str, Tuple[int, int]] = {}
    for i, (key, line) in enumerate(ordered):
        end = ordered[i + 1][1] if i + 1 < len(ordered) else n_lines
        spans[key] = (line, end)
    return spans


def _absorb_leading_comments(raw: List[str], start: int) -> int:
    """Extend a span's start upward over comment lines that belong to the key."""
    while start - 1 >= 0 and raw[start - 1].lstrip().startswith("#"):
        start -= 1
    return start


def merge_new_keys(
    yaml_dir: Path, new_keys: Set[str], dry_run: bool = False
) -> MergeResult:
    """
    Merge new translation keys into all YAML files.

    For English: new keys get the key itself as value
    For other languages: new keys get empty string (needs translation)

    Args:
        yaml_dir: Directory containing translation YAML files
        new_keys: Set of new keys to add
        dry_run: If True, don't modify files

    Returns:
        MergeResult with statistics
    """
    if not dry_run:
        require_ruamel("Merging new keys")

    result = MergeResult()

    for yaml_path in sorted(yaml_dir.glob("*.yml")):
        data = load_yaml_file(yaml_path)
        locale = data.get("locale", yaml_path.stem)
        translations = data.get("translations")
        existing = set(translations.keys()) if translations else set()

        to_add = sorted(k for k in new_keys if k not in existing)
        if not to_add:
            continue

        result.keys_added += len(to_add)
        result.files_modified += 1
        result.keys_per_file[yaml_path.name] = len(to_add)

        if not dry_run:
            entries = [
                (key, key if locale == "en" else "", None) for key in to_add
            ]
            _surgical_insert(yaml_path, translations, entries)

    return result


def merge_new_keys_with_sources(
    yaml_dir: Path,
    new_keys_with_sources: Dict[str, List[Tuple[str, int]]],
    dry_run: bool = False,
) -> MergeResult:
    """
    Merge new translation keys with source file comments.

    Args:
        yaml_dir: Directory containing translation YAML files
        new_keys_with_sources: Dict mapping keys to list of (filename, line) tuples
        dry_run: If True, don't modify files

    Returns:
        MergeResult with statistics
    """
    if not dry_run:
        require_ruamel("Merging new keys")

    result = MergeResult()

    for yaml_path in sorted(yaml_dir.glob("*.yml")):
        data = load_yaml_file(yaml_path)
        locale = data.get("locale", yaml_path.stem)
        translations = data.get("translations")
        existing = set(translations.keys()) if translations else set()

        to_add = sorted(k for k in new_keys_with_sources if k not in existing)
        if not to_add:
            continue

        result.keys_added += len(to_add)
        result.files_modified += 1
        result.keys_per_file[yaml_path.name] = len(to_add)

        if not dry_run:
            entries = []
            for key in to_add:
                sources = new_keys_with_sources[key]
                source_str = ", ".join(f"{f}:{l}" for f, l in sources[:3])
                if len(sources) > 3:
                    source_str += f" (+{len(sources) - 3} more)"
                comment = f" Source: {source_str}"
                entries.append((key, key if locale == "en" else "", comment))
            _surgical_insert(yaml_path, translations, entries)

    return result
