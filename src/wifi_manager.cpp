// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
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

#include "wifi_manager.h"

#include "lvgl/lvgl.h"
#include "safe_log.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

// ============================================================================
// Constructor / Destructor
// ============================================================================

WiFiManager::WiFiManager() : scan_timer_(nullptr) {
    spdlog::info("[WiFiManager] Initializing with backend system");

    // Create platform-appropriate backend (already started by factory)
    backend_ = WifiBackend::create();
    if (!backend_) {
        spdlog::error("[WiFiManager] Failed to create WiFi backend");
        return;
    }

    // Check backend status immediately after creation
    if (backend_->is_running()) {
        spdlog::info("[WiFiManager] WiFi backend initialized and running");
    } else {
        spdlog::warn("[WiFiManager] WiFi backend created but not running (may need permissions)");
    }

    // Register event callbacks
    backend_->register_event_callback(
        "SCAN_COMPLETE", [this](const std::string& data) { handle_scan_complete(data); });
    backend_->register_event_callback("CONNECTED",
                                      [this](const std::string& data) { handle_connected(data); });
    backend_->register_event_callback(
        "DISCONNECTED", [this](const std::string& data) { handle_disconnected(data); });
    backend_->register_event_callback(
        "AUTH_FAILED", [this](const std::string& data) { handle_auth_failed(data); });
}

void WiFiManager::init_self_reference(std::shared_ptr<WiFiManager> self) {
    self_ = self;
    spdlog::debug("[WiFiManager] Self-reference initialized for async callback safety");
}

WiFiManager::~WiFiManager() {
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WiFiManager] Destructor called\n");

    // Clean up scanning
    stop_scan();

    // Clear callbacks BEFORE stopping backend
    // Pending lv_async_call operations check for null callbacks before invoking
    scan_callback_ = nullptr;
    connect_callback_ = nullptr;

    // Stop backend (this stops backend threads)
    if (backend_) {
        backend_->stop();
    }
}

// ============================================================================
// Network Scanning
// ============================================================================

std::vector<WiFiNetwork> WiFiManager::scan_once() {
    if (!backend_) {
        spdlog::warn("[WiFiManager] No backend available for scan");
        return {};
    }

    spdlog::debug("[WiFiManager] Performing single scan");

    // Trigger scan and wait briefly for results
    WiFiError scan_result = backend_->trigger_scan();
    if (!scan_result.success()) {
        spdlog::warn("[WiFiManager] Failed to trigger scan: {}", scan_result.technical_msg);
        return {};
    }

    // For synchronous scan, we need to get existing results
    // Note: This may not include the just-triggered scan results immediately
    std::vector<WiFiNetwork> networks;
    WiFiError get_result = backend_->get_scan_results(networks);
    if (!get_result.success()) {
        spdlog::warn("[WiFiManager] Failed to get scan results: {}", get_result.technical_msg);
        return {};
    }

    return networks;
}

void WiFiManager::start_scan(
    std::function<void(const std::vector<WiFiNetwork>&)> on_networks_updated) {
    if (!backend_) {
        spdlog::error("[WiFiManager] No backend available for scanning");
        return;
    }

    spdlog::debug("[WiFiManager] start_scan ENTRY, callback is {}",
                  on_networks_updated ? "NOT NULL" : "NULL");

    scan_callback_ = on_networks_updated;
    spdlog::debug("[WiFiManager] Scan callback registered");

    // Stop existing timer if running
    stop_scan();

    spdlog::info("[WiFiManager] Starting periodic network scan (every 7 seconds)");

    // Create timer for periodic scanning
    scan_timer_ = lv_timer_create(scan_timer_callback, 7000, this);
    spdlog::debug("[WiFiManager] Timer created: {}", (void*)scan_timer_);

    // Trigger immediate scan
    spdlog::debug("[WiFiManager] About to trigger initial scan");
    WiFiError scan_result = backend_->trigger_scan();
    if (!scan_result.success()) {
        spdlog::warn("[WiFiManager] Failed to trigger initial scan: {}", scan_result.technical_msg);
    } else {
        spdlog::debug("[WiFiManager] Initial scan triggered successfully");
    }
}

void WiFiManager::stop_scan() {
    if (scan_timer_) {
        lv_timer_delete(scan_timer_);
        scan_timer_ = nullptr;
        spdlog::info("[WiFiManager] Stopped network scanning");
    }
    // Note: Callback is NOT cleared here - callers can clear it explicitly if needed
}

