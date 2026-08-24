#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_hardcoded_pixels.py — the design-token gate.
#
# The gate exists because HelixScreen ships on screens from 480x272 to 1440p and
# a raw style_pad_all="12" is 12px on every one of them. The token ladders in
# ui_xml/globals.xml resolve per breakpoint; a literal freezes whatever the
# author's dev window happened to be.
#
# These tests pin both halves of the contract. The silent cases matter as much as
# the loud ones: width="1" is a REQUIRED LVGL flex idiom, style_pad_all="0" is a
# reset, and the crash screens (helix_watchdog.cpp, ui_fatal_error.cpp) render
# before theme init where a token read returns 0 and would collapse the layout to
# nothing. A gate that fired on those would be noise and get switched off.

GATE="scripts/check_hardcoded_pixels.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/hardcoded_pixels"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into a fixture file named $1 and run the gate over that file alone.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name"
    run python3 "$GATE" --list "$FIXTURE_DIR/$name"
}

# ---------------------------------------------------------------- must catch

@test "flags a literal style_pad_all in XML" {
    run_gate pad.xml '<component><view>
  <lv_obj style_pad_all="12"/>
</view></component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-pad]"* ]]
}

@test "flags a literal style_margin_* in XML" {
    run_gate margin.xml '<component><view>
  <lv_obj style_margin_top="8"/>
</view></component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-pad]"* ]]
}

@test "flags a width that reproduces a declared token by hand" {
    # 36 is icon_button_size_lg. Matching a declared value is the signal: the
    # author wrote the number a token already spells.
    run_gate size.xml '<component><view>
  <lv_obj width="36" height="36"/>
</view></component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-size]"* ]]
    [[ "$output" == *"icon_button_size_lg"* ]]
}

@test "flags a literal pad in a C++ lv_obj_set_style_pad_* call" {
    run_gate pad.cpp 'void f() {
    lv_obj_set_style_pad_all(row, 12, 0);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[cpp-pad]"* ]]
}

@test "flags a negative margin — space_2xl_neg_* exists, so negatives are tokenizable" {
    run_gate neg.xml '<component><view>
  <lv_obj style_margin_top="-8"/>
</view></component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-pad]"* ]]
}

@test "counts width and height on one element as two sites" {
    run_gate two.xml '<component><view>
  <lv_obj width="36" height="36"/>
</view></component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"xml-size      2"* ]]
}

@test "flags a min-height taller than the MICRO dialog budget" {
    # hidden_network_modal shipped style_min_height="280" on a 272px screen.
    # A floor above the budget can never be satisfied (#1204).
    run_gate tallmin.xml '<component><view>
  <lv_obj style_min_height="280"/>
</view></component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-tall]"* ]]
}

@test "flags a max-height taller than the MICRO dialog budget" {
    # debug_bundle_modal shipped style_max_height="400" and rendered y=-103.
    run_gate tallmax.xml '<component><view>
  <lv_obj style_max_height="400"/>
</view></component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-tall]"* ]]
}

@test "silent on a tall inner cap when the root is a percentage" {
    # job_queue_modal caps its list at 300 but is itself height="90%" -- 244 on
    # a MICRO panel -- so the inner number never binds. Flagging this was a
    # false positive, and four of the seven original findings were this shape.
    run_gate boundedpct.xml '<component>
  <view name="x" extends="ui_dialog" width="85%" height="90%">
    <lv_obj name="list" height="content" style_max_height="300"/>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "silent on a tall inner cap when the root carries its own max_height" {
    run_gate boundedmax.xml '<component>
  <view name="x" extends="ui_dialog" width="70%" height="content" style_max_height="85%">
    <lv_obj name="list" height="content" style_max_height="250"/>
  </view>
</component>'
    [ "$status" -eq 0 ]
}

@test "still flags a tall inner cap when the root sizes to content unbounded" {
    # The debug_bundle_modal / crash_report_modal shape: the inner cap is the
    # only bound, and at 400 it exceeded the whole 272px screen.
    run_gate unbounded.xml '<component>
  <view name="x" extends="ui_dialog" width="85%" height="content">
    <lv_obj name="content_container" height="content" style_max_height="400"/>
  </view>
</component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-tall]"* ]]
}

@test "silent on a min/max height within the MICRO budget" {
    run_gate shortmax.xml '<component><view>
  <lv_obj style_max_height="220" style_min_height="60"/>
</view></component>'
    [ "$status" -eq 0 ]
}

# --------------------------------------------------------------- must ignore

@test "silent on a pixel value quoted inside an XML comment" {
    # The good comments in this tree document a fix by quoting the attribute
    # they replaced. That is documentation, not a violation -- and before the
    # gate blanked comments it counted them as real hits.
    run_gate comment.xml '<component><view>
  <!-- The old style_pad_all="12" and width="36" are gone; see the budget. -->
  <lv_obj style_pad_all="#space_md"/>
</view></component>'
    [ "$status" -eq 0 ]
}


