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

#ifndef MOONRAKER_CLIENT_H
#define MOONRAKER_CLIENT_H

#include "hv/WebSocketClient.h"
#include "moonraker_domain_service.h"
#include "moonraker_error.h"
#include "moonraker_events.h"
#include "moonraker_request.h"
#include "printer_capabilities.h"
#include "spdlog/spdlog.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "hv/json.hpp" // libhv's nlohmann json (via cpputil/)

using json = nlohmann::json;

/**
 * @brief Unique identifier for notification subscriptions
 *
 * Used to track and remove subscriptions registered via register_notify_update().
 * Valid IDs are always > 0; ID 0 indicates invalid/unsubscribed.
 */
using SubscriptionId = uint64_t;

/** @brief Invalid subscription ID constant */
constexpr SubscriptionId INVALID_SUBSCRIPTION_ID = 0;

/**
 * @brief Unique identifier for JSON-RPC requests
 *
 * Used to track pending requests and allow cancellation.
 * Returned by send_jsonrpc() overloads that take callbacks.
 * Valid IDs are always > 0; ID 0 indicates invalid/failed request.
 */
using RequestId = uint64_t;

/** @brief Invalid request ID constant */
constexpr RequestId INVALID_REQUEST_ID = 0;

/**
 * @brief Connection state for Moonraker WebSocket
 */
enum class ConnectionState {
    DISCONNECTED, // Not connected
    CONNECTING,   // Connection in progress
    CONNECTED,    // Connected and ready
    RECONNECTING, // Automatic reconnection in progress
    FAILED        // Connection failed (max retries exceeded)
};

/**
 * @brief WebSocket client for Moonraker API communication
 *
 * Implements JSON-RPC 2.0 protocol for Klipper/Moonraker integration.
 * Handles connection lifecycle, automatic reconnection, and message routing.
 */
class MoonrakerClient : public hv::WebSocketClient {
  public:
    MoonrakerClient(hv::EventLoopPtr loop = nullptr);
    ~MoonrakerClient();

    // Prevent copying (WebSocket client should not be copied)
    MoonrakerClient(const MoonrakerClient&) = delete;
    MoonrakerClient& operator=(const MoonrakerClient&) = delete;

    /**
     * @brief Connect to Moonraker WebSocket server
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * @param url WebSocket URL (e.g., "ws://127.0.0.1:7125/websocket")
     * @param on_connected Callback invoked when connection opens
     * @param on_disconnected Callback invoked when connection closes
     * @return 0 on success, non-zero on error
     */
    virtual int connect(const char* url, std::function<void()> on_connected,
                        std::function<void()> on_disconnected);

    /**
     * @brief Disconnect from Moonraker WebSocket server
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * Closes the WebSocket connection and resets internal state.
     * Safe to call multiple times (idempotent).
     */
    virtual void disconnect();

    /**
     * @brief Force full reconnection with complete state reset
     *
     * Unlike automatic reconnection (which preserves callbacks and pending requests),
     * force_reconnect() performs a complete teardown and rebuild:
     *   1. Disconnects cleanly (if connected)
     *   2. Clears all pending requests (callbacks not invoked)
     *   3. Resets retry counter to 0
     *   4. Reconnects to the same URL
     *   5. Runs discover_printer() to re-subscribe to Moonraker events
     *
     * Use this when:
     *   - User manually requests reconnection (e.g., settings panel "Reconnect" button)
     *   - Recovering from persistent error states
     *   - After printer firmware restart
     *
     * Thread-safe. Can be called from any thread.
     */
    void force_reconnect();

    /**
     * @brief Register callback for status update notifications
     *
     * Invoked when Moonraker sends "notify_status_update" messages
     * (triggered by printer.objects.subscribe subscriptions).
     *
     * @param cb Callback function receiving parsed JSON notification
     * @return Subscription ID for later unsubscription (0 = invalid/failed)
     */
    SubscriptionId register_notify_update(std::function<void(json)> cb);

    /**
     * @brief Unsubscribe from status update notifications
     *
     * Removes a previously registered notification callback.
     * Safe to call with invalid IDs (no-op).
     *
     * @param id Subscription ID returned by register_notify_update()
     * @return true if subscription was found and removed, false otherwise
     */
    bool unsubscribe_notify_update(SubscriptionId id);