void WiFiManager::scan_timer_callback(lv_timer_t* timer) {
    WiFiManager* manager = static_cast<WiFiManager*>(lv_timer_get_user_data(timer));
    if (manager && manager->backend_) {
        // Trigger scan - results will arrive via SCAN_COMPLETE event
        WiFiError result = manager->backend_->trigger_scan();
        if (!result.success()) {
            spdlog::warn("[WiFiManager] Periodic scan failed: {}", result.technical_msg);
        }
    }
}

// ============================================================================
// Connection Management
// ============================================================================

void WiFiManager::connect(const std::string& ssid, const std::string& password,
                          std::function<void(bool success, const std::string& error)> on_complete) {
    if (!backend_) {
        spdlog::error("[WiFiManager] No backend available for connection");
        if (on_complete) {
            on_complete(false, "No WiFi backend available");
        }
        return;
    }

    spdlog::info("[WiFiManager] Connecting to '{}'", ssid);

    connect_callback_ = on_complete;
    spdlog::debug("[WiFiManager] Connect callback registered for '{}'", ssid);

    // Use backend's connect method
    WiFiError result = backend_->connect_network(ssid, password);
    if (!result.success()) {
        spdlog::error("[WiFiManager] Backend failed to initiate connection: {}",
                      result.technical_msg);
        if (connect_callback_) {
            connect_callback_(false,
                              result.user_msg.empty() ? result.technical_msg : result.user_msg);
            connect_callback_ = nullptr;
        }
    }
    // Success/failure will be reported via CONNECTED/AUTH_FAILED events
}

void WiFiManager::disconnect() {
    if (!backend_) {
        spdlog::warn("[WiFiManager] No backend available for disconnect");
        return;
    }

    spdlog::info("[WiFiManager] Disconnecting");
    WiFiError result = backend_->disconnect_network();
    if (!result.success()) {
        spdlog::warn("[WiFiManager] Disconnect failed: {}", result.technical_msg);
    }
}

// ============================================================================
// Status Queries
// ============================================================================

bool WiFiManager::is_connected() {
    if (!backend_)
        return false;

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.connected;
}

std::string WiFiManager::get_connected_ssid() {
    if (!backend_)
        return "";

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.ssid;
}

std::string WiFiManager::get_ip_address() {
    if (!backend_)
        return "";

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.ip_address;
}

int WiFiManager::get_signal_strength() {
    if (!backend_)
        return 0;

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.signal_strength;
}

// ============================================================================
// Hardware Detection (Legacy Compatibility)
// ============================================================================

bool WiFiManager::has_hardware() {
    // Backend creation handles hardware availability
    return (backend_ != nullptr);
}

bool WiFiManager::is_enabled() {
    if (!backend_)
        return false;
    return backend_->is_running();
}

bool WiFiManager::set_enabled(bool enabled) {
    if (!backend_)
        return false;

    spdlog::debug("[WiFiManager] set_enabled({})", enabled);

    if (enabled) {
        WiFiError result = backend_->start();
        if (!result.success()) {
            spdlog::error("[WiFiManager] Failed to enable WiFi: {}", result.technical_msg);
        } else {
            spdlog::debug("[WiFiManager] WiFi backend started successfully");
        }
        return result.success();
    } else {
        backend_->stop();
        spdlog::debug("[WiFiManager] WiFi backend stopped");
        return true;
    }
}

// ============================================================================
// Event Handling
// ============================================================================

// Helper struct for async callback dispatch
// Uses weak_ptr to safely handle manager destruction before callback executes
struct ScanCallbackData {
    std::weak_ptr<WiFiManager> manager;
    std::vector<WiFiNetwork> networks;
};

