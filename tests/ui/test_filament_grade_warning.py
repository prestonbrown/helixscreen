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

Mutation check (measured 2026-08-18, not assumed):
  - VARIANT_AFFIXES[]: flip CF's `filled` column to false
    -> both filled_lane tests fail, and they fail by the dialog never appearing
       at all: PLA-CF now reads as unfilled, grades_match() accepts the lane,
       and the print simply starts. test_matching_lane still passes, which is
       what proves the two are measuring different things. That table is the
       load-bearing part of the feature.

The directional wording is not separately mutation-checked here; its assertion
("hardened") is pinned by the gate-level unit test in test_print_start_gates.cpp
against both directions, which is a cheaper place to catch a reworded string.
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

# The print-select grid, whose card order conftest pins by normalizing fixture
# mtimes. Card 0 is the calibration cube: single used tool, plain PLA.
_CARD0 = ("s/app_layout_0/content_area/panel_container/print_select_panel"
          "/card_view_container/card_root[0]")
_CARD0_NAME = "xyz-10mm-calibration-cube"
# By name, not by path: the modal's screen index depends on how many screens
# the instance has created, so an "s/5/..." path is per-instance and breaks the
# moment a test boots its own app.
_DIALOG_TITLE = "dialog_title"
_DIALOG_MESSAGE = "dialog_message"
_DIALOG_PRIMARY = "btn_primary"


@pytest.fixture
def filled_lane_app(tmp_path):
    """An instance whose every AMS lane holds PLA-CF."""
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built - run `make -j`")
    app = HelixApp(binary=_BINARY,
                   socket_path=tmp_path / "control.sock",
                   log_path=tmp_path / "app.log",
                   extra_env={"HELIX_MOCK_AMS_STATE": "grade"})
    with app:
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
        f"print-select card 0 is {shown!r}, not the plain-PLA fixture these tests "
        "drive; conftest's mtime pinning may have changed")
    app.click(_CARD0)
    app.wait_idle()
    # The filament card is the observable end of the metadata load the gates read.
    _await_widget(app, "filament_mapping_card")
    _tap_print_until_a_dialog_answers(app)


def test_filled_lane_warns_with_the_abrasive_direction(filled_lane_app):
    _tap_print_on_the_pla_cube(filled_lane_app)

    assert filled_lane_app.text(_DIALOG_TITLE) == "Filament Grade Mismatch"

    message = filled_lane_app.text(_DIALOG_MESSAGE)
    # Both materials are named: a dialog that says only "grade mismatch" leaves
    # the user to go hunting for which lane and which grade.
    assert "PLA-CF" in message
    # The loaded spool is the filled one, so this is the direction that costs
    # hardware, and the dialog has to say so.
    assert "hardened" in message


def test_filled_lane_still_maps_and_the_warning_is_click_through(filled_lane_app):
    _tap_print_on_the_pla_cube(filled_lane_app)

    # A grade change is a warning, not a routing change: the mapper picked the
    # filled lane exactly as it would have picked the plain one, which is why
    # the dialog can name a loaded material at all rather than reporting an
    # unresolved tool.
    message = filled_lane_app.text(_DIALOG_MESSAGE)
    assert "sliced for PLA" in message

    # And it is dismissible rather than a block.
    assert filled_lane_app.text(_DIALOG_PRIMARY) == "Start Anyway"


def test_matching_lane_shows_no_grade_dialog(helix_app):
    """The default mock's lanes hold plain filament: no grade dialog."""
    _tap_print_on_the_pla_cube(helix_app)

    # This fixture does raise the ordinary material dialog against the default
    # lanes (the cube's used tool falls through to an ASA lane), which is the
    # point: the gate is speaking, and it is NOT saying "grade".
    # Asserted as an equality rather than "not the grade title": a dialog that
    # failed to appear, or a stale one left by another test, would satisfy the
    # negative form.
    assert helix_app.text(_DIALOG_TITLE) == "Material Mismatch"