@test 'silent on width="1" — the required LVGL flex idiom' {
    # flex wraps against the declared width before growing. Removing this
    # breaks the layout, so it must stay legal forever.
    run_gate flex.xml '<component><view>
  <lv_obj width="1" flex_grow="1"/>
</view></component>'
    [ "$status" -eq 0 ]
}

@test 'silent on 0 — a reset is not a measurement' {
    run_gate zero.xml '<component><view>
  <lv_obj style_pad_all="0" width="0"/>
</view></component>'
    [ "$status" -eq 0 ]
}

@test "silent on a token reference" {
    run_gate token.xml '<component><view>
  <lv_obj style_pad_all="#space_md" width="#icon_button_size_lg"/>
</view></component>'
    [ "$status" -eq 0 ]
}

@test "silent on percentages and content sizing" {
    run_gate pct.xml '<component><view>
  <lv_obj width="100%" height="content" style_pad_all="0"/>
</view></component>'
    [ "$status" -eq 0 ]
}

@test "silent on min_width/max_width px guards — the dialog idiom" {
    # action_prompt_modal.xml:10 pairs width="70%" with px clamps. The clamp is
    # not the layout, and flagging it would fight the house pattern.
    run_gate guard.xml '<component><view>
  <lv_obj width="70%" style_min_width="320" style_max_width="480"/>
</view></component>'
    [ "$status" -eq 0 ]
}

@test "silent on a width no token declares" {
    # A measured canvas or a one-off has nothing to name it with.
    run_gate odd.xml '<component><view>
  <lv_obj width="9999"/>
</view></component>'
    [ "$status" -eq 0 ]
}

@test "silent on a C++ pad already reading a token" {
    run_gate ok.cpp 'void f() {
    lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_md"), 0);
}'
    [ "$status" -eq 0 ]
}

@test "silent on a 1px hairline divider in C++" {
    run_gate hair.cpp 'void f() {
    lv_obj_set_style_pad_top(divider, 1, 0);
}'
    [ "$status" -eq 0 ]
}

# --------------------------------------------------------------------- opt-out

@test "SIZE_OK on the same line suppresses" {
    run_gate same.cpp 'void f() {
    lv_obj_set_style_pad_all(row, 12, 0); // SIZE_OK: encoder pitch, not a token
}'
    [ "$status" -eq 0 ]
}

@test "SIZE_OK in an XML comment above a wrapped element suppresses" {
    # XML elements wrap across lines, so the annotation window has to reach
    # further back than the 1 line check_imperative_ui.py allows for C++.
    run_gate above.xml '<component><view>
  <!-- SIZE_OK: 470 is the two-column content floor, see the budget above -->
  <lv_obj
      name="thing"
      width="36"/>
</view></component>'
    [ "$status" -eq 0 ]
}

@test "SIZE_OK does not leak past the lookback window" {
    # A gate whose opt-out silently covers the rest of the file is not a gate.
    run_gate far.xml '<component><view>
  <!-- SIZE_OK: applies to the element right below, not the whole file -->
  <lv_obj name="a"/>
  <lv_obj name="b"/>
  <lv_obj name="c"/>
  <lv_obj name="d"/>
  <lv_obj name="e"/>
  <lv_obj name="f"/>
  <lv_obj name="g" style_pad_all="12"/>
</view></component>'
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-pad]"* ]]
}

# --------------------------------------------------------------------- ratchet

@test "--max-allowed at the exact count passes and says so" {
    printf '%s\n' '<component><view>
  <lv_obj style_pad_all="12" style_pad_top="8"/>
</view></component>' > "$FIXTURE_DIR/ratchet.xml"
    run python3 "$GATE" --max-allowed 2 "$FIXTURE_DIR/ratchet.xml"
    [ "$status" -eq 0 ]
    [[ "$output" == *'== baseline'* ]]
}

@test "--max-allowed above the count asks for the baseline to be lowered" {
    printf '%s\n' '<component><view>
  <lv_obj style_pad_all="12"/>
</view></component>' > "$FIXTURE_DIR/below.xml"
    run python3 "$GATE" --max-allowed 5 "$FIXTURE_DIR/below.xml"
    [ "$status" -eq 0 ]
    [[ "$output" == *'ratchet the baseline down'* ]]
}

@test "--max-allowed below the count fails: the ratchet only turns one way" {
    printf '%s\n' '<component><view>
  <lv_obj style_pad_all="12" style_pad_top="8" style_pad_left="4"/>
</view></component>' > "$FIXTURE_DIR/over.xml"
    run python3 "$GATE" --max-allowed 2 "$FIXTURE_DIR/over.xml"
    [ "$status" -eq 1 ]
    [[ "$output" == *'exceeds baseline'* ]]
}

