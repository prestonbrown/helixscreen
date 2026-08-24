#!/usr/bin/env bats
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_modal_chrome_budget.py (#1277).
#
# Both halves of the contract are pinned: the shape it must catch (a pinned
# block below the scroll area, which clips the button row off the card) and the
# shapes it must stay quiet about. The silent cases matter as much as the loud
# ones — a gate that fires on correct markup gets switched off, and then it
# protects nothing.

setup() {
    REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"
    GATE="${REPO_ROOT}/scripts/check_modal_chrome_budget.py"
    FIXTURE="$(mktemp -d)"
    mkdir -p "${FIXTURE}/ui_xml"
}

teardown() {
    rm -rf "${FIXTURE}"
}

# --- the shape it must CATCH ------------------------------------------------

@test "flags a pinned block between the scroll area and the button row" {
    cat > "${FIXTURE}/ui_xml/bad_modal.xml" <<'EOF'
<component>
  <view name="bad_modal" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_max"/>
    <lv_obj name="pinned_graphic" height="content"/>
    <divider_horizontal/>
    <lv_obj height="#button_height" flex_flow="row"/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"pinned_graphic"* ]] || [[ "$output" == *"bad_modal.xml"* ]]
}

@test "flags a SECOND pinned block even when the container opted into the pinned cap" {
    # The pinned ladder reserves exactly one block. Two is back over budget.
    cat > "${FIXTURE}/ui_xml/two_pinned.xml" <<'EOF'
<component>
  <view name="two_pinned" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_pinned_max"/>
    <lv_obj name="graphic_one" height="content"/>
    <lv_obj name="graphic_two" height="content"/>
    <divider_horizontal/>
    <modal_button_row/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 1 ]
}

@test "flags a SECOND button row on the standard cap" {
    # The ladder budgets exactly ONE button row. A second (klipper_recovery's
    # restart row + Dismiss) is the tall-chrome shape and belongs on
    # #dialog_content_tall_chrome_max, which reserves that row's height.
    cat > "${FIXTURE}/ui_xml/two_rows.xml" <<'EOF'
<component>
  <view name="two_rows" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_max"/>
    <divider_horizontal/>
    <modal_button_row/>
    <divider_horizontal/>
    <lv_obj height="#button_height" flex_flow="row"/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"two_rows"* ]]
}

@test "flags a card raised above the shared 85% cap" {
    # klipper_recovery_dialog carried 90% for exactly this reason before the
    # tall-chrome ladder existed (#1277). Raising one card unsizes the ladder
    # arithmetic every modal shares, so the hack itself is flagged now.
    cat > "${FIXTURE}/ui_xml/own_cap.xml" <<'EOF'
<component>
  <view name="own_cap" extends="ui_dialog" height="content" style_max_height="90%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_max"/>
    <divider_horizontal/>
    <modal_button_row/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 1 ]
    [[ "$output" == *"90%"* ]]
}

# --- the shapes it must stay QUIET about ------------------------------------

@test "silent on the reference shape: content, divider, button row" {
    cat > "${FIXTURE}/ui_xml/good_modal.xml" <<'EOF'
<component>
  <view name="good_modal" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_max"/>
    <divider_horizontal/>
    <lv_obj height="#button_height" flex_flow="row"/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "silent when one pinned block opted into the pinned cap" {
    cat > "${FIXTURE}/ui_xml/pinned_ok.xml" <<'EOF'
<component>
  <view name="pinned_ok" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_pinned_max"/>
    <lv_obj name="pinned_graphic" height="content"/>
    <divider_horizontal/>
    <modal_button_row/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "silent on mutually exclusive blocks bound to the same subject" {
    # hidden_network_modal: the form and the connecting spinner swap on
    # `hidden_connecting`, so their heights never sum. Counting the second
    # against the first's budget would be a false positive.
    cat > "${FIXTURE}/ui_xml/exclusive.xml" <<'EOF'
<component>
  <view name="exclusive" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="form" height="content" style_max_height="#dialog_content_max">
      <bind_flag_if_eq subject="hidden_connecting" flag="hidden" ref_value="1"/>
    </lv_obj>
    <lv_obj name="connecting" height="#hidden_connecting_height">
      <bind_flag_if_eq subject="hidden_connecting" flag="hidden" ref_value="0"/>
    </lv_obj>
    <divider_horizontal/>
    <lv_obj height="#button_height" flex_flow="row"/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "silent on a wrapping button container (it IS the budgeted row)" {
    cat > "${FIXTURE}/ui_xml/wrap_row.xml" <<'EOF'
<component>
  <view name="wrap_row" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_max"/>
    <lv_obj name="button_container" height="content" flex_flow="row_wrap"/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "silent on a self-sized modal that uses no shared ladder" {
    # favorite_macro_config_modal caps its card at a flat 520px and never
    # references a #dialog_content_* token — it sized itself and is not the
    # shared budget's business. Only a RAISED cap on a modal that ALSO uses a
    # shared content token is flagged.
    cat > "${FIXTURE}/ui_xml/own_cap.xml" <<'EOF'
<component>
  <view name="own_cap" extends="ui_dialog" height="content" style_max_height="520">
    <lv_obj name="content" height="content" style_max_height="400"/>
    <divider_horizontal/>
    <modal_button_row/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "silent on the tall-chrome shape: second button row on the tall cap" {
    # klipper_recovery_dialog's shape — a restart-row block (wrapper hides with
    # the restart actions), its divider, then Dismiss — on the tall-chrome token.
    cat > "${FIXTURE}/ui_xml/tall_chrome.xml" <<'EOF'
<component>
  <view name="tall_chrome" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_tall_chrome_max"/>
    <lv_obj name="restart_actions" height="content">
      <bind_flag_if_eq subject="recovery_can_restart" flag="hidden" ref_value="0"/>
    </lv_obj>
    <divider_horizontal/>
    <ui_button name="dismiss" height="#button_height"/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "honours the file-level opt-out" {
    cat > "${FIXTURE}/ui_xml/opted_out.xml" <<'EOF'
<component>
  <!-- MODAL_CHROME_OK: deliberate, measured on device -->
  <view name="opted_out" extends="ui_dialog" height="content" style_max_height="85%">
    <lv_obj name="content" height="content" style_max_height="#dialog_content_max"/>
    <lv_obj name="pinned_graphic" height="content"/>
    <divider_horizontal/>
    <modal_button_row/>
  </view>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "malformed XML is left to the xmllint pass, not reported as a violation" {
    cat > "${FIXTURE}/ui_xml/broken.xml" <<'EOF'
<component>
  <view name="broken"
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "silent on a tree with no modals at all" {
    cat > "${FIXTURE}/ui_xml/plain.xml" <<'EOF'
<component>
  <view name="plain" extends="lv_obj"/>
</component>
EOF
    run python3 "${GATE}" --repo-root "${FIXTURE}"
    [ "$status" -eq 0 ]
}

@test "the real repository passes its own gate" {
    run python3 "${GATE}" --repo-root "${REPO_ROOT}"
    [ "$status" -eq 0 ]
}