void WiFiManager::handle_scan_complete(const std::string& event_data) {
    (void)event_data; // Unused for now

    spdlog::debug("[WiFiManager] handle_scan_complete ENTRY (backend thread)");

    if (!scan_callback_) {
        spdlog::warn("[WiFiManager] Scan complete but no callback registered");
        return;
    }

    // CRITICAL: This is called from backend thread - must dispatch to LVGL thread!
    spdlog::debug("[WiFiManager] Scan callback is registered, fetching results");
    std::vector<WiFiNetwork> networks;
    WiFiError result = backend_->get_scan_results(networks);

    if (result.success()) {
        spdlog::debug("[WiFiManager] Got {} scan results, dispatching to LVGL thread",
                      networks.size());

        // Store weak_ptr for safe async access
        auto* callback_data = new ScanCallbackData{self_, networks};

        // Dispatch to LVGL thread using lv_async_call
        lv_async_call(
            [](void* user_data) {
                auto* data = static_cast<ScanCallbackData*>(user_data);

                spdlog::debug("[WiFiManager] async_call executing in LVGL thread with {} networks",
                              data->networks.size());

                // Safely check if manager still exists
                if (auto manager = data->manager.lock()) {
                    if (manager->scan_callback_) {
                        manager->scan_callback_(data->networks);
                        spdlog::debug("[WiFiManager] scan_callback_ completed successfully");
                    } else {
                        spdlog::warn(
                            "[WiFiManager] scan_callback_ was cleared before async dispatch");
                    }
                } else {
                    spdlog::debug(
                        "[WiFiManager] Manager destroyed before async callback - safely ignored");
                }

                delete data; // Clean up
            },
            callback_data);

    } else {
        spdlog::warn("[WiFiManager] Failed to get scan results: {}", result.technical_msg);
        // Store weak_ptr for safe async access
        auto* callback_data = new ScanCallbackData{self_, {}};

        lv_async_call(
            [](void* user_data) {
                auto* data = static_cast<ScanCallbackData*>(user_data);

                spdlog::warn("[WiFiManager] async_call: calling callback with empty results");
                if (auto manager = data->manager.lock()) {
                    if (manager->scan_callback_) {
                        manager->scan_callback_({});
                    }
                } else {
                    spdlog::debug(
                        "[WiFiManager] Manager destroyed before async callback - safely ignored");
                }

                delete data;
            },
            callback_data);
    }

    spdlog::debug("[WiFiManager] handle_scan_complete EXIT (dispatch queued)");
}

// Helper struct for connection callback dispatch
// Uses weak_ptr to safely handle manager destruction before callback executes
struct ConnectCallbackData {
    std::weak_ptr<WiFiManager> manager;
    bool success;
    std::string error;
};

void WiFiManager::handle_connected(const std::string& event_data) {
    (void)event_data; // Could parse IP address from event data

    spdlog::debug("[WiFiManager] Connected event received (backend thread)");

    if (!connect_callback_) {
        spdlog::warn("[WiFiManager] Connected event but no callback registered");
        return;
    }

    // Store weak_ptr for safe async access
    auto* data = new ConnectCallbackData{self_, true, ""};
    lv_async_call(
        [](void* user_data) {
            auto* d = static_cast<ConnectCallbackData*>(user_data);
            if (auto manager = d->manager.lock()) {
                if (manager->connect_callback_) {
                    manager->connect_callback_(d->success, d->error);
                    manager->connect_callback_ = nullptr;
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before connect callback - safely ignored");
            }
            delete d;
        },
        data);
}

void WiFiManager::handle_disconnected(const std::string& event_data) {
    (void)event_data; // Could parse reason from event data

    spdlog::debug("[WiFiManager] Disconnected event received (backend thread)");

    if (!connect_callback_) {
        spdlog::warn("[WiFiManager] Disconnected event but no callback registered");
        return;
    }

    // Store weak_ptr for safe async access
    auto* data = new ConnectCallbackData{self_, false, "Disconnected"};
    lv_async_call(
        [](void* user_data) {
            auto* d = static_cast<ConnectCallbackData*>(user_data);
            if (auto manager = d->manager.lock()) {
                if (manager->connect_callback_) {
                    manager->connect_callback_(d->success, d->error);
                    manager->connect_callback_ = nullptr;
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before disconnect callback - safely ignored");
            }
            delete d;
        },
        data);
}

void WiFiManager::handle_auth_failed(const std::string& event_data) {
    (void)event_data; // Could parse specific error from event data

    spdlog::warn("[WiFiManager] Authentication failed event received (backend thread)");

    if (!connect_callback_) {
        spdlog::warn("[WiFiManager] Auth failed event but no callback registered");
        return;
    }

    // Store weak_ptr for safe async access
    auto* data = new ConnectCallbackData{self_, false, "Authentication failed"};
    lv_async_call(
        [](void* user_data) {
            auto* d = static_cast<ConnectCallbackData*>(user_data);
            if (auto manager = d->manager.lock()) {
                if (manager->connect_callback_) {
                    manager->connect_callback_(d->success, d->error);
                    manager->connect_callback_ = nullptr;
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before auth_failed callback - safely ignored");
            }
            delete d;
        },
        data);
}
