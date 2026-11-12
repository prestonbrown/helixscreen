#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every image the user-facing docs show has an entry in docs/images/doc-screenshots.yml.

An image with no entry cannot be regenerated, so it goes stale the first time
the screen changes. Three findings, one line each:

  unrecipe: <image> (referenced from <page>)   a user-facing page shows an image the manifest lacks
  missing: <image> (manifest entry, file absent)
  unreferenced: <image>                         in the manifest, shown by no doc

User-facing pages are docs/user/**, README.md, docs/README.md and
docs/devel/GALLERY.md. An image counts as referenced when any markdown file
outside .worktrees/, docs/superpowers/, lib/ and build/ shows it, as
`![...](path)` or `<img src="path">`, resolved against that page.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

IMAGE_RE = re.compile(r"!\[[^\]]*\]\(([^)\s]+)[^)]*\)|<img\b[^>]*\bsrc=\"([^\"]+)\"")
IMAGE_EXT = (".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg")
SKIP_DIRS = (".worktrees", "docs/superpowers", "lib", "build", "node_modules", ".git")
MANIFEST = Path("docs/images/doc-screenshots.yml")
# The gate runs under the system python3, which may lack PyYAML: the paths are
# all it needs, and every entry writes them as `path: <value>`.
MANIFEST_PATH_RE = re.compile(r"(?:^\s*-\s*|[{,]\s*)path:\s*([^,}\s]+)", re.MULTILINE)


def _user_facing(rel: str) -> bool:
    return rel.startswith("docs/user/") or rel in (
        "README.md",
        "docs/README.md",
        "docs/devel/GALLERY.md",
    )


def _markdown_files(root: Path):
    """Tracked markdown in a git tree (fast, and untracked notes do not count);
    every markdown file outside SKIP_DIRS otherwise."""
    import subprocess

    listed = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", "*.md"],
                            capture_output=True, text=True)
    if listed.returncode == 0:
        rels = [r for r in listed.stdout.split("\0") if r]
    else:
        rels = [p.relative_to(root).as_posix() for p in root.rglob("*.md")]
    for rel in rels:
        if any(rel == d or rel.startswith(d + "/") for d in SKIP_DIRS):
            continue
        path = root / rel
        if path.is_file():
            yield rel, path


def _references(root: Path) -> dict:
    """image path (repo-relative) -> pages that show it."""
    refs: dict = {}
    for rel, path in _markdown_files(root):
        for m in IMAGE_RE.finditer(path.read_text(errors="replace")):
            target = (m.group(1) or m.group(2)).split("#")[0]
            if "://" in target or not target.lower().endswith(IMAGE_EXT):
                continue
            resolved = (path.parent / target).resolve()
            try:
                image = resolved.relative_to(root.resolve()).as_posix()
            except ValueError:
                continue
            refs.setdefault(image, []).append(rel)
    return refs


def find_problems(root: Path) -> list:
    root = Path(root)
    manifest = MANIFEST_PATH_RE.findall((root / MANIFEST).read_text())
    in_manifest = set(manifest)
    refs = _references(root)
    problems = []
    for image, pages in sorted(refs.items()):
        if image in in_manifest:
            continue
        shown = [p for p in pages if _user_facing(p)]
        if shown:
            problems.append(f"unrecipe: {image} (referenced from {shown[0]})")
    for image in manifest:
        if not (root / image).exists():
            problems.append(f"missing: {image} (manifest entry, file absent)")
        if image not in refs:
            problems.append(f"unreferenced: {image}")
    return problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    args = ap.parse_args(argv)
    problems = find_problems(Path(args.repo_root))
    for p in problems:
        print(p)
    if problems:
        print(f"{len(problems)} doc image finding(s); see scripts/check_doc_images.py", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
