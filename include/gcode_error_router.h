// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "error_event.h"
#include "firmware_error_envelope.h"
#include "i_moonraker_client.h" // SubscriptionId
#include "json_fwd.h"
#include "lvgl.h" // lv_timer_t (held presentations)

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace helix {
class IMoonrakerAPI;
}

// Test-only accessor (tests/test_helpers/gcode_error_router_test_access.h),
// forward-declared here so the friend grant below can name it explicitly.
struct GcodeErrorRouterTestAccess;

namespace helix {
class IMoonrakerClient;

namespace ui {
class RecoveryModalPresenter;
} // namespace ui

/// How a classified error should be surfaced to the user. Decided purely
/// from an ErrorEvent severity + whether it carries a recovery action,
/// so the routing decision is unit-testable without LVGL or printer state.
enum class PresentAs { MODAL, TOAST, TOAST_WITH_RECOVER, MODAL_WITH_RECOVER, NONE };

/// Pure routing function: maps an ErrorEvent to its presentation. In L0,
/// INFO is not surfaced. CRITICAL -> modal (with a recovery button if the
/// event carries one); WARNING -> toast (with a Recover action if present).
PresentAs decide_presentation(const ErrorEvent& e);

/// How a TOAST_WITH_RECOVER event's single action button should dispatch.
enum class RecoverDispatch {
    ESCALATE_TO_MODAL, ///< A toast cannot render it faithfully; use the modal.
    RECOVERY_SERVICE,  ///< key298: PrinterRecoveryService, not execute_gcode.
    GCODE,             ///< Run the action's own gcode.
    PLAIN_TOAST,       ///< Nothing runnable; surface the fault without a button.
};

/// Pure: decide how present_recover_toast() should honour @p e's actions.
///
/// A toast carries exactly one button and has no preheat gate, so an event
/// with several actions — or one needing a hot nozzle (#1193) — escalates to
/// the recovery modal rather than silently dropping affordances or firing into
/// a cold nozzle. Everything else runs the action's own gcode; only key298
/// routes through PrinterRecoveryService, which is why it alone carries an
/// empty gcode. Before #1172 this decision did not exist and every WARNING
/// with an action was offered a klipper_mcu bounce.
RecoverDispatch decide_recover_dispatch(const ErrorEvent& e);

/// Centralizes Klipper/Moonraker gcode-error surfacing for HelixScreen.
///
/// Two input paths feed in:
///   1. Live `notify_gcode_response` broadcast -- fires once per event.
///   2. `server.gcode_store` replay on (re)connect -- catches errors that
///      fired while HelixScreen was offline (broken boot autostart, crash
///      recovery, WebSocket bounce). Without this, reconnecting to a
///      paused printer shows no error context; the chinglish stays buried
///      in klippy.log.
///
/// Translation: `!! {"code":"key849",...}` -> `CfsErrorDecoder` ->
/// "Retract failed -- filament stuck in connector in unit 1 slot A.
///  Manually pull the filament back through the connector".
///
/// Routing:
///   - `key8xx` (CFS hardware) -> modal "Filament System Error"
///   - `key298` (MCU bridge)   -> toast with "Recover" action
///   - other `!!`              -> deferred toast (RPC-correlation dedup)
///   - `Error:` lines          -> toast
///   - replay path             -> modal (the user was disconnected; a
///                                transient toast they can miss is not
///                                enough on first reconnect)
///
/// Lifetime: owned by `Application`. Registers callbacks in the ctor and
/// unregisters them in the dtor; the MoonrakerClient pointer is not
/// owned and must outlive this router.
class GcodeErrorRouter {
  public:
    /// api and client may be nullptr (test/mock builds). presenter must
    /// outlive this router -- Application owns both and destroys the router
    /// before the presenter.
    GcodeErrorRouter(IMoonrakerAPI* api, IMoonrakerClient* client,
                     helix::ui::RecoveryModalPresenter& presenter);
    ~GcodeErrorRouter();

    GcodeErrorRouter(const GcodeErrorRouter&) = delete;
    GcodeErrorRouter& operator=(const GcodeErrorRouter&) = delete;

