// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "translation_loader.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Event types emitted by MoonrakerClient
 *
 * These events replace direct UI notification calls, allowing the transport
 * layer to remain decoupled from the UI layer.
 */
enum class MoonrakerEventType {
    CONNECTION_FAILED,   ///< Reconnect stalled past the limit, or the socket never opened
    CONNECTION_LOST,     ///< WebSocket connection closed unexpectedly
    RECONNECTING,        ///< Attempting to reconnect
    RECONNECTED,         ///< Successfully reconnected after disconnect
    MESSAGE_OVERSIZED,   ///< Received message exceeds size limit
    RPC_ERROR,           ///< JSON-RPC request failed
    KLIPPY_DISCONNECTED, ///< Klipper firmware disconnected from Moonraker
    KLIPPY_SHUTDOWN,     ///< Klipper firmware entered shutdown state (M112, thermal, error)
    KLIPPY_READY,        ///< Klipper firmware ready
    DISCOVERY_FAILED,    ///< Printer discovery failed (non-retryable — RPC/server error)
    DISCOVERY_DEFERRED,  ///< Discovery deferred waiting on Klippy (auto-retries on state change)
    REQUEST_TIMEOUT,     ///< JSON-RPC request timed out
    LOGIN_REFUSED        ///< Moonraker requires a login this client's credential does not satisfy
};

namespace helix {

/// @p tmpl with each `{}` replaced by the next of @p args. Never fails: a translation with
/// fewer placeholders drops the surplus args, and any other brace is copied as text, so a
/// translated template is safe where fmt would throw (or abort, without exceptions).
inline std::string fill_placeholders(const char* tmpl, const std::vector<std::string>& args) {
    std::string out;
    size_t next = 0;
    for (const char* p = tmpl; *p; ++p) {
        if (p[0] == '{' && p[1] == '}' && next < args.size()) {
            out += args[next++];
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
}

} // namespace helix

/**
 * @brief Event structure passed to event handlers
 */
struct MoonrakerEvent {
    MoonrakerEventType type;
    std::string message; ///< Human-readable message, English
    std::string details; ///< Additional details (optional)
    bool is_error;       ///< true for errors, false for warnings/info

    /// Untranslated template `message` was rendered from, each `{}` taking the
    /// next of `message_args`. nullptr when `message` is not ours to translate
    /// (Klipper's or Moonraker's own words).
    const char* message_tag = nullptr;
    std::vector<std::string> message_args;

    /// @p tmpl filled from `message_args`. The presenter passes lv_tr(message_tag)
    /// when it shows the event.
    std::string render(const char* tmpl) const {
        return helix::fill_placeholders(tmpl, message_args);
    }

    /// An event whose text the UI translates; `message` holds the English rendering.
    static MoonrakerEvent translatable(MoonrakerEventType type, const char* tag,
                                       std::vector<std::string> args, bool is_error,
                                       std::string details = {}) {
        MoonrakerEvent evt{type, {}, std::move(details), is_error, tag, std::move(args)};
        evt.message = evt.render(tag);
        return evt;
    }
};

/// Events every Moonraker client emits for the same condition, so the desktop and
/// ESP32 clients present identical, translatable text.
namespace helix::moonraker_event {

inline MoonrakerEvent reconnected() {
    return MoonrakerEvent::translatable(MoonrakerEventType::RECONNECTED,
                                        TR_NOOP("Connection restored"), {}, false);
}

inline MoonrakerEvent connection_lost_reconnecting() {
    return MoonrakerEvent::translatable(
        MoonrakerEventType::CONNECTION_LOST,
        TR_NOOP("Connection to printer lost - attempting to reconnect..."), {}, false);
}

/// Reconnection has been stalled past the client's limit.
inline MoonrakerEvent reconnect_stalled() {
    return MoonrakerEvent::translatable(
        MoonrakerEventType::CONNECTION_FAILED,
        TR_NOOP("Unable to reach printer. Check power and network connection."), {}, true);
}

inline MoonrakerEvent rpc_failed(const std::string& method, const std::string& error) {
    return MoonrakerEvent::translatable(MoonrakerEventType::RPC_ERROR,
                                        TR_NOOP("Printer command '{}' failed: {}"), {method, error},
                                        true, method);
}

/// Moonraker refused this client's login at connect; reported once per credential.
inline MoonrakerEvent login_refused() {
    return MoonrakerEvent::translatable(MoonrakerEventType::LOGIN_REFUSED,
                                        TR_NOOP("Moonraker refused the login"), {}, true);
}

inline MoonrakerEvent request_timed_out(const std::string& method, uint32_t timeout_ms) {
    return MoonrakerEvent::translatable(MoonrakerEventType::REQUEST_TIMEOUT,
                                        TR_NOOP("Printer command '{}' timed out after {}ms"),
                                        {method, std::to_string(timeout_ms)}, false, method);
}

/// A warning, not an error: discovery still completes without the subscription.
inline MoonrakerEvent subscribe_failed(const std::string& error) {
    return MoonrakerEvent::translatable(MoonrakerEventType::DISCOVERY_FAILED,
                                        TR_NOOP("Failed to subscribe to printer updates: {}"),
                                        {error}, false);
}

} // namespace helix::moonraker_event

namespace helix {
/**
 * @brief Callback type for event handlers
 */
using MoonrakerEventCallback = std::function<void(const MoonrakerEvent&)>;
} // namespace helix
