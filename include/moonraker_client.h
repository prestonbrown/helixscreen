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
#include "hv/json.hpp"  // libhv's nlohmann json (via cpputil/)
#include "spdlog/spdlog.h"
#include "moonraker_request.h"
#include "moonraker_error.h"

#include <map>
#include <vector>
#include <atomic>
#include <functional>
#include <string>
#include <mutex>

using json = nlohmann::json;

/**
 * @brief Connection state for Moonraker WebSocket
 */
enum class ConnectionState {
  DISCONNECTED,  // Not connected
  CONNECTING,    // Connection in progress
  CONNECTED,     // Connected and ready
  RECONNECTING,  // Automatic reconnection in progress
  FAILED         // Connection failed (max retries exceeded)
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

  /**
   * @brief Connect to Moonraker WebSocket server
   *
   * @param url WebSocket URL (e.g., "ws://127.0.0.1:7125/websocket")
   * @param on_connected Callback invoked when connection opens
   * @param on_disconnected Callback invoked when connection closes
   * @return 0 on success, non-zero on error
   */
  int connect(const char* url,
              std::function<void()> on_connected,
              std::function<void()> on_disconnected);

  /**
   * @brief Register callback for status update notifications
   *
   * Invoked when Moonraker sends "notify_status_update" messages
   * (triggered by printer.objects.subscribe subscriptions).
   *
   * @param cb Callback function receiving parsed JSON notification
   */
  void register_notify_update(std::function<void(json&)> cb);

  /**
   * @brief Register persistent callback for specific notification methods
   *
   * Unlike one-time request callbacks, these persist across multiple messages.
   * Useful for console output, prompt notifications, etc.
   *
   * @param method Notification method name (e.g., "notify_gcode_response")
   * @param handler_name Unique identifier for this handler (for debugging)
   * @param cb Callback function receiving parsed JSON notification
   */
  void register_method_callback(const std::string& method,
                                 const std::string& handler_name,
                                 std::function<void(json&)> cb);

  /**
   * @brief Send JSON-RPC request without parameters
   *
   * @param method RPC method name (e.g., "printer.info")
   * @return 0 on success, non-zero on error
   */
  int send_jsonrpc(const std::string& method);

  /**
   * @brief Send JSON-RPC request with parameters
   *
   * @param method RPC method name
   * @param params JSON parameters object
   * @return 0 on success, non-zero on error
   */
  int send_jsonrpc(const std::string& method, const json& params);

  /**
   * @brief Send JSON-RPC request with one-time response callback
   *
   * Callback is invoked once when response arrives, then removed.
   *
   * @param method RPC method name
   * @param params JSON parameters object
   * @param cb Callback function receiving response JSON
   * @return 0 on success, non-zero on error
   */
  int send_jsonrpc(const std::string& method,
                   const json& params,
                   std::function<void(json&)> cb);

  /**
   * @brief Send JSON-RPC request with success and error callbacks
   *
   * @param method RPC method name
   * @param params JSON parameters object
   * @param success_cb Callback for successful response
   * @param error_cb Callback for errors (timeout, JSON-RPC error, etc.)
   * @param timeout_ms Optional timeout override (0 = use default)
   * @return 0 on success, non-zero on error
   */
  int send_jsonrpc(const std::string& method,
                   const json& params,
                   std::function<void(json&)> success_cb,
                   std::function<void(const MoonrakerError&)> error_cb,
                   uint32_t timeout_ms = 0);

  /**
   * @brief Send G-code script command
   *
   * Convenience wrapper for printer.gcode.script method.
   *
   * @param gcode G-code string (e.g., "G28", "M104 S210")
   * @return 0 on success, non-zero on error
   */
  int gcode_script(const std::string& gcode);

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
   * @brief Get discovered heaters (extruders, beds, generic heaters)
   */
  const std::vector<std::string>& get_heaters() const { return heaters_; }

  /**
   * @brief Get discovered read-only sensors
   */
  const std::vector<std::string>& get_sensors() const { return sensors_; }

  /**
   * @brief Get discovered fans
   */
  const std::vector<std::string>& get_fans() const { return fans_; }

  /**
   * @brief Get discovered LED outputs
   */
  const std::vector<std::string>& get_leds() const { return leds_; }

  /**
   * @brief Get current connection state
   */
  ConnectionState get_connection_state() const { return connection_state_; }

  /**
   * @brief Set callback for connection state changes
   *
   * @param cb Callback invoked when state changes (old_state, new_state)
   */
  void set_state_change_callback(std::function<void(ConnectionState, ConnectionState)> cb) {
    state_change_callback_ = cb;
  }

  /**
   * @brief Set connection timeout in milliseconds
   *
   * @param timeout_ms Connection timeout (default 10000ms)
   */
  void set_connection_timeout(uint32_t timeout_ms) { connection_timeout_ms_ = timeout_ms; }

  /**
   * @brief Set default request timeout in milliseconds
   *
   * @param timeout_ms Request timeout
   */
  void set_default_request_timeout(uint32_t timeout_ms) { default_request_timeout_ms_ = timeout_ms; }

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
  void configure_timeouts(uint32_t connection_timeout_ms,
                          uint32_t request_timeout_ms,
                          uint32_t keepalive_interval_ms,
                          uint32_t reconnect_min_delay_ms,
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
  void process_timeouts() { check_request_timeouts(); }

private:
  /**
   * @brief Transition to new connection state
   *
   * @param new_state The new state to transition to
   */
  void set_connection_state(ConnectionState new_state);

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
  std::vector<std::string> heaters_;   // Controllable heaters (extruders, bed, etc.)
  std::vector<std::string> sensors_;   // Read-only temperature sensors
  std::vector<std::string> fans_;      // All fan types
  std::vector<std::string> leds_;      // LED outputs

private:
  // Pending requests keyed by request ID
  std::map<uint32_t, PendingRequest> pending_requests_;
  std::mutex requests_mutex_;  // Protect pending_requests_ map

  // Persistent notify_status_update callbacks
  std::vector<std::function<void(json&)>> notify_callbacks_;

  // Persistent method-specific callbacks
  // method_name : { handler_name : callback }
  std::map<std::string, std::map<std::string, std::function<void(json&)>>> method_callbacks_;

  // Auto-incrementing JSON-RPC request ID
  std::atomic_uint64_t request_id_;

  // Connection state tracking
  std::atomic_bool was_connected_;
  std::atomic<ConnectionState> connection_state_;
  std::function<void(ConnectionState, ConnectionState)> state_change_callback_;
  uint32_t connection_timeout_ms_;
  uint32_t reconnect_attempts_ = 0;
  uint32_t max_reconnect_attempts_ = 0;  // 0 = infinite

  // Request timeout tracking
  uint32_t default_request_timeout_ms_;

  // Connection parameters (from config)
  uint32_t keepalive_interval_ms_;
  uint32_t reconnect_min_delay_ms_;
  uint32_t reconnect_max_delay_ms_;
};

#endif // MOONRAKER_CLIENT_H