    /**
     * @brief Register persistent callback for specific notification methods
     *
     * Unlike one-time request callbacks, these persist across multiple messages.
     * Useful for console output, prompt notifications, etc.
     *
     * @param method Notification method name (e.g., "notify_gcode_response")
     * @param handler_name Unique identifier for this handler (for unregistration)
     * @param cb Callback function receiving parsed JSON notification
     */
    void register_method_callback(const std::string& method, const std::string& handler_name,
                                  std::function<void(json)> cb);

    /**
     * @brief Unregister a method callback by handler name
     *
     * Removes a previously registered method-specific callback.
     * Safe to call with non-existent method/handler combinations (no-op).
     *
     * @param method Notification method name (e.g., "notify_gcode_response")
     * @param handler_name Handler name used during registration
     * @return true if handler was found and removed, false otherwise
     */
    bool unregister_method_callback(const std::string& method, const std::string& handler_name);

    /**
     * @brief Send JSON-RPC request without parameters
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * @param method RPC method name (e.g., "printer.info")
     * @return 0 on success, non-zero on error
     */
    virtual int send_jsonrpc(const std::string& method);

    /**
     * @brief Send JSON-RPC request with parameters
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * @param method RPC method name
     * @param params JSON parameters object
     * @return 0 on success, non-zero on error
     */
    virtual int send_jsonrpc(const std::string& method, const json& params);

    /**
     * @brief Send JSON-RPC request with one-time response callback
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * Callback is invoked once when response arrives, then removed.
     *
     * @param method RPC method name
     * @param params JSON parameters object
     * @param cb Callback function receiving response JSON
     * @return Request ID for cancellation, or INVALID_REQUEST_ID on error
     */
    virtual RequestId send_jsonrpc(const std::string& method, const json& params,
                                   std::function<void(json)> cb);

    /**
     * @brief Send JSON-RPC request with success and error callbacks
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * @param method RPC method name
     * @param params JSON parameters object
     * @param success_cb Callback for successful response
     * @param error_cb Callback for errors (timeout, JSON-RPC error, etc.)
     * @param timeout_ms Optional timeout override (0 = use default)
     * @return Request ID for cancellation, or INVALID_REQUEST_ID on error
     */
    virtual RequestId send_jsonrpc(const std::string& method, const json& params,
                                   std::function<void(json)> success_cb,
                                   std::function<void(const MoonrakerError&)> error_cb,
                                   uint32_t timeout_ms = 0);

    /**
     * @brief Cancel a pending JSON-RPC request
     *
     * Removes the request from the pending queue without invoking callbacks.
     * Safe to call with invalid IDs or already-completed requests (no-op).
     * The actual WebSocket message cannot be recalled once sent; this only
     * prevents callback invocation when the response arrives.
     *
     * @param id Request ID returned by send_jsonrpc()
     * @return true if request was found and cancelled, false otherwise
     */
    bool cancel_request(RequestId id);

    /**
     * @brief Send G-code script command
     *
     * Virtual to allow mock override for testing without real network connection.
     *
     * Convenience wrapper for printer.gcode.script method.
     *
     * @param gcode G-code string (e.g., "G28", "M104 S210")
     * @return 0 on success, non-zero on error
     */
    virtual int gcode_script(const std::string& gcode);

    /**
     * @brief Perform printer auto-discovery sequence
     *
     * Calls printer.objects.list → server.info → printer.info → printer.objects.subscribe
     * in sequence, parsing discovered objects and populating PrinterState.
     *
     * Virtual to allow mock override for testing without real printer connection.
     *
     * @param on_complete Callback invoked when discovery completes successfully
     */
    virtual void discover_printer(std::function<void()> on_complete);

    /**
     * @brief Parse object list from printer.objects.list response
     *
     * Categorizes Klipper objects into typed arrays (extruders, heaters, sensors, fans).
     *
     * @param objects JSON array of object names
     */
    void parse_objects(const json& objects);

    /**
     * @brief Parse bed mesh data from Moonraker notification
     *
     * Extracts bed_mesh object from printer state updates (notify_status_update).
     * Updates active_bed_mesh_ with probed_matrix, bounds, and available profiles.
     *
     * @param bed_mesh JSON object from bed_mesh subscription
     */
    void parse_bed_mesh(const json& bed_mesh);

    /**
     * @brief Get discovered heaters (extruders, beds, generic heaters)
     */
    const std::vector<std::string>& get_heaters() const {
        return heaters_;
    }

