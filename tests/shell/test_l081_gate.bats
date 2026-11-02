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

@test "src/ui is in the default scan" {
    run python3 -c "
import importlib.util, sys
sys.path.insert(0, 'scripts')  # the gate imports staged_content as a sibling
spec = importlib.util.spec_from_file_location('gate', 'scripts/check_l081_anti_pattern.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
assert 'src/ui' in m.DEFAULT_SCAN_DIRS, m.DEFAULT_SCAN_DIRS
print('ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *'ok'* ]]
}

# --- thread context ---------------------------------------------------------

@test "an observer callback (Owner* self) is main-thread and not a violation" {
    run_gate 'void W::attach() {
    auto token = lifetime_.token();
    obs_ = helix::ui::observe<int>(subj, this, [token](W* self, int v) {
        if (token.expired())
            return;
        self->apply(v);
    }, lifetime);
}'
    [ "$status" -eq 0 ]
}

@test "a queue_update body is main-thread and not a violation" {
    run_gate 'void W::f() {
    helix::ui::queue_update("W::f", [this, token]() {
        if (token.expired())
            return;
        member_ = 1;
    });
}'
    [ "$status" -eq 0 ]
}

@test "a modal button callback is main-thread and not a violation" {
    run_gate 'void W::f() {
    runout_modal_.set_on_purge([this, token]() {
        if (token.expired())
            return;
        user_took_manual_action_ = true;
    });
}'
    [ "$status" -eq 0 ]
}

@test "an API callback is background even when it calls a main-thread helper later" {
    run_gate 'void W::f() {
    api_->timelapse().get_settings(
        [this, tok](const Settings&) {
            if (tok.expired() || !wizard_active_)
                return;
            tok.defer([this]() { done(); });
        });
}'
    [ "$status" -eq 1 ]
}

# --- block shapes -----------------------------------------------------------

@test "a braced { return; } guard followed by a member access is a violation" {
    run_gate 'void W::f() {
    api_->go([this, tok]() {
        if (tok.expired()) {
            return;
        }
        scan_in_flight_ = false;
    });
}'
    [ "$status" -eq 1 ]
    [[ "$output" != *'redundant'* ]]
}

@test "a loop between the guard and the defer does not end the scan" {
    run_gate 'void W::f() {
    api->get_devices([this, token, wanted](const std::vector<Dev>& devices) {
        if (token.expired())
            return;
        bool any_on = false;
        for (const auto& dev : devices) {
            if (wanted.count(dev.name) > 0) {
                any_on = true;
            }
        }
        token.defer("W::f", [this, any_on]() { show(any_on); });
    });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'redundant'* ]]
}

# --- the tree itself --------------------------------------------------------

@test "the default scan of the real tree is clean" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------- --staged-only content
#
# --staged-only picks its file SET from `git diff --cached`, but must read
# each file's STAGED content (the index blob), not the working tree. Stage a
# violation and then revert the working file to something clean, and a gate
# that reads the working tree reports nothing while the bad blob commits.

GATE_ABS="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/check_l081_anti_pattern.py"

setup_tmp_repo() {
    TMP_REPO="$(mktemp -d "${BATS_TEST_TMPDIR:-${BATS_TMPDIR:-/tmp}}/l081-XXXXXX")"
    cd "$TMP_REPO" || return 1
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test"
    mkdir -p src/printer
    printf 'void f() {\n    if (tok.expired()) return;\n}\n' > src/printer/foo.cpp
    git add -A && git commit -qm base
}

@test "--staged-only catches a violation in the STAGED blob, not a reverted working file" {
    setup_tmp_repo
    printf 'void f() {\n    if (tok.expired()) {\n        this->value_ = 1;\n    }\n}\n' \
        > src/printer/foo.cpp
    git add src/printer/foo.cpp
    printf 'void f() {\n    if (tok.expired()) return;\n}\n' > src/printer/foo.cpp
    run python3 "$GATE_ABS" --staged-only
    [ "$status" -eq 1 ]
    contains "Mechanism C" "$output"
}

@test "--staged-only stays silent on a clean staged file with a dirty violation on disk" {
    setup_tmp_repo
    # Genuinely different from the base commit (an added clean function), so
    # the file shows up as staged at all - content byte-identical to HEAD
    # stages nothing for git to report, which would pass this test for free.
    printf 'void f() {\n    if (tok.expired()) return;\n}\nvoid g() {}\n' \
        > src/printer/foo.cpp
    git add src/printer/foo.cpp
    printf 'void f() {\n    if (tok.expired()) {\n        this->value_ = 1;\n    }\n}\n' \
        > src/printer/foo.cpp
    run python3 "$GATE_ABS" --staged-only
    [ "$status" -eq 0 ]
}
