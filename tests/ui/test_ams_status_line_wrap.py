# SPDX-License-Identifier: GPL-3.0-or-later

"""The AMS sidebar's status line on a 480x272 screen, where the Bypass toggle
rides inline beside the status text.

The column there is about 130px wide. Pinned:

  - a short word ("Idle") keeps the toggle beside it on one line
  - a word too wide to share the line stays whole on one line and the toggle
    drops below it, instead of the text being squeezed to a sliver and broken
    mid-word
  - a long message wraps at the line's width, grows tall enough to show every
    line, and keeps the toggle below it
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from helix.app import HelixApp

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))

_SIZE = "480x272"

# "Paused" in Russian: the longest single idle-state word the sidebar shows.
_LONG_WORD = "Приостановлено"
_LONG_MESSAGE = ("Lane 3 runout: filament sensor reports empty while the hub "
                 "still holds filament")


def _geom(app: HelixApp, target: str) -> dict:
    widgets = app.geom(target).get("widgets")
    assert widgets, f"{target}: no geom record"
    return widgets[0]


def _bottom(w: dict) -> int:
    return w["y"] + w["h"]


def _show_detail(app: HelixApp, text: str) -> tuple[dict, dict]:
    app.set("ams_action_detail", text)
    app.wait_idle()
    return _geom(app, "status_label"), _geom(app, "bypass_inline")


@pytest.fixture
def ams_app(tmp_path):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make -j`")
    before = os.environ.get("HELIX_SCREEN_SIZE")
    os.environ["HELIX_SCREEN_SIZE"] = _SIZE
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log",
                       extra_env={"HELIX_MOCK_AMS": "afc", "HELIX_AMS_GATES": "4"})
        with app:
            app.wait_for("nav_buttons_enabled", 1, timeout=30)
            app.ctl("demo", "ams")
            app.wait_idle()
            app.freeze()
            yield app
    finally:
        if before is None:
            os.environ.pop("HELIX_SCREEN_SIZE", None)
        else:
            os.environ["HELIX_SCREEN_SIZE"] = before


def test_status_word_never_breaks_beside_inline_bypass(ams_app):
    app = ams_app

    label, bypass = _show_detail(app, "Idle")
    assert bypass["hidden"] is False, "Bypass toggle is not inline at 480x272"
    line_h = label["h"]
    assert bypass["y"] < _bottom(label) and label["y"] < _bottom(bypass), (
        f"'Idle' {label} and the toggle {bypass} no longer share one line")

    label, bypass = _show_detail(app, _LONG_WORD)
    assert label["h"] == line_h, (
        f"'{_LONG_WORD}' is {label['h']}px tall, one line is {line_h}px: "
        "the word broke mid-word beside the toggle")
    assert bypass["y"] >= _bottom(label), (
        f"toggle at y={bypass['y']} overlaps the word ending at {_bottom(label)}")

    section = _geom(app, "status_section")
    label, bypass = _show_detail(app, _LONG_MESSAGE)
    assert label["w"] <= section["w"], (
        f"message is {label['w']}px wide in a {section['w']}px line")
    assert label["h"] > line_h, (
        f"message is {label['h']}px tall in a {label['w']}px line: it did not "
        "wrap, so every line past the first is clipped")
    assert bypass["y"] >= _bottom(label), (
        f"toggle at y={bypass['y']} overlaps the message ending at {_bottom(label)}")
