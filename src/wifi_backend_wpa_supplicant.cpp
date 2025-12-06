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

#include "wifi_backend_wpa_supplicant.h"

#include "ui_error_reporting.h"

#include "spdlog/spdlog.h"

#ifndef __APPLE__
// ============================================================================
// Linux Implementation: Full wpa_supplicant integration
// ============================================================================

#include "wpa_ctrl.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

WifiBackendWpaSupplicant::WifiBackendWpaSupplicant()
    : hv::EventLoopThread(NULL), conn(NULL), mon_conn(NULL) // Initialize monitor connection
{
    spdlog::debug("[WifiBackend] Initialized (wpa_supplicant mode)");
}

WifiBackendWpaSupplicant::~WifiBackendWpaSupplicant() {
    spdlog::trace("[WifiBackend] Destructor called");
    stop();
    cleanup_wpa(); // SECURITY: Ensure resources are cleaned up
}

WiFiError WifiBackendWpaSupplicant::start() {
    spdlog::debug("[WifiBackend] Starting wpa_supplicant backend...");

    // Pre-flight checks before starting event loop
    WiFiError preflight_result = check_system_prerequisites();
    if (!preflight_result.success()) {
        // User-facing critical error - wpa_supplicant not running or no permissions
        // In silent mode (e.g., HomePanel signal probe), only log - don't show modals
        if (is_silent()) {
            spdlog::debug("[WifiBackend] Pre-flight failed (silent mode): {}",
                          preflight_result.technical_msg);
        } else if (preflight_result.result == WiFiResult::SERVICE_NOT_RUNNING) {
            NOTIFY_ERROR_MODAL("WiFi Service Not Running",
                               "wpa_supplicant is not running. WiFi features unavailable.");
        } else if (preflight_result.result == WiFiResult::PERMISSION_DENIED) {
            NOTIFY_ERROR_MODAL("WiFi Permission Denied", "{}",
                               preflight_result.user_msg.empty() ? preflight_result.technical_msg
                                                                 : preflight_result.user_msg);
        } else {
            LOG_ERROR_INTERNAL("Pre-flight check failed: {}", preflight_result.technical_msg);
        }
        return preflight_result;
    }

    if (isRunning()) {
        // Event loop already running - schedule initialization in loop thread
        spdlog::debug("[WifiBackend] Already running, scheduling init_wpa in loop");
        loop()->runInLoop(std::bind(&WifiBackendWpaSupplicant::init_wpa, this));
        return WiFiErrorHelper::success();
    } else {
        // Start new event loop thread with initialization callback
        spdlog::info("[WifiBackend] Starting event loop thread");
        try {
            hv::EventLoopThread::start(true, [this]() -> int {
                WifiBackendWpaSupplicant::init_wpa();
                return 0; // Return int as expected by libhv (0 = success)
            });
            spdlog::info("[WifiBackend] Event loop started successfully");
            return WiFiErrorHelper::success();
        } catch (const std::exception& e) {
            return WiFiErrorHelper::connection_failed("Failed to start event loop: " +
                                                      std::string(e.what()));
        }
    }
}

void WifiBackendWpaSupplicant::stop() {
    if (!isRunning()) {
        spdlog::trace("[WifiBackend] Not running, nothing to stop");
        return;
    }

    spdlog::info("[WifiBackend] Stopping event loop thread");
    hv::EventLoopThread::stop(true); // Block until thread terminates
    spdlog::trace("[WifiBackend] Event loop stopped");
}

void WifiBackendWpaSupplicant::register_event_callback(
    const std::string& name, std::function<void(const std::string&)> callback) {
    // THREAD SAFETY: Lock callbacks map during access
    std::lock_guard<std::mutex> lock(callbacks_mutex_);

    const auto& entry = callbacks.find(name);
    if (entry == callbacks.end()) {
        callbacks.insert({name, callback});
        spdlog::debug("[WifiBackend] Registered callback '{}'", name);
    } else {
        // Callback already exists - could replace it, but parent doesn't
        LOG_WARN_INTERNAL("Callback '{}' already registered (not replacing)", name);
    }
}

// ============================================================================
// System Validation and Permission Checking
// ============================================================================

