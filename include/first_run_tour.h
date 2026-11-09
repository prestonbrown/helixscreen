// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_guard.h"
#include "core/timer_guard.h"
#include "tour_steps.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace helix::tour {

class TourOverlay; // fwd-declare (defined in tour_overlay.h)

/// Current tour version. Bump when tour content materially changes.
constexpr int kTourVersion = 2;

/// Singleton used by runtime methods (start, advance, skip); gate helpers below are static.
class FirstRunTour {
  public:
    static FirstRunTour& instance();

    /// Gate check: true iff tour should auto-start on home activate.
    static bool should_auto_start();

    /// Writes tour.completed=true and tour.last_seen_version=kTourVersion.
    static void mark_completed();

    /// Auto-trigger entry point — checks gate, schedules start on next LVGL tick.
    void maybe_start();

    /// Discovery finished, so which filament system the printer has is known and
    /// the tour can be built. Offers the tour.
    void on_printer_ready();

    /// Whether a printer has been discovered this session.
    void set_printer_ready(bool ready) {
        printer_ready_ = ready;
    }
    bool is_printer_ready() const {
        return printer_ready_;
    }

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
    void start_impl(int seen_version);
    void finish();
    void end_tour();
    void render_current_step();
    void show_panel(TourPanel panel);
    void arm_panel_timer(lv_timer_cb_t callback);
    static void on_panel_settled(lv_timer_t* timer);
    static void on_ams_closed(lv_timer_t* timer);

    bool running_ = false;
    bool printer_ready_ = false;
    // maybe_start() defers start_impl() by a tick, and start_impl() is what sets
    // running_. Both entry points (HomePanel::on_activate and wizard completion)
    // can fire in the same tick, so running_ alone would let both queue a start.
    bool start_queued_ = false;
    std::size_t current_index_ = 0;
    TourPanel shown_panel_ = TourPanel::Home;
    // A panel the tour switches to needs its slide-in and layout done before
    // the target's coordinates mean anything.
    ui::LvglTimerGuard panel_settle_timer_;
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
