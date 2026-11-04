// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix_sparkline.h"

#include "core/observer_factory.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "performance_state.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix {
namespace ui {

// ---- HelixSparkline implementation ----

lv_obj_t* HelixSparkline::create(lv_obj_t* parent, const std::string& source) {
    auto* impl = new HelixSparkline(source);
    impl->obj_ = lv_obj_create(parent);
    lv_obj_set_user_data(impl->obj_, impl);
    lv_obj_remove_style_all(impl->obj_);
    lv_obj_set_size(impl->obj_, 80, 16);
    lv_obj_clear_flag(impl->obj_, LV_OBJ_FLAG_SCROLLABLE);

    // LV_EVENT_DRAW_MAIN_END fires after the base LVGL draw pass — we overlay
    // our sparkline lines on top without interfering with style-based rendering.
    // L079: DRAW_TASK_ADDED is post-draw and cannot add new draw tasks; MAIN_END
    // is the correct event for custom drawing.
    lv_obj_add_event_cb(impl->obj_, on_draw, LV_EVENT_DRAW_MAIN_END, impl);
    lv_obj_add_event_cb(impl->obj_, on_delete, LV_EVENT_DELETE, impl);

    // perf_history_tick is NOT a never-freed singleton: PerformanceState
    // destroys and recreates its subjects on reconnect / printer switch (and
    // between tests). Pass PerformanceState's subject lifetime so this observer
    // releases safely when the subject is torn down — otherwise a sparkline that
    // outlives the subject (e.g. a row condemned via lv_obj_delete_async, then
    // deleted after deinit_subjects()) would call lv_observer_remove() on a
    // freed subject → UAF at lv_observer.c:584.
    lv_subject_t* tick = lv_xml_get_subject(nullptr, "perf_history_tick");
    if (tick) {
        impl->tick_observer_ = helix::ui::observe<int>(
            tick, impl, [](HelixSparkline* self, int /*value*/) { self->invalidate_self(); },
            helix::perf::PerformanceState::instance().subjects_lifetime());
    } else {
        spdlog::debug("[HelixSparkline] perf_history_tick subject not found — "
                      "sparkline for '{}' will not auto-refresh",
                      source);
    }

    return impl->obj_;
}

HelixSparkline::HelixSparkline(const std::string& source) : source_(source) {}

void HelixSparkline::invalidate_self() {
    if (obj_) {
        lv_obj_invalidate(obj_);
    }
}

void HelixSparkline::on_draw(lv_event_t* e) {
    // user_data is the per-callback value passed to lv_obj_add_event_cb (L069).
    auto* self = static_cast<HelixSparkline*>(lv_event_get_user_data(e));
    if (!self || !self->obj_)
        return;

    auto hist = helix::perf::PerformanceState::instance().read_history(self->source_);
    if (hist.size() < 2)
        return;

    lv_area_t area;
    lv_obj_get_coords(self->obj_, &area);
    const int x0 = area.x1;
    const int y0 = area.y1;
    const int w = lv_area_get_width(&area);
    const int h = lv_area_get_height(&area);
    if (w <= 0 || h <= 0)
        return;

    // Scale to visible range (with 1-unit floor to avoid div-by-zero).
    float min_v = hist.front();
    float max_v = hist.front();
    for (float v : hist) {
        if (v < min_v)
            min_v = v;
        if (v > max_v)
            max_v = v;
    }
    if (max_v - min_v < 1e-3f)
        max_v = min_v + 1.0f;

    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer)
        return;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    // Read from the obj's style so XML callers can set style_line_color and
    // bind_style can swap it for threshold colors (e.g. green/amber/red).
    dsc.color = lv_obj_get_style_line_color(self->obj_, LV_PART_MAIN);
    dsc.width = 1;
    dsc.opa = LV_OPA_COVER;

    const size_t n = hist.size();
    for (size_t i = 1; i < n; ++i) {
        const float t0 = static_cast<float>(i - 1) / static_cast<float>(n - 1);
        const float t1 = static_cast<float>(i) / static_cast<float>(n - 1);
        const float n0 = (hist[i - 1] - min_v) / (max_v - min_v);
        const float n1 = (hist[i] - min_v) / (max_v - min_v);

        dsc.p1.x = static_cast<lv_value_precise_t>(x0 + t0 * (w - 1));
        dsc.p1.y = static_cast<lv_value_precise_t>(y0 + (1.0f - n0) * (h - 1));
        dsc.p2.x = static_cast<lv_value_precise_t>(x0 + t1 * (w - 1));
        dsc.p2.y = static_cast<lv_value_precise_t>(y0 + (1.0f - n1) * (h - 1));

        lv_draw_line(layer, &dsc);
    }
}

void HelixSparkline::on_delete(lv_event_t* e) {
    // L069: retrieve impl from per-callback user_data, not lv_obj_get_user_data.
    // tick_observer_ dtor calls reset() automatically — L085 compliant.
    auto* self = static_cast<HelixSparkline*>(lv_event_get_user_data(e));
    delete self;
}

// ---- XML widget registration ----

namespace {

void* sparkline_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    const char* source = nullptr;
    for (int i = 0; attrs && attrs[i]; i += 2) {
        if (strcmp(attrs[i], "source") == 0) {
            source = attrs[i + 1];
        }
    }
    return HelixSparkline::create(parent, source ? source : "");
}

} // namespace

void register_helix_sparkline_widget() {
    // Pass lv_xml_obj_apply directly — no sparkline-specific apply logic
    // (matches notification_badge / ui_card convention).
    lv_xml_register_widget("helix_sparkline", sparkline_xml_create, lv_xml_obj_apply);
    spdlog::trace("[HelixSparkline] Widget registered with XML system");
}

} // namespace ui
} // namespace helix
