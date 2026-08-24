// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_graph_tooltip.h"

#include "ui_format_utils.h"

#include "temp_graph_internal.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <new>
#include <string>

// State definition. Kept out of ui_temp_graph.h on purpose.
struct temp_graph_tooltip_t {
    bool has_pin = false;
    helix::temp_graph_internal::TempGraphHit pin{};
};

namespace helix::temp_graph_internal {

// See doc comment in temp_graph_internal.h.
const ui_temp_series_meta_t* find_meta_by_id(const ui_temp_graph_t* graph, int id) {
    if (!graph) {
        return nullptr;
    }
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (graph->series_meta[i].chart_series && graph->series_meta[i].id == id) {
            return &graph->series_meta[i];
        }
    }
    return nullptr;
}

int16_t target_deci_at(const ui_temp_series_meta_t* meta, int point_count, int logical_index) {
    if (!meta || !meta->target_deci_buf || meta->target_head <= 0) {
        return 0;
    }
    const int lead = point_count - meta->target_head; // chart slots with no target entry
    const int t_idx = logical_index - lead;
    if (t_idx < 0 || t_idx >= meta->target_head) {
        return 0;
    }
    return meta->target_deci_buf[t_idx];
}

std::optional<TempGraphHit> tooltip_hit_test(ui_temp_graph_t* graph, int32_t x, int32_t y) {
    if (!ui_temp_graph_is_valid(graph)) {
        return std::nullopt;
    }

    temp_graph_geometry_t geo{};
    if (!temp_graph_compute_geometry(graph, &geo)) {
        return std::nullopt;
    }

    const int32_t pc = static_cast<int32_t>(geo.point_count);
    const int32_t floor_y = geo.cy1 + geo.ch;
    const temp_graph_time_axis_t axis = temp_graph_time_axis(graph);

    int64_t best_d2 = std::numeric_limits<int64_t>::max();
    TempGraphHit best;
    bool found = false;

    // Ascending series order, then ascending logical index, gives the documented
    // deterministic tie-break for free (strict < never displaces an equal).
    for (int s = 0; s < UI_TEMP_GRAPH_MAX_SERIES; s++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[s];
        if (!meta->chart_series || !meta->visible) {
            continue;
        }
        int32_t* y_data = lv_chart_get_y_array(graph->chart, meta->chart_series);
        if (!y_data) {
            continue;
        }
        // SHIFT mode is circular, not a memmove: lv_chart_set_next_value writes at
        // start_point and advances it (lv_chart.c:694-696). Logical index pc-1 is
        // the newest sample; leading logical slots stay POINT_NONE until full.
        const uint32_t sp = lv_chart_get_x_start_point(graph->chart, meta->chart_series);

        // Record a candidate at squared distance `d2`, attributed to sample `idx`.
        // Strict < preserves the documented tie-break (lowest series, then lowest
        // logical index) because both loops below run in ascending order.
        auto consider = [&](int64_t d2, int32_t idx, int32_t val) {
            if (d2 >= best_d2) {
                return;
            }
            best_d2 = d2;
            best.series_id = meta->id;
            best.logical_index = idx;
            best.deci_temp = val;
            best.deci_target = target_deci_at(meta, pc, idx);
            // True sample time, not the axis-label mapping. The axis spreads
            // total_ms (= pc * 3s) across pc-1 gaps, so the two differ by under
            // one sample interval; the caption describes an actual sample.
            best.timestamp_ms = axis.latest_ms - static_cast<int64_t>(pc - 1 - idx) *
                                                     UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC * 1000;
            found = true;
        };

        int32_t prev_i = -1;
        int32_t prev_px = 0;
        int32_t prev_py = 0;
        int32_t prev_v = 0;

        for (int32_t i = 0; i < pc; i++) {
            const int32_t v = y_data[(sp + i) % pc];
            if (v == LV_CHART_POINT_NONE) {
                prev_i = -1; // a gap breaks the drawn line, so no segment spans it
                continue;
            }
            // Same forward mapping the gradient walk draws with. Deriving this
            // any other way puts the marker dot visibly off the line.
            const int32_t px = geo.cx1 + i * (geo.cw - 1) / (pc - 1);
            const int32_t py = floor_y - lv_map(v, geo.y_min, geo.y_max, 0, geo.ch);

            // The sample itself.
            const int64_t dx = px - x;
            const int64_t dy = py - y;
            consider(dx * dx + dy * dy, i, v);

            // The drawn segment from the previous adjacent sample to this one.
            // Testing samples alone makes the line feel dead exactly where it is
            // most interesting: on a steep run (a heater ramp) consecutive samples
            // are far apart vertically, so a tap landing squarely on the visible
            // line can be well outside the radius of BOTH endpoints. The hit is
            // attributed to the nearer endpoint, since the caption must describe a
            // real sample rather than an interpolated point.
            if (prev_i == i - 1) {
                const int64_t abx = px - prev_px;
                const int64_t aby = py - prev_py;
                const int64_t ab2 = abx * abx + aby * aby;
                if (ab2 > 0) {
                    int64_t t = (x - prev_px) * abx + (y - prev_py) * aby;
                    if (t < 0) {
                        t = 0;
                    } else if (t > ab2) {
                        t = ab2;
                    }
                    // Closest point on the segment, in integer math. The division
                    // truncates by at most a pixel, which is immaterial against a
                    // 28px radius.
                    const int64_t cx = prev_px + (abx * t) / ab2;
                    const int64_t cy = prev_py + (aby * t) / ab2;
                    const int64_t sdx = cx - x;
                    const int64_t sdy = cy - y;
                    const bool nearer_is_prev = (t * 2 <= ab2);
                    consider(sdx * sdx + sdy * sdy, nearer_is_prev ? prev_i : i,
                             nearer_is_prev ? prev_v : v);
                }
            }

            prev_i = i;
            prev_px = px;
            prev_py = py;
            prev_v = v;
        }
    }

    constexpr int64_t r = TEMP_GRAPH_TOOLTIP_HIT_RADIUS_PX;
    if (!found || best_d2 > r * r) {
        return std::nullopt;
    }
    return best;
}

