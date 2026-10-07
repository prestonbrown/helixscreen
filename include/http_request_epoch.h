// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_error.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

/// The printer REST requests are addressed to. MoonrakerAPI advances the epoch when its HTTP
/// base URL moves to another host; a reply that completes under a newer epoch came from the
/// previous printer and must not reach its caller as the current printer's data.
namespace helix::http_epoch {

inline std::atomic<uint64_t>& counter() {
    static std::atomic<uint64_t> epoch{0};
    return epoch;
}

inline uint64_t current() {
    return counter().load();
}

inline void advance() {
    counter().fetch_add(1);
}

/// Identifies the printer REST requests go to, for caches whose entries belong to one printer.
inline std::atomic<size_t>& printer_key_storage() {
    static std::atomic<size_t> key{0};
    return key;
}

inline size_t printer_key() {
    return printer_key_storage().load();
}

/// Records that REST now goes to @p base_url, after the owner has stored it, so a request
/// that reads the new epoch also reads the new URL. @p moved starts a new epoch.
inline void set_base_url(const std::string& base_url, bool moved) {
    printer_key_storage().store(std::hash<std::string>{}(base_url));
    if (moved) {
        advance();
    }
}

/// Wraps a REST request's success callback, taken when the request starts: a reply that
/// arrives after the epoch moved goes to @p on_error as CONNECTION_LOST instead. Safe to call
/// from the worker thread that delivers the reply.
template <typename Success>
auto guard_reply(Success on_success, std::function<void(const MoonrakerError&)> on_error,
                 std::string method) {
    const uint64_t started = current();
    return [on_success = std::move(on_success), on_error = std::move(on_error),
            method = std::move(method), started](auto&&... args) {
        if (current() != started) {
            if (on_error) {
                on_error(MoonrakerError::connection_lost(method, "the printer was switched"));
            }
            return;
        }
        if (on_success) {
            on_success(std::forward<decltype(args)>(args)...);
        }
    };
}

} // namespace helix::http_epoch