    /// Splits a raw response line into translated `text` plus extracted
    /// `out_code`. A firmware JSON envelope anywhere in the text is replaced
    /// by its message (or the error table's curated wording for its code),
    /// keeping the prose around it; `out_scheme`, when given, says whether
    /// the code is one the error table and severity rules know. Static +
    /// side-effect free so the replay path can reuse it without holding any
    /// router state.
    static void clean_error_text(std::string& text, std::string& out_code,
                                 FirmwareCodeScheme* out_scheme = nullptr);

    /// Replay age gate. Decides whether a latched `!!` line from the
    /// gcode_store should be re-surfaced on (re)connect, given its
    /// Moonraker timestamp `entry_time` and the current `now` (both Unix
    /// seconds). Static + side-effect free so it is unit-testable without
    /// time mocking or router state.
    ///
    /// Returns `true` (surface) when:
    ///   - the age cannot be positively determined (`entry_time <= 0`, an
    ///     absent/zero timestamp) -- we never suppress a possibly-fresh
    ///     error on missing data, or
    ///   - the error is recent (age <= kReplayMaxAgeSeconds).
    /// Returns `false` (suppress, log at debug) only when age is positively
    /// known to exceed the threshold -- the stale-after-restart case.
    static bool should_surface_replay(double entry_time, double now);

  private:
    /// Test-only access to the private presentation glue (`process_line`).
    /// Kept private + friend (L065/L088) so production callers cannot bypass
    /// the live/replay entry points. The accessor lives in the global
    /// namespace (tests/test_helpers), hence the leading `::`.
    friend struct ::GcodeErrorRouterTestAccess;

    /// Live `notify_gcode_response` handler -- runs on the WS thread.
    void on_notify_gcode_response(const nlohmann::json& msg);

    /// notify_status_update handler -- runs on main (the ctor's bg_cb wrapper
    /// defers the WS-thread delivery). Reads the firmware's standing-fault
    /// list and feeds every line not seen since the previous frame through
    /// process_line, so a fault standing across a restart reaches the screen
    /// even though nothing printed it to the console this session.
    void on_notify_status_update(const nlohmann::json& msg);

    /// Raise-notification handler (e.g. a fault-code firmware pushing every
    /// raise as it happens) -- runs on main via the ctor's bg_cb wrapper.
    /// The method name identifies the provider, so the payload reads through
    /// the same classify/wording/presentation pipe a console line takes.
    void on_notify_fault(const std::string& method, const nlohmann::json& msg);

    /// Fires on every WS connect / Klippy ready transition. Queries
    /// `server.gcode_store` and replays the most recent `!!` line that
    /// passes age + dedup gates.
    void on_connected();

    /// Walks a single response line through translate + emit. Used by
    /// both the live path and the replay path (replay only feeds `!!`
    /// lines; this still handles `Error:` for the live caller).
    /// `firmware_reported` marks a line the firmware itself reported (the
    /// standing-fault list, or a raise notification) rather than the console
    /// stream: such a fault is current by definition, so INFO floors to a
    /// toast.
    void process_line(const std::string& line, bool firmware_reported = false);

    /// Classify context read from the live printer state. Main thread only.
    static ClassifyContext current_classify_context();

    /// True when the active backend classifies the `!!` copy of an RPC error
    /// carrying `message` as a command refusal this router presents.
    [[nodiscard]] bool explains_rpc_error(const std::string& message) const;

    /// The broadcast claimant installed into rpc_error_correlation: when
    /// explains_rpc_error(), take `report` and run it after
    /// kRpcReportClaimMs unless the explanation has surfaced by then.
    bool claim_rpc_report(const std::string& message, std::function<void()>& report);

    /// Context carried by one claimed report's timer. Defined in the .cpp.
    struct ClaimCtx;

    /// Claimed reports' timers, cancelled in the dtor like pending_deferred_.
    /// Main-thread only.
    std::vector<lv_timer_t*> pending_claims_;

    /// CRITICAL error that carries a recovery action: delegates to the
    /// shared RecoveryModalPresenter.
    void present_recovery_modal(const ErrorEvent& e);

