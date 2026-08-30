// SPDX-License-Identifier: GPL-3.0-or-later

#include "lan_client_auth_router.h"

#include "ui_toast_manager.h"

#include "i_moonraker_client.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_error.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <chrono>

namespace helix {
namespace {

/// Handler key for register/unregister. One key across every provider's
/// notification - they all land in the same handler.
constexpr const char* NOTIFY_HANDLER_NAME = "lan_client_auth";

} // namespace

LanClientAuthRouter::LanClientAuthRouter(IMoonrakerClient* client) : client_(client) {
    if (!client_) {
        spdlog::debug("[LanAuth] no client - router is inert");
        return;
    }

    // bg_cb hops the delivery to the main thread and re-checks a generation
    // snapshot on the way in, so a notification that races our destructor is
    // dropped instead of touching freed members. Building a modal straight
    // from the WebSocket thread would be the invariant-1 crash.
    for (const std::string& method : lan_auth::notification_methods()) {
        client_->register_method_callback(
            method, NOTIFY_HANDLER_NAME,
            lifetime_.bg_cb(
                "LanClientAuthRouter::on_request",
                [this, method](const nlohmann::json& msg) { on_request(method, msg); }));
    }
    // Worth a line: "the prompt never appeared" is the whole failure mode, and
    // this is what says the screen is listening at all.
    spdlog::debug("[LanAuth] listening for {} authorization notification(s)",
                  lan_auth::notification_methods().size());
}

LanClientAuthRouter::~LanClientAuthRouter() {
    // Do not leave a "Connection Request" on screen whose buttons no longer
    // answer anything. detach() first so the teardown is not reported back as
    // the user dismissing an unanswered request. ~Modal self-guards on
    // lv_is_initialized() and defers the widget delete, so this is safe from a
    // destructor and does not delete inside a queued batch.
    if (prompt_) {
        prompt_->detach();
        prompt_.reset();
    }

    if (!client_) {
        return;
    }
    for (const std::string& method : lan_auth::notification_methods()) {
        client_->unregister_method_callback(method, NOTIFY_HANDLER_NAME);
    }
}

std::string LanClientAuthRouter::describe_request(const lan_auth::PendingRequest& req) {
    if (req.requester.empty()) {
        // The firmware named no product. Say what we do know rather than
        // inventing one - the user is being asked to trust this thing.
        return lv_tr("An app on your network is asking to connect to this printer.");
    }
    return fmt::format(lv_tr("{} is asking to connect to this printer."), req.requester);
}

std::string LanClientAuthRouter::decision_state(const nlohmann::json& response) {
    if (!response.contains("result") || !response["result"].is_object()) {
        return {};
    }
    const auto& result = response["result"];
    auto it = result.find("state");
    if (it == result.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

bool LanClientAuthRouter::decision_succeeded(const nlohmann::json& response) {
    return decision_state(response) == "success";
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

    if (lan_auth::suppressed_by_denial(req->client_id, denied_clients_,
                                       std::chrono::steady_clock::now())) {
        // A denied client never enters the firmware's registry, so each of its
        // reconnects files a fresh request. Dropping it here is the whole fix;
        // the log line is the only trace the client ever see from us.
        spdlog::info("[LanAuth] dropping request from {} - denied within the suppression window",
                     req->client_id);
        return;
    }

    spdlog::info("[LanAuth] {} authorization request from {} ({})", req->provider,
                 req->requester.empty() ? "unidentified client" : req->requester, req->client_id);

    std::string message = describe_request(*req);
    pending_ = std::move(req);

    prompt_ = std::make_unique<LanAuthPromptModal>();
    prompt_->set_handlers([this](bool approve) { decide(approve); },
                          [this] {
                              // Dismissed without an answer. The client is
                              // still waiting and will re-file, so drop the
                              // request rather than hold the gate shut.
                              spdlog::debug("[LanAuth] prompt dismissed without an answer");
                              pending_.reset();
                          });

    helix::ui::modal_configure(ModalSeverity::Warning, true, lv_tr("Allow"), lv_tr("Deny"));
    const char* attrs[] = {"title", lv_tr("Connection Request"), "message", message.c_str(),
                           nullptr};
    if (!prompt_->show(lv_screen_active(), attrs)) {
        // Nothing on screen means nothing can answer, so do not hold the gate
        // shut against the client's next attempt.
        spdlog::error("[LanAuth] could not show the prompt; leaving {} unanswered",
                      pending_->client_id);
        prompt_->detach();
        prompt_.reset();
        pending_.reset();
    }
}

void LanClientAuthRouter::decide(bool approve) {
    if (!pending_) {
        return;
    }
    lan_auth::PendingRequest req = std::move(*pending_);
    pending_.reset();

    // Suppression keys on an actual denial only (prestonbrown/helixscreen#1376):
    // an approval clears that client's record, a dismissal never writes one.
    if (approve) {
        denied_clients_.erase(req.client_id);
    } else {
        // Prune expired entries so the map stays bounded by clients denied
        // within the window, not by every client ever denied.
        const auto now = std::chrono::steady_clock::now();
        for (auto it = denied_clients_.begin(); it != denied_clients_.end();) {
            if (now - it->second >= lan_auth::denial_suppression_window) {
                it = denied_clients_.erase(it);
            } else {
                ++it;
            }
        }
        denied_clients_[req.client_id] = now;
    }

    std::optional<lan_auth::Decision> decision = lan_auth::build_decision(req, approve);
    if (!decision || !client_) {
        return;
    }

    spdlog::info("[LanAuth] {} {} for {}", approve ? "approving" : "denying", req.client_id,
                 req.provider);

    // Both continuations land on the WebSocket thread and both raise a toast,
    // which is an LVGL call - bg_cb hops them to the main thread and drops
    // them if this router died while the RPC was in flight.
    client_->send_jsonrpc(
        decision->method, decision->params,
        lifetime_.bg_cb("LanClientAuthRouter::decision_reply",
                        [approve](const nlohmann::json& response) {
                            // The firmware reports its own outcome in the
                            // result body; a transport-level success does not
                            // mean it minted anything.
                            if (LanClientAuthRouter::decision_succeeded(response)) {
                                if (approve) {
                                    ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                                  lv_tr("Device connected"));
                                }
                                return;
                            }
                            spdlog::error("[LanAuth] firmware refused the decision: state='{}'",
                                          LanClientAuthRouter::decision_state(response));
                            ToastManager::instance().show(
                                ToastSeverity::ERROR,
                                lv_tr("Could not answer the connection request"));
                        }),
        lifetime_.bg_cb("LanClientAuthRouter::decision_failed", [](const MoonrakerError& err) {
            spdlog::error("[LanAuth] decision RPC failed: {}", err.message);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Could not answer the connection request"));
        }));
}

} // namespace helix
