#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_timer_destructor_cancel.py: a class that arms a
# raw lv_timer_t* member and cancels it somewhere must reach that cancel from
# its destructor, directly or through the methods it calls. Teardown that skips
# cleanup()/stop() otherwise leaves an armed timer holding a freed `this`.
# The transitive cases and the comment that only NAMES a cancelling method are
# the ones a looser or stricter gate would get wrong.

GATE="scripts/check_timer_destructor_cancel.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers
    FIX="${BATS_TEST_TMPDIR:-$(mktemp -d)}/timer_dtor"
    mkdir -p "$FIX"
}

run_gate() {
    printf '%s\n' "$2" > "$FIX/$1.cpp"
    run python3 "$GATE" --list "$FIX/$1.cpp"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "flags a timer cancelled only in cleanup()" {
    run_gate stop_only '
void Foo::start() { poll_timer_ = lv_timer_create(cb, 100, this); }
void Foo::cleanup() { lv_timer_delete(poll_timer_); poll_timer_ = nullptr; }
Foo::~Foo() {
    spdlog::debug("bye");
}'
    [ "$status" -eq 1 ]
    contains "Foo::poll_timer_" "$output"
    contains "not reachable from ~Foo()" "$output"
}

@test "flags a class that cancels its timer but defines no destructor here" {
    run_gate no_dtor '
void Bar::arm() { tick_timer_ = lv_timer_create(cb, 50, this); }
void Bar::stop() { helix::ui::lv_timer_cancel_safe(tick_timer_); }'
    [ "$status" -eq 1 ]
    contains "defines no destructor in this TU" "$output"
}

@test "an m_ prefixed member counts too" {
    run_gate m_prefix '
void Saver::start() { m_tick_timer = lv_timer_create(cb, 16, this); }
void Saver::stop() { lv_timer_delete(m_tick_timer); }
Saver::~Saver() {}'
    [ "$status" -eq 1 ]
    contains "Saver::m_tick_timer" "$output"
}

@test "a destructor comment naming cleanup() is not a call to it" {
    run_gate comment_only '
void Foo::start() { poll_timer_ = lv_timer_create(cb, 100, this); }
void Foo::cleanup() { lv_timer_delete(poll_timer_); }
Foo::~Foo() {
    // cleanup() cancels poll_timer_
}'
    [ "$status" -eq 1 ]
}

# ------------------------------------------------ idioms that must stay QUIET

@test "a destructor that cancels directly passes" {
    run_gate direct '
void Foo::start() { poll_timer_ = lv_timer_create(cb, 100, this); }
void Foo::stop() { lv_timer_delete(poll_timer_); }
Foo::~Foo() { helix::ui::lv_timer_cancel_safe(poll_timer_); }'
    [ "$status" -eq 0 ]
}

@test "a cancel reached through two helper calls passes" {
    run_gate transitive '
void Foo::start() { poll_timer_ = lv_timer_create(cb, 100, this); }
void Foo::cancel_poll_timer() { lv_timer_delete(this->poll_timer_); }
void Foo::deinit_subjects() { cancel_poll_timer(); }
Foo::~Foo() { deinit_subjects(); }'
    [ "$status" -eq 0 ]
}

@test "a timer never cancelled anywhere is outside this gate" {
    run_gate never '
void Foo::start() { poll_timer_ = lv_timer_create(cb, 100, this); }
Foo::~Foo() {}'
    [ "$status" -eq 0 ]
}

@test "a local lv_timer_t is not a member" {
    run_gate local '
void Foo::flash() {
    lv_timer_t* once_ = lv_timer_create(cb, 10, this);
    lv_timer_delete(once_);
}
Foo::~Foo() {}'
    [ "$status" -eq 0 ]
}

@test "the opt-out in the destructor passes" {
    run_gate opt_out '
void Foo::start() { poll_timer_ = lv_timer_create(cb, 100, this); }
void Foo::cleanup() { lv_timer_delete(poll_timer_); }
Foo::~Foo() {
    // TIMER_DTOR_OK: the owner deletes this timer before destroying Foo
}'
    [ "$status" -eq 0 ]
}

@test "--max-allowed ratchets the count" {
    printf '%s\n' '
void Foo::start() { poll_timer_ = lv_timer_create(cb, 100, this); }
void Foo::cleanup() { lv_timer_delete(poll_timer_); }
Foo::~Foo() {}' > "$FIX/one.cpp"
    run python3 "$GATE" --max-allowed 1 "$FIX/one.cpp"
    [ "$status" -eq 0 ]
    contains "1 == baseline (1)" "$output"
}

@test "quality-checks runs the gate from qc_decl_ui" {
    grep -q 'python3 scripts/check_timer_destructor_cancel.py --max-allowed 0' scripts/qc/decl_ui.sh
}
