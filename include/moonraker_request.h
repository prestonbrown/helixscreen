// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "moonraker_error.h"
#include "rpc_error_policy.h"

#include <chrono>
#include <functional>
#include <string>

/**
 * @brief Structure to track pending JSON-RPC requests
 *
 * Stores request metadata for timeout tracking and callback management.
 */
struct PendingRequest {
    uint64_t id;        ///< JSON-RPC request ID
    std::string method; ///< Method name for logging
    std::function<void(const json&)>
        success_callback; ///< Success callback (pass-by-value for thread safety)
    std::function<void(const MoonrakerError&)> error_callback; ///< Error callback (optional)
    std::chrono::steady_clock::time_point timestamp;           ///< When request was sent
    uint32_t timeout_ms;                                       ///< Timeout in milliseconds
    /// What the caller told us about who reports this error to the user, captured
    /// before any internal callback wrapping. Consumed by
    /// helix::rpc_error_policy::decide() — see include/rpc_error_policy.h.
    helix::rpc_error_policy::CallerIntent intent{};

    /**
     * @brief Check if request has timed out
     *
     * @return true if timeout has been exceeded
     */
    bool is_timed_out() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - timestamp);
        return elapsed.count() > timeout_ms;
    }

    /**
     * @brief Get elapsed time since request was sent
     *
     * @return Elapsed time in milliseconds
     */
    uint32_t get_elapsed_ms() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - timestamp);
        return static_cast<uint32_t>(elapsed.count());
    }
};