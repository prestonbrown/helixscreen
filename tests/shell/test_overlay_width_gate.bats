#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_overlay_width.py — the overlay width gate.
#
# The gate exists because the two overlay widths encode destination-vs-transient
# layer, and which one an overlay gets depends on how the user reached it. The
# same fan_control_overlay is a transient layer from Controls and a drill-down
# from Settings > Fans, so no static XML attribute is correct for it. Authors
# picking a constant by hand left 20 panels gapped and 36 full with no rule, and
# made console_settings_overlay render wider than the console_panel it was
# pushed from (#1178).
#
# These tests pin both halves of the contract. The silent cases matter as much
# as the loud ones: hand-rolled overlay roots legitimately name the transient
# constant, and widget_catalog_overlay legitimately sets width="70%". A gate
# that fired on those would be noise on every XML commit and get switched off.

GATE="scripts/check_overlay_width.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/overlay_width"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into a fixture .xml and run the gate over that file alone.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name.xml"
    run python3 "$GATE" "$FIXTURE_DIR/$name.xml"
}

# ---------------------------------------------------------------- must catch

@test "flags an overlay_panel hand-picking the destination width" {
    run_gate dest '<component>
  <view name="x" extends="overlay_panel" width="#overlay_width_destination" title="X"/>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"overlay_width_destination"* ]]
}

@test "flags an overlay_panel hand-picking the transient width" {
    run_gate trans '<component>
  <view name="x" extends="overlay_panel" width="#overlay_width_transient" title="X"/>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"hand-picks a navigation width class"* ]]
}

@test "flags the destination constant anywhere, not just on overlay_panel" {
    # Destination-ness is declared in C++ by is_destination(). XML never names it.
    run_gate dest_anywhere '<component>
  <view name="x" extends="lv_obj" width="#overlay_width_destination" align="right_mid"/>
</component>'
    [ "$status" -eq 1 ]
}

@test "flags a width attribute spread across multiple lines" {
    # Real overlays wrap their attributes; a line-oriented check would miss this.
    run_gate wrapped '<component>
  <view name="x"
        extends="overlay_panel"
        width="#overlay_width_destination"
        title="X"/>
</component>'
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------- must ignore

@test "silent on an overlay_panel with no width attribute" {
    run_gate clean '<component>
  <view name="x" extends="overlay_panel" title="X" title_tag="X"/>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on a hand-rolled lv_obj root using the transient constant" {
    # Pre-overlay_panel panels still need a born-width: setup() runs before the
    # push and several of them measure their own layout there.
    run_gate handrolled '<component>
  <view name="x" extends="lv_obj" width="#overlay_width_transient"
        height="100%" align="right_mid"/>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on a deliberate custom width — widget_catalog_overlay is 70%" {
    # Neither navigation class applies; the overlay opts out in C++ via
    # NavigationManager::set_overlay_width_unmanaged().
    run_gate custom '<component>
  <view name="x" extends="overlay_panel" width="70%" title="X"/>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on a percentage width inside the overlay content" {
    run_gate inner '<component>
  <view name="x" extends="overlay_panel" title="X">
    <lv_obj name="overlay_content" width="100%" flex_grow="1"/>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------- --staged-only content
#
# --staged-only picks its file SET from `git diff --cached`, but must read
# each file's STAGED content (the index blob), not the working tree. Stage a
# violation and then revert the working file to something clean, and a gate
# that reads the working tree reports nothing while the bad blob commits.

GATE_ABS="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/check_overlay_width.py"

setup_tmp_repo() {
    TMP_REPO="$(mktemp -d "${BATS_TEST_TMPDIR:-${BATS_TMPDIR:-/tmp}}/overlay-width-XXXXXX")"
    cd "$TMP_REPO" || return 1
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test"
    mkdir -p ui_xml
    printf '<component>\n  <view name="x" extends="overlay_panel" title="X"/>\n</component>\n' \
        > ui_xml/base.xml
    git add -A && git commit -qm base
}

@test "--staged-only catches a violation in the STAGED blob, not a reverted working file" {
    setup_tmp_repo
    printf '<component>\n  <view name="x" extends="overlay_panel" width="#overlay_width_destination" title="X"/>\n</component>\n' \
        > ui_xml/base.xml
    git add ui_xml/base.xml
    printf '<component>\n  <view name="x" extends="overlay_panel" title="X"/>\n</component>\n' \
        > ui_xml/base.xml
    run python3 "$GATE_ABS" --staged-only
    [ "$status" -eq 1 ]
    [[ "$output" == *"overlay_width_destination"* ]]
}

@test "--staged-only stays silent on a clean staged file with a dirty violation on disk" {
    setup_tmp_repo
    # Genuinely different from the base commit (an added title_tag=), so the
    # file shows up as staged at all - content byte-identical to HEAD stages
    # nothing for git to report, which would pass this test for free.
    printf '<component>\n  <view name="x" extends="overlay_panel" title="X" title_tag="X"/>\n</component>\n' \
        > ui_xml/base.xml
    git add ui_xml/base.xml
    printf '<component>\n  <view name="x" extends="overlay_panel" width="#overlay_width_destination" title="X"/>\n</component>\n' \
        > ui_xml/base.xml
    run python3 "$GATE_ABS" --staged-only
    [ "$status" -eq 0 ]
}
