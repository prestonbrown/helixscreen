// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file moonraker_api_internal.h
 * @brief Internal helpers shared across MoonrakerAPI implementation files
 *
 * This header is NOT part of the public API. It provides validation and
 * utility functions used by the split moonraker_api_*.cpp implementation files.
 */

#if !defined(HELIX_PLATFORM_ESP32)
#include "hv/HttpMessage.h" // HttpResponse — only the REST/HTTP sub-APIs use it
#endif
#include "json_fwd.h"
#include "json_utils.h"
#include "moonraker_api.h"
#include "moonraker_validation.h"
#include "spdlog/spdlog.h"
#include "system/telemetry_manager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

namespace moonraker_internal {

/**
 * @brief Report an HTTP error with automatic type mapping
 *
 * Maps HTTP status codes to appropriate MoonrakerErrorType:
 * - 404 -> FILE_NOT_FOUND
 * - 403 -> PERMISSION_DENIED
 * - Other -> UNKNOWN
 *
 * @param on_error Error callback (may be nullptr)
 * @param status_code HTTP status code
 * @param method Method name for error context
 * @param status_message HTTP status message
 */
inline void report_http_error(const MoonrakerAPI::ErrorCallback& on_error, int status_code,
                              std::string_view method, std::string_view status_message) {
    // Record telemetry for server errors and client errors (except 404, which is common)
    if (status_code != 404) {
        std::string code_str = (status_code >= 500) ? "http_5xx" : "http_4xx";
        TelemetryManager::instance().record_error("moonraker_api", code_str, std::string(method));
    }

    if (!on_error)
        return;

    MoonrakerErrorType type;
    if (status_code == 404) {
        type = MoonrakerErrorType::FILE_NOT_FOUND;
    } else if (status_code == 403) {
        type = MoonrakerErrorType::PERMISSION_DENIED;
    } else {
        type = MoonrakerErrorType::UNKNOWN;
    }

    MoonrakerError err;
    err.type = type;
    err.code = status_code;
    err.method = std::string(method);
    err.message = "HTTP " + std::to_string(status_code) + ": " + std::string(status_message);
    on_error(err);
}

/**
 * @brief Report a connection error (convenience wrapper)
 *
 * @param on_error Error callback (may be nullptr)
 * @param method Method name for error context
 * @param message Human-readable error message
 */
inline void report_connection_error(const MoonrakerAPI::ErrorCallback& on_error,
                                    std::string_view method, std::string_view message) {
    TelemetryManager::instance().record_error("moonraker_api", "connection_refused",
                                              std::string(method));
    report_error(on_error, MoonrakerErrorType::CONNECTION_LOST, method, message);
}

/**
 * @brief Report a parse error (convenience wrapper)
 *
 * @param on_error Error callback (may be nullptr)
 * @param method Method name for error context
 * @param message Human-readable error message
 */
inline void report_parse_error(const MoonrakerAPI::ErrorCallback& on_error, std::string_view method,
                               std::string_view message) {
    TelemetryManager::instance().record_error("moonraker_api", "parse_error", std::string(method));
    report_error(on_error, MoonrakerErrorType::PARSE_ERROR, method, message);
}

// ============================================================================
// HTTP RESPONSE HANDLING
// ============================================================================
// Consolidates the repeated HTTP response validation pattern:
// 1. Check for null response (connection lost)
// 2. Map HTTP status codes to error types
// 3. Return success/failure
//
// Usage:
//   if (!handle_http_response(resp, "download_file", on_error)) return;
//   if (!handle_http_response(resp, "upload_file", on_error, 201)) return;
//   if (!handle_http_response(resp, "download_partial", on_error, {200, 206})) return;

/**
 * @brief Map HTTP status code to MoonrakerErrorType
 *
 * @param status_code HTTP status code
 * @return Appropriate MoonrakerErrorType
 */
inline MoonrakerErrorType http_status_to_error_type(int status_code) {
    switch (status_code) {
    case 404:
        return MoonrakerErrorType::FILE_NOT_FOUND;
    case 401:
    case 403:
        return MoonrakerErrorType::PERMISSION_DENIED;
    default:
        return MoonrakerErrorType::UNKNOWN;
    }
}

/**
 * @brief Handle HTTP response with single expected status code
 *
 * Consolidates the common HTTP error handling pattern:
 * - Null response -> CONNECTION_LOST error
 * - Non-matching status code -> appropriate error type
 * - Matching status code -> success (true)
 *
 * @param resp HTTP response (may be nullptr)
 * @param method Method name for error context
 * @param on_error Error callback (may be nullptr)
 * @param expected Expected HTTP status code (default: 200)
 * @return true if response is valid and has expected status, false otherwise
 */
#if !defined(HELIX_PLATFORM_ESP32) // HTTP response handlers — REST/HTTP sub-APIs only
inline bool handle_http_response(const std::shared_ptr<HttpResponse>& resp, std::string_view method,
                                 const MoonrakerAPI::ErrorCallback& on_error, int expected = 200) {
    if (!resp) {
        report_error(on_error, MoonrakerErrorType::CONNECTION_LOST, method, "No response received");
        return false;
    }

    if (resp->status_code != expected) {
        MoonrakerErrorType type = http_status_to_error_type(resp->status_code);
        std::string message =
            "HTTP " + std::to_string(resp->status_code) + ": " + resp->status_message();
        report_error(on_error, type, method, message, resp->status_code);
        return false;
    }

    return true;
}

/**
 * @brief Handle HTTP response with multiple acceptable status codes
 *
 * Useful for operations that accept multiple success codes (e.g., 200 or 206 for downloads).
 *
 * @param resp HTTP response (may be nullptr)
 * @param method Method name for error context
 * @param on_error Error callback (may be nullptr)
 * @param expected_codes List of acceptable HTTP status codes
 * @return true if response is valid and has one of the expected codes, false otherwise
 */
inline bool handle_http_response(const std::shared_ptr<HttpResponse>& resp, std::string_view method,
                                 const MoonrakerAPI::ErrorCallback& on_error,
                                 std::initializer_list<int> expected_codes) {
    if (!resp) {
        report_error(on_error, MoonrakerErrorType::CONNECTION_LOST, method, "No response received");
        return false;
    }

    for (int code : expected_codes) {
        if (resp->status_code == code) {
            return true;
        }
    }

    // Status code not in expected list
    MoonrakerErrorType type = http_status_to_error_type(resp->status_code);
    std::string message =
        "HTTP " + std::to_string(resp->status_code) + ": " + resp->status_message();
    report_error(on_error, type, method, message, resp->status_code);
    return false;
}
#endif // !HELIX_PLATFORM_ESP32

// ============================================================================
// JSON EXTRACTION HELPERS
// ============================================================================
// Null-safe JSON field extraction. Unlike json::value(), handles fields that
// exist but are null, returning the default value in both cases. Plain numbers
// go through helix::json_util (json_utils.h), which also coerces the JSON strings
// some Moonraker forks send (prestonbrown/helixscreen#1713).

/**
 * @brief A count such as layer_count, as a uint32_t
 *
 * A negative or out-of-range value reads as 0 rather than wrapping into a huge count.
 */
inline uint32_t json_count_or_zero(const nlohmann::json& j, const char* key) {
    return static_cast<uint32_t>(std::max(0, helix::json_util::safe_int(j, key)));
}

/**
 * @brief Normalize a Moonraker metadata field that may be a semicolon-delimited string,
 *        a JSON array, or a stringified JSON array into a semicolon-delimited string.
 *
 * Moonraker returns filament_type (and similar fields) in multiple formats depending
 * on the slicer and firmware:
 *   - Semicolon-delimited string: "PLA;PLA;ASA;PETG"
 *   - JSON array: ["PLA", "PLA", "ASA", "PETG"]
 *   - Stringified JSON array: "[\"PLA\", \"PLA\", \"ASA\"]"
 *
 * @param obj JSON object containing the field
 * @param key Field name to extract
 * @return Semicolon-delimited string, or empty if not present
 */
inline std::string json_string_list_or(const nlohmann::json& obj, const char* key) {
    if (!obj.contains(key))
        return {};

    auto join_array = [](const nlohmann::json& arr) -> std::string {
        std::string joined;
        for (const auto& item : arr) {
            if (item.is_string()) {
                if (!joined.empty())
                    joined += ';';
                joined += item.get<std::string>();
            }
        }
        return joined;
    };

    const auto& val = obj[key];

    if (val.is_array())
        return join_array(val);
    if (!val.is_string())
        return {};

    std::string raw = val.get<std::string>();
    if (raw.empty())
        return {};

    // Stringified JSON array
    if (raw.front() == '[') {
        auto arr = nlohmann::json::parse(raw, nullptr, false);
        if (arr.is_array())
            return join_array(arr);
    }

    return raw;
}

/**
 * @brief Parse per-tool filament weight metadata into a vector of doubles.
 *
 * Slicers vary in how they expose per-tool filament usage. We accept:
 *   - "filament_weights" as a JSON array of numbers (grams)         — preferred
 *   - "filament_used"    as a JSON array of numbers (mm or grams)   — fallback
 *   - "filament_used"    as a comma/semicolon-delimited string       — fallback
 *
 * For the fallbacks, only the >0 / ==0 distinction is meaningful to callers
 * (which use this to decide if a tool actually extrudes); unit mismatches
 * (mm vs grams) don't affect that decision.
 *
 * Numeric strings ("12.5") parse like numbers, since some Moonraker forks write
 * metadata as JSON strings (prestonbrown/helixscreen#1713). Any entry that still
 * does not parse empties the whole result: a 0.0 in its place would read as
 * "tool unused" and skip that tool's checks.
 *
 * Returns an empty vector when the slicer emitted no per-tool data. Callers
 * MUST treat empty as "unknown — check every tool" rather than "all zero".
 */
inline std::vector<double> parse_filament_weights(const nlohmann::json& obj) {
    auto parse_text = [](const std::string& text, double& out) {
        const auto parsed = helix::text_io::parse_leading<double>(text);
        if (!parsed) {
            return false;
        }
        out = *parsed;
        return std::isfinite(out);
    };
    auto parse_array = [&parse_text](const nlohmann::json& arr) {
        std::vector<double> weights;
        for (const auto& v : arr) {
            double w = 0.0;
            if (v.is_number()) {
                w = v.get<double>();
            } else if (!v.is_string() || !parse_text(v.get<std::string>(), w)) {
                return std::vector<double>{};
            }
            weights.push_back(w);
        }
        return weights;
    };

    if (obj.contains("filament_weights") && obj["filament_weights"].is_array()) {
        return parse_array(obj["filament_weights"]);
    }
    if (obj.contains("filament_used") && obj["filament_used"].is_array()) {
        return parse_array(obj["filament_used"]);
    }
    std::vector<double> weights;
    if (obj.contains("filament_used") && obj["filament_used"].is_string()) {
        std::string used_str = obj["filament_used"].get<std::string>();
        const char* delims = ";,";
        size_t pos = 0;
        while (pos < used_str.size()) {
            size_t end = used_str.find_first_of(delims, pos);
            if (end == std::string::npos) {
                end = used_str.size();
            }
            double w = 0.0;
            if (!parse_text(used_str.substr(pos, end - pos), w)) {
                return {};
            }
            weights.push_back(w);
            pos = end + 1;
        }
    }
    return weights;
}

} // namespace moonraker_internal
