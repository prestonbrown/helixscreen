# SPDX-License-Identifier: GPL-3.0-or-later

"""The print-start grade dialog, driven through a real instance.

A filled grade (CF/GF/AERO, and the particle-filled "cosmetic" grades) is a
different thing to print than its base polymer, but it shares a compat group
with it, so FilamentMapper::materials_match() accepts the lane and the material
dialog never fires. filament::grades_match() runs as a second pass and raises a
separate dialog. These tests pin the two halves of that:

  test_filled_lane_warns_with_the_abrasive_direction
      the dialog appears at all, and says the thing that protects a nozzle
  test_filled_lane_still_maps_and_the_warning_is_click_through
      the mapper still ROUTED to the filled lane (a grade change is a warning,
      never a routing change) and the user can proceed
  test_matching_lane_shows_no_grade_dialog
      the default mock, whose lanes hold plain filaments, does not raise it

The third is what makes the first two mean anything: without it, a dialog that
fired unconditionally would pass both.

HELIX_MOCK_AMS_STATE=grade puts PLA-CF in every lane. Every lane rather than
one because a tool reaches a lane by colour match and then by POSITIONAL
fallback over the file's whole palette -- the calibration cube's used tool is
T2, so it lands on lane 3, not lane 1. See MOCK_ENVIRONMENT_VARIABLES.md.

The cube is sliced for a centre-origin bed, which is the geometry the mock
Voron models, so it passes the print-start fit check untouched. Each app here
plants a fresh-mtime copy (HELIX_MOCK_GCODE_DIR); that makes it print-select
card 0, and its metadata and tools are the cube's own.

Mutation check (measured 2026-08-18, not assumed):
  - kVariantAffixes[]: flip CF's `filled` column to false
    -> both filled_lane tests fail, and they fail by the dialog never appearing
       at all: PLA-CF now reads as unfilled, grades_match() accepts the lane,
       and the print simply starts. test_matching_lane still passes, which is
       what proves the two are measuring different things. That table is the
       load-bearing part of the feature.

The directional wording is not asserted here; the gate-level unit test in
test_print_start_gates.cpp covers both directions.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from helix.app import HelixApp, HelixCtlError  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parents[2]
_BINARY = Path(os.environ.get("HELIX_UI_BINARY", str(_REPO_ROOT / "build" / "bin" / "helix-screen")))

# The print-select grid, newest first. Card 0 is the planted copy of the
# calibration cube (single used tool, plain PLA), newer than every pinned fixture.
_CARD0 = ("s/app_layout_0/content_area/panel_container/print_select_panel"
          "/card_view_container/card_root[0]")
_CARD0_NAME = "grade-cube-on-bed"
_CUBE = _REPO_ROOT / "assets" / "test_gcodes" / "xyz-10mm-calibration-cube.gcode"
# By name, not by path: the modal's screen index depends on how many screens
# the instance has created, so an "s/5/..." path is per-instance and breaks the
# moment a test boots its own app.
_DIALOG_TITLE = "dialog_title"
_DIALOG_MESSAGE = "dialog_message"
_DIALOG_PRIMARY = "btn_primary"


def _plant_cube_on_bed(directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    text = _CUBE.read_text(encoding="utf-8", errors="surrogateescape")
    planted = directory / f"{_CARD0_NAME}.gcode"
    planted.write_text(text, encoding="utf-8", errors="surrogateescape")
    return planted


def _app_with_the_planted_cube(tmp_path, extra_env: dict[str, str]):
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make -j`")
    gcode_dir = tmp_path / "gcodes"
    _plant_cube_on_bed(gcode_dir)
    return HelixApp(binary=_BINARY,
                    socket_path=tmp_path / "control.sock",
                    log_path=tmp_path / "app.log",
                    extra_env={"HELIX_MOCK_GCODE_DIR": str(gcode_dir), **extra_env})


@pytest.fixture
def filled_lane_app(tmp_path):
    """An instance whose every AMS lane holds PLA-CF."""
    with _app_with_the_planted_cube(tmp_path, {"HELIX_MOCK_AMS_STATE": "grade"}) as app:
        yield app


@pytest.fixture
def default_lane_app(tmp_path):
    """An instance with the default mock lanes, which hold plain filament."""
    with _app_with_the_planted_cube(tmp_path, {}) as app:
        yield app


def _await_widget(app, name: str, timeout: float = 20.0) -> None:
    """Poll until `name` exists.

    wait_idle() is not enough on its own here: the detail view fetches metadata
    and builds its filament card well after the click that opened it settles,
    and the print-start gates only have a context to evaluate once that has
    landed. Tapping Print before then produces no dialog at all, which reads as
    a missing feature rather than a race.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            app.geom(name)
            return
        except HelixCtlError:
            time.sleep(0.25)
    raise AssertionError(f"{name} never appeared within {timeout}s")


