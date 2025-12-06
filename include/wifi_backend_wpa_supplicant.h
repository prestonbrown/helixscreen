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

#pragma once

#include "wifi_backend.h" // Base class

#include <functional>
#include <map>
#include <string>
#include <vector>

#ifndef __APPLE__
// ============================================================================
// Linux Implementation: Full wpa_supplicant integration
// ============================================================================

#include "hv/EventLoop.h"
#include "hv/EventLoopThread.h"
#include "hv/hloop.h"

#include <mutex>

// Forward declaration - avoid including wpa_ctrl.h in header
struct wpa_ctrl;

/**
 * @brief wpa_supplicant backend using libhv async event loop
 *
 * Provides asynchronous communication with wpa_supplicant daemon via
 * Unix socket control interface. Uses libhv's EventLoopThread for
 * non-blocking socket I/O.
 *
 * Architecture:
 * - Inherits privately from hv::EventLoopThread for async I/O
 * - Dual wpa_ctrl connections: control (commands) + monitor (events)
 * - Event callbacks broadcast to registered handlers
 * - Commands sent synchronously via wpa_ctrl_request()
 *
 * Usage:
 * @code
 *   WifiBackendWpaSupplicant backend;
 *   backend.register_callback("scan", [](const std::string& event) {
 *       // Handle scan complete events
 *   });
 *   backend.start();  // Connects to wpa_supplicant, starts event loop
 *   std::string result = backend.send_command("SCAN");
 *   backend.stop();   // Clean shutdown
 * @endcode
 */
class WifiBackendWpaSupplicant : public WifiBackend, private hv::EventLoopThread {
  public:
    /**
     * @brief Construct WiFi backend
     *
     * Does NOT connect to wpa_supplicant. Call start() to initialize.
     */
    WifiBackendWpaSupplicant();

    /**
     * @brief Destructor - ensures clean shutdown
     */
    ~WifiBackendWpaSupplicant();

    // ========================================================================
    // WifiBackend Interface Implementation
    // ========================================================================

    /**
     * @brief Initialize and start wpa_supplicant backend
     *
     * Discovers wpa_supplicant socket, establishes dual connections
     * (control + monitor), and starts libhv event loop thread.
     *
     * @return true if initialization succeeded
     */
    WiFiError start() override;

    /**
     * @brief Stop wpa_supplicant backend
     *
     * Blocks until event loop thread terminates.
     */
    void stop() override;

    /**
     * @brief Check if backend is running
     *
     * @return true if event loop is active
     */
    bool is_running() const override;

    /**
     * @brief Register event callback
     *
     * Translates standard event names to wpa_supplicant-specific events:
     * - "SCAN_COMPLETE" → "CTRL-EVENT-SCAN-RESULTS"
     * - "CONNECTED" → "CTRL-EVENT-CONNECTED"
     * - "DISCONNECTED" → "CTRL-EVENT-DISCONNECTED"
     * - "AUTH_FAILED" → "CTRL-EVENT-SSID-TEMP-DISABLED"
     *
     * @param name Standard event name
     * @param callback Handler function
     */
    void register_event_callback(const std::string& name,
                                 std::function<void(const std::string&)> callback) override;

    /**
     * @brief Send synchronous command to wpa_supplicant
     *
     * Blocks until response received or timeout (usually <100ms).
     *
     * Common commands:
     * - "SCAN" - Trigger network scan
     * - "SCAN_RESULTS" - Get scan results (tab-separated format)
     * - "ADD_NETWORK" - Add network configuration (returns network ID)
     * - "SET_NETWORK <id> ssid \"<ssid>\"" - Set network SSID
     * - "SET_NETWORK <id> psk \"<password>\"" - Set WPA password
     * - "ENABLE_NETWORK <id>" - Connect to network
     * - "STATUS" - Get connection status
     *
     * @param cmd Command string (see wpa_supplicant control interface docs)
     * @return Response string (may contain newlines), or empty on error
     */
    std::string send_command(const std::string& cmd);

