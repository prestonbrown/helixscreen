// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/observer_guard.h"
#include "lvgl/lvgl.h"

#include <string>

namespace helix {
namespace ui {

/// Lightweight sparkline widget that renders a PerformanceState ring buffer
/// as a line graph. Observes "perf_history_tick" and redraws on each tick.
///
/// Usage (C++):
///   auto* obj = HelixSparkline::create(parent, "host_cpu_pct");
///
/// Usage (XML):
///   <helix_sparkline source="host_cpu_pct" style_line_color="#accent" />
class HelixSparkline {
  public:
    /// Create a sparkline bound to a PerformanceState ring buffer.
    /// `source` is the ring-buffer name (e.g. "host_cpu_pct").
    /// Returns the LVGL object (caller may set size/style on it).
    static lv_obj_t* create(lv_obj_t* parent, const std::string& source);

  private:
    explicit HelixSparkline(const std::string& source);
    ~HelixSparkline() = default;
    HelixSparkline(const HelixSparkline&) = delete;
    HelixSparkline& operator=(const HelixSparkline&) = delete;

    static void on_draw(lv_event_t* e);
    static void on_delete(lv_event_t* e);
    void invalidate_self();

    lv_obj_t* obj_ = nullptr;
    std::string source_;
    ObserverGuard tick_observer_; // dtor calls reset() automatically (L085)
};

/// Register the helix_sparkline custom widget with the helix-xml engine.
/// Called once from Application::register_widgets().
void register_helix_sparkline_widget();

} // namespace ui
} // namespace helix
