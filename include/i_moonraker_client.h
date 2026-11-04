// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "connection_state.h"
#include "helix_shared_callback.h"
#include "json_fwd.h"
#include "moonraker_error.h"
#include "moonraker_events.h"          // for helix::MoonrakerEventCallback
#include "moonraker_request_tracker.h" // for helix::RequestId
#include "moonraker_types.h"           // for ::GcodeStoreEntry

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix {

using ::json; // Make global json alias visible in this namespace

// hardware() returns a PrinterDiscovery by value, and a declaration may return an
// incomplete type: a caller that reads the snapshot includes printer_discovery.h itself.
class PrinterDiscovery;

/**
 * @brief Unique identifier for notification subscriptions
 *
 * Used to track and remove subscriptions registered via register_notify_update().
 * Valid IDs are always > 0; ID 0 indicates invalid/unsubscribed.
 */
using SubscriptionId = uint64_t;

/** @brief Invalid subscription ID constant */
inline constexpr SubscriptionId kInvalidSubscriptionId = 0;

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
inline constexpr const char* kCachedSnapshotMarker = "_helix_cached_snapshot";

/**
 * @brief Marker key stamped onto a synthetic notify_status_update whose payload
 * holds every field of each object it names, as a printer.objects.subscribe
 * response does, rather than only the fields that changed.
 *
 * A live frame names a field only when its value changed, so a field it carries
 * is news. A whole-object payload repeats values the printer has held all along,
 * which is what consumers that act on a value's arrival need to know. Set by the
 * discovery subscription replay (also a cached snapshot) and by a subscription
 * refresh (current, so not a cached snapshot).
 */
inline constexpr const char* kWholeObjectsMarker = "_helix_whole_objects";

/**
 * @brief Wrap a raw status object as the notify_status_update a client dispatches
 *
 * The inverse of parse_status_notification() (printer_state.h). A synthetic
 * dispatch is not a Klipper frame, so its eventtime is 0.0 ("untimestamped") and
 * provenance travels in the markers instead of being inferred from the clock.
 */
inline json make_status_notification(const json& status, bool from_cached_snapshot,
                                     bool whole_objects) {
    json notification = {{"method", "notify_status_update"},
                         {"params", json::array({status, 0.0})}}; // [status, eventtime]
    if (from_cached_snapshot) {
        notification[kCachedSnapshotMarker] = true;
    }
    if (whole_objects) {
        notification[kWholeObjectsMarker] = true;
    }
    return notification;
}

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
    virtual int connect(const char* url, helix::SharedCallback<void()> on_connected,
                        helix::SharedCallback<void()> on_disconnected) = 0;

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

    /// @brief Why the last connect() attempt failed before it ever opened
    ///
    /// Valid once on_disconnected has fired for an attempt that never connected;
    /// connect() resets it. A transport that cannot read the socket's cause
    /// reports Unknown, which is why this one has a body.
    virtual ConnectFailureKind last_connect_failure() const {
        return ConnectFailureKind::Unknown;
    }

    // ========================================================================
    // JSON-RPC Protocol
    // ========================================================================

    /// @brief Send JSON-RPC request without parameters
    virtual int send_jsonrpc(const std::string& method) = 0;

    /// @brief Send JSON-RPC request with parameters
    virtual int send_jsonrpc(const std::string& method, const json& params) = 0;

    /// @brief Send JSON-RPC request with one-time response callback
    virtual RequestId send_jsonrpc(const std::string& method, const json& params,
                                   helix::SharedCallback<void(const json&)> cb) = 0;

    /// @brief Send JSON-RPC request with success and error callbacks
    /// @param intent Explicit error-reporting intent (include/rpc_error_policy.h),
    ///        captured from the CALLER's own callbacks before any internal
    ///        wrapping. Omit it to have the tracker infer intent from @p silent
    ///        and the presence of @p error_cb.
    virtual RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb,
                 uint32_t timeout_ms = 0, bool silent = false,
                 std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) = 0;

    /// @brief Send G-code script command
    virtual int gcode_script(const std::string& gcode) = 0;

    /// @brief Fetch G-code command history from Moonraker
    virtual void
    get_gcode_store(int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                    helix::SharedCallback<void(const MoonrakerError&)> on_error) = 0;

    /// @brief Fetch cached temperature history from Moonraker (server.temperature_store)
    virtual void
    get_temperature_store(std::function<void(const TemperatureStore&)> on_success,
                          helix::SharedCallback<void(const MoonrakerError&)> on_error) = 0;

    // ========================================================================
    // Discovery
    // ========================================================================

    /// @brief Perform printer auto-discovery sequence
    virtual void
    discover_printer(helix::SharedCallback<void()> on_complete,
                     helix::SharedCallback<void(const std::string& reason)> on_error = nullptr) = 0;

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
    virtual void set_bed_mesh_callback(helix::SharedCallback<void(const json&)> callback) = 0;

    // ========================================================================
    // Subscriptions & Method Callbacks
    // ========================================================================

    /// @brief Register callback for status update notifications
    virtual SubscriptionId register_notify_update(helix::SharedCallback<void(const json&)> cb) = 0;

    /// @brief Unsubscribe from status update notifications
    virtual bool unsubscribe_notify_update(SubscriptionId id) = 0;

    /// @brief Register persistent callback for specific notification methods
    virtual void register_method_callback(const std::string& method,
                                          const std::string& handler_name,
                                          helix::SharedCallback<void(const json&)> cb) = 0;

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
    ///        kCachedSnapshotMarker on the synthetic notification so liveness-
    ///        sensitive consumers can refuse to regress on it. Defaults to false:
    ///        a dispatch that reflects the caller's current view — including every
    ///        mock-driven state change — is live.
    /// @param whole_objects true when `status` is a subscription response, carrying
    ///        every field of each object instead of only the changed ones. Stamps
    ///        kWholeObjectsMarker.
    virtual void dispatch_status_update(const json& status, bool from_cached_snapshot = false,
                                        bool whole_objects = false) = 0;

    /// @brief Objects merged into every printer.objects.subscribe the discovery sequence sends
    ///
    /// The result is a printer.objects.subscribe objects map (object name to null or an array
    /// of field names). It is unioned into the app's map and can never narrow it.
    ///
    /// Provider contract:
    /// - It runs on the WebSocket thread (discovery and every reconnect) and on whichever
    ///   thread calls refresh_subscription(), while the client holds an internal mutex.
    /// - It returns a snapshot guarded by its own lock. It must not touch Lua, PluginHost,
    ///   LVGL or the UpdateQueue, and must not call back into this client.
    /// - It must not take a lock that a thread clearing the provider holds at that moment,
    ///   or the clear deadlocks.
    /// - Lifetime: the owner clears it with an empty function before anything it captures
    ///   dies (the client outlives PluginHost). Once that call returns, the old provider is
    ///   not running and is never called again.
    virtual void set_subscription_extras_provider(std::function<json()> provider) = 0;

    /// @brief Re-send printer.objects.subscribe with the app objects merged with current extras
    ///
    /// Safe from any thread and cheap when nothing changed. A call made while no refresh can
    /// be sent (before this connection's discovery subscribe completes, or while a refresh is
    /// in flight) is remembered and re-checked once that settles. While disconnected it does
    /// nothing; the next connection's discovery subscribes the current extras.
    virtual void refresh_subscription() = 0;

    // ========================================================================
    // Connection State & Observers
    // ========================================================================

    /// @brief Get current connection state
    virtual ConnectionState get_connection_state() const = 0;

    /// @brief Register an additional on-connected observer
    virtual void add_connected_observer(const std::string& handler_name,
                                        helix::SharedCallback<void()> cb) = 0;

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

    /// @brief Counter bumped by every connect, so a cached per-connection
    /// answer can tell it belongs to an earlier connection.
    virtual uint64_t connection_generation() const = 0;
};

/**
 * Platform-provided client factory for embedded targets. NOT defined in the
 * desktop build — the ESP32 firmware tree implements it over
 * esp_websocket_client (MoonrakerManager calls it when ESP_PLATFORM is defined).
 */
std::unique_ptr<IMoonrakerClient> create_platform_moonraker_client();

} // namespace helix
