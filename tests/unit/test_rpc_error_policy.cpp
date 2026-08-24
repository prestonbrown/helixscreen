// SPDX-License-Identifier: GPL-3.0-or-later
#include "rpc_error_policy.h"

#include "catch_amalgamated.hpp"

using helix::rpc_error_policy::CallerIntent;
using helix::rpc_error_policy::decide;
using helix::rpc_error_policy::Decision;
using helix::rpc_error_policy::method_has_broadcast_channel;
using helix::rpc_error_policy::RequestFacts;

namespace {
// What the caller promised.
constexpr CallerIntent nobody{/*silent=*/false, /*surfaces_errors=*/false};
constexpr CallerIntent quiet{/*silent=*/true, /*surfaces_errors=*/false};
constexpr CallerIntent shows{/*silent=*/false, /*surfaces_errors=*/true};
constexpr CallerIntent quiet_and_shows{/*silent=*/true, /*surfaces_errors=*/true};

// What the request is. `gcode` is mirrored by Klipper as a `!!` line, so
// GcodeErrorRouter reports it; `plain_rpc` has no second channel.
constexpr RequestFacts gcode{/*has_broadcast_channel=*/true, /*suppress_all=*/false};
constexpr RequestFacts plain_rpc{/*has_broadcast_channel=*/false, /*suppress_all=*/false};
constexpr RequestFacts muted{/*has_broadcast_channel=*/true, /*suppress_all=*/true};

/// How many user-visible reports a decision authorises, given whether the
/// caller itself renders one. The whole policy exists to keep this at 1.
constexpr int surfaces(const Decision& d, const CallerIntent& i, const RequestFacts& f) {
    const int caller = i.surfaces_errors ? 1 : 0;
    const int generic = d.emit_generic_toast ? 1 : 0;
    // The router speaks for a broadcast method unless someone recorded a dedup.
    const int router = (f.has_broadcast_channel && !f.suppress_all && !d.record_for_dedup) ? 1 : 0;
    return caller + generic + router;
}
} // namespace

// --- Method classification ----------------------------------------------------

TEST_CASE("only printer.gcode.script carries a `!!` broadcast channel",
          "[error-center][rpc-error-policy]") {
    REQUIRE(method_has_broadcast_channel("printer.gcode.script"));
    REQUIRE_FALSE(method_has_broadcast_channel("server.files.list"));
    REQUIRE_FALSE(method_has_broadcast_channel("printer.emergency_stop"));
    REQUIRE_FALSE(method_has_broadcast_channel(""));
    // Whole-string, not prefix — a longer method name must not trip it.
    REQUIRE_FALSE(method_has_broadcast_channel("printer.gcode.script.extra"));
}

// --- gcode: the router owns the report ----------------------------------------

TEST_CASE("gcode nobody claims -> router alone reports, generic stays out of its way",
          "[error-center][rpc-error-policy]") {
    // The dominant macro-send shape (on_error == nullptr). Klipper's `!!` copy
    // is a strictly richer surface than "Printer command '...' failed": it
    // classifies severity, lets the AMS backends interpret the line, and
    // escalates to a recovery modal mid-print.
    const Decision d = decide(nobody, gcode);
    REQUIRE_FALSE(d.emit_generic_toast);
    REQUIRE_FALSE(d.record_for_dedup);
    REQUIRE(surfaces(d, nobody, gcode) == 1);
}

TEST_CASE("gcode silent log-only caller -> router still reports",
          "[error-center][rpc-error-policy]") {
    // AmsSubscriptionBackend's shape. `silent` means "no automatic toast from
    // us", not "the user has been told", so it must not silence the router.
    const Decision d = decide(quiet, gcode);
    REQUIRE_FALSE(d.emit_generic_toast);
    REQUIRE_FALSE(d.record_for_dedup);
    REQUIRE(surfaces(d, quiet, gcode) == 1);
}

TEST_CASE("gcode caller that surfaces errors owns the report end to end",
          "[error-center][rpc-error-policy]") {
    const Decision d = decide(shows, gcode);
    REQUIRE_FALSE(d.emit_generic_toast);
    REQUIRE(d.record_for_dedup); // silences the `!!` copy
    REQUIRE(surfaces(d, shows, gcode) == 1);
}

