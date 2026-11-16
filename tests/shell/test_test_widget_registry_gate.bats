#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_test_widget_registry.py: test code must not
# serve an XML widget name production registers, with a create or apply
# callback defined under tests/. Every <name> a test builds would then be the
# test copy. Registering a production name with the SHIPPED callbacks is
# legitimate (the XML attribute validator tests do it), so that stays quiet.

GATE="scripts/check_test_widget_registry.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    load helpers
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    rm -rf "$WORK"
    mkdir -p "$WORK/src" "$WORK/tests/unit"
    cat > "$WORK/src/ui_widget.cpp" <<'EOF'
void* ui_widget_create(void* state, const char** attrs) { return nullptr; }
void ui_widget_apply(void* state, const char** attrs) {}
void register_widget() {
    lv_xml_register_widget("my_widget", ui_widget_create, ui_widget_apply);
}
EOF
}

gate() {
    run python3 "$REPO_ROOT/$GATE" --root "$WORK"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "flags a production name served by a create callback defined in tests" {
    cat > "$WORK/tests/unit/test_copy.cpp" <<'EOF'
static void* fake_create(void* state, const char** attrs) {
    return nullptr;
}
void setup() {
    lv_xml_register_widget("my_widget", fake_create, ui_widget_apply);
}
EOF
    gate
    [ "$status" -eq 1 ]
    contains "tests/unit/test_copy.cpp:5: <my_widget>" "$output"
    contains "fake_create is defined under tests/" "$output"
}

@test "flags a hijacked apply callback whose signature ends the line" {
    cat > "$WORK/tests/unit/test_copy.cpp" <<'EOF'
static void fake_apply(void* state, const char** attrs)
{
}
void setup() { lv_xml_register_widget("my_widget", ui_widget_create, fake_apply); }
EOF
    gate
    [ "$status" -eq 1 ]
    contains "fake_apply" "$output"
}

# ------------------------------------------------ idioms that must stay QUIET

@test "registering a production name with the shipped callbacks passes" {
    cat > "$WORK/tests/unit/test_validator.cpp" <<'EOF'
void setup() {
    lv_xml_register_widget("my_widget", ui_widget_create, ui_widget_apply);
}
EOF
    gate
    [ "$status" -eq 0 ]
}

@test "a test-only widget name collides with nothing and passes" {
    cat > "$WORK/tests/unit/test_own.cpp" <<'EOF'
static void* probe_create(void* s, const char** a) { return nullptr; }
void setup() { lv_xml_register_widget("test_probe", probe_create, probe_create); }
EOF
    gate
    [ "$status" -eq 0 ]
}

@test "a call site of the test callback is not mistaken for its definition" {
    cat > "$WORK/tests/unit/test_calls.cpp" <<'EOF'
void setup() {
    lv_xml_register_widget("my_widget", helper_create, ui_widget_apply);
    helper_create(nullptr, nullptr);
}
EOF
    gate
    [ "$status" -eq 0 ]
}

@test "the opt-out within two lines of the registration passes" {
    cat > "$WORK/tests/unit/test_copy.cpp" <<'EOF'
static void* fake_create(void* state, const char** attrs) { return nullptr; }
void setup() {
    // TEST_WIDGET_OK: the shipped object cannot link here
    lv_xml_register_widget("my_widget", fake_create, ui_widget_apply);
}
EOF
    gate
    [ "$status" -eq 0 ]
}

@test "quality-checks runs the gate on src and test changes" {
    grep -q 'python3 scripts/check_test_widget_registry.py' scripts/qc/test_widget_registry.sh
    trig="$(qc_trigger test_widget_registry)"
    [[ "tests/unit/test_x.cpp" =~ $trig ]] || fail "trigger misses tests/: $trig"
    [[ "src/ui/ui_x.cpp" =~ $trig ]] || fail "trigger misses src/: $trig"
}
