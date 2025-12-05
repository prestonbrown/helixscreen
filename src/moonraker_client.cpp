// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 * Based on GuppyScreen WebSocket client implementation.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "moonraker_client.h"

#include "ui_error_reporting.h"

#include "app_globals.h"
#include "printer_state.h"

#include <algorithm> // For std::sort in MCU query handling

using namespace hv;

// Anonymous namespace for file-scoped state
namespace {
// Rate limiting flags for reconnection notifications
std::atomic<bool> g_already_notified_max_attempts{false};
std::atomic<bool> g_already_notified_disconnect{false};

// Reset notification flags on successful connection
void reset_notification_flags() {
    g_already_notified_max_attempts.store(false);
    g_already_notified_disconnect.store(false);
}
} // namespace

MoonrakerClient::MoonrakerClient(EventLoopPtr loop)
    : WebSocketClient(loop), request_id_(0), was_connected_(false),
      connection_state_(ConnectionState::DISCONNECTED),
      connection_timeout_ms_(10000) // Default 10 seconds
      ,
      default_request_timeout_ms_(30000) // Default 30 seconds
      ,
      keepalive_interval_ms_(10000) // Default 10 seconds
      ,
      reconnect_min_delay_ms_(200) // Default 200ms
      ,
      reconnect_max_delay_ms_(2000) { // Default 2 seconds
}

MoonrakerClient::~MoonrakerClient() {
    // Set destroying flag FIRST - during static destruction, mutexes may already be invalid.
    // This flag prevents any callbacks from firing even if methods check it.
    is_destroying_.store(true);

    // During static destruction (via exit()), mutexes may be in an invalid state.
    // DO NOT lock any mutexes or call methods that lock mutexes.
    // Just clear callbacks directly - no other thread should be accessing during destruction.
    state_change_callback_ = nullptr;

    // Close WebSocket connection without complex cleanup that might lock mutexes.
    // The base class WebSocketClient destructor will handle socket cleanup.
    onopen = []() {};
    onmessage = [](const std::string&) {};
    onclose = []() {};

    // Note: We skip cleanup_pending_requests() and disconnect() because they use locks.
    // Any pending requests will be abandoned - this is acceptable during shutdown.
}

void MoonrakerClient::set_connection_state(ConnectionState new_state) {
    ConnectionState old_state = connection_state_.exchange(new_state);

    if (old_state != new_state) {
        const char* state_names[] = {"DISCONNECTED", "CONNECTING", "CONNECTED", "RECONNECTING",
                                     "FAILED"};
        spdlog::debug("[Moonraker Client] Connection state: {} -> {}",
                      state_names[static_cast<int>(old_state)],
                      state_names[static_cast<int>(new_state)]);

        // Handle state-specific logic
        if (new_state == ConnectionState::RECONNECTING) {
            reconnect_attempts_++;
            if (max_reconnect_attempts_ > 0 && reconnect_attempts_ >= max_reconnect_attempts_) {
                spdlog::error("[Moonraker Client] Max reconnect attempts ({}) exceeded",
                              max_reconnect_attempts_);

                // Emit event only once during reconnect sequence
                if (!g_already_notified_max_attempts.load()) {
                    emit_event(MoonrakerEventType::CONNECTION_FAILED,
                               fmt::format("Unable to reach printer after {} attempts. "
                                           "Check power and network connection.",
                                           max_reconnect_attempts_),
                               true);
                    g_already_notified_max_attempts.store(true);
                }

                set_connection_state(ConnectionState::FAILED);
                return;
            }
        } else if (new_state == ConnectionState::CONNECTED) {
            reconnect_attempts_ = 0; // Reset on successful connection
        }

        // Copy callback under lock to prevent race with destructor clearing it
        // We invoke OUTSIDE the lock so we don't hold mutex during LVGL operations
        std::function<void(ConnectionState, ConnectionState)> callback_copy;
        {
            std::lock_guard<std::mutex> lock(state_callback_mutex_);
            if (state_change_callback_ && !is_destroying_.load()) {
                callback_copy = state_change_callback_;
            }
        }

        // Double-check is_destroying_ AFTER releasing lock but BEFORE invoking callback
        // This catches the race where destructor set the flag between our copy and invocation
        if (callback_copy && !is_destroying_.load()) {
            try {
                callback_copy(old_state, new_state);
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("[Moonraker Client] State change callback threw exception: {}",
                                   e.what());
            } catch (...) {
                LOG_ERROR_INTERNAL(
                    "[Moonraker Client] State change callback threw unknown exception");
            }
        }
    }
}

void MoonrakerClient::disconnect() {
    ConnectionState current_state = connection_state_.load();

    // Only log if we're actually connected/connecting
    if (current_state != ConnectionState::DISCONNECTED &&
        current_state != ConnectionState::FAILED) {
        spdlog::debug("[Moonraker Client] Disconnecting from WebSocket server");
    }

    // Close the WebSocket connection FIRST (before replacing callbacks)
    // This allows the is_destroying_ flag check in callbacks to prevent execution
    // The callbacks will check is_destroying_ and early-return if true
    close();

    // Now replace callbacks with no-op lambdas to prevent any late invocations
    onopen = []() { /* no-op */ };
    onmessage = [](const std::string&) { /* no-op */ };
    onclose = []() { /* no-op */ };

    // Clean up any pending requests
    cleanup_pending_requests();

    // Reset connection state
    set_connection_state(ConnectionState::DISCONNECTED);
    reconnect_attempts_ = 0;
}

void MoonrakerClient::force_reconnect() {
    spdlog::info("[Moonraker Client] Force reconnect requested - full state reset");

    // Copy stored connection info under lock
    std::string url;
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void()> on_discovery_complete;
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        url = last_url_;
        on_connected = last_on_connected_;
        on_disconnected = last_on_disconnected_;
        on_discovery_complete = last_discovery_complete_;
    }

    // Verify we have stored connection info
    if (url.empty()) {
        spdlog::warn(
            "[Moonraker Client] force_reconnect() called but no previous connection info - "
            "call connect() first");
        return;
    }

    // 1. Disconnect cleanly (clears pending requests, resets state)
    disconnect();

    // 2. Connect using stored URL and callbacks
    int result = connect(url.c_str(), on_connected, on_disconnected);
    if (result != 0) {
        spdlog::error("[Moonraker Client] force_reconnect() connect failed: {}", result);
        return;
    }

    // 3. Re-run discovery if we have a stored callback
    //    Note: discover_printer() is typically called in on_connected callback,
    //    so it will be triggered automatically. But if the caller wants explicit
    //    discovery, we provide the mechanism.
    spdlog::debug("[Moonraker Client] force_reconnect() complete - connection initiated");
}

