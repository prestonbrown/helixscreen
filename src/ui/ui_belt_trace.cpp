// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_belt_trace.h"

#include "ui_panel_belt_tension.h"

#include "belt_live_data.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "observer_factory.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace helix {
namespace ui {

// ---- BeltTrace implementation ----

lv_obj_t* BeltTrace::create(lv_obj_t* parent, Mode mode) {
    auto* impl = new BeltTrace(mode);
    impl->obj_ = lv_obj_create(parent);
    lv_obj_set_user_data(impl->obj_, impl);
    lv_obj_remove_style_all(impl->obj_);
    lv_obj_set_size(impl->obj_, 80, 40);
    lv_obj_clear_flag(impl->obj_, LV_OBJ_FLAG_SCROLLABLE);

    // Same L079 reasoning as HelixSparkline: DRAW_MAIN_END is the correct
    // event for overlaying custom drawing on top of the base style pass.
    lv_obj_add_event_cb(impl->obj_, on_draw, LV_EVENT_DRAW_MAIN_END, impl);
    lv_obj_add_event_cb(impl->obj_, on_delete, LV_EVENT_DELETE, impl);

    // bt_live_tick is owned by BeltTensionPanel, which tears down and
    // rebuilds its subjects across activations (deinit_subjects() /
    // init_subjects()). Passing its subject lifetime means this observer
    // releases safely when that happens instead of calling
    // lv_observer_remove() on a freed subject (#705) - same pattern
    // HelixSparkline uses for PerformanceState's subjects.
    lv_subject_t* tick = lv_xml_get_subject(nullptr, "bt_live_tick");
    if (tick) {
        impl->tick_observer_ = helix::ui::observe_int_sync<BeltTrace>(
            tick, impl, [](BeltTrace* self, int /*value*/) { self->invalidate_self(); },
            get_global_belt_tension_panel().get_subjects_lifetime());
    } else {
        spdlog::debug("[BeltTrace] bt_live_tick subject not found — trace will not auto-refresh");
    }

    return impl->obj_;
}

BeltTrace::BeltTrace(Mode mode) : mode_(mode) {}

void BeltTrace::invalidate_self() {
    if (obj_) {
        lv_obj_invalidate(obj_);
    }
}

void BeltTrace::on_draw(lv_event_t* e) {
    // user_data is the per-callback value passed to lv_obj_add_event_cb (L069).
    auto* self = static_cast<BeltTrace*>(lv_event_get_user_data(e));
    if (!self || !self->obj_)
        return;

    const auto& data = self->mode_ == Mode::WAVEFORM
                           ? helix::calibration::BeltLiveData::instance().waveform()
                           : helix::calibration::BeltLiveData::instance().spectrum();
    if (data.size() < 2)
        return;

    lv_area_t area;
    lv_obj_get_coords(self->obj_, &area);
    const int x0 = area.x1;
    const int y0 = area.y1;
    const int w = lv_area_get_width(&area);
    const int h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0)
        return;

