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
import time
from pathlib import Path

import pytest

from helix.app import HelixApp

_REPO = Path(__file__).resolve().parents[2]
_BINARY = Path(os.environ.get("HELIX_UI_BINARY", str(_REPO / "build" / "bin" / "helix-screen")))
_LANGUAGES = _REPO / "src" / "system" / "system_settings_manager.cpp"


def _german_index() -> int:
    codes = re.search(r"kLanguageCodes\[\] = \{([^}]*)\}", _LANGUAGES.read_text()).group(1)
    return re.findall(r'"([\w-]+)"', codes).index("de")


def _label(app: HelixApp) -> str:
    value = app.get("print_control_primary_label")
    return value.get("value", "") if isinstance(value, dict) else value


def _overflowing(app: HelixApp, names: set[str]) -> dict[str, int]:
    rows = app.ctl("overflow").get("overflowing", [])
    return {r["name"]: max(r["left"], r["right"]) for r in rows if r["name"] in names}


@pytest.fixture
def german_app_480x320(tmp_path):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make`")
    saved = {k: os.environ.get(k) for k in ("HELIX_SCREEN_SIZE", "HELIX_MOCK_AUTO_PRINT")}
    os.environ["HELIX_SCREEN_SIZE"] = "480x320"
    os.environ["HELIX_MOCK_AUTO_PRINT"] = "1"
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log", extra_args=["--sim-speed", "6"])
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
    app.wait_for("print_control_primary_enabled", 1, timeout=30)
    assert "print_status_panel" in app.current().get("overlays", [])
    strip = {"btn_pause", "btn_tune", "btn_more", "btn_cancel"}
    over = _overflowing(app, strip)
    assert not over, f"print status strip buttons overflow their label (px): {over}"

    # Paused, the primary button reads "Resume" in English; German must differ,
    # which proves the language switch took effect, and must fit too.
    running = _label(app)
    app.click("btn_pause")
    resume = _label(app)
    deadline = time.monotonic() + 10
    while resume == running and time.monotonic() < deadline:
        time.sleep(0.2)
        app.wait_idle()
        resume = _label(app)
    assert resume and resume != "Resume", f"label still English or empty: {resume!r}"
    over = _overflowing(app, strip)
    assert not over, f"paused strip buttons overflow their label (px): {over}"


def test_controls_motion_label_fits_its_button(german_app_480x320):
    app = german_app_480x320
    app.navigate("controls")
    app.wait_idle()
    over = _overflowing(app, {"btn_motion"})
    assert not over, f"Motion button overflows its label (px): {over}"
