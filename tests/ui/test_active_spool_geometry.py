# SPDX-License-Identifier: GPL-3.0-or-later

"""Active Spool home-widget text containment, measured through `ctl geom`.

#1286 (v0.99.113, Ender-3 V3 KE / Nebula Pad 480x272): the Spoolman brand fix
(#1264) started composing `vendor + filament_name` into the widget's brand row
("Elegoo TPU 95A - Preto" instead of the pre-.113 vendor-only "Elegoo"). At
Micro the wide layout's text column is ~67px, so the composed label wrapped to
a second line, the three-row text stack grew past the single-row card, and the
card (scrollable=false) clipped the weight row in half — with the widget at the
screen edge the clipped lines read as text escaping the screen.

The fix keeps every label one line (`long_mode="scroll_circular"`, the same
treatment ams_loaded_card.xml gives this exact string) and drops the wide row's
vertical padding, so the stack's height is deterministic: three lines, exactly
the Micro single-row card's content height.

The assertions are containment in absolute screen coords (see
test_modal_geometry.py for why parent-relative coords are a trap):

  test_brand_row_is_populated        precondition — without a synced spool the
                                     widget hides its labels and everything
                                     below passes vacuously
  test_text_rows_stay_inside_the_card
                                     every label's box inside the card box

Mutation checks (measured 2026-08-17, not assumed):
  - restore the pre-fix labels (wrap, padded row: `git show b59df4ff4:ui_xml/
    components/panel_widget_active_spool.xml`) -> test_text_rows_stay_inside_
    the_card fails at 480x272 (weight 8px past the wide layout) and 480x320
    (17px, the weight itself wrapped). It passes at 800x480 and 1024x600,
    where the single-row card is tall enough to absorb the wrapped brand
    line — same shape as the modal geometry suite: the guard lives on the
    small tiers, the big ones pin no-regression.

The widget is placed at col 4, row 0, colspan 2, rowspan 1 — the smallest span
that trips the wide layout at Micro (2 columns incl. gutter = 142px >=
W_NORMAL 135) and the tightest box it can be asked to fit (a 1-column span is
69px < 135 and renders the compact icon-only layout instead).
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

import pytest

from helix.app import HelixApp, _TEST_SEED_SETTINGS

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))

# The app's own serialization of the default mock-printer home dashboard, with
# active_spool enabled at (col 4, row 0, 2x1). Captured from a --test run's
# settings-test.json so the placement is exactly what a user's edit-mode drag
# produces; a hand-minimized layout could silently diverge from what
# PanelWidgetManager accepts. Verified collision-free at every size below.
_HOME_LAYOUT = json.loads('{"main_page_index":0,"next_page_id":1,"pages":[{"id":"main","widgets":[{"col":0,"colspan":2,"enabled":true,"id":"printer_image","row":0,"rowspan":2},{"col":0,"colspan":2,"enabled":true,"id":"print_status","row":2,"rowspan":2},{"col":2,"colspan":2,"enabled":true,"id":"tips","row":0,"rowspan":2},{"col":2,"colspan":1,"enabled":true,"id":"temperature","row":2,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"shutdown","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"lock","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"power_device","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"network","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"firmware_restart","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"tool_switcher","row":-1,"rowspan":1},{"col":2,"colspan":1,"enabled":true,"id":"led","row":3,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"led_controls","row":-1,"rowspan":1},{"col":3,"colspan":1,"enabled":true,"id":"fan_stack","row":3,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"fan","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"nozzle_temps","row":-1,"rowspan":2},{"col":-1,"colspan":1,"enabled":false,"id":"chamber_temperature","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"temp_stack","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"thermistor","row":-1,"rowspan":1},{"col":-1,"colspan":2,"enabled":false,"id":"temp_graph","row":-1,"rowspan":2},{"col":-1,"colspan":3,"enabled":false,"id":"preheat","row":-1,"rowspan":1},{"col":4,"colspan":1,"enabled":true,"id":"ams","row":3,"rowspan":1},{"col":4,"colspan":2,"enabled":true,"id":"active_spool","row":0,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"filament","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"humidity","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"width_sensor","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"favorite_macro","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"macros","row":-1,"rowspan":1},{"col":-1,"colspan":1,"enabled":false,"id":"motion","row":-1,"rowspan":1},{"col":-1,"colspan":2,"enabled":false,"id":"clock","row":-1,"rowspan":1},{"col":-1,"colspan":2,"enabled":false,"id":"control_buttons","row":-1,"rowspan":1},{"col":-1,"colspan":2,"enabled":false,"id":"job_queue","row":-1,"rowspan":2},{"col":-1,"colspan":1,"enabled":false,"id":"clog_detection","row":-1,"rowspan":1},{"col":-1,"colspan":2,"enabled":false,"id":"print_stats","row":-1,"rowspan":2},{"col":-1,"colspan":1,"enabled":false,"id":"gcode_console","row":-1,"rowspan":1},{"col":-1,"colspan":2,"enabled":false,"id":"camera","row":-1,"rowspan":2},{"col":5,"colspan":1,"enabled":true,"id":"notifications","row":3,"rowspan":1},{"col":2,"colspan":1,"enabled":false,"id":"bed_temperature","row":3,"rowspan":1}]}]}')

# 480x272 is the Nebula Pad this was reported on; 480x320 the next tier up.
# 800x480 and 1024x600 pin that the single-line treatment does not unsizes the
# taller tiers where the old wrap also fit.
_SIZES = ["480x272", "480x320", "800x480", "1024x600"]

_LABELS = ("spoolman_material", "spoolman_brand_color", "spoolman_weight")


def _geom(app: HelixApp, target: str) -> dict | None:
    """First widget geom record for `target`, or None when it is not on screen."""
    result = app.geom(target)
    widgets = result.get("widgets") if isinstance(result, dict) else None
    return widgets[0] if widgets else None


@pytest.fixture
def spool_app(request, tmp_path):
    """An instance at a given size with the Active Spool widget placed at 2x1."""
    size = request.param
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built — run `make -j`")

    # HELIX_MOCK_AMS=none mirrors the reporter's printer (Ender-3 V3 KE: no
    # filament system, external spool synced from Spoolman). With the default
    # Happy-Hare mock the active material comes from the AMS backend instead
    # and exercises a different source_slot path than the one that regressed.
    env_overrides = {
        "HELIX_SCREEN_SIZE": size,
        "HELIX_MOCK_AMS": "none",
        # Caller-supplied config dir: HelixApp then skips its own seeding, so
        # the dashboard layout below is what boots — and each test gets its
        # own dir, keeping the instance lock private.
        "HELIX_CONFIG_DIR": str(tmp_path / "helix-config"),
    }
    (tmp_path / "helix-config").mkdir()

    seed = dict(_TEST_SEED_SETTINGS)
    seed["printers"] = {
        "default": {
            "type": "generic",
            "panel_widgets": {"home": _HOME_LAYOUT},
        }
    }
    (tmp_path / "helix-config" / "settings-test.json").write_text(
        json.dumps(seed, indent=2))

    before = {k: os.environ.get(k) for k in env_overrides}
    os.environ.update(env_overrides)
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log")
        with app:
            # The external spool arrives async (mock Spoolman startup sync);
            # the widget's labels are set from it. Poll rather than wait_idle
            # — the brand row is not a subject.
            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline:
                try:
                    if (app.text("spoolman_brand_color") or "").strip():
                        break
                except Exception:
                    pass
                time.sleep(0.25)
            yield app, size
    finally:
        for k, v in before.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


@pytest.mark.parametrize("spool_app", _SIZES, indirect=True)
def test_brand_row_is_populated(spool_app):
    """Precondition: a synced spool with a resolved identity (#1264 path).

    Without it the widget hides all three labels and the containment test
    below would pass against an empty card — vacuously green is worse than
    red here, because the regression only exists when the brand row shows.
    """
    app, size = spool_app
    for label in _LABELS:
        text = (app.text(label) or "").strip()
        assert text, f"{size}: {label} is empty — spool sync did not reach the widget"


@pytest.mark.parametrize("spool_app", _SIZES, indirect=True)
def test_text_rows_stay_inside_the_card(spool_app):
    """Every label box inside the wide layout and the card, absolute coords.

    Two boundaries, two failure modes:

    - `spoolman_wide_layout` is the scrollable=false box that actually clips
      pixels. This is the load-bearing assertion: pre-fix at 480x272 the
      wrapped brand pushes the weight label's box to end exactly at the
      CARD's bottom edge — inside the card, but 8px past the wide layout,
      with its glyphs cut in half. Containment against the card alone
      passes that layout; the wide layout is what catches it.
    - the card (`spoolman_btn`) catches grosser escapes (label boxes past
      the widget itself, what the 480x272 screenshot in #1286 shows).
    """
    app, size = spool_app

    card = _geom(app, "spoolman_btn")
    wide = _geom(app, "spoolman_wide_layout")
    assert card is not None, f"{size}: Active Spool widget not on the dashboard"
    assert wide is not None, f"{size}: wide layout not found (compact layout?)"
    card_bottom = card["y"] + card["h"]
    card_right = card["x"] + card["w"]
    wide_bottom = wide["y"] + wide["h"]
    wide_right = wide["x"] + wide["w"]

    for label_name in _LABELS:
        label = _geom(app, label_name)
        assert label is not None, f"{size}: {label_name} not found (compact layout?)"
        bottom = label["y"] + label["h"]
        right = label["x"] + label["w"]
        assert bottom <= wide_bottom, (
            f"{size}: {label_name} runs {bottom - wide_bottom}px past the wide "
            f"layout bottom (wide y={wide['y']} h={wide['h']}, label "
            f"y={label['y']} h={label['h']}) — the scrollable=false box clips "
            f"it mid-glyph")
        assert right <= wide_right and right <= card_right, (
            f"{size}: {label_name} box runs past the widget right edge "
            f"(wide x={wide['x']} w={wide['w']}, card x={card['x']} "
            f"w={card['w']}, label x={label['x']} w={label['w']})")
        assert bottom <= card_bottom, (
            f"{size}: {label_name} runs {bottom - card_bottom}px past the card "
            f"bottom (card y={card['y']} h={card['h']}, label y={label['y']} "
            f"h={label['h']})")