int MoonrakerClient::connect(const char* url, std::function<void()> on_connected,
                             std::function<void()> on_disconnected) {
    // Reset WebSocket state from previous connection attempt BEFORE setting new callbacks.
    // This prevents libhv from rejecting the new open() call if we're already connecting/connected.
    // Note: close() is safe to call even if already closed (idempotent).
    close();

    // Apply connection timeout to libhv (must be called before open())
    setConnectTimeout(static_cast<int>(connection_timeout_ms_));

    spdlog::debug("[Moonraker Client] WebSocket connecting to {}", url);
    set_connection_state(ConnectionState::CONNECTING);

    // Connection opened callback
    // Wrap entire callback body in try-catch to prevent any exception from escaping to libhv
    onopen = [this, on_connected, url]() {
        try {
            // Prevent callback execution if client is being destroyed (avoid use-after-free)
            if (is_destroying_.load()) {
                return;
            }

            // Note: getHttpResponse() available here if needed for upgrade response inspection
            spdlog::debug("[Moonraker Client] WebSocket connected to {}", url);
            was_connected_ = true;
            set_connection_state(ConnectionState::CONNECTED);

            // Reset notification flags on successful connection
            reset_notification_flags();

            // Invoke user callback with exception safety
            try {
                on_connected();
            } catch (const std::exception& e) {
                LOG_ERROR_INTERNAL("[Moonraker Client] Connection callback threw exception: {}",
                                   e.what());
            } catch (...) {
                LOG_ERROR_INTERNAL(
                    "[Moonraker Client] Connection callback threw unknown exception");
            }
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onopen callback threw unexpected exception: {}",
                               e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onopen callback threw unknown exception");
        }
    };

    // Message received callback
    // Wrap entire callback body in try-catch to prevent any exception from escaping to libhv
    onmessage = [this, on_connected, on_disconnected](const std::string& msg) {
        try {
            // Prevent callback execution if client is being destroyed (avoid use-after-free)
            if (is_destroying_.load()) {
                return;
            }

            // Validate message size to prevent memory exhaustion
            static constexpr size_t MAX_MESSAGE_SIZE = 1024 * 1024; // 1 MB
            if (msg.size() > MAX_MESSAGE_SIZE) {
                spdlog::error("[Moonraker Client] Message too large: {} bytes (max: {})",
                              msg.size(), MAX_MESSAGE_SIZE);

                // Emit event - this indicates a protocol problem
                emit_event(MoonrakerEventType::MESSAGE_OVERSIZED,
                           fmt::format("Received oversized data from printer ({} bytes). "
                                       "This may indicate a communication error.",
                                       msg.size()),
                           true);

                disconnect();
                return;
            }

            // Check for timed out requests on each message (opportunistic cleanup)
            check_request_timeouts();

            // Parse JSON message
            json j;
            try {
                j = json::parse(msg);
            } catch (const json::parse_error& e) {
                LOG_ERROR_INTERNAL("[Moonraker Client] JSON parse error: {}", e.what());
                return;
            }

            // Handle responses with request IDs (one-time callbacks)
            if (j.contains("id")) {
                // Validate 'id' field type
                if (!j["id"].is_number_integer()) {
                    LOG_ERROR_INTERNAL("[Moonraker Client] Invalid 'id' type in response: {}",
                                       j["id"].type_name());
                    return;
                }

                uint64_t id = j["id"].get<uint64_t>();

                // Copy callbacks out before invoking to avoid deadlock
                std::function<void(json)> success_cb;
                std::function<void(const MoonrakerError&)> error_cb;
                std::string method_name;
                bool has_error = false;
                MoonrakerError error;

                {
                    std::lock_guard<std::mutex> lock(requests_mutex_);
                    auto it = pending_requests_.find(id);
                    if (it != pending_requests_.end()) {
                        PendingRequest& request = it->second;
                        method_name = request.method;

                        // Check for JSON-RPC error
                        if (j.contains("error")) {
                            has_error = true;
                            error = MoonrakerError::from_json_rpc(j["error"], request.method);
                            error_cb = request.error_callback;
                        } else {
                            success_cb = request.success_callback;
                        }

                        pending_requests_.erase(it); // Remove before invoking callbacks
                    }
                } // Lock released here

                // Invoke callbacks outside the lock to avoid deadlock
                if (has_error) {
                    spdlog::error("[Moonraker Client] Request {} failed: {}", method_name,
                                  error.message);

                    // Emit RPC error event
                    emit_event(
                        MoonrakerEventType::RPC_ERROR,
                        fmt::format("Printer command '{}' failed: {}", method_name, error.message),
                        true, method_name);

                    if (error_cb) {
                        error_cb(error);
                    }
                } else if (success_cb) {
                    success_cb(j);
                }
            }

            // Handle notifications (no request ID)
            if (j.contains("method")) {
                // Validate 'method' field type
                if (!j["method"].is_string()) {
                    LOG_ERROR_INTERNAL(
                        "[Moonraker Client] Invalid 'method' type in notification: {}",
                        j["method"].type_name());
                    return;
                }

                std::string method = j["method"].get<std::string>();

                // Copy callbacks to invoke (to avoid holding lock during callback execution)
                std::vector<std::function<void(json)>> callbacks_to_invoke;

                {
                    std::lock_guard<std::mutex> lock(callbacks_mutex_);

                    // Printer status updates (most common)
                    if (method == "notify_status_update" || method == "notify_filelist_changed") {
                        // Copy all notify callbacks from map
                        callbacks_to_invoke.reserve(notify_callbacks_.size());
                        for (const auto& [id, cb] : notify_callbacks_) {
                            callbacks_to_invoke.push_back(cb);
                        }
                    }

                    // Method-specific persistent callbacks
                    auto method_it = method_callbacks_.find(method);
                    if (method_it != method_callbacks_.end()) {
                        for (auto& [handler_name, cb] : method_it->second) {
                            callbacks_to_invoke.push_back(cb);
                        }
                    }
                } // Release lock

                // Parse bed mesh updates before invoking user callbacks
                if (method == "notify_status_update" && j.contains("params") &&
                    j["params"].is_array() && !j["params"].empty()) {
                    const json& params = j["params"][0];
                    if (params.contains("bed_mesh") && params["bed_mesh"].is_object()) {
                        parse_bed_mesh(params["bed_mesh"]);
                    }
                }

                // Invoke callbacks outside lock to prevent deadlock
                for (auto& cb : callbacks_to_invoke) {
                    try {
                        cb(j);
                    } catch (const std::exception& e) {
                        LOG_ERROR_INTERNAL("[Moonraker Client] Callback for {} threw exception: {}",
                                           method, e.what());
                    } catch (...) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Callback for {} threw unknown exception", method);
                    }
                }

                // Klippy disconnected from Moonraker
                if (method == "notify_klippy_disconnected") {
                    spdlog::warn("[Moonraker Client] Klipper disconnected from Moonraker");

                    // Update klippy state in PrinterState (SHUTDOWN = firmware disconnected)
                    get_printer_state().set_klippy_state(KlippyState::SHUTDOWN);

                    // Emit event for UI layer to handle
                    emit_event(MoonrakerEventType::KLIPPY_DISCONNECTED,
                               "Klipper has disconnected from Moonraker. Check for errors in your "
                               "printer interface.",
                               true);

                    // Invoke user callback with exception safety
                    try {
                        on_disconnected();
                    } catch (const std::exception& e) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw exception: {}",
                            e.what());
                    } catch (...) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Disconnection callback threw unknown exception");
                    }
                }
                // Klippy reconnected to Moonraker
                else if (method == "notify_klippy_ready") {
                    spdlog::info("[Moonraker Client] Klipper ready");

                    // Update klippy state in PrinterState (READY = firmware ready)
                    get_printer_state().set_klippy_state(KlippyState::READY);

                    // Invoke user callback with exception safety
                    try {
                        on_connected();
                    } catch (const std::exception& e) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Connection callback threw exception: {}", e.what());
                    } catch (...) {
                        LOG_ERROR_INTERNAL(
                            "[Moonraker Client] Connection callback threw unknown exception");
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL(
                "[Moonraker Client] onmessage callback threw unexpected exception: {}", e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onmessage callback threw unknown exception");
        }
    };

    // Connection closed callback
    // Wrap entire callback body in try-catch to prevent any exception from escaping
    // to libhv (which may not handle exceptions properly or may be noexcept)
    onclose = [this, on_disconnected]() {
        try {
            spdlog::debug("[Moonraker Client] onclose callback invoked, is_destroying={}",
                          is_destroying_.load());

            // Prevent callback execution if client is being destroyed (avoid use-after-free)
            if (is_destroying_.load()) {
                spdlog::debug(
                    "[Moonraker Client] onclose callback early return due to destruction");
                return;
            }

            ConnectionState current = connection_state_.load();

            // Cleanup all pending requests (invoke error callbacks)
            cleanup_pending_requests();

            if (was_connected_) {
                spdlog::warn("[Moonraker Client] WebSocket connection closed");
                was_connected_ = false;

                // Emit event with rate limiting to prevent spam during reconnect loop
                if (!g_already_notified_disconnect.load()) {
                    emit_event(MoonrakerEventType::CONNECTION_LOST,
                               "Connection to printer lost - attempting to reconnect...", false);
                    g_already_notified_disconnect.store(true);
                }

                // Check if this is a reconnection scenario
                if (current != ConnectionState::FAILED) {
                    set_connection_state(ConnectionState::RECONNECTING);
                }

                // Invoke user callback with exception safety
                try {
                    on_disconnected();
                } catch (const std::exception& e) {
                    LOG_ERROR_INTERNAL(
                        "[Moonraker Client] Disconnection callback threw exception: {}", e.what());
                } catch (...) {
                    LOG_ERROR_INTERNAL(
                        "[Moonraker Client] Disconnection callback threw unknown exception");
                }
            } else {
                spdlog::debug(
                    "[Moonraker Client] WebSocket connection failed (printer not available)");

                // Initial connection failed
                if (current == ConnectionState::CONNECTING) {
                    set_connection_state(ConnectionState::DISCONNECTED);
                }

                // Call on_disconnected() to notify about connection failure
                // Callers can use their own state tracking (e.g. connection_testing flag)
                // to distinguish initial connection failures from reconnection scenarios
                try {
                    on_disconnected();
                } catch (const std::exception& e) {
                    LOG_ERROR_INTERNAL(
                        "[Moonraker Client] Disconnection callback threw exception: {}", e.what());
                } catch (...) {
                    LOG_ERROR_INTERNAL(
                        "[Moonraker Client] Disconnection callback threw unknown exception");
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onclose callback threw unexpected exception: {}",
                               e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("[Moonraker Client] onclose callback threw unknown exception");
        }
    };

    // WebSocket ping (keepalive) - use configured interval
    setPingInterval(static_cast<int>(keepalive_interval_ms_));

    // Automatic reconnection with exponential backoff - use configured values
    reconn_setting_t reconn;
    reconn_setting_init(&reconn);
    reconn.min_delay = reconnect_min_delay_ms_;
    reconn.max_delay = reconnect_max_delay_ms_;
    reconn.delay_policy = 2; // Exponential backoff
    setReconnect(&reconn);

    // Store connection info for force_reconnect()
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        last_url_ = url;
        last_on_connected_ = on_connected;
        last_on_disconnected_ = on_disconnected;
    }

    // Connect
    http_headers headers;
    return open(url, headers);
}

