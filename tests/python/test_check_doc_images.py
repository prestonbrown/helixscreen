# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/check_doc_images.py: every doc image has a manifest entry."""

import sys
import textwrap
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from check_doc_images import find_problems  # noqa: E402


def tree(tmp_path: Path, files: dict) -> Path:
    for rel, body in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if rel.endswith(".png"):
            p.write_bytes(b"\x89PNG")
        else:
            p.write_text(textwrap.dedent(body))
    return tmp_path


MANIFEST_HEAD = "images:\n"


def entry(path: str) -> str:
    return f"  - {{path: {path}, kind: manual, source: s, recapture: r}}\n"


def test_resolves_relative_and_html_refs(tmp_path):
    root = tree(
        tmp_path,
        {
            "docs/user/guide/settings/display.md": "![a](../../../images/user/a.png)\n"
            '<img src="../../../images/user/b.png" width="300">\n',
            "docs/images/user/a.png": "",
            "docs/images/user/b.png": "",
            "docs/images/doc-screenshots.yml": MANIFEST_HEAD
            + entry("docs/images/user/a.png")
            + entry("docs/images/user/b.png"),
        },
    )
    assert find_problems(root) == []


def test_reports_unrecipe_missing_unreferenced(tmp_path):
    root = tree(
        tmp_path,
        {
            "README.md": "![x](docs/images/x.png)\n",
            "docs/images/x.png": "",
            "docs/images/dead.png": "",
            "docs/images/doc-screenshots.yml": MANIFEST_HEAD
            + entry("docs/images/dead.png")
            + entry("docs/images/gone.png"),
        },
    )
    problems = sorted(find_problems(root))
    assert problems == sorted(
        [
            "unrecipe: docs/images/x.png (referenced from README.md)",
            "missing: docs/images/gone.png (manifest entry, file absent)",
            "unreferenced: docs/images/dead.png",
            "unreferenced: docs/images/gone.png",
        ]
    )


def test_ignores_external_and_non_image_links(tmp_path):
    root = tree(
        tmp_path,
        {
            "docs/user/page.md": "![b](https://example.com/b.png) [doc](other.md)\n",
            "docs/images/doc-screenshots.yml": MANIFEST_HEAD,
        },
    )
    assert find_problems(root) == []


def test_in_a_git_tree_only_tracked_pages_count(tmp_path):
    import subprocess

    root = tree(
        tmp_path,
        {
            "docs/user/page.md": "no pictures\n",
            "docs/images/user/a.png": "",
            "docs/images/doc-screenshots.yml": MANIFEST_HEAD + entry("docs/images/user/a.png"),
            "notes/scratch.md": "![a](../docs/images/user/a.png)\n",
        },
    )
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "docs"], check=True)
    # notes/scratch.md is untracked, so a.png's only reference does not count.
    assert find_problems(root) == ["unreferenced: docs/images/user/a.png"]
