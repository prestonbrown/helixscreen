#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_lvgl_subject_api.py: the wall keeping every LVGL subject on
# helix::Subject (include/core/subject.h).
#
# The quiet half matters as much as the loud one: lv_subject_t* and lv_subject_t& appear
# at nearly every subject call site, and a gate that counted them would be switched off.

load helpers

GATE="scripts/check_lvgl_subject_api.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src/core" "$ROOT/include" "$ROOT/tests/unit"
}

run_gate() {
    run python3 "$GATE" --root "$ROOT"
}

# The site count the gate's --list reports for the fixture tree.
expect_sites() {
    run python3 "$GATE" --root "$ROOT" --list
    contains "$1 site(s) in 1 file(s)." "$output"
}

# ------------------------------------------------------------- the loud half

@test "catches a deprecated init and deinit call" {
    printf 'void f(lv_subject_t* s) {\n    lv_subject_init_int(s, 0);\n    lv_subject_deinit(s);\n}\n' \
        > "$ROOT/src/x.cpp"
    run_gate
    [ "$status" -eq 1 ] || fail "deprecated calls passed: $output"
    expect_sites 2
}

@test "catches an lv_subject_t member, array and container" {
    cat > "$ROOT/include/x.h" <<'CPP'
struct X {
    lv_subject_t count_;
    lv_subject_t rows_[4];
    lv_subject_t view = {};
    std::array<lv_subject_t, 3> cells_{};
    std::unique_ptr<lv_subject_t> dyn_;
};
CPP
    run_gate
    [ "$status" -eq 1 ] || fail "by-value storage passed: $output"
    expect_sites 5
}

@test "catches heap storage made by hand" {
    printf 'auto a = std::make_unique<lv_subject_t>();\nauto* b = new lv_subject_t{};\n' \
        > "$ROOT/tests/unit/test_x.cpp"
    run_gate
    [ "$status" -eq 1 ] || fail "hand-made heap storage passed: $output"
    expect_sites 2
}

@test "counts each declarator of a comma declaration" {
    printf 'struct X {\n    lv_subject_t a, b;\n    lv_subject_t rows_[2] = {}, c;\n};\n' > "$ROOT/include/x.h"
    run_gate
    [ "$status" -eq 1 ] || fail "comma declarations passed: $output"
    expect_sites 4
}

@test "catches the array form and lv_subject_t in any template argument" {
    cat > "$ROOT/src/x.cpp" <<'CPP'
std::unique_ptr<lv_subject_t[]> rows;
auto more = std::make_unique<lv_subject_t[]>(4);
std::map<std::string, lv_subject_t> by_name;
std::optional<lv_subject_t> maybe;
std::pair<lv_subject_t, int> paired;
CPP
    run_gate
    [ "$status" -eq 1 ] || fail "template-argument storage passed: $output"
    expect_sites 5
}

# ------------------------------------------------------------- the quiet half

@test "pointers, references and helix::Subject are quiet" {
    cat > "$ROOT/src/x.cpp" <<'CPP'
lv_subject_t* get_subject() { return &count_; }
void bind(lv_subject_t* subject, const lv_subject_t& ref, lv_subject_t *spaced);
helix::Subject count_;
std::vector<lv_subject_t*> table;
lv_subject_set_int(&count_, 1);
std::map<std::string, lv_subject_t*> index;
std::pair<int, const lv_subject_t*> found;
void param(lv_subject_t* a, lv_subject_t* b);
CPP
    run_gate
    [ "$status" -eq 0 ] || fail "pointer and reference uses were counted: $output"
}

@test "comments and string literals are not counted" {
    cat > "$ROOT/src/x.cpp" <<'CPP'
// lv_subject_init_int(&s, 0) used to be here; lv_subject_t s;
const char* doc = "lv_subject_deinit(&s); lv_subject_t s;";
CPP
    run_gate
    [ "$status" -eq 0 ] || fail "comment or literal was counted: $output"
}

@test "SUBJECT_API_OK on or above the line opts a site out" {
    cat > "$ROOT/src/x.cpp" <<'CPP'
lv_subject_init_int(&s, 0); // SUBJECT_API_OK: exercises LVGL's own API
// SUBJECT_API_OK: exercises LVGL's own API
lv_subject_t raw;
CPP
    run_gate
    [ "$status" -eq 0 ] || fail "opted-out sites were counted: $output"
}

# ------------------------------------------------------------- the wall

@test "a single site anywhere fails, including the owner's own implementation" {
    printf 'void f(lv_subject_t& s) { lv_subject_init_int(&s, 0); }\n' > "$ROOT/src/core/subject.cpp"
    run_gate
    [ "$status" -eq 1 ] || fail "a site in src/core/subject.cpp passed: $output"
    contains "src/core/subject.cpp:1: call" "$output"
}

@test "a tree with no site passes" {
    printf 'helix::Subject count_;\nlv_subject_set_int(&count_, 1);\n' > "$ROOT/src/x.cpp"
    run_gate
    [ "$status" -eq 0 ] || fail "a clean tree failed: $output"
    contains "OK:" "$output"
}

@test "the gate runs for sources, tests, firmware and its own files" {
    local re
    re="$(qc_trigger lvgl_subject_api)"
    [[ "src/ui/ui_x.cpp" =~ $re ]] || fail "a source file does not trigger the gate"
    [[ "include/x.h" =~ $re ]] || fail "a header does not trigger the gate"
    [[ "tests/unit/test_x.cpp" =~ $re ]] || fail "a test does not trigger the gate"
    [[ "firmware/helixscreen-esp32/main/app.c" =~ $re ]] || fail "firmware does not trigger it"
    [[ "scripts/qc/lvgl_subject_api.sh" =~ $re ]] || fail "its own wrapper does not trigger it"
    [[ ! "docs/devel/THREADING.md" =~ $re ]] || fail "a doc triggers the gate"
}