SubscriptionId MoonrakerClient::register_notify_update(std::function<void(json)> cb) {
    if (!cb) {
        spdlog::warn("[Moonraker Client] register_notify_update called with null callback");
        return INVALID_SUBSCRIPTION_ID;
    }

    SubscriptionId id = next_subscription_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        notify_callbacks_.emplace(id, cb);
    }
    spdlog::debug("[Moonraker Client] Registered notify callback with ID {}", id);
    return id;
}

bool MoonrakerClient::unsubscribe_notify_update(SubscriptionId id) {
    if (id == INVALID_SUBSCRIPTION_ID) {
        return false;
    }

    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = notify_callbacks_.find(id);
    if (it != notify_callbacks_.end()) {
        notify_callbacks_.erase(it);
        spdlog::debug("[Moonraker Client] Unsubscribed notify callback ID {}", id);
        return true;
    }
    spdlog::debug("[Moonraker Client] Unsubscribe failed: notify callback ID {} not found", id);
    return false;
}

void MoonrakerClient::register_event_handler(MoonrakerEventCallback cb) {
    std::lock_guard<std::mutex> lock(event_handler_mutex_);
    event_handler_ = std::move(cb);
    spdlog::debug("[Moonraker Client] Event handler {}",
                  event_handler_ ? "registered" : "unregistered");
}

