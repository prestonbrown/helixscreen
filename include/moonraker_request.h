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

#ifndef MOONRAKER_REQUEST_H
#define MOONRAKER_REQUEST_H

#include "moonraker_error.h"

#include <chrono>
#include <functional>
#include <string>

#include "hv/json.hpp"

using json = nlohmann::json;

/**
 * @brief Structure to track pending JSON-RPC requests
 *
 * Stores request metadata for timeout tracking and callback management.
 */
struct PendingRequest {
    uint64_t id;        ///< JSON-RPC request ID
    std::string method; ///< Method name for logging
    std::function<void(json)>
        success_callback; ///< Success callback (pass-by-value for thread safety)
    std::function<void(const MoonrakerError&)> error_callback; ///< Error callback (optional)
    std::chrono::steady_clock::time_point timestamp;           ///< When request was sent
    uint32_t timeout_ms;                                       ///< Timeout in milliseconds

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
        return elapsed.count();
    }
};

#endif // MOONRAKER_REQUEST_H