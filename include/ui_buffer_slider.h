// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"

#include "buffer_reading.h"
#include "clog_meter_geometry.h"
#include "lvgl/lvgl.h"

namespace helix::ui {

/**
 * @brief Draws a filament buffer reading as an upright gauge, and optionally
 *        its last minute as a trace beside it.
 *
 * A Bias reading is a housing on the filament strand with a block riding it:
 * loose is up, tight is down. A dashed window marks the target and faint zones
 * mark both end stops; only the block moves, in buffer_status_token() of its
 * severity. A Fill reading (compression only) is a housing filled from the
 * bottom up to the pressure, with a tick at the set point; the trace beside it
 * plots the same 0..100 axis with the set point dashed. The trace draws inside
 * its own object, which XML gives the panel background. The layouts are
 * buffer_slider_geometry() and buffer_fill_geometry(); this class only paints
 * them into objects XML authored and sized.
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

    /// Draw a Bias reading.
    void set_reading(float bias, ClogMeterStatus status);

    /// Draw @p reading as its own gauge.
    void set_reading(const BufferReading& reading);

    /// Draw AmsState's system-level reading (the buffer_* subjects), following
    /// it as it changes.
    void follow_system_reading();

    [[nodiscard]] BufferGauge gauge() const {
        return gauge_;
    }
    [[nodiscard]] int value_pct() const {
        return value_pct_;
    }
    [[nodiscard]] int target_pct() const {
        return target_pct_;
    }
    /// Whether the slider object has been painted, and the gauge its last
    /// paint drew.
    [[nodiscard]] bool has_painted() const {
        return painted_;
    }
    [[nodiscard]] BufferGauge painted_gauge() const {
        return painted_gauge_;
    }
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

    void invalidate();
    void draw_slider(lv_layer_t* layer) const;
    void draw_fill_gauge(lv_layer_t* layer, const lv_area_t& content) const;
    void draw_trace(lv_layer_t* layer) const;

    lv_obj_t* slider_obj_ = nullptr;
    lv_obj_t* trace_obj_ = nullptr;
    int trace_unit_ = -1;
    LvglTimerGuard trace_timer_;
    ObserverGuard bias_observer_;
    ObserverGuard status_observer_;
    ObserverGuard gauge_observer_;
    ObserverGuard value_observer_;
    ObserverGuard target_observer_;
    int trace_ticks_ = 0;

    BufferGauge gauge_ = BufferGauge::Bias;
    int value_pct_ = 0;
    int target_pct_ = -1;
    mutable bool painted_ = false;
    mutable BufferGauge painted_gauge_ = BufferGauge::Bias; ///< set by the painter that ran
    float bias_ = 0.0f;
    ClogMeterStatus status_ = ClogMeterStatus::Ok;
};

} // namespace helix::ui
