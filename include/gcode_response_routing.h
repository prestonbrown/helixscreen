// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

class IMoonrakerAPI; // NAMESPACE_OK: the interface is declared at global scope

namespace helix {
class ActionPromptManager;
class AmsErrorBridge;
class GcodeErrorRouter;
class GcodeNarrationRouter;
class IMoonrakerClient;
class LanClientAuthRouter;
} // namespace helix
namespace helix::ui {
class ActionPromptModal;
class RecoveryModalPresenter;
} // namespace helix::ui

namespace helix {

/// Everything that turns a printer's G-code console output and its AMS action state into UI:
/// the Klipper action:prompt system, the CRITICAL recovery modal presenter, the `!!`/`Error:`
/// router, the toolchange narration router, the LAN pairing router, the AMS error bridge, and
/// the layer-tracking fallback. One bundle per printer scope: attach() builds it against that
/// scope's client, and the two detach calls take it down in the order the client's lifetime
/// requires.
class GcodeResponseRouting {
  public:
    GcodeResponseRouting();
    ~GcodeResponseRouting();

    GcodeResponseRouting(const GcodeResponseRouting&) = delete;
    GcodeResponseRouting& operator=(const GcodeResponseRouting&) = delete;

    /// Builds the seven objects and registers the `action_prompt_manager` and `layer_tracker`
    /// `notify_gcode_response` handlers. `client` must be non-null; `api` may be null.
    void attach(IMoonrakerClient* client, IMoonrakerAPI* api);

    /// First detach, with the client still alive: unregisters both `notify_gcode_response`
    /// handlers, clears AmsState's injected-line callback, and destroys the prompt modal and
    /// manager. `client` may be null when the printer scope never connected.
    void detach_handlers(IMoonrakerClient* client);

    /// Second detach, after ObserverGuard::invalidate_all() and before the client is
    /// destroyed: the routers unregister from the client in their destructors. Destroys them
    /// in dependency order, the presenter last because the error router and the bridge hold
    /// references into it.
    void release_routers();

    /// Null until attach() and after detach_handlers().
    [[nodiscard]] ActionPromptManager* action_prompt_manager() const {
        return m_action_prompt_manager.get();
    }

  private:
    std::unique_ptr<ActionPromptManager> m_action_prompt_manager;
    std::unique_ptr<ui::ActionPromptModal> m_action_prompt_modal;

    // Declared before the error router and the bridge so it destructs after them.
    std::unique_ptr<ui::RecoveryModalPresenter> m_recovery_presenter;

    // Surfaces `!!` / `Error:` lines as modals or toasts and replays the latest stored error
    // on (re)connect. Owns the notify_gcode_response and connected-observer registrations.
    std::unique_ptr<GcodeErrorRouter> m_gcode_error_router;

    // Routes `//` toolchange narration to the active AMS backend's step model. Owns a
    // separate notify_gcode_response handler key from the error router.
    std::unique_ptr<GcodeNarrationRouter> m_gcode_narration_router;

    // Answers the firmware's LAN pairing prompt on the touchscreen. Owns the authorization
    // notification registrations.
    std::unique_ptr<LanClientAuthRouter> m_lan_client_auth_router;

    // Routes AmsAction::ERROR edges from AmsState's action subject to the presenter.
    std::unique_ptr<AmsErrorBridge> m_ams_error_bridge;
};

} // namespace helix
