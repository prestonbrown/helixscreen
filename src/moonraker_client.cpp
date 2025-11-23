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

using namespace hv;

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
    spdlog::debug("[Moonraker Client] Destructor called");
    // Set flag to prevent callbacks from executing during destruction
    is_destroying_.store(true);
    // Disconnect cleanly to close WebSocket and cleanup all resources
    disconnect();
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
                set_connection_state(ConnectionState::FAILED);
                return;
            }
        } else if (new_state == ConnectionState::CONNECTED) {
            reconnect_attempts_ = 0; // Reset on successful connection
        }

        // Invoke state change callback if set
        if (state_change_callback_) {
            try {
                state_change_callback_(old_state, new_state);
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker Client] State change callback threw exception: {}",
                              e.what());
            } catch (...) {
                spdlog::error("[Moonraker Client] State change callback threw unknown exception");
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

int MoonrakerClient::connect(const char* url, std::function<void()> on_connected,
                             std::function<void()> on_disconnected) {
    // Reset WebSocket state from previous connection attempt BEFORE setting new callbacks.
    // This prevents libhv from rejecting the new open() call if we're already connecting/connected.
    // Note: close() is safe to call even if already closed (idempotent).
    close();

    // Apply connection timeout to libhv (must be called before open())
    setConnectTimeout(connection_timeout_ms_);

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

            const HttpResponsePtr& resp = getHttpResponse();
            spdlog::info("[Moonraker Client] WebSocket connected to {}: {}", url,
                         resp->body.c_str());
            was_connected_ = true;
            set_connection_state(ConnectionState::CONNECTED);

            // Invoke user callback with exception safety
            try {
                on_connected();
            } catch (const std::exception& e) {
                spdlog::error("[Moonraker Client] Connection callback threw exception: {}",
                              e.what());
            } catch (...) {
                spdlog::error("[Moonraker Client] Connection callback threw unknown exception");
            }
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker Client] onopen callback threw unexpected exception: {}",
                          e.what());
        } catch (...) {
            spdlog::error("[Moonraker Client] onopen callback threw unknown exception");
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
                spdlog::error("[Moonraker Client] JSON parse error: {}", e.what());
                return;
            }

            // Handle responses with request IDs (one-time callbacks)
            if (j.contains("id")) {
                // Validate 'id' field type
                if (!j["id"].is_number_integer()) {
                    spdlog::warn("[Moonraker Client] Invalid 'id' type in response: {}",
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
                    spdlog::warn("[Moonraker Client] Invalid 'method' type in notification: {}",
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
                        // Copy all notify callbacks
                        callbacks_to_invoke = notify_callbacks_;
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
                        spdlog::error("[Moonraker Client] Callback for {} threw exception: {}",
                                      method, e.what());
                    } catch (...) {
                        spdlog::error("[Moonraker Client] Callback for {} threw unknown exception",
                                      method);
                    }
                }

                // Klippy disconnected from Moonraker
                if (method == "notify_klippy_disconnected") {
                    spdlog::warn("[Moonraker Client] Klipper disconnected from Moonraker");

                    // Invoke user callback with exception safety
                    try {
                        on_disconnected();
                    } catch (const std::exception& e) {
                        spdlog::error(
                            "[Moonraker Client] Disconnection callback threw exception: {}",
                            e.what());
                    } catch (...) {
                        spdlog::error(
                            "[Moonraker Client] Disconnection callback threw unknown exception");
                    }
                }
                // Klippy reconnected to Moonraker
                else if (method == "notify_klippy_ready") {
                    spdlog::info("[Moonraker Client] Klipper ready");

                    // Invoke user callback with exception safety
                    try {
                        on_connected();
                    } catch (const std::exception& e) {
                        spdlog::error("[Moonraker Client] Connection callback threw exception: {}",
                                      e.what());
                    } catch (...) {
                        spdlog::error(
                            "[Moonraker Client] Connection callback threw unknown exception");
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker Client] onmessage callback threw unexpected exception: {}",
                          e.what());
        } catch (...) {
            spdlog::error("[Moonraker Client] onmessage callback threw unknown exception");
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

                // Check if this is a reconnection scenario
                if (current != ConnectionState::FAILED) {
                    set_connection_state(ConnectionState::RECONNECTING);
                }

                // Invoke user callback with exception safety
                try {
                    on_disconnected();
                } catch (const std::exception& e) {
                    spdlog::error("[Moonraker Client] Disconnection callback threw exception: {}",
                                  e.what());
                } catch (...) {
                    spdlog::error(
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
                    spdlog::error("[Moonraker Client] Disconnection callback threw exception: {}",
                                  e.what());
                } catch (...) {
                    spdlog::error(
                        "[Moonraker Client] Disconnection callback threw unknown exception");
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker Client] onclose callback threw unexpected exception: {}",
                          e.what());
        } catch (...) {
            spdlog::error("[Moonraker Client] onclose callback threw unknown exception");
        }
    };

    // Error callback - handles WebSocket protocol errors
    // Wrap entire callback body in try-catch to prevent any exception from escaping
    onerror = [this](const std::string& err_msg) {
        try {
            // Prevent callback execution if client is being destroyed
            if (is_destroying_.load()) {
                return;
            }

            spdlog::error("[Moonraker Client] WebSocket error: {}", err_msg);

            // Update connection state to indicate error
            ConnectionState current = connection_state_.load();
            if (current == ConnectionState::CONNECTING) {
                set_connection_state(ConnectionState::DISCONNECTED);
            } else if (current == ConnectionState::CONNECTED) {
                set_connection_state(ConnectionState::RECONNECTING);
            }
        } catch (const std::exception& e) {
            spdlog::error("[Moonraker Client] onerror callback threw unexpected exception: {}",
                          e.what());
        } catch (...) {
            spdlog::error("[Moonraker Client] onerror callback threw unknown exception");
        }
    };

    // WebSocket ping (keepalive) - use configured interval
    setPingInterval(keepalive_interval_ms_);

    // Automatic reconnection with exponential backoff - use configured values
    reconn_setting_t reconn;
    reconn_setting_init(&reconn);
    reconn.min_delay = reconnect_min_delay_ms_;
    reconn.max_delay = reconnect_max_delay_ms_;
    reconn.delay_policy = 2; // Exponential backoff
    setReconnect(&reconn);

    // Connect
    http_headers headers;
    return open(url, headers);
}

