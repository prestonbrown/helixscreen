# SPDX-License-Identifier: GPL-3.0-or-later

"""Temperature overlay control strip across landscape screens.

The nozzle and bed strips are not scrollable, so every preset button and the
Custom button must sit fully inside both the screen and the overlay content
area. The Snapmaker U1 persona (four heads) is the tallest nozzle strip: the
tool row sits between the heater card and the preset buttons.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from helix.app import HelixApp

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))

_SIZES = [
    # (size, uses the fixed-row "_tiny" preset twin)
    ("480x272", True),
    ("480x320", True),
    ("800x480", False),
    ("1024x600", False),
]

_NOZZLE = ("btn_nozzle_temp", "", ["preset_off", "preset_1", "preset_2", "preset_3",
                                   "nozzle_btn_custom"], "nozzle_control_strip")
_BED = ("btn_bed_temp", "bed_", ["bed_preset_off", "bed_preset_1", "bed_preset_2",
                                 "bed_preset_3", "bed_btn_custom"], "bed_control_strip")


def _geom(app: HelixApp, target: str) -> dict:
    result = app.geom(target)
    widgets = result.get("widgets") if isinstance(result, dict) else None
    assert widgets, f"{target}: no geom record"
    return widgets[0]


@pytest.mark.parametrize("opener,_prefix,buttons,strip_name", [_NOZZLE, _BED],
                         ids=["nozzle", "bed"])
@pytest.mark.parametrize("size,tiny", _SIZES, ids=[s for s, _ in _SIZES])
def test_control_buttons_fit_inside_screen_and_panel(size, tiny, opener, _prefix,
                                                     buttons, strip_name, tmp_path):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make`")
    saved = {k: os.environ.get(k) for k in ("HELIX_SCREEN_SIZE", "HELIX_MOCK_PRINTER")}
    os.environ["HELIX_SCREEN_SIZE"] = size
    os.environ["HELIX_MOCK_PRINTER"] = "snapmaker_u1"
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log")
        with app:
            app.wait_for("nav_buttons_enabled", 1, timeout=30)
            app.navigate("controls")
            app.wait_idle()
            app.click(opener)
            app.wait_idle()
            assert "temp_graph_overlay" in app.current().get("overlays", [])

            width, height = (int(v) for v in size.split("x"))
            content = _geom(app, "overlay_content")
            strip = _geom(app, strip_name)
            bottom_limit = min(height, content["y"] + content["h"],
                               strip["y"] + strip["h"])
            for name in buttons:
                geo = _geom(app, name + "_tiny" if tiny else name)
                assert geo["x"] >= 0 and geo["x"] + geo["w"] <= width, (
                    f"{size} {name}: x {geo['x']}..{geo['x'] + geo['w']} leaves the screen")
                assert geo["y"] >= content["y"] and geo["y"] + geo["h"] <= bottom_limit, (
                    f"{size} {name}: y {geo['y']}..{geo['y'] + geo['h']} is not inside "
                    f"the panel or its control strip (limit {bottom_limit})")
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
