// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_events.h"

namespace helix {

/**
 * @brief What MoonrakerManager should do with an incoming event
 *
 * One route per branch of the old inline handler, so the mapping is total and
 * testable without a client, a UI, or an event loop.
 */
enum class MoonrakerEventRoute {
    Ignore,                ///< Suppressed — caller logs the reason, shows nothing
    RecoveryDisconnected,  ///< Unified recovery dialog, DISCONNECTED reason
    RecoveryShutdown,      ///< Unified recovery dialog, SHUTDOWN reason
    ConnectionFailedModal, ///< Change-Address prompt (not a plain OK-only alert)
    ErrorToast,            ///< NOTIFY_ERROR_T with the decision's title
    WarningToast           ///< NOTIFY_WARNING
};

/// Why an event was suppressed. Only meaningful when route == Ignore; lets the
/// caller log the same messages the inline handler did.
enum class MoonrakerEventSuppression {
    None,
    DiscoveryDeferred, ///< Klippy not yet gate-acceptable; retried automatically
    Wizard,            ///< First connection during the setup wizard, not a reconnect
    KlippyReady        ///< Klippy-ready is an internal lifecycle event; the recovery UI owns
                       ///< the user signal
};

/**
 * @brief Routing decision for one Moonraker event
 *
 * `title_tag` is the UNTRANSLATED source string. Translating it is the caller's
 * job and must happen on the main thread — lv_translation_get() reads the
 * file-scope selected_lang with no synchronisation, and
 * lv_translation_set_language() frees and replaces it from the main thread, so
 * an off-thread lv_tr() is a read of freed memory, not just a torn read (#1219).
 */
struct MoonrakerEventDecision {
    MoonrakerEventRoute route;
    const char* title_tag; ///< Untranslated; nullptr when the route needs no title
    MoonrakerEventSuppression suppressed_because;
};

/**
 * @brief Decide how to present a Moonraker event
 *
 * Pure and total: no LVGL, no clock, no globals. The caller samples the wizard
 * and modal state and passes them in.
 *
 * @param type          Event type
 * @param is_error      MoonrakerEvent::is_error
 * @param wizard_active Setup wizard is on screen; suppresses non-error toasts and the
 *                      connection-failed prompt, never the recovery dialogs
 * @param modal_active  A modal is already open (see the CONNECTION_FAILED note below)
 */
MoonrakerEventDecision decide_moonraker_event(MoonrakerEventType type, bool is_error,
                                              bool wizard_active, bool modal_active = false);

} // namespace helix
