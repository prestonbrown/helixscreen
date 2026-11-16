#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/check_hold_ends_press.py: every long_pressed handler ends its press when it
# acts, or says why it never does.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR}/tree"
    mkdir -p "$ROOT/src" "$ROOT/ui_xml"
}

hold_xml() {
    printf '%s\n' "<component><view name=\"row\" extends=\"lv_obj\">" \
        "  <event_cb trigger=\"clicked\" callback=\"on_tap\"/>" \
        "  <event_cb trigger=\"long_pressed\" callback=\"$1\"/>" \
        "</view></component>" > "$ROOT/ui_xml/row.xml"
}

@test "the gate fires on an XML hold handler that never ends its press" {
    hold_xml on_row_hold
    cat > "$ROOT/src/row.cpp" <<'EOF'
void register_row() {
    lv_xml_register_event_cb(nullptr, "on_row_hold", [](lv_event_t* e) { open_menu(); });
}
EOF
    run python3 scripts/check_hold_ends_press.py --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "on_row_hold" "$output"
}

@test "the gate passes a handler that ends the press itself or through a helper" {
    hold_xml on_row_hold
    cat > "$ROOT/src/row.cpp" <<'EOF'
static void hold(lv_event_t* e, int slot) {
    if (open_menu(slot))
        helix::ui::end_press(e);
}
void register_row() {
    lv_xml_register_event_cb(nullptr, "on_row_hold", [](lv_event_t* e) { hold(e, 0); });
}
EOF
    run python3 scripts/check_hold_ends_press.py --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "the gate passes a handler that says why it never ends the press" {
    cat > "$ROOT/src/keys.cpp" <<'EOF'
void Keys::on_hold(lv_event_t* e) {
    // HOLD_DECLINES_OK: the popover reads the release
    show_popover();
}
void Keys::attach(lv_obj_t* kb) {
    lv_obj_add_event_cb(kb, on_hold, LV_EVENT_LONG_PRESSED, nullptr);
}
EOF
    run python3 scripts/check_hold_ends_press.py --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "the gate fires on a C++ hold registration that never ends its press" {
    cat > "$ROOT/src/card.cpp" <<'EOF'
void Cards::on_card_long_pressed(lv_event_t* e) {
    confirm_delete();
}
void Cards::build(lv_obj_t* card) {
    lv_obj_add_event_cb(card, on_card_long_pressed, LV_EVENT_LONG_PRESSED, this);
}
EOF
    run python3 scripts/check_hold_ends_press.py --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "on_card_long_pressed" "$output"
}

@test "the gate fails closed on a hold handler it cannot find" {
    hold_xml on_unregistered_hold
    run python3 scripts/check_hold_ends_press.py --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "cannot find" "$output"
}

@test "the gate resolves a \$prop hold callback through the component's users" {
    printf '%s\n' '<component><api><prop name="reset_callback" type="string"/></api>' \
        '<view name="part" extends="lv_obj"><event_cb trigger="long_pressed" callback="$reset_callback"/></view></component>' \
        > "$ROOT/ui_xml/part.xml"
    printf '%s\n' '<component><view extends="lv_obj"><part reset_callback="on_part_reset"/></view></component>' \
        > "$ROOT/ui_xml/tile.xml"
    cat > "$ROOT/src/tile.cpp" <<'EOF'
void register_tile() {
    lv_xml_register_event_cb(nullptr, "on_part_reset", [](lv_event_t* e) { reset(); });
}
EOF
    run python3 scripts/check_hold_ends_press.py --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "on_part_reset" "$output"
}

@test "the commit hook runs the hold gate" {
    run grep -q 'QC_ALL=.*qc_hold_ends_press' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}