    // Both traces hold non-negative magnitudes (waveform is a DC-free
    // envelope, spectrum is PSD power), so there is a single scale to find:
    // the largest value, with a floor so a silent stream draws a flat line
    // or empty baseline rather than dividing by zero. peak_index (unused by
    // WAVEFORM) locates that same bar for the spectrum's frequency label.
    float max_v = 0.0f;
    size_t peak_index = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] > max_v) {
            max_v = data[i];
            peak_index = i;
        }
    }
    if (max_v < 1e-3f) {
        max_v = 1.0f;
    }

    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer)
        return;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    // Read from the obj's style so XML callers can set style_line_color and
    // bind_style can swap it per trace.
    dsc.color = lv_obj_get_style_line_color(self->obj_, LV_PART_MAIN);
    dsc.width = 1;
    dsc.opa = LV_OPA_COVER;

    const size_t n = data.size();

    if (self->mode_ == Mode::WAVEFORM) {
        // Mirrored top/bottom envelope about the vertical midline - the usual
        // way to render a magnitude-only signal as a waveform (there is no
        // sign left in the data; BeltLiveData::set_waveform() already reduced
        // each bucket to a peak magnitude).
        const float mid = static_cast<float>(y0) + static_cast<float>(h) / 2.0f;
        const float half_h = static_cast<float>(h) / 2.0f;
        for (size_t i = 1; i < n; ++i) {
            const float t0 = static_cast<float>(i - 1) / static_cast<float>(n - 1);
            const float t1 = static_cast<float>(i) / static_cast<float>(n - 1);
            const float n0 = data[i - 1] / max_v;
            const float n1 = data[i] / max_v;
            const float x0f = static_cast<float>(x0) + t0 * static_cast<float>(w - 1);
            const float x1f = static_cast<float>(x0) + t1 * static_cast<float>(w - 1);

            dsc.p1.x = static_cast<lv_value_precise_t>(x0f);
            dsc.p2.x = static_cast<lv_value_precise_t>(x1f);

            dsc.p1.y = static_cast<lv_value_precise_t>(mid - n0 * half_h);
            dsc.p2.y = static_cast<lv_value_precise_t>(mid - n1 * half_h);
            lv_draw_line(layer, &dsc);

            dsc.p1.y = static_cast<lv_value_precise_t>(mid + n0 * half_h);
            dsc.p2.y = static_cast<lv_value_precise_t>(mid + n1 * half_h);
            lv_draw_line(layer, &dsc);
        }
    } else {
        // Vertical bars from the baseline. Peak-reduced buckets (see
        // BeltLiveData::set_spectrum()), so a narrow spike stays visible
        // instead of being averaged into the noise floor around it.
        //
        // Bars are scaled to a height short of the strip's top, reserving
        // room for the peak label drawn below. Without this, the tallest
        // bar's norm is exactly 1.0 by construction (it IS max_v), so its top
        // always lands exactly on y0 and the label's "is there room above
        // it" check below could never pass - confirmed by replaying a real
        // capture (see docs/devel/ENVIRONMENT_VARIABLES.md's
        // HELIX_BELT_CAPTURE_DIR section) into a live panel and screenshotting
        // it: the label was silently dropped on every single spectrum.
        const lv_font_t* peak_font = theme_manager_get_font("font_xs");
        const int32_t label_h = theme_manager_get_font_height(peak_font);
        constexpr int32_t label_gap = 2;
        const float bar_scale_h =
            std::max(1.0f, static_cast<float>(h) - 1.0f - static_cast<float>(label_h + label_gap));

        const float baseline_y = static_cast<float>(y0 + h - 1);
        float peak_bar_top = baseline_y;
        float peak_x = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float norm = data[i] / max_v;
            const float x = static_cast<float>(x0) + t * static_cast<float>(w - 1);
            const float bar_top = baseline_y - norm * bar_scale_h;

            dsc.p1.x = static_cast<lv_value_precise_t>(x);
            dsc.p2.x = static_cast<lv_value_precise_t>(x);
            dsc.p1.y = static_cast<lv_value_precise_t>(baseline_y);
            dsc.p2.y = static_cast<lv_value_precise_t>(bar_top);
            lv_draw_line(layer, &dsc);

            if (i == peak_index) {
                peak_bar_top = bar_top;
                peak_x = x;
            }
        }

        // Mark the estimator's fundamental before labelling the peak, so the
        // tick sits under the bars rather than over the label. The two are
        // different numbers by design: the estimator resolves f0 by harmonic
        // structure, the peak is a bare argmax, and on a real belt the argmax
        // lands on 2*f0 whenever the second harmonic dominates
        // (pitch_estimator.h:14-18). Drawing both is how that is visible.
        const auto& live = helix::calibration::BeltLiveData::instance();
        const float estimate_hz = live.spectrum_estimate_hz();
        const auto estimate_fraction = live.spectrum_fraction_for_hz(estimate_hz);
        if (estimate_fraction.has_value()) {
            lv_draw_line_dsc_t tick;
            lv_draw_line_dsc_init(&tick);
            tick.color = theme_manager_get_color("primary");
            tick.width = 1;
            tick.opa = LV_OPA_COVER;
            const float tick_x =
                static_cast<float>(x0) + *estimate_fraction * static_cast<float>(w - 1);
            tick.p1.x = static_cast<lv_value_precise_t>(tick_x);
            tick.p2.x = static_cast<lv_value_precise_t>(tick_x);
            tick.p1.y = static_cast<lv_value_precise_t>(baseline_y);
            tick.p2.y = static_cast<lv_value_precise_t>(baseline_y - bar_scale_h);
            lv_draw_line(layer, &tick);
        }

        // The peak-frequency label. This is the point of the widget (see the
        // class comment): the reference machine had belt A and belt B
        // landing on the same spectral peak while the panel reported two
        // different committed numbers, and a labelled peak would have shown
        // that at a glance. Reads BeltLiveData's tracked bin frequency rather
        // than deriving one from `data`'s bucket index - the reduction above
        // keeps the peak's power but not reliably its original bin.
        const float peak_hz = live.spectrum_peak_hz();
        if (peak_hz > 0.0f) {
            // static: lv_draw_label_dsc_t::text is a pointer LVGL keeps alive
            // into a deferred draw task, so a stack buffer would be a
            // use-after-free once this function returns (see
            // temp_graph_tooltip.cpp for the same trap).
            //
            // INVARIANT: at most one belt_trace may be in SPECTRUM mode at a
            // time - panel_belt_tension.xml instantiates exactly one
            // (bt_spectrum). A second one would have both draws racing for
            // this buffer within a single frame. Give each instance its own
            // buffer before adding one.
            static char peak_label[24];
            // "peak", not a bare frequency: this is the naive argmax, NOT the
            // number the panel reports, and beside a live readout an unlabelled
            // second frequency reads as a competing answer. Two numbers an
            // octave apart with no way to tell which is which is worse than no
            // label at all.
            snprintf(peak_label, sizeof(peak_label), "peak %.0f Hz", static_cast<double>(peak_hz));

            lv_draw_label_dsc_t label_dsc;
            lv_draw_label_dsc_init(&label_dsc);
            label_dsc.color = dsc.color;
            label_dsc.font = peak_font;
            label_dsc.align = LV_TEXT_ALIGN_CENTER;
            label_dsc.text = peak_label;

            constexpr int32_t label_w = 72;
            lv_area_t label_area;
            label_area.y2 = static_cast<lv_coord_t>(peak_bar_top) - label_gap;
            label_area.y1 = label_area.y2 - label_h;
            label_area.x1 = static_cast<lv_coord_t>(peak_x) - label_w / 2;
            label_area.x2 = label_area.x1 + label_w;

            // Clamp horizontally into the trace's own bounds. Done as a single
            // clamp of x1 rather than two independent shifts: shifting right
            // off the left edge and then left off the right edge can push x1
            // back out again, so sequential corrections do not compose. When
            // the label is wider than the strip there is no position that
            // fits at all, so it is left flush with the left edge and
            // overflows to the right, which is the readable half.
            const lv_coord_t max_x1 = static_cast<lv_coord_t>(x0 + w) - label_w;
            if (label_area.x1 > max_x1) {
                label_area.x1 = max_x1;
            }
            if (label_area.x1 < static_cast<lv_coord_t>(x0)) {
                label_area.x1 = static_cast<lv_coord_t>(x0);
            }
            label_area.x2 = label_area.x1 + label_w;
            if (label_area.y1 >= y0) {
                lv_draw_label(layer, &label_dsc, &label_area);
            }
        }
    }
}

void BeltTrace::on_delete(lv_event_t* e) {
    // L069: retrieve impl from per-callback user_data, not lv_obj_get_user_data.
    // tick_observer_ dtor calls reset() automatically - L085 compliant.
    auto* self = static_cast<BeltTrace*>(lv_event_get_user_data(e));
    delete self;
}

// ---- XML widget registration ----

namespace {

void* belt_trace_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    BeltTrace::Mode mode = BeltTrace::Mode::WAVEFORM;
    for (int i = 0; attrs && attrs[i]; i += 2) {
        if (strcmp(attrs[i], "mode") == 0) {
            mode = strcmp(attrs[i + 1], "spectrum") == 0 ? BeltTrace::Mode::SPECTRUM
                                                         : BeltTrace::Mode::WAVEFORM;
        }
    }
    return BeltTrace::create(parent, mode);
}

} // namespace

void register_belt_trace_widget() {
    // Pass lv_xml_obj_apply directly — no belt_trace-specific apply logic
    // (matches helix_sparkline / notification_badge convention).
    lv_xml_register_widget("belt_trace", belt_trace_xml_create, lv_xml_obj_apply);
    spdlog::trace("[BeltTrace] Widget registered with XML system");
}

} // namespace ui
} // namespace helix
