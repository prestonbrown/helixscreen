// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_client_cli.h
 * @brief Minimal Moonraker WebSocket client for CLI tools
 *
 * This is a standalone version of MoonrakerClient without dependencies
 * on PrinterState, TelemetryManager, or other application components.
 * Designed for use in diagnostic tools like moonraker-inspector.
 */

#ifndef MOONRAKER_CLIENT_CLI_H
#define MOONRAKER_CLIENT_CLI_H

#include <hv/WebSocketClient.h>

#include <atomic>
#include <functional>
#include <json.hpp>
#include <memory>
#include <string>
#include <unordered_map>

namespace helix {
namespace cli {

/// Error information for Moonraker API errors
struct MoonrakerError {
    std::string message;
    std::string code;

    MoonrakerError() = default;
    MoonrakerError(const std::string& msg, const std::string& c = "") : message(msg), code(c) {}
};

/// Connection state enumeration
enum class ConnectionState { Disconnected, Connecting, Connected };

/// Minimal Moonraker client for CLI tools
class MoonrakerClient {
  public:
    using JsonCallback = std::function<void(nlohmann::json)>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using StateCallback = std::function<void()>;

    MoonrakerClient();
    explicit MoonrakerClient(std::shared_ptr<hv::EventLoop> loop);
    ~MoonrakerClient();

    // =========================================================================
    // Connection Management
    // =========================================================================

    /**
     * @brief Connect to Moonraker WebSocket
     * @param url WebSocket URL (e.g., "ws://192.168.1.100:7125/websocket")
     * @param on_connect Callback when connected
     * @param on_disconnect Callback when disconnected
     * @return 0 on success, error code otherwise
     */
    int connect(const char* url, StateCallback on_connect, StateCallback on_disconnect);

    /**
     * @brief Disconnect from Moonraker
     */
    void disconnect();

    /**
     * @brief Check if connected
     */
    bool isConnected() const {
        return state_ == ConnectionState::Connected;
    }

    /**
     * @brief Get current connection state
     */
    ConnectionState getState() const {
        return state_;
    }

    // =========================================================================
    // JSON-RPC API
    // =========================================================================

    /**
     * @brief Send a JSON-RPC request (fire-and-forget)
     * @param method RPC method name
     */
    void send_jsonrpc(const std::string& method);

    /**
     * @brief Send a JSON-RPC request with parameters
     * @param method RPC method name
     * @param params JSON parameters
     */
    void send_jsonrpc(const std::string& method, const nlohmann::json& params);

    /**
     * @brief Send a JSON-RPC request with callbacks
     * @param method RPC method name
     * @param params JSON parameters (optional)
     * @param on_success Callback on successful response
     * @param on_error Callback on error
     * @param timeout_ms Request timeout in milliseconds (default 10000)
     * @param require_result Whether to require 'result' field (default true)
     */
    void send_jsonrpc(const std::string& method, const nlohmann::json& params,
                      JsonCallback on_success, ErrorCallback on_error,
                      unsigned int timeout_ms = 10000, bool require_result = true);

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Configure connection timeouts
     * @param connect_timeout_ms Connection timeout
     * @param read_timeout_ms Read timeout
     * @param write_timeout_ms Write timeout
     * @param keepalive_interval_ms Keepalive ping interval
     * @param keepalive_timeout_ms Keepalive response timeout
     */
    void configure_timeouts(int connect_timeout_ms, int read_timeout_ms, int write_timeout_ms,
                            int keepalive_interval_ms, int keepalive_timeout_ms);

  private:
    // Internal helpers
    void setup_callbacks();
    void onMessage(const std::string& msg);
    void onClose();
    void onOpen();

    void dispatch_response(const nlohmann::json& response);
    void set_state(ConnectionState state);

    // The underlying WebSocket client
    std::unique_ptr<hv::WebSocketClient> ws_client_;

    // State
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    StateCallback on_connect_;
    StateCallback on_disconnect_;

    // Request tracking (simplified for CLI)
    struct PendingRequest {
        JsonCallback on_success;
        ErrorCallback on_error;
        uint64_t timeout_at;
        bool require_result;
    };

    std::unordered_map<uint64_t, PendingRequest> pending_requests_;
    uint64_t next_request_id_ = 1;

    // Timeouts
    int connect_timeout_ms_ = 5000;
    int keepalive_interval_ms_ = 200;
    int keepalive_timeout_ms_ = 2000;
};

} // namespace cli
} // namespace helix

#endif // MOONRAKER_CLIENT_CLI_H
