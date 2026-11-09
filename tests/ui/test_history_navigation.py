# SPDX-License-Identifier: GPL-3.0-or-later

"""Print history: reaching the full list, and coming back to it.

  - the dashboard's View Full History control sits wholly inside the screen
    with nothing scrolled, at every landscape size, and a real tap on it opens
    the full list
  - Back from a job's detail lands on the list with its rows showing, and
    leaving the list and opening it again shows them too
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from helix.app import HelixApp

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))

_SIZES = ["480x320", "800x480", "1024x600", "1280x720"]

_VIEW_ALL = "View Full History"


def _run_app(size: str, tmp_path: Path):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make`")
    before = os.environ.get("HELIX_SCREEN_SIZE")
    os.environ["HELIX_SCREEN_SIZE"] = size
    try:
        return HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                        log_path=tmp_path / "app.log"), before
    except BaseException:
        _restore_size(before)
        raise


def _restore_size(before: str | None) -> None:
    if before is None:
        os.environ.pop("HELIX_SCREEN_SIZE", None)
    else:
        os.environ["HELIX_SCREEN_SIZE"] = before


def _geom(app: HelixApp, target: str, depth: int = 0) -> list[dict]:
    result = app.geom(target, depth)
    widgets = result.get("widgets") if isinstance(result, dict) else None
    assert widgets, f"{target}: no geom record"
    return widgets


def _open_dashboard(app: HelixApp) -> None:
    app.wait_for("nav_buttons_enabled", 1, timeout=30)
    app.navigate("advanced")
    app.wait_idle()
    app.click("row_print_history")
    app.wait_idle()
    assert app.current().get("overlays", [])[-1] == "history_dashboard_panel", (
        "row_print_history did not open the history dashboard")


def _view_all_button(app: HelixApp) -> dict:
    """The dashboard button labelled View Full History, wherever the layout puts it."""
    widgets = app.ls("history_dashboard_panel").get("widgets", [])
    for w in widgets:
        if w.get("type") != "button":
            continue
        # `text` reads a button's first label, which can be its icon glyph;
        # the caption is one of its first few children.
        for target in [w["path"]] + [f"{w['path']}/{i}" for i in range(3)]:
            try:
                if app.text(target) == _VIEW_ALL:
                    return _geom(app, w["path"])[0]
            except Exception:
                continue
    pytest.fail(f"no on-screen button reads {_VIEW_ALL!r} on the history dashboard")


def _open_full_list(app: HelixApp, width: int, height: int) -> None:
    btn = _view_all_button(app)
    assert btn["hidden"] is False
    assert btn["x"] >= 0 and btn["x"] + btn["w"] <= width, (
        f"{_VIEW_ALL} spans x {btn['x']}..{btn['x'] + btn['w']} on a {width}px screen")
    assert btn["y"] >= 0 and btn["y"] + btn["h"] <= height, (
        f"{_VIEW_ALL} spans y {btn['y']}..{btn['y'] + btn['h']} on a {height}px screen: "
        "the user has to scroll to find it")
    # A real pointer tap, so a button under another widget or off screen fails.
    app.ctl("tap", btn["x"] + btn["w"] // 2, btn["y"] + btn["h"] // 2)
    app.wait_idle()
    assert app.current().get("overlays", [])[-1] == "history_list_panel", (
        f"tapping {_VIEW_ALL} did not open the full history list")


def _visible_rows(app: HelixApp) -> int:
    widgets = _geom(app, "list_rows", 1)
    return sum(1 for w in widgets if w["name"] == "history_row" and not w["hidden"])


@pytest.mark.parametrize("size", _SIZES)
def test_view_full_history_is_on_screen(size, tmp_path):
    width, height = (int(v) for v in size.split("x"))
    app, before = _run_app(size, tmp_path)
    try:
        with app:
            _open_dashboard(app)
            _open_full_list(app, width, height)
    finally:
        _restore_size(before)


def test_back_from_detail_shows_the_list(tmp_path):
    app, before = _run_app("800x480", tmp_path)
    try:
        with app:
            _open_dashboard(app)
            _open_full_list(app, 800, 480)
            rows_before = _visible_rows(app)
            assert rows_before > 0, "the mock history list opened with no rows"

            app.click("history_row[0]")
            app.wait_idle()
            assert app.current().get("overlays", [])[-1] == "history_detail_overlay"

            app.go_back()
            app.wait_idle()
            assert app.current().get("overlays", [])[-1] == "history_list_panel"
            assert _visible_rows(app) == rows_before, (
                "Back from the detail left the history list blank")

            # Back again to the dashboard, then into the list afresh.
            app.go_back()
            app.wait_idle()
            assert app.current().get("overlays", [])[-1] == "history_dashboard_panel"
            _open_full_list(app, 800, 480)
            assert _visible_rows(app) == rows_before
    finally:
        _restore_size(before)
