// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "format_utils.h"
#include "json_fwd.h"

#include <string>

/**
 * @brief Error types for Moonraker operations
 */
enum class MoonrakerErrorType {
    NONE,              ///< No error
    TIMEOUT,           ///< Request timed out
    CONNECTION_LOST,   ///< WebSocket connection lost
    JSON_RPC_ERROR,    ///< JSON-RPC protocol error from Moonraker
    PARSE_ERROR,       ///< JSON parsing failed
    VALIDATION_ERROR,  ///< Response validation failed
    NOT_READY,         ///< Klipper not in ready state
    FILE_NOT_FOUND,    ///< Requested file doesn't exist
    PERMISSION_DENIED, ///< Operation not allowed
    UNKNOWN            ///< Unknown error
};

/**
 * @brief Comprehensive error information for Moonraker operations
 */
struct MoonrakerError {
    MoonrakerErrorType type = MoonrakerErrorType::NONE; ///< Error type classification
    int code = 0;                                       ///< JSON-RPC error code if applicable
    std::string message;                                ///< Human-readable error message
    std::string method;                                 ///< Method that caused the error
    json details;                                       ///< Additional error details from Moonraker

    /**
     * @brief Check if there's an error
     * @return true if error type is not NONE
     */
    bool has_error() const {
        return type != MoonrakerErrorType::NONE;
    }

    /**
     * @brief Did the transport vanish, rather than the printer answering?
     *
     * A dropped WebSocket and an RPC timeout a slow command outlived both say
     * nothing about what the printer did with the command: it may still be
     * running, and its console output arrives on the notification stream a
     * collector is already listening to. Anything else carries Klipper's own
     * complaint and is terminal (prestonbrown/helixscreen#1543).
     */
    bool is_transport_loss() const {
        return type == MoonrakerErrorType::TIMEOUT || type == MoonrakerErrorType::CONNECTION_LOST;
    }

    /**
     * @brief Get string representation of error type
     * @return Error type as string (e.g., "TIMEOUT", "CONNECTION_LOST")
     */
    std::string get_type_string() const {
        switch (type) {
        case MoonrakerErrorType::NONE:
            return "NONE";
        case MoonrakerErrorType::TIMEOUT:
            return "TIMEOUT";
        case MoonrakerErrorType::CONNECTION_LOST:
            return "CONNECTION_LOST";
        case MoonrakerErrorType::JSON_RPC_ERROR:
            return "JSON_RPC_ERROR";
        case MoonrakerErrorType::PARSE_ERROR:
            return "PARSE_ERROR";
        case MoonrakerErrorType::VALIDATION_ERROR:
            return "VALIDATION_ERROR";
        case MoonrakerErrorType::NOT_READY:
            return "NOT_READY";
        case MoonrakerErrorType::FILE_NOT_FOUND:
            return "FILE_NOT_FOUND";
        case MoonrakerErrorType::PERMISSION_DENIED:
            return "PERMISSION_DENIED";
        case MoonrakerErrorType::UNKNOWN:
            return "UNKNOWN";
        default:
            return "UNKNOWN";
        }
    }

    /**
     * @brief Get user-friendly error message
     * @return Localized error message suitable for display to users
     */
    std::string user_message() const {
        if (type == MoonrakerErrorType::TIMEOUT) {
            return "Request timed out. The printer may be busy.";
        } else if (type == MoonrakerErrorType::CONNECTION_LOST) {
            return "Connection to printer lost.";
        } else if (type == MoonrakerErrorType::NOT_READY) {
            // A populated NOT_READY message is always more specific than the
            // generic fallback, and the fallback is actively misleading for the
            // transient cases: the guards distinguish "busy — try again in a
            // moment" and "homing is disabled while a print is in progress",
            // neither of which is "wait for initialization". Klipper's own
            // not-ready text likewise names the actual fault.
            //
            // Deliberately narrow: TIMEOUT/CONNECTION_LOST above keep their
            // curated strings because their `message` fields hold diagnostic
            // detail ("WebSocket connection lost"), which reads as jargon.
            return message.empty() ? "Printer is not ready. Please wait for initialization."
                                   : message;
        } else if (type == MoonrakerErrorType::FILE_NOT_FOUND) {
            return "File not found on printer.";
        } else if (type == MoonrakerErrorType::PERMISSION_DENIED) {
            return "Permission denied. Check printer configuration.";
        } else if (!message.empty()) {
            return message;
        } else {
            return "An unknown error occurred.";
        }
    }