    /**
     * @brief Get discovered read-only sensors
     */
    const std::vector<std::string>& get_sensors() const {
        return sensors_;
    }

    /**
     * @brief Get discovered fans
     */
    const std::vector<std::string>& get_fans() const {
        return fans_;
    }

    /**
     * @brief Get discovered LED outputs
     */
    const std::vector<std::string>& get_leds() const {
        return leds_;
    }

    /**
     * @brief Get printer capabilities (QGL, Z-tilt, bed mesh, macros, etc.)
     *
     * Populated during discover_printer() from printer.objects.list response.
     */
    const PrinterCapabilities& capabilities() const {
        return capabilities_;
    }

    /**
     * @brief Guess the most likely bed heater from discovered hardware
     *
     * Searches heaters_ for names containing "bed", "heated_bed", "heater_bed".
     * Returns the first match found, or empty string if none found.
     *
     * @return Bed heater name or empty string
     * @deprecated Use MoonrakerAPI::guess_bed_heater() instead. Domain logic is migrating
     *             from transport layer (MoonrakerClient) to API layer (MoonrakerAPI).
     */
    [[deprecated("Use MoonrakerAPI::guess_bed_heater() instead")]]
    std::string guess_bed_heater() const;

    /**
     * @brief Guess the most likely hotend heater from discovered hardware
     *
     * Searches heaters_ for names containing "extruder", "hotend", "e0".
     * Prioritizes "extruder" (base extruder) over numbered variants.
     *
     * @return Hotend heater name or empty string
     * @deprecated Use MoonrakerAPI::guess_hotend_heater() instead. Domain logic is migrating
     *             from transport layer (MoonrakerClient) to API layer (MoonrakerAPI).
     */
    [[deprecated("Use MoonrakerAPI::guess_hotend_heater() instead")]]
    std::string guess_hotend_heater() const;

    /**
     * @brief Guess the most likely bed temperature sensor from discovered hardware
     *
     * First checks heaters_ for bed heater (heaters have built-in sensors).
     * If no bed heater found, searches sensors_ for names containing "bed".
     *
     * @return Bed sensor name or empty string
     * @deprecated Use MoonrakerAPI::guess_bed_sensor() instead. Domain logic is migrating
     *             from transport layer (MoonrakerClient) to API layer (MoonrakerAPI).
     */
    [[deprecated("Use MoonrakerAPI::guess_bed_sensor() instead")]]
    std::string guess_bed_sensor() const;

    /**
     * @brief Guess the most likely hotend temperature sensor from discovered hardware
     *
     * First checks heaters_ for extruder heater (heaters have built-in sensors).
     * If no extruder heater found, searches sensors_ for names containing "extruder", "hotend",
     * "e0".
     *
     * @return Hotend sensor name or empty string
     * @deprecated Use MoonrakerAPI::guess_hotend_sensor() instead. Domain logic is migrating
     *             from transport layer (MoonrakerClient) to API layer (MoonrakerAPI).
     */
    [[deprecated("Use MoonrakerAPI::guess_hotend_sensor() instead")]]
    std::string guess_hotend_sensor() const;

    /**
     * @brief Get currently active bed mesh profile
     *
     * Returns the active mesh profile loaded from Moonraker's bed_mesh object.
     * The probed_matrix field contains the 2D Z-height array ready for rendering.
     *
     * @return Active mesh profile, or empty profile if none loaded
     * @deprecated Use MoonrakerAPI::get_active_bed_mesh() instead. Domain logic is migrating
     *             from transport layer (MoonrakerClient) to API layer (MoonrakerAPI).
     */
    [[deprecated("Use MoonrakerAPI::get_active_bed_mesh() instead")]]
    const BedMeshProfile& get_active_bed_mesh() const {
        return active_bed_mesh_;
    }

    /**
     * @brief Get list of available mesh profile names
     *
     * Returns profile names from bed_mesh.profiles (e.g., "default", "adaptive", "calibration").
     * Empty vector if no profiles available or discovery hasn't completed.
     *
     * @return Vector of profile names
     * @deprecated Use MoonrakerAPI::get_bed_mesh_profiles() instead. Domain logic is migrating
     *             from transport layer (MoonrakerClient) to API layer (MoonrakerAPI).
     */
    [[deprecated("Use MoonrakerAPI::get_bed_mesh_profiles() instead")]]
    const std::vector<std::string>& get_bed_mesh_profiles() const {
        return bed_mesh_profiles_;
    }

