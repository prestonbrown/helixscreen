#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_raw_this_queue_update.py — the raw-`this`
# queue_update gate.
#
# The gate exists because a queued lambda runs at the next process_pending()
# tick whether or not the object it captured is still alive. That is #1146:
# MoonrakerAPI::notify_build_volume_changed() queued `[this, …]`,
# ~MoonrakerAPI deinited the subject, and a later unrelated test drained the
# corpse — so the SIGSEGV landed on an innocent test and cost days.
#
# These tests pin both halves of the contract. The positive half is the shape
# that crashes, including the two spellings a `grep '\[this'` cannot see: a
# capture list on its own line, and the default captures `[&]` / `[=]`. The
# negative half is every correct idiom already in the tree — `lifetime_.bg_cb`,
# `tok.defer`, and by-value/init captures. A gate that fires on those is a gate
# that gets switched off, which is how the debt stops shrinking.

GATE="scripts/check_raw_this_queue_update.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/raw_this_qu"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into a fixture .cpp and run the gate over that file alone.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name.cpp"
    run python3 "$GATE" "$FIXTURE_DIR/$name.cpp"
}

# --- the shape that crashes -------------------------------------------------

@test "flags the #1146 shape: a fully-qualified queue_update capturing this" {
    run_gate qualified '
void MoonrakerAPI::notify_build_volume_changed() {
    helix::ui::queue_update([this]() {
        lv_subject_set_int(&build_volume_subject_, 1);
    });
}'
    [ "$status" -eq 1 ]
    contains '[this]' "$output"
    [[ "$output" == *':3:'* ]]
}

@test "flags the ui:: and bare spellings too" {
    run_gate spellings '
void A::f() {
    ui::queue_update([this, tok]() { apply(); });
}
void B::g() {
    queue_update([this, x]() { apply(x); });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'TOTAL'* || "$output" == *'2 site'* ]] \
        || fail "no site count in the summary: $output"
    contains ':3:' "$output"
    [[ "$output" == *':6:'* ]]
}

@test "flags the tagged overload, where the lambda is the second argument" {
    run_gate tagged '
void PrinterState::set_timelapse_available() {
    helix::ui::queue_update("PrinterState::set_timelapse_available", [this]() {
        lv_subject_set_int(&timelapse_available_, 1);
    });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *':3:'* ]]
}

@test "flags the templated overload, where the lambda is the last of three args" {
    run_gate templated '
void ToolState::load() {
    helix::ui::queue_update<int>(std::make_unique<int>(0), [this, api](int*) {
        load_spool_json();
    });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *':3:'* ]]
}

@test "flags a capture list that sits on its own line" {
    # printer_state.cpp:311 and four others are wrapped this way. A
    # line-oriented grep for `queue_update.*\[this` misses every one of them.
    run_gate wrapped '
void PrinterState::on_status_update(const json& notification) {
    helix::ui::queue_update("PrinterState::on_status_update",
                            [this, state_json = params[0]]() {
        update_from_status(state_json);
    });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *':4:'* ]]
}

@test "flags [&] default capture by reference" {
    run_gate amp_default '
void A::f() {
    helix::ui::queue_update([&]() { member_ = 1; });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'[&]'* ]]
}

@test "flags [=] default capture by copy" {
    # [=] captures the `this` POINTER, not the object — the exact same dangle.
    run_gate eq_default '
void A::f() {
    int v = 1;
    helix::ui::queue_update([=]() { member_ = v; });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'[=]'* ]]
}

@test "flags the mixed default-capture forms [&, x] and [=, &x]" {
    run_gate mixed_default '
void A::f(int x) {
    helix::ui::queue_update([&, x]() { member_ = x; });
    helix::ui::queue_update([=, &x]() { member_ = x; });
}'
    [ "$status" -eq 1 ]
    contains '[&]' "$output"
    [[ "$output" == *'[=]'* ]]
}

@test "flags [*this], which snapshots the whole object" {
    run_gate star_this '
void A::f() {
    helix::ui::queue_update([*this]() { use(member_); });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'*this'* ]]
}

@test "counts the outer lambda once, not once per nested re-capture" {
    run_gate nested '
void A::f(Ctx ctx) {
    helix::ui::queue_update([this, ctx]() {
        schedule([this]() { finish(); });
    });
}'
    [ "$status" -eq 1 ]
    run python3 "$GATE" --list "$FIXTURE_DIR/nested.cpp"
    [ "$status" -eq 1 ]
    contains 'TOTAL       1' "$output"
    [[ "$output" == *':3:'* ]]
}