    /**
     * @brief Extract a human-readable message from Moonraker/Klipper error strings.
     *
     * Klipper errors often arrive as Python dict repr strings like:
     *   {'error': 'WebRequestError', 'message': 'Must home axis first'}
     * This extracts just the 'message' value for user-friendly display.
     */
    static std::string extract_friendly_message(const std::string& raw) {
        // Look for 'message': '...' pattern (Python dict repr with single quotes)
        static constexpr const char* key = "'message': '";
        auto pos = raw.find(key);
        if (pos != std::string::npos) {
            auto start = pos + std::char_traits<char>::length(key);
            auto end = raw.find('\'', start);
            if (end != std::string::npos && end > start) {
                return raw.substr(start, end - start);
            }
        }
        // Also handle "message": "..." (JSON with double quotes)
        static constexpr const char* key2 = "\"message\": \"";
        pos = raw.find(key2);
        if (pos == std::string::npos) {
            static constexpr const char* key3 = "\"message\":\"";
            pos = raw.find(key3);
            if (pos != std::string::npos) {
                auto start = pos + std::char_traits<char>::length(key3);
                auto end = raw.find('"', start);
                if (end != std::string::npos && end > start) {
                    return raw.substr(start, end - start);
                }
            }
        } else {
            auto start = pos + std::char_traits<char>::length(key2);
            auto end = raw.find('"', start);
            if (end != std::string::npos && end > start) {
                return raw.substr(start, end - start);
            }
        }
        // Creality/Klipper key-error envelope uses "msg" instead of "message":
        //   {"code":"keyNNN","msg":"...","values":[...]}
        // Checked last so an explicit "message" is preferred when both exist.
        for (const char* msg_key : {"\"msg\": \"", "\"msg\":\""}) {
            pos = raw.find(msg_key);
            if (pos != std::string::npos) {
                auto start = pos + std::char_traits<char>::length(msg_key);
                auto end = raw.find('"', start);
                if (end != std::string::npos && end > start) {
                    return raw.substr(start, end - start);
                }
            }
        }
        return raw;
    }

    /**
     * @brief Create error from JSON-RPC error response
     * @param error_obj JSON-RPC error object from Moonraker
     * @param method_name Method name that triggered the error
     * @return MoonrakerError with parsed error information
     */
    static MoonrakerError from_json_rpc(const json& error_obj, const std::string& method_name) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::JSON_RPC_ERROR;
        err.method = method_name;

        // Type-check before .get<T>(): a wrong-typed field (a string "code", a
        // null "message") throws nlohmann::type_error, and this runs on the
        // WebSocket thread inside the request tracker's dispatch path. A throw
        // there strands the caller's callback. Treat a wrong-typed field as
        // absent instead.
        const auto code_it = error_obj.find("code");
        if (code_it != error_obj.end() && code_it->is_number()) {
            err.code = code_it->get<int>();
        }

        const auto msg_it = error_obj.find("message");
        if (msg_it != error_obj.end() && msg_it->is_string()) {
            err.message = extract_friendly_message(msg_it->get<std::string>());
        }

        if (error_obj.contains("data")) {
            err.details = error_obj["data"];
        }

        // Map specific error codes to types
        if (err.code == -32601) { // Method not found
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
        } else if (err.message.find("not ready") != std::string::npos) {
            err.type = MoonrakerErrorType::NOT_READY;
        } else if (err.message.find("File not found") != std::string::npos) {
            err.type = MoonrakerErrorType::FILE_NOT_FOUND;
        }