    /**
     * @brief Check if bed mesh data is available
     *
     * Returns true if at least one mesh has been loaded from Moonraker.
     * Does NOT guarantee the mesh is currently active in Klipper.
     *
     * @return true if probed_matrix is non-empty
     * @deprecated Use MoonrakerAPI::has_bed_mesh() instead. Domain logic is migrating
     *             from transport layer (MoonrakerClient) to API layer (MoonrakerAPI).
     */
    [[deprecated("Use MoonrakerAPI::has_bed_mesh() instead")]]
    bool has_bed_mesh() const {
        return !active_bed_mesh_.probed_matrix.empty();
    }

    /**
     * @brief Get printer hostname from printer.info
     *
     * Returns hostname discovered during printer discovery sequence.
     * Empty string if discovery hasn't completed or printer.info unavailable.
     */
    const std::string& get_hostname() const {
        return hostname_;
    }

    /**
     * @brief Get Klipper software version from printer.info
     *
     * Returns version string discovered during printer discovery sequence.
     * Examples: "v0.12.0-108-g2c7a9d58", "v0.11.0"
     * Empty string if discovery hasn't completed or printer.info unavailable.
     */
    const std::string& get_software_version() const {
        return software_version_;
    }

    /**
     * @brief Get Moonraker software version from server.info
     *
     * Returns version string discovered during printer discovery sequence.
     * Examples: "v0.8.0-143-g2c7a9d58"
     * Empty string if discovery hasn't completed or server.info unavailable.
     */
    const std::string& get_moonraker_version() const {
        return moonraker_version_;
    }

    /**
     * @brief Get current connection state
     */
    ConnectionState get_connection_state() const {
        return connection_state_;
    }

    /**
     * @brief Set callback for connection state changes
     *
     * @param cb Callback invoked when state changes (old_state, new_state)
     */
    void set_state_change_callback(std::function<void(ConnectionState, ConnectionState)> cb) {
        state_change_callback_ = cb;
    }

    /**
     * @brief Set callback for printer discovery completion
     *
     * Called after discover_printer() successfully completes auto-discovery.
     * Provides the discovered PrinterCapabilities for reactive UI updates.
     *
     * @param cb Callback invoked with discovered capabilities
     */
    void set_on_discovery_complete(std::function<void(const PrinterCapabilities&)> cb) {
        on_discovery_complete_ = cb;
    }

    /**
     * @brief Register callback for transport events
     *
     * Only one handler can be registered at a time. Registering a new
     * handler replaces the previous one.
     *
     * @param cb Callback function, or nullptr to unregister
     */
    void register_event_handler(MoonrakerEventCallback cb);

    /**
     * @brief Set connection timeout in milliseconds
     *
     * @param timeout_ms Connection timeout (default 10000ms)
     */
    void set_connection_timeout(uint32_t timeout_ms) {
        connection_timeout_ms_ = timeout_ms;
    }

    /**
     * @brief Set default request timeout in milliseconds
     *
     * @param timeout_ms Request timeout
     */
    void set_default_request_timeout(uint32_t timeout_ms) {
        default_request_timeout_ms_ = timeout_ms;
    }

    /**
     * @brief Configure timeout and reconnection parameters
     *
     * Sets all timeout and reconnection parameters from config values.
     *
     * @param connection_timeout_ms Connection timeout in milliseconds
     * @param request_timeout_ms Default request timeout in milliseconds
     * @param keepalive_interval_ms WebSocket keepalive ping interval
     * @param reconnect_min_delay_ms Minimum reconnection delay
     * @param reconnect_max_delay_ms Maximum reconnection delay
     */
    void configure_timeouts(uint32_t connection_timeout_ms, uint32_t request_timeout_ms,
                            uint32_t keepalive_interval_ms, uint32_t reconnect_min_delay_ms,
                            uint32_t reconnect_max_delay_ms) {
        connection_timeout_ms_ = connection_timeout_ms;
        default_request_timeout_ms_ = request_timeout_ms;
        keepalive_interval_ms_ = keepalive_interval_ms;
        reconnect_min_delay_ms_ = reconnect_min_delay_ms;
        reconnect_max_delay_ms_ = reconnect_max_delay_ms;
    }

    /**
     * @brief Process timeout checks for pending requests
     *
     * Should be called periodically (e.g., from main loop) to check for timed out requests.
     * Typically called every 1-5 seconds.
     */
    void process_timeouts() {
        check_request_timeouts();
    }

