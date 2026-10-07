// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"

#include "clog_meter_geometry.h"
#include "lvgl/lvgl.h"

namespace helix::ui {

/**
 * @brief Draws a filament buffer reading as an upright slider, and optionally
 *        its last minute as a trace beside it.
 *
 * A housing on the filament strand with a block riding it: loose is up, tight
 * is down. A dashed window marks the target and faint zones mark both end
 * stops; only the block moves, in buffer_status_token() of its severity. The
 * trace draws inside its own object, which XML gives the panel background.
 * The layout is buffer_slider_geometry(); this class only paints it into
 * objects XML authored and sized.
 */
class UiBufferSlider {
  public:
    /// @p slider_obj draws the slider. @p trace_obj, when given, draws
    /// AmsState::buffer_trace(@p trace_unit) and is redrawn once a second, so
    /// the trace scrolls with no new reading.
    explicit UiBufferSlider(lv_obj_t* slider_obj, lv_obj_t* trace_obj = nullptr,
                            int trace_unit = -1);
    ~UiBufferSlider();

    UiBufferSlider(const UiBufferSlider&) = delete;
    UiBufferSlider& operator=(const UiBufferSlider&) = delete;

    void set_reading(float bias, ClogMeterStatus status);

    /// Draw AmsState's system-level reading (buffer_bias_pct, buffer_status),
    /// following it as it changes.
    void follow_system_reading();

    [[nodiscard]] float bias() const {
        return bias_;
    }
    [[nodiscard]] ClogMeterStatus status() const {
        return status_;
    }
    /// How many times the once-a-second timer has redrawn the trace.
    [[nodiscard]] int trace_ticks() const {
        return trace_ticks_;
    }
    /// The trace timer is armed: true only while the trace object is alive.
    [[nodiscard]] bool has_trace_timer() const {
        return static_cast<bool>(trace_timer_);
    }

    /// Test seam: the harness only runs timers with a finite repeat count.
    [[nodiscard]] lv_timer_t* timer_for_test() const {
        return trace_timer_.get();
    }

  private:
    static void on_draw(lv_event_t* e);
    static void on_deleted(lv_event_t* e);
    static void on_trace_timer(lv_timer_t* timer);

    void draw_slider(lv_layer_t* layer) const;
    void draw_trace(lv_layer_t* layer) const;

    lv_obj_t* slider_obj_ = nullptr;
    lv_obj_t* trace_obj_ = nullptr;
    int trace_unit_ = -1;
    LvglTimerGuard trace_timer_;
    ObserverGuard bias_observer_;
    ObserverGuard status_observer_;
    int trace_ticks_ = 0;

    float bias_ = 0.0f;
    ClogMeterStatus status_ = ClogMeterStatus::Ok;
};

} // namespace helix::ui
