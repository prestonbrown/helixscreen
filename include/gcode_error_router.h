// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "action_prompt_manager.h" // PromptData / PromptButton
#include "async_lifetime_guard.h"
#include "error_event.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hv/json.hpp"

class IMoonrakerAPI;

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

/// Pure: turn an ErrorEvent's recovery actions into a renderable PromptData.
/// LVGL-free; style maps to PromptButton.color. Title falls back to a default.
PromptData build_recovery_prompt(const ErrorEvent& e);

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
    /// `out_code`. Static + side-effect free so the replay path can
    /// reuse it without holding any router state. Public because tests
    /// cover both the pure-JSON and embedded-JSON shapes K2 emits.
    static void clean_error_text(std::string& text, std::string& out_code);

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
    ///   - the error is recent (age <= REPLAY_MAX_AGE_SECONDS).
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

    /// Fires on every WS connect / Klippy ready transition. Queries
    /// `server.gcode_store` and replays the most recent `!!` line that
    /// passes age + dedup gates.
    void on_connected();

    /// Walks a single response line through translate + emit. Used by
    /// both the live path and the replay path (replay only feeds `!!`
    /// lines; this still handles `Error:` for the live caller).
    void process_line(const std::string& line);

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

    /// Plain unclassified toast: deferred 150ms so a late-arriving RPC
    /// error response can populate the correlation buffer first.
    /// @param text     cleaned/translated text to display
    /// @param raw_text Klipper's pre-clean wording — the dedup identity the
    ///                 fire-time re-check matches on (see ErrorEvent::raw_detail)
    void present_deferred_toast(const std::string& text, const std::string& raw_text);

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
