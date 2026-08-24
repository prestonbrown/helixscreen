# SPDX-License-Identifier: GPL-3.0-or-later

"""Navigation smoke tests — the automated replacement for test-navigation.sh."""

import pytest


def test_app_responds_to_ping(helix_app):
    assert helix_app.ctl("ping") == "pong"


def test_lists_the_expected_base_panels(helix_app):
    panels = helix_app.ctl("list_panels")["panels"]
    # list_panels returns the fixed PanelId set; these four are load-bearing
    # enough that losing one is a regression worth failing on.
    for expected in ("home", "controls", "settings", "print-select"):
        assert expected in panels, f"{expected} missing from {panels}"


@pytest.mark.parametrize("panel", ["home", "controls", "settings"])
def test_navigate_to_each_base_panel(helix_app, panel):
    helix_app.navigate(panel)
    assert helix_app.current()["panel"] == panel


def test_descend_into_an_overlay_and_back(helix_app):
    helix_app.navigate("controls")
    helix_app.click("btn_motion")
    after_click = helix_app.current()
    assert after_click["overlays"], "clicking btn_motion pushed no overlay"

    helix_app.go_back()
    assert helix_app.current()["overlays"] == []


def test_navigate_pops_an_open_overlay_stack(helix_app):
    # navigate used to call set_active(), which deliberately preserves the
    # overlay stack so the base panel can be swapped underneath an open
    # overlay. Driving it that way left the overlay and its opaque snapshot
    # backdrop covering the screen while reporting a successful navigation, so
    # every screenshot afterwards came back byte-identical. A finger on the
    # navbar clears the stack; navigate must do the same.
    helix_app.navigate("controls")
    helix_app.click("btn_motion")
    helix_app.wait_idle()
    assert helix_app.current()["overlays"], "setup failed — no overlay pushed"

    result = helix_app.navigate("settings")
    assert result["switched"] is True

    state = helix_app.current()
    assert state["panel"] == "settings"
    assert state["overlays"] == [], "navigate left the overlay stack standing"


def test_navigate_is_synchronous(helix_app):
    # The switch runs inline rather than queued, so the very next command sees
    # the new panel. A queued switch would make this race.
    helix_app.navigate("home")
    helix_app.navigate("controls")
    assert helix_app.current()["panel"] == "controls"


def test_navigate_to_the_panel_already_showing_is_not_a_switch(helix_app):
    helix_app.navigate("settings")
    helix_app.wait_idle()
    repeat = helix_app.navigate("settings")
    assert repeat["switched"] is False
    assert helix_app.current()["panel"] == "settings"


def test_retapping_home_reports_the_carousel_reset(helix_app):
    helix_app.navigate("home")
    helix_app.wait_idle()
    again = helix_app.navigate("home")
    # Home is the one panel where landing on it twice does something rather
    # than nothing — it scrolls the widget carousel back to page 0.
    assert again["home_retapped"] is True
    assert again["switched"] is False


def test_unknown_widget_raises_with_the_command_attached(helix_app):
    from helix.app import HelixCtlError

    helix_app.navigate("home")
    with pytest.raises(HelixCtlError) as exc:
        helix_app.click("no_such_widget_xyz")
    assert "no_such_widget_xyz" in " ".join(exc.value.command)
