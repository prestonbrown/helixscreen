# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/doc_screenshots.py: the doc-image manifest and its capture."""

import sys
import textwrap
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import doc_screenshots  # noqa: E402
from doc_screenshots import ManifestError, load_manifest, recipe_tokens  # noqa: E402

doc_screenshots.SETTLE_SECONDS = 0


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


# --- capture: marker placement and the ctl calls ------------------------------

from doc_screenshots import (  # noqa: E402
    Box,
    CaptureError,
    Entry,
    Marker,
    capture,
    marker_points,
)


def test_marker_points_relative_to_crop():
    anchors = {"btn": Box(100, 50, 40, 20)}
    crop = Box(80, 40, 200, 100)
    pts = marker_points("e.png", anchors, crop, (Marker(1, "btn", "top_right"),))
    # top-right corner of the widget (140, 50) in crop space is (60, 10).
    assert pts == [(1, 60, 10)]


def test_marker_points_full_screen_and_center():
    anchors = {"a": Box(100, 50, 40, 20)}
    pts = marker_points("e.png", anchors, None, (Marker(2, "a", "center"),))
    assert pts == [(2, 120, 60)]


def test_missing_anchor_fails_entry():
    with pytest.raises(CaptureError) as err:
        marker_points("docs/images/x.png", {}, None, (Marker(1, "gone", "center"),))
    assert "docs/images/x.png" in str(err.value) and "gone" in str(err.value)


class FakeSession:
    """Records ctl calls; answers geom with fixed boxes and writes a blank PNG."""

    def __init__(self, boxes):
        self.calls = []
        self.boxes = boxes

    def ctl(self, *args):
        self.calls.append(args)
        if args[0] == "geom":
            if args[1] not in self.boxes:
                raise RuntimeError(f"ctl geom {args[1]}: Error: Widget not found: {args[1]}")
            b = self.boxes[args[1]]
            hidden = args[1] in getattr(self, "hidden", ())
            # The shape `helix-screen ctl geom` prints.
            return {"widgets": [{"name": args[1], "x": b.x, "y": b.y, "w": b.w, "h": b.h,
                                 "hidden": hidden}]}
        if args[0] == "screenshot":
            from PIL import Image

            Image.new("RGB", (800, 480)).save(args[1])
        return {}


def test_capture_runs_recipe_steps_uses_stable_and_crops(tmp_path):
    entry = Entry(
        path="docs/images/user/x.png",
        kind="app",
        steps=("navigate filament",),
        crop="card",
        margin=10,
        markers=(Marker(1, "btn", "center"),),
    )
    session = FakeSession({"card": Box(100, 100, 200, 100), "btn": Box(150, 120, 20, 20)})
    out = capture(entry, session, tmp_path)
    assert ("navigate", "filament") in session.calls
    shot = [c for c in session.calls if c[0] == "screenshot"][0]
    assert "--stable" in shot
    # Animations and the mock's simulation are stopped for the shot, then resumed.
    names = [c[0] for c in session.calls]
    assert names.index("freeze") < names.index("screenshot") < names.index("unfreeze")
    from PIL import Image

    img = Image.open(out)
    # crop box grown by the margin on every side
    assert img.size == (220, 120)


from doc_screenshots import run  # noqa: E402


def test_run_counts_and_skips_manual(tmp_path, capsys):
    entries = [
        Entry(path="docs/images/a.png", kind="app", steps=("navigate home",)),
        Entry(path="docs/images/m.png", kind="manual", source="s", recapture="r"),
        Entry(path="docs/images/b.png", kind="app", steps=("navigate home",), crop="gone"),
    ]
    sessions = []

    class Factory(FakeSession):
        def __init__(self, env, size):
            super().__init__({})
            sessions.append(self)

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            pass

        def ctl(self, *args):
            if args[0] == "geom":
                raise RuntimeError(f"Widget not found: {args[1]}")
            return super().ctl(*args)

    rc = run(entries, Factory, tmp_path)
    out = capsys.readouterr().out
    assert rc == 1
    assert "captured 1, failed 1, skipped 1 (manual)" in out
    assert "docs/images/b.png" in out and "gone" in out
    # A fresh app per capture: a recipe that leaves a mode open (Home edit mode)
    # must not leak into the next capture.
    assert len(sessions) == 2
    assert (tmp_path / "docs/images/a.png").exists()


