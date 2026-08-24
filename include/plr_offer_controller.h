// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "plr_backend.h"

#include <string>

class PlrOfferControllerTestAccess;

namespace helix::ui {

/// App-lifetime controller that offers the power-loss-recovery prompt at
/// connect time. Owned by SubjectInitializer (constructed in init_observers,
/// destroyed with it — the ObserverGuard members auto-reset via RAII).
///
/// Two backends feed ONE normalized "a resumable snapshot exists" signal, which
/// is what keeps the latch / re-arm / wizard logic backend-agnostic. Full
/// mechanism: docs/devel/POWER_LOSS_RECOVERY.md.
///
///   Snapmaker (PASSIVE): virtual_sdcard.pl_env_valid goes true on its own.
///   Creality  (ACTIVE):  print_stats.power_loss appearing in status marks the
///                        capability; availability requires firing the
///                        side-effectful `check_continue_print_state` probe,
///                        ONCE per connection, ONLY while the printer is in
///                        STANDBY, and getting both states back true.
///
/// Four observers, all on STATIC subjects (singleton lifetime, no
/// SubjectLifetime token needed). The authoritative account of WHEN the offer
/// fires and re-fires lives at the plr_should_offer decision site in
/// evaluate_offer (ui_plr_offer_controller.cpp); the observers below are just
/// the edges that drive it:
///   - pl_env_valid (PRIMARY Snapmaker trigger): a genuine 0->1 edge offers.
///   - creality_plr_capable (PRIMARY Creality trigger): a 0->1 edge fires the
///     one-shot probe, whose response then offers.
///   - connection state: on a CONNECTED->not-CONNECTED edge, re-arms BOTH
///     one-shot latches (offer and probe), drops the cached Creality detect
///     result, AND forces both capability subjects back to 0 (see
///     on_connection_state_changed) so the next reconnect produces real 0->1
///     edges rather than same-value writes the subjects would swallow.
///   - wizard active: on a 1->0 edge (wizard closed) re-evaluates the offer,
///     so an offer that was suppressed only because the wizard owned the
///     screen now fires. This is what makes wizard suppression temporary
///     instead of permanent.
///
/// Observer callbacks all run on the main thread — observe_int_sync defers via
/// the update queue and plr_should_offer/plr_should_rearm are pure. The one
/// genuine thread crossing is the probe response (a JSON-RPC callback on the
/// WebSocket thread), which is wrapped in lifetime_.bg_cb().
class PlrOfferController {
    friend class ::PlrOfferControllerTestAccess;

  public:
    PlrOfferController();
    ~PlrOfferController() = default;

    PlrOfferController(const PlrOfferController&) = delete;
    PlrOfferController& operator=(const PlrOfferController&) = delete;

  private:
    // Single evaluation point: reads both capability subjects plus
    // printer/wizard state live, normalizes them into one availability signal,
    // and offers when plr_should_offer() agrees AND a plan with a permitted
    // resume can be built. Every observer routes here so the decision lives in
    // exactly one place.
    void evaluate_offer();
    void on_pl_env_valid_changed(int pl_env_valid);
    void on_creality_capable_changed(int capable);
    void on_connection_state_changed(int new_conn_state);
    void on_wizard_active_changed(int wizard_active);

    /// Fire the Creality probe at most once per connection, and only from
    /// STANDBY. See the header warning on
    /// IMoonrakerAPI::check_continue_print_state — this call has side effects.
    void probe_creality_once();
    void on_creality_detect_result(const helix::PlrDetectResult& result);

    ObserverGuard pl_valid_observer_;
    ObserverGuard creality_capable_observer_;
    ObserverGuard conn_observer_;
    ObserverGuard wizard_observer_;

    /// Guards the probe response, which arrives on the WebSocket thread.
    helix::AsyncLifetimeGuard lifetime_;

    bool prompted_this_connect_ = false;
    bool creality_probed_this_connect_ = false;
    /// Cached probe outcome. `completed` is the resume authorization — never
    /// set it anywhere but on_creality_detect_result.
    helix::PlrDetectResult creality_detect_{};
    std::string creality_recovery_file_;
    int last_conn_state_ = 0;
};

} // namespace helix::ui
