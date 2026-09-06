#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_l081_anti_pattern.py — the L081 bg-thread gate.
#
# The gate flags a bare `tok.expired()` check reached on a background thread:
# either followed by a `this`/member access (a real TOCTOU use-after-free, #707)
# or immediately followed by `tok.defer(...)` (dead code, because defer already
# re-checks atomically on the main thread).
#
# What these pin is that it reads CODE, not prose. The script matched inside
# comments and string literals, so a comment describing the anti-pattern — or
# quoting the fix advice the script itself prints — was reported as a violation.
# That is worse than a missed hit: it makes the accurate comment the thing you
# have to delete, so the next person writes a vaguer one. The sibling gate
# check_raw_this_queue_update.py already tested this; this one did not.

GATE="scripts/check_l081_anti_pattern.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/l081_gate"
    mkdir -p "$FIXTURE_DIR"
}

# Write $1 into a fixture .cpp and run the gate over that file alone.
run_gate() {
    printf '%s\n' "$1" > "$FIXTURE_DIR/case.cpp"
    run python3 "$GATE" "$FIXTURE_DIR/case.cpp"
}

# --- the shapes that must still fire ----------------------------------------

@test "flags a bg expired() check followed by defer (redundant guard)" {
    run_gate 'void A::f() {
    auto token = lifetime_.token();
    api_->go([this, token]() {
        if (token.expired())
            return;
        token.defer([this]() { apply(); });
    });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'redundant'* ]]
}

@test "flags a bg expired() check followed by a member access (UAF)" {
    run_gate 'void A::f() {
    auto token = lifetime_.token();
    api_->go([this, token]() {
        if (token.expired())
            return;
        this->apply();
    });
}'
    [ "$status" -eq 1 ]
}

# --- prose must not fire ----------------------------------------------------

@test "a violation quoted in a line comment is not a violation" {
    run_gate 'void A::f() {
    // The bare `if (token.expired()) return;` this replaced was dead code —
    // defer() already re-checks atomically on the main thread.
    api_->go(lifetime_.bg_cb("A::f", [this]() { apply(); }));
}'
    [ "$status" -eq 0 ]
}

@test "a violation quoted in a block comment is not a violation" {
    run_gate 'void A::f() {
    /* Historical note: this used to read
     *     if (token.expired())
     *         return;
     *     token.defer([this]() { apply(); });
     * before the migration to bg_cb.
     */
    api_->go(lifetime_.bg_cb("A::f", [this]() { apply(); }));
}'
    [ "$status" -eq 0 ]
}

@test "a violation inside a string literal is not a violation" {
    run_gate 'void A::f() {
    spdlog::warn("if (token.expired()) return; token.defer([this]() { apply(); });");
}'
    [ "$status" -eq 0 ]
}

# --- the opt-out still works, and it lives in a comment ----------------------

@test "L081_OK on the line suppresses a real hit" {
    run_gate 'void A::f() {
    auto token = lifetime_.token();
    api_->go([this, token]() {
        if (token.expired()) // L081_OK: synchronous wait wrapper
            return;
        token.defer([this]() { apply(); });
    });
}'
    [ "$status" -eq 0 ]
}

# --- the ThumbnailLoadContext spelling of the same race ---------------------
#
# `ctx.is_valid()` calls `lifetime_token->expired()` (thumbnail_load_context.h),
# so it is the same TOCTOU. Matching only `expired()` let five instances build up
# in ui_panel_print_select.cpp, each dereferencing `self` on an HttpExecutor
# worker — found while triaging bundle 6F3QJLFG (#960).

@test "flags a bg ctx.is_valid() check followed by a member access" {
    run_gate 'void A::f() {
    get_thumbnail_cache().fetch_for_card_view(api_, path, ctx,
        [self, ctx](const std::string& p) { use(p); },
        [self, filename_copy, ctx](const std::string& error) {
            if (!ctx.is_valid())
                return;
            spdlog::warn("[{}] failed for {}: {}",
                         self->get_name(), filename_copy, error);
        });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'uaf'* || "$output" == *'member access'* ]] \
        || fail "neither wording appeared: $output"
    # The report must quote the guard the author wrote. Calling this a bare
    # `tok.expired()` reads as a false positive and gets dismissed.
    [[ "$output" == *'ctx.is_valid()'* ]]
}

@test "an is_valid() that is not a lifetime context is not a violation" {
    # ~25 unrelated is_valid() predicates live in this tree (themes, calibration
    # results, gcode index entries, stream sources). Matching on the method name
    # alone would flag every one of them; the receiver name is the discriminator.
    run_gate 'void A::f() {
    auto theme = load();
    if (!theme.is_valid())
        return;
    theme_manager_apply(theme);
    editing_theme_ = theme;
}'
    [ "$status" -eq 0 ]
}

@test "L081_OK on the line suppresses a ctx.is_valid() hit" {
    run_gate 'void A::f() {
    fetch(api_, path, ctx,
        [self, ctx](const std::string& error) {
            if (!ctx.is_valid()) // L081_OK: locals only below, see fetch contract
                return;
            spdlog::warn("[{}] {}", self->get_name(), error);
        });
}'
    [ "$status" -eq 0 ]
}

@test "src/ui is scanned for the ctx.is_valid() spelling" {
    # src/ui/ is excluded from the generic expired() scan on purpose: observer
    # callbacks there run on the main thread, where the check is legitimate. That
    # rationale does not extend to ThumbnailLoadContext, which only ever reaches
    # ThumbnailCache::fetch_*, whose callbacks are always background. Excluding
    # src/ui/ from BOTH spellings is what kept the five real sites unlinted, so
    # pin the narrower scope rather than the directory list as a whole.
    run python3 -c "
import importlib.util, sys
spec = importlib.util.spec_from_file_location('gate', 'scripts/check_l081_anti_pattern.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
assert 'src/ui' in m.CTX_ONLY_SCAN_DIRS, m.CTX_ONLY_SCAN_DIRS
assert 'src/ui' not in m.DEFAULT_SCAN_DIRS, m.DEFAULT_SCAN_DIRS
print('ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *'ok'* ]]
}

# --- the tree itself --------------------------------------------------------

@test "the default scan of the real tree is clean" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}
