// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MOCK_WEBSOCKET_SERVER_H
#define MOCK_WEBSOCKET_SERVER_H

/**
 * @file mock_websocket_server.h
 * @brief Mock WebSocket server for testing MoonrakerClient
 *
 * Provides a real WebSocket server using libhv that can accept connections,
 * parse JSON-RPC requests, and send responses. Used for integration testing
 * of MoonrakerClient without requiring a real Moonraker instance.
 *
 * @example
 * MockWebSocketServer server;
 * server.on_method("printer.info", [](const json& params) {
 *     return json{{"state", "ready"}, {"hostname", "test"}};
 * });
 * server.start();
 *
 * client.connect(server.url().c_str(), on_connected, on_disconnected);
 */

#include "hv/WebSocketServer.h"
#include "json_fwd.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

/**
 * @brief Mock WebSocket server for testing JSON-RPC clients
 *
 * Thread-safe mock server that:
 * - Accepts WebSocket connections on localhost
 * - Parses incoming JSON-RPC requests
 * - Routes requests to registered handlers
 * - Sends JSON-RPC responses with matching IDs
 * - Can send unsolicited notifications
 * - Tracks connection and request statistics
 */
class MockWebSocketServer {
  public:
    /**
     * @brief Handler function type for JSON-RPC methods
     *
     * @param params The "params" field from the JSON-RPC request
     * @return JSON result to include in response, or nullptr for error
     */
    using Handler = std::function<json(const json& params)>;

    /**
     * @brief Error handler type for generating JSON-RPC errors
     *
     * @param params The "params" field from the JSON-RPC request
     * @return Pair of (error_code, error_message)
     */
    using ErrorHandler = std::function<std::pair<int, std::string>(const json& params)>;

    MockWebSocketServer();
    ~MockWebSocketServer();

    // Non-copyable
    MockWebSocketServer(const MockWebSocketServer&) = delete;
    MockWebSocketServer& operator=(const MockWebSocketServer&) = delete;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Start the server
     *
     * @param port Port to listen on (0 = ephemeral, system assigns)
     * @return Actual port number on success, -1 on failure
     */
    int start(int port = 0);

    /**
     * @brief Stop the server and close all connections
     */
    void stop();

    /**
     * @brief Check if server is running
     */
    bool is_running() const {
        return running_.load();
    }

    /**
     * @brief Get the port number (valid after start())
     */
    int port() const {
        return port_.load();
    }

    /**
     * @brief Get the WebSocket URL for connecting
     *
     * @return URL like "ws://127.0.0.1:12345/websocket"
     */
    std::string url() const;

    // =========================================================================
    // Handler Registration
    // =========================================================================

    /**
     * @brief Register a handler for a specific JSON-RPC method
     *
     * @param method Method name (e.g., "printer.info")
     * @param handler Function that returns the result JSON
     */
    void on_method(const std::string& method, Handler handler);

    /**
     * @brief Register an error handler for a specific JSON-RPC method
     *
     * @param method Method name
     * @param handler Function that returns (error_code, error_message)
     */
    void on_method_error(const std::string& method, ErrorHandler handler);

    /**
     * @brief Register a fallback handler for unregistered methods
     *
     * @param handler Function that handles any unmatched method
     */
    void on_any_method(Handler handler);

    /**
     * @brief Clear all registered handlers
     */
    void clear_handlers();

    // =========================================================================
    // Test Control
    // =========================================================================

    /**
     * @brief Set artificial delay before sending responses
     *
     * @param ms Delay in milliseconds (0 = immediate)
     */
    void set_response_delay_ms(int ms) {
        response_delay_ms_.store(ms);
    }

    /**
     * @brief Send a notification to all connected clients
     *
     * @param method Notification method name
     * @param params Notification parameters
     */
    void send_notification(const std::string& method, const json& params);

    /**
     * @brief Send a notification to a specific channel
     *
     * @param channel_id Channel to send to (from connection_ids())
     * @param method Notification method name
     * @param params Notification parameters
     */
    void send_notification_to(int channel_id, const std::string& method, const json& params);

    /**
     * @brief Disconnect all connected clients
     */
    void disconnect_all();

    // =========================================================================
    // Statistics
    // =========================================================================

    /**
     * @brief Get number of currently connected clients
     */
    int connection_count() const {
        return connection_count_.load();
    }

    /// @p name from the most recent connection's upgrade request, "" if it had none.
    std::string last_upgrade_header(const std::string& name) const {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        const auto it = last_upgrade_headers_.find(name);
        return it == last_upgrade_headers_.end() ? std::string{} : it->second;
    }

    /**
     * @brief Get total number of requests received
     */
    int request_count() const {
        return request_count_.load();
    }

    /**
     * @brief Get list of all methods that were called
     */
    std::vector<std::string> received_methods() const;

    /**
     * @brief Reset all statistics
     */
    void reset_stats();

  private:
    void setup_callbacks();
    void handle_message(const WebSocketChannelPtr& channel, const std::string& msg);
    void send_response(const WebSocketChannelPtr& channel, uint64_t id, const json& result);
    void send_error(const WebSocketChannelPtr& channel, uint64_t id, int code,
                    const std::string& message);

    std::unique_ptr<hv::WebSocketService> ws_service_;
    std::unique_ptr<hv::WebSocketServer> server_;

    std::map<std::string, Handler> handlers_;
    std::map<std::string, ErrorHandler> error_handlers_;
    Handler fallback_handler_;
    mutable std::mutex handlers_mutex_;

    std::vector<WebSocketChannelPtr> channels_;
    mutable std::mutex channels_mutex_;
    http_headers last_upgrade_headers_; ///< Guarded by channels_mutex_.

    std::vector<std::string> received_methods_;
    mutable std::mutex methods_mutex_;

    std::atomic<int> port_{0};
    std::atomic<bool> running_{false};
    std::atomic<int> connection_count_{0};
    std::atomic<int> request_count_{0};
    std::atomic<int> response_delay_ms_{0};
};

#endif // MOCK_WEBSOCKET_SERVER_H
