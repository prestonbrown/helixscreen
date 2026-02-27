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

#include "hv/HttpMessage.h"
#include "moonraker_api.h"
#include "spdlog/spdlog.h"
#include "system/telemetry_manager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

#include "hv/json.hpp"

namespace moonraker_internal {

/**
 * @brief Validate that a string contains only safe identifier characters
 *
 * Allows alphanumeric, underscore, and space (for names like "heater_generic chamber").
 * Rejects newlines, semicolons, and other G-code control characters.
 *
 * @param str String to validate
 * @return true if safe, false otherwise
 */
inline bool is_safe_identifier(const std::string& str) {
    if (str.empty()) {
        return false;
    }

    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ' ';
    });
}

/**
 * @brief Validate that a file path is safe from directory traversal attacks
 *
 * Rejects paths containing:
 * - Parent directory references (..)
 * - Absolute paths (starting with /)
 * - Null bytes (path truncation attack)
 * - Windows-style absolute paths (C:, D:, etc)
 * - Suspicious characters (<>|*?)
 *
 * @param path File path to validate
 * @return true if safe relative path, false otherwise
 */
inline bool is_safe_path(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    if (path.find("..") != std::string::npos) {
        return false;
    }

    if (path[0] == '/') {
        return false;
    }

    if (path.find('\0') != std::string::npos) {
        return false;
    }

    if (path.size() >= 2 && path[1] == ':') {
        return false;
    }

    const std::string dangerous_chars = "<>|*?";
    if (path.find_first_of(dangerous_chars) != std::string::npos) {
        return false;
    }

    for (char c : path) {
        if (std::iscntrl(static_cast<unsigned char>(c))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Validate that an axis character is valid
 *
 * @param axis Axis character to validate
 * @return true if valid axis (X, Y, Z, E), false otherwise
 */
inline bool is_valid_axis(char axis) {
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(axis)));
    return upper == 'X' || upper == 'Y' || upper == 'Z' || upper == 'E';
}

/**
 * @brief Validate temperature is in safe range
 *
 * @param temp Temperature in Celsius
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
inline bool is_safe_temperature(double temp, const SafetyLimits& limits) {
    return temp >= limits.min_temperature_celsius && temp <= limits.max_temperature_celsius;
}

/**
 * @brief Validate fan speed is in valid percentage range
 *
 * @param speed Speed percentage
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
inline bool is_safe_fan_speed(double speed, const SafetyLimits& limits) {
    return speed >= limits.min_fan_speed_percent && speed <= limits.max_fan_speed_percent;
}

/**
 * @brief Validate feedrate is within safe limits
 *
 * @param feedrate Feedrate in mm/min
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
inline bool is_safe_feedrate(double feedrate, const SafetyLimits& limits) {
    return feedrate >= limits.min_feedrate_mm_min && feedrate <= limits.max_feedrate_mm_min;
}

/**
 * @brief Validate distance is reasonable for axis movement
 *
 * @param distance Distance in mm
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
inline bool is_safe_distance(double distance, const SafetyLimits& limits) {
    return distance >= limits.min_relative_distance_mm &&
           distance <= limits.max_relative_distance_mm;
}

/**
 * @brief Validate position is reasonable for axis positioning
 *
 * @param position Position in mm
 * @param limits Safety limits configuration
 * @return true if within configured range, false otherwise
 */
inline bool is_safe_position(double position, const SafetyLimits& limits) {
    return position >= limits.min_absolute_position_mm &&
           position <= limits.max_absolute_position_mm;
}

// ============================================================================
// VALIDATION + ERROR HELPERS
// ============================================================================
// These functions combine validation with error callback invocation, reducing
// the ~10-line boilerplate pattern repeated in every API method.
//
// Usage:
//   if (reject_invalid_path(filename, "method_name", on_error)) return;
//   if (reject_invalid_path(filename, "method_name", on_error, silent)) return;

/**
 * @brief Validate path and invoke error callback if invalid
 *
 * Consolidates the common pattern of:
 * 1. Check is_safe_path()
 * 2. NOTIFY_ERROR if !silent
 * 3. Construct MoonrakerError and call on_error if provided
 *
 * @param path Path to validate
 * @param method Method name for error context
 * @param on_error Error callback (may be nullptr)
 * @param silent If true, skip NOTIFY_ERROR (default: false)
 * @return true if path is INVALID (caller should return), false if valid
 */
inline bool reject_invalid_path(const std::string& path, const char* method,
                                const MoonrakerAPI::ErrorCallback& on_error, bool silent = false) {
    if (is_safe_path(path)) {
        return false; // Valid, continue
    }

    if (!silent) {
        spdlog::error("[Moonraker API] {}: Invalid path '{}'", method, path);
    }

    if (on_error) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::VALIDATION_ERROR;
        err.message = "Invalid path contains directory traversal or illegal characters";
        err.method = method;
        on_error(err);
    }
    return true; // Invalid, caller should return
}