# --- correct idioms that must stay silent -----------------------------------

@test "accepts lifetime_.bg_cb, the canonical fix" {
    run_gate bg_cb '
void SpoolWizard::create_vendor() {
    api_->spoolman().create_vendor(
        name,
        lifetime_.bg_cb("SpoolWizard::create_vendor_ok", [this](const Vendor& v) {
            on_vendor_created(v);
        }),
        lifetime_.bg_cb("SpoolWizard::create_vendor_error", [this](const MoonrakerError& err) {
            show_error(err.message);
        }));
}'
    [ "$status" -eq 0 ]
}

@test "accepts tok.defer and guard_.defer" {
    run_gate defer '
void SpoolWizard::load_vendors(Ctx ctx) {
    tok.defer("SpoolWizard::load_vendors_apply", [this, ctx]() { apply(ctx); });
    guard_.defer("SpoolWizard::refresh", [this]() { refresh(); });
    lifetime_.defer("SpoolWizard::paint", [this]() { paint(); });
}'
    [ "$status" -eq 0 ]
}

@test "accepts a queue_update whose lambda captures nothing at all" {
    run_gate no_capture '
void f() {
    helix::ui::queue_update([]() { lv_subject_set_int(&g_subject, 1); });
}'
    [ "$status" -eq 0 ]
}

@test "accepts by-value captures with no path to this" {
    run_gate by_value '
void f(int progress, std::string name) {
    helix::ui::queue_update([progress, name]() {
        lv_subject_set_int(&g_progress, progress);
    });
}'
    [ "$status" -eq 0 ]
}

@test "accepts a named by-reference capture, which is not a default capture" {
    # `[&x]` binds one local. `[&]` binds everything including this. Conflating
    # the two would flag correct code on sight.
    run_gate named_ref '
void f(Ctx& ctx) {
    helix::ui::queue_update([&ctx]() { ctx.apply(); });
}'
    [ "$status" -eq 0 ]
}

@test "accepts an init-capture, whose = is not a default capture" {
    # `[msg = err.message]` is the shape the fix produces when moving state out
    # of the object. It must not read as `[=]`.
    run_gate init_capture '
void f(const MoonrakerError& err, Cb cb) {
    helix::ui::queue_update([msg = err.message, cb = std::move(cb)]() { cb(msg); });
}'
    [ "$status" -eq 0 ]
}

@test "accepts a subscript argument that is not a lambda introducer" {
    run_gate subscript '
void f(int i, std::unique_ptr<int> d) {
    helix::ui::queue_update(rows_[i], std::move(d), [](int*) { done(); });
}'
    [ "$status" -eq 0 ]
}

@test "a violation inside a comment is not a violation" {
    run_gate commented '
void A::f() {
    // was: helix::ui::queue_update([this]() { apply(); });
    /* also: helix::ui::queue_update([this, x]() { apply(x); }); */
    lifetime_.bg_cb("A::f", [this]() { apply(); });
}'
    [ "$status" -eq 0 ]
}

@test "a violation inside a string literal is not a violation" {
    run_gate stringed '
void A::f() {
    spdlog::debug("use helix::ui::queue_update([this]() {{ }}) here");
}'
    [ "$status" -eq 0 ]
}

@test "ui_queue_update in prose does not match queue_update" {
    # The name appears only in doc comments, but the word-boundary rule that
    # excludes it is load-bearing — an `_` prefix is not a call.
    run_gate ui_prefixed '
// Callers must use ui_queue_update() if touching LVGL objects.
void A::f() {
    lifetime_.bg_cb("A::f", [this]() { apply(); });
}'
    [ "$status" -eq 0 ]
}

# --- opt-out ----------------------------------------------------------------

@test "QUEUE_RAW_THIS_OK on the line suppresses it" {
    run_gate opt_out_inline '
void A::f() {
    helix::ui::queue_update([this]() { apply(); });  // QUEUE_RAW_THIS_OK: process-lifetime singleton
}'
    [ "$status" -eq 0 ]
}

@test "QUEUE_RAW_THIS_OK on a comment line above suppresses it" {
    run_gate opt_out_above '
void A::f() {
    // QUEUE_RAW_THIS_OK: owner outlives the queue, torn down after lv_deinit
    helix::ui::queue_update([this]() { apply(); });
}'
    [ "$status" -eq 0 ]
}

