# SPDX-License-Identifier: GPL-3.0-or-later

"""Motion overlay geometry across small-landscape and portrait screens.

Coordinates ride in the header_bar content slot on landscape screens and in
a full-width strip under the header on portrait screens; the jog pad claims
the shared row's leftover width. These tests pin, per resolution:

  - landscape: the widest realistic coordinate values ("350.00" x2 + "250.00")
    render through the slot without touching the title or the e-stop button,
    and the pad keeps the width the card freed up
  - portrait: the coordinate strip sits under the header, the pad and the Z
    column share one row with no dead band either side, the bottom row fits
    inside the panel without scrolling, and the header-slot copy stays hidden
  - one control overlay: a header_bar that fills nothing renders at exactly
    the geometry it had before the slot existed

Values were measured with ctl geom after the layout landed; floors sit 2px
under the measurement so a font-tier change that costs a pixel or two fails
loudly instead of silently eroding the pad.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from helix.app import HelixApp, HelixCtlError

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))

_LANDSCAPE = [
    # (size, jog pad diameter floor)
    ("480x272", 184),
    ("480x320", 234),
    ("800x480", 339),
    ("1024x600", 430),
]

_PORTRAIT = [
    # Height-bound square after the Z column takes its floor width first;
    # the coordinate row and the home row above the modes claim the rest of the
    # column; the tabs sit in the header.
    ("272x480", 188),
    ("320x480", 188),
]

# 1/100 mm: the widest realistic readout, 3 digits + 2 decimals per axis.
_WIDE_XY = 35000
_WIDE_Z = 25000

# The mock pushes a full status snapshot every fourth 250 ms physics tick,
# which overwrites manual sets; freeze() parks that thread first.
_MOCK_PUSH_INTERVAL_S = 1.0


def _geom(app: HelixApp, target: str) -> dict:
    result = app.geom(target)
    widgets = result.get("widgets") if isinstance(result, dict) else None
    assert widgets, f"{target}: no geom record"
    return widgets[0]


def _right(w: dict) -> int:
    return w["x"] + w["w"]


def _bottom(w: dict) -> int:
    return w["y"] + w["h"]


def _open_motion(app: HelixApp) -> None:
    # A fresh boot can reach this before the mock finishes connecting, and
    # navigate is refused while nav_buttons_enabled is 0.
    app.wait_for("nav_buttons_enabled", 1, timeout=30)
    app.navigate("controls")
    app.wait_idle()
    app.click("btn_motion")
    app.wait_idle()
    assert "motion_panel" in app.current().get("overlays", []), (
        "clicking btn_motion did not open the motion overlay")
    app.freeze()
    app.set("gcode_position_x", _WIDE_XY)
    app.set("gcode_position_y", _WIDE_XY)
    app.set("gcode_position_z", _WIDE_Z)
    app.wait_idle()


def _run_app(size: str, tmp_path: Path):
    """A live instance at `size`; the caller navigates inside the context."""
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make -j`")
    before = os.environ.get("HELIX_SCREEN_SIZE")
    os.environ["HELIX_SCREEN_SIZE"] = size
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log")
        return app, before
    except BaseException:
        if before is None:
            os.environ.pop("HELIX_SCREEN_SIZE", None)
        else:
            os.environ["HELIX_SCREEN_SIZE"] = before
        raise


def _restore_size(before: str | None) -> None:
    if before is None:
        os.environ.pop("HELIX_SCREEN_SIZE", None)
    else:
        os.environ["HELIX_SCREEN_SIZE"] = before


@pytest.mark.parametrize("size,pad_floor", _LANDSCAPE, ids=[s for s, _ in _LANDSCAPE])
def test_landscape_header_coords_fit_and_pad_grew(size, pad_floor, tmp_path):
    app, before = _run_app(size, tmp_path)
    try:
        with app:
            _open_motion(app)
            _assert_wide_values_rendered(app, "header_pos")
            title = _geom(app, "header_title")
            coords = _geom(app, "header_coords")
            assert coords["hidden"] is False, f"{size}: header coords hidden in landscape"

            # The coordinate group must clear both neighbours: the title on the
            # left and the cog (action_icon_button) on the right. The cog only
            # exists from the SMALL tier up; below it the header's own right
            # edge is the limit.
            right_edge = _right(coords)
            try:
                next_left = _geom(app, "action_icon_button")["x"]
            except HelixCtlError:
                next_left = _right(_geom(app, "overlay_header"))
            assert right_edge + 4 <= next_left, (
                f"{size}: coords right edge {right_edge} intrudes on the next "
                f"header widget at {next_left} at widest values")
            assert _right(title) <= coords["x"], (
                f"{size}: title right edge {_right(title)} overlaps coords at "
                f"{coords['x']}")

            pad = _geom(app, "jog_pad")
            assert pad["w"] >= pad_floor and pad["h"] >= pad_floor, (
                f"{size}: jog pad is {pad['w']}x{pad['h']}, floor is {pad_floor} "
                "- the shared row's leftover width did not reach the pad")
    finally:
        _restore_size(before)


@pytest.mark.parametrize("size,pad_floor", _PORTRAIT, ids=[s for s, _ in _PORTRAIT])
def test_portrait_strips_stack_and_fit(size, pad_floor, tmp_path):
    app, before = _run_app(size, tmp_path)
    try:
        with app:
            _open_motion(app)
            _assert_wide_values_rendered(app, "row_pos")

            # The header slot copy must stay hidden in portrait.
            with pytest.raises(HelixCtlError):
                app.geom("header_coords")

            header = _geom(app, "overlay_header")
            strip = _geom(app, "coord_row")
            assert strip["y"] >= _bottom(header), (
                f"{size}: coordinate strip at y={strip['y']} overlaps the header "
                f"(bottom {_bottom(header)})")
            assert strip["w"] >= header["w"] - 2, (
                f"{size}: coordinate strip is {strip['w']}px wide, expected the "
                f"full {header['w']}px header width")

            pad = _geom(app, "jog_pad")
            assert pad["w"] >= pad_floor and pad["h"] >= pad_floor, (
                f"{size}: jog pad is {pad['w']}x{pad['h']}, floor is {pad_floor}")

            # The full-width Z row under the pad is gone: portrait pairs the
            # pad with a tall Z column in one row instead.
            with pytest.raises(HelixCtlError):
                app.geom("z_row")

            # No dead band beside the circle: the pad hugs its row's left
            # edge, the Z column hugs the right edge. 6px is space_sm-ish;
            # anything wider means the wrapper stopped claiming the row.
            pad_row = _geom(app, "pad_row")
            z_col = _geom(app, "z_column")
            assert pad["x"] - pad_row["x"] <= 6, (
                f"{size}: {pad['x'] - pad_row['x']}px of dead space left of the "
                f"jog pad - the wrapper is not claiming the row width")
            assert _right(pad_row) - _right(z_col) <= 6, (
                f"{size}: {_right(pad_row) - _right(z_col)}px of dead space "
                f"right of the Z column")
            # And none between them: the wrapper is clamped to the pad square
            # so the growing Z column, not empty row width, takes the rest.
            # The wrapper-width bound also catches the overlap direction: a
            # wrapper that lost the clamp lets the pad spill across the gap.
            assert z_col["x"] - _right(pad) <= 8, (
                f"{size}: {z_col['x'] - _right(pad)}px of dead space between "
                f"the pad and the Z column - the wrapper is not clamped to "
                f"the pad square")
            wrapper = _geom(app, "jog_pad_wrapper")
            assert pad["w"] <= wrapper["w"] + 2, (
                f"{size}: pad is {pad['w']}px wide but its wrapper is only "
                f"{wrapper['w']}px - the pad spills out of the row's share")
            assert z_col["x"] + 2 >= _right(pad), (
                f"{size}: the Z column starts at {z_col['x']} under the pad's "
                f"right edge {_right(pad)} - the pad overlaps the Z column")

            content = _geom(app, "overlay_content")
            assert all(v == 0 for v in content["scroll"].values()), (
                f"{size}: overlay scrolled {content['scroll']} - portrait does "
                f"not fit without scrolling")
            panel = _geom(app, "motion_panel")
            bottom = _geom(app, "bottom_row")
            assert _bottom(bottom) <= panel["y"] + panel["h"], (
                f"{size}: bottom row bottom {_bottom(bottom)} exceeds the "
                f"panel bottom {panel['y'] + panel['h']} - portrait does not fit")

            # Jog owns the bottom row: the Move actions are not in it.
            with pytest.raises(HelixCtlError):
                app.geom("move_park")

            # On Move, Park and Motors off sit in the bottom row beside QGL
            # and the grid's own action row stays hidden, so the grid fills
            # the height that row used to claim.
            app.set("motion_tab", 1)
            app.wait_idle()
            with pytest.raises(HelixCtlError):
                app.geom("move_actions")
            with pytest.raises(HelixCtlError):
                app.geom("jog_mode_fine")
            park = _geom(app, "move_park")
            motors = _geom(app, "move_motors_off")
            qgl = _geom(app, "btn_qgl")
            assert park["y"] == motors["y"] == qgl["y"] == bottom["y"], (
                f"{size}: Park/Motors off (y={park['y']}/{motors['y']}) are not "
                f"in the bottom row (y={bottom['y']})")
            assert _right(park) <= motors["x"] and _right(motors) <= qgl["x"], (
                f"{size}: bottom row order is Park({park['x']}-{_right(park)}) "
                f"Motors off({motors['x']}-{_right(motors)}) "
                f"QGL({qgl['x']}-{_right(qgl)})")
            grid = _geom(app, "move_grid")
            assert _bottom(grid) <= bottom["y"], (
                f"{size}: grid bottom {_bottom(grid)} runs past the bottom row "
                f"top {bottom['y']}")
    finally:
        _restore_size(before)


def _assert_wide_values_rendered(app: HelixApp, prefix: str) -> None:
    """The widest values must actually be on screen before geometry is read.

    Geometry measured while the subjects still hold the boot values proves
    nothing about the widest case, which is the case that has to fit.
    """
    app.wait_idle()
    got = app.text(f"{prefix}_x")
    assert got == "350.00", (
        f"{prefix} X reads {got!r}, expected '350.00' - the wide-value setup "
        "did not land, so the fit measured below is not the worst case")
    got_z = app.text(f"{prefix}_z")
    assert got_z == "250.00", (
        f"{prefix} Z reads {got_z!r}, expected '250.00' (unitless - the readout "
        "shares one line, so values carry no ' mm' suffix)")


def test_unfilled_header_slot_leaves_control_overlay_geometry(tmp_path):
    """A header_bar that fills nothing renders exactly as it did pre-slot.

    bed_mesh uses header_bar with no slot children; its overlay header box is
    pinned to the measured pre-slot value. The slot collapses to width=content
    with nothing inside, so this only breaks if the slot starts costing layout.
    """
    app, before = _run_app("800x480", tmp_path)
    try:
        with app:
            app.wait_for("nav_buttons_enabled", 1, timeout=30)
            app.navigate("controls")
            app.wait_idle()
            app.click("btn_bed_mesh")
            app.wait_idle()
            overlays = app.current().get("overlays", [])
            assert any("bed_mesh" in o for o in overlays), (
                "clicking btn_bed_mesh did not open the bed mesh overlay")
            header = _geom(app, "overlay_header")
            assert (header["x"], header["y"], header["w"], header["h"]) == (92, 0, 708, 56), (
                f"bed_mesh header is {header['x'], header['y'], header['w'], header['h']}, "
                "expected the pre-slot (92, 0, 708, 56) - an unfilled header_content "
                "slot must cost no layout")
    finally:
        _restore_size(before)