    /// WARNING error that carries a recovery action: toast with a "Recover"
    /// button (the key298 flow -- bounces klipper_mcu via
    /// PrinterRecoveryService rather than running a gcode).
    /// @return false when no API client exists, so nothing was shown — the
    ///         caller must not then claim the fault in
    ///         fault_surface_correlation and stand AmsErrorBridge down.
    bool present_recover_toast(const ErrorEvent& e);

    /// Held presentation: the event is shown `how`, 150ms later, unless a
    /// correlated copy of the same fault landed in between and made this one
    /// redundant -- a caller-handled RPC error response, or (on a fault-code
    /// firmware) the coded raise notification whose wording replaces the
    /// console prose. Plain toasts always take this path; on a fault-code
    /// firmware, uncoded console lines do too, whatever their severity.
    void present_deferred(const ErrorEvent& e, PresentAs how);

    /// The fire half of present_deferred: the re-checks passed, so `e` shows
    /// as `how` right now.
    void show_deferred(const ErrorEvent& e, PresentAs how, const std::string& short_form);

    /// Record that a coded raise carries `message`, so a prose presentation
    /// of the same fault still pending its 150ms timer stands down.
    void displace_prose_twin(const std::string& message);

    /// True when a coded raise displaced the prose copy carrying `raw`.
    [[nodiscard]] bool prose_twin_displaced(const std::string& raw) const;

    /// Context carried by one held presentation's timer. Defined in the .cpp.
    struct DeferredCtx;

    /// The held presentations' timers, cancelled in the dtor: each ctx holds
    /// a pointer back to this router, so a fire after teardown must not run.
    /// Main-thread only.
    std::vector<lv_timer_t*> pending_deferred_;

    /// Bytes-only truncation for transient toasts. Modals always get the
    /// full text -- they wrap to multiple lines.
    static std::string truncate_for_toast(std::string text);

    IMoonrakerAPI* api_;
    IMoonrakerClient* client_;

    /// Shared modal presenter. Not owned; must outlive this router.
    helix::ui::RecoveryModalPresenter& presenter_;

    /// Dedup state for the replay path. Multiple WS events can fire the
    /// connected observer in quick succession (WS open -> Klippy ready);
    /// without this we would modal the same error twice.
    std::mutex replay_mutex_;
    double last_replayed_time_ = 0.0;

    /// [L072] notify_status_update subscription, unregistered symmetrically
    /// in the dtor.
    SubscriptionId standing_notify_id_ = kInvalidSubscriptionId;

    /// The coded lines surfaced from the standing-fault list, main-thread only
    /// (the bg_cb wrapper defers the notify body). A line leaves the set when
    /// the firmware clears it, so a re-raised fault surfaces again; a delta
    /// frame that omits the fault object leaves the set untouched.
    std::set<std::string> standing_fault_lines_;

    /// Standing-fault lines handed to process_line so far. The correlation
    /// registry's short window cannot distinguish "surfaced once" from
    /// "surfaced twice", so this counter is the count tests assert on.
    size_t standing_fed_count_ = 0;

    /// (unix seconds, firmware message) of coded raises, consulted by a
    /// pending prose presentation's fire-time re-check. Router-internal
    /// rather than a correlation registry because those are owner-less: a
    /// record the prose path itself wrote at schedule time would read back
    /// as its own displacement. Main-thread only.
    std::vector<std::pair<double, std::string>> displaced_prose_;

    /// Held presentations that actually fired. Both spellings of a
    /// prose/coded pair schedule one, so counts alone cannot say whose
    /// wording showed; last_deferred_shown_ names it. Test observables.
    size_t deferred_shown_count_ = 0;
    std::string last_deferred_shown_;

    /// [L072] Generation guard for callbacks captured by MoonrakerClient.
    /// `MoonrakerClient::unregister_method_callback` and
    /// `remove_connected_observer` are not synchronized against in-flight
    /// invocations (callbacks are copied under lock then invoked outside),
    /// so a WS-thread callback that already entered the invoke phase can
    /// fire on a destroyed router. All three registrations route through
    /// `lifetime_.bg_cb(...)`, which queues to main and skips on
    /// generation mismatch. Declared last so it destructs FIRST in member
    /// teardown -- outstanding tokens are invalidated before anything else
    /// the body might touch.
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
