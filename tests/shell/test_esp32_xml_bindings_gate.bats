#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_esp32_xml_bindings.py: a name XML binds must be
# registered by a file the ESP32 firmware compiles, in a branch it compiles.

load helpers

GATE="scripts/check_esp32_xml_bindings.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    FW="$ROOT/firmware/helixscreen-esp32/components/helixapp"
    mkdir -p "$ROOT/src" "$ROOT/include" "$ROOT/ui_xml" "$FW"
    cat > "$FW/CMakeLists.txt" <<'CMAKE'
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HELIX_HAS_PLUGINS=0)
CMAKE
    echo "src/kept.cpp" > "$FW/app_srcs.txt"
    cat > "$ROOT/ui_xml/demo.xml" <<'XML'
<component><view>
  <x callback="on_row"><bind_flag_if_eq subject="row_available" flag="hidden" ref_value="0"/></x>
</view></component>
XML
}

run_gate() {
    run python3 "$GATE" --repo-root "$ROOT" --list
}

@test "a callback and subject registered only by an excluded file are reported" {
    cat > "$ROOT/src/excluded.cpp" <<'CPP'
void reg() {
    lv_xml_register_event_cb(nullptr, "on_row", on_row);
    lv_xml_register_subject(nullptr, "row_available", &row_available_);
}
CPP
    run_gate
    contains "callback:on_row" "$output"
    contains "subject:row_available" "$output"
}

@test "registrations in a listed file or a firmware stub stay silent" {
    cat > "$ROOT/src/kept.cpp" <<'CPP'
void reg() { lv_xml_register_event_cb(nullptr, "on_row", on_row); }
CPP
    cat > "$FW/stub.cpp" <<'CPP'
void reg() { lv_xml_register_subject(nullptr, "row_available", &row_available_); }
CPP
    run_gate
    [ "$status" -eq 0 ]
    lacks "on_row" "$output"
    lacks "row_available" "$output"
}

@test "a registration in a branch the firmware compiles out is reported" {
    cat > "$ROOT/src/kept.cpp" <<'CPP'
void reg() {
#if HELIX_HAS_PLUGINS
    lv_xml_register_event_cb(nullptr, "on_row", on_row);
#endif
    lv_xml_register_subject(nullptr, "row_available", &row_available_);
}
CPP
    run_gate
    contains "callback:on_row" "$output"
    lacks "row_available" "$output"
    sed -i 's/#endif/#else\n    lv_xml_register_event_cb(nullptr, "on_row", on_noop);\n#endif/' "$ROOT/src/kept.cpp"
    run_gate
    lacks "on_row" "$output"
}

@test "a name nothing registers anywhere is not this gate's finding" {
    run_gate
    lacks "on_row" "$output"
    lacks "row_available" "$output"
}

@test "the baseline fails a new finding and passes accepted debt" {
    cat > "$ROOT/src/excluded.cpp" <<'CPP'
void reg() { lv_xml_register_event_cb(nullptr, "on_row", on_row); }
CPP
    echo "callback:on_row" > "$ROOT/baseline.txt"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 0 ]
    echo 'void more() { lv_xml_register_subject(nullptr, "row_available", &s); }' >> "$ROOT/src/excluded.cpp"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 1 ]
    contains "subject:row_available" "$output"
}

@test "this tree holds its baseline" {
    run python3 "$GATE" --baseline scripts/esp32_xml_binding_baseline.txt
    [ "$status" -eq 0 ]
}

# Runs qc_esp32_bindings_touched against what is staged in $1.
bindings_touched() {
    local qc="$PWD/scripts/qc/esp32_xml_bindings.sh"
    (cd "$1" && STAGED_ONLY=true QC_STAGED_ALL="$(git diff --cached --name-only)" \
        bash -c '. "$0" && qc_esp32_bindings_touched' "$qc")
}