void MoonrakerClient::register_notify_update(std::function<void(json)> cb) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    notify_callbacks_.push_back(cb);
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

int MoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                  std::function<void(json)> cb) {
    // Forward to new overload with null error callback
    return send_jsonrpc(method, params, cb, nullptr, 0);
}

int MoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                  std::function<void(json)> success_cb,
                                  std::function<void(const MoonrakerError&)> error_cb,
                                  uint32_t timeout_ms) {
    // Atomically fetch and increment to avoid race condition in concurrent calls
    uint64_t id = request_id_.fetch_add(1);

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
            spdlog::warn("[Moonraker Client] Request ID {} already has a registered callback", id);
            return -1;
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
    return result;
}

int MoonrakerClient::gcode_script(const std::string& gcode) {
    json params = {{"script", gcode}};
    return send_jsonrpc("printer.gcode.script", params);
}

void MoonrakerClient::discover_printer(std::function<void()> on_complete) {
    spdlog::info("[Moonraker Client] Starting printer auto-discovery");

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
                std::string moonraker_version = result.value("moonraker_version", "unknown");

                spdlog::info("[Moonraker Client] Moonraker version: {}", moonraker_version);
                spdlog::info("[Moonraker Client] Klippy version: {}", klippy_version);

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
                    std::string software_version = result.value("software_version", "unknown");
                    std::string state_message = result.value("state_message", "");

                    spdlog::info("[Moonraker Client] Printer hostname: {}", hostname_);
                    spdlog::info("[Moonraker Client] Klipper software version: {}",
                                 software_version);
                    if (!state_message.empty()) {
                        spdlog::info("[Moonraker Client] Printer state: {}", state_message);
                    }
                }

                // Step 4: Subscribe to all discovered objects + core objects
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

                json subscribe_params = {{"objects", subscription_objects}};

                send_jsonrpc(
                    "printer.objects.subscribe", subscribe_params,
                    [on_complete, subscription_objects](json sub_response) {
                        if (sub_response.contains("result")) {
                            spdlog::info(
                                "[Moonraker Client] Subscription complete: {} objects subscribed",
                                subscription_objects.size());
                        } else if (sub_response.contains("error")) {
                            spdlog::error("[Moonraker Client] Subscription failed: {}",
                                          sub_response["error"].dump());
                        }

                        // Discovery complete
                        on_complete();
                    });
            });
        });
    });
}

void MoonrakerClient::parse_objects(const json& objects) {
    heaters_.clear();
    sensors_.clear();
    fans_.clear();
    leds_.clear();

    for (const auto& obj : objects) {
        std::string name = obj.template get<std::string>();

        // Extruders (controllable heaters)
        // Match "extruder", "extruder1", etc., but NOT "extruder_stepper"
        if (name.rfind("extruder", 0) == 0 && name.rfind("extruder_stepper", 0) != 0) {
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

    spdlog::info("[Moonraker Client] Discovered: {} heaters, {} sensors, {} fans, {} LEDs",
                 heaters_.size(), sensors_.size(), fans_.size(), leds_.size());

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
        active_bed_mesh_.y_count = active_bed_mesh_.probed_matrix.size();
        active_bed_mesh_.x_count =
            active_bed_mesh_.probed_matrix.empty() ? 0 : active_bed_mesh_.probed_matrix[0].size();
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

                // Capture callback in lambda if present
                if (request.error_callback) {
                    MoonrakerError error =
                        MoonrakerError::timeout(request.method, request.timeout_ms);
                    std::string method_name = request.method;
                    timed_out_callbacks.push_back(
                        [cb = request.error_callback, error, method_name]() {
                            try {
                                cb(error);
                            } catch (const std::exception& e) {
                                spdlog::error("[Moonraker Client] Timeout error callback for {} "
                                              "threw exception: {}",
                                              method_name, e.what());
                            } catch (...) {
                                spdlog::error("[Moonraker Client] Timeout error callback for {} "
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
                    cleanup_callbacks.push_back(
                        [cb = request.error_callback, error, method_name]() {
                            try {
                                cb(error);
                            } catch (const std::exception& e) {
                                spdlog::error("[Moonraker Client] Cleanup error callback for {} "
                                              "threw exception: {}",
                                              method_name, e.what());
                            } catch (...) {
                                spdlog::error("[Moonraker Client] Cleanup error callback for {} "
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
