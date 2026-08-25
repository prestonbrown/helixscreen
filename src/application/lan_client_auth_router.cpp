// SPDX-License-Identifier: GPL-3.0-or-later

#include "lan_client_auth_router.h"

#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include <spdlog/spdlog.h>

#include <spdlog/fmt/fmt.h>

#include "lvgl/src/others/translation/lv_translation.h"

namespace helix {
namespace {

/// Handler key for register/unregister. One key across every provider's
/// notification - they all land in the same handler.
constexpr const char* NOTIFY_HANDLER_NAME = "lan_client_auth";

/// The router that owns the prompt currently on screen.
///
/// Application owns at most one router but REPLACES it on a printer switch,
/// and a modal callback receives a raw void* that a replaced router would
/// leave dangling. Every callback re-checks its user_data against this, so a
/// click - or the DELETE that arrives after the exit animation finishes -
/// landing on a prompt whose owner is gone does nothing instead of writing
/// through freed memory.
LanClientAuthRouter* g_active = nullptr;

} // namespace

LanClientAuthRouter::LanClientAuthRouter(IMoonrakerClient* client) : client_(client) {
    g_active = this;
    if (!client_) {
        return;
    }

    // bg_cb hops the delivery to the main thread and re-checks a generation
    // snapshot on the way in, so a notification that races our destructor is
    // dropped instead of touching freed members. Building a modal straight
    // from the WebSocket thread would be the invariant-1 crash.
    for (const std::string& method : lan_auth::notification_methods()) {
        client_->register_method_callback(
            method, NOTIFY_HANDLER_NAME,
            lifetime_.bg_cb("LanClientAuthRouter::on_request",
                            [this, method](const nlohmann::json& msg) {
                                on_request(method, msg);
                            }));
    }
    // Worth a line: "the prompt never appeared" is the whole failure mode, and
    // this is what says the screen is listening at all.
    spdlog::debug("[LanAuth] listening for {} authorization notification(s)",
                  lan_auth::notification_methods().size());
}

LanClientAuthRouter::~LanClientAuthRouter() {
    // Clear the guard BEFORE hiding: Modal::hide animates the exit, so the
    // prompt's DELETE fires after this destructor has already returned.
    if (g_active == this) {
        g_active = nullptr;
    }
    // Do not leave a "Connection Request" on screen whose buttons no longer
    // answer anything. Modal::hide self-guards on lv_is_initialized() and
    // defers the actual delete to the exit animation, so this is safe from a
    // destructor and does not delete inside a queued batch.
    dismiss_prompt();

    if (!client_) {
        return;
    }
    for (const std::string& method : lan_auth::notification_methods()) {
        client_->unregister_method_callback(method, NOTIFY_HANDLER_NAME);
    }
}

LanClientAuthRouter* LanClientAuthRouter::from_event(lv_event_t* e) {
    auto* self = static_cast<LanClientAuthRouter*>(lv_event_get_user_data(e));
    return (self && self == g_active) ? self : nullptr;
}

std::string LanClientAuthRouter::describe_request(const lan_auth::PendingRequest& req) {
    if (req.requester.empty()) {
        // The firmware named no product. Say what we do know rather than
        // inventing one - the user is being asked to trust this thing.
        return lv_tr("An app on your network is asking to connect to this printer.");
    }
    return fmt::format(lv_tr("{} is asking to connect to this printer."), req.requester);
}

void LanClientAuthRouter::on_request(const std::string& method, const nlohmann::json& msg) {
    std::optional<lan_auth::PendingRequest> req = lan_auth::parse_request(method, msg);
    if (!req) {
        return;
    }

    if (pending_) {
        // A client that has not been answered yet re-files on every connection
        // attempt, so repeats are the norm rather than an error. Refresh the
        // request in place: the app_id changes per attempt and the newest one
        // is the one still listening for an answer.
        if (pending_->client_id == req->client_id) {
            spdlog::debug("[LanAuth] {} re-filed while its prompt was open", req->client_id);
            pending_ = std::move(req);
            return;
        }
        // A different client while one prompt is up. Answering two at once
        // would need a modal queue for a case that needs two people standing
        // at one printer; log it so it is visible if it ever happens for real.
        spdlog::info("[LanAuth] ignoring request from {} - {} is still awaiting an answer",
                     req->client_id, pending_->client_id);
        return;
    }

    spdlog::info("[LanAuth] {} authorization request from {} ({})", req->provider,
                 req->requester.empty() ? "unidentified client" : req->requester,
                 req->client_id);

    std::string message = describe_request(*req);
    pending_ = std::move(req);

    prompt_ = helix::ui::modal_show_confirmation(lv_tr("Connection Request"), message.c_str(),
                                                ModalSeverity::Warning, lv_tr("Allow"), on_allow,
                                                on_deny, this, lv_tr("Deny"));
    if (!prompt_) {
        // Nothing on screen means nothing can answer, so do not hold the gate
        // shut against the client's next attempt.
        spdlog::error("[LanAuth] could not show the prompt; leaving {} unanswered",
                      pending_->client_id);
        pending_.reset();
        return;
    }
    // The prompt can go away without either button being pressed:
    // Modal::rebuild_top hides a non-rebuildable dialog on a breakpoint or
    // theme change. A pending_ left set behind one would silently refuse every
    // later request as "one is already awaiting an answer", so track the
    // teardown rather than assume a button caused it.
    lv_obj_add_event_cb(prompt_, on_prompt_deleted, LV_EVENT_DELETE, this);
}

void LanClientAuthRouter::dismiss_prompt() {
    if (!prompt_) {
        return;
    }
    lv_obj_t* prompt = prompt_;
    prompt_ = nullptr;
    // Hide by handle, not Modal::get_top(): the top modal is ours in every
    // path that reaches here today, but nothing structurally guarantees it.
    Modal::hide(prompt);
}

void LanClientAuthRouter::decide(bool approve) {
    if (!pending_) {
        return;
    }
    lan_auth::PendingRequest req = std::move(*pending_);
    pending_.reset();

    std::optional<lan_auth::Decision> decision = lan_auth::build_decision(req, approve);
    if (!decision || !client_) {
        return;
    }

    spdlog::info("[LanAuth] {} {} for {}", approve ? "approving" : "denying", req.client_id,
                 req.provider);

    // Both continuations land on the WebSocket thread and both raise a toast,
    // which is an LVGL call — bg_cb hops them to the main thread and drops
    // them if this router died while the RPC was in flight.
    client_->send_jsonrpc(
        decision->method, decision->params,
        lifetime_.bg_cb("LanClientAuthRouter::decision_reply",
                        [approve](const nlohmann::json& response) {
                            // The firmware reports its own outcome in the
                            // result body; a transport-level success does not
                            // mean it minted anything.
                            std::string state;
                            if (response.contains("result") &&
                                response["result"].is_object()) {
                                const auto& result = response["result"];
                                auto it = result.find("state");
                                if (it != result.end() && it->is_string()) {
                                    state = it->get<std::string>();
                                }
                            }
                            if (state == "success") {
                                if (approve) {
                                    ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                                  lv_tr("Device connected"));
                                }
                                return;
                            }
                            spdlog::error("[LanAuth] firmware refused the decision: state='{}'",
                                          state);
                            ToastManager::instance().show(
                                ToastSeverity::ERROR,
                                lv_tr("Could not answer the connection request"));
                        }),
        lifetime_.bg_cb("LanClientAuthRouter::decision_failed",
                        [](const MoonrakerError& err) {
                            spdlog::error("[LanAuth] decision RPC failed: {}", err.message);
                            ToastManager::instance().show(
                                ToastSeverity::ERROR,
                                lv_tr("Could not answer the connection request"));
                        }));
}

void LanClientAuthRouter::on_allow(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LanAuth] allow_cb");
    if (LanClientAuthRouter* self = from_event(e)) {
        self->dismiss_prompt();
        self->decide(true);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void LanClientAuthRouter::on_deny(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LanAuth] deny_cb");
    if (LanClientAuthRouter* self = from_event(e)) {
        self->dismiss_prompt();
        // Answering "no" matters as much as answering "yes": it is what turns
        // the client's silent wait into an immediate, explicit refusal.
        self->decide(false);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void LanClientAuthRouter::on_prompt_deleted(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LanAuth] prompt_deleted_cb");
    LanClientAuthRouter* self = from_event(e);
    // A cleared prompt_ means decide() already took the request; a mismatched
    // one means this DELETE belongs to a prompt we have since replaced.
    if (self && self->prompt_ == lv_event_get_target_obj(e)) {
        // Dismissed without an answer. The client is still waiting and will
        // re-file, so drop the request rather than hold the gate shut.
        spdlog::debug("[LanAuth] prompt dismissed without an answer");
        self->prompt_ = nullptr;
        self->pending_.reset();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
