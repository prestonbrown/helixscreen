// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "lan_client_authorization.h"
#include "lvgl.h"

#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix {

class IMoonrakerClient;

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
/// Lifetime: owned by Application. Registers callbacks in the ctor and
/// unregisters them in the dtor; the client pointer is not owned and must
/// outlive this router.
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

  private:
    /// Notification handler. Runs on the MAIN thread: registration wraps it in
    /// lifetime_.bg_cb, which hops off the WebSocket thread and drops the call
    /// if this router died in between.
    void on_request(const std::string& method, const nlohmann::json& msg);

    /// Send the answer and clear the pending request.
    void decide(bool approve);

    /// Take down the prompt, if one is up. Clears the handle FIRST so the
    /// LV_EVENT_DELETE that follows the exit animation is a no-op instead of
    /// mistaking a deliberate dismissal for an unanswered one.
    void dismiss_prompt();

    /// Modal callbacks. lv_event_cb_t is a plain function pointer, so these
    /// take `this` through user_data and re-check it against the active
    /// instance before touching it - see the note on the dtor.
    static void on_allow(lv_event_t* e);
    static void on_deny(lv_event_t* e);
    /// Fires however the prompt goes away - button, Modal::rebuild_top on a
    /// breakpoint change, screen teardown. Clears the pending request so a
    /// dismissal that answered nothing cannot wedge the gate shut.
    static void on_prompt_deleted(lv_event_t* e);

    /// Resolve the router behind a modal callback, or nullptr if it is gone.
    static LanClientAuthRouter* from_event(lv_event_t* e);

    IMoonrakerClient* client_;

    /// The request currently on screen, if any. Doubles as the dedup gate.
    std::optional<lan_auth::PendingRequest> pending_;

    /// The prompt showing pending_, so the dtor can take it down with it.
    lv_obj_t* prompt_ = nullptr;

    /// Guards the WebSocket-thread delivery: unregister_method_callback in
    /// our dtor does NOT block an in-flight invocation, so the handler has to
    /// be able to notice that `this` is gone.
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
