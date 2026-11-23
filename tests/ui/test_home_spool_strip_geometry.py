# SPDX-License-Identifier: GPL-3.0-or-later

"""The home spool strip with more lanes than fit: it scrolls sideways, and what
shows at rest is whole.

  - the pinned External label is drawn in the same face as the lane labels the
    cells were sized for, so it is not dotted ("Exte...")
  - no lane stands cut off at the viewport's right edge showing a sliver of its
    label
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from helix.app import HelixApp

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))


def _geom(app: HelixApp, target: str) -> dict:
    widgets = app.geom(target).get("widgets")
    assert widgets, f"{target}: no geom record"
    return widgets[0]


@pytest.mark.parametrize("size", ["800x480", "480x800"])
def test_scrolling_strip_shows_whole_labels(size, tmp_path):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built")
    before = os.environ.get("HELIX_SCREEN_SIZE")
    os.environ["HELIX_SCREEN_SIZE"] = size
    try:
        with HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                      log_path=tmp_path / "app.log") as app:
            app.wait_for("nav_buttons_enabled", 1, timeout=30)
            app.navigate("home")
            app.wait_idle()
            scroller = _geom(app, "ams_spools_container")
            lane = _geom(app, "spool_material_0")
            external = _geom(app, "bypass_material")
            assert external["h"] == lane["h"], (
                f"{size}: External label line is {external['h']}px, a lane's is "
                f"{lane['h']}px: it is drawn in another face than its cell was sized for")
            right = scroller["x"] + scroller["w"]
            for i in range(8):
                try:
                    cell = _geom(app, f"spool_cell_{i}")
                except Exception:
                    break
                if cell["hidden"]:
                    continue
                if cell["x"] >= right:
                    continue  # scrolled out of view entirely
                assert cell["x"] + cell["w"] <= right, (
                    f"{size}: lane {i} runs {cell['x'] + cell['w'] - right}px past the "
                    "strip's edge, leaving a cut-off label")
    finally:
        if before is None:
            os.environ.pop("HELIX_SCREEN_SIZE", None)
        else:
            os.environ["HELIX_SCREEN_SIZE"] = before
