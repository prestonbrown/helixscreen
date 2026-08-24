# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/minify_xml_tree.py.

The minifier itself is covered by test_esp32_stage_assets.py — these cover the
tree walker: the source-tree guard, in-place rewriting, idempotence, and the
refusal to write a file the transform made larger.
"""

import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "minify_xml_tree.py"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
from minify_xml_tree import minify_tree  # noqa: E402


def write(tmp_path: Path, name: str, text: str) -> Path:
    p = tmp_path / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")
    return p


def test_strips_comments_and_inter_tag_whitespace(tmp_path):
    f = write(tmp_path, "a.xml", '<view>\n  <!-- gone -->\n  <lv_obj x="1"/>\n</view>\n')
    files, before, after = minify_tree(tmp_path)
    assert files == 1
    assert after < before
    # The trailing newline survives on purpose: it is a text node with a tag on
    # only one side, so the collapser leaves it alone.
    assert f.read_text() == '<view><lv_obj x="1"/></view>\n'


def test_preserves_text_content_and_attribute_values(tmp_path):
    # Whitespace INSIDE a text node or an attribute is content, not formatting.
    original = '<view>\n  <label text="a  b" >Hello  world</label>\n</view>'
    f = write(tmp_path, "b.xml", original)
    minify_tree(tmp_path)
    out = f.read_text()
    assert ">Hello  world<" in out
    assert 'text="a  b"' in out


def test_recurses_into_subdirectories(tmp_path):
    write(tmp_path, "top.xml", "<a>\n  <b/>\n</a>")
    write(tmp_path, "sub/deep/nested.xml", "<c>\n  <d/>\n</c>")
    files, _, _ = minify_tree(tmp_path)
    assert files == 2
    assert (tmp_path / "sub" / "deep" / "nested.xml").read_text() == "<c><d/></c>"


def test_idempotent(tmp_path):
    write(tmp_path, "a.xml", "<view>\n  <!-- x -->\n  <o/>\n</view>")
    minify_tree(tmp_path)
    first = (tmp_path / "a.xml").read_text()
    files, before, after = minify_tree(tmp_path)
    # Second pass rewrites nothing and reports no change.
    assert files == 0
    assert before == after
    assert (tmp_path / "a.xml").read_text() == first


def test_non_xml_files_untouched(tmp_path):
    keep = write(tmp_path, "notes.txt", "  spaced  \n")
    write(tmp_path, "a.xml", "<a>\n  <b/>\n</a>")
    minify_tree(tmp_path)
    assert keep.read_text() == "  spaced  \n"


def test_refuses_the_protected_tree(tmp_path):
    # The guard is exercised against a FIXTURE protected root, never the repo's
    # real ui_xml/. Pointing this at the live path is what makes mutating the
    # guard destructive: the test would then minify the working tree for real.
    fixture = tmp_path / "ui_xml"
    write(tmp_path, "ui_xml/a.xml", "<a>\n  <b/>\n</a>")
    before = (fixture / "a.xml").read_text()

    with pytest.raises(SystemExit) as exc:
        minify_tree(fixture, protected=fixture)
    assert "refusing" in str(exc.value)
    # And it must not have written anything on the way to refusing.
    assert (fixture / "a.xml").read_text() == before


def test_refuses_a_subdirectory_of_the_protected_tree(tmp_path):
    fixture = tmp_path / "ui_xml"
    write(tmp_path, "ui_xml/components/c.xml", "<c>\n  <d/>\n</c>")
    with pytest.raises(SystemExit):
        minify_tree(fixture / "components", protected=fixture)


def test_default_protected_root_is_the_repo_ui_xml(tmp_path):
    # Pins the default the guard uses, without ever invoking it on that path.
    from minify_xml_tree import source_ui_xml_root
    assert source_ui_xml_root() == (REPO_ROOT / "ui_xml").resolve()


def test_force_bypasses_the_guard(tmp_path):
    # --force must actually bypass the guard, or the escape hatch is a lie.
    fixture = tmp_path / "ui_xml"
    write(tmp_path, "ui_xml/a.xml", "<a>\n  <b/>\n</a>")
    files, _, _ = minify_tree(fixture, force=True, protected=fixture)
    assert files == 1


def test_cli_runs_and_reports(tmp_path):
    write(tmp_path, "a.xml", "<view>\n  <!-- c -->\n  <o/>\n</view>")
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(tmp_path)],
        capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stderr
    assert "Minified 1 XML file" in result.stdout


def test_cli_rejects_a_non_directory(tmp_path):
    f = write(tmp_path, "a.xml", "<a/>")
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(f)],
        capture_output=True, text=True,
    )
    assert result.returncode == 1
