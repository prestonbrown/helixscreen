// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

/**
 * @brief Single source of truth for "who reports this JSON-RPC error to the user".
 *
 * A failed `printer.gcode.script` can be reported through three independent
 * surfaces, and exactly one of them should win:
 *
 *  1. The caller's own error callback (a contextual toast/modal it raises itself).
 *  2. The Request Tracker's generic "Printer command '<m>' failed" fallback.
 *  3. `GcodeErrorRouter`, from Klipper's `!!` broadcast on the gcode response
 *     stream — the richest surface, since it classifies severity, lets the AMS
 *     backends interpret the line, and escalates to a recovery modal mid-print.
 *
 * This decision used to be re-derived at four sites that drifted apart:
 * the tracker (`caller_handles_ui`), `MoonrakerMotionAPI::execute_gcode`
 * (`silent = on_error != nullptr`), the mock's print path, and
 * `IAdvancedAPI::execute_macro`'s `suppress_auto_toast` alias. Only the motion
 * copy got the subtle part right — see `CallerIntent::surfaces_errors`.
 */
namespace helix::rpc_error_policy {

/**
 * @brief What the CALLER told us about who surfaces the error.
 *
 * Must be built from the caller's own callbacks, BEFORE any internal wrapping.
 * `IMoonrakerAPI::execute_gcode` and `MoonrakerMotionAPI::execute_gcode` both
 * wrap `on_error` to settle their in-flight activity counters, which makes the
 * callback non-null even when the caller passed `nullptr`. Deriving intent
 * after that point reads our own bookkeeping as a promise the caller never
 * made, and silently suppresses surface 3.
 */
struct CallerIntent {
    /// Caller opted out of ALL automatic error UI (internal probes, polls, and
    /// long AMS macros whose completion is tracked out-of-band). Reaches here
    /// as `silent` / `suppress_auto_toast`.
    bool silent = false;

    /// Caller supplied its own error callback and is presumed to show the user
    /// something. Pass `false` explicitly when that callback only logs — a
    /// `spdlog::warn` is not a user-visible report, and letting it claim
    /// ownership silences the `!!` router for an error nobody ever sees.
    bool surfaces_errors = false;
};

/// Facts about the request itself, independent of what the caller promised.
struct RequestFacts {
    /// Klipper mirrors this failure onto its gcode response stream as a `!!`
    /// line, so `GcodeErrorRouter` will report it. True for
    /// `printer.gcode.script` only — see method_has_broadcast_channel().
    bool has_broadcast_channel = false;

    /// Global mute (teardown/shutdown).
    bool suppress_all = false;
};

/// True when a failed @p method is also broadcast on Klipper's gcode response
/// stream. Keeps the method-name literal in one place.
constexpr bool method_has_broadcast_channel(std::string_view method) {
    return method == std::string_view{"printer.gcode.script"};
}

/// Which surfaces may fire for one failed request.
struct Decision {
    /// Emit the tracker's generic RPC_ERROR fallback toast.
    bool emit_generic_toast;

    /// Record the message so `GcodeErrorRouter` suppresses its own toast for
    /// the matching `!!` broadcast (see include/rpc_error_correlation.h).
    bool record_for_dedup;
};

/**
 * @brief Decide which error surfaces may fire.
 *
 * The invariant: one Klipper rejection produces exactly ONE user-visible
 * report. Zero is the silent-failure bug; two is the `key69` bug.
 *
 * @param intent What the caller promised, captured pre-wrap.
 * @param facts  Properties of the request itself.
 */
constexpr Decision decide(const CallerIntent& intent, const RequestFacts& facts) {
    if (facts.suppress_all) {
        // Nothing is surfaced and nothing is recorded: the `!!` router is
        // equally muted, and a stale record would poison the 1.5s correlation
        // window past reconnect and eat the first real error after recovery.
        return Decision{false, false};
    }

    // A caller that raises its own error UI — or opted out of error UI
    // entirely — owns the report. Stacking the generic fallback on top
    // double-reports one Klipper rejection (the K2 `key69` chamber error
    // produced three toasts, two of them raw JSON; bdf32d07d).
    const bool caller_owns_report = intent.silent || intent.surfaces_errors;

    // When Klipper also broadcasts the failure, GcodeErrorRouter owns the
    // report and the generic fallback must stay out of its way: the router
    // classifies severity, lets the AMS backends interpret the line, and
    // escalates to a recovery modal mid-print. Note the router only defers
    // (and re-checks) its plain-TOAST arm — a CRITICAL modal fires
    // immediately, so a competing generic toast would stack on top of it
    // regardless of which channel lands first.
    //
    // Residual gap, accepted knowingly: a gcode.script RPC error that Klipper
    // does NOT mirror as `!!` (a Moonraker-level rejection) whose caller does
    // not surface errors reports nothing. That set is small because the
    // klippy-not-ready guards reject those before a request is ever tracked.
    const bool emit_generic_toast = !caller_owns_report && !facts.has_broadcast_channel;

    // Record whenever SOMEONE is definitely reporting this — the caller's own
    // UI, or the generic fallback we just authorised — so the `!!` copy of the
    // same rejection dedups against it. A merely `silent` caller does not
    // qualify: nothing was shown, so the `!!` copy is the only signal left.
    return Decision{emit_generic_toast,
                    /*record_for_dedup=*/intent.surfaces_errors || emit_generic_toast};
}

} // namespace helix::rpc_error_policy
