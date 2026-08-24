# SPDX-License-Identifier: GPL-3.0-or-later

"""Modal height budgets, measured through `ctl geom` on a real instance.

Klipper reports shutdown reasons as arbitrarily long text. The recovery card had
no height cap at any level, so a long reason grew it past both screen edges and
clipped its title and buttons off a 480x272 panel. These tests pin the card and
its last button inside the screen at three sizes, with a message long enough to
fill the 512-byte `recovery_message` subject buffer.

Geometry comes from `ctl geom`, which reports absolute screen coords. Measuring
the same thing in-process is a trap: `lv_obj_get_x/y()` is *parent-relative*, so
comparing a nested button's offset against an ancestor's screen-space edge
compares two coordinate spaces and passes regardless of the layout.

The subject-set message alone cannot reach the worst case on every breakpoint:
recovery_message_buf_ is 512 bytes, which pegs the container at its cap on
480x272 but leaves it under the cap on the taller tiers. The fixture therefore
ALSO does `ctl set_text` with a ~2000-char string, which bypasses the subject
buffer and pegs the scroll container at its cap at EVERY size — the state a
user hits with a long translated shutdown reason.

Mutation checks (measured 2026-08-14, not assumed):
  - raise the message container's style_max_height above its content token
    -> test_long_message_scrolls fails.
       This is the load-bearing guard.
  - dropping style_max_height from the *card* fails test_recovery_card_fits at
    800x480 and 1024x600: with the worst-case text the scroll container pegs at
    its cap and chrome + cap exceeds the screen there. (At 480x272 and 480x320
    the uncapped card still fits the screen; the content cap alone holds it.)

  - restore the pre-fix layout wholesale (`git show d44686c45:` this dialog — an
    lg icon on its own row above three full-width stacked buttons)
    -> test_recovery_card_fits fails at 480x272 AND 800x480, passes at 1024x600.
       That is the originally reported bug, and it is a small-screen bug.

The three assertions catch different failure modes, and none subsumes another:

  test_recovery_card_fits_the_screen   card grows past the screen (chrome too tall)
  test_dismiss_button_stays_inside     card pinned AT its cap, contents overflow it
  test_recovery_card_honors_the_standard_cap
                                      card raised ABOVE the shared 85% cap, the
                                      per-dialog patch #1277 retired

The second does NOT fail on a screen-overflow layout: there the card and its
buttons run off the screen together, so the button is still within the card's
own bounds. It exists for the case that actually bit during this fix — a card
clamped at max_height whose last child lands past the clamped edge, unreachable
because the card is scrollable=false. That was 4px at 85%, which this dialog
first papered over with a 90% card cap; the tall-chrome ladder now budgets the
second button row instead (#1277).
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from helix.app import HelixApp

_BINARY = Path(os.environ.get(
    "HELIX_UI_BINARY",
    str(Path(__file__).resolve().parents[2] / "build" / "bin" / "helix-screen")))

# Sized to fill recovery_message_buf_[512] (511 usable), so the message container
# is pegged at its cap and the card is at its worst case. Measured: at 480x272 a
# 340-char message still fits the container without scrolling, so a shorter string
# here would assert nothing about the scroll path.
_LONG_REASON = (
    "Internal error during ready callback: No active exception to reraise. "
    "MCU shutdown: Timer too close. This often indicates the host computer is "
    "overloaded. Check for other processes consuming CPU. Once the underlying "
    "issue is corrected, use the FIRMWARE_RESTART command to reset the firmware, "
    "reload the config and restart the host software. Check the host for CPU "
    "contention from other services, verify no USB device is resetting, and "
    "review klippy.log around the shutdown timestamp for the first error."
)
assert 480 <= len(_LONG_REASON) <= 511, (
    f"message must fill the 512-byte subject buffer without being truncated "
    f"mid-test; got {len(_LONG_REASON)}")

# The subject buffer cannot produce the worst case at every breakpoint (a
# 511-char string pegs the container at 480x272 but not on the taller tiers,
# where #dialog_content_max grows faster than the text). `set_text` writes the
# label directly with no buffer, so this pegs the scroll container at its cap
# at every size — chrome + full cap is the shape the 85% budget is derived from.
_WORST_REASON = "MCU shutdown: Timer too close. " * 56
assert len(_WORST_REASON) >= 1500, (
    f"worst-case message must overflow the tallest content cap once wrapped; "
    f"got {len(_WORST_REASON)}")

# 480x272 is the smallest panel we ship (Qidi Q2, AD5M); 480x320 the next
# (SonicPad). The other two cover the breakpoints where #dialog_content_max
# steps up.
_SIZES = ["480x272", "480x320", "800x480", "1024x600"]


def _geom(app: HelixApp, target: str) -> dict:
    """First widget geom record for `target`, or None when it is not on screen."""
    result = app.geom(target)
    widgets = result.get("widgets") if isinstance(result, dict) else None
    return widgets[0] if widgets else None


@pytest.fixture
def shutdown_app(request, tmp_path):
    """An instance at a given screen size, sitting on the recovery dialog."""
    size = request.param
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built — run `make -j`")

    before = os.environ.get("HELIX_SCREEN_SIZE")
    os.environ["HELIX_SCREEN_SIZE"] = size
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log")
        with app:
            # "error" puts klippy into an error state, which is what raises the
            # unified recovery dialog.
            app.ctl("scenario", "error")
            app.wait_idle()
            assert _geom(app, "klipper_recovery_card") is not None, (
                "recovery dialog never appeared; scenario 'error' may have changed")
            app.set("recovery_message", _LONG_REASON)
            app.wait_idle()
            # Peg the container at its cap at every size (see _WORST_REASON).
            app.ctl("set_text", "recovery_message", _WORST_REASON)
            app.wait_idle()
            yield app, size
    finally:
        if before is None:
            os.environ.pop("HELIX_SCREEN_SIZE", None)
        else:
            os.environ["HELIX_SCREEN_SIZE"] = before


@pytest.mark.parametrize("shutdown_app", _SIZES, indirect=True)
def test_recovery_card_fits_the_screen(shutdown_app):
    app, size = shutdown_app
    screen_h = int(size.split("x")[1])

    card = _geom(app, "klipper_recovery_card")
    assert card is not None
    top, bottom = card["y"], card["y"] + card["h"]
    assert top >= 0, f"{size}: card starts above the screen at y={top}"
    assert bottom <= screen_h, (
        f"{size}: card runs {bottom - screen_h}px past the bottom "
        f"(y={top} h={card['h']})")


@pytest.mark.parametrize("shutdown_app", _SIZES, indirect=True)
def test_dismiss_button_stays_inside_the_card(shutdown_app):
    """The last child is what a capped card clips first.

    The card is `scrollable=false`, so anything past its bottom edge is not
    merely off-view, it is unreachable — the user cannot dismiss the dialog.
    """
    app, size = shutdown_app

    card = _geom(app, "klipper_recovery_card")
    dismiss = _geom(app, "recovery_dismiss_btn")
    assert card is not None and dismiss is not None

    card_bottom = card["y"] + card["h"]
    dismiss_bottom = dismiss["y"] + dismiss["h"]
    assert dismiss_bottom <= card_bottom, (
        f"{size}: Dismiss runs {dismiss_bottom - card_bottom}px past the card "
        f"(card y={card['y']} h={card['h']}, button y={dismiss['y']} h={dismiss['h']})")


@pytest.mark.parametrize("shutdown_app", _SIZES, indirect=True)
def test_recovery_card_honors_the_standard_cap(shutdown_app):
    """The card cap is shared arithmetic, not a per-dialog dial (#1277).

    #dialog_content_max and its sibling ladders are derived from ONE card cap —
    85% of the screen — at every breakpoint. Raising a single card above that
    (this dialog carried 90% for a while) unsizes every ladder it shares and is
    how the clipped-button family keeps coming back: the extra chrome belongs
    in the budget, via #dialog_content_tall_chrome_max, not in a taller card.
    """
    app, size = shutdown_app
    screen_h = int(size.split("x")[1])

    card = _geom(app, "klipper_recovery_card")
    assert card is not None
    cap = screen_h * 85 // 100  # LVGL floors the percentage
    assert card["h"] <= cap, (
        f"{size}: card is {card['h']}px against the shared {cap}px cap "
        f"(85% of {screen_h}) — extra chrome belongs in "
        f"#dialog_content_tall_chrome_max, not a raised card")


@pytest.mark.parametrize("shutdown_app", _SIZES, indirect=True)
def test_long_message_scrolls_instead_of_growing_the_card(shutdown_app):
    """A capped container with no scroll range is an inert fix, not a fix.

    The worst-case text pegs the container at every size, so the scroll range
    is checkable everywhere.
    """
    app, _ = shutdown_app

    scroll = _geom(app, "recovery_message_scroll")
    assert scroll is not None, "message container is missing its name"
    assert scroll["scrollable"] is True

    overflow = scroll["scroll"]["bottom"] + scroll["scroll"]["top"]
    assert overflow > 0, (
        "message container reports no scroll range, so the cap is truncating "
        f"content rather than making it reachable (h={scroll['h']}, "
        f"content_h={scroll['content_h']})")


# ============================================================================
# AMS loading-error modal
#
# The AFC position diagram sits OUTSIDE this modal's scrollable text area, so it
# never scrolls out of view — which also means it counts against the card's fixed
# height budget. #dialog_content_max is documented in globals.xml as sized for a
# header + divider + one button row, so this card is already over that allowance
# and the arithmetic deserves pinning rather than trusting.
#
# Reaching it: the AMS panel is not on the navbar. Go to the filament panel and
# click the mini AMS status widget, which pushes the ams_panel overlay; the modal
# is then raised by ams_action reaching ERROR (9).
# ============================================================================

_AMS_ACTION_ERROR = 9  # AmsAction::ERROR, ams_types.h

# A recognised AFC fault publishes its stop point here and the <afc_fault_path>
# graphic binds `hidden` to it, so setting it directly is enough to show the
# diagram without needing a backend that produces the matching fault text.
_AFC_SEGMENT_VISIBLE = 2


@pytest.fixture
def ams_error_app(request, tmp_path):
    """An instance sitting on the AMS loading-error modal, held open.

    The panel dismisses this modal as soon as ams_action leaves ERROR, and the
    mock pushes its own state back within ~400ms, so the modal is only briefly
    real. `freeze` pins it; subjects still propagate while frozen, which is what
    lets the diagram be toggled afterwards.
    """
    size = getattr(request, "param", "480x272")
    if not _BINARY.exists():
        pytest.skip(f"{_BINARY} not built — run `make -j`")

    before_size = os.environ.get("HELIX_SCREEN_SIZE")
    before_ams = os.environ.get("HELIX_MOCK_AMS")
    os.environ["HELIX_SCREEN_SIZE"] = size
    os.environ["HELIX_MOCK_AMS"] = "afc"
    try:
        app = HelixApp(binary=_BINARY, socket_path=tmp_path / "control.sock",
                       log_path=tmp_path / "app.log")
        with app:
            app.navigate("filament")
            app.wait_idle()
            app.click("ams_mini_status")
            app.wait_idle()
            assert "ams_panel" in app.current().get("overlays", []), (
                "clicking the mini AMS status did not open the AMS panel")

            # Racing the mock's next state push: set, freeze, confirm, retry.
            for _ in range(5):
                app.set("ams_action", _AMS_ACTION_ERROR)
                app.freeze()
                if _geom(app, "ams_loading_error_modal") is not None:
                    break
                app.unfreeze()
            else:
                pytest.fail("could not pin the AMS loading-error modal open")

            # The mock's fault text is a short generic sentence, which leaves the
            # card ~138px on a 272px panel — far enough from any cap that no cap
            # mutation could fail these tests. This message comes from a backend
            # field rather than a subject, so `set_text` is the only way to reach
            # it and make the height budget actually load-bearing.
            app.ctl("set_text", "error_message", _LONG_REASON)
            app.wait_idle()

            try:
                yield app, size
            finally:
                app.unfreeze()
    finally:
        for key, val in (("HELIX_SCREEN_SIZE", before_size),
                         ("HELIX_MOCK_AMS", before_ams)):
            if val is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = val


def test_ams_error_modal_fits_the_screen(ams_error_app):
    app, size = ams_error_app
    screen_h = int(size.split("x")[1])

    card = _geom(app, "ams_loading_error_modal")
    assert card is not None
    top, bottom = card["y"], card["y"] + card["h"]
    assert top >= 0, f"{size}: card starts above the screen at y={top}"
    assert bottom <= screen_h, (
        f"{size}: card runs {bottom - screen_h}px past the bottom "
        f"(y={top} h={card['h']})")


def test_ams_error_modal_fits_with_the_fault_diagram(ams_error_app):
    """The pinned diagram is extra fixed chrome — the tightest case on a micro panel."""
    app, size = ams_error_app
    screen_h = int(size.split("x")[1])

    app.set("afc_fault_segment", _AFC_SEGMENT_VISIBLE)
    app.wait_idle()

    diagram = _geom(app, "fault_path")
    assert diagram is not None, "fault_path did not appear for a recognised segment"

    card = _geom(app, "ams_loading_error_modal")
    primary = _geom(app, "btn_primary")
    assert card is not None and primary is not None

    card_bottom = card["y"] + card["h"]
    assert card["y"] >= 0 and card_bottom <= screen_h, (
        f"{size}: card {card['y']}..{card_bottom} outside 0..{screen_h}")

    # The button row is the last child, so a card pinned at its cap clips it first.
    btn_bottom = primary["y"] + primary["h"]
    assert btn_bottom <= card_bottom, (
        f"{size}: Retry runs {btn_bottom - card_bottom}px past the card "
        f"(card y={card['y']} h={card['h']}, button y={primary['y']} h={primary['h']}, "
        f"diagram h={diagram['h']})")
