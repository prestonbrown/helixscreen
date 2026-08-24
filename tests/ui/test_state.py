# SPDX-License-Identifier: GPL-3.0-or-later

"""`state <target>` — reading a widget's LVGL states and flags.

Motivated by verification that a `disabled=` subject binding really put a
switch into LV_STATE_DISABLED: `click` reports what it toggled but nothing
could prove the switch would refuse a real tap. Hidden widgets stay
resolvable by name (only `ls` filters them), so bind_flag_if contracts are
assertable here too.
"""

import pytest

from helix.app import HelixCtlError


def test_reports_states_and_flags_of_a_button(helix_app):
    helix_app.navigate("controls")
    helix_app.wait_idle()
    s = helix_app.state("btn_motion")
    assert isinstance(s["states"], list)
    assert s["disabled"] is False
    assert s["flags"]["clickable"] is True
    # The booleans and the array must agree — they are two views of one query.
    assert ("checked" in s["states"]) == s["checked"]


def test_descends_a_composite_row_to_its_control(helix_app):
    # Same descent click/set_value use: `state <row>` reports on the switch
    # inside, so callers don't need the toggle's path from ls.
    helix_app.navigate("settings")
    helix_app.wait_idle()
    helix_app.click("row_display_sound")
    helix_app.wait_idle()
    try:
        s = helix_app.state("row_widget_labels")
        assert s.get("descended_to"), "row_widget_labels did not descend to its switch"
        assert s["descended_to"].endswith("/toggle")
    finally:
        helix_app.go_back()


def test_checked_follows_the_bound_subject(helix_app):
    # Content correctness, not just shape: drive the subject a switch is bound
    # to and confirm `state` observes the flip (bind_state_if_eq wiring).
    # row_widget_labels is desktop-visible and driven by show_widget_labels.
    helix_app.navigate("settings")
    helix_app.wait_idle()
    helix_app.click("row_display_sound")
    helix_app.wait_idle()
    before = None
    toggle_path = None
    try:
        listing = helix_app.ls()
        widget_labels_row = next((w for w in listing["widgets"]
                                  if w.get("name") == "row_widget_labels"), None)
        assert widget_labels_row is not None, "row_widget_labels not listed"
        toggle_path = widget_labels_row["path"] + "/toggle"
        before = helix_app.state(toggle_path)["checked"]
        helix_app.set("show_widget_labels", 0 if before else 1)
        helix_app.wait_idle()
        after = helix_app.state(toggle_path)["checked"]
        assert after == (not before), (
            f"switch stayed checked={after} after flipping its subject "
            f"(was {before}) — either the wrong anchor or a dead binding")
    finally:
        if before is not None:
            helix_app.set("show_widget_labels", 1 if before else 0)
        helix_app.go_back()


def test_hidden_widget_is_still_resolvable_and_reports_the_flag(helix_app):
    # The AMS Management overlay builds every row and hides the ones the
    # backend does not support; on the default mock no backend reports spool
    # ids, so the keep-spool-info row exists but is hidden. `ls` will not
    # list it — resolving it by name and reading flags.hidden is the point.
    helix_app.navigate("settings")
    helix_app.wait_idle()
    helix_app.click("row_hardware")
    helix_app.wait_idle()
    helix_app.click("row_ams_settings")
    helix_app.wait_idle()
    try:
        s = helix_app.state("row_ams_keep_spool_info_on_eject")
        hidden = s.get("target", s)["flags"]["hidden"]
        assert hidden is True, (
            "keep-spool row visible without a spool-id-reporting backend — "
            "either the anchor became wrong (pick another gated row) or the "
            "hide binding broke")
    finally:
        helix_app.go_back()
        helix_app.go_back()


def test_unknown_target_raises(helix_app):
    with pytest.raises(HelixCtlError):
        helix_app.state("definitely_not_a_widget")
