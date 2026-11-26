# SPDX-License-Identifier: GPL-3.0-or-later

"""Icon-over-label buttons keep their German labels inside the button at 480x320.

German labels run longer than English ones. At the 480x320 tier the print
status strip's Cancel and Tune buttons and the Controls panel's Motion button
are the narrowest places they land; `ctl overflow` lists any widget whose
content reaches past its box.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pytest

from helix.app import HelixApp

_REPO = Path(__file__).resolve().parents[2]
_BINARY = Path(os.environ.get("HELIX_UI_BINARY", str(_REPO / "build" / "bin" / "helix-screen")))
_LANGUAGES = _REPO / "src" / "system" / "system_settings_manager.cpp"


def _german_index() -> int:
    codes = re.search(r"kLanguageCodes\[\] = \{([^}]*)\}", _LANGUAGES.read_text()).group(1)
    return re.findall(r'"([\w-]+)"', codes).index("de")


def _overflowing(app: HelixApp, names: set[str]) -> dict[str, int]:
    rows = app.ctl("overflow").get("overflowing", [])
    return {r["name"]: max(r["left"], r["right"]) for r in rows if r["name"] in names}


@pytest.fixture
def german_app_480x320(tmp_path):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make`")
    saved = {k: os.environ.get(k) for k in ("HELIX_SCREEN_SIZE",)}
    os.environ["HELIX_SCREEN_SIZE"] = "480x320"
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log")
        with app:
            app.wait_for("nav_buttons_enabled", 1, timeout=30)
            app.navigate("settings")
            app.wait_idle()
            app.click("row_language_time")
            app.wait_idle()
            app.ctl("set_value", "row_language", _german_index())
            app.wait_idle()
            yield app
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def test_print_status_strip_labels_fit_their_buttons(german_app_480x320):
    app = german_app_480x320
    app.reset()
    app.ctl("demo", "print-status")
    app.wait_idle()
    assert "print_status_panel" in app.current().get("overlays", [])
    over = _overflowing(app, {"btn_pause", "btn_tune", "btn_more", "btn_cancel"})
    assert not over, f"print status strip buttons overflow their label (px): {over}"


def test_controls_motion_label_fits_its_button(german_app_480x320):
    app = german_app_480x320
    app.navigate("controls")
    app.wait_idle()
    over = _overflowing(app, {"btn_motion"})
    assert not over, f"Motion button overflows its label (px): {over}"
