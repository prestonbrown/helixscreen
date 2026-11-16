// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix::nav {

// Cost of one panel switch, by phase, logged as one line. The switch blocks the
// UI thread, so every millisecond here is a frame the user waits for. Taps on a
// slow display are dominated by paints and layout, so the line also carries the
// layout and render time of every refresh inside the switch, and of the first
// frame rendered after it: queued work (the active_panel observer that un-hides
// the panel, deferred observer fires) lands in that frame, and it is the one
// that puts the new panel on screen when the switch did not paint it.
//
// Only the outermost switch traces; switch_to_panel_impl cascading into
// set_active charges its phases to the outer line.
class SwitchTrace {
  public:
    explicit SwitchTrace(int panel_id);
    ~SwitchTrace();
    SwitchTrace(const SwitchTrace&) = delete;
    SwitchTrace& operator=(const SwitchTrace&) = delete;

    // Charges the time since the previous mark to `phase`.
    static void mark(const char* phase);

  private:
    bool owns_;
};

} // namespace helix::nav