/**
 * @brief Validate identifier and invoke error callback if invalid
 *
 * For validating root names, heater names, etc.
 *
 * @param id Identifier to validate
 * @param method Method name for error context
 * @param on_error Error callback (may be nullptr)
 * @param silent If true, skip error logging (default: false)
 * @return true if identifier is INVALID (caller should return), false if valid
 */
inline bool reject_invalid_identifier(const std::string& id, const char* method,
                                      const MoonrakerAPI::ErrorCallback& on_error,
                                      bool silent = false) {
    if (is_safe_identifier(id)) {
        return false; // Valid, continue
    }

    if (!silent) {
        spdlog::error("[Moonraker API] {}: Invalid identifier '{}'", method, id);
    }

    if (on_error) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::VALIDATION_ERROR;
        err.message = "Invalid identifier contains illegal characters";
        err.method = method;
        on_error(err);
    }
    return true; // Invalid, caller should return
}

/**
 * @brief Check if value is in range and invoke error callback if not
 *
 * For validating temperatures, speeds, positions, etc.
 *
 * @param value Value to check
 * @param min Minimum allowed value
 * @param max Maximum allowed value
 * @param param_name Parameter name for error message (e.g., "temperature")
 * @param method Method name for error context
 * @param on_error Error callback (may be nullptr)
 * @param silent If true, skip error logging (default: false)
 * @return true if value is OUT OF RANGE (caller should return), false if valid
 */
inline bool reject_out_of_range(double value, double min, double max, const char* param_name,
                                const char* method, const MoonrakerAPI::ErrorCallback& on_error,
                                bool silent = false) {
    if (value >= min && value <= max) {
        return false; // Valid, continue
    }

    if (!silent) {
        spdlog::error("[Moonraker API] {}: {} {} out of range [{}, {}]", method, param_name, value,
                      min, max);
    }

    if (on_error) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::VALIDATION_ERROR;
        err.message = std::string(param_name) + " value out of allowed range";
        err.method = method;
        on_error(err);
    }
    return true; // Invalid, caller should return
}

/**
 * @brief Check if any double value is NaN or Inf, and reject with error callback if so
 *
 * Prevents malformed G-code from being sent to the printer when upstream data
 * contains NaN or Inf values (e.g., from corrupted JSON or division by zero).
 *
 * @param values List of double values to check
 * @param method Method name for error context
 * @param on_error Error callback (may be nullptr)
 * @return true if any value is NON-FINITE (caller should return), false if all valid
 */
inline bool reject_non_finite(std::initializer_list<double> values, const char* method,
                              const MoonrakerAPI::ErrorCallback& on_error) {
    for (double v : values) {
        if (std::isnan(v) || std::isinf(v)) {
            spdlog::warn("[Moonraker API] {}: Rejecting G-code generation: "
                         "invalid value (NaN/Inf)",
                         method);
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::VALIDATION_ERROR;
                err.message = "Parameter contains NaN or Inf value";
                err.method = method;
                on_error(err);
            }
            return true; // Invalid, caller should return
        }
    }
    return false; // All finite, continue
}

// ============================================================================
// ERROR REPORTING HELPERS
// ============================================================================
// These functions consolidate the common pattern of:
// 1. Check if callback exists
// 2. Construct MoonrakerError with appropriate type
// 3. Invoke callback
//
// Usage:
//   report_error(on_error, MoonrakerErrorType::CONNECTION_LOST, "method", "message");
//   report_http_error(on_error, 404, "method", "status message");

/**
 * @brief Report an error via callback with specified type
 *
 * @param on_error Error callback (may be nullptr)
 * @param type Error type classification
 * @param method Method name for error context
 * @param message Human-readable error message
 * @param code Optional error code (default: 0)
 */
inline void report_error(const MoonrakerAPI::ErrorCallback& on_error, MoonrakerErrorType type,
                         std::string_view method, std::string_view message, int code = 0) {
    if (!on_error)
        return;

    MoonrakerError err;
    err.type = type;
    err.method = std::string(method);
    err.message = std::string(message);
    err.code = code;
    on_error(err);
}

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

// ============================================================================
// JSON EXTRACTION HELPERS
// ============================================================================
// Null-safe JSON field extraction. Unlike json::value(), handles fields that
// exist but are null, returning the default value in both cases.

/**
 * @brief Null-safe numeric value extraction from JSON
 *
 * Unlike json::value(), this handles fields that exist but are null.
 * Returns default_val if key is missing OR if value is null/non-numeric.
 *
 * @tparam T Numeric type (double, int, uint64_t, size_t, etc.)
 * @param j JSON object to extract from
 * @param key Field name to extract
 * @param default_val Value to return if missing, null, or non-numeric
 * @return Extracted value or default
 *
 * Example usage:
 *   double temp = json_number_or(obj, "temperature", 0.0);
 *   int count = json_number_or(obj, "layer_count", 0);
 *   size_t size = json_number_or(obj, "size", static_cast<size_t>(0));
 */
template <typename T>
inline T json_number_or(const nlohmann::json& j, const char* key, T default_val) {
    if (j.contains(key) && j[key].is_number()) {
        return j[key].get<T>();
    }
    return default_val;
}

} // namespace moonraker_internal
