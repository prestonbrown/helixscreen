// SPDX-License-Identifier: GPL-3.0-or-later
// tests/unit/test_busy_toast_app_initiated.cpp

/**
 * @file test_busy_toast_app_initiated.cpp
 * @brief The "Printer is busy — your X will run when it's ready" toast must not
 *        fire when the blocking op is one HelixScreen itself just started.
 *
 * prestonbrown/helixscreen#1206: pressing Unload on the filament panel sends
 * UNLOAD_FILAMENT, which takes Klipper's gcode lock and flips idle_timeout to
 * "Printing". Any discretionary command that follows (the panel's own fan/temp
 * housekeeping, an LED change) then hit the busy branch and toasted the user
 * about the very operation they had just started themselves.
 *
 * The fix is scoped to the TOAST DECISION only. Neither blocking-op predicate
 * moves: motion must keep refusing a late jog during a filament op (#1108
 * toolhead-collision hazard). MoonrakerAPI::execute_gcode stamps
 * PrinterState::app_macro_activity() around every non-discretionary send, and
 * the toast — and only the toast — consults it.
 *
 * The attribution is sound only while that counter is balanced. A leaked
 * note_sent() suppresses the busy toast for the whole session, which is the
 * same silent-wedge shape as #1129 (LedController's in-flight counter stuck at
 * >=1 until disconnect, greying the light buttons out). The most dangerous
 * shape is the null-callback send: the naive `if (on_success)` wrapper never
 * installs a settle when the caller passed no callbacks, so the stamp leaks.
 * The first test below pins exactly that.
 */

#include "../../include/app_macro_activity.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../busy_guard_fixture.h"

#include <chrono>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A macro/filament op — non-discretionary, so execute_gcode stamps it.
constexpr const char* MACRO = "UNLOAD_FILAMENT";
/// A benign discretionary command — never stamped, and the thing that used to
/// trigger the spurious toast.
constexpr const char* DISCRETIONARY = "M106 S128";

class MacroActivityFixture : public helix::BusyGuardFixture {
  public:
    bool macro_inflight() {
        return helix::activity_inflight(state.app_macro_activity());
    }

    /**
     * True once note_done() has fired at all: note_done() stamps the grace
     * timestamp, and an activity that was never stamped has last_done_ == 0,
     * which never reads active. Only meaningful while inflight is zero.
     */
    bool macro_note_done_fired() {
        return state.app_macro_activity().recently_active();
    }

    /// Destructive — call it last in a test.
    int drain_inflight() {
        return helix::drain_activity_inflight(state.app_macro_activity());
    }
};

} // namespace

// ============================================================================
// Balance — the counter must never leak upward
// ============================================================================

TEST_CASE_METHOD(MacroActivityFixture,
                 "execute_gcode balances the macro counter when BOTH callbacks are null",
                 "[busy_guard][1206][mock]") {
    // THE case this change exists to protect. Most macro senders pass no
    // callbacks at all. The naive wrapper — `if (on_success) success_wrapper =
    // ...` — installs no settle here, so note_sent() runs and note_done() never
    // does. The counter sticks at 1 and the busy toast is suppressed for the
    // rest of the session, silently, exactly like #1129.
    api->execute_gcode(MACRO, nullptr, nullptr);

    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");

    // note_done() ran (it stamps the grace timestamp) ...
    CHECK(macro_note_done_fired());
    // ... and left nothing outstanding.
    CHECK_FALSE(macro_inflight());
    CHECK(drain_inflight() == 0);
}

