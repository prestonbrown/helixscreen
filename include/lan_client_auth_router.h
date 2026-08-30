// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include "async_lifetime_guard.h"
#include "lan_client_authorization.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix {

class IMoonrakerClient;

/// The "may this client in?" prompt.
///
/// A Modal subclass rather than a modal_confirm() call, because the
/// owner here outlives no dialog reliably: Application replaces the router on a
/// printer switch, and a click - or the DELETE that arrives after the exit
/// animation - could otherwise land on a prompt whose owner is gone. The Modal
/// base already solves exactly that when it hides: it invalidates its async
/// lifetime, clears button user_data and disables clicks across the dialog tree
/// (ui_modal.cpp:815-835). The static helper skips all of it, because it pushes
/// with no owner, which is why the hand-rolled alternative was a global.
class LanAuthPromptModal : public Modal {
  public:
    /// Answer chosen by the user. Fires before the dialog starts hiding.
    using DecisionCallback = std::function<void(bool approve)>;
    /// The dialog went away WITHOUT an answer - Modal::rebuild_top hides a
    /// non-rebuildable dialog when XML hot reload rebuilds the active views
    /// (dev builds only), and a backdrop tap or ESC does it anywhere. The request is
    /// still unanswered and the client will re-file, so the owner must drop it
    /// rather than hold its gate shut.
    using DismissCallback = std::function<void()>;

    const char* get_name() const override {
        return "LAN Auth";
    }
    const char* component_name() const override {
        return "modal_dialog";
    }

    void set_handlers(DecisionCallback on_decision, DismissCallback on_dismiss) {
        on_decision_ = std::move(on_decision);
        on_dismiss_ = std::move(on_dismiss);
    }

    /// Drop the handlers without answering. Used by the owner's destructor so
    /// tearing the prompt down is not mistaken for the user dismissing it.
    void detach() {
        on_decision_ = nullptr;
        on_dismiss_ = nullptr;
    }

  protected:
    void on_show() override {
        wire_ok_button("btn_primary");
        wire_cancel_button("btn_secondary");
    }
    void on_ok() override {
        answer(true);
    }
    void on_cancel() override {
        answer(false);
    }
    /// Fires however the dialog goes away, including both button paths, so it
    /// has to distinguish the two - only an unanswered teardown is a dismissal.
    void on_hide() override {
        if (!answered_ && on_dismiss_) {
            on_dismiss_();
        }
    }

  private:
    void answer(bool approve) {
        answered_ = true;
        if (on_decision_) {
            on_decision_(approve);
        }
        hide();
    }

    bool answered_ = false;
    DecisionCallback on_decision_;
    DismissCallback on_dismiss_;
};

/// Answers the firmware's "may this client in?" question with the touchscreen.
///
/// On printers whose firmware brokers LAN pairing (see
/// lan_client_authorization.h) the screen IS the approval step. This router
/// subscribes to every provider's notification, shows the request as a
/// confirmation modal, and sends the user's answer back. Nothing here names a
/// firmware; the shapes all come from helix::lan_auth.
///
/// One request is presented at a time. A client that retries while its modal
/// is still up - Orca re-files roughly every time it reconnects - refreshes
/// the pending request in place instead of stacking a second dialog.
///
/// Lifetime: owned by Application, which tears this down and only then builds a
/// replacement on a printer switch (tear_down_printer_state() precedes
/// init_action_prompt()). The prompt is owned outright, so it cannot outlive
/// the router. The client pointer is not owned and must outlive this router.
class LanClientAuthRouter {
  public:
    /// @param client may be nullptr (test/mock builds), in which case the
    ///        router registers nothing and is inert.
    explicit LanClientAuthRouter(IMoonrakerClient* client);
    ~LanClientAuthRouter();

    LanClientAuthRouter(const LanClientAuthRouter&) = delete;
    LanClientAuthRouter& operator=(const LanClientAuthRouter&) = delete;

    /// The message body shown for @p req. Static and free of router state, so
    /// the wording - including the fallback for a client the firmware could
    /// not name - is testable without standing up a client or a screen. Still
    /// goes through lv_tr(), so its tests take the LVGL fixture.
    static std::string describe_request(const lan_auth::PendingRequest& req);

    /// The firmware's own outcome for a decision, or empty when the reply
    /// carries none. Split out so the failure log can name the state.
    static std::string decision_state(const nlohmann::json& response);

    /// Whether the firmware's reply to a decision reports that it actually did
    /// something. A transport-level success does not mean it minted anything -
    /// the component answers {"state": "error"} for a request it rejected, and
    /// that has to read as a failure rather than a paired client.
    static bool decision_succeeded(const nlohmann::json& response);

  private:
    /// Notification handler. Runs on the MAIN thread: registration wraps it in
    /// lifetime_.bg_cb, which hops off the WebSocket thread and drops the call
    /// if this router died in between.
    void on_request(const std::string& method, const nlohmann::json& msg);

    /// Send the answer and clear the pending request.
    void decide(bool approve);

    IMoonrakerClient* client_;

    /// The request currently on screen, if any. Doubles as the dedup gate.
    std::optional<lan_auth::PendingRequest> pending_;

    /// The prompt showing pending_. Owned, so it dies with this router and its
    /// callbacks can hold a plain `this`.
    std::unique_ptr<LanAuthPromptModal> prompt_;

    /// Guards the WebSocket-thread delivery: unregister_method_callback in
    /// our dtor does NOT block an in-flight invocation, so the handler has to
    /// be able to notice that `this` is gone.
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