# ------------------------------------------------------------- the real tree

# Current violation count for the committed tree.
tree_count() {
    python3 "$GATE" --summary 2>/dev/null | awk '/^  TOTAL/ { print $2 }'
}

@test "the committed tree is at or under the baseline recorded in quality-checks.sh" {
    # The wiring is the thing that actually gates commits, so pin it rather than
    # a number duplicated here. Converting a site lowers the count; this keeps
    # passing, and the recorded baseline can be ratcheted down at leisure.
    #
    # --staged-only scans the post-commit tree (HEAD when nothing is staged, as
    # in CI), so a dirty local working tree — another session's WIP — cannot
    # false-fail this the way a whole-WT scan would.
    baseline=$(grep -oE 'check_hardcoded_pixels\.py --max-allowed [0-9]+' \
                 scripts/quality-checks.sh | grep -oE '[0-9]+$')
    [ -n "$baseline" ]
    run python3 "$GATE" --staged-only --max-allowed "$baseline" --summary
    [ "$status" -eq 0 ]
}

@test "the gate goes red when one more hardcoded pixel is added to the tree" {
    # The proof that the ratchet works on the REAL tree, not just a fixture:
    # one below the current count must fail. Derived, not hardcoded, so a
    # legitimate conversion cannot silently turn this into a no-op.
    count=$(tree_count)
    [ -n "$count" ] && [ "$count" -gt 0 ]
    run python3 "$GATE" --max-allowed "$(( count - 1 ))" --summary
    [ "$status" -eq 1 ]
    [[ "$output" == *'exceeds baseline'* ]]
}

@test "the dev-only and crash-path files are exempt" {
    # ui_fatal_error.cpp renders before theme init, where a token read returns 0.
    run python3 "$GATE" --list src/ui/ui_fatal_error.cpp
    [ "$status" -eq 0 ]
}

# ----------------------------------------------------- post-commit tree (--staged-only)
#
# --staged-only is NOT "only staged files" — it scans the tree the commit WILL
# create (index applied over HEAD via `git write-tree`). The pre-commit hook
# uses it so another session's unstaged WIP cannot trip the ratchet on a clean
# commit (the failure that forced a stash workaround, violating the never-touch-
# WIP rule). These pin the three load-bearing properties.

GATE_ABS="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/check_hardcoded_pixels.py"

# Build a throwaway git repo with a clean baseline. xml-pad fires on any
# style_pad_* literal >=2, so no globals.xml token table is needed.
setup_tmp_repo() {
    TMP_REPO="$(mktemp -d "${BATS_TEST_TMPDIR:-${BATS_TMPDIR:-/tmp}}/pixels-XXXXXX")"
    cd "$TMP_REPO" || return 1
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test"
    mkdir -p ui_xml
    printf '<component><view>\n  <lv_obj width="#x"/>\n</view></component>\n' > ui_xml/base.xml
    git add -A && git commit -qm base
}

@test "--staged-only counts a violation in a STAGED file" {
    setup_tmp_repo
    printf '<component><view>\n  <lv_obj style_pad_all="12"/>\n</view></component>\n' > ui_xml/base.xml
    git add ui_xml/base.xml
    run python3 "$GATE_ABS" --staged-only --list
    [ "$status" -eq 1 ]
    [[ "$output" == *"[xml-pad]"* ]]
    [[ "$output" == *"ui_xml/base.xml"* ]]
}

@test "--staged-only ignores a violation left as UNSTAGED WIP (the bug)" {
    # The exact shape that forced the stash: another session's dirty file trips
    # the whole-tree scan. The post-commit tree does not contain it, so a clean
    # commit must not see it.
    setup_tmp_repo
    printf '<component><view>\n  <lv_obj style_pad_all="12"/>\n</view></component>\n' > ui_xml/dirty.xml
    run python3 "$GATE_ABS" --staged-only --list
    [ "$status" -eq 0 ]
    [[ "$output" != *"dirty.xml"* ]]
    [[ "$output" != *"[xml-pad]"* ]]
}

@test "--staged-only sees the STAGED version, not unstaged dirt heaped on top" {
    # A file can be staged clean and then gather more WT edits. The commit will
    # ship the staged (clean) blob; the hook must evaluate that one, not the
    # dirty WT copy a plain `read()` would pick up.
    setup_tmp_repo
    printf '<component><view>\n  <lv_obj width="#x"/>\n</view></component>\n' > ui_xml/base.xml
    git add ui_xml/base.xml
    printf '<component><view>\n  <lv_obj style_pad_all="12"/>\n</view></component>\n' > ui_xml/base.xml
    run python3 "$GATE_ABS" --staged-only --list
    [ "$status" -eq 0 ]
    [[ "$output" != *"[xml-pad]"* ]]
}
