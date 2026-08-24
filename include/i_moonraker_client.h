// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "connection_state.h"
#include "json_fwd.h"
#include "moonraker_error.h"
#include "moonraker_events.h"          // for helix::MoonrakerEventCallback
#include "moonraker_request_tracker.h" // for helix::RequestId
#include "moonraker_types.h"           // for ::GcodeStoreEntry
#include "printer_discovery.h"         // for helix::PrinterDiscovery

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix {

using ::json; // Make global json alias visible in this namespace

/**
 * @brief Unique identifier for notification subscriptions
 *
 * Used to track and remove subscriptions registered via register_notify_update().
 * Valid IDs are always > 0; ID 0 indicates invalid/unsubscribed.
 */
using SubscriptionId = uint64_t;

/** @brief Invalid subscription ID constant */
inline constexpr SubscriptionId INVALID_SUBSCRIPTION_ID = 0;

/**
 * @brief Marker key stamped onto a synthetic notify_status_update built from a
 *        status snapshot that was captured earlier and is being replayed.
 *
 * Present (and true) ONLY when dispatch_status_update() was told the payload is a
 * cached snapshot — today just the discovery subscription response, which is
 * captured on a background thread and re-dispatched at the end of discovery, long
 * after live WebSocket traffic may have moved the printer on. Consumers that care
 * about liveness (PrinterState's klippy-state freshness guard) read it; everything
 * else ignores it. Absent on live frames and on the mock's own synthetic
 * dispatches, which ARE the current truth for their session.
 */
inline constexpr const char* CACHED_SNAPSHOT_MARKER = "_helix_cached_snapshot";

/**
 * @brief Abstract interface for the Moonraker WebSocket + JSON-RPC transport layer.
 *
 * Production and test consumers that only need polymorphic access to the Moonraker
 * transport should depend on this interface rather than the concrete MoonrakerClient.
 * The concrete class inherits this interface (alongside hv::WebSocketClient).
 *
 * This interface IS the complete consumer contract for MoonrakerClient (Plan 3 Task 2,
 * docs/devel/specs/2026-06-10-esp32-*): every method a consumer (MoonrakerAPI, UI code,
 * platform client factories) calls on MoonrakerClient is declared here. Only
 * hv::WebSocketClient itself and protected/private implementation details remain on
 * the concrete class.
 */
class IMoonrakerClient {
  public:
    virtual ~IMoonrakerClient() = default;

    // ========================================================================
    // Connection Lifecycle
    // ========================================================================

    /// @brief Connect to Moonraker WebSocket server
    virtual int connect(const char* url, std::function<void()> on_connected,
                        std::function<void()> on_disconnected) = 0;

    /// @brief Disconnect from Moonraker WebSocket server
    virtual void disconnect() = 0;

    /// @brief Get URL from the last connect() call ("" if never connected)
    virtual const std::string& get_last_url() const = 0;

    /// @brief Enable/disable automatic reconnection
    ///
    /// Disabling stops the transport from retrying in the background — used by
    /// connection-test flows (setup wizard, change-host modal) that probe a
    /// candidate host and don't want a background reconnect loop fighting the
    /// probe. Transient by design: the next connect() call unconditionally
    /// re-installs its own reconnect settings, regardless of this flag.
    virtual void set_auto_reconnect(bool enabled) = 0;

    // ========================================================================
    // JSON-RPC Protocol
    // ========================================================================

    /// @brief Send JSON-RPC request without parameters
    virtual int send_jsonrpc(const std::string& method) = 0;

    /// @brief Send JSON-RPC request with parameters
    virtual int send_jsonrpc(const std::string& method, const json& params) = 0;

    /// @brief Send JSON-RPC request with one-time response callback
    virtual RequestId send_jsonrpc(const std::string& method, const json& params,
                                   std::function<void(const json&)> cb) = 0;

    /// @brief Send JSON-RPC request with success and error callbacks
    /// @param intent Explicit error-reporting intent (include/rpc_error_policy.h),
    ///        captured from the CALLER's own callbacks before any internal
    ///        wrapping. Omit it to have the tracker infer intent from @p silent
    ///        and the presence of @p error_cb.
    virtual RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 std::function<void(const json&)> success_cb,
                 std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
                 bool silent = false,
                 std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) = 0;

    /// @brief Send G-code script command
    virtual int gcode_script(const std::string& gcode) = 0;

    /// @brief Fetch G-code command history from Moonraker
    virtual void
    get_gcode_store(int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                    std::function<void(const MoonrakerError&)> on_error) = 0;

    /// @brief Fetch cached temperature history from Moonraker (server.temperature_store)
    virtual void get_temperature_store(std::function<void(const TemperatureStore&)> on_success,
                                       std::function<void(const MoonrakerError&)> on_error) = 0;

    // ========================================================================
    // Discovery
    // ========================================================================

    /// @brief Perform printer auto-discovery sequence
    virtual void
    discover_printer(std::function<void()> on_complete,
                     std::function<void(const std::string& reason)> on_error = nullptr) = 0;

    /// @brief Get discovered hardware data
    virtual PrinterDiscovery hardware() const = 0;

    /// @brief Parse object list from printer.objects.list response
    virtual void parse_objects(const json& objects) = 0;

    /// @brief Clear all cached discovery data
    virtual void clear_discovery_cache() = 0;

    /// @brief Set callback for hardware discovery (early phase, before full subscription)
    virtual void
    set_on_hardware_discovered(std::function<void(const helix::PrinterDiscovery&)> cb) = 0;

    /// @brief Set callback for printer discovery completion
    virtual void set_on_discovery_complete(
        std::function<void(const helix::PrinterDiscovery&, const json& initial_status)> cb) = 0;

    /// @brief Set callback for bed mesh updates received via WebSocket
    virtual void set_bed_mesh_callback(std::function<void(const json&)> callback) = 0;

    // ========================================================================
    // Subscriptions & Method Callbacks
    // ========================================================================

    /// @brief Register callback for status update notifications
    virtual SubscriptionId register_notify_update(std::function<void(const json&)> cb) = 0;

    /// @brief Unsubscribe from status update notifications
    virtual bool unsubscribe_notify_update(SubscriptionId id) = 0;

    /// @brief Register persistent callback for specific notification methods
    virtual void register_method_callback(const std::string& method,
                                          const std::string& handler_name,
                                          std::function<void(const json&)> cb) = 0;

    /// @brief Unregister a method callback by handler name
    virtual bool unregister_method_callback(const std::string& method,
                                            const std::string& handler_name) = 0;

    /// @brief Dispatch printer status to all registered notify callbacks
    ///
    /// Wraps raw status data (e.g., from a subscription response) into a
    /// notify_status_update notification format and dispatches to callbacks.
    /// Used for both initial subscription state and incremental updates.
    ///
    /// @param status Raw status object to fan out
    /// @param from_cached_snapshot true when `status` was captured earlier and is
    ///        being replayed (the discovery subscription response). Stamps
    ///        CACHED_SNAPSHOT_MARKER on the synthetic notification so liveness-
    ///        sensitive consumers can refuse to regress on it. Defaults to false:
    ///        a dispatch that reflects the caller's current view — including every
    ///        mock-driven state change — is live.
    virtual void dispatch_status_update(const json& status, bool from_cached_snapshot = false) = 0;

    // ========================================================================
    // Connection State & Observers
    // ========================================================================

    /// @brief Get current connection state
    virtual ConnectionState get_connection_state() const = 0;

    /// @brief Register an additional on-connected observer
    virtual void add_connected_observer(const std::string& handler_name,
                                        std::function<void()> cb) = 0;

    /// @brief Remove a previously-registered on-connected observer
    virtual bool remove_connected_observer(const std::string& handler_name) = 0;

    /// @brief Force full reconnection with complete state reset
    virtual void force_reconnect() = 0;

    // ========================================================================
    // Events & Modal Suppression
    // ========================================================================

    /// @brief Register callback for transport events
    virtual void register_event_handler(MoonrakerEventCallback cb) = 0;

    /// @brief Temporarily suppress disconnect modal notifications
    virtual void suppress_disconnect_modal(uint32_t duration_ms = 10000) = 0;

    /// @brief Check if disconnect modal is currently suppressed
    virtual bool is_disconnect_modal_suppressed() const = 0;

    // ========================================================================
    // Request Management
    // ========================================================================

    /// @brief Cancel a pending JSON-RPC request
    virtual bool cancel_request(RequestId id) = 0;

    // ========================================================================
    // Owner Wiring & Configuration
    // ========================================================================

    /// @brief Set callback for connection state changes
    virtual void
    set_state_change_callback(std::function<void(ConnectionState, ConnectionState)> cb) = 0;

    /// @brief Set connection timeout in milliseconds
    virtual void set_connection_timeout(uint32_t timeout_ms) = 0;

    /// @brief Set default request timeout in milliseconds
    virtual void set_default_request_timeout(uint32_t timeout_ms) = 0;

    /// @brief Configure timeout and reconnection parameters
    virtual void configure_timeouts(uint32_t connection_timeout_ms, uint32_t request_timeout_ms,
                                    uint32_t keepalive_interval_ms, uint32_t reconnect_min_delay_ms,
                                    uint32_t reconnect_max_delay_ms) = 0;

    /// @brief Process timeout checks for pending requests
    virtual void process_timeouts() = 0;

    // ========================================================================
    // Simulation Hooks (for testing)
    // ========================================================================

    /// @brief Toggle filament runout simulation (no-op in production)
    virtual void toggle_filament_runout_simulation() = 0;

    // ========================================================================
    // Lifetime Guard (for SubscriptionGuard)
    // ========================================================================

    /// @brief Get lifetime guard for safe destructor-aware captures
    virtual std::weak_ptr<bool> lifetime_weak() const = 0;
};

/**
 * Platform-provided client factory for embedded targets. NOT defined in the
 * desktop build — the ESP32 firmware tree implements it over
 * esp_websocket_client (MoonrakerManager calls it when ESP_PLATFORM is defined).
 */
std::unique_ptr<IMoonrakerClient> create_platform_moonraker_client();

} // namespace helix
