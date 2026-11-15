#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_lvgl_subject_api.py: the ratchet moving every LVGL subject
# onto helix::Subject (include/core/subject.h).
#
# The quiet half matters as much as the loud one: lv_subject_t* and lv_subject_t& appear
# at nearly every subject call site, and a gate that counted them would be switched off.

load helpers

GATE="scripts/check_lvgl_subject_api.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src/core" "$ROOT/include" "$ROOT/tests/unit"
    : > "$ROOT/base.txt"
}

run_gate() {
    run python3 "$GATE" --root "$ROOT" --baseline "$ROOT/base.txt"
}

# ------------------------------------------------------------- the loud half

@test "catches a deprecated init and deinit call" {
    printf 'void f(lv_subject_t* s) {\n    lv_subject_init_int(s, 0);\n    lv_subject_deinit(s);\n}\n' \
        > "$ROOT/src/x.cpp"
    run_gate
    [ "$status" -eq 1 ] || fail "deprecated calls passed: $output"
    contains "src/x.cpp: 2 site(s)" "$output"
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
    contains "include/x.h: 5 site(s)" "$output"
}

@test "catches heap storage made by hand" {
    printf 'auto a = std::make_unique<lv_subject_t>();\nauto* b = new lv_subject_t{};\n' \
        > "$ROOT/tests/unit/test_x.cpp"
    run_gate
    [ "$status" -eq 1 ] || fail "hand-made heap storage passed: $output"
    contains "tests/unit/test_x.cpp: 2 site(s)" "$output"
}

# ------------------------------------------------------------- the quiet half

@test "pointers, references and helix::Subject are quiet" {
    cat > "$ROOT/src/x.cpp" <<'CPP'
lv_subject_t* get_subject() { return &count_; }
void bind(lv_subject_t* subject, const lv_subject_t& ref, lv_subject_t *spaced);
helix::Subject count_;
std::vector<lv_subject_t*> table;
lv_subject_set_int(&count_, 1);
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

@test "the owner's own implementation is exempt" {
    printf 'void f(lv_subject_t& s) { lv_subject_init_int(&s, 0); }\n' > "$ROOT/src/core/subject.cpp"
    run_gate
    [ "$status" -eq 0 ] || fail "src/core/subject.cpp was counted: $output"
}

# ------------------------------------------------------------- the ratchet

@test "fails above a nonzero baseline, passes at it, and fails below it only at a push" {
    printf 'lv_subject_t a;\nlv_subject_t b;\n' > "$ROOT/src/x.cpp"
    printf '1 src/x.cpp\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 1 ] || fail "two sites over a baseline of one passed"
    printf '2 src/x.cpp\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 0 ] || fail "two sites at a baseline of two failed: $output"
    printf '3 src/x.cpp\n' > "$ROOT/base.txt"
    HELIX_RATCHET_STRICT=0 run_gate
    [ "$status" -eq 0 ] || fail "below baseline failed at commit: $output"
    contains "lowered" "$output"
    HELIX_RATCHET_STRICT=1 run_gate
    [ "$status" -eq 1 ] || fail "below baseline passed at a push: $output"
    contains "src/x.cpp: 3 -> 2" "$output"
}

@test "the gate runs for sources, tests, firmware and its own files" {
    local re
    re="$(qc_trigger lvgl_subject_api)"
    [[ "src/ui/ui_x.cpp" =~ $re ]] || fail "a source file does not trigger the gate"
    [[ "include/x.h" =~ $re ]] || fail "a header does not trigger the gate"
    [[ "tests/unit/test_x.cpp" =~ $re ]] || fail "a test does not trigger the gate"
    [[ "firmware/helixscreen-esp32/main/app.c" =~ $re ]] || fail "firmware does not trigger it"
    [[ "scripts/lvgl_subject_api_baseline.txt" =~ $re ]] || fail "the baseline does not trigger it"
    [[ ! "docs/devel/THREADING.md" =~ $re ]] || fail "a doc triggers the gate"
}