staging_repo() {
    REPO="$BATS_TEST_TMPDIR/repo"
    rm -rf "$REPO"
    mkdir -p "$REPO/src" "$REPO/ui_xml"
    git -C "$REPO" init -q
    printf 'int a;\nvoid reg() { lv_xml_register_event_cb(nullptr, "on_x", cb); }\n' > "$REPO/src/a.cpp"
    git -C "$REPO" add -A && git -C "$REPO" -c user.email=t@t -c user.name=t commit -qm init
}

@test "the hook runs the gate, and skips it for a src change that moves no binding" {
    run grep -q 'QC_ALL=.*qc_esp32_xml_bindings' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
    staging_repo
    sed -i 's/int a;/int a; \/\/ comment/' "$REPO/src/a.cpp" && git -C "$REPO" add src/a.cpp
    refute bindings_touched "$REPO"
}

@test "a staged registration, #if or XML change runs the gate" {
    staging_repo
    sed -i 's/"on_x"/"on_y"/' "$REPO/src/a.cpp" && git -C "$REPO" add src/a.cpp
    bindings_touched "$REPO"
    staging_repo
    printf '#if HELIX_HAS_PLUGINS\n#endif\n' >> "$REPO/src/a.cpp" && git -C "$REPO" add src/a.cpp
    bindings_touched "$REPO"
    staging_repo
    echo '<component/>' > "$REPO/ui_xml/x.xml" && git -C "$REPO" add ui_xml/x.xml
    bindings_touched "$REPO"
}

# --- a widget the firmware compiles out ---

disabled_widget_tree() {
    mkdir -p "$ROOT/firmware/helixscreen-esp32/components/helixcore" "$ROOT/lib/helix-xml/src/xml"
    cat > "$ROOT/firmware/helixscreen-esp32/components/helixcore/lv_conf.h" <<'CONF'
#define LV_USE_XML 1
#define LV_USE_SCALE 0
#define LV_USE_SCALE_SECTION 1
#define LV_USE_ROLLER 1
CONF
    cat > "$ROOT/lib/helix-xml/src/xml/lv_xml.c" <<'C'
#if LV_USE_XML
#if LV_USE_SCALE
    lv_xml_register_widget("lv_scale", create, apply);
#if LV_USE_SCALE_SECTION
    lv_xml_register_widget("lv_scale-section", create, apply);
#endif
#endif
#if LV_USE_ROLLER
    lv_xml_register_widget("lv_roller", create, apply);
#endif
#endif
C
}

@test "firmware XML using a widget lv_conf.h compiles out fails, naming tag and option" {
    disabled_widget_tree
    echo '<component><view><lv_scale-section/></view></component>' > "$ROOT/ui_xml/gauge.xml"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "gauge.xml: <lv_scale-section> needs LV_USE_SCALE" "$output"
}

@test "a firmware override using a compiled-out widget fails" {
    disabled_widget_tree
    mkdir -p "$ROOT/firmware/helixscreen-esp32/ui_xml_overrides"
    echo '<component><view><lv_scale/></view></component>' > "$ROOT/firmware/helixscreen-esp32/ui_xml_overrides/o.xml"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "o.xml: <lv_scale>" "$output"
}

@test "an enabled widget, an XML file the firmware does not ship, and an overridden file stay silent" {
    disabled_widget_tree
    mkdir -p "$ROOT/ui_xml/micro" "$ROOT/firmware/helixscreen-esp32/ui_xml_overrides"
    echo '<component><view><lv_roller/></view></component>' > "$ROOT/ui_xml/ok.xml"
    echo '<component><view><lv_scale/></view></component>' > "$ROOT/ui_xml/micro/small.xml"
    echo '<component><view><lv_scale/></view></component>' > "$ROOT/ui_xml/swapped.xml"
    echo '<component><view/></component>' > "$ROOT/firmware/helixscreen-esp32/ui_xml_overrides/swapped.xml"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 0 ]
}

@test "a registration table the gate cannot read fails rather than passing" {
    disabled_widget_tree
    echo '/* no registrations */' > "$ROOT/lib/helix-xml/src/xml/lv_xml.c"
    run python3 "$GATE" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    contains "cannot judge" "$output"
}
