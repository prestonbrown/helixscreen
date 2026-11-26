// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>

namespace helix {

/**
 * @brief A display sleep and wake handler that lives as long as its owner
 *
 * DisplayManager keeps every sleep callback for the life of the process, so the registration
 * holds a weak handle that dies with this listener, never the owner. listen() registers once; a
 * later call replaces the handler. Without a DisplayManager (tests, a build with none) nothing
 * is registered. Main thread only.
 */
class DisplaySleepListener {
  public:
    /// Calls `on_change` with true when the display goes to sleep and false when it wakes.
    void listen(std::function<void(bool sleeping)> on_change);

    /// True while the display sleeps; false without a DisplayManager.
    static bool display_sleeping();

  private:
    std::shared_ptr<std::function<void(bool)>> handler_;
};

} // namespace helix
