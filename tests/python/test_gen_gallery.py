# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/gen_gallery.py: the gallery lists every app image by area."""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from gen_gallery import area_of, main, render  # noqa: E402

ENTRIES = [
    {"path": "docs/images/screenshot-home-panel.png", "kind": "app"},
    {"path": "docs/images/user/settings-sound-2.png", "kind": "app"},
    {"path": "docs/images/user/settings-sound.png", "kind": "app"},
    {"path": "docs/images/user/runout-modal.png", "kind": "app"},
    {"path": "docs/images/user/home.png", "kind": "manual", "source": "s", "recapture": "r"},
]


def test_every_app_image_listed_and_manual_skipped():
    text = render(ENTRIES)
    assert '<img src="../images/screenshot-home-panel.png"' in text
    assert '<img src="../images/user/settings-sound-2.png"' in text
    assert "user/home.png" not in text


def test_grouped_by_area_in_order():
    text = render(ENTRIES)
    assert text.index("## Main panels") < text.index("## Filament (AMS)") < text.index("## Settings")
    assert area_of("docs/images/user/runout-modal.png") == "Filament (AMS)"


def test_check_flags_stale_file_and_write_fixes_it(tmp_path):
    manifest = tmp_path / "m.yml"
    manifest.write_text("images:\n  - {path: docs/images/screenshot-home-panel.png, kind: app, recipe: home}\n")
    out = tmp_path / "G.md"
    out.write_text("stale\n")
    assert main(["--check", "--manifest", str(manifest), "--out", str(out)]) == 1
    assert main(["--manifest", str(manifest), "--out", str(out)]) == 0
    assert main(["--check", "--manifest", str(manifest), "--out", str(out)]) == 0
