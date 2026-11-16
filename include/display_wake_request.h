// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <utility>

namespace helix {

/// The function request_display_wake() calls. The display owner installs it; a build
/// with no display sleep (the firmware) leaves it empty.
inline std::function<void()>& display_wake_handler() {
    static std::function<void()> handler;
    return handler;
}

inline void set_display_wake_handler(std::function<void()> handler) {
    display_wake_handler() = std::move(handler);
}

/// Wakes a sleeping or dimmed display for something the user has to answer. Does
/// nothing while the display is awake or when no handler is installed. Main thread only.
inline void request_display_wake() {
    if (const auto& handler = display_wake_handler()) {
        handler();
    }
}

} // namespace helix