void MoonrakerClient::emit_event(MoonrakerEventType type, const std::string& message, bool is_error,
                                 const std::string& details) {
    MoonrakerEventCallback handler;
    {
        std::lock_guard<std::mutex> lock(event_handler_mutex_);
        handler = event_handler_;
    }

    if (handler) {
        MoonrakerEvent evt{type, message, details, is_error};
        try {
            handler(evt);
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker Client] Event handler threw exception: {}", e.what());
        }
    } else {
        // No handler registered - just log the event
        if (is_error) {
            spdlog::error("[Moonraker Event] {}: {}", static_cast<int>(type), message);
        } else {
            spdlog::warn("[Moonraker Event] {}: {}", static_cast<int>(type), message);
        }
    }
}

void MoonrakerClient::dispatch_status_update(const json& status) {
    // Parse bed mesh data before dispatching (mirrors WebSocket handler behavior)
    // This ensures bed mesh is populated on initial subscription response,
    // not just on subsequent notify_status_update messages
    if (status.contains("bed_mesh") && status["bed_mesh"].is_object()) {
        parse_bed_mesh(status["bed_mesh"]);

        // Also extract build volume from bed_mesh bounds for printer detection
        const json& mesh = status["bed_mesh"];
        if (mesh.contains("mesh_min") && mesh["mesh_min"].is_array() &&
            mesh["mesh_min"].size() >= 2) {
            build_volume_.x_min = mesh["mesh_min"][0].get<float>();
            build_volume_.y_min = mesh["mesh_min"][1].get<float>();
        }
        if (mesh.contains("mesh_max") && mesh["mesh_max"].is_array() &&
            mesh["mesh_max"].size() >= 2) {
            build_volume_.x_max = mesh["mesh_max"][0].get<float>();
            build_volume_.y_max = mesh["mesh_max"][1].get<float>();
            spdlog::debug("[Moonraker Client] Build volume from mesh: [{:.0f},{:.0f}] to "
                          "[{:.0f},{:.0f}]",
                          build_volume_.x_min, build_volume_.y_min, build_volume_.x_max,
                          build_volume_.y_max);
        }
    }

    // Extract kinematics type from toolhead data (for printer detection)
    if (status.contains("toolhead") && status["toolhead"].is_object()) {
        const json& toolhead = status["toolhead"];
        if (toolhead.contains("kinematics") && toolhead["kinematics"].is_string()) {
            kinematics_ = toolhead["kinematics"].get<std::string>();
            spdlog::debug("[Moonraker Client] Kinematics type: {}", kinematics_);
        }
    }

    // Wrap raw status into notify_status_update format
    json notification = {
        {"method", "notify_status_update"},
        {"params", json::array({status, 0.0})} // [status, eventtime]
    };

    // Dispatch to all registered callbacks
    // Two-phase: copy under lock, invoke outside to avoid deadlock
    std::vector<std::function<void(json)>> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_copy.reserve(notify_callbacks_.size());
        for (const auto& [id, cb] : notify_callbacks_) {
            callbacks_copy.push_back(cb);
        }
    }

    for (const auto& cb : callbacks_copy) {
        if (cb) {
            cb(notification);
        }
    }

    spdlog::debug("[Moonraker Client] Dispatched status update to {} callbacks",
                  callbacks_copy.size());
}

void MoonrakerClient::register_method_callback(const std::string& method,
                                               const std::string& handler_name,
                                               std::function<void(json)> cb) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = method_callbacks_.find(method);
    if (it == method_callbacks_.end()) {
        spdlog::debug("[Moonraker Client] Registering new method callback: {} (handler: {})",
                      method, handler_name);
        std::map<std::string, std::function<void(json)>> handlers;
        handlers.insert({handler_name, cb});
        method_callbacks_.insert({method, handlers});
    } else {
        spdlog::debug("[Moonraker Client] Adding handler to existing method {}: {}", method,
                      handler_name);
        it->second.insert({handler_name, cb});
    }
}

bool MoonrakerClient::unregister_method_callback(const std::string& method,
                                                 const std::string& handler_name) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto method_it = method_callbacks_.find(method);
    if (method_it == method_callbacks_.end()) {
        spdlog::debug("[Moonraker Client] Unregister failed: method '{}' not found", method);
        return false;
    }

    auto handler_it = method_it->second.find(handler_name);
    if (handler_it == method_it->second.end()) {
        spdlog::debug(
            "[Moonraker Client] Unregister failed: handler '{}' not found for method '{}'",
            handler_name, method);
        return false;
    }

    method_it->second.erase(handler_it);
    spdlog::debug("[Moonraker Client] Unregistered handler '{}' from method '{}'", handler_name,
                  method);

    // Clean up empty method entries to avoid memory leaks
    if (method_it->second.empty()) {
        method_callbacks_.erase(method_it);
        spdlog::debug("[Moonraker Client] Removed empty method entry for '{}'", method);
    }

    return true;
}

int MoonrakerClient::send_jsonrpc(const std::string& method) {
    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;
    rpc["id"] = request_id_++;

    spdlog::debug("[Moonraker Client] send_jsonrpc: {}", rpc.dump());
    return send(rpc.dump());
}

int MoonrakerClient::send_jsonrpc(const std::string& method, const json& params) {
    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;

    // Only include params if not null or empty
    if (!params.is_null() && !params.empty()) {
        rpc["params"] = params;
    }

    rpc["id"] = request_id_++;

    spdlog::debug("[Moonraker Client] send_jsonrpc: {}", rpc.dump());
    return send(rpc.dump());
}

RequestId MoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                        std::function<void(json)> cb) {
    // Forward to new overload with null error callback
    return send_jsonrpc(method, params, cb, nullptr, 0);
}

