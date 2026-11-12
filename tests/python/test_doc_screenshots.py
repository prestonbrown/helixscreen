# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/doc_screenshots.py: the doc-image manifest and its capture."""

import sys
import textwrap
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from doc_screenshots import ManifestError, load_manifest, recipe_tokens  # noqa: E402


def write(tmp_path: Path, body: str) -> Path:
    path = tmp_path / "doc-screenshots.yml"
    path.write_text(textwrap.dedent(body))
    return path


def test_recipe_tokens_come_from_the_recipe_table():
    tokens = recipe_tokens(REPO_ROOT / "scripts" / "screenshot-recipes.sh")
    assert "home" in tokens
    assert "filament" in tokens
    assert all(not t.startswith("#") for t in tokens)


def test_loads_app_and_manual_entries(tmp_path):
    entries = load_manifest(
        write(
            tmp_path,
            """
            images:
              - path: docs/images/user/home.png
                kind: app
                recipe: home
              - path: docs/images/user/filament-callout.png
                kind: app
                steps: [navigate filament]
                env: {HELIX_MOCK_AMS: medusahc}
                crop: spool_card
                markers:
                  - {n: 1, widget: extruder_dropdown}
                  - {n: 2, widget: btn_target_tool_action, at: center}
              - path: docs/images/user/screen-sharing-browser.png
                kind: manual
                source: a browser window on the printer's remote screen page
                recapture: open http://<printer-ip>/screen in a desktop browser and capture the window
            """,
        )
    )
    assert [e.path for e in entries] == [
        "docs/images/user/home.png",
        "docs/images/user/filament-callout.png",
        "docs/images/user/screen-sharing-browser.png",
    ]
    home, callout, manual = entries
    assert home.size == (800, 480)
    assert home.crop is None and home.markers == ()
    assert callout.env == {"HELIX_MOCK_AMS": "medusahc"}
    assert callout.margin == 12
    assert callout.markers[0].at == "top_right"
    assert callout.markers[1].at == "center"
    assert manual.kind == "manual"


@pytest.mark.parametrize(
    "entry, reason",
    [
        ("{path: docs/images/a.png, kind: app, recipe: home, steps: [navigate home]}", "both"),
        ("{path: docs/images/a.png, kind: app}", "recipe"),
        ("{path: docs/images/a.png, kind: app, recipe: no-such-token}", "no-such-token"),
        ("{path: docs/images/a.png, kind: manual, source: x}", "recapture"),
        ("{path: docs/images/a.png, kind: manual, source: x, recapture: y, recipe: home}", "manual"),
        ("{kind: app, recipe: home}", "path"),
        ("{path: docs/images/a.png, kind: video, recipe: home}", "kind"),
        (
            "{path: docs/images/a.png, kind: app, recipe: home, markers: [{n: 1, widget: a}, {n: 1, widget: b}]}",
            "marker 1",
        ),
        ("{path: docs/images/a.png, kind: app, recipe: home, size: 800by480}", "size"),
    ],
)
def test_rejects_malformed_entries(tmp_path, entry, reason):
    with pytest.raises(ManifestError) as err:
        load_manifest(write(tmp_path, f"images:\n  - {entry}\n"))
    assert reason in str(err.value)


def test_rejects_duplicate_path(tmp_path):
    with pytest.raises(ManifestError) as err:
        load_manifest(
            write(
                tmp_path,
                """
                images:
                  - {path: docs/images/a.png, kind: app, recipe: home}
                  - {path: docs/images/a.png, kind: app, recipe: filament}
                """,
            )
        )
    assert "docs/images/a.png" in str(err.value) and "duplicate" in str(err.value)


def test_the_repo_manifest_loads():
    entries = load_manifest(REPO_ROOT / "docs" / "images" / "doc-screenshots.yml")
    assert len(entries) > 100
