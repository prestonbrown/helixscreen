// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "async_lifetime_guard.h"
#include "observer_factory.h"
#include "recovery_modal_presenter.h"

#include <string>

namespace helix {

/// Observes AmsState's action subject and routes AmsAction::ERROR edges into
/// RecoveryModalPresenter. Purely a bridge — contains no error semantics.
///
/// Also the application-wide backstop for faults nothing else surfaces: a
/// backend that raises ERROR without an ErrorEvent and without a `!!` line
/// (AFC's local action timeout) otherwise stops the spinner and shows the user
/// nothing at all, which reads as success.
class AmsErrorBridge {
  public:
    explicit AmsErrorBridge(helix::ui::RecoveryModalPresenter& presenter);
    void start(); ///< installs the observer on AmsState's action subject (one-shot)
  private:
    void on_action_changed(int action);

    /// Fires on every ams_action_detail subject change. Mid-ERROR-episode it
    /// re-consults current_error() so a fault whose text moved on while the
    /// action stayed ERROR is re-presented instead of leaving the first message
    /// on screen. Outside ERROR it is a no-op.
    void on_detail_changed(const char* detail);

    /// Runs one queue tick after the ERROR edge. Toasts only if the fault is
    /// still current and no dialog anywhere ended up describing it.
    void surface_unhandled_error();

    /// True when a dialog for this fault is already on screen.
    [[nodiscard]] bool fault_already_on_screen() const;

    helix::ui::RecoveryModalPresenter& presenter_;
    ObserverGuard action_observer_;
    /// Watches ams_action_detail so a fault whose content changes while
    /// AmsAction stays ERROR re-presents instead of going stale.
    ObserverGuard detail_observer_;
    /// Expires the deferred fallback if Application drops the bridge between
    /// the ERROR edge and the tick the check runs on.
    AsyncLifetimeGuard lifetime_;
    int prev_action_ = -1;   ///< sentinel; no AmsAction maps to -1, so the first tick never edges
    bool presented_ = false; ///< true if we showed the modal for the current ERROR episode
    /// Backend operation_detail captured at the ERROR edge, for the fallback
    /// toast. Captured rather than re-read: the backend can be swapped between
    /// the edge and the deferred check.
    std::string error_detail_;
};

} // namespace helix