RequestId MoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                        std::function<void(json)> success_cb,
                                        std::function<void(const MoonrakerError&)> error_cb,
                                        uint32_t timeout_ms) {
    // Atomically fetch and increment to avoid race condition in concurrent calls
    // Note: request_id_ starts at 0, but we increment FIRST, so actual IDs start at 1
    // This ensures we never return 0 (INVALID_REQUEST_ID) for a valid request
    RequestId id = request_id_.fetch_add(1) + 1;

    // Create pending request
    PendingRequest request;
    request.id = id;
    request.method = method;
    request.success_callback = success_cb;
    request.error_callback = error_cb;
    request.timestamp = std::chrono::steady_clock::now();
    request.timeout_ms = (timeout_ms > 0) ? timeout_ms : default_request_timeout_ms_;

    // Register request
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        auto it = pending_requests_.find(id);
        if (it != pending_requests_.end()) {
            LOG_ERROR_INTERNAL("[Moonraker Client] Request ID {} already has a registered callback",
                               id);
            return INVALID_REQUEST_ID;
        }
        pending_requests_.insert({id, request});
        spdlog::debug("[Moonraker Client] Registered request {} for method {}, total pending: {}",
                      id, method, pending_requests_.size());
    }

    // Build and send JSON-RPC message with the registered ID
    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;
    rpc["id"] = id; // Use the ID we registered, not a new one

    // Only include params if not null or empty
    if (!params.is_null() && !params.empty()) {
        rpc["params"] = params;
    }

    spdlog::debug("[Moonraker Client] send_jsonrpc: {}", rpc.dump());
    int result = send(rpc.dump());
    spdlog::debug("[Moonraker Client] send_jsonrpc({}) returned {}", method, result);

    // Return the request ID on success, or INVALID_REQUEST_ID on send failure
    if (result < 0) {
        // Send failed - remove pending request and invoke error callback
        std::function<void(const MoonrakerError&)> error_callback_copy;
        std::string method_name;
        {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                error_callback_copy = it->second.error_callback;
                method_name = it->second.method;
                pending_requests_.erase(it);
            }
        }
        spdlog::error("[Moonraker Client] Failed to send request {}, removed from pending", id);

        // Invoke error callback outside lock (prevents deadlock if callback sends new request)
        if (error_callback_copy) {
            try {
                error_callback_copy(MoonrakerError::connection_lost(method_name));
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker Client] Error callback threw exception: {}", e.what());
            }
        }
        return INVALID_REQUEST_ID;
    }

    return id;
}

bool MoonrakerClient::cancel_request(RequestId id) {
    if (id == INVALID_REQUEST_ID) {
        return false;
    }

    std::lock_guard<std::mutex> lock(requests_mutex_);
    auto it = pending_requests_.find(id);
    if (it != pending_requests_.end()) {
        spdlog::debug("[Moonraker Client] Cancelled request {} ({})", id, it->second.method);
        pending_requests_.erase(it);
        return true;
    }

    spdlog::debug("[Moonraker Client] Cancel failed: request {} not found (already completed?)",
                  id);
    return false;
}

int MoonrakerClient::gcode_script(const std::string& gcode) {
    json params = {{"script", gcode}};
    return send_jsonrpc("printer.gcode.script", params);
}

void MoonrakerClient::discover_printer(std::function<void()> on_complete) {
    spdlog::debug("[Moonraker Client] Starting printer auto-discovery");

    // Store callback for force_reconnect()
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        last_discovery_complete_ = on_complete;
    }

    // Step 1: Query available printer objects (no params required)
    send_jsonrpc("printer.objects.list", json(), [this, on_complete](json response) {
        // Debug: Log raw response
        spdlog::debug("[Moonraker Client] printer.objects.list response: {}", response.dump());

        // Validate response
        if (!response.contains("result") || !response["result"].contains("objects")) {
            spdlog::error("[Moonraker Client] printer.objects.list failed: invalid response");
            if (response.contains("error")) {
                spdlog::error("[Moonraker Client]   Error details: {}", response["error"].dump());
            }

            // Emit discovery failed event
            emit_event(MoonrakerEventType::DISCOVERY_FAILED,
                       "Failed to query printer objects from Moonraker", true);
            return;
        }

        // Parse discovered objects into typed arrays
        const json& objects = response["result"]["objects"];
        parse_objects(objects);

        // Step 2: Get server information
        send_jsonrpc("server.info", {}, [this, on_complete](json info_response) {
            if (info_response.contains("result")) {
                const json& result = info_response["result"];
                std::string klippy_version = result.value("klippy_version", "unknown");
                moonraker_version_ = result.value("moonraker_version", "unknown");

                spdlog::debug("[Moonraker Client] Moonraker version: {}", moonraker_version_);
                spdlog::debug("[Moonraker Client] Klippy version: {}", klippy_version);

                if (result.contains("components")) {
                    std::vector<std::string> components =
                        result["components"].get<std::vector<std::string>>();
                    spdlog::debug("[Moonraker Client] Server components: {}",
                                  json(components).dump());
                }
            }

            // Step 3: Get printer information
            send_jsonrpc("printer.info", {}, [this, on_complete](json printer_response) {
                if (printer_response.contains("result")) {
                    const json& result = printer_response["result"];
                    hostname_ = result.value("hostname", "unknown");
                    software_version_ = result.value("software_version", "unknown");
                    std::string state_message = result.value("state_message", "");

                    spdlog::debug("[Moonraker Client] Printer hostname: {}", hostname_);
                    spdlog::debug("[Moonraker Client] Klipper software version: {}",
                                  software_version_);
                    if (!state_message.empty()) {
                        spdlog::info("[Moonraker Client] Printer state: {}", state_message);
                    }
                }

                // Step 4: Query MCU information for printer detection
                // Find all MCU objects (e.g., "mcu", "mcu EBBCan", "mcu rpi")
                std::vector<std::string> mcu_objects;
                for (const auto& obj : printer_objects_) {
                    // Match "mcu" or "mcu <name>" pattern
                    if (obj == "mcu" || obj.rfind("mcu ", 0) == 0) {
                        mcu_objects.push_back(obj);
                    }
                }

                if (mcu_objects.empty()) {
                    spdlog::debug("[Moonraker Client] No MCU objects found, skipping MCU query");
                    // Continue to subscription step
                    complete_discovery_subscription(on_complete);
                    return;
                }

                // Query all MCU objects in parallel using a shared counter
                auto pending_mcu_queries =
                    std::make_shared<std::atomic<size_t>>(mcu_objects.size());
                auto mcu_results =
                    std::make_shared<std::vector<std::pair<std::string, std::string>>>();
                auto mcu_results_mutex = std::make_shared<std::mutex>();

                for (const auto& mcu_obj : mcu_objects) {
                    json mcu_query = {{mcu_obj, nullptr}};
                    send_jsonrpc(
                        "printer.objects.query", {{"objects", mcu_query}},
                        [this, on_complete, mcu_obj, pending_mcu_queries, mcu_results,
                         mcu_results_mutex](json mcu_response) {
                            std::string chip_type;

                            // Extract MCU chip type from mcu_constants.MCU
                            if (mcu_response.contains("result") &&
                                mcu_response["result"].contains("status") &&
                                mcu_response["result"]["status"].contains(mcu_obj)) {
                                const json& mcu_data = mcu_response["result"]["status"][mcu_obj];

                                if (mcu_data.contains("mcu_constants") &&
                                    mcu_data["mcu_constants"].is_object() &&
                                    mcu_data["mcu_constants"].contains("MCU")) {
                                    chip_type = mcu_data["mcu_constants"]["MCU"].get<std::string>();
                                    spdlog::debug("[Moonraker Client] Detected MCU '{}': {}",
                                                  mcu_obj, chip_type);
                                }
                            }

                            // Store result thread-safely
                            if (!chip_type.empty()) {
                                std::lock_guard<std::mutex> lock(*mcu_results_mutex);
                                mcu_results->push_back({mcu_obj, chip_type});
                            }

                            // Check if all queries complete
                            if (pending_mcu_queries->fetch_sub(1) == 1) {
                                // All MCU queries complete - populate mcu_ and mcu_list_
                                mcu_list_.clear();
                                mcu_.clear();

                                // Sort results to ensure consistent ordering (primary "mcu" first)
                                std::lock_guard<std::mutex> lock(*mcu_results_mutex);
                                std::sort(mcu_results->begin(), mcu_results->end(),
                                          [](const auto& a, const auto& b) {
                                              // "mcu" comes first, then alphabetical
                                              if (a.first == "mcu")
                                                  return true;
                                              if (b.first == "mcu")
                                                  return false;
                                              return a.first < b.first;
                                          });

                                for (const auto& [obj_name, chip] : *mcu_results) {
                                    mcu_list_.push_back(chip);
                                    if (obj_name == "mcu" && mcu_.empty()) {
                                        mcu_ = chip;
                                    }
                                }

                                if (!mcu_.empty()) {
                                    spdlog::info("[Moonraker Client] Primary MCU: {}", mcu_);
                                }
                                if (mcu_list_.size() > 1) {
                                    spdlog::info("[Moonraker Client] All MCUs: {}",
                                                 json(mcu_list_).dump());
                                }

                                // Continue to subscription step
                                complete_discovery_subscription(on_complete);
                            }
                        },
                        [this, on_complete, mcu_obj,
                         pending_mcu_queries](const MoonrakerError& err) {
                            spdlog::warn("[Moonraker Client] MCU query for '{}' failed: {}",
                                         mcu_obj, err.message);

                            // Check if all queries complete (even on error)
                            if (pending_mcu_queries->fetch_sub(1) == 1) {
                                // Continue to subscription step even if some MCU queries failed
                                complete_discovery_subscription(on_complete);
                            }
                        });
                }
            });
        });
    });
}