TEST_CASE("gcode silent + surfaces_errors still records for dedup",
          "[error-center][rpc-error-policy]") {
    // print_control_buttons / ui_resume_dispatch pass suppress_auto_toast AND
    // raise their own UI. The `!!` broadcast for the same rejection must dedup.
    const Decision d = decide(quiet_and_shows, gcode);
    REQUIRE_FALSE(d.emit_generic_toast);
    REQUIRE(d.record_for_dedup);
    REQUIRE(surfaces(d, quiet_and_shows, gcode) == 1);
}

// --- plain RPC: no second channel, so the fallback matters --------------------

TEST_CASE("plain RPC nobody claims -> generic fallback is the only report",
          "[error-center][rpc-error-policy]") {
    const Decision d = decide(nobody, plain_rpc);
    REQUIRE(d.emit_generic_toast);
    REQUIRE(surfaces(d, nobody, plain_rpc) == 1);
}

TEST_CASE("plain RPC caller that surfaces errors suppresses the fallback",
          "[error-center][rpc-error-policy]") {
    const Decision d = decide(shows, plain_rpc);
    REQUIRE_FALSE(d.emit_generic_toast);
    REQUIRE(surfaces(d, shows, plain_rpc) == 1);
}

// --- Global mute --------------------------------------------------------------

TEST_CASE("suppress_all mutes every surface and records nothing",
          "[error-center][rpc-error-policy]") {
    // Recording during teardown would leave the correlation window poisoned for
    // 1.5s past a reconnect, eating the first real error after recovery.
    for (const CallerIntent& i : {nobody, quiet, shows, quiet_and_shows}) {
        const Decision d = decide(i, muted);
        REQUIRE_FALSE(d.emit_generic_toast);
        REQUIRE_FALSE(d.record_for_dedup);
    }
}

// --- Named regressions this policy exists to hold ----------------------------

TEST_CASE("key69: one Klipper rejection never yields a stacked generic toast",
          "[error-center][rpc-error-policy][key69]") {
    // bdf32d07d: the K2 chamber rejection produced three toasts (generic +
    // caller's own + `!!`). The caller's contextual toast wins; the generic
    // fallback stays quiet and the `!!` copy dedups against it.
    const Decision d = decide(shows, gcode);
    REQUIRE_FALSE(d.emit_generic_toast);
    REQUIRE(d.record_for_dedup);
    REQUIRE(surfaces(d, shows, gcode) == 1);
}

TEST_CASE("a log-only callback does not get to silence the `!!` router",
          "[error-center][rpc-error-policy]") {
    // An AFC / Happy Hare / CFS macro rejection whose error_cb only calls
    // spdlog must still reach the user through GcodeErrorRouter.
    const CallerIntent log_only{/*silent=*/true, /*surfaces_errors=*/false};
    const Decision d = decide(log_only, gcode);
    REQUIRE_FALSE(d.record_for_dedup);
    REQUIRE(surfaces(d, log_only, gcode) == 1);
}

TEST_CASE("intent is the caller's, not the activity-counter wrapper's",
          "[error-center][rpc-error-policy]") {
    // MoonrakerAPI::execute_gcode wraps on_error to settle app_macro_activity,
    // making the callback non-null for every non-discretionary gcode even when
    // the caller passed nullptr. Intent built pre-wrap keeps that invisible
    // here: a caller who promised nothing does not get to silence the router.
    const CallerIntent caller_passed_nullptr{/*silent=*/false, /*surfaces_errors=*/false};
    const Decision d = decide(caller_passed_nullptr, gcode);
    REQUIRE_FALSE(d.record_for_dedup);
    REQUIRE(surfaces(d, caller_passed_nullptr, gcode) == 1);
}

// --- The invariant, over the whole matrix ------------------------------------

TEST_CASE("exactly one user-visible report for every intent x request shape",
          "[error-center][rpc-error-policy]") {
    // Zero is the silent-failure bug; two is the key69 bug. The one documented
    // exception is a caller that explicitly opted out of automatic UI on a
    // method with no broadcast channel — it asked for silence and there is no
    // richer surface to fall back to.
    for (const RequestFacts& f : {gcode, plain_rpc}) {
        for (bool silent : {false, true}) {
            for (bool surfaces_errors : {false, true}) {
                const CallerIntent i{silent, surfaces_errors};
                const Decision d = decide(i, f);
                const int n = surfaces(d, i, f);
                CAPTURE(f.has_broadcast_channel, silent, surfaces_errors, n);

                const bool opted_out_with_no_fallback =
                    !f.has_broadcast_channel && silent && !surfaces_errors;
                if (opted_out_with_no_fallback) {
                    REQUIRE(n == 0);
                } else {
                    REQUIRE(n == 1);
                }
            }
        }
    }
}
