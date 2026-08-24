// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_event_routing.h"

namespace helix {

MoonrakerEventDecision decide_moonraker_event(MoonrakerEventType type, bool is_error,
                                              bool wizard_active, bool modal_active) {
    // Recovery-dialog events are routed regardless of is_error or the wizard; a
    // disconnected or shut-down Klippy is not a toast that can be suppressed as
    // startup noise.
    if (type == MoonrakerEventType::KLIPPY_DISCONNECTED) {
        return {MoonrakerEventRoute::RecoveryDisconnected, nullptr,
                MoonrakerEventSuppression::None};
    }
    if (type == MoonrakerEventType::KLIPPY_SHUTDOWN) {
        return {MoonrakerEventRoute::RecoveryShutdown, nullptr, MoonrakerEventSuppression::None};
    }

    if (is_error) {
        // Deferred discovery = Klippy not yet in a gate-acceptable state. Always
        // transient: notify_klippy_ready/shutdown will retry, and the UI already
        // surfaces connection state through PrinterStatusIcon.
        if (type == MoonrakerEventType::DISCOVERY_DEFERRED) {
            return {MoonrakerEventRoute::Ignore, nullptr,
                    MoonrakerEventSuppression::DiscoveryDeferred};
        }
        if (type == MoonrakerEventType::CONNECTION_FAILED) {
            // The wizard owns the screen, and its Connection step is the UI
            // that collects the address and reports the result inline — this
            // prompt is a second, competing host-entry dialog on top of it. On
            // a fresh install there is no saved host at all, so the address
            // that "failed" is only the 127.0.0.1 default the wizard exists to
            // replace, and on a standalone display the message even claims
            // Klipper runs on this machine. Bundle L53W5PKG: pushed over the
            // Language step and left sitting there for 16.5 minutes.
            //
            // Suppressed, not degraded to a toast like the modal case below:
            // there the user is doing something else and needs to learn the
            // connection failed, here they are already in the flow that fixes
            // it.
            if (wizard_active) {
                return {MoonrakerEventRoute::Ignore, nullptr, MoonrakerEventSuppression::Wizard};
            }
            // Never steal an open modal. This event is latched and fires 60 s
            // after startup, which on an unreachable printer is exactly when the
            // user is in Settings > Network typing a WiFi password to fix it —
            // and the prompt lands on top of that keyboard (bundle 865DXBQ7:
            // pushed at stack depth 2, the password re-entered from scratch
            // afterwards). Degrade to a toast: same information, no focus theft,
            // no dialog to dismiss before getting back to the field. The
            // disconnected status icon still carries the state afterwards.
            if (modal_active) {
                return {MoonrakerEventRoute::ErrorToast, "Connection Failed",
                        MoonrakerEventSuppression::None};
            }
            return {MoonrakerEventRoute::ConnectionFailedModal, "Connection Failed",
                    MoonrakerEventSuppression::None};
        }
        return {MoonrakerEventRoute::ErrorToast,
                type == MoonrakerEventType::RPC_ERROR ? "Request Failed" : "Printer Error",
                MoonrakerEventSuppression::None};
    }

    // Non-error: klippy becoming ready is an internal lifecycle event, not a
    // notification. A klippy-down path either shows the recovery dialog (the
    // branches above) or is suppressed by the flow that initiated the restart
    // (SAVE_CONFIG, power/host toggles); either way the klippy_state READY
    // observer in ui_emergency_stop.cpp carries the completion signal -
    // dialog dismissal, or the expected-restart success toast when the dialog
    // was suppressed. Routing this event as a notification instead would
    // write a history row for good news. Unconditional, because any
    // boot-anchored grace window covers only the first ready.
    if (type == MoonrakerEventType::KLIPPY_READY) {
        return {MoonrakerEventRoute::Ignore, nullptr, MoonrakerEventSuppression::KlippyReady};
    }

    // Non-error: the wizard owns the screen during first connection, so a
    // "reconnected" toast there is wrong, not merely noisy.
    if (wizard_active) {
        return {MoonrakerEventRoute::Ignore, nullptr, MoonrakerEventSuppression::Wizard};
    }
    return {MoonrakerEventRoute::WarningToast, nullptr, MoonrakerEventSuppression::None};
}

} // namespace helix