lv_area_t temp_graph_tooltip_box_area(const temp_graph_geometry_t& geo, int32_t px, int32_t py,
                                      int32_t box_w, int32_t box_h) {
    constexpr int32_t GAP = 8;
    const int32_t plot_x2 = geo.cx1 + geo.cw;
    const int32_t plot_y2 = geo.cy1 + geo.ch;

    // Clamp to the plot BEFORE positioning. If the box is wider or taller than
    // the plot, no placement can satisfy both edges at once, so shrink it and
    // let the text clip rather than letting the box hang outside the chart.
    // With the shrink applied first, the two "pull back inside" adjustments
    // below can no longer undo each other, so their order doesn't matter.
    const int32_t w = LV_MIN(box_w, geo.cw);
    const int32_t h = LV_MIN(box_h, geo.ch);

    lv_area_t a;
    a.x1 = px - w / 2;
    if (a.x1 + w > plot_x2) {
        a.x1 = plot_x2 - w;
    }
    if (a.x1 < geo.cx1) {
        a.x1 = geo.cx1;
    }
    a.x2 = a.x1 + w;

    // Flip below when the point sits in the top third of the plot.
    const bool below = py < geo.cy1 + geo.ch / 3;
    a.y1 = below ? (py + GAP) : (py - GAP - h);
    if (a.y1 + h > plot_y2) {
        a.y1 = plot_y2 - h;
    }
    if (a.y1 < geo.cy1) {
        a.y1 = geo.cy1;
    }
    a.y2 = a.y1 + h;
    return a;
}

