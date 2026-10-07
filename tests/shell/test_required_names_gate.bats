#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_required_names.py: every literal passed to
# find_required() must exist in the XML its file creates, in every layout
# variant chain. The misses are what matter: a name missing only from a
# portrait override, or only from a nested component, must still be reported.

load helpers

GATE="scripts/check_required_names.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src" "$ROOT/include" "$ROOT/ui_xml/components" "$ROOT/ui_xml/portrait" \
        "$ROOT/ui_xml/micro_portrait"
    # The variant chains come from the app's own LayoutManager.
    cp src/layout_manager.cpp "$ROOT/src/layout_manager.cpp"
    cat > "$ROOT/src/demo.cpp" <<'CPP'
lv_obj_t* Demo::create(lv_obj_t* p) { return create_overlay_from_xml(p, "demo_overlay"); }
void Demo::before_show() {
    lv_obj_t* row = find_required(overlay_root_, "row_volume", get_name());
    find_required(row, "slider", get_name());
}
CPP
    cat > "$ROOT/ui_xml/components/demo_row.xml" <<'XML'
<component><view><lv_slider name="slider"/></view></component>
XML
    cat > "$ROOT/ui_xml/demo_overlay.xml" <<'XML'
<component><view><demo_row name="row_volume"/></view></component>
XML
}

@test "names present directly and through a nested component pass" {
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 0 ]
}

@test "a name missing from one variant override is reported with that layout" {
    echo '<component><view><demo_row name="row_volume_p"/></view></component>' \
        > "$ROOT/ui_xml/portrait/demo_overlay.xml"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "'row_volume'" "$output"
    contains "portrait" "$output"
}

@test "a nested component's variant is resolved through the fallback chain" {
    # micro_portrait falls back to portrait/ before base; an override keeps the
    # base file's relative path.
    mkdir -p "$ROOT/ui_xml/portrait/components"
    echo '<component><view><lv_bar name="bar"/></view></component>' \
        > "$ROOT/ui_xml/portrait/components/demo_row.xml"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "'slider'" "$output"
    contains "micro_portrait" "$output"
}

@test "an xml_component() override in the matching header counts as created" {
    cat > "$ROOT/src/demo.cpp" <<'CPP'
void Demo::before_show() { find_required(overlay_root_, "row_volume", get_name()); }
CPP
    cat > "$ROOT/include/demo.h" <<'H'
class Demo : public OverlayBase {
    const char* xml_component() const override { return "demo_overlay"; }
};
H
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 0 ]
    sed -i 's/row_volume/row_missing/' "$ROOT/src/demo.cpp"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "'row_missing'" "$output"
}

@test "a modal's component_name() override counts as created" {
    cat > "$ROOT/src/demo.cpp" <<'CPP'
void Demo::on_show() { find_required(dialog(), "row_volume", get_name()); }
CPP
    cat > "$ROOT/include/demo.h" <<'H'
class Demo : public Modal {
    const char* component_name() const override {
        return "demo_overlay";
    }
};
H
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 0 ]
    sed -i 's/row_volume/row_missing/' "$ROOT/src/demo.cpp"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "'row_missing'" "$output"
}

@test "an annotated family is checked in every member" {
    cat > "$ROOT/src/demo.cpp" <<'CPP'
void fill(lv_obj_t* row) {
    // required-names: demo_row other_row
    find_required(row, "slider", "Demo");
}
CPP
    echo '<component><view><lv_obj name="slider"/></view></component>' \
        > "$ROOT/ui_xml/components/other_row.xml"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 0 ]
    echo '<component><view><lv_obj name="knob"/></view></component>' \
        > "$ROOT/ui_xml/components/other_row.xml"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "other_row" "$output"
}

@test "a lookup in a file that creates no component must be annotated" {
    cat > "$ROOT/src/demo.cpp" <<'CPP'
void fill(lv_obj_t* row) { find_required(row, "slider", "Demo"); }
CPP
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "required-names" "$output"
}

@test "find_optional and non-literal names are left alone" {
    cat > "$ROOT/src/demo.cpp" <<'CPP'
lv_obj_t* Demo::create(lv_obj_t* p) { return create_overlay_from_xml(p, "demo_overlay"); }
void f(const char* n) {
    find_optional(overlay_root_, "plugin_supplied");
    find_required(overlay_root_, n, "Demo");
}
CPP
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 0 ]
}

@test "the variant chains are read from LayoutManager, and a missing one fails closed" {
    # A chain the app adds is checked without touching the gate.
    sed -i 's/return {"tiny"};/return {"tiny", "portrait"};/' "$ROOT/src/layout_manager.cpp"
    echo '<component><view><demo_row name="row_volume_p"/></view></component>' \
        > "$ROOT/ui_xml/portrait/demo_overlay.xml"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains ", tiny)" "$output"

    rm "$ROOT/src/layout_manager.cpp"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -ne 0 ]
    contains "variant_chain" "$output"
}

@test "this tree holds its baseline" {
    run python3 "$GATE" --baseline scripts/required_names_baseline.txt
    [ "$status" -eq 0 ]
}
