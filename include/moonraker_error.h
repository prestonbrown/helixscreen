// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "error_event.h"
#include "firmware_error_envelope.h"
#include "format_utils.h"
#include "json_fwd.h"
#include "translation_loader.h"

#include <optional>
#include <string>
#include <string_view>

#include "hv/json.hpp"

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
    QUEUE_FULL,        ///< Not sent: the transport's request queue was full; retry later
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
    /// Untranslated key `message` was set from; nullptr when `message` is not ours to translate
    const char* message_tag = nullptr;

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
     * @brief Did a queued G-code RPC give up while Klipper may still run it?
     *
     * printer.gcode.script answers only once Klipper executes the script, which
     * waits behind the G-code queue (a long PRINT_START holds it for minutes). A
     * timeout says nothing about the command; any other error is a failure.
     */
    bool command_may_still_run() const {
        return type == MoonrakerErrorType::TIMEOUT;
    }

    /**
     * @brief Did Moonraker answer that the thing asked for does not exist?
     *
     * Moonraker's JSON-RPC layer turns a ServerError with status 404 into code
     * -32601 and keeps its message: a database key or namespace ("Namespace
     * <ns> not found"), or Spoolman's own 404 through the proxy ("Not Found").
     * The same code with "Method not found" means the method itself is not
     * registered, which says nothing about the thing asked for.
     */
    bool is_not_found() const {
        if (code == 404) {
            return true;
        }
        return code == -32601 && message.rfind("Method not found", 0) != 0;
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
        case MoonrakerErrorType::QUEUE_FULL:
            return "QUEUE_FULL";
        case MoonrakerErrorType::UNKNOWN:
            return "UNKNOWN";
        default:
            return "UNKNOWN";
        }
    }

    /**
     * @brief Untranslated source text for what a user sees
     * @return A translation key (`message_tag`, or a curated string for the
     *         error's type), or nullptr when the text is `message` as received:
     *         Klipper's or Moonraker's own words, which are not ours to translate.
     */
    const char* display_tag() const {
        if (message_tag) {
            return message_tag;
        }
        switch (type) {
        case MoonrakerErrorType::TIMEOUT:
            return TR_NOOP("Request timed out. The printer may be busy.");
        case MoonrakerErrorType::CONNECTION_LOST:
            return TR_NOOP("Connection to printer lost.");
        case MoonrakerErrorType::NOT_READY:
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
            return message.empty()
                       ? TR_NOOP("Printer is not ready. Please wait for initialization.")
                       : nullptr;
        case MoonrakerErrorType::FILE_NOT_FOUND:
            return TR_NOOP("File not found on printer.");
        case MoonrakerErrorType::PERMISSION_DENIED:
            return TR_NOOP("Permission denied. Check printer configuration.");
        default:
            return message.empty() ? TR_NOOP("An unknown error occurred.") : nullptr;
        }
    }

    /**
     * @brief User-facing error text in English. Safe on any thread.
     *
     * For what the user sees, prefer localized_message() on the main thread.
     */
    std::string user_message() const {
        const char* tag = display_tag();
        return tag ? std::string(tag) : message;
    }

    /**
     * @brief user_message() in the active language. Safe from any thread.
     */
    std::string localized_message() const;

    /**
     * @brief Extract a human-readable message from Moonraker/Klipper error strings.
     *
     * Klipper errors often arrive as Python dict repr strings like:
     *   {'error': 'WebRequestError', 'message': 'Must home axis first'}
     * This extracts just the 'message' value for user-friendly display, then
     * replaces any firmware JSON error envelope in it with the envelope's
     * message (firmware_error_envelope.h).
     */
    static std::string extract_friendly_message(const std::string& raw) {
        return helix::decode_firmware_error_text(extract_message_field(raw));
    }

    /// extract_friendly_message() of an error line that may still carry its
    /// `!!` / `Error:` prefixes (`!! Error: foo`), with them and the
    /// whitespace after each removed.
    static std::string friendly_error_line(const std::string& raw) {
        std::string text = raw;
        for (auto parsed = helix::parse_gcode_error_line(text);
             parsed.prefix != helix::GcodeErrorPrefix::None;
             parsed = helix::parse_gcode_error_line(text)) {
            text = std::move(parsed.text);
            const auto start = text.find_first_not_of(" \t");
            text.erase(0, start == std::string::npos ? text.size() : start);
        }
        return extract_friendly_message(text);
    }

    /// The 'message' value of a Python-repr or JSON error dict, or `raw`.
    static std::string extract_message_field(const std::string& raw) {
        // Python dict repr with single quotes, then JSON with and without a space.
        for (const char* key : {"'message': '", "\"message\": \"", "\"message\":\""}) {
            if (auto value = quoted_value_after(raw, key)) {
                return std::move(*value);
            }
        }
        return raw;
    }

    /// The non-empty quoted value right after `key` in `raw`, which ends with
    /// the opening quote. A backslash escapes the next character, so an
    /// escaped quote or backslash is kept as the character itself and any
    /// other escape is kept verbatim. nullopt when `key` is absent or the
    /// value is empty or unterminated.
    static std::optional<std::string> quoted_value_after(const std::string& raw, const char* key) {
        const auto pos = raw.find(key);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        const std::string_view k(key);
        const char quote = k.back();
        std::string value;
        for (size_t i = pos + k.size(); i < raw.size(); ++i) {
            const char c = raw[i];
            if (c == quote) {
                if (value.empty()) {
                    return std::nullopt;
                }
                return value;
            }
            if (c == '\\' && i + 1 < raw.size()) {
                const char next = raw[++i];
                if (next != quote && next != '\\') {
                    value += c;
                }
                value += next;
                continue;
            }
            value += c;
        }
        return std::nullopt;
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
     * @brief A refusal HelixScreen issued itself, with translatable text
     * @param method Method that was refused
     * @param tag    Untranslated, static-lifetime text (wrap the literal in TR_NOOP)
     * @return NOT_READY error whose localized_message() translates @p tag
     */
    static MoonrakerError refusal(const std::string& method, const char* tag) {
        MoonrakerError err = not_ready(method, tag);
        err.message_tag = tag;
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
        // Callers hand over raw `!!` lines, which can carry a firmware envelope;
        // decoded the way from_json_rpc() decodes Moonraker's message.
        err.message = extract_friendly_message(message);
        err.details = details;
        return err;
    }
};

/// True when a server.files.get_directory failure means the directory is not there.
/// Moonraker reports that as a plain 400 with this text, not as a not-found code.
// NAMESPACE_OK: sits beside the global MoonrakerError it reads
inline bool moonraker_directory_missing(const MoonrakerError& error) {
    return error.type == MoonrakerErrorType::FILE_NOT_FOUND ||
           error.message.find("Directory does not exist") != std::string::npos;
}
