// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "tour_steps.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace helix::tour {

class TourOverlay; // fwd-declare (defined in tour_overlay.h)

/// Current tour version. Bump when tour content materially changes.
constexpr int TOUR_VERSION = 1;

/// Singleton used by runtime methods (start, advance, skip); gate helpers below are static.
class FirstRunTour {
  public:
    static FirstRunTour& instance();

    /// Gate check: true iff tour should auto-start on home activate.
    static bool should_auto_start();

    /// Writes tour.completed=true and tour.last_seen_version=TOUR_VERSION.
    static void mark_completed();

    /// Auto-trigger entry point — checks gate, schedules start on next LVGL tick.
    void maybe_start();

    /// Replay entry point — bypasses gate, starts immediately.
    void start();

    /// Advance to next step; finishes if last.
    void advance();

    /// Skip button — marks completed and tears down overlay.
    void skip();

    bool is_running() const {
        return running_;
    }

  private:
    FirstRunTour() = default;
    void start_impl();
    void finish();
    void render_current_step();

    bool running_ = false;
    std::size_t current_index_ = 0;
    std::vector<TourStep> steps_;
    std::unique_ptr<TourOverlay> overlay_;
    // Cancels the tour if the user navigates away from Home (e.g. navbar tap).
    // The overlay lives on lv_layer_top() and doesn't block navbar input, so a
    // tour would otherwise be orphaned on top of a different panel with a stale
    // target rect.
    ObserverGuard nav_observer_;
    // Re-resolves the current step's highlight target when the responsive
    // breakpoint changes. Panel rebuilds destroy/recreate widgets, so the
    // cached highlight pointer would otherwise dangle until the user taps Next.
    ObserverGuard breakpoint_observer_;
};

} // namespace helix::tour