void MoonrakerClient::complete_discovery_subscription(std::function<void()> on_complete) {
    // Step 5: Subscribe to all discovered objects + core objects
    json subscription_objects;

    // Core non-optional objects
    subscription_objects["print_stats"] = nullptr;
    subscription_objects["virtual_sdcard"] = nullptr;
    subscription_objects["toolhead"] = nullptr;
    subscription_objects["gcode_move"] = nullptr;
    subscription_objects["motion_report"] = nullptr;
    subscription_objects["system_stats"] = nullptr;

    // All discovered heaters (extruders, beds, generic heaters)
    for (const auto& heater : heaters_) {
        subscription_objects[heater] = nullptr;
    }

    // All discovered sensors
    for (const auto& sensor : sensors_) {
        subscription_objects[sensor] = nullptr;
    }

    // All discovered fans
    for (const auto& fan : fans_) {
        subscription_objects[fan] = nullptr;
    }

    // All discovered LEDs
    for (const auto& led : leds_) {
        subscription_objects[led] = nullptr;
    }

    // Bed mesh (for 3D visualization)
    subscription_objects["bed_mesh"] = nullptr;

    // Exclude object (for mid-print object exclusion)
    subscription_objects["exclude_object"] = nullptr;

    json subscribe_params = {{"objects", subscription_objects}};

    send_jsonrpc(
        "printer.objects.subscribe", subscribe_params,
        [this, on_complete, subscription_objects](json sub_response) {
            if (sub_response.contains("result")) {
                spdlog::debug("[Moonraker Client] Subscription complete: {} objects subscribed",
                              subscription_objects.size());

                // Process initial state from subscription response
                // Moonraker returns current values in result.status
                if (sub_response["result"].contains("status")) {
                    spdlog::info(
                        "[Moonraker Client] Processing initial printer state from subscription");
                    dispatch_status_update(sub_response["result"]["status"]);
                }
            } else if (sub_response.contains("error")) {
                spdlog::error("[Moonraker Client] Subscription failed: {}",
                              sub_response["error"].dump());

                // Emit discovery failed event (subscription is part of discovery)
                std::string error_msg = sub_response["error"].dump();
                emit_event(MoonrakerEventType::DISCOVERY_FAILED,
                           fmt::format("Failed to subscribe to printer updates: {}", error_msg),
                           false); // Warning, not error - discovery still completes
            }

            // Discovery complete - notify observers
            if (on_discovery_complete_) {
                on_discovery_complete_(capabilities_);
            }
            on_complete();
        });
}

