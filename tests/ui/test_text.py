# SPDX-License-Identifier: GPL-3.0-or-later

"""Reading label text — the most common assertion in any UI test."""

import pytest

from helix.app import HelixCtlError


def test_descends_to_the_label_inside_a_composite(helix_app):
    # A button wrapping a label should report the label's text rather than
    # failing, mirroring how click() descends to a value-control.
    helix_app.navigate("controls")
    helix_app.wait_idle()
    value = helix_app.text("btn_motion")
    assert value.strip(), f"btn_motion reported empty text: {value!r}"


def test_text_matches_a_subject_we_set(helix_app):
    # Content correctness, not just "returned some string": drive a
    # subject-bound label to a value we control, then confirm `text` reports
    # exactly that value back. A `text` that returned the widget's name, or
    # any other plausible-but-wrong string, would still pass every other test
    # in this file — only this one proves the read path end-to-end.
    #
    # `about_copyright` (Settings > Help > About) is set once at overlay
    # construction and never touched again by any background system, so
    # there is nothing racing our write.
    helix_app.navigate("settings")
    helix_app.wait_idle()
    helix_app.click("row_help")
    helix_app.wait_idle()
    helix_app.click("row_about")
    helix_app.wait_idle()
    try:
        probe = "helixctl-text-probe-12345"
        helix_app.set("about_copyright", probe)
        assert helix_app.text("copyright_text") == probe
    finally:
        # Restore navigation state for later tests sharing this session-scoped
        # app: set_active() preserves overlays across a base-panel switch, so
        # leaving these two open would leak into whatever test runs next.
        helix_app.go_back()  # pop about_settings_overlay
        helix_app.go_back()  # pop settings_help_overlay


def test_unknown_target_raises(helix_app):
    with pytest.raises(HelixCtlError):
        helix_app.text("definitely_not_a_widget")


# Home-panel widgets that render no text at all, and cannot acquire a
# descended label without being redesigned: `temp_graph` plots its series onto
# a canvas, `ams_bars_container` draws filled bars, and a `divider_horizontal`
# is a bare rule with no children. Several are listed because the anchor has to
# survive a home-layout change — the widget this test used to key on lived
# inside the idle print card, and vanished the moment that card was replaced.
#
# Leaf-ness cannot be derived from the `ls` listing instead: the walk emits only
# *named* widgets while still recursing through unnamed ones, so a widget with
# no listed children (`temp_text_label`, `idle_data_col`) can still own an
# unnamed label and report its text.
_NO_TEXT_HOME_ANCHORS = ("temp_graph", "ams_bars_container")


def test_widget_with_no_text_raises_rather_than_returning_empty(helix_app):
    # An empty string and "this widget has no text" are different facts, and
    # conflating them makes an assertion silently vacuous.
    helix_app.navigate("home")
    helix_app.wait_idle()
    listing = helix_app.ls()
    containers = [w for w in listing["widgets"]
                  if w.get("name") and w.get("type") == "obj"]
    # There is no arbitrary fallback here on purpose: `containers[0]` on this
    # screen is `app_layout_0`, which wraps the entire panel and genuinely does
    # have a descended label — silently falling back to it would turn this into
    # a false failure on *correct* code instead of testing the no-text case.
    anchor = next((w for w in containers
                   if w["name"] in _NO_TEXT_HOME_ANCHORS or "divider" in w["name"]),
                  None)
    if anchor is None:
        pytest.fail("none of the known no-text anchors are on the home panel "
                    f"({', '.join(_NO_TEXT_HOME_ANCHORS)}, or a divider) — add a "
                    "different verified no-text widget rather than falling back "
                    "to an arbitrary container (see comment above)")
    # Matching the reason, not just the type: an anchor that stopped resolving
    # would raise "Widget not found" and satisfy a bare `raises(HelixCtlError)`
    # without ever exercising the no-text path.
    with pytest.raises(HelixCtlError, match="has no text"):
        helix_app.text(anchor["name"])
