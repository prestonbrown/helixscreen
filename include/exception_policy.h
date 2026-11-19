// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Desktop builds have exceptions; the ESP32 image does not, and there a throw
// is an abort. Code both of them compile goes through these two helpers
// rather than writing try/catch or throw.

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace helix {

/// Throw @p e where the build has exceptions; log it and abort where it does
/// not. For invariants whose violation is a bug, such as a singleton read
/// before its init: desktop's catch-all nets turn the throw into a log line,
/// and on the firmware there is nothing to recover to.
template <typename E> [[noreturn]] void throw_or_abort(const E& e) {
#if defined(__cpp_exceptions)
    throw e;
#else
    spdlog::critical("Fatal: {}", e.what());
    std::abort();
#endif
}

/// Run @p fn, containing any exception it throws: logged as "<context> threw:
/// <what>" instead of propagating, and reported by returning false. For calls
/// into code the caller cannot vouch for (plugin hooks, registered callbacks),
/// where one bad callee must not take the caller down with it. Without
/// exceptions it is a plain call that returns true. A literal @p context costs
/// no allocation, so a per-message call site can pass one.
template <typename Fn> bool contain_exceptions(std::string_view context, Fn&& fn) {
#if defined(__cpp_exceptions)
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("{} threw: {}", context, e.what());
    } catch (...) {
        spdlog::error("{} threw a non-standard exception", context);
    }
    return false;
#else
    (void)context;
    fn();
    return true;
#endif
}

} // namespace helix