@test "the opt-out suppresses a wrapped capture list from the call line" {
    run_gate opt_out_wrapped '
void A::f() {
    helix::ui::queue_update("A::f",  // QUEUE_RAW_THIS_OK: singleton
                            [this]() { apply(); });
}'
    [ "$status" -eq 0 ]
}

@test "the opt-out is per-lambda, not per-file" {
    run_gate opt_out_partial '
void A::f() {
    helix::ui::queue_update([this]() { apply(); });  // QUEUE_RAW_THIS_OK: deliberate
    helix::ui::queue_update([this]() { apply_again(); });
}'
    [ "$status" -eq 1 ]
    contains ':4:' "$output"
    [[ "$output" != *':3:'* ]]
}

@test "a trailing opt-out does not leak onto the next statement" {
    # The comment belongs to the call it trails. Only a comment on its OWN line
    # covers the line below — same rule as the duplicate-name gate.
    run_gate opt_out_no_leak '
void A::f() {
    int x = 0;  // QUEUE_RAW_THIS_OK: not about the line below
    helix::ui::queue_update([this]() { apply(); });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *':4:'* ]]
}

# --- ratchet ----------------------------------------------------------------

@test "--max-allowed at the exact count passes and says so" {
    printf '%s\n' '
void A::f() {
    helix::ui::queue_update([this]() { apply(); });
    helix::ui::queue_update([this, x]() { apply(x); });
}' > "$FIXTURE_DIR/ratchet.cpp"
    run python3 "$GATE" --max-allowed 2 "$FIXTURE_DIR/ratchet.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *'== baseline'* ]]
}

@test "--max-allowed above the count asks for the baseline to be lowered" {
    printf '%s\n' '
void A::f() {
    helix::ui::queue_update([this]() { apply(); });
}' > "$FIXTURE_DIR/below.cpp"
    run python3 "$GATE" --max-allowed 5 "$FIXTURE_DIR/below.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *'ratchet the baseline down'* ]]
}

@test "--max-allowed below the count fails: the ratchet only turns one way" {
    printf '%s\n' '
void A::f() {
    helix::ui::queue_update([this]() { apply(); });
    helix::ui::queue_update([this, x]() { apply(x); });
    helix::ui::queue_update([this, y]() { apply(y); });
}' > "$FIXTURE_DIR/over.cpp"
    run python3 "$GATE" --max-allowed 2 "$FIXTURE_DIR/over.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *'exceeds baseline'* ]]
}

@test "--summary reports a per-capture-form breakdown" {
    printf '%s\n' '
void A::f() {
    helix::ui::queue_update([this]() { apply(); });
    helix::ui::queue_update([&]() { apply(); });
}' > "$FIXTURE_DIR/summary.cpp"
    run python3 "$GATE" --max-allowed 2 --summary "$FIXTURE_DIR/summary.cpp"
    [ "$status" -eq 0 ]
    contains 'TOTAL       2' "$output"
    [[ "$output" == *'[&]'* ]]
}

# --- the real tree ----------------------------------------------------------

@test "catches all nine sites in ui_spool_wizard.cpp as it stood before d54a943e5" {
    # The strongest test in the file: a real file, a real fix, a known count.
    # Synthetic fixtures only prove the detector matches what its author
    # imagined; this proves it matches what actually shipped broken.
    #
    # The pre-fix source is vendored rather than read back with
    # `git show d54a943e5^:src/ui/ui_spool_wizard.cpp`: CI checks out at
    # fetch-depth 1, so that object does not exist on the runner and the test
    # failed on the fetch, never reaching the detector. Vendoring also keeps it
    # working in worktrees, archive exports, and any shallow clone.
    #
    # Stored .cpp.txt and copied to .cpp here on purpose. The gate filters
    # explicit paths by extension, so scanning the fixture in place would report
    # zero sites and exit 0 — a silent pass. The .txt suffix also keeps a
    # deliberately-broken file out of clang-format and the src/ tree scans.
    cp "$BATS_TEST_DIRNAME/fixtures/ui_spool_wizard_pre_d54a943e5.cpp.txt" \
       "$FIXTURE_DIR/spool_wizard_pre.cpp"
    run python3 "$GATE" --list "$FIXTURE_DIR/spool_wizard_pre.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *'TOTAL       9'* ]]
}

@test "and none in ui_spool_wizard.cpp after the fix converted them to bg_cb/defer" {
    run python3 "$GATE" src/ui/ui_spool_wizard.cpp
    [ "$status" -eq 0 ]
    [[ "$output" == *'✅'* ]]
}