void MoonrakerClient::parse_objects(const json& objects) {
    heaters_.clear();
    sensors_.clear();
    fans_.clear();
    leds_.clear();
    steppers_.clear();
    printer_objects_.clear();

    for (const auto& obj : objects) {
        std::string name = obj.template get<std::string>();

        // Store all objects for detection heuristics (object_exists, macro_match)
        printer_objects_.push_back(name);

        // Steppers (stepper_x, stepper_y, stepper_z, stepper_z1, etc.)
        if (name.rfind("stepper_", 0) == 0) {
            steppers_.push_back(name);
        }
        // Extruders (controllable heaters)
        // Match "extruder", "extruder1", etc., but NOT "extruder_stepper"
        else if (name.rfind("extruder", 0) == 0 && name.rfind("extruder_stepper", 0) != 0) {
            heaters_.push_back(name);
        }
        // Heated bed
        else if (name == "heater_bed") {
            heaters_.push_back(name);
        }
        // Generic heaters (e.g., "heater_generic chamber")
        else if (name.rfind("heater_generic ", 0) == 0) {
            heaters_.push_back(name);
        }
        // Read-only temperature sensors
        else if (name.rfind("temperature_sensor ", 0) == 0) {
            sensors_.push_back(name);
        }
        // Temperature-controlled fans (also act as sensors)
        else if (name.rfind("temperature_fan ", 0) == 0) {
            sensors_.push_back(name);
            fans_.push_back(name); // Also add to fans for control
        }
        // Part cooling fan
        else if (name == "fan") {
            fans_.push_back(name);
        }
        // Heater fans (e.g., "heater_fan hotend_fan")
        else if (name.rfind("heater_fan ", 0) == 0) {
            fans_.push_back(name);
        }
        // Generic fans
        else if (name.rfind("fan_generic ", 0) == 0) {
            fans_.push_back(name);
        }
        // Controller fans
        else if (name.rfind("controller_fan ", 0) == 0) {
            fans_.push_back(name);
        }
        // Output pins (can be used as fans)
        else if (name.rfind("output_pin ", 0) == 0) {
            fans_.push_back(name);
        }
        // LED outputs
        else if (name.rfind("led ", 0) == 0 || name.rfind("neopixel ", 0) == 0 ||
                 name.rfind("dotstar ", 0) == 0) {
            leds_.push_back(name);
        }
    }

    spdlog::debug(
        "[Moonraker Client] Discovered: {} heaters, {} sensors, {} fans, {} LEDs, {} steppers",
        heaters_.size(), sensors_.size(), fans_.size(), leds_.size(), steppers_.size());

    // Debug output of discovered objects
    if (!heaters_.empty()) {
        spdlog::debug("[Moonraker Client] Heaters: {}", json(heaters_).dump());
    }
    if (!sensors_.empty()) {
        spdlog::debug("[Moonraker Client] Sensors: {}", json(sensors_).dump());
    }
    if (!fans_.empty()) {
        spdlog::debug("[Moonraker Client] Fans: {}", json(fans_).dump());
    }
    if (!leds_.empty()) {
        spdlog::debug("[Moonraker Client] LEDs: {}", json(leds_).dump());
    }
    if (!steppers_.empty()) {
        spdlog::debug("[Moonraker Client] Steppers: {}", json(steppers_).dump());
    }

    // Parse printer capabilities (QGL, Z-tilt, bed mesh, macros)
    capabilities_.parse_objects(objects);
}

void MoonrakerClient::parse_bed_mesh(const json& bed_mesh) {
    // Parse active profile name
    if (bed_mesh.contains("profile_name") && !bed_mesh["profile_name"].is_null()) {
        active_bed_mesh_.name = bed_mesh["profile_name"].template get<std::string>();
    }

    // Parse probed_matrix (2D array of Z heights)
    if (bed_mesh.contains("probed_matrix") && bed_mesh["probed_matrix"].is_array()) {
        active_bed_mesh_.probed_matrix.clear();
        for (const auto& row : bed_mesh["probed_matrix"]) {
            if (row.is_array()) {
                std::vector<float> row_vec;
                for (const auto& val : row) {
                    if (val.is_number()) {
                        row_vec.push_back(val.template get<float>());
                    }
                }
                if (!row_vec.empty()) {
                    active_bed_mesh_.probed_matrix.push_back(row_vec);
                }
            }
        }

        // Update dimensions
        active_bed_mesh_.y_count = static_cast<int>(active_bed_mesh_.probed_matrix.size());
        active_bed_mesh_.x_count = active_bed_mesh_.probed_matrix.empty()
                                       ? 0
                                       : static_cast<int>(active_bed_mesh_.probed_matrix[0].size());
    }

    // Parse mesh bounds
    if (bed_mesh.contains("mesh_min") && bed_mesh["mesh_min"].is_array() &&
        bed_mesh["mesh_min"].size() >= 2) {
        active_bed_mesh_.mesh_min[0] = bed_mesh["mesh_min"][0].template get<float>();
        active_bed_mesh_.mesh_min[1] = bed_mesh["mesh_min"][1].template get<float>();
    }

    if (bed_mesh.contains("mesh_max") && bed_mesh["mesh_max"].is_array() &&
        bed_mesh["mesh_max"].size() >= 2) {
        active_bed_mesh_.mesh_max[0] = bed_mesh["mesh_max"][0].template get<float>();
        active_bed_mesh_.mesh_max[1] = bed_mesh["mesh_max"][1].template get<float>();
    }

    // Parse available profiles
    if (bed_mesh.contains("profiles") && bed_mesh["profiles"].is_object()) {
        bed_mesh_profiles_.clear();
        for (auto& [profile_name, profile_data] : bed_mesh["profiles"].items()) {
            bed_mesh_profiles_.push_back(profile_name);
        }
    }

    // Parse algorithm from mesh_params (if available)
    if (bed_mesh.contains("mesh_params") && bed_mesh["mesh_params"].is_object()) {
        const json& params = bed_mesh["mesh_params"];
        if (params.contains("algo") && params["algo"].is_string()) {
            active_bed_mesh_.algo = params["algo"].template get<std::string>();
        }
    }

    if (active_bed_mesh_.probed_matrix.empty()) {
        spdlog::debug("[Moonraker Client] Bed mesh data cleared (no probed_matrix)");
    } else {
        spdlog::info("[Moonraker Client] Bed mesh updated: profile='{}', size={}x{}, "
                     "profiles={}, algo='{}'",
                     active_bed_mesh_.name, active_bed_mesh_.x_count, active_bed_mesh_.y_count,
                     bed_mesh_profiles_.size(), active_bed_mesh_.algo);
    }
}

std::string MoonrakerClient::guess_bed_heater() const {
    // Priority-based search for bed heater
    // 1. Exact match: "heater_bed" (most common)
    if (std::find(heaters_.begin(), heaters_.end(), "heater_bed") != heaters_.end()) {
        spdlog::debug("[Moonraker Client] guess_bed_heater() -> 'heater_bed'");
        return "heater_bed";
    }

    // 2. Exact match: "heated_bed"
    if (std::find(heaters_.begin(), heaters_.end(), "heated_bed") != heaters_.end()) {
        spdlog::debug("[Moonraker Client] guess_bed_heater() -> 'heated_bed'");
        return "heated_bed";
    }

    // 3. Substring match: any heater containing "bed"
    for (const auto& heater : heaters_) {
        if (heater.find("bed") != std::string::npos) {
            spdlog::debug("[Moonraker Client] guess_bed_heater() -> '{}'", heater);
            return heater;
        }
    }

    spdlog::debug("[Moonraker Client] guess_bed_heater() -> no match found");
    return "";
}