    // ========================================================================
    // Clean Abstraction API - Hides wpa_supplicant ugliness
    // ========================================================================

    WiFiError trigger_scan() override;
    WiFiError get_scan_results(std::vector<WiFiNetwork>& networks) override;
    WiFiError connect_network(const std::string& ssid, const std::string& password) override;
    WiFiError disconnect_network() override;
    ConnectionStatus get_status() override;

  private:
    // ========================================================================
    // System Validation and Permission Checking
    // ========================================================================

    /**
     * @brief Check system prerequisites before starting backend
     *
     * Performs comprehensive validation:
     * - WiFi hardware detection
     * - wpa_supplicant socket availability
     * - Permission checking for socket access
     * - RF-kill status validation
     *
     * @return WiFiError with detailed status
     */
    WiFiError check_system_prerequisites();

    /**
     * @brief Check if user has permission to access wpa_supplicant sockets
     *
     * @param socket_path Path to test socket access
     * @return WiFiError indicating permission status
     */
    WiFiError check_socket_permissions(const std::string& socket_path);

    /**
     * @brief Detect WiFi hardware interfaces
     *
     * @return WiFiError with hardware status
     */
    WiFiError check_wifi_hardware();

    // ========================================================================
    // wpa_supplicant Communication
    // ========================================================================

    /**
     * @brief Initialize wpa_supplicant connection (runs in event loop thread)
     *
     * Called by start() in the context of the libhv event loop thread.
     * Discovers socket, opens connections, registers I/O callbacks.
     */
    void init_wpa();

    /**
     * @brief Cleanup wpa_supplicant connections
     *
     * Closes both control and monitor connections, detaches from events.
     * Called from destructor to prevent resource leaks.
     */
    void cleanup_wpa();

    /**
     * @brief Handle incoming wpa_supplicant events
     *
     * Broadcasts event to all registered callbacks.
     *
     * @param data Raw event data from wpa_supplicant
     * @param len Length of event data in bytes
     */
    void handle_wpa_events(void* data, int len);

    /**
     * @brief Static trampoline for C callback compatibility
     *
     * libhv uses C-style function pointers for I/O callbacks.
     * This static method extracts the instance pointer from hio_context()
     * and forwards to the member function handle_wpa_events().
     *
     * @param io libhv I/O handle
     * @param data Event data buffer
     * @param readbyte Number of bytes read
     */
    static void _handle_wpa_events(hio_t* io, void* data, int readbyte);

    /**
     * @brief Dispatch a synthetic event to a specific registered callback
     *
     * Used for internal events like INIT_FAILED that don't come from wpa_supplicant.
     *
     * @param event_name Name of the callback to dispatch to
     * @param message Message to pass to the callback
     */
    void dispatch_event(const std::string& event_name, const std::string& message);

    // Helper methods for clean API (encapsulate wpa_supplicant ugliness)
    std::vector<WiFiNetwork> parse_scan_results(const std::string& raw);
    std::vector<std::string> split_by_tabs(const std::string& str);
    int dbm_to_percentage(int dbm);
    std::string detect_security_type(const std::string& flags, bool& is_secured);

    /**
     * @brief Map raw wpa_supplicant event to callback name
     *
     * Parses the event string to determine which callback should handle it.
     * Returns empty string for informational events that don't need handling.
     *
     * @param event Raw wpa_supplicant event string
     * @return Callback name ("SCAN_COMPLETE", "CONNECTED", etc.) or empty string
     */
    std::string map_event_to_callback(const std::string& event);

    struct wpa_ctrl* conn;     ///< Control connection for sending commands
    struct wpa_ctrl* mon_conn; ///< Monitor connection for receiving events (FIXED LEAK)

    // Thread safety for callbacks (accessed from multiple threads)
    std::mutex callbacks_mutex_; ///< Protects callbacks map from race conditions
    std::map<std::string, std::function<void(const std::string&)>>
        callbacks; ///< Registered event handlers

    // Change detection for status logging (reduces log noise)
    ConnectionStatus last_logged_status_; ///< Previous status for change detection
};

#endif // __APPLE__