TEST_CASE_METHOD(MacroActivityFixture,
                 "execute_gcode balances the macro counter on the success path",
                 "[busy_guard][1206][mock]") {
    bool success_called = false;
    api->execute_gcode(
        MACRO, [&success_called]() { success_called = true; },
        [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(success_called);
    CHECK_FALSE(error_called);

    CHECK(macro_note_done_fired());
    CHECK_FALSE(macro_inflight());
    CHECK(drain_inflight() == 0);
}

TEST_CASE_METHOD(MacroActivityFixture, "execute_gcode balances the macro counter on the error path",
                 "[busy_guard][1206][mock]") {
    // Force the RPC to fail after the stamp: the error wrapper owes the same
    // single note_done() the success wrapper does, or the count leaks upward.
    mock_client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "forced RPC failure", MACRO);

    bool success_called = false;
    api->execute_gcode(
        MACRO, [&success_called]() { success_called = true; },
        [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(error_called);
    CHECK_FALSE(success_called);

    CHECK(macro_note_done_fired());
    CHECK_FALSE(macro_inflight());
    CHECK(drain_inflight() == 0);
}

TEST_CASE_METHOD(MacroActivityFixture, "execute_gcode stamps exactly one note_done per macro send",
                 "[busy_guard][1206][mock]") {
    // Two unrelated sends already outstanding. A balanced send must leave the
    // count exactly where it found it: a leaked note_sent reads 3, a double
    // note_done reads 1. Only exact pairing reads 2.
    state.app_macro_activity().note_sent();
    state.app_macro_activity().note_sent();

    api->execute_gcode(MACRO, nullptr, [this](const MoonrakerError& err) { error_cb(err); });

    CHECK_FALSE(error_called);
    CHECK(drain_inflight() == 2);
}

// ============================================================================
// Nothing that was refused, and nothing discretionary, may stamp
// ============================================================================

TEST_CASE_METHOD(MacroActivityFixture, "execute_gcode does not stamp when klippy is halted",
                 "[busy_guard][1206][mock]") {
    // The klippy-halted gate is the earliest return in execute_gcode. A stamp
    // there would leak forever — nothing acks a send that was never made.
    state.set_klippy_state_sync(KlippyState::SHUTDOWN);

    api->execute_gcode(MACRO, nullptr, [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(error_called);
    CHECK(captured_error.type == MoonrakerErrorType::NOT_READY);
    CHECK(mock_client.gcode_script_history().empty());

    CHECK_FALSE(macro_inflight());
    CHECK_FALSE(macro_note_done_fired());
}

TEST_CASE_METHOD(MacroActivityFixture, "execute_gcode never stamps discretionary gcode",
                 "[busy_guard][1206][mock]") {
    // Fan/temp/LED commands are exactly what the busy toast is about; treating
    // one as app-macro activity would suppress the toast for the command that
    // is supposed to raise it. Both reads false proves neither note_sent() nor
    // note_done() ran — a stamped-and-balanced send would still show
    // note_done_fired().
    bool success_called = false;
    api->execute_gcode(
        DISCRETIONARY, [&success_called]() { success_called = true; },
        [this](const MoonrakerError& err) { error_cb(err); });

    REQUIRE(success_called);
    CHECK_FALSE(error_called);
    REQUIRE(mock_client.last_send_method() == "printer.gcode.script");

    CHECK_FALSE(macro_inflight());
    CHECK_FALSE(macro_note_done_fired());
}

// ============================================================================
// The toast decision itself
// ============================================================================

TEST_CASE_METHOD(MacroActivityFixture,
                 "busy toast is suppressed while an app-initiated macro is in flight",
                 "[busy_guard][1206][mock]") {
    // The user pressed Unload themselves: a macro is outstanding, so the
    // busy-ness is self-inflicted and the toast is noise.
    state.app_macro_activity().note_sent();
    begin_blocking_episode();

    // The blocking-op predicates are deliberately NOT narrowed — motion must
    // keep refusing during a filament op (#1108). Only the toast changes.
    REQUIRE(state.is_external_blocking_operation_active());

    api->execute_gcode(DISCRETIONARY, nullptr, nullptr);

    // The latch must still be claimable: suppression short-circuits BEFORE
    // claim_busy_queue_toast(), so a genuinely external op later in the same
    // episode can still announce itself.
    CHECK(state.claim_busy_queue_toast());

    // The command itself still went out fire-and-forget — suppression is about
    // the notification only.
    CHECK_FALSE(mock_client.gcode_script_history().empty());

    state.app_macro_activity().note_done();
}

TEST_CASE_METHOD(MacroActivityFixture,
                 "busy toast is suppressed inside the grace window after a macro settles",
                 "[busy_guard][1206][mock]") {
    // The #1206 sequence end to end: UNLOAD_FILAMENT acks (Moonraker returns as
    // soon as the script is accepted) but idle_timeout.state lags behind, so the
    // very next discretionary command still sees a blocking op. Without the
    // grace window the toast would fire in exactly that gap.
    api->execute_gcode(MACRO, nullptr, nullptr);
    REQUIRE(macro_note_done_fired()); // settled, and inside the grace window
    REQUIRE_FALSE(macro_inflight());

    begin_blocking_episode();
    REQUIRE(state.is_external_blocking_operation_active());

    api->execute_gcode(DISCRETIONARY, nullptr, nullptr);

    CHECK(state.claim_busy_queue_toast());
}

TEST_CASE_METHOD(MacroActivityFixture,
                 "busy toast STILL fires for an externally-initiated blocking op",
                 "[busy_guard][1206][mock]") {
    // The regression guard. A bed mesh started from Mainsail, or a macro typed
    // into another frontend's console, arrives out of nowhere from the panel's
    // point of view — that is worth announcing, and #1206 must not silence it.
    // No app macro in flight, so nothing suppresses the toast.
    begin_blocking_episode();
    REQUIRE(state.is_external_blocking_operation_active());
    REQUIRE_FALSE(macro_note_done_fired());

    api->execute_gcode(DISCRETIONARY, nullptr, nullptr);

    // The toast consumed the once-per-episode latch, so it is no longer
    // claimable.
    CHECK_FALSE(state.claim_busy_queue_toast());
}

// ============================================================================
// The in-flight age ceiling
// ============================================================================
//
// execute_gcode wraps both callbacks, and the request tracker settles a
// registered request exactly once (success / JSON-RPC error / timeout / send
// failure / connection loss). But a few paths settle NOTHING: ~MoonrakerClient
// deliberately drops pending requests without invoking their error callbacks
// (UAF avoidance), and cancel_request() erases an entry silently. A stamp lost
// that way pins inflight_ at >= 1 forever, and a permanently "active" tracker
// would suppress the busy toast for the whole session — the #1129 wedge, moved
// to a new counter.
//
// MAX_INFLIGHT_AGE bounds that: once even the NEWEST send is older than the
// ceiling, the counter is treated as stuck rather than busy.

TEST_CASE_METHOD(MacroActivityFixture,
                 "a dropped RPC response wedges the counter until the ceiling",
                 "[busy_guard][1206][mock]") {
    // force_next_gcode_dropped_response() is the mock's simulation of exactly
    // the real hazard: Klipper runs the gcode, but NEITHER callback ever fires.
    mock_client.force_next_gcode_dropped_response(MACRO);

    bool success_called = false;
    api->execute_gcode(
        MACRO, [&success_called]() { success_called = true; },
        [this](const MoonrakerError& err) { error_cb(err); });

    // The send happened; the response did not.
    REQUIRE_FALSE(mock_client.gcode_script_history().empty());
    REQUIRE_FALSE(success_called);
    REQUIRE_FALSE(error_called);

    // The counter is genuinely stuck: nothing ran note_done(), so the tracker
    // reads active right now and would keep reading active indefinitely.
    const auto now = AppMacroActivity::clock::now();
    REQUIRE(state.app_macro_activity().recently_active(now));

    // The ceiling releases it. Asserted symbolically against MAX_INFLIGHT_AGE so
    // the test tracks the constant rather than pinning today's value.
    CHECK_FALSE(state.app_macro_activity().recently_active(
        now + AppMacroActivity::MAX_INFLIGHT_AGE + std::chrono::minutes(1)));

    // ...and it was exactly one leaked stamp, not more. Destructive; last.
    CHECK(drain_inflight() == 1);
}

TEST_CASE_METHOD(MacroActivityFixture, "the in-flight ceiling does not expire early",
                 "[busy_guard][1206][mock]") {
    // The other half of the contract, and the one that separates a working
    // ceiling from a stub: a ceiling that always reported "stuck" would satisfy
    // the release test above while silently disabling #1206 suppression
    // entirely — every legitimately in-flight macro would read inactive and the
    // spurious toast would come straight back.
    mock_client.force_next_gcode_dropped_response(MACRO);
    api->execute_gcode(MACRO, nullptr, nullptr);

    // CHECK, not REQUIRE: a stubbed ceiling breaks this too, and aborting here
    // would stop the near-ceiling assertion below — the one that actually pins
    // the contract — from ever running.
    const auto now = AppMacroActivity::clock::now();
    CHECK(state.app_macro_activity().recently_active(now));

    // One minute short of the ceiling, an outstanding send is still active.
    // A five-minute macro (MACRO_TIMEOUT_MS / AMS_OPERATION_TIMEOUT_MS) sits
    // well inside this window.
    CHECK(state.app_macro_activity().recently_active(now + AppMacroActivity::MAX_INFLIGHT_AGE -
                                                     std::chrono::minutes(1)));

    CHECK(drain_inflight() == 1);
}

TEST_CASE_METHOD(MacroActivityFixture,
                 "busy toast returns once a wedged stamp ages past the ceiling",
                 "[busy_guard][1206][mock]") {
    // The user-visible promise: a leaked stamp costs ten minutes of suppressed
    // toast, not the rest of the session.
    //
    // The production toast decision calls recently_active() with the default
    // `now`, which a test cannot fast-forward — so instead of waiting out the
    // wall clock, plant the stamp with an aged timestamp. This is precisely the
    // state the wedge in the two tests above reaches once MAX_INFLIGHT_AGE has
    // elapsed: inflight_ == 1, no note_done() ever, newest send older than the
    // ceiling.
    state.app_macro_activity().note_sent(AppMacroActivity::clock::now() -
                                         AppMacroActivity::MAX_INFLIGHT_AGE -
                                         std::chrono::minutes(1));
    REQUIRE_FALSE(state.app_macro_activity().recently_active());

    begin_blocking_episode();
    REQUIRE(state.is_external_blocking_operation_active());

    api->execute_gcode(DISCRETIONARY, nullptr, nullptr);

    // Suppression is off, so the toast fired and consumed the once-per-episode
    // latch — normal #1108 behaviour is fully restored despite the leak.
    CHECK_FALSE(state.claim_busy_queue_toast());
}