        return err;
    }

    /**
     * @brief Create timeout error
     * @param method_name Method name that timed out
     * @param timeout_ms Timeout duration in milliseconds
     * @return MoonrakerError configured as timeout
     */
    static MoonrakerError timeout(const std::string& method_name, uint32_t timeout_ms) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::TIMEOUT;
        err.method = method_name;
        err.message = "Request timed out after " + helix::format::duration(timeout_ms / 1000);
        return err;
    }

    /**
     * @brief Create connection lost error
     * @param method_name Optional method name when connection was lost
     * @param message Human-readable explanation (defaults to the generic transport message)
     * @return MoonrakerError configured as connection lost
     */
    static MoonrakerError
    connection_lost(const std::string& method_name = "",
                    const std::string& message = "WebSocket connection lost") {
        MoonrakerError err;
        err.type = MoonrakerErrorType::CONNECTION_LOST;
        err.method = method_name;
        err.message = message;
        return err;
    }

    /**
     * @brief Create parse error
     * @param what Description of parse failure
     * @param method_name Optional method name being parsed
     * @return MoonrakerError configured as parse error
     */
    static MoonrakerError parse_error(const std::string& what,
                                      const std::string& method_name = "") {
        MoonrakerError err;
        err.type = MoonrakerErrorType::PARSE_ERROR;
        err.method = method_name;
        err.message = "JSON parse error: " + what;
        return err;
    }

    /**
     * @brief Create a "not ready" error (Klipper halted/busy/homing-blocked)
     * @param method Method name that was refused
     * @param message Human-readable explanation
     * @return MoonrakerError configured as NOT_READY
     */
    static MoonrakerError not_ready(const std::string& method, const std::string& message) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::NOT_READY;
        err.method = method;
        err.message = message;
        return err;
    }

    /**
     * @brief Create a validation error (bad request / rejected parameters)
     * @param method Method name that failed validation
     * @param message Human-readable explanation
     * @return MoonrakerError configured as VALIDATION_ERROR
     */
    static MoonrakerError validation_error(const std::string& method, const std::string& message) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::VALIDATION_ERROR;
        err.method = method;
        err.message = message;
        return err;
    }

    /**
     * @brief Create an UNKNOWN error (uncategorized failure)
     * @param message Human-readable explanation
     * @param method Optional method name that failed
     * @return MoonrakerError configured as UNKNOWN
     */
    static MoonrakerError unknown(const std::string& message, const std::string& method = "") {
        MoonrakerError err;
        err.type = MoonrakerErrorType::UNKNOWN;
        err.method = method;
        err.message = message;
        return err;
    }

    /**
     * @brief Create an UNKNOWN error from a non-2xx HTTP response
     * @param method Method name that issued the request
     * @param status_code HTTP status code returned
     * @return MoonrakerError with code=status_code and message "HTTP <code>"
     */
    static MoonrakerError http_status_error(const std::string& method, int status_code) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::UNKNOWN;
        err.code = status_code;
        err.method = method;
        err.message = "HTTP " + std::to_string(status_code);
        return err;
    }

    /**
     * @brief Create a FILE_NOT_FOUND error
     * @param method Method name that requested the file
     * @param message Human-readable explanation
     * @return MoonrakerError configured as FILE_NOT_FOUND
     */
    static MoonrakerError file_not_found(const std::string& method, const std::string& message) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::FILE_NOT_FOUND;
        err.method = method;
        err.message = message;
        return err;
    }

    /**
     * @brief Create a JSON_RPC_ERROR with an explicit message (not parsed from an error object)
     * @param method Method name that failed
     * @param message Human-readable explanation
     * @param details Optional raw response payload for diagnostics
     * @return MoonrakerError configured as JSON_RPC_ERROR
     */
    static MoonrakerError json_rpc_error(const std::string& method, const std::string& message,
                                         const json& details = json()) {
        MoonrakerError err;
        err.type = MoonrakerErrorType::JSON_RPC_ERROR;
        err.method = method;
        err.message = message;
        err.details = details;
        return err;
    }
};