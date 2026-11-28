#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the xml-create-registered rule of scripts/check_source_rules.py: every
# component name src/ passes to lv_xml_create must be registrable, or the call returns NULL.

load helpers

GATE="scripts/check_source_rules.py --rule xml-create-registered"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src/ui"
    cat > "$ROOT/src/ui/panel.cpp" <<'EOF'
void build(lv_obj_t* parent, const char** attrs) {
    lv_xml_create(parent,
                  "demo_row", attrs);
    lv_xml_create(parent, "lv_obj", nullptr);
}
EOF
}

@test "an unregistered lv_xml_create name fails the gate" {
    run python3 $GATE --root "$ROOT"
    [ "$status" -eq 1 ]
    contains "demo_row" "$output"
    contains "src/ui/panel.cpp:3" "$output"
}

@test "a component file at the top of ui_xml passes the gate" {
    mkdir -p "$ROOT/ui_xml"
    echo '<component/>' > "$ROOT/ui_xml/demo_row.xml"
    run python3 $GATE --root "$ROOT"
    [ "$status" -eq 0 ]
}

@test "a component file under ui_xml/components passes the gate" {
    mkdir -p "$ROOT/ui_xml/components"
    echo '<component/>' > "$ROOT/ui_xml/components/demo_row.xml"
    run python3 $GATE --root "$ROOT"
    [ "$status" -eq 0 ]
}

@test "a component file in a layout variant directory alone does not pass" {
    mkdir -p "$ROOT/ui_xml/portrait"
    echo '<component/>' > "$ROOT/ui_xml/portrait/demo_row.xml"
    run python3 $GATE --root "$ROOT"
    [ "$status" -eq 1 ]
}

@test "registering the component file by path passes the gate" {
    echo 'void reg() { lv_xml_register_component_from_file("A:extra/demo_row.xml"); }' \
        > "$ROOT/src/demo_reg.cpp"
    run python3 $GATE --root "$ROOT"
    [ "$status" -eq 0 ]
}

@test "registering a widget of that name passes the gate" {
    echo 'void reg() { lv_xml_register_widget("demo_row", create_cb, apply_cb); }' \
        > "$ROOT/src/demo_widget.cpp"
    run python3 $GATE --root "$ROOT"
    [ "$status" -eq 0 ]
}