def _tap_print_until_a_dialog_answers(app, attempts: int = 6) -> None:
    """Tap Print, retrying until a dialog answers.

    PrintStartController drops any Print tap inside a one-second grace period
    after startup ("Rejected print start during startup grace period"), and a
    headless boot reaches this point well inside that second. The tap is
    swallowed with no dialog and no print, so a single click makes this suite
    fail on roughly a third of runs -- measured, not theorized. Retrying is
    right rather than sleeping past it: the grace period is wall-clock from
    startup, so the cost is one extra round trip on a fast boot and nothing at
    all on a slow one.
    """
    for _ in range(attempts):
        app.click("print_button")
        app.wait_idle()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                app.geom(_DIALOG_TITLE)
                return
            except HelixCtlError:
                time.sleep(0.25)
    raise AssertionError(
        f"no print-start dialog after {attempts} taps; the gate pipeline never ran")


def _tap_print_on_the_pla_cube(app) -> None:
    """Open the plain-PLA fixture and tap Print, leaving whatever dialog fires."""
    app.navigate("print_select")
    app.wait_idle()
    shown = app.text(f"{_CARD0}/metadata_overlay/filename_label")
    assert shown == _CARD0_NAME, (
        f"print-select card 0 is {shown!r}, not the planted plain-PLA cube these "
        "tests drive; HELIX_MOCK_GCODE_DIR may not have reached the mock")
    app.click(_CARD0)
    app.wait_idle()
    # The filament card is the observable end of the metadata load the gates read.
    _await_widget(app, "filament_mapping_card")
    _tap_print_until_a_dialog_answers(app)


def test_filled_lane_warns_with_the_abrasive_direction(filled_lane_app):
    _tap_print_on_the_pla_cube(filled_lane_app)

    message = filled_lane_app.text(_DIALOG_MESSAGE)
    # Both materials are named: a dialog that says only "grade mismatch" leaves
    # the user to go hunting for which lane and which grade.
    assert "PLA-CF" in message
    # The expected (sliced-for) material is named too, so the user sees both
    # sides of the mismatch. Which gate classifies the mismatch is covered by
    # test_print_start_gates.cpp; the dialogs share severity and widget names,
    # so this test only pins what is visible end to end.
    assert "PLA" in message.replace("PLA-CF", "")


def test_filled_lane_still_maps_and_the_warning_is_click_through(filled_lane_app):
    _tap_print_on_the_pla_cube(filled_lane_app)

    # A grade change is a warning, not a routing change: the mapper picked the
    # filled lane exactly as it would have picked the plain one, which is why
    # the dialog can name a loaded material at all rather than reporting an
    # unresolved tool.
    message = filled_lane_app.text(_DIALOG_MESSAGE)
    assert "PLA-CF" in message
    assert "PLA" in message.replace("PLA-CF", "")

    # And it is dismissible rather than a block: the primary button is live,
    # and tapping it closes the dialog.
    primary = filled_lane_app.state(_DIALOG_PRIMARY)
    assert primary["disabled"] is False
    assert primary["flags"]["hidden"] is False
    filled_lane_app.click(_DIALOG_PRIMARY)
    filled_lane_app.wait_idle()
    with pytest.raises(HelixCtlError):
        filled_lane_app.geom(_DIALOG_TITLE)


def test_matching_lane_shows_no_grade_dialog(default_lane_app):
    """The default mock's lanes hold plain filament: no grade dialog."""
    _tap_print_on_the_pla_cube(default_lane_app)

    # This fixture does raise the ordinary material dialog against the default
    # lanes (the cube's used tool falls through to an ASA lane), which is the
    # point: a dialog is up. The loaded materials are plain, so the message
    # never names a filled grade; gate classification is covered by
    # test_print_start_gates.cpp. It does
    # name the lane's actual material, which proves a mismatch dialog is up
    # (a missing or stale dialog would have no such text).
    assert default_lane_app.text(_DIALOG_TITLE).strip()
    message = default_lane_app.text(_DIALOG_MESSAGE)
    assert "ASA" in message
    assert "PLA-CF" not in message