std::string MoonrakerClient::guess_hotend_heater() const {
    // Priority-based search for hotend heater
    // 1. Exact match: "extruder" (base extruder, most common)
    if (std::find(heaters_.begin(), heaters_.end(), "extruder") != heaters_.end()) {
        spdlog::debug("[Moonraker Client] guess_hotend_heater() -> 'extruder'");
        return "extruder";
    }

    // 2. Exact match: "extruder0"
    if (std::find(heaters_.begin(), heaters_.end(), "extruder0") != heaters_.end()) {
        spdlog::debug("[Moonraker Client] guess_hotend_heater() -> 'extruder0'");
        return "extruder0";
    }

    // 3. Substring match: any heater containing "extruder"
    for (const auto& heater : heaters_) {
        if (heater.find("extruder") != std::string::npos) {
            spdlog::debug("[Moonraker Client] guess_hotend_heater() -> '{}'", heater);
            return heater;
        }
    }

    // 4. Substring match: any heater containing "hotend"
    for (const auto& heater : heaters_) {
        if (heater.find("hotend") != std::string::npos) {
            spdlog::debug("[Moonraker Client] guess_hotend_heater() -> '{}'", heater);
            return heater;
        }
    }

    // 5. Substring match: any heater containing "e0"
    for (const auto& heater : heaters_) {
        if (heater.find("e0") != std::string::npos) {
            spdlog::debug("[Moonraker Client] guess_hotend_heater() -> '{}'", heater);
            return heater;
        }
    }

    spdlog::debug("[Moonraker Client] guess_hotend_heater() -> no match found");
    return "";
}

std::string MoonrakerClient::guess_bed_sensor() const {
    // Heaters have built-in thermistors, so check heaters first
    std::string bed_heater = guess_bed_heater();
    if (!bed_heater.empty()) {
        spdlog::debug("[Moonraker Client] guess_bed_sensor() -> '{}' (from heater)", bed_heater);
        return bed_heater;
    }

    // No bed heater found, search sensors for names containing "bed"
    for (const auto& sensor : sensors_) {
        if (sensor.find("bed") != std::string::npos) {
            spdlog::debug("[Moonraker Client] guess_bed_sensor() -> '{}'", sensor);
            return sensor;
        }
    }

    spdlog::debug("[Moonraker Client] guess_bed_sensor() -> no match found");
    return "";
}

std::string MoonrakerClient::guess_hotend_sensor() const {
    // Heaters have built-in thermistors, so check heaters first
    std::string hotend_heater = guess_hotend_heater();
    if (!hotend_heater.empty()) {
        spdlog::debug("[Moonraker Client] guess_hotend_sensor() -> '{}' (from heater)",
                      hotend_heater);
        return hotend_heater;
    }

    // No hotend heater found, search sensors for names containing relevant patterns
    // Priority: extruder > hotend > e0
    for (const auto& sensor : sensors_) {
        if (sensor.find("extruder") != std::string::npos) {
            spdlog::debug("[Moonraker Client] guess_hotend_sensor() -> '{}'", sensor);
            return sensor;
        }
    }

    for (const auto& sensor : sensors_) {
        if (sensor.find("hotend") != std::string::npos) {
            spdlog::debug("[Moonraker Client] guess_hotend_sensor() -> '{}'", sensor);
            return sensor;
        }
    }

    for (const auto& sensor : sensors_) {
        if (sensor.find("e0") != std::string::npos) {
            spdlog::debug("[Moonraker Client] guess_hotend_sensor() -> '{}'", sensor);
            return sensor;
        }
    }

    spdlog::debug("[Moonraker Client] guess_hotend_sensor() -> no match found");
    return "";
}

void MoonrakerClient::check_request_timeouts() {
    // Two-phase pattern: collect callbacks under lock, invoke outside lock
    // This prevents deadlock if callback tries to send new request
    std::vector<std::function<void()>> timed_out_callbacks;

    // Phase 1: Find timed out requests and copy callbacks (under lock)
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        std::vector<uint64_t> timed_out_ids;

        for (auto& [id, request] : pending_requests_) {
            if (request.is_timed_out()) {
                spdlog::warn("[Moonraker Client] Request {} ({}) timed out after {}ms", id,
                             request.method, request.get_elapsed_ms());

                // Emit timeout event
                std::string method_name = request.method;
                uint32_t timeout = request.timeout_ms;
                emit_event(
                    MoonrakerEventType::REQUEST_TIMEOUT,
                    fmt::format("Printer command '{}' timed out after {}ms", method_name, timeout),
                    false, method_name);

                // Capture callback in lambda if present
                if (request.error_callback) {
                    MoonrakerError error =
                        MoonrakerError::timeout(request.method, request.timeout_ms);
                    timed_out_callbacks.push_back([cb = request.error_callback, error,
                                                   method_name]() {
                        try {
                            cb(error);
                        } catch (const std::exception& e) {
                            LOG_ERROR_INTERNAL("[Moonraker Client] Timeout error callback for {} "
                                               "threw exception: {}",
                                               method_name, e.what());
                        } catch (...) {
                            LOG_ERROR_INTERNAL("[Moonraker Client] Timeout error callback for {} "
                                               "threw unknown exception",
                                               method_name);
                        }
                    });
                }

                timed_out_ids.push_back(id);
            }
        }

        // Remove timed out requests while still holding lock
        for (uint64_t id : timed_out_ids) {
            pending_requests_.erase(id);
        }
    } // Lock released here

    // Phase 2: Invoke callbacks outside lock (safe - callbacks can call send_jsonrpc)
    for (auto& callback : timed_out_callbacks) {
        callback();
    }
}

void MoonrakerClient::cleanup_pending_requests() {
    // Two-phase pattern: collect callbacks under lock, invoke outside lock
    // This prevents deadlock if callback tries to send new request
    std::vector<std::function<void()>> cleanup_callbacks;

    // Phase 1: Copy callbacks and clear map (under lock)
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);

        if (!pending_requests_.empty()) {
            spdlog::debug("[Moonraker Client] Cleaning up {} pending requests due to disconnect",
                          pending_requests_.size());

            // Capture callbacks in lambdas
            for (auto& [id, request] : pending_requests_) {
                if (request.error_callback) {
                    MoonrakerError error = MoonrakerError::connection_lost(request.method);
                    std::string method_name = request.method;
                    cleanup_callbacks.push_back([cb = request.error_callback, error,
                                                 method_name]() {
                        try {
                            cb(error);
                        } catch (const std::exception& e) {
                            LOG_ERROR_INTERNAL("[Moonraker Client] Cleanup error callback for {} "
                                               "threw exception: {}",
                                               method_name, e.what());
                        } catch (...) {
                            LOG_ERROR_INTERNAL("[Moonraker Client] Cleanup error callback for {} "
                                               "threw unknown exception",
                                               method_name);
                        }
                    });
                }
            }

            pending_requests_.clear();
        }
    } // Lock released here

    // Phase 2: Invoke callbacks outside lock (safe - callbacks can call send_jsonrpc)
    for (auto& callback : cleanup_callbacks) {
        callback();
    }
}
