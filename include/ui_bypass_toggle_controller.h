// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "ams_types.h"

class AmsBackend;

namespace helix::ui {

/// Widgets-free bypass toggle policy shared by the AMS sidebar and the home
/// BypassWidget. Owns the pending-enable state machine (unload-first
/// chaining, #1229 discipline) and the print-active refusal.
///
/// The pending unload->enable chain completes on the backend's TERMINAL STATE,
/// not on an observed action edge. The chain asks should_unload_before_bypass()
/// to arm and re-asks the same question to settle, so arming and completing
/// cannot drift, and the answer does not depend on which edges a status sync
/// happened to sample: AmsState publishes ams_action from the backend's current
/// state, so an unload that finishes between two syncs publishes IDLE->IDLE and
/// notifies nobody (prestonbrown/helixscreen#1512). Backends also end an unload
/// on an action other than UNLOADING - a K2 Plus ends it CUTTING->IDLE - which
/// no single edge predicate covers.
///
/// The wake feed is ams_data_revision, bumped after every backend-event sync
/// and therefore always notifying. It is armed when the chain starts and torn
/// down when the chain settles (lane clear enables, ERROR disarms) or is
/// cancelled - a settled controller stays detached.
class BypassToggleController {
  public:
    BypassToggleController() = default;
    ~BypassToggleController();

    BypassToggleController(const BypassToggleController&) = delete;
    BypassToggleController& operator=(const BypassToggleController&) = delete;

    /// User asked to flip bypass. Runs every guard, performs the backend
    /// call (or arms the unload→enable chain), toasts outcomes.
    void toggle();

    /// Re-evaluate a pending chain against the backend's CURRENT state: enable
    /// once the backend is idle and no longer wants the unload that armed the
    /// chain, disarm on a reported ERROR, otherwise keep waiting. Idempotent,
    /// so a caller may run it as often as it likes. Still public for direct
    /// unit-driving; the production feed is the controller's own
    /// ams_data_revision observer. Returns true if the chain settled.
    bool poll_pending_engage();

    /// Abort any pending chain (owner is going away / context reset).
    void cancel_pending();

    [[nodiscard]] bool pending_enable() const {
        return pending_bypass_enable_;
    }

  private:
    void enable_now(AmsBackend* backend);
    /// Subscribe to ams_data_revision for the pending chain (idempotent).
    void arm_backend_observer();
    /// Detach the revision observer (chain settled or cancelled).
    void disarm_backend_observer();

    bool pending_bypass_enable_ = false;
    ObserverGuard backend_observer_;
};

} // namespace helix::ui
