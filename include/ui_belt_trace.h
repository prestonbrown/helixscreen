// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"

namespace helix {
namespace ui {

/// Live waveform / spectrum strip for the belt tuner LISTEN screen. Same
/// custom-widget idiom as HelixSparkline (see helix_sparkline.h/.cpp): a
/// plain lv_obj plus a LV_EVENT_DRAW_MAIN_END draw callback, refreshed by a
/// tick subject rather than owning a timer. One widget class serves both
/// traces - Mode picks which of BeltLiveData's two vectors it reads.
///
/// Deliberately not ui_frequency_response_chart: that widget is an lv_chart
/// built for comparing two captured curves, with series, peak marking and a
/// table-mode fallback on hardware that cannot afford the chart. This is a
/// 10 Hz redraw of a single strip beside a time-domain trace, and about a
/// tenth of the code.
///
/// Usage (XML):
///   <belt_trace mode="waveform" style_line_color="#primary"/>
///   <belt_trace mode="spectrum" style_line_color="#success"/>
class BeltTrace {
  public:
    enum class Mode {
        WAVEFORM, ///< Time-domain ring-down, mirrored envelope about the midline
        SPECTRUM, ///< Frequency-domain bars from the baseline
    };

    /// Create a trace strip. `mode` picks BeltLiveData::waveform() or
    /// ::spectrum() as the data source. Both are read fresh on every draw, so
    /// there is nothing to push in - only invalidation (driven by
    /// bt_live_tick) is needed. Returns the LVGL object (caller/XML may set
    /// size and style on it).
    static lv_obj_t* create(lv_obj_t* parent, Mode mode);

  private:
    explicit BeltTrace(Mode mode);
    ~BeltTrace() = default;
    BeltTrace(const BeltTrace&) = delete;
    BeltTrace& operator=(const BeltTrace&) = delete;

    static void on_draw(lv_event_t* e);
    static void on_delete(lv_event_t* e);
    void invalidate_self();

    lv_obj_t* obj_ = nullptr;
    Mode mode_;
    ObserverGuard tick_observer_; // dtor calls reset() automatically (L085)
};

/// Register the belt_trace custom widget with the helix-xml engine. Must run
/// before any register_xml() call that loads panel_belt_tension.xml, or the
/// tag resolves to nothing and the panel silently loses both traces.
void register_belt_trace_widget();

} // namespace ui
} // namespace helix
