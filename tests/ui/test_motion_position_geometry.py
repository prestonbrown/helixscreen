# SPDX-License-Identifier: GPL-3.0-or-later

"""Motion overlay position-card stability while the "Act:" Z line flickers.

Discord report (Lanman1, 5" 480x800): during Z-Tilt the position card's
"Act:" sub-line appears and disappears as |gcode Z - toolhead Z| crosses
0.01 mm (MotionPanel::update_z_display). The card is size-to-content in
both layout branches, so each flip resized the card and re-flowed the
flex sibling that carries the jog pad - the pad is a fixed-px widget
re-centred in its wrapper, so the homing circle itself slid sideways
(landscape) or vertically (portrait) a few px each way, several times a
second on a bumpy Z-Tilt.

Measured pre-fix (ctl geom, diverged vs equal Z):
  800x480  card w 115->119, jog_pad x 252->254
  1280x720 card w 140->157, jog_pad x 440->448
  480x800  card h  90->112, jog_pad y 216->227

The contract these tests pin: toggling the Act line changes NOTHING -
not the jog pad's position, not the card's own size or position, not the
existing X/Y/Z rows. The card is size-to-content floored by the
motion_card_min_w / motion_card_min_h px tokens (declared in
motion_panel.xml), measured per font tier for the worst content
(7-char values on every axis, homed dots shown, Act row present), so
the reserved space absorbs the row and no sibling reflows.

Mutation map (each assertion against its fix half):
  - card w/h equality <- drop the min-width token from the landscape
    card (back to 115) or the min-height from the component root ->
    red at every size where the worst case exceeds the floor.
  - pad x/y and pos_z x/y equality <- same mutations, plus the pad
    reflows with the card.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from helix.app import HelixApp, HelixCtlError

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))

# Landscape sizes cover the font tiers where the jitter scales up (measured
# 2px at a 480-wide cramped axis, 8px at 720); 480x800 is the reporter's
# panel and exercises the portrait branch, where the same flip moves the
# pad vertically instead.
_SIZES = ["800x480", "1024x600", "1280x720", "480x800"]

# Subject values are 1/100 mm: 250 = 2.50 mm commanded, 300 = 3.00 mm
# actual -> 0.50 mm divergence, far over the 0.01 mm Act threshold.
_Z_EQUAL = 250
_Z_DIVERGED = 300


def _geom(app: HelixApp, target: str) -> dict:
    """First widget geom record for `target`."""
    result = app.geom(target)
    widgets = result.get("widgets") if isinstance(result, dict) else None
    assert widgets, f"{target}: no geom record"
    return widgets[0]


def _act_text(app: HelixApp) -> str | None:
    """The Act value, or None while its row is hidden (ctl skips hidden widgets)."""
    try:
        return app.text("pos_z_actual")
    except HelixCtlError:
        return None


@pytest.fixture
def motion_app(request, tmp_path):
    """An instance at a given size, on the motion overlay, frozen for measurement.

    freeze() stops the mock's 250 ms status pushes from re-equalising the Z
    subjects mid-measurement; manual `set` still propagates while frozen
    (same trick as the AMS loading-error modal fixture in test_modal_geometry).
    """
    size = request.param
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make -j`")

    before = os.environ.get("HELIX_SCREEN_SIZE")
    os.environ["HELIX_SCREEN_SIZE"] = size
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log")
        with app:
            app.navigate("controls")
            app.wait_idle()
            app.click("btn_motion")
            app.wait_idle()
            assert "motion_panel" in app.current().get("overlays", []), (
                "clicking btn_motion did not open the motion overlay")
            app.freeze()
            try:
                yield app, size
            finally:
                app.unfreeze()
    finally:
        if before is None:
            os.environ.pop("HELIX_SCREEN_SIZE", None)
        else:
            os.environ["HELIX_SCREEN_SIZE"] = before


@pytest.mark.parametrize("motion_app", _SIZES, indirect=True)
def test_act_row_does_not_shift_stable_geometry(motion_app):
    app, size = motion_app

    # Equal Z: the Act row is hidden.
    app.set("gcode_position_z", _Z_EQUAL)
    app.set("position_z", _Z_EQUAL)
    app.wait_idle()
    assert _act_text(app) is None, (
        f"{size}: Act row visible with equal Z - setup never reached the hidden state")

    hidden = {name: _geom(app, name) for name in ("jog_pad", "position_card", "pos_z")}

    # Diverged Z: the Act row appears. Assert the state BEFORE the geometry:
    # these tests assert nothing if the row never showed.
    app.set("position_z", _Z_DIVERGED)
    app.wait_idle()
    assert _act_text(app) == "3.00 mm", (
        f"{size}: Act row did not appear on divergence")

    shown = {name: _geom(app, name) for name in ("jog_pad", "position_card", "pos_z")}

    for name in ("jog_pad", "position_card", "pos_z"):
        before, after = hidden[name], shown[name]
        for axis in ("x", "y"):
            assert before[axis] == after[axis], (
                f"{size}: {name} moved {axis} {before[axis]}->{after[axis]} "
                f"when the Act line appeared - stable elements must not shift "
                f"while transient content toggles")

    # The card's SIZE is reserved for the Act row too: the min-width /
    # min-height token floors must sit at or above the with-Act content, or
    # the card edge breathes even though nothing else moves.
    for axis in ("w", "h"):
        assert hidden["position_card"][axis] == shown["position_card"][axis], (
            f"{size}: position_card resized {axis} {hidden['position_card'][axis]}->"
            f"{shown['position_card'][axis]} when the Act line appeared - the "
            f"motion_card_min_* floor is below the worst-case content")

    # The pad is created once at a fixed size and may only ever be re-centred;
    # a resize here would mean something started re-fitting it mid-session.
    for axis in ("w", "h"):
        assert hidden["jog_pad"][axis] == shown["jog_pad"][axis], (
            f"{size}: jog_pad resized {axis} {hidden['jog_pad'][axis]}->"
            f"{shown['jog_pad'][axis]}")
