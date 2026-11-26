// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

namespace helix {

/**
 * @brief A display sleep and wake handler that lives as long as its owner
 *
 * The app reports every sleep and wake edge through notify() (wired once beside the display
 * in application_display.cpp), so a UI object follows the display without reaching up into
 * DisplayManager. A listener is called from listen() until it is destroyed; a later listen()
 * replaces the handler. Main thread only.
 */
class DisplaySleepListener {
  public:
    DisplaySleepListener() = default;
    ~DisplaySleepListener();
    DisplaySleepListener(const DisplaySleepListener&) = delete;
    DisplaySleepListener& operator=(const DisplaySleepListener&) = delete;

    /// Calls `on_change` with true when the display goes to sleep and false when it wakes.
    void listen(std::function<void(bool sleeping)> on_change);

    /// True while the display sleeps, as the last notify() said; false before any.
    ///
    /// Deliberately the last notify(), not the display's own flag: DisplaySleepController
    /// flips its state and runs the panel lifecycle (an on_activate() on wake) before it runs
    /// the sleep callbacks, so during that window this still reads the old edge. Every listener
    /// gets notify() right after, so an object that reads this from on_activate() and also
    /// listens settles on the new state within the same wake. Reading the controller directly
    /// would make the UI layer depend on the app's.
    static bool display_sleeping();

    /// The display went to sleep (true) or woke (false): every live listener hears it.
    static void notify(bool sleeping);

  private:
    std::function<void(bool)> on_change_;
    bool registered_ = false;
};

} // namespace helix