  protected:
    /**
     * @brief Transition to new connection state
     *
     * @param new_state The new state to transition to
     */
    void set_connection_state(ConnectionState new_state);

    /**
     * @brief Dispatch printer status to all registered notify callbacks
     *
     * Wraps raw status data (e.g., from subscription response) into a
     * notify_status_update notification format and dispatches to callbacks.
     * Used for both initial subscription state and incremental updates.
     *
     * @param status Raw printer status object
     */
    void dispatch_status_update(const json& status);

    /**
     * @brief Emit event to registered handler
     *
     * Thread-safe. If no handler is registered, the event is logged and dropped.
     *
     * @param type Event type
     * @param message Human-readable message
     * @param is_error true for error events, false for warnings
     * @param details Additional details (optional)
     */
    void emit_event(MoonrakerEventType type, const std::string& message, bool is_error = false,
                    const std::string& details = "");

  private:
    /**
     * @brief Check for timed out requests and invoke error callbacks
     */
    void check_request_timeouts();

    /**
     * @brief Cleanup all pending requests (called on disconnect)
     */
    void cleanup_pending_requests();

  protected:
    // Auto-discovered printer objects (protected to allow mock access)
    std::vector<std::string> heaters_; // Controllable heaters (extruders, bed, etc.)
    std::vector<std::string> sensors_; // Read-only temperature sensors
    std::vector<std::string> fans_;    // All fan types
    std::vector<std::string> leds_;    // LED outputs
    std::string hostname_;             // Printer hostname from printer.info
    std::string software_version_;     // Klipper software version from printer.info
    std::string moonraker_version_;    // Moonraker version from server.info
    PrinterCapabilities capabilities_; // QGL, Z-tilt, bed mesh, macros

    // Discovery callback (protected to allow mock to invoke it)
    std::function<void(const PrinterCapabilities&)> on_discovery_complete_;

    // Bed mesh data
    BedMeshProfile active_bed_mesh_;             // Currently active mesh profile
    std::vector<std::string> bed_mesh_profiles_; // Available profile names

    // Notification callbacks (protected to allow mock to trigger notifications)
    // Map of subscription ID -> callback for O(1) unsubscription
    std::map<SubscriptionId, std::function<void(json)>> notify_callbacks_;
    std::atomic<SubscriptionId> next_subscription_id_{1}; // Start at 1 (0 = invalid)
    std::mutex callbacks_mutex_; // Protect notify_callbacks_ and method_callbacks_

  private:
    // Pending requests keyed by request ID
    std::map<uint64_t, PendingRequest> pending_requests_;
    std::mutex requests_mutex_; // Protect pending_requests_ map

    // Persistent method-specific callbacks
    // method_name : { handler_name : callback }
    std::map<std::string, std::map<std::string, std::function<void(json)>>> method_callbacks_;

    // Auto-incrementing JSON-RPC request ID
    std::atomic_uint64_t request_id_;

    // Connection state tracking
    std::atomic_bool was_connected_;
    std::atomic<ConnectionState> connection_state_;
    std::atomic_bool is_destroying_{false}; // Prevent callbacks during destruction
    std::function<void(ConnectionState, ConnectionState)> state_change_callback_;
    mutable std::mutex state_callback_mutex_; // Protect state_change_callback_ during destruction
    uint32_t connection_timeout_ms_;
    uint32_t reconnect_attempts_ = 0;
    uint32_t max_reconnect_attempts_ = 0; // 0 = infinite

    // Request timeout tracking
    uint32_t default_request_timeout_ms_;

    // Connection parameters (from config)
    uint32_t keepalive_interval_ms_;
    uint32_t reconnect_min_delay_ms_;
    uint32_t reconnect_max_delay_ms_;

    // Stored connection info for force_reconnect()
    std::string last_url_;                                // URL used in last connect()
    std::function<void()> last_on_connected_;             // Callback from last connect()
    std::function<void()> last_on_disconnected_;          // Callback from last connect()
    std::function<void()> last_discovery_complete_;       // Callback from last discover_printer()
    mutable std::mutex reconnect_mutex_;                  // Protect stored connection info

    // Event handler for transport events (decouples from UI layer)
    MoonrakerEventCallback event_handler_;
    mutable std::mutex event_handler_mutex_;
};

#endif // MOONRAKER_CLIENT_H