void temp_graph_tooltip_draw_cb(lv_event_t* e) {
    auto* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));
    const TempGraphHit* pin = temp_graph_tooltip_pinned(graph);
    if (!pin) {
        return;
    }
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) {
        return;
    }
    temp_graph_geometry_t geo{};
    if (!temp_graph_compute_geometry(graph, &geo)) {
        return;
    }
    const ui_temp_series_meta_t* meta = find_meta_by_id(graph, pin->series_id);
    if (!meta || !meta->visible) {
        return;
    }

    const int32_t pc = static_cast<int32_t>(geo.point_count);
    const int32_t px = geo.cx1 + pin->logical_index * (geo.cw - 1) / (pc - 1);
    const int32_t py = (geo.cy1 + geo.ch) - lv_map(pin->deci_temp, geo.y_min, geo.y_max, 0, geo.ch);

    // Line 1: "<name>   <temp>"   Line 2: "<time>   [<target>]"
    // `static`, not stack: lv_draw_label_dsc_t::text (ld.text, set below) is a
    // pointer LVGL keeps alive into a deferred draw task, so a stack buffer
    // would be a use-after-free once this function returns. That makes these
    // buffers per-translation-unit, not per-graph-instance - two graphs both
    // rendering a pin in the same draw pass would have the second overwrite
    // the first's text before either is actually drawn. Only the full-screen
    // overlay enables the tooltip today (see temp_graph_tooltip.h), so only
    // one graph can have a pin at a time; this would need revisiting if that
    // changes.
    static char l1_name[32];
    static char l1_temp[16];
    static char l2_time[16];
    static char l2_target[16];
    strncpy(l1_name, meta->name, sizeof(l1_name) - 1);
    l1_name[sizeof(l1_name) - 1] = '\0';
    snprintf(l1_temp, sizeof(l1_temp), "%.1f°", pin->deci_temp / 10.0f);

    time_t sec = static_cast<time_t>(pin->timestamp_ms / 1000);
    struct tm* tm_info = localtime(&sec);
    std::string t = helix::ui::format_time_with_seconds(tm_info);
    strncpy(l2_time, t.c_str(), sizeof(l2_time) - 1);
    l2_time[sizeof(l2_time) - 1] = '\0';

    const bool has_target = pin->deci_target != 0;
    if (has_target) {
        snprintf(l2_target, sizeof(l2_target), "%.0f°", pin->deci_target / 10.0f);
    }

    // ---- measure ----
    const lv_font_t* font = theme_manager_get_font("font_xs");
    const int32_t font_h = theme_manager_get_font_height(font);
    const int32_t pad = theme_manager_get_spacing("space_xs");
    const int32_t col_gap = pad * 2;

    auto text_w = [&](const char* s) {
        lv_point_t sz;
        lv_text_get_size(&sz, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return sz.x;
    };
    const int32_t left_w = LV_MAX(text_w(l1_name), text_w(l2_time));
    const int32_t right_w = LV_MAX(text_w(l1_temp), has_target ? text_w(l2_target) : 0);
    const int32_t box_w = pad + left_w + col_gap + right_w + pad;
    const int32_t box_h = pad + font_h * 2 + 2 + pad;

    const lv_area_t box = temp_graph_tooltip_box_area(geo, px, py, box_w, box_h);

    // ---- marker dot on the sampled point ----
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = meta->color;
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = LV_RADIUS_CIRCLE;
    dot.border_color = graph->cached_graph_bg;
    dot.border_width = 1;
    dot.border_opa = LV_OPA_COVER;
    constexpr int32_t DOT_R = 4;
    lv_area_t dot_area = {px - DOT_R, py - DOT_R, px + DOT_R, py + DOT_R};
    lv_draw_rect(layer, &dot, &dot_area);

    // ---- tail, only when the clamped box still spans the point ----
    const lv_color_t box_bg = theme_manager_get_color("card_bg");
    constexpr int32_t TAIL_W = 6;
    const bool box_above = box.y2 <= py;
    if (px - TAIL_W >= box.x1 && px + TAIL_W <= box.x2) {
        lv_draw_triangle_dsc_t tail;
        lv_draw_triangle_dsc_init(&tail);
        tail.color = box_bg;
        tail.opa = LV_OPA_COVER;
        const int32_t base_y = box_above ? box.y2 : box.y1;
        const int32_t tip_y = box_above ? (py - DOT_R) : (py + DOT_R);
        tail.p[0].x = px - TAIL_W;
        tail.p[0].y = base_y;
        tail.p[1].x = px + TAIL_W;
        tail.p[1].y = base_y;
        tail.p[2].x = px;
        tail.p[2].y = tip_y;
        lv_draw_triangle(layer, &tail);
    }

    // ---- box ----
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = box_bg;
    r.bg_opa = LV_OPA_COVER;
    r.radius = pad;
    r.border_color = meta->color;
    r.border_width = 1;
    r.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &r, &box);

    // ---- text ----
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = font;
    ld.opa = LV_OPA_COVER;

    const int32_t l1_y = box.y1 + pad;
    const int32_t l2_y = l1_y + font_h + 2;
    const int32_t left_x = box.x1 + pad;
    const int32_t right_x2 = box.x2 - pad;

    auto draw_text = [&](const char* s, int32_t x1, int32_t x2, int32_t y, lv_text_align_t align,
                         lv_color_t color) {
        ld.text = s;
        ld.align = align;
        ld.color = color;
        lv_area_t a = {x1, y, x2, y + font_h};
        lv_draw_label(layer, &ld, &a);
    };

    const lv_color_t text_c = theme_manager_get_color("text");
    const lv_color_t muted_c = theme_manager_get_color("text_muted");

    draw_text(l1_name, left_x, left_x + left_w, l1_y, LV_TEXT_ALIGN_LEFT, meta->color);
    draw_text(l1_temp, right_x2 - right_w, right_x2, l1_y, LV_TEXT_ALIGN_RIGHT, text_c);
    draw_text(l2_time, left_x, left_x + left_w, l2_y, LV_TEXT_ALIGN_LEFT, muted_c);
    if (has_target) {
        draw_text(l2_target, right_x2 - right_w, right_x2, l2_y, LV_TEXT_ALIGN_RIGHT, muted_c);
    }
}

