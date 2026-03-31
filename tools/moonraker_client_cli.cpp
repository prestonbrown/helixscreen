// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_client_cli.cpp
 * @brief Implementation of minimal Moonraker client for CLI tools
 */

#include "moonraker_client_cli.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>

namespace helix {
namespace cli {

MoonrakerClient::MoonrakerClient() {
    ws_client_ = std::make_unique<hv::WebSocketClient>();
    setup_callbacks();
}

MoonrakerClient::MoonrakerClient(std::shared_ptr<hv::EventLoop> loop)
    : ws_client_(std::make_unique<hv::WebSocketClient>(loop)) {
    setup_callbacks();
}

MoonrakerClient::~MoonrakerClient() {
    disconnect();
}

void MoonrakerClient::setup_callbacks() {
    // Configure WebSocket callbacks
    ws_client_->onopen = [this]() { onOpen(); };

    ws_client_->onclose = [this]() { onClose(); };

    ws_client_->onmessage = [this](const std::string& msg) { onMessage(msg); };

    // Configure ping interval
    ws_client_->setPingInterval(keepalive_interval_ms_ / 1000);
}

void MoonrakerClient::configure_timeouts(int connect_timeout_ms,
                                         int, // read_timeout_ms (not available in WebSocketClient)
                                         int, // write_timeout_ms (not available)
                                         int keepalive_interval_ms, int keepalive_timeout_ms) {
    connect_timeout_ms_ = connect_timeout_ms;
    keepalive_interval_ms_ = keepalive_interval_ms;
    keepalive_timeout_ms_ = keepalive_timeout_ms;

    // Apply to underlying WebSocketClient
    ws_client_->setConnectTimeout(connect_timeout_ms);
    ws_client_->setPingInterval(keepalive_interval_ms / 1000);
    (void)keepalive_timeout_ms; // Not directly configurable
}

int MoonrakerClient::connect(const char* url, StateCallback on_connect,
                             StateCallback on_disconnect) {
    on_connect_ = on_connect;
    on_disconnect_ = on_disconnect;

    spdlog::debug("Connecting to {}", url);
    set_state(ConnectionState::Connecting);

    return ws_client_->open(url);
}

void MoonrakerClient::disconnect() {
    spdlog::debug("Disconnecting");
    set_state(ConnectionState::Disconnected);

    // Clear pending requests
    pending_requests_.clear();

    ws_client_->close();
}

void MoonrakerClient::set_state(ConnectionState state) {
    state_ = state;
}

void MoonrakerClient::onOpen() {
    spdlog::debug("WebSocket connected");
    set_state(ConnectionState::Connected);
    if (on_connect_) {
        on_connect_();
    }
}

void MoonrakerClient::onClose() {
    spdlog::debug("WebSocket closed");
    set_state(ConnectionState::Disconnected);

    // Notify error to all pending requests
    MoonrakerError error("Connection closed", "disconnected");
    for (auto& [id, req] : pending_requests_) {
        if (req.on_error) {
            req.on_error(error);
        }
    }
    pending_requests_.clear();

    if (on_disconnect_) {
        on_disconnect_();
    }
}

void MoonrakerClient::onMessage(const std::string& msg) {
    try {
        auto response = nlohmann::json::parse(msg);
        dispatch_response(response);
    } catch (const nlohmann::json::parse_error& e) {
        spdlog::warn("Failed to parse JSON response: {}", e.what());
    }
}

void MoonrakerClient::dispatch_response(const nlohmann::json& response) {
    // Extract request ID
    uint64_t request_id = 0;
    if (response.contains("id")) {
        request_id = response["id"].get<uint64_t>();
    }

    // Find pending request
    auto it = pending_requests_.find(request_id);
    if (it == pending_requests_.end()) {
        // Notification or unknown response
        spdlog::trace("Received response for unknown request {}", request_id);
        return;
    }

    PendingRequest req = std::move(it->second);
    pending_requests_.erase(it);

    // Check for error
    if (response.contains("error")) {
        const auto& error = response["error"];
        std::string message =
            error.contains("message") ? error["message"].get<std::string>() : "Unknown error";
        std::string code =
            error.contains("code") ? std::to_string(error["code"].get<int>()) : "unknown";

        if (req.on_error) {
            req.on_error(MoonrakerError(message, code));
        }
        return;
    }

    // Success
    if (req.on_success) {
        req.on_success(response);
    }
}

void MoonrakerClient::send_jsonrpc(const std::string& method) {
    send_jsonrpc(method, nlohmann::json::object(), nullptr, nullptr);
}

void MoonrakerClient::send_jsonrpc(const std::string& method, const nlohmann::json& params) {
    send_jsonrpc(method, params, nullptr, nullptr);
}

void MoonrakerClient::send_jsonrpc(const std::string& method, const nlohmann::json& params,
                                   JsonCallback on_success, ErrorCallback on_error,
                                   unsigned int timeout_ms, bool require_result) {
    // Build JSON-RPC request
    uint64_t request_id = next_request_id_++;

    nlohmann::json request = {{"jsonrpc", "2.0"}, {"method", method}, {"id", request_id}};

    if (!params.is_null() && !params.empty()) {
        request["params"] = params;
    }

    // Store pending request
    if (on_success || on_error) {
        auto now = std::chrono::steady_clock::now();
        auto timeout_at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() +
            timeout_ms);

        pending_requests_[request_id] = {on_success, on_error, timeout_at, require_result};
    }

    // Send
    std::string json_str = request.dump();
    spdlog::debug("Sending JSON-RPC: {}", json_str);

    ws_client_->send(json_str);
}

} // namespace cli
} // namespace helix