@pytest.mark.parametrize(
    "boxes, hidden, reason",
    [
        ({"card": Box(100, 100, 200, 100)}, (), "btn"),  # marker widget missing
        ({"card": Box(100, 100, 200, 100), "btn": Box(150, 120, 20, 20)}, ("btn",), "hidden"),
        ({"card": Box(100, 100, 200, 100), "btn": Box(150, 120, 0, 20)}, (), "zero size"),
        ({"card": Box(100, 100, 200, 100), "btn": Box(600, 400, 20, 20)}, (), "outside"),
        ({"card": Box(100, 100, 200, 100), "btn": Box(150, 120, 20, 20)}, ("card",), "hidden"),
    ],
)
def test_capture_fails_loudly_on_a_bad_widget(tmp_path, boxes, hidden, reason):
    entry = Entry(path="docs/images/user/x.png", kind="app", steps=("navigate filament",),
                  crop="card", markers=(Marker(1, "btn", "center"),))
    session = FakeSession(boxes)
    session.hidden = hidden
    with pytest.raises(CaptureError) as err:
        capture(entry, session, tmp_path)
    assert "docs/images/user/x.png" in str(err.value) and reason in str(err.value)
    assert not (tmp_path / entry.path).exists()


def test_geometry_is_read_while_frozen(tmp_path):
    entry = Entry(path="docs/images/user/x.png", kind="app", steps=("navigate filament",),
                  crop="card", markers=(Marker(1, "btn", "center"),))
    session = FakeSession({"card": Box(100, 100, 200, 100), "btn": Box(150, 120, 20, 20)})
    capture(entry, session, tmp_path)
    names = [c[0] for c in session.calls]
    last_geom = max(i for i, n in enumerate(names) if n == "geom")
    assert names.index("freeze") < last_geom < names.index("unfreeze")


def test_run_survives_a_failed_boot_and_cleans_up(tmp_path, capsys):
    entries = [
        Entry(path="docs/images/a.png", kind="app", steps=("navigate home",)),
        Entry(path="docs/images/b.png", kind="app", steps=("navigate home",)),
    ]
    exits = []

    class Factory(FakeSession):
        boots = 0

        def __init__(self, env, size):
            super().__init__({})

        def __enter__(self):
            Factory.boots += 1
            if Factory.boots == 1:
                raise RuntimeError("the app did not answer")
            return self

        def __exit__(self, *exc):
            exits.append(True)

    rc = run(entries, Factory, tmp_path)
    out = capsys.readouterr().out
    assert rc == 1
    assert "captured 1, failed 1" in out
    assert (tmp_path / "docs/images/b.png").exists()


def test_cli_only_filter_and_counts(tmp_path, monkeypatch, capsys):
    manifest = write(tmp_path, """
        images:
          - {path: docs/images/user/home.png, kind: app, recipe: home}
          - {path: docs/images/user/filament.png, kind: app, recipe: filament}
          - {path: docs/images/user/m.png, kind: manual, source: s, recapture: r}
        """)
    monkeypatch.setattr(doc_screenshots, "MANIFEST", manifest)
    rc = doc_screenshots.main(["--only", "docs/images/user/home*", "--list"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "docs/images/user/home.png" in out and "filament" not in out


def test_session_env_is_pinned():
    env = doc_screenshots.session_env({"HELIX_MOCK_AMS": "medusahc"},
                                      {"HELIX_THEME": "nord", "HELIX_MOCK_AMS": "afc",
                                       "HELIX_MOCK_PRINTER": "k1", "PATH": "/bin"})
    assert env["HELIX_MOCK_AMS"] == "medusahc"
    assert "HELIX_THEME" not in env and "HELIX_MOCK_PRINTER" not in env
    assert env["PATH"] == "/bin"
    assert env["HELIX_HEADLESS"] == "1" and env["SDL_VIDEODRIVER"] == "dummy"


def test_lint_and_loader_agree_on_the_manifest():
    import check_doc_images

    text = (REPO_ROOT / "docs/images/doc-screenshots.yml").read_text()
    by_regex = check_doc_images.MANIFEST_PATH_RE.findall(text)
    by_yaml = [e.path for e in load_manifest(REPO_ROOT / "docs/images/doc-screenshots.yml")]
    assert by_regex == by_yaml