static void tooltip_press_cb(lv_event_t* e) {
    auto* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));
    if (!ui_temp_graph_is_valid(graph) || !graph->tooltip) {
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    auto hit = tooltip_hit_test(graph, p.x, p.y);
    if (hit.has_value()) {
        temp_graph_tooltip_pin(graph, *hit);
    } else {
        temp_graph_tooltip_clear(graph); // tap-away dismisses
    }
}

void temp_graph_tooltip_pin(ui_temp_graph_t* graph, const TempGraphHit& hit) {
    if (!ui_temp_graph_is_valid(graph) || !graph->tooltip) {
        return;
    }
    graph->tooltip->pin = hit;
    graph->tooltip->has_pin = true;
    spdlog::debug("[TempGraph] Caption pinned: series={} idx={} {}d {}ms", hit.series_id,
                  hit.logical_index, hit.deci_temp, hit.timestamp_ms);
    lv_obj_invalidate(graph->chart);
}

const TempGraphHit* temp_graph_tooltip_pinned(const ui_temp_graph_t* graph) {
    if (!graph || !graph->tooltip || !graph->tooltip->has_pin) {
        return nullptr;
    }
    return &graph->tooltip->pin;
}

void temp_graph_tooltip_clear(ui_temp_graph_t* graph) {
    if (!graph || !graph->tooltip || !graph->tooltip->has_pin) {
        return;
    }
    graph->tooltip->has_pin = false;
    if (graph->chart) {
        lv_obj_invalidate(graph->chart);
    }
}

