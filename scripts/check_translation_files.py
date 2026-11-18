#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep translations/*.yml in the shape Weblate reads and writes.

Weblate edits the locale files through translate-toolkit's YAML format, which
re-serializes a whole file on every commit (docs/devel/TRANSLATION_SYSTEM.md,
"Weblate"). Three properties keep that safe, and this gate checks each one:

1. Shape. Every file is byte-identical to canonical_text() of itself, which is
   what Weblate writes. A file in any other shape is reflowed wholesale by the
   next Weblate commit, burying the translator's change in hundreds of
   unrelated lines. ``--fix`` rewrites the files into that shape.
2. Locale field. ``locale:`` equals the file name. Weblate shows the field as an
   ordinary string, and translation-sync decides from it whether a new key gets
   its English text or a blank.
3. Plural entries. Weblate's YAML format has no plural support: a plural
   mapping in a locale file (``one:``/``few:``/``other:``) has no unit matching
   the flat English source, and translating that string in Weblate replaces the
   whole mapping with one flat string. Measured against the branch's fork point
   (scripts/diff_base.py), a key that was a plural mapping and is now a flat
   string fails.

Exit status: 0 = all good, 1 = a finding, 2 = usage error.
"""

import argparse
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

from translations.yaml_manager import canonical_text  # noqa: E402

try:
    import yaml as pyyaml
except ImportError:  # the gate runs from .venv, which always has it
    pyyaml = None


def _safe_load(text: str):
    loader = getattr(pyyaml, "CSafeLoader", pyyaml.SafeLoader)
    return pyyaml.load(text, Loader=loader) or {}


def _shape_finding(path_str: str):
    """(path, canonical text) when the file is not in Weblate's shape, else None."""
    path = Path(path_str)
    text = path.read_text(encoding="utf-8")
    canon = canonical_text(text)
    return None if canon == text else (path_str, canon)


def first_difference(text: str, canon: str) -> int:
    """1-based number of the first line where ``text`` and ``canon`` differ."""
    a, b = text.splitlines(), canon.splitlines()
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i + 1
    return min(len(a), len(b)) + 1


def check_shape(files, fix: bool) -> list:
    """Messages for files not in canonical shape; rewrites them when ``fix``."""
    if not files:
        return []
    workers = min(len(files), os.cpu_count() or 1)
    with ProcessPoolExecutor(max_workers=workers) as pool:
        findings = [f for f in pool.map(_shape_finding, [str(p) for p in files]) if f]
    messages = []
    for path_str, canon in findings:
        path = Path(path_str)
        if fix:
            path.write_text(canon, encoding="utf-8")
            print(f"rewrote {path.name} in Weblate's shape")
            continue
        line = first_difference(path.read_text(encoding="utf-8"), canon)
        messages.append(
            f"{path.name}:{line}: not in the shape Weblate writes (first difference "
            f"here); its next commit would reflow this file"
        )
    return messages


def check_locale_field(files) -> list:
    messages = []
    for path in files:
        data = _safe_load(path.read_text(encoding="utf-8"))
        locale = data.get("locale")
        if locale != path.stem:
            messages.append(
                f"{path.name}: locale is {locale!r}, but the file name says "
                f"{path.stem!r}; restore 'locale: {path.stem}'"
            )
    return messages


def _git(root: Path, *args) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace",
    )


def collapsed_plurals(base_translations: dict, translations: dict) -> list:
    """Keys whose value was a plural mapping and is now a flat string."""
    return sorted(
        key for key, old in base_translations.items()
        if isinstance(old, dict) and isinstance(translations.get(key), str)
    )


def check_plurals(files, root: Path, base: str) -> list:
    messages = []
    for path in files:
        rel = path.resolve().relative_to(root.resolve()).as_posix()
        shown = _git(root, "show", f"{base}:{rel}")
        if shown.returncode != 0:
            continue  # a locale the base does not have yet
        old = (_safe_load(shown.stdout).get("translations") or {})
        new = (_safe_load(path.read_text(encoding="utf-8")).get("translations") or {})
        for key in collapsed_plurals(old, new):
            messages.append(
                f"{path.name}: plural entry {key!r} became a flat string. Weblate's "
                f"YAML format cannot edit plural forms; restore the mapping from "
                f"{base[:12]} and put the new text in the form it belongs to"
            )
    return messages


def plural_keys(files) -> list:
    """Every key some locale writes as a plural mapping: the strings Weblate's
    component marks read-only (TRANSLATION_SYSTEM.md, "Weblate")."""
    keys = set()
    for path in files:
        translations = _safe_load(path.read_text(encoding="utf-8")).get("translations") or {}
        keys.update(k for k, v in translations.items() if isinstance(v, dict))
    return sorted(keys)


def resolve_base(root: Path, explicit: str):
    """(base, why) via scripts/diff_base.py, or (None, why) when there is none."""
    from diff_base import resolve_base as diff_resolve_base

    base, why = diff_resolve_base(root, None if explicit == "auto" else explicit)
    if _git(root, "rev-parse", "--verify", "--quiet", f"{base}^{{commit}}").returncode != 0:
        return None, f"{base} does not resolve"
    return base, why


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", type=Path, default=REPO_ROOT / "translations",
                    help="locale directory (default: translations/)")
    ap.add_argument("--root", type=Path, default=REPO_ROOT,
                    help="git work tree the plural check reads the base from")
    ap.add_argument("--base", default="auto",
                    help="REV the plural check compares against; 'auto' = the fork "
                         "point scripts/diff_base.py picks; 'none' skips the check")
    ap.add_argument("--fix", action="store_true",
                    help="rewrite files that are not in Weblate's shape")
    ap.add_argument("--list-plural-keys", action="store_true",
                    help="print every key a locale writes as a plural mapping, then exit")
    args = ap.parse_args(argv)

    if pyyaml is None:
        print("check_translation_files.py needs PyYAML; run it with .venv/bin/python")
        return 2

    files = sorted(args.dir.glob("*.yml"))
    if not files:
        print(f"no locale files in {args.dir}")
        return 2

    if args.list_plural_keys:
        for key in plural_keys(files):
            print(key)
        return 0

    messages = check_shape(files, args.fix)
    messages += check_locale_field(files)
    if args.base != "none":
        base, why = resolve_base(args.root, args.base)
        if base is None:
            print(f"plural check skipped: {why}")
        else:
            messages += check_plurals(files, args.root, base)

    for message in messages:
        print(f"❌ {message}")
    return 1 if messages else 0


if __name__ == "__main__":
    sys.exit(main())
