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
import time
from pathlib import Path

import pytest

from conftest import ARTIFACT_ROOT, dump_failure_diagnostics
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

# The mock printer dispatches a status notification every fourth 250 ms physics
# tick (NOTIFICATION_INTERVAL_TICKS in moonraker_client_mock.h).
_MOCK_PUSH_INTERVAL_S = 1.0


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


def _assert_z_subjects(app: HelixApp, size: str, gcode: int, actual: int) -> None:
    """Fail if the Z subjects do not hold what the caller just wrote.

    Both states this test measures are reachable without either `set` landing:
    a frozen instance starts at 0/0, which is equal, and equal is what the
    hidden half looks for. Reading the subjects back is what makes the setup a
    step that can fail rather than one that is assumed.
    """
    for name, expected in (("gcode_position_z", gcode), ("position_z", actual)):
        got = app.get(name).get("value")
        assert got == expected, (
            f"{size}: {name} reads {got!r}, expected {expected} - the write did "
            f"not land, so what follows measures a state nobody established")


def _wait_act_text(app: HelixApp, expected: str | None,
                   timeout: float = 15.0) -> str | None:
    """Poll the Act value until it reads `expected`, then report what it reads.

    wait_idle() is best-effort and does not see raw lv_async_call work, so on a
    slow machine the row can still be pending when it returns. The caller's
    assert is the real check - this only stops a fast reader from failing it
    prematurely.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and _act_text(app) != expected:
        time.sleep(0.25)
    return _act_text(app)


@pytest.fixture
def motion_app(request, tmp_path):
    """An instance at a given size, on the motion overlay, frozen for measurement.

    freeze() parks the mock's simulation thread as well as LVGL's timers, so no
    status push re-equalises the Z subjects mid-measurement; manual `set` still
    propagates while frozen (same trick as the AMS loading-error modal fixture
    in test_modal_geometry).
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
                # This fixture builds its own HelixApp, so the `artifacts`
                # fixture cannot resolve it and writes nothing. Dump here
                # instead, before unfreeze, so a failure is inspected in the
                # state it failed in.
                if any(getattr(request.node, f"rep_{phase}", None) is not None
                       and getattr(request.node, f"rep_{phase}").failed
                       for phase in ("setup", "call")):
                    dump_failure_diagnostics(app, ARTIFACT_ROOT / request.node.name)
                app.unfreeze()
    finally:
        if before is None:
            os.environ.pop("HELIX_SCREEN_SIZE", None)
        else:
            os.environ["HELIX_SCREEN_SIZE"] = before


@pytest.mark.parametrize("motion_app", _SIZES, indirect=True)
def test_act_row_does_not_shift_stable_geometry(motion_app):
    app, size = motion_app

    # Diverged Z first, so the hidden half below asserts a transition rather
    # than a starting condition: a frozen instance already sits at equal Z, and
    # "row is hidden" there is true whether or not anything this test did took
    # effect.
    app.set("gcode_position_z", _Z_EQUAL)
    app.set("position_z", _Z_DIVERGED)
    app.wait_idle()
    _assert_z_subjects(app, size, gcode=_Z_EQUAL, actual=_Z_DIVERGED)
    # Report the value, not just the expectation: None means the row is hidden or
    # absent, anything else means it rendered and the text is wrong. A bare
    # message cannot tell those apart, and a custom message suppresses pytest's
    # own comparison output.
    actual = _wait_act_text(app, "3.00 mm")
    assert actual == "3.00 mm", (
        f"{size}: Act row text is {actual!r}, expected '3.00 mm' "
        f"(None = row hidden or absent)")
    # The Act row alone cannot tell "both sets landed" from "only the toolhead
    # one did" - 0.00 vs 3.00 diverges too. The commanded row reads the other
    # subject, so it is what pins the divergence to the one set up here.
    commanded = app.text("pos_z")
    assert commanded == "2.50 mm", (
        f"{size}: commanded Z row reads {commanded!r}, expected '2.50 mm' - the "
        f"gcode_position_z write did not land, so the divergence under test is "
        f"not the one this test set up")

    # The row has to still be there when the geometry below is read, or the
    # measurement describes an undefined state. A mock status push carries a
    # full position snapshot with gcode Z and toolhead Z equal, which re-hides
    # the row; outlasting one push interval is what says the frozen state holds.
    time.sleep(_MOCK_PUSH_INTERVAL_S * 1.5)
    held = _act_text(app)
    assert held == "3.00 mm", (
        f"{size}: Act row read {held!r} after {_MOCK_PUSH_INTERVAL_S * 1.5:.1f}s frozen, "
        f"expected it to still say '3.00 mm' - the printer state moved under a "
        f"frozen instance, so the geometry below would measure nothing definite")

    shown = {name: _geom(app, name) for name in ("jog_pad", "position_card", "pos_z")}

    # Equal Z: the row goes away again. The read above is what makes this mean
    # something - the row was demonstrably there a moment ago.
    app.set("position_z", _Z_EQUAL)
    app.wait_idle()
    _assert_z_subjects(app, size, gcode=_Z_EQUAL, actual=_Z_EQUAL)
    gone = _wait_act_text(app, None)
    assert gone is None, (
        f"{size}: Act row reads {gone!r} with equal Z, expected it hidden")

    hidden = {name: _geom(app, name) for name in ("jog_pad", "position_card", "pos_z")}

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
