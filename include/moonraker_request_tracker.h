// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "moonraker_error.h"
#include "moonraker_events.h"
#include "moonraker_request.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hv {
class WebSocketClient;
}

class MoonrakerRequestTrackerTestAccess;

namespace helix {

using ::json;

/// @brief Unique identifier for JSON-RPC requests (valid IDs > 0)
using RequestId = uint64_t;

/// @brief Invalid request ID constant
constexpr RequestId INVALID_REQUEST_ID = 0;

/**
 * @brief Owns pending JSON-RPC request lifecycle
 *
 * Handles request ID generation, registration, timeout checking,
 * response routing, and disconnect cleanup. Uses two-phase lock
 * pattern: copy callbacks under lock, invoke outside lock.
 */
class MoonrakerRequestTracker {
  public:
    static constexpr size_t MAX_PENDING_REQUESTS = 500;

    MoonrakerRequestTracker() = default;

    // Non-copyable (has mutex)
    MoonrakerRequestTracker(const MoonrakerRequestTracker&) = delete;
    MoonrakerRequestTracker& operator=(const MoonrakerRequestTracker&) = delete;

    /**
     * @brief Send JSON-RPC request and register for response tracking
     *
     * Builds JSON-RPC envelope, sends via WebSocket, registers pending request.
     *
     * @param ws WebSocket client to send through
     * @param method RPC method name
     * @param params JSON parameters (can be null)
     * @param success_cb Success callback
     * @param error_cb Error callback (optional)
     * @param timeout_ms Timeout override (0 = use default)
     * @param silent Suppress RPC_ERROR events
     * @param intent Explicit caller intent, captured before any internal callback
     *        wrapping. When omitted it is inferred from @p silent and the presence
     *        of @p error_cb, which is what the ~130 direct callers below the gcode
     *        APIs rely on: they carry no `!!` channel, so "supplied an error_cb"
     *        is still a fair proxy for "will report this itself".
     * @return Request ID, or INVALID_REQUEST_ID on error
     */
    RequestId send(hv::WebSocketClient& ws, const std::string& method, const json& params,
                   std::function<void(const json&)> success_cb,
                   std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
                   bool silent = false,
                   std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt);

    /**
     * @brief Send fire-and-forget JSON-RPC (no callbacks, no tracking)
     *
     * @param ws WebSocket client to send through
     * @param method RPC method name
     * @param params JSON parameters (can be null/empty)
     * @return 0 on success, negative on error
     */
    int send_fire_and_forget(hv::WebSocketClient& ws, const std::string& method,
                             const json& params);

    /**
     * @brief Route an incoming JSON-RPC response to its pending request
     *
     * Matches response ID to pending request, copies callbacks under lock,
     * invokes outside lock. Handles both success and JSON-RPC error responses.
     *
     * @param msg Parsed JSON message containing "id" field
     * @param emit_event Function to emit transport events (type, message, is_error, details)
     * @param suppress_error_toast Optional predicate — when it returns true, RPC error
     *        events are logged at debug level instead of error (e.g., during shutdown)
     * @return true if message was a tracked response, false if not a response or unknown ID
     */
    bool route_response(
        const json& msg,
        std::function<void(MoonrakerEventType, const std::string&, bool, const std::string&)>
            emit_event,
        std::function<bool()> suppress_error_toast = nullptr);

    /**
     * @brief Cancel a pending request (no callbacks invoked)
     *
     * @param id Request ID to cancel
     * @return true if request was found and cancelled
     */
    bool cancel(RequestId id);

    /**
     * @brief Check for timed-out requests and invoke error callbacks
     *
     * Two-phase: collect timed-out requests under lock, invoke callbacks outside lock.
     *
     * @param emit_event Function to emit transport events
     */
    void check_timeouts(
        std::function<void(MoonrakerEventType, const std::string&, bool, const std::string&)>
            emit_event);

    /**
     * @brief Cancel all pending requests, invoking error callbacks with connection_lost
     *
     * Called on disconnect. Two-phase pattern preserved.
     */
    void cleanup_all();

    /** @brief Set default request timeout in milliseconds */
    void set_default_timeout(uint32_t timeout_ms) {
        default_request_timeout_ms_ = timeout_ms;
    }

    /** @brief Get default request timeout in milliseconds */
    [[nodiscard]] uint32_t get_default_timeout() const {
        return default_request_timeout_ms_;
    }

    /// Default timeout for Moonraker requests (tool changes, moves, etc.)
    static constexpr uint32_t DEFAULT_REQUEST_TIMEOUT_MS = 60000; // 60s

    /// Floor on the oldest pending request's age before check_timeouts() logs the
    /// periodic pending-count line at debug. A request that has been in flight for
    /// tens of milliseconds is simply working; only a queue that is failing to
    /// drain is worth a line. See the throttle note in check_timeouts().
    static constexpr uint32_t PENDING_LOG_MIN_AGE_MS = 1000; // 1s

    /// Age at which the periodic pending-count line escalates from debug to warn.
    static constexpr uint32_t PENDING_WARN_AGE_MS = 30000; // 30s

    /// Same, for a long-running G-code script. `printer.gcode.script` does not
    /// return until Klipper finishes the macro, and an AFC toolchange over a 2 m
    /// bowden is legitimately a minute or more, so the 30 s threshold flagged
    /// healthy work: 77 warnings in one AFC bundle, all of them normal lane
    /// loads. Past five minutes a script really has stopped.
    static constexpr uint32_t PENDING_WARN_AGE_GCODE_MS = 300000; // 5min

  private:
    std::map<uint64_t, PendingRequest> pending_requests_;
    std::mutex requests_mutex_;
    std::atomic_uint64_t request_id_{0};
    uint32_t default_request_timeout_ms_{DEFAULT_REQUEST_TIMEOUT_MS};

    // Log-throttle state for the periodic pending-count line in check_timeouts()
    // (timer thread only). Without it a single stuck request logs ~4x/sec for
    // its whole lifetime. Emit only when the pending signature changes; re-emit
    // a still-stuck warn at most once per 10 s so the #909 leading indicator
    // stays visible without flooding.
    size_t last_logged_pending_count_{0};
    std::string last_logged_oldest_method_;
    bool last_logged_was_warn_{false};
    std::chrono::steady_clock::time_point last_warn_log_{};

    friend class ::MoonrakerRequestTrackerTestAccess;
};

} // namespace helix