void temp_graph_tooltip_on_sample_pushed(ui_temp_graph_t* graph, int series_id) {
    const TempGraphHit* pin = temp_graph_tooltip_pinned(graph);
    if (!pin || pin->series_id != series_id) {
        return;
    }
    if (graph->tooltip->pin.logical_index <= 0) {
        temp_graph_tooltip_clear(graph); // scrolled off the left edge
        return;
    }
    graph->tooltip->pin.logical_index--;
    lv_obj_invalidate(graph->chart);
}

void temp_graph_tooltip_on_series_hidden(ui_temp_graph_t* graph, int series_id) {
    const TempGraphHit* pin = temp_graph_tooltip_pinned(graph);
    if (pin && pin->series_id == series_id) {
        temp_graph_tooltip_clear(graph);
    }
}

void temp_graph_tooltip_destroy(ui_temp_graph_t* graph) {
    if (!graph) {
        return;
    }
    // Sever the press and draw callbacks before the chart's deferred deletion.
    // The chart outlives `graph` by one async tick (lv_obj_delete_async in
    // ui_temp_graph_destroy), so a CLICKED landing in that window would fire
    // tooltip_press_cb, or a pending redraw would fire temp_graph_tooltip_draw_cb,
    // against a freed graph. Unconditional: remove_event_cb is a no-op when the
    // callback was never registered (draw_cb is registered unconditionally at
    // create time; press_cb only when the tooltip was enabled). Mirrors the
    // severance block in ui_temp_graph_destroy, which exists for exactly this
    // reason.
    if (graph->chart) {
        lv_obj_remove_event_cb(graph->chart, tooltip_press_cb);
        lv_obj_remove_event_cb(graph->chart, temp_graph_tooltip_draw_cb);
    }
    if (!graph->tooltip) {
        return;
    }
    delete graph->tooltip;
    graph->tooltip = nullptr;
}

void temp_graph_tooltip_free_state(ui_temp_graph_t* graph) {
    if (!graph || !graph->tooltip) {
        return;
    }
    delete graph->tooltip;
    graph->tooltip = nullptr;
}

} // namespace helix::temp_graph_internal

void ui_temp_graph_set_tooltip_enabled(ui_temp_graph_t* graph, bool enabled) {
    if (!ui_temp_graph_is_valid(graph)) {
        return;
    }
    if (enabled == (graph->tooltip != nullptr)) {
        return;
    }
    if (enabled) {
        graph->tooltip = new (std::nothrow) temp_graph_tooltip_t();
        if (!graph->tooltip) {
            spdlog::error("[TempGraph] Failed to allocate tooltip state");
            return;
        }
        lv_obj_add_flag(graph->chart, LV_OBJ_FLAG_CLICKABLE);
        // CLICKED (release without scroll), not PRESSED, so a scroll gesture that
        // happens to begin over the chart does not raise a caption.
        // DECLARATIVE_OK: the chart is a C++-created widget with no XML layer,
        // and the handler needs the raw indev coordinates.
        lv_obj_add_event_cb(graph->chart, helix::temp_graph_internal::tooltip_press_cb,
                            LV_EVENT_CLICKED, graph);
    } else {
        // temp_graph_tooltip_destroy is NOT used here: it also severs
        // draw_cb, which is registered exactly once, unconditionally, at
        // graph creation and never re-added on enable. Severing it on
        // disable would leave a later re-enable pinning state on tap but
        // drawing nothing. Sever press_cb (below) and free the pin state
        // only; draw_cb stays registered and simply no-ops with no pin.
        helix::temp_graph_internal::temp_graph_tooltip_clear(graph);
        lv_obj_remove_event_cb_with_user_data(graph->chart,
                                              helix::temp_graph_internal::tooltip_press_cb, graph);
        lv_obj_remove_flag(graph->chart, LV_OBJ_FLAG_CLICKABLE);
        helix::temp_graph_internal::temp_graph_tooltip_free_state(graph);
    }
}

bool ui_temp_graph_tooltip_is_enabled(const ui_temp_graph_t* graph) {
    return graph && graph->tooltip != nullptr;
}