WiFiError WifiBackendWpaSupplicant::check_system_prerequisites() {
    spdlog::debug("[WifiBackend] Performing system prerequisites check");

    // 1. Check WiFi hardware availability
    WiFiError hw_result = check_wifi_hardware();
    if (!hw_result.success()) {
        return hw_result;
    }

    // 2. Check if any wpa_supplicant sockets exist
    std::vector<std::string> socket_paths = {"/run/wpa_supplicant", "/var/run/wpa_supplicant"};
    bool socket_found = false;
    std::string accessible_socket;

    for (const auto& base_path : socket_paths) {
        if (fs::exists(base_path) && fs::is_directory(base_path)) {
            spdlog::debug("[WifiBackend] Found wpa_supplicant directory: {}", base_path);

            // Look for interface sockets
            for (const auto& entry : fs::directory_iterator(base_path)) {
                if (fs::is_socket(entry.path())) {
                    std::string socket_path = entry.path().string();

                    // Skip P2P sockets
                    if (socket_path.find("p2p") == std::string::npos) {
                        socket_found = true;
                        spdlog::debug("[WifiBackend] Found wpa_supplicant socket: {}", socket_path);

                        // Check permissions for this socket
                        WiFiError perm_result = check_socket_permissions(socket_path);
                        if (perm_result.success()) {
                            accessible_socket = socket_path;
                            break;
                        } else {
                            LOG_WARN_INTERNAL("Socket {} permission check failed: {}", socket_path,
                                              perm_result.technical_msg);
                        }
                    }
                }
            }
            if (!accessible_socket.empty())
                break;
        }
    }

    if (!socket_found) {
        return WiFiErrorHelper::service_not_running("wpa_supplicant (no control sockets found)");
    }

    if (accessible_socket.empty()) {
        return WiFiErrorHelper::permission_denied("Found wpa_supplicant sockets but cannot access "
                                                  "them - check user permissions (netdev group)");
    }

    spdlog::debug("[WifiBackend] System prerequisites check passed - accessible socket: {}",
                  accessible_socket);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::check_socket_permissions(const std::string& socket_path) {
    spdlog::trace("[WifiBackend] Checking permissions for socket: {}", socket_path);

    // Try to open a test connection
    struct wpa_ctrl* test_ctrl = wpa_ctrl_open(socket_path.c_str());
    if (!test_ctrl) {
        // Get more specific error information
        int err = errno;
        std::string error_detail = "wpa_ctrl_open failed: " + std::string(strerror(err));

        if (err == EACCES || err == EPERM) {
            return WiFiErrorHelper::permission_denied(error_detail +
                                                      " (try adding user to netdev group)");
        } else if (err == ENOENT) {
            return WiFiErrorHelper::service_not_running("wpa_supplicant socket not found");
        } else if (err == ECONNREFUSED) {
            return WiFiErrorHelper::service_not_running("wpa_supplicant daemon not responding");
        } else {
            return WiFiErrorHelper::connection_failed(error_detail);
        }
    }

    // Test connection successful - close it immediately
    wpa_ctrl_close(test_ctrl);
    spdlog::debug("[WifiBackend] Socket permission check passed: {}", socket_path);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::check_wifi_hardware() {
    spdlog::trace("[WifiBackend] Checking WiFi hardware availability");

    // Check for common WiFi interface patterns in /sys/class/net
    bool wifi_found = false;
    std::string interface_name;

    try {
        const std::string net_path = "/sys/class/net";
        if (fs::exists(net_path)) {
            for (const auto& entry : fs::directory_iterator(net_path)) {
                std::string iface = entry.path().filename().string();

                // Check for common WiFi interface patterns
                if (iface.find("wlan") == 0 || iface.find("wlp") == 0 || iface.find("wlx") == 0 ||
                    iface.find("wifi") == 0) {
                    // Verify it's a wireless interface by checking for wireless directory
                    std::string wireless_path = entry.path().string() + "/wireless";
                    if (fs::exists(wireless_path)) {
                        wifi_found = true;
                        interface_name = iface;
                        spdlog::debug("[WifiBackend] Found WiFi interface: {}", iface);
                        break;
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOG_WARN_INTERNAL("Error checking WiFi interfaces: {}", e.what());
        // Don't fail entirely - this might be a permission issue or unusual system
    }

    if (!wifi_found) {
        return WiFiErrorHelper::hardware_not_available();
    }

    // Check RF-kill status
    try {
        const std::string rfkill_path = "/sys/class/rfkill";
        if (fs::exists(rfkill_path)) {
            for (const auto& entry : fs::directory_iterator(rfkill_path)) {
                std::string type_file = entry.path().string() + "/type";
                if (fs::exists(type_file)) {
                    std::ifstream type_stream(type_file);
                    std::string type;
                    if (type_stream >> type && type == "wlan") {
                        // Check if soft-blocked
                        std::string soft_file = entry.path().string() + "/soft";
                        if (fs::exists(soft_file)) {
                            std::ifstream soft_stream(soft_file);
                            int soft_blocked;
                            if (soft_stream >> soft_blocked && soft_blocked == 1) {
                                return WiFiErrorHelper::rf_kill_blocked();
                            }
                        }
                        break;
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOG_WARN_INTERNAL("Error checking RF-kill status: {}", e.what());
        // Continue - RF-kill check is nice-to-have
    }

    spdlog::debug("[WifiBackend] WiFi hardware check passed - interface: {}", interface_name);
    return WiFiErrorHelper::success();
}

// ============================================================================
// wpa_supplicant Communication
// ============================================================================

void WifiBackendWpaSupplicant::init_wpa() {
    spdlog::trace("[WifiBackend] init_wpa() called in event loop thread");

    // Socket discovery: Try common paths
    std::string wpa_socket;
    bool socket_found = false;

    // Try modern systemd path first: /run/wpa_supplicant
    std::string base_path = "/run/wpa_supplicant";
    if (fs::exists(base_path) && fs::is_directory(base_path)) {
        spdlog::debug("[WifiBackend] Searching for wpa_supplicant socket in {}", base_path);

        for (const auto& entry : fs::directory_iterator(base_path)) {
            if (fs::is_socket(entry.path())) {
                std::string socket_path = entry.path().string();

                // Filter out P2P sockets (e.g., p2p-dev-wlan0)
                if (socket_path.find("p2p") == std::string::npos) {
                    wpa_socket = socket_path;
                    socket_found = true;
                    spdlog::debug("[WifiBackend] Found wpa_supplicant socket: {}", wpa_socket);
                    break;
                }
            }
        }
    }

    // Try older path if not found: /var/run/wpa_supplicant
    if (!socket_found) {
        base_path = "/var/run/wpa_supplicant";
        if (fs::exists(base_path) && fs::is_directory(base_path)) {
            spdlog::debug("[WifiBackend] Searching for wpa_supplicant socket in {}", base_path);

            for (const auto& entry : fs::directory_iterator(base_path)) {
                if (fs::is_socket(entry.path())) {
                    std::string socket_path = entry.path().string();

                    // Filter out P2P sockets
                    if (socket_path.find("p2p") == std::string::npos) {
                        wpa_socket = socket_path;
                        socket_found = true;
                        spdlog::debug("[WifiBackend] Found wpa_supplicant socket: {}", wpa_socket);
                        break;
                    }
                }
            }
        }
    }

    if (!socket_found) {
        LOG_ERROR_INTERNAL("Could not find wpa_supplicant socket in /run or /var/run");
        LOG_ERROR_INTERNAL("Is wpa_supplicant daemon running?");
        dispatch_event("INIT_FAILED", "wpa_supplicant socket not found");
        return;
    }

    // Open control connection (for sending commands)
    if (conn == NULL) {
        conn = wpa_ctrl_open(wpa_socket.c_str());
        if (conn == NULL) {
            LOG_ERROR_INTERNAL("Failed to open control connection to {}", wpa_socket);
            dispatch_event("INIT_FAILED", "Failed to connect to wpa_supplicant");
            return;
        }
        spdlog::debug("[WifiBackend] Opened control connection");
    }

    // Open monitor connection (for receiving events)
    mon_conn = wpa_ctrl_open(wpa_socket.c_str()); // SECURITY: Use member variable to prevent leak
    if (mon_conn == NULL) {
        LOG_ERROR_INTERNAL("Failed to open monitor connection to {}", wpa_socket);
        dispatch_event("INIT_FAILED", "Failed to connect to wpa_supplicant monitor");
        return;
    }

    // Attach to wpa_supplicant event stream
    if (wpa_ctrl_attach(mon_conn) != 0) {
        LOG_ERROR_INTERNAL("Failed to attach to wpa_supplicant events");
        dispatch_event("INIT_FAILED", "Failed to attach to wpa_supplicant events");
        wpa_ctrl_close(mon_conn);
        mon_conn = NULL; // Clear member to avoid double-close
        return;
    }
    spdlog::debug("[WifiBackend] Attached to wpa_supplicant event stream");

    // Get file descriptor for monitor socket
    int monfd = wpa_ctrl_get_fd(mon_conn);
    if (monfd < 0) {
        LOG_ERROR_INTERNAL("Failed to get monitor socket file descriptor");
        dispatch_event("INIT_FAILED", "Failed to initialize wpa_supplicant communication");
        wpa_ctrl_close(mon_conn);
        mon_conn = NULL; // Clear member to avoid double-close
        return;
    }
    spdlog::trace("[WifiBackend] Monitor socket fd: {}", monfd);

    // Register with libhv event loop for async I/O
    hio_t* io = hio_get(loop()->loop(), monfd);
    if (io == NULL) {
        LOG_ERROR_INTERNAL("Failed to register monitor socket with libhv");
        dispatch_event("INIT_FAILED", "Failed to initialize WiFi event handling");
        wpa_ctrl_close(mon_conn);
        return;
    }

    // Set up I/O callbacks
    hio_set_context(io, this); // Store 'this' pointer for static callback
    hio_setcb_read(io, WifiBackendWpaSupplicant::_handle_wpa_events); // Static trampoline
    hio_read_start(io); // Start monitoring socket for events

    spdlog::debug("[WifiBackend] wpa_supplicant backend initialized successfully");
}

void WifiBackendWpaSupplicant::cleanup_wpa() {
    spdlog::trace("[WifiBackend] Cleaning up wpa_supplicant connections");

    // Close monitor connection first (detach from events)
    if (mon_conn) {
        spdlog::trace("[WifiBackend] Detaching from wpa_supplicant events");
        wpa_ctrl_detach(mon_conn); // Detach from event stream
        wpa_ctrl_close(mon_conn);  // Close monitor connection
        mon_conn = NULL;
    }

    // Close control connection
    if (conn) {
        spdlog::trace("[WifiBackend] Closing wpa_supplicant control connection");
        wpa_ctrl_close(conn);
        conn = NULL;
    }

    spdlog::debug("[WifiBackend] wpa_supplicant connections cleaned up");
}

std::string WifiBackendWpaSupplicant::map_event_to_callback(const std::string& event) {
    // Map wpa_supplicant events to registered callback names
    // Only actionable events are mapped; informational events return empty string

    if (event.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
        return "SCAN_COMPLETE";
    }
    if (event.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
        return "CONNECTED";
    }
    if (event.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
        return "DISCONNECTED";
    }
    // Auth failures can come in multiple forms
    if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos &&
        event.find("WRONG_KEY") != std::string::npos) {
        return "AUTH_FAILED";
    }
    if (event.find("CTRL-EVENT-AUTH-REJECT") != std::string::npos) {
        return "AUTH_FAILED";
    }

    // Informational events - no callback needed
    // SCAN-STARTED, BSS-ADDED, BSS-REMOVED, REGDOM-CHANGE, SIGNAL-CHANGE, etc.
    return "";
}

void WifiBackendWpaSupplicant::handle_wpa_events(void* data, int len) {
    if (data == nullptr || len <= 0) {
        LOG_WARN_INTERNAL("Received empty event");
        return;
    }

    // Convert to string (may contain newlines)
    std::string event = std::string(static_cast<char*>(data), len);

    spdlog::trace("[WifiBackend] Event received: {}", event);

    // Determine which callback (if any) should receive this event
    std::string callback_name = map_event_to_callback(event);

    if (callback_name.empty()) {
        // Informational event - no callback needed
        spdlog::trace("[WifiBackend] Ignoring informational event (no matching callback)");
        return;
    }

    // THREAD SAFETY: Lock callbacks during lookup and dispatch
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = callbacks.find(callback_name);
    if (it != callbacks.end()) {
        spdlog::debug("[WifiBackend] Dispatching {} event to callback", callback_name);
        try {
            it->second(event);
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("Exception in callback '{}': {}", callback_name, e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("Unknown exception in callback '{}'", callback_name);
        }
    } else {
        spdlog::trace("[WifiBackend] No callback registered for event type: {}", callback_name);
    }
}

void WifiBackendWpaSupplicant::_handle_wpa_events(hio_t* io, void* data, int readbyte) {
    // Static trampoline: Extract instance pointer and forward to member function
    WifiBackendWpaSupplicant* instance = static_cast<WifiBackendWpaSupplicant*>(hio_context(io));
    if (instance) {
        instance->handle_wpa_events(data, readbyte);
    } else {
        LOG_ERROR_INTERNAL("Static callback invoked with NULL context");
    }
}

void WifiBackendWpaSupplicant::dispatch_event(const std::string& event_name,
                                              const std::string& message) {
    // Dispatch to a specific registered callback (for synthetic events like INIT_FAILED)
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = callbacks.find(event_name);
    if (it != callbacks.end()) {
        spdlog::debug("[WifiBackend] Dispatching synthetic event '{}': {}", event_name, message);
        try {
            it->second(message);
        } catch (const std::exception& e) {
            LOG_ERROR_INTERNAL("Exception in callback '{}': {}", event_name, e.what());
        } catch (...) {
            LOG_ERROR_INTERNAL("Unknown exception in callback '{}'", event_name);
        }
    }
}

// Helper function to sanitize commands for logging (remove passwords)
static std::string sanitize_command_for_log(const std::string& cmd) {
    // Check if command contains password
    if (cmd.find(" psk ") != std::string::npos) {
        size_t psk_pos = cmd.find(" psk ");
        return cmd.substr(0, psk_pos + 5) + "\"[REDACTED]\"";
    }
    return cmd;
}

std::string WifiBackendWpaSupplicant::send_command(const std::string& cmd) {
    if (conn == NULL) {
        LOG_WARN_INTERNAL("send_command called but not connected to wpa_supplicant");
        return "";
    }

    char resp[4096];
    size_t len = sizeof(resp) - 1;

    // SECURITY: Don't log passwords
    std::string safe_cmd = sanitize_command_for_log(cmd);
    spdlog::trace("[WifiBackend] Sending command: {}", safe_cmd);

    int result = wpa_ctrl_request(conn, cmd.c_str(), cmd.length(), resp, &len, NULL);
    if (result != 0) {
        LOG_ERROR_INTERNAL("Command failed: {} (error code: {})", safe_cmd, result);
        return "";
    }

    // SECURITY: Validate len before using as array index
    if (len >= sizeof(resp)) {
        LOG_ERROR_INTERNAL("Response too large: {} bytes", len);
        return "";
    }

    // Null-terminate response
    resp[len] = '\0';

    // SECURITY: Don't log password responses
    if (cmd.find(" psk ") == std::string::npos) {
        spdlog::trace("[WifiBackend] Command response ({} bytes): {}", len, std::string(resp, len));
    } else {
        spdlog::trace("[WifiBackend] Command response ({} bytes): [REDACTED]", len);
    }

    return std::string(resp, len);
}

// ============================================================================
// WifiBackend Interface Implementation
// ============================================================================

bool WifiBackendWpaSupplicant::is_running() const {
    return const_cast<WifiBackendWpaSupplicant*>(this)->isRunning();
}

WiFiError WifiBackendWpaSupplicant::trigger_scan() {
    if (!isRunning()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string result = send_command("SCAN");
    if (result == "OK\n") {
        spdlog::debug("[WifiBackend] Scan triggered successfully");
        return WiFiErrorHelper::success();
    } else if (result.empty()) {
        return WiFiErrorHelper::connection_failed("No response from wpa_supplicant SCAN command");
    } else if (result.find("FAIL") != std::string::npos) {
        return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant SCAN command failed: " + result,
                         "Failed to start network scan", "Check WiFi interface status");
    } else {
        spdlog::warn("[WifiBackend] Unexpected scan response: {}", result);
        return WiFiError(WiFiResult::BACKEND_ERROR, "Unexpected scan response: " + result,
                         "Network scan returned unexpected result");
    }
}

// Deduplicate networks by SSID, keeping the strongest signal for each.
// In mesh WiFi systems, multiple APs broadcast the same SSID - show only the best one.
static std::vector<WiFiNetwork> deduplicate_by_ssid(std::vector<WiFiNetwork>& networks) {
    std::unordered_map<std::string, size_t> best_index_by_ssid;

    for (size_t i = 0; i < networks.size(); ++i) {
        const auto& net = networks[i];
        auto it = best_index_by_ssid.find(net.ssid);
        if (it == best_index_by_ssid.end()) {
            best_index_by_ssid[net.ssid] = i;
        } else if (net.signal_strength > networks[it->second].signal_strength) {
            it->second = i;
        }
    }

    std::vector<WiFiNetwork> result;
    result.reserve(best_index_by_ssid.size());
    for (const auto& [ssid, idx] : best_index_by_ssid) {
        result.push_back(networks[idx]);
    }

    if (result.size() < networks.size()) {
        spdlog::debug("[WifiBackend] Deduplicated {} networks to {} unique SSIDs", networks.size(),
                      result.size());
    }

    return result;
}

WiFiError WifiBackendWpaSupplicant::get_scan_results(std::vector<WiFiNetwork>& networks) {
    if (!isRunning()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string raw = send_command("SCAN_RESULTS");
    if (raw.empty()) {
        return WiFiErrorHelper::connection_failed(
            "No response from wpa_supplicant SCAN_RESULTS command");
    }

    if (raw.find("FAIL") != std::string::npos) {
        return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant SCAN_RESULTS failed: " + raw,
                         "Failed to retrieve scan results");
    }

    try {
        networks = parse_scan_results(raw);
        networks = deduplicate_by_ssid(networks);
        spdlog::debug("[WifiBackend] Retrieved {} unique networks", networks.size());
        return WiFiErrorHelper::success();
    } catch (const std::exception& e) {
        return WiFiError(WiFiResult::BACKEND_ERROR,
                         "Failed to parse scan results: " + std::string(e.what()),
                         "Error processing network scan data");
    }
}

// Helper function to validate and escape wpa_supplicant strings
static std::string validate_wpa_string(const std::string& input, const std::string& field_name) {
    // Check for dangerous characters that could enable command injection
    for (char c : input) {
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c < 32 || c == 127) {
            LOG_ERROR_INTERNAL("Invalid character in {}: ASCII {}", field_name, (int)c);
            return "";
        }
    }

    // Additional checks for reasonable limits
    if (input.empty() || input.length() > 255) {
        LOG_ERROR_INTERNAL("Invalid {} length: {}", field_name, input.length());
        return "";
    }

    return input;
}

WiFiError WifiBackendWpaSupplicant::connect_network(const std::string& ssid,
                                                    const std::string& password) {
    if (!isRunning()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    // SECURITY: Validate inputs to prevent command injection
    std::string clean_ssid = validate_wpa_string(ssid, "SSID");
    if (clean_ssid.empty()) {
        return WiFiError(WiFiResult::INVALID_PARAMETERS,
                         "SSID contains invalid characters (quotes, control chars, etc.)",
                         "Invalid network name", "Check that the network name is correct");
    }

    std::string clean_password = validate_wpa_string(password, "password");
    if (!password.empty() && clean_password.empty()) {
        return WiFiErrorHelper::authentication_failed(ssid +
                                                      " (password contains invalid characters)");
    }

    spdlog::info("[WifiBackend] Connecting to network '{}'", clean_ssid);

    // Step 1: Add new network (get network ID)
    std::string add_result = send_command("ADD_NETWORK");
    if (add_result.empty() || add_result == "FAIL\n") {
        NOTIFY_ERROR("Failed to save WiFi network");
        return WiFiErrorHelper::connection_failed("Failed to add network to wpa_supplicant");
    }

    // Parse network ID (should be a number)
    std::string network_id = add_result;
    // Remove trailing newline
    if (!network_id.empty() && network_id.back() == '\n') {
        network_id.pop_back();
    }

    // SECURITY: Validate network ID is actually a number
    for (char c : network_id) {
        if (!std::isdigit(c)) {
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             "wpa_supplicant returned invalid network ID: " + network_id,
                             "Internal WiFi error", "Try restarting WiFi services");
        }
    }

    spdlog::debug("[WifiBackend] Added network with ID: {}", network_id);

    // Step 2: Set SSID
    std::string set_ssid_cmd = "SET_NETWORK " + network_id + " ssid \"" + clean_ssid + "\"";
    std::string ssid_result = send_command(set_ssid_cmd);
    if (ssid_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to set SSID: {}", ssid_result);
        // Clean up: remove the network
        send_command("REMOVE_NETWORK " + network_id);
        NOTIFY_ERROR("Failed to save WiFi network");
        return WiFiErrorHelper::connection_failed("Failed to configure network SSID");
    }

    // Step 3: Set security (PSK for secured networks, key_mgmt for open)
    if (clean_password.empty()) {
        // Open network
        std::string set_open_cmd = "SET_NETWORK " + network_id + " key_mgmt NONE";
        std::string open_result = send_command(set_open_cmd);
        if (open_result != "OK\n") {
            LOG_ERROR_INTERNAL("Failed to set open security: {}", open_result);
            send_command("REMOVE_NETWORK " + network_id);
            NOTIFY_ERROR("Failed to save WiFi network");
            return WiFiErrorHelper::connection_failed("Failed to configure open network security");
        }
        spdlog::debug("[WifiBackend] Configured as open network");
    } else {
        // Secured network with PSK
        std::string set_psk_cmd = "SET_NETWORK " + network_id + " psk \"" + clean_password + "\"";
        std::string psk_result = send_command(set_psk_cmd);
        if (psk_result != "OK\n") {
            LOG_ERROR_INTERNAL(
                "Failed to set PSK"); // Don't log the actual result (may contain password)
            send_command("REMOVE_NETWORK " + network_id);
            NOTIFY_ERROR("Failed to connect to '{}'. Check password.", clean_ssid);
            return WiFiErrorHelper::authentication_failed(ssid);
        }
        spdlog::debug("[WifiBackend] Configured with PSK");
    }

    // Step 4: Enable and select network
    std::string enable_cmd = "ENABLE_NETWORK " + network_id;
    std::string enable_result = send_command(enable_cmd);
    if (enable_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to enable network: {}", enable_result);
        send_command("REMOVE_NETWORK " + network_id);
        NOTIFY_ERROR("Failed to save WiFi network");
        return WiFiErrorHelper::connection_failed("Failed to enable network configuration");
    }
    spdlog::debug("[WifiBackend] Network {} enabled, selecting for connection", network_id);

    // Step 5: Select network (disconnect others)
    std::string select_cmd = "SELECT_NETWORK " + network_id;
    std::string select_result = send_command(select_cmd);
    if (select_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to select network: {}", select_result);
        send_command("REMOVE_NETWORK " + network_id);
        NOTIFY_ERROR("Failed to connect to '{}'", clean_ssid);
        return WiFiErrorHelper::connection_failed("Failed to select network for connection");
    }

    spdlog::info("[WifiBackend] Network configuration complete, connecting to '{}'", ssid);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::disconnect_network() {
    if (!isRunning()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string result = send_command("DISCONNECT");
    if (result == "OK\n") {
        spdlog::debug("[WifiBackend] Disconnect successful");
        return WiFiErrorHelper::success();
    } else if (result.empty()) {
        return WiFiErrorHelper::connection_failed(
            "No response from wpa_supplicant DISCONNECT command");
    } else {
        return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant DISCONNECT failed: " + result,
                         "Failed to disconnect from network");
    }
}

WifiBackend::ConnectionStatus WifiBackendWpaSupplicant::get_status() {
    ConnectionStatus status = {};
    status.connected = false;

    std::string raw_status = send_command("STATUS");
    if (raw_status.empty()) {
        LOG_WARN_INTERNAL("Empty STATUS response");
        return status;
    }

    // Parse key=value pairs from STATUS output
    std::istringstream stream(raw_status);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty())
            continue;

        // Find key=value separator
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos)
            continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        if (key == "wpa_state") {
            // Check if fully connected
            status.connected = (value == "COMPLETED");
        } else if (key == "ssid") {
            status.ssid = value;
        } else if (key == "bssid") {
            status.bssid = value;
        } else if (key == "ip_address") {
            status.ip_address = value;
        } else if (key == "freq") {
            // Frequency in MHz - could be useful for signal info
            try {
                int freq_mhz = std::stoi(value);
                (void)freq_mhz; // Available if needed later
            } catch (const std::exception&) {
                // Ignore parse errors
            }
        }
    }

    // If connected, get additional signal info via SIGNAL_POLL
    if (status.connected) {
        std::string signal_raw = send_command("SIGNAL_POLL");
        if (!signal_raw.empty()) {
            std::istringstream signal_stream(signal_raw);
            std::string signal_line;

            while (std::getline(signal_stream, signal_line)) {
                size_t eq_pos = signal_line.find('=');
                if (eq_pos == std::string::npos)
                    continue;

                std::string key = signal_line.substr(0, eq_pos);
                std::string value = signal_line.substr(eq_pos + 1);

                if (key == "RSSI") {
                    try {
                        int rssi_dbm = std::stoi(value);
                        status.signal_strength = dbm_to_percentage(rssi_dbm);
                    } catch (const std::exception& e) {
                        spdlog::trace("[WifiBackend] Invalid RSSI value '{}': {}", value, e.what());
                    }
                    break; // Found what we need
                }
            }
        }
    }

    // Only log when status actually changes (reduces log noise)
    bool status_changed =
        (status.connected != last_logged_status_.connected ||
         status.ssid != last_logged_status_.ssid ||
         status.ip_address != last_logged_status_.ip_address ||
         std::abs(status.signal_strength - last_logged_status_.signal_strength) > 5);

    if (status_changed) {
        spdlog::debug("[WifiBackend] Status: connected={} ssid='{}' ip='{}' signal={}%",
                      status.connected, status.ssid, status.ip_address, status.signal_strength);
        last_logged_status_ = status;
    }

    return status;
}

// ============================================================================
// Helper Methods (encapsulate wpa_supplicant ugliness)
// ============================================================================

std::vector<WiFiNetwork> WifiBackendWpaSupplicant::parse_scan_results(const std::string& raw) {
    std::vector<WiFiNetwork> networks;

    if (raw.empty()) {
        spdlog::debug("[WifiBackend] Empty scan results");
        return networks;
    }

    // Skip header line (bssid / frequency / signal level / flags / ssid)
    std::istringstream stream(raw);
    std::string line;
    bool skip_header = true;

    while (std::getline(stream, line)) {
        if (skip_header) {
            skip_header = false;
            continue; // Skip "bssid / frequency / signal level / flags / ssid"
        }

        if (line.empty())
            continue;

        // Parse tab-separated fields: BSSID\tfreq\tsignal\tflags\tSSID
        // Note: Hidden networks may have only 4 fields (SSID completely absent)
        std::vector<std::string> fields = split_by_tabs(line);
        if (fields.size() < 4) {
            // Truly malformed - need at least BSSID, freq, signal, flags
            spdlog::trace("[WifiBackend] Skipping malformed scan line ({} fields): {}",
                          fields.size(), line);
            continue;
        }

        std::string bssid = fields[0];
        std::string freq_str = fields[1];
        std::string signal_str = fields[2];
        std::string flags = fields[3];
        // SSID may be missing entirely for hidden networks
        std::string ssid = (fields.size() >= 5) ? fields[4] : "";

        // Skip hidden networks (empty or missing SSID)
        if (ssid.empty()) {
            spdlog::trace("[WifiBackend] Skipping hidden network: {}", bssid);
            continue;
        }

        // Parse signal strength (dBm)
        int signal_dbm = 0;
        try {
            signal_dbm = std::stoi(signal_str);
        } catch (const std::exception& e) {
            LOG_WARN_INTERNAL("Invalid signal strength '{}': {}", signal_str, e.what());
            continue;
        }

        // Convert dBm to percentage
        int signal_percent = dbm_to_percentage(signal_dbm);

        // Detect security type
        bool is_secured = false;
        std::string security_type = detect_security_type(flags, is_secured);

        // Create network entry
        WiFiNetwork network(ssid, signal_percent, is_secured, security_type);
        networks.push_back(network);

        spdlog::trace("[WifiBackend] Parsed network: '{}' {}% {} {}", ssid, signal_percent,
                      security_type, bssid);
    }

    spdlog::debug("[WifiBackend] Parsed {} networks from scan results", networks.size());
    return networks;
}

std::vector<std::string> WifiBackendWpaSupplicant::split_by_tabs(const std::string& str) {
    std::vector<std::string> parts;
    std::stringstream ss(str);
    std::string part;
    while (std::getline(ss, part, '\t')) {
        parts.push_back(part);
    }
    return parts;
}

int WifiBackendWpaSupplicant::dbm_to_percentage(int dbm) {
    // -30 dBm = 100% (excellent), -90 dBm = 0% (unusable)
    return std::max(0, std::min(100, (dbm + 90) * 100 / 60));
}

std::string WifiBackendWpaSupplicant::detect_security_type(const std::string& flags,
                                                           bool& is_secured) {
    if (flags.find("WPA3") != std::string::npos) {
        is_secured = true;
        return "WPA3";
    }
    if (flags.find("WPA2") != std::string::npos) {
        is_secured = true;
        return "WPA2";
    }
    if (flags.find("WPA") != std::string::npos) {
        is_secured = true;
        return "WPA";
    }
    if (flags.find("WEP") != std::string::npos) {
        is_secured = true;
        return "WEP";
    }
    is_secured = false;
    return "Open";
}

#else
// ============================================================================
// macOS Stub Implementation: No-op for simulator
// ============================================================================

// Empty file - all methods are inline in header

#endif // __APPLE__
