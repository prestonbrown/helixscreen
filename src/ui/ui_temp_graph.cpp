// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_temp_graph.h"

#include "ui_format_utils.h"

#include "system/crash_handler.h"
#include "temp_graph_column_map.h"
#include "temp_graph_internal.h"
#include "temp_graph_tooltip.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utility>
#include <vector>

using helix::temp_graph_internal::temp_graph_compute_geometry;
using helix::temp_graph_internal::temp_graph_geometry_t;
using helix::temp_graph_internal::temp_graph_time_axis;
using helix::temp_graph_internal::temp_graph_time_axis_t;
using helix::ui::format_time;

// Internal scale factor: store deci-degrees (×10) in the LVGL chart for 0.1°C precision.
// Without this, float→int32_t truncation creates visible staircases in the line chart.
// All public API values remain in degrees; scaling is applied at the LVGL boundary.
static constexpr int32_t TEMP_SCALE = 10;

// Mark the gradient cache for recompute on the next draw. Called from every path
// that changes what the gradient should look like (data push, range change,
// feature toggle, visibility change, theme color refresh).
static inline void mark_gradient_cache_dirty(ui_temp_graph_t* graph) {
    if (graph)
        graph->gradient_cache_dirty = true;
}

// Helper: Find series metadata by ID
// Returns nullptr if graph, chart, or series is invalid (protects against use-after-free
// when chart LVGL widget is destroyed but ui_temp_graph_t struct survives)
static ui_temp_series_meta_t* find_series(ui_temp_graph_t* graph, int series_id) {
    if (!graph || !graph->chart || series_id < 0 || series_id >= UI_TEMP_GRAPH_MAX_SERIES) {
        return nullptr;
    }

    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (graph->series_meta[i].id == series_id &&
            graph->series_meta[i].chart_series != nullptr) {
            return &graph->series_meta[i];
        }
    }
    return nullptr;
}

namespace helix::temp_graph_internal {

// Split a target buffer into [start, end) runs of strictly positive samples.
// Pure: no LVGL, no allocation beyond the return vector. Exposed for unit testing.
std::vector<std::pair<int, int>> segment_target_buf(const int16_t* buf, int count) {
    std::vector<std::pair<int, int>> out;
    if (!buf || count <= 0)
        return out;

    int seg_start = -1;
    for (int i = 0; i < count; i++) {
        bool active = buf[i] > 0;
        if (active && seg_start < 0) {
            seg_start = i;
        } else if (!active && seg_start >= 0) {
            out.emplace_back(seg_start, i);
            seg_start = -1;
        }
    }
    if (seg_start >= 0)
        out.emplace_back(seg_start, count);

    return out;
}

// Collapse runs of equal target values within [first, second) into single line
// segments. The target trace is a step function, so a held setpoint produces a
// long run of identical samples that the old per-sample draw rendered as one
// sub-pixel dashed line per sample (up to ~1200 per series). Each returned pair
// (a, b) means "draw a line from buffer index a to index b"; the union of the
// returned pairs is geometrically identical to connecting every consecutive
// sample, but with O(value-changes) draw calls instead of O(samples) (#979).
std::vector<std::pair<int, int>> coalesce_target_runs(const int16_t* buf, int first, int second) {
    std::vector<std::pair<int, int>> out;
    if (!buf || second - first < 2)
        return out;

    int run_start = first;
    for (int j = first; j + 1 < second; j++) {
        if (buf[j + 1] != buf[j]) {
            // Flush the flat horizontal run ending at j (skip if it's a single point).
            if (j > run_start)
                out.emplace_back(run_start, j);
            // Connector to the new value (sloped/vertical step).
            out.emplace_back(j, j + 1);
            run_start = j + 1;
        }
    }
    // Trailing horizontal run (skip if it's a single point).
    if (second - 1 > run_start)
        out.emplace_back(run_start, second - 1);

    return out;
}

} // namespace helix::temp_graph_internal

// Helper: Create a muted (reduced opacity) version of a color
// Since LVGL chart cursors don't support opacity, we blend toward the background
static lv_color_t mute_color(lv_color_t color, lv_opa_t opa, lv_color_t bg) {
    // Blend toward chart background based on opacity
    // opa=255 = full color, opa=0 = full background
    uint8_t r = (color.red * opa + bg.red * (255 - opa)) / 255;
    uint8_t g = (color.green * opa + bg.green * (255 - opa)) / 255;
    uint8_t b = (color.blue * opa + bg.blue * (255 - opa)) / 255;
    return lv_color_make(r, g, b);
}

// Event callback: Invalidate chart on resize so custom draw callbacks recalculate
static void chart_resize_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    if (chart) {
        lv_obj_invalidate(chart);
    }
}

// Event callback: Null out graph->chart when the LVGL chart widget is destroyed.
// Prevents use-after-free when parent widget deletion cascades to the chart
// but the ui_temp_graph_t struct (and temp_graphs registrations) survive.
static void chart_delete_cb(lv_event_t* e) {
    auto* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));
    if (graph) {
        spdlog::debug("[TempGraph] Chart widget deleted, nulling graph->chart");
        graph->chart = nullptr;
        // The gradient canvas is a child of the chart, so LVGL's delete cascade
        // already freed it — just null our pointer. The backing draw buffer is
        // ours to free.
        graph->gradient_canvas = nullptr;
        if (graph->gradient_cache_buf) {
            lv_draw_buf_destroy(graph->gradient_cache_buf);
            graph->gradient_cache_buf = nullptr;
        }
        graph->gradient_cache_w = 0;
        graph->gradient_cache_h = 0;
    }
}

// Helper: Find series metadata by color (for draw task matching)
static ui_temp_series_meta_t* find_series_by_color(ui_temp_graph_t* graph, lv_color_t color) {
    if (!graph)
        return nullptr;

    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (graph->series_meta[i].chart_series &&
            lv_color_to_u32(graph->series_meta[i].color) == lv_color_to_u32(color)) {
            return &graph->series_meta[i];
        }
    }
    return nullptr;
}

// Helper: Update max visible temperature across all series
// Called when data changes to maintain gradient reference point
static void update_max_visible_temp(ui_temp_graph_t* graph) {
    if (!graph)
        return;

    float max_temp = graph->min_temp; // Start at minimum

    // Scan all series to find the maximum visible temperature
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[i];
        if (!meta->chart_series || !meta->visible)
            continue;

        // Get series data array from LVGL chart
        uint32_t point_count = 0;
        int32_t* y_points = lv_chart_get_y_array(graph->chart, meta->chart_series);
        if (!y_points)
            continue;

        point_count = lv_chart_get_point_count(graph->chart);

        // Find max in this series (skip uninitialized LV_CHART_POINT_NONE values)
        for (uint32_t j = 0; j < point_count; j++) {
            // Skip uninitialized points (LVGL sets these to LV_CHART_POINT_NONE = INT32_MAX)
            if (y_points[j] == LV_CHART_POINT_NONE)
                continue;

            float temp = static_cast<float>(y_points[j]) / TEMP_SCALE;
            if (temp > graph->min_temp && temp > max_temp) {
                max_temp = temp;
            }
        }
    }

    // Ensure we have at least some gradient span (avoid division by zero)
    if (max_temp <= graph->min_temp) {
        max_temp = graph->min_temp + 1.0f;
    }

    graph->max_visible_temp = max_temp;
}

// Over-range point masking: temporarily replace data points exceeding the Y-axis range
// with LV_CHART_POINT_NONE so LVGL draws gaps instead of misleading clamped flat bars.
// Original values are saved and restored after drawing completes.
struct overrange_save_t {
    int32_t* location; // Pointer into LVGL's y_points array
    int32_t value;     // Original value
};
// Keyed by graph so concurrently-drawn charts (overlay + mini graph + widget) do
// not clobber each other's saved points. A single file-static vector was a
// correctness bug: chart B's DRAW_MAIN_BEGIN would clear chart A's saved
// pointers before chart A's DRAW_MAIN_END could restore them, leaving chart A's
// over-range points stuck at LV_CHART_POINT_NONE (line gaps / frozen segments)
// until the next data write.
static std::map<ui_temp_graph_t*, std::vector<overrange_save_t>> s_overrange_saved;

// DRAW_MAIN_BEGIN: mask over-range points before LVGL draws chart lines
static void mask_overrange_begin_cb(lv_event_t* e) {
    auto* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));
    if (!graph || !graph->chart)
        return;

    std::vector<overrange_save_t>& saved = s_overrange_saved[graph];
    saved.clear();

    int32_t y_max = static_cast<int32_t>(graph->max_temp * TEMP_SCALE);
    uint32_t pc = lv_chart_get_point_count(graph->chart);

    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[i];
        if (!meta->chart_series || !meta->visible)
            continue;

        int32_t* y_data = lv_chart_get_y_array(graph->chart, meta->chart_series);
        if (!y_data)
            continue;

        for (uint32_t j = 0; j < pc; j++) {
            if (y_data[j] != LV_CHART_POINT_NONE && y_data[j] > y_max) {
                saved.push_back({&y_data[j], y_data[j]});
                y_data[j] = LV_CHART_POINT_NONE;
            }
        }
    }
}

// DRAW_MAIN_END: restore original values so gradient callback sees full data
static void mask_overrange_end_cb(lv_event_t* e) {
    auto* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));
    if (!graph)
        return;

    auto it = s_overrange_saved.find(graph);
    if (it == s_overrange_saved.end())
        return;

    for (auto& saved : it->second) {
        *saved.location = saved.value;
    }
    it->second.clear();
}

// See temp_graph_internal.h for the doc comment — shared with the hit test.
namespace helix::temp_graph_internal {

bool temp_graph_compute_geometry(ui_temp_graph_t* graph, temp_graph_geometry_t* g) {
    lv_obj_t* chart = graph->chart;
    if (!chart)
        return false;

    lv_area_t obj_coords;
    lv_obj_get_coords(chart, &obj_coords);

    int32_t pad_left = lv_obj_get_style_pad_left(chart, LV_PART_MAIN);
    int32_t pad_right = lv_obj_get_style_pad_right(chart, LV_PART_MAIN);
    int32_t pad_top = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);

    int32_t cx1 = obj_coords.x1 + pad_left;
    int32_t cx2 = obj_coords.x2 - pad_right;
    int32_t cy1 = obj_coords.y1 + pad_top;
    int32_t cy2 = obj_coords.y2 - pad_bottom;
    int32_t cw = cx2 - cx1;
    int32_t ch = cy2 - cy1;
    if (cw <= 1 || ch <= 0)
        return false;

    uint32_t point_count = lv_chart_get_point_count(chart);
    if (point_count < 2)
        return false;

    int32_t y_min = static_cast<int32_t>(graph->min_temp * TEMP_SCALE);
    int32_t y_max = static_cast<int32_t>(graph->max_temp * TEMP_SCALE);
    if (y_max - y_min <= 0)
        return false;

    g->cx1 = cx1;
    g->cy1 = cy1;
    g->cw = cw;
    g->ch = ch;
    g->y_min = y_min;
    g->y_max = y_max;
    g->point_count = point_count;
    return true;
}

} // namespace helix::temp_graph_internal

// Render the per-column gradient fill for every visible series into `target`,
// with column 0 at x_off and the chart floor at y_off + ch (content-relative
// when drawing to the cache buffer, absolute when drawing to the event layer).
static void gradient_render_columns(ui_temp_graph_t* graph, const temp_graph_geometry_t& g,
                                    lv_layer_t* target, int32_t x_off, int32_t y_off) {
    int32_t cw = g.cw;
    int32_t ch = g.ch;
    int32_t pc = static_cast<int32_t>(g.point_count);
    int32_t y_min = g.y_min;
    int32_t y_max = g.y_max;
    int32_t floor_y = y_off + ch; // chart floor in target coords

    for (int s = 0; s < UI_TEMP_GRAPH_MAX_SERIES; s++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[s];
        if (!meta->chart_series || !meta->visible)
            continue;
        if (meta->gradient_top_opa == LV_OPA_TRANSP && meta->gradient_bottom_opa == LV_OPA_TRANSP)
            continue;

        int32_t* y_data = lv_chart_get_y_array(graph->chart, meta->chart_series);
        if (!y_data)
            continue;

        uint32_t sp = lv_chart_get_x_start_point(graph->chart, meta->chart_series);

        lv_draw_fill_dsc_t fd;
        lv_draw_fill_dsc_init(&fd);
        fd.color = meta->color;
        fd.opa = LV_OPA_COVER;
        fd.grad.dir = LV_GRAD_DIR_VER;
        fd.grad.stops_count = 2;
        // LVGL 9.5 vertical fill: frac=0 maps to the TOP row of the fill region,
        // frac=255 to the BOTTOM (verified in lv_draw_sw_grad.c color_map indexing +
        // lv_draw_sw_fill.c VER case: opa_map[h - y1], so y1/top => index 0 => frac 0).
        // Each column is filled from the curve (top) down to the chart floor, so the
        // top stop (frac=0) carries the "at the line" opacity and the bottom stop
        // (frac=255) the floor opacity — the line is solid, fading toward the axis.
        fd.grad.stops[0].color = meta->color;
        fd.grad.stops[0].opa = meta->gradient_top_opa;
        fd.grad.stops[0].frac = 0;
        fd.grad.stops[1].color = meta->color;
        fd.grad.stops[1].opa = meta->gradient_bottom_opa;
        fd.grad.stops[1].frac = 255;

        // Coalesce runs of columns that share a top row. Every such column is
        // the SAME fill - same vertical ramp, same span down to the floor - so
        // one rect per run is pixel-identical to one rect per column. Worth it
        // because the cost is per DRAW TASK, not per pixel: measured on a K2
        // Plus, queueing 456 single-column fills took 7-22ms while
        // lv_canvas_finish_layer() spent 106-162ms rasterising them. A near-flat
        // trace - an idle printer - collapses to a handful of runs.
        //
        // A run MUST break at any column the renderer does not fill, or the
        // merged rect bridges the gap and paints columns that must stay empty.
        bool run_open = false;
        int32_t run_y = 0;
        int32_t run_x1 = 0;
        int32_t run_x2 = 0;
        auto flush_run = [&]() {
            if (!run_open)
                return;
            lv_area_t run_area;
            run_area.x1 = run_x1;
            run_area.x2 = run_x2;
            run_area.y1 = run_y;
            run_area.y2 = floor_y - 1;
            lv_draw_fill(target, &fd, &run_area);
            run_open = false;
        };

        // Walk columns (not segments) so each pixel column is drawn exactly
        // once. When pc > cw (e.g. 400 points over ~332 px) a segment walk
        // would draw overlapping fills at shared boundaries, compounding
        // semi-transparent opacity into visible dark bands.
        for (int32_t x = 0; x < cw; x++) {
            // One shared mapper answers "where does this column's fill start",
            // so the cache signature cannot drift from what is actually drawn.
            int32_t idx = 0;
            int32_t t = 0;
            helix::temp_graph::column_to_point(x, cw, pc, idx, t);
            const int32_t mapped = helix::temp_graph::column_series_y(
                y_data[(sp + idx) % pc], y_data[(sp + idx + 1) % pc], t, y_min, y_max, ch);
            if (mapped == helix::temp_graph::COLUMN_NOT_DRAWN) {
                flush_run();
                continue;
            }
            const int32_t series_y = y_off + mapped; // mapper is content-relative

            // Fill from the curve down to the chart floor with the full-height
            // vertical fade. The ramp stays faintly visible well below the line,
            // so a taller series' tint correctly overlays lower series via alpha.
            // We deliberately do NOT clamp to a band: with this gentle ramp the
            // deep pixels are low-opacity but not invisible, so clamping would
            // change the multi-series appearance (#979 review).
            if (floor_y - 1 < series_y) {
                flush_run();
                continue;
            }

            if (run_open && series_y == run_y) {
                run_x2 = x_off + x; // extend
                continue;
            }
            flush_run();
            run_open = true;
            run_y = series_y;
            run_x1 = x_off + x;
            run_x2 = x_off + x;
        }

        flush_run(); // tail
    }
}

// Lazily (re)create the offscreen canvas + ARGB8888 buffer for the current
// viewport size. Returns true when a usable buffer is available. A size change
// reallocates the buffer and marks the cache dirty (cached pixels no longer map
// to the new geometry). Must run OUTSIDE the render pass.
static bool gradient_ensure_cache_buf(ui_temp_graph_t* graph, const temp_graph_geometry_t& g) {
    if (!graph->chart)
        return false;

    if (!graph->gradient_canvas) {
        // The canvas is HIDDEN — LVGL never auto-draws it. We render into its
        // backing buffer below and blit that buffer manually in the draw cb.
        graph->gradient_canvas = lv_canvas_create(graph->chart);
        if (graph->gradient_canvas) {
            lv_obj_add_flag(graph->gradient_canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(graph->gradient_canvas, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if (!graph->gradient_canvas)
        return false;

    if (!graph->gradient_cache_buf || graph->gradient_cache_w != g.cw ||
        graph->gradient_cache_h != g.ch) {
        if (graph->gradient_cache_buf) {
            lv_draw_buf_destroy(graph->gradient_cache_buf);
            graph->gradient_cache_buf = nullptr;
        }
        graph->gradient_cache_buf = lv_draw_buf_create(g.cw, g.ch, LV_COLOR_FORMAT_ARGB8888, 0);
        if (graph->gradient_cache_buf) {
            // Bind the draw-buf directly. lv_canvas_set_buffer() would treat its
            // pointer arg as raw pixel data; passing a lv_draw_buf_t* there makes
            // the canvas rasterize ~cw*ch*4 bytes onto the 40-byte struct address
            // (heap overrun). set_draw_buf assigns the buffer as-is.
            lv_canvas_set_draw_buf(graph->gradient_canvas, graph->gradient_cache_buf);
            graph->gradient_cache_w = g.cw;
            graph->gradient_cache_h = g.ch;
            graph->gradient_cache_dirty = true;
            // Fresh buffer: its contents are undefined, so a signature match must
            // not be allowed to skip the first render into it.
            graph->gradient_cache_sig_valid = false;
        }
    }
    return graph->gradient_cache_buf != nullptr;
}

// Hash of the PIXEL ROWS gradient_render_columns() will actually fill.
//
// The obvious signature - hash the raw y-data - is exact but useless: real
// temperatures jitter a tenth of a degree every sample, so the values always
// differ and the hash always changes, even though at 456x102 that wiggle lands
// on the same pixel row and the drawn result is identical. Measured on a K2
// Plus: 40 renders, 0 skips, median 167ms each.
//
// So quantise the way the renderer does. Per column it derives exactly one
// number, series_y (the top of the fill; everything below is the same ramp down
// to the floor), and that number alone decides the pixels. Walk the same columns
// with the same integer math and hash series_y, and the signature changes if and
// only if the rendered image would.
static uint64_t gradient_content_signature(ui_temp_graph_t* graph, const temp_graph_geometry_t& g) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL; // FNV-1a prime
    };

    const int32_t cw = g.cw;
    const int32_t ch = g.ch;
    const int32_t pc = static_cast<int32_t>(g.point_count);
    const int32_t y_min = g.y_min;
    const int32_t y_max = g.y_max;
    mix(static_cast<uint64_t>(cw));
    mix(static_cast<uint64_t>(ch));
    mix(static_cast<uint64_t>(pc));
    if (cw <= 1 || pc <= 1) {
        return h; // degenerate viewport: the renderer draws nothing
    }
    // Buffer-relative coords, matching the gradient_render_columns(.., 0, 0) call.
    const int32_t y_off = 0;
    const int32_t floor_y = y_off + ch;

    for (int si = 0; si < UI_TEMP_GRAPH_MAX_SERIES; si++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[si];
        // Mirror the renderer's skip conditions exactly, so a series it ignores
        // cannot perturb the hash and one it draws is always covered.
        if (!meta->chart_series || !meta->visible)
            continue;
        if (meta->gradient_top_opa == LV_OPA_TRANSP && meta->gradient_bottom_opa == LV_OPA_TRANSP)
            continue;
        int32_t* y_data = lv_chart_get_y_array(graph->chart, meta->chart_series);
        if (!y_data)
            continue;

        mix(static_cast<uint64_t>(si));
        mix(static_cast<uint64_t>(lv_color_to_u32(meta->color)));
        mix(static_cast<uint64_t>(meta->gradient_top_opa));
        mix(static_cast<uint64_t>(meta->gradient_bottom_opa));

        const uint32_t sp = lv_chart_get_x_start_point(graph->chart, meta->chart_series);
        for (int32_t x = 0; x < cw; x++) {
            int32_t idx = 0;
            int32_t t = 0;
            helix::temp_graph::column_to_point(x, cw, pc, idx, t);
            // Hash the mapper's answer INCLUDING its not-drawn sentinel, so a
            // column appearing or disappearing changes the signature.
            mix(static_cast<uint64_t>(static_cast<uint32_t>(helix::temp_graph::column_series_y(
                y_data[(sp + idx) % pc], y_data[(sp + idx + 1) % pc], t, y_min, y_max, ch))));
        }
    }
    return h;
}

// Escape hatch for measuring the skip's effect on a device without swapping
// binaries: HELIX_TEMP_GRAPH_GRAD_SKIP=0 forces every dirty frame to render.
static bool gradient_skip_enabled() {
    static const bool enabled = [] {
        const char* v = getenv("HELIX_TEMP_GRAPH_GRAD_SKIP");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// Recompute the cached gradient buffer. lv_canvas_init_layer / finish_layer run a
// nested synchronous draw dispatch and end with lv_obj_invalidate(canvas), which
// is ILLEGAL during an active render pass (disp->rendering_in_progress) — doing it
// from the DRAW_MAIN_END draw callback produced an empty/undrawn buffer, so the
// blit rendered nothing (the LVGL 9.5 gradient regression; same rule the filament
// path layers obey, see ui_filament_path_layers.cpp). This runs from the
// gradient_refresh timer, OUTSIDE the render pass, so the canvas layer round-trip
// is legal here.
static void gradient_recompute(ui_temp_graph_t* graph) {
    if (!graph || !graph->chart)
        return;
    if (!(graph->features & TEMP_GRAPH_FEATURE_GRADIENTS))
        return;

    temp_graph_geometry_t g;
    if (!temp_graph_compute_geometry(graph, &g))
        return;
    if (!gradient_ensure_cache_buf(graph, g))
        return;
    if (!graph->gradient_cache_dirty)
        return; // nothing changed since the last recompute

    // A pushed sample dirties the cache unconditionally, but most frames render
    // the same picture. Skip those before paying for the per-column fill - and
    // before the invalidate below, which would repaint the chart for nothing.
    const uint64_t sig = gradient_content_signature(graph, g);
    if (gradient_skip_enabled() && graph->gradient_cache_sig_valid &&
        graph->gradient_cache_sig == sig) {
        graph->gradient_cache_dirty = false;
        ++graph->gradient_skip_count;
        spdlog::debug("[TempGraph {}] gradient skip #{} (unchanged content, {}x{})",
                      static_cast<const void*>(graph), graph->gradient_skip_count, g.cw, g.ch);
        return;
    }

    using clk = std::chrono::steady_clock;
    auto us_since = [](clk::time_point t) {
        return std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t).count();
    };
    const auto render_start = clk::now();

    lv_canvas_fill_bg(graph->gradient_canvas, lv_color_black(), LV_OPA_TRANSP);
    const auto t_fill = us_since(render_start);

    auto mark = clk::now();
    lv_layer_t glayer;
    lv_canvas_init_layer(graph->gradient_canvas, &glayer);
    const auto t_init = us_since(mark);

    mark = clk::now();
    gradient_render_columns(graph, g, &glayer, 0, 0); // buffer-relative coords
    const auto t_cols = us_since(mark);

    // finish_layer runs a nested synchronous draw dispatch, so it is a genuine
    // suspect for the bulk of the cost rather than mere bookkeeping.
    mark = clk::now();
    lv_canvas_finish_layer(graph->gradient_canvas, &glayer);
    const auto t_finish = us_since(mark);

    graph->gradient_cache_dirty = false;
    graph->gradient_cache_sig = sig;
    graph->gradient_cache_sig_valid = true;

    const auto render_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - render_start)
                               .count();
    graph->gradient_render_us_total += static_cast<uint64_t>(render_us);
    ++graph->gradient_render_count;
    // debug, not info: the measurement is done (K2 Plus: 167ms median before,
    // ~10ms after), and a line every couple of seconds on every printer forever
    // is not worth it. HELIX_TEMP_GRAPH_GRAD_SKIP=0 plus -vv brings it back.
    spdlog::debug("[TempGraph {}] gradient render #{} ({}x{}, {} series) {}us "
                  "[fill {}us init {}us cols {}us finish {}us] [skipped {}, mean {}us]",
                  static_cast<const void*>(graph), graph->gradient_render_count, g.cw, g.ch,
                  graph->series_count, render_us, t_fill, t_init, t_cols, t_finish,
                  graph->gradient_skip_count,
                  graph->gradient_render_us_total / graph->gradient_render_count);

    // The buffer changed — request a redraw so the draw cb blits the fresh pixels.
    lv_obj_invalidate(graph->chart);
}

// Render gradient fills by reading chart data directly.
// LVGL 9.5 changed DRAW_TASK_ADDED to fire during rendering (after all draw events),
// so we can no longer add draw tasks from that callback. Instead, we compute pixel
// positions from the raw chart series data and draw gradient fills in DRAW_MAIN_END.
static void draw_gradient_cb(lv_event_t* e) {
    ui_temp_graph_t* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));
    if (!graph || !graph->chart || graph->series_count == 0)
        return;

    if (!(graph->features & TEMP_GRAPH_FEATURE_GRADIENTS))
        return;

    // Disable gradients when too many series are visible (visual clutter)
    int visible_count = 0;
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (graph->series_meta[i].chart_series && graph->series_meta[i].visible)
            visible_count++;
    }
    if (visible_count > 3)
        return;

    lv_layer_t* event_layer = lv_event_get_layer(e);
    if (!event_layer)
        return;

    temp_graph_geometry_t g;
    if (!temp_graph_compute_geometry(graph, &g))
        return;

    // Fast path: the cached buffer is valid for the current viewport and clean.
    // Blit it straight onto the event layer (cheap — no per-column software fill).
    bool cache_ready = graph->gradient_cache_buf && graph->gradient_cache_w == g.cw &&
                       graph->gradient_cache_h == g.ch && !graph->gradient_cache_dirty;

    if (cache_ready) {
        lv_draw_image_dsc_t idsc;
        lv_draw_image_dsc_init(&idsc);
        idsc.src = graph->gradient_cache_buf;
        idsc.opa = LV_OPA_COVER;
        lv_area_t dst = {g.cx1, g.cy1, g.cx1 + g.cw - 1, g.cy1 + g.ch - 1};
        lv_draw_image(event_layer, &idsc, &dst);

        static uint64_t s_gradient_blit_count = 0;
        spdlog::trace("[TempGraph] gradient blit #{}", ++s_gradient_blit_count);
        return;
    }

    // Cache is stale, mis-sized, or not yet built. Draw straight into the event
    // layer this frame so the gradient is never invisible (the proven direct path,
    // L079), and schedule an out-of-render-pass recompute so subsequent frames hit
    // the cheap blit path above. This callback runs per drawn frame, so the
    // request is leading-edge: frames that arrive while a recompute is pending
    // ride the one already queued instead of allocating another timer for it.
    gradient_render_columns(graph, g, event_layer, g.cx1, g.cy1);
    graph->gradient_refresh.schedule_once([graph]() { gradient_recompute(graph); });
}

// Draw legend chips in the upper-left of the chart content area.
// Each chip is a semi-transparent rounded rectangle with a color swatch and series name.
// Only drawn when TEMP_GRAPH_FEATURE_LEGEND is enabled (rowspan >= 2 or colspan >= 2).
static void draw_legend_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    ui_temp_graph_t* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));
    if (!graph || !graph->chart)
        return;

    if (!(graph->features & TEMP_GRAPH_FEATURE_LEGEND))
        return;

    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer)
        return;

    // Count visible series — skip legend if only one series (no ambiguity)
    int visible_count = 0;
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (graph->series_meta[i].chart_series && graph->series_meta[i].visible)
            visible_count++;
    }
    if (visible_count <= 1)
        return;

    // Content area (inside padding)
    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    int32_t pad_left = lv_obj_get_style_pad_left(chart, LV_PART_MAIN);
    int32_t pad_top = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);
    int32_t pad_right = lv_obj_get_style_pad_right(chart, LV_PART_MAIN);
    int32_t content_x1 = coords.x1 + pad_left;
    int32_t content_x2 = coords.x2 - pad_right;
    int32_t content_y1 = coords.y1 + pad_top;

    // Layout constants
    const lv_font_t* font = theme_manager_get_font("font_xs");
    int32_t font_h = theme_manager_get_font_height(font);
    int32_t chip_h = font_h + 4;      // 2px vertical padding
    int32_t swatch_size = font_h - 2; // Color swatch square
    int32_t chip_pad_h = 4;           // Horizontal padding inside chip
    int32_t chip_gap = 3;             // Gap between chips
    int32_t chip_radius = chip_h / 2; // Fully rounded ends

    // Starting position: upper-left with small inset
    int32_t x_start = content_x1 + 4;
    int32_t x = x_start;
    int32_t y = content_y1 + 3;
    int32_t available_x_max = content_x2 - 4;

    // Worst-case overflow indicator width ("+N" with N up to visible_count).
    // Reserved when there could still be more chips after the one we're about
    // to draw, so we never strand the final chip without room for the +N pill.
    // The sizing pass uses a local buffer; the persistent overflow_buf below
    // is what we hand to lv_draw_label and must outlive deferred draw.
    char sizing_buf[16];
    std::snprintf(sizing_buf, sizeof(sizing_buf), "+%d", visible_count);
    lv_point_t overflow_txt_size;
    lv_text_get_size(&overflow_txt_size, sizing_buf, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t overflow_chip_w = chip_pad_h + overflow_txt_size.x + chip_pad_h;

    // Persistent string buffers for labels (LVGL may defer draw)
    static char legend_bufs[UI_TEMP_GRAPH_MAX_SERIES][32];
    int buf_idx = 0;

    int chips_remaining = visible_count;
    int chips_drawn = 0;

    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES && buf_idx < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[i];
        if (!meta->chart_series || !meta->visible)
            continue;

        // Copy name to persistent buffer
        strncpy(legend_bufs[buf_idx], meta->name, 31);
        legend_bufs[buf_idx][31] = '\0';

        // Measure text width
        lv_point_t txt_size;
        lv_text_get_size(&txt_size, legend_bufs[buf_idx], font, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        int32_t chip_w = chip_pad_h + swatch_size + 3 + txt_size.x + chip_pad_h;

        // Reserve room for the +N pill only if more chips would follow this one.
        bool more_after_this = chips_remaining > 1;
        int32_t budget = available_x_max - (more_after_this ? (overflow_chip_w + chip_gap) : 0);

        if (x + chip_w > budget) {
            // No room for this chip (plus a future +N if applicable). If we
            // haven't drawn any chip at all, the legend can't fit even one —
            // skip rendering entirely rather than show only a "+N" with no
            // context. Otherwise draw a +N pill at the current x for the
            // remaining (including current) chips.
            if (chips_drawn == 0)
                return;
            // Static buffer because lv_draw_label may run the draw step
            // asynchronously and needs the label text to outlive this scope.
            static char overflow_buf[16];
            std::snprintf(overflow_buf, sizeof(overflow_buf), "+%d", chips_remaining);
            lv_point_t ov_size;
            lv_text_get_size(&ov_size, overflow_buf, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int32_t ov_w = chip_pad_h + ov_size.x + chip_pad_h;

            lv_draw_rect_dsc_t r_dsc;
            lv_draw_rect_dsc_init(&r_dsc);
            r_dsc.bg_color = graph->cached_graph_bg;
            r_dsc.bg_opa = LV_OPA_70;
            r_dsc.radius = chip_radius;
            lv_area_t r_area;
            r_area.x1 = x;
            r_area.y1 = y;
            r_area.x2 = x + ov_w;
            r_area.y2 = y + chip_h;
            lv_draw_rect(layer, &r_dsc, &r_area);

            lv_draw_label_dsc_t l_dsc;
            lv_draw_label_dsc_init(&l_dsc);
            l_dsc.color = lv_obj_get_style_text_color(chart, LV_PART_MAIN);
            l_dsc.font = font;
            l_dsc.opa = LV_OPA_80;
            l_dsc.align = LV_TEXT_ALIGN_CENTER;
            l_dsc.text = overflow_buf;
            lv_area_t l_area;
            l_area.x1 = x + chip_pad_h;
            l_area.y1 = y + (chip_h - font_h) / 2;
            l_area.x2 = x + ov_w - chip_pad_h;
            l_area.y2 = l_area.y1 + font_h;
            lv_draw_label(layer, &l_dsc, &l_area);
            return;
        }

        // Draw chip background (semi-transparent dark)
        lv_draw_rect_dsc_t rect_dsc;
        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_color = graph->cached_graph_bg;
        rect_dsc.bg_opa = LV_OPA_70;
        rect_dsc.radius = chip_radius;

        lv_area_t chip_area;
        chip_area.x1 = x;
        chip_area.y1 = y;
        chip_area.x2 = x + chip_w;
        chip_area.y2 = y + chip_h;
        lv_draw_rect(layer, &rect_dsc, &chip_area);

        // Draw color swatch (small filled circle)
        lv_draw_rect_dsc_t swatch_dsc;
        lv_draw_rect_dsc_init(&swatch_dsc);
        swatch_dsc.bg_color = meta->color;
        swatch_dsc.bg_opa = LV_OPA_COVER;
        swatch_dsc.radius = LV_RADIUS_CIRCLE;

        lv_area_t swatch_area;
        swatch_area.x1 = x + chip_pad_h;
        swatch_area.y1 = y + (chip_h - swatch_size) / 2;
        swatch_area.x2 = swatch_area.x1 + swatch_size;
        swatch_area.y2 = swatch_area.y1 + swatch_size;
        lv_draw_rect(layer, &swatch_dsc, &swatch_area);

        // Draw series name label
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = lv_obj_get_style_text_color(chart, LV_PART_MAIN);
        label_dsc.font = font;
        label_dsc.opa = LV_OPA_80;
        label_dsc.align = LV_TEXT_ALIGN_LEFT;
        label_dsc.text = legend_bufs[buf_idx];

        lv_area_t label_area;
        label_area.x1 = swatch_area.x2 + 3;
        label_area.y1 = y + (chip_h - font_h) / 2;
        label_area.x2 = x + chip_w - chip_pad_h;
        label_area.y2 = label_area.y1 + font_h;
        lv_draw_label(layer, &label_dsc, &label_area);

        // Advance to next chip position (horizontal flow)
        x += chip_w + chip_gap;
        buf_idx++;
        chips_drawn++;
        chips_remaining--;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared plot geometry — one definition for every draw hook
// ─────────────────────────────────────────────────────────────────────────────

// The chart's plot rectangle in absolute screen coordinates: the area inside
// the widget's padding that every hook interpolates data positions across.
//
// `width` is deliberately the x2 - x1 SPAN, not lv_obj_get_content_width().
// Those differ by one pixel (plus borders) because lv_area coordinates are
// inclusive, and interpolating `content_x1 + offset * width / total` is only
// exact when width is the span — offset == total then lands precisely on
// content_x2. The X-axis labels used to compute their own width the other way,
// which is why the grid lines and the times they were supposed to be aligned
// with sat 1-2px apart (#1245).
struct temp_graph_plot_area_t {
    int32_t x1;
    int32_t x2;
    int32_t y1;
    int32_t y2;
    int32_t width;
    int32_t height;
};

static temp_graph_plot_area_t temp_graph_plot_area(lv_obj_t* chart) {
    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);

    temp_graph_plot_area_t a;
    a.x1 = coords.x1 + lv_obj_get_style_pad_left(chart, LV_PART_MAIN);
    a.x2 = coords.x2 - lv_obj_get_style_pad_right(chart, LV_PART_MAIN);
    a.y1 = coords.y1 + lv_obj_get_style_pad_top(chart, LV_PART_MAIN);
    a.y2 = coords.y2 - lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);
    a.width = a.x2 - a.x1;
    a.height = a.y2 - a.y1;
    return a;
}

// Both used to derive this independently; keeping one source is what actually
// guarantees a grid line under every label. See temp_graph_internal.h for the
// struct's doc comment — shared with the hit test.
namespace helix::temp_graph_internal {

temp_graph_time_axis_t temp_graph_time_axis(const ui_temp_graph_t* graph) {
    temp_graph_time_axis_t t;
    t.total_ms =
        static_cast<int64_t>(graph->point_count) * UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC * 1000;
    t.latest_ms = graph->latest_point_time_ms;
    t.leftmost_ms = t.latest_ms - t.total_ms;

    // 400 points x 3 s = a 20 min window, labelled every 5 min (#979).
    t.interval_ms = 5 * 60 * 1000;
    if (t.total_ms < 2 * 60 * 1000) {
        t.interval_ms = 30 * 1000;
    } else if (t.total_ms < 10 * 60 * 1000) {
        t.interval_ms = 2 * 60 * 1000;
    }

    t.first_ms = (t.leftmost_ms / t.interval_ms) * t.interval_ms;
    if (t.first_ms < t.leftmost_ms) {
        t.first_ms += t.interval_ms;
    }
    return t;
}

} // namespace helix::temp_graph_internal

// Draw target lines: time-varying step trace (TARGET_HISTORY on, default) or
// a single horizontal dashed line at the current setpoint (TARGET_HISTORY off).
// Constrained to the content area — LVGL's built-in cursor drawing extends
// across the full widget bounds (including Y-axis labels), so we hide those
// and draw our own.
static void draw_target_lines_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    ui_temp_graph_t* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));
    if (!graph || !graph->chart)
        return;

    if (!(graph->features & TEMP_GRAPH_FEATURE_TARGET_LINES))
        return;

    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer)
        return;

    // Content area (inside padding)
    const temp_graph_plot_area_t plot = temp_graph_plot_area(chart);
    const int32_t cx1 = plot.x1;
    const int32_t cx2 = plot.x2;
    const int32_t cy1 = plot.y1;
    const int32_t cy2 = plot.y2;
    const int32_t chart_width = plot.width;
    const int32_t chart_height = plot.height;
    if (chart_height <= 0 || chart_width <= 0)
        return;

    bool history_mode = (graph->features & TEMP_GRAPH_FEATURE_TARGET_HISTORY) != 0;

    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[i];
        if (!meta->chart_series || !meta->visible || !meta->show_target)
            continue;

        // Dashed target trace is intentionally subdued (~70% transparent) so it
        // reads as background context, not a peer of the actuals polyline. The
        // accent tick at the current setpoint stays full-color for emphasis.
        lv_color_t muted = mute_color(meta->color, LV_OPA_30, graph->cached_graph_bg);

        // -----------------------------------------------------------------
        // Legacy horizontal-line mode (fallback when TARGET_HISTORY is off)
        // -----------------------------------------------------------------
        if (!history_mode) {
            int32_t content_y =
                chart_height - lv_map(static_cast<int32_t>(meta->target_temp * TEMP_SCALE),
                                      static_cast<int32_t>(graph->min_temp * TEMP_SCALE),
                                      static_cast<int32_t>(graph->max_temp * TEMP_SCALE), 0,
                                      chart_height);
            int32_t abs_y = cy1 + content_y;
            if (abs_y < cy1 || abs_y > cy2)
                continue;

            lv_draw_line_dsc_t line_dsc;
            lv_draw_line_dsc_init(&line_dsc);
            line_dsc.base.layer = layer;
            line_dsc.color = muted;
            line_dsc.width = 1;
            line_dsc.dash_width = 6;
            line_dsc.dash_gap = 4;
            line_dsc.p1.x = cx1;
            line_dsc.p1.y = abs_y;
            line_dsc.p2.x = cx2;
            line_dsc.p2.y = abs_y;
            lv_draw_line(layer, &line_dsc);
            continue;
        }

        // -----------------------------------------------------------------
        // History mode — polyline of target_deci_buf samples with breaks
        // -----------------------------------------------------------------
        if (!meta->target_deci_buf || meta->target_head <= 0)
            continue;

        // Project buffer index → pixel X. The chart in shift-mode renders the
        // most recent sample at the right edge; until the buffer is full, the
        // valid samples occupy visual positions [point_count - target_head,
        // point_count - 1]. Our buffer stores left-aligned (index 0 = oldest),
        // so map buffer index j to visual position (point_count - target_head + j)
        // and project that to a pixel X.
        const int head = meta->target_head;
        const int visual_offset = graph->point_count - head;
        auto x_for_index = [&](int idx) -> int32_t {
            if (graph->point_count <= 1)
                return cx1 + chart_width / 2;
            int visual_idx = visual_offset + idx;
            return cx1 + (visual_idx * (chart_width - 1)) / (graph->point_count - 1);
        };

        auto y_for_deci = [&](int16_t deci) -> int32_t {
            int32_t content_y =
                chart_height - lv_map(static_cast<int32_t>(deci),
                                      static_cast<int32_t>(graph->min_temp * TEMP_SCALE),
                                      static_cast<int32_t>(graph->max_temp * TEMP_SCALE), 0,
                                      chart_height);
            return cy1 + content_y;
        };

        // Walk segments and draw dashed line between consecutive points within a segment.
        lv_draw_line_dsc_t seg_dsc;
        lv_draw_line_dsc_init(&seg_dsc);
        seg_dsc.base.layer = layer;
        seg_dsc.color = muted;
        seg_dsc.width = 1;
        seg_dsc.dash_width = 6;
        seg_dsc.dash_gap = 4;

        auto segments = helix::temp_graph_internal::segment_target_buf(meta->target_deci_buf,
                                                                       meta->target_head);

        for (const auto& seg : segments) {
            // Vertical riser at segment start (target transitioned from 0 → value).
            // Skip when the segment starts at buffer index 0 — that's the oldest
            // visible sample, and we don't know whether target was 0 or 'value' before
            // the visible window started. Drawing a riser there would be a lie.
            if (seg.first > 0) {
                int32_t riser_x = x_for_index(seg.first);
                int32_t riser_y_top = y_for_deci(meta->target_deci_buf[seg.first]);
                int32_t riser_y_bottom = y_for_deci(0); // baseline at 0°C
                if (riser_y_bottom > cy2)
                    riser_y_bottom = cy2;
                if (riser_y_top < cy1)
                    riser_y_top = cy1;
                seg_dsc.p1.x = riser_x;
                seg_dsc.p1.y = riser_y_bottom;
                seg_dsc.p2.x = riser_x;
                seg_dsc.p2.y = riser_y_top;
                lv_draw_line(layer, &seg_dsc);
            }

            // Step segments between samples, with flat runs coalesced into a
            // single line so a held setpoint draws one dashed line instead of
            // one sub-pixel line per sample (#979 — the per-frame cost that
            // froze the touch UI on slow 32-bit boards). Geometry is identical.
            auto runs = helix::temp_graph_internal::coalesce_target_runs(meta->target_deci_buf,
                                                                         seg.first, seg.second);
            for (const auto& run : runs) {
                seg_dsc.p1.x = x_for_index(run.first);
                seg_dsc.p1.y = y_for_deci(meta->target_deci_buf[run.first]);
                seg_dsc.p2.x = x_for_index(run.second);
                seg_dsc.p2.y = y_for_deci(meta->target_deci_buf[run.second]);
                lv_draw_line(layer, &seg_dsc);
            }
        }

        // Accent tick: short solid horizontal hash in the series' full color
        // at the rightmost sample with target > 0. Communicates "this is the
        // current setpoint" without re-introducing a full-width horizontal line.
        if (!segments.empty()) {
            int last_idx = segments.back().second - 1;
            int16_t last_deci = meta->target_deci_buf[last_idx];
            int32_t tick_y = y_for_deci(last_deci);
            int32_t tick_x = x_for_index(last_idx);
            if (tick_y >= cy1 && tick_y <= cy2) {
                lv_draw_line_dsc_t tick_dsc;
                lv_draw_line_dsc_init(&tick_dsc);
                tick_dsc.base.layer = layer;
                tick_dsc.color = meta->color; // full color, not muted
                tick_dsc.width = 2;
                tick_dsc.p1.x = tick_x - 2;
                tick_dsc.p1.y = tick_y;
                tick_dsc.p2.x = tick_x + 2;
                tick_dsc.p2.y = tick_y;
                lv_draw_line(layer, &tick_dsc);
            }
        }
    }
}

// Draw X-axis time labels (rendered directly on graph canvas)
// Uses LV_EVENT_DRAW_POST to draw after chart content is rendered
static void draw_x_axis_labels_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    ui_temp_graph_t* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));

    if (!layer || !graph || !graph->show_x_axis || graph->visible_point_count == 0) {
        return; // No data to label yet or X-axis disabled
    }

    spdlog::trace("[TempGraph] Drawing X-axis labels: {} points, first={}ms, latest={}ms",
                  graph->visible_point_count, graph->first_point_time_ms,
                  graph->latest_point_time_ms);

    // Content area (inside padding) — shared with the grid callback so labels
    // and grid lines land on the same pixel.
    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    const temp_graph_plot_area_t plot = temp_graph_plot_area(chart);
    const int32_t content_width = plot.width;
    const int32_t content_x1 = plot.x1;
    const int32_t content_x2 = plot.x2;

    // Setup label descriptor - match Y-axis label style exactly
    // Y-axis labels use configurable font and get their color from LVGL's theme default
    // We get the text color from the chart's LV_PART_TICKS style (used for axis labels)
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_obj_get_style_text_color(chart, LV_PART_MAIN); // Use chart's text color
    label_dsc.font = graph->axis_font;                                  // Configurable axis font
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    label_dsc.opa = lv_obj_get_style_text_opa(chart, LV_PART_MAIN); // Use chart's text opacity

    // Time axis — shared with the grid callback (see temp_graph_time_axis).
    const temp_graph_time_axis_t axis = temp_graph_time_axis(graph);
    const int64_t total_display_time_ms = axis.total_ms;
    const int64_t latest_ms = axis.latest_ms;
    const int64_t leftmost_ms = axis.leftmost_ms;

    // Label positioning: Y is aligned with bottom Y-axis label (0° baseline)
    // The Y-axis labels use space_between layout, with 0° at the chart content bottom
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);
    int32_t label_height =
        theme_manager_get_font_height(graph->axis_font); // Configurable axis font
    // Add small gap between chart content and X-axis labels
    int32_t space_xs = theme_manager_get_spacing("space_xs"); // 4/5/6px gap
    // Position below chart content with responsive gap
    int32_t label_y = coords.y2 - pad_bottom + space_xs;

    const int64_t label_interval_ms = axis.interval_ms;

    // Track previous label to skip duplicates
    char prev_label[12] = ""; // Sized for 12H format: "12:30 PM"

    // Draw labels at regular time intervals, starting from the first tick on a
    // nice boundary at or after the left edge.
    for (int64_t label_time_ms = axis.first_ms; label_time_ms <= latest_ms;
         label_time_ms += label_interval_ms) {
        // Calculate X position for this time
        // Position is proportional: (time - leftmost) / total_display_time * width
        int64_t time_offset = label_time_ms - leftmost_ms;
        int32_t label_x = content_x1 + static_cast<int32_t>((time_offset * content_width) /
                                                            total_display_time_ms);

        // Skip if outside chart bounds
        if (label_x < content_x1 || label_x > content_x2) {
            continue;
        }

        // Format time via central formatter (handles 12H/24H, leading zero strip)
        time_t time_sec = static_cast<time_t>(label_time_ms / 1000);
        struct tm* tm_info = localtime(&time_sec);
        std::string formatted = format_time(tm_info);
        // Copy to static buffer — LVGL draw tasks need persistent string pointers
        static char time_str_buf[8][12]; // 8 labels max, 12 chars each
        static int time_str_idx = 0;
        char* time_str = time_str_buf[time_str_idx++ % 8];
        strncpy(time_str, formatted.c_str(), 11);
        time_str[11] = '\0';

        // Skip duplicate labels (same HH:MM)
        if (strcmp(time_str, prev_label) == 0) {
            continue;
        }
        strncpy(prev_label, time_str, sizeof(prev_label) - 1);

        // Create label area (centered on label_x)
        // Sized for 12H format like "12:30 PM" (wider than 24H "14:30")
        lv_area_t label_area;
        label_area.x1 = label_x - 40; // 80px width, centered (fits "12:30 PM")
        label_area.y1 = label_y;
        label_area.x2 = label_x + 40;
        label_area.y2 = label_y + label_height;

        label_dsc.text = time_str;
        lv_draw_label(layer, &label_dsc, &label_area);
    }

    // Show "now" label at rightmost edge ONLY when chart is reasonably full
    // (at least 80% of points have data) - prevents overlap with time-based labels
    if (graph->visible_point_count >= (graph->point_count * 4 / 5)) {
        time_t now_sec = static_cast<time_t>(latest_ms / 1000);
        struct tm* tm_info = localtime(&now_sec);
        std::string now_formatted = format_time(tm_info);
        static char now_str[12];
        strncpy(now_str, now_formatted.c_str(), 11);
        now_str[11] = '\0';

        // Only draw if different from last label
        if (strcmp(now_str, prev_label) != 0) {
            // Sized for 12H format like "12:30 PM" (wider than 24H "14:30")
            lv_area_t label_area;
            label_area.x1 = content_x2 - 44; // 80px width, right-aligned
            label_area.y1 = label_y;
            label_area.x2 = content_x2 + 36;
            label_area.y2 = label_y + label_height;

            label_dsc.text = now_str;
            label_dsc.align = LV_TEXT_ALIGN_RIGHT; // Right-align the "now" label
            lv_draw_label(layer, &label_dsc, &label_area);
        }
    }
}

// Draw custom grid lines constrained to content area (not extending into label areas)
// Uses LV_EVENT_DRAW_MAIN to draw before chart content
static void draw_grid_lines_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    ui_temp_graph_t* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));

    if (!layer || !graph) {
        return;
    }

    // Content area (where data is drawn, excluding label areas) — shared with
    // the X-axis label callback so the vertical lines and the times they mark
    // land on the same pixel.
    const temp_graph_plot_area_t plot = temp_graph_plot_area(chart);
    const int32_t content_x1 = plot.x1;
    const int32_t content_x2 = plot.x2;
    const int32_t content_y1 = plot.y1;
    const int32_t content_y2 = plot.y2;
    const int32_t content_width = plot.width;
    const int32_t content_height = plot.height;

    if (content_width <= 0 || content_height <= 0) {
        return; // Chart not laid out yet
    }

    // Setup line style - use explicit theme token for consistent grid appearance
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = graph->cached_grid_color;
    line_dsc.width = 1;
    line_dsc.opa = LV_OPA_30;

    // Draw horizontal grid lines (5 lines = 4 divisions)
    constexpr int H_DIVISIONS = 5;
    for (int i = 0; i <= H_DIVISIONS; i++) {
        int32_t y = content_y1 + (content_height * i) / H_DIVISIONS;
        line_dsc.p1.x = content_x1;
        line_dsc.p1.y = y;
        line_dsc.p2.x = content_x2;
        line_dsc.p2.y = y;
        lv_draw_line(layer, &line_dsc);
    }

    // Vertical grid lines sit at TIME-BASED positions from the same axis the
    // X-axis labels use, so a line lands under every label (#1245).
    //
    // Nothing plotted yet means latest_point_time_ms is still 0, and the ticks
    // would be walked from the Unix epoch — the same reason the label callback
    // bails above. Skip the vertical lines; the horizontal ones above are
    // time-independent and still draw.
    if (graph->visible_point_count == 0) {
        return;
    }

    const temp_graph_time_axis_t axis = temp_graph_time_axis(graph);

    for (int64_t line_time_ms = axis.first_ms; line_time_ms <= axis.latest_ms;
         line_time_ms += axis.interval_ms) {
        int64_t time_offset = line_time_ms - axis.leftmost_ms;
        int32_t x =
            content_x1 + static_cast<int32_t>((time_offset * content_width) / axis.total_ms);

        if (x < content_x1 || x > content_x2)
            continue;

        line_dsc.p1.x = x;
        line_dsc.p1.y = content_y1;
        line_dsc.p2.x = x;
        line_dsc.p2.y = content_y2;
        lv_draw_line(layer, &line_dsc);
    }
}

// Draw Y-axis temperature labels (rendered directly on graph canvas)
// Uses LV_EVENT_DRAW_POST to draw after chart content is rendered
static void draw_y_axis_labels_cb(lv_event_t* e) {
    lv_obj_t* chart = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    ui_temp_graph_t* graph = static_cast<ui_temp_graph_t*>(lv_event_get_user_data(e));

    if (!layer || !graph || !graph->show_y_axis || graph->y_axis_increment <= 0) {
        return; // Y-axis labels disabled or invalid config
    }

    // Get chart bounds and content area
    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    int32_t pad_top = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);

    // Chart content area (where data is drawn) — read actual padding from style
    // so this stays in sync with set_axis_size() and set_features() adjustments
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);
    int32_t content_top = coords.y1 + pad_top;
    int32_t content_bottom = coords.y2 - pad_bottom;
    int32_t content_height = content_bottom - content_top;

    // Setup label descriptor - same style as X-axis
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_obj_get_style_text_color(chart, LV_PART_MAIN);
    label_dsc.font = graph->axis_font;     // Configurable axis font
    label_dsc.align = LV_TEXT_ALIGN_RIGHT; // Right-align Y-axis labels
    label_dsc.opa = lv_obj_get_style_text_opa(chart, LV_PART_MAIN);

    // Y-axis label dimensions (for positioning)
    int32_t label_height = theme_manager_get_font_height(graph->axis_font);
    int32_t label_width = graph->y_axis_width; // Use configured width

    // Temperature range
    float temp_range = graph->max_temp - graph->min_temp;
    if (temp_range <= 0)
        return;

    // Draw labels at each temperature increment
    // Use static buffer array - LVGL may defer draw and need persistent strings
    static char temp_str_buf[8][8]; // 8 labels max (0, 80, 160, 240, 320 = 5 for nozzle)
    static int temp_str_idx = 0;
    temp_str_idx = 0; // Reset each draw cycle

    for (float temp = graph->min_temp; temp <= graph->max_temp; temp += graph->y_axis_increment) {
        // Calculate Y position: (max_temp - temp) / range * height
        // Top = max_temp, Bottom = min_temp
        float temp_fraction = (graph->max_temp - temp) / temp_range;
        int32_t label_y = content_top + static_cast<int32_t>(temp_fraction * content_height);

        // Center label vertically on the temperature line
        label_y -= label_height / 2;

        // Clamp so labels don't extend above or below the chart area
        if (label_y < coords.y1)
            label_y = coords.y1;
        if (label_y + label_height > coords.y2)
            continue; // Skip if below chart

        // Format temperature string into persistent buffer
        char* temp_str = temp_str_buf[temp_str_idx++ % 8];
        snprintf(temp_str, 8, "%d°", static_cast<int>(temp));

        // Draw label in left padding area (to the left of chart content)
        lv_area_t label_area;
        label_area.x1 = coords.x1;
        label_area.y1 = label_y;
        label_area.x2 = coords.x1 + label_width;
        label_area.y2 = label_y + label_height;

        label_dsc.text = temp_str;
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

// Theme change callback: re-apply chart colors when theme toggles
static void theme_change_cb(lv_observer_t* observer, lv_subject_t* subject) {
    (void)subject;
    auto* graph = static_cast<ui_temp_graph_t*>(lv_observer_get_user_data(observer));
    if (!graph || !graph->chart) {
        return;
    }

    // Refresh cached theme colors for draw callbacks
    graph->cached_grid_color = theme_manager_get_color("elevated_bg");
    graph->cached_graph_bg = theme_manager_get_color("screen_bg");

    // Series gradient colors derive from the theme — recompute the cached fill.
    mark_gradient_cache_dirty(graph);

    // Re-apply themed background color
    lv_obj_set_style_bg_color(graph->chart, graph->cached_graph_bg, LV_PART_MAIN);

    // Re-apply themed text color for axis labels
    lv_obj_set_style_text_color(graph->chart, theme_manager_get_color("text"), LV_PART_MAIN);

    // Force full redraw so draw callbacks (grid, axis labels, gradients) pick up new colors
    lv_obj_invalidate(graph->chart);

    spdlog::debug("[TempGraph] Updated colors on theme change");
}

// Create temperature graph widget
ui_temp_graph_t* ui_temp_graph_create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[TempGraph] NULL parent");
        return nullptr;
    }

    // Defense-in-depth: verify parent is a live LVGL object.
    // Catches stale/freed container pointers from deferred callbacks that race
    // with panel teardown (see #674, #675, #676).
    if (!lv_obj_is_valid(parent)) {
        spdlog::error("[TempGraph] Parent object is freed or corrupt ({:#x})",
                      reinterpret_cast<uintptr_t>(parent));
        return nullptr;
    }

    // Allocate graph structure using RAII
    auto graph_ptr = std::make_unique<ui_temp_graph_t>();
    if (!graph_ptr) {
        spdlog::error("[TempGraph] Failed to allocate graph structure");
        return nullptr;
    }

    // make_unique value-initializes, which zeroes every trivial member before
    // running the members that have constructors. Blanket-memsetting the result
    // would overwrite those constructed members.
    ui_temp_graph_t* graph = graph_ptr.get();

    // Initialize defaults
    graph->point_count = UI_TEMP_GRAPH_DEFAULT_POINTS;
    graph->min_temp = UI_TEMP_GRAPH_DEFAULT_MIN_TEMP;
    graph->max_temp = UI_TEMP_GRAPH_DEFAULT_MAX_TEMP;
    graph->series_count = 0;
    graph->next_series_id = 0;
    graph->features =
        TEMP_GRAPH_ALL_FEATURES & ~TEMP_GRAPH_FEATURE_Y_AXIS; // Y-axis off until caller configures
    graph->y_axis_increment = 0; // Disabled by default (caller must enable)
    graph->show_y_axis = false;
    graph->show_x_axis = true;
    graph->max_visible_temp = graph->min_temp + 1.0f; // Initialize to avoid zero gradient span
    graph->axis_font = theme_manager_get_font("font_small"); // Default axis label font
    graph->y_axis_width = 40;                                // Default Y-axis label width

    // Gradient cache (#979): zero-init the offscreen render target; first draw
    // lazily creates the canvas/buffer and computes the gradient once.
    graph->gradient_canvas = nullptr;
    graph->gradient_cache_buf = nullptr;
    graph->gradient_cache_w = 0;
    graph->gradient_cache_h = 0;
    graph->gradient_cache_dirty = true;

    // Create LVGL chart
    graph->chart = lv_chart_create(parent);
    if (!graph->chart) {
        spdlog::error("[TempGraph] Failed to create chart widget");
        return nullptr; // graph_ptr auto-freed
    }

    // Configure chart
    lv_chart_set_type(graph->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(graph->chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(graph->chart, static_cast<uint32_t>(graph->point_count));

    // Set Y-axis range (in deci-degrees for 0.1°C precision)
    lv_chart_set_axis_range(graph->chart, LV_CHART_AXIS_PRIMARY_Y,
                            static_cast<int32_t>(graph->min_temp * TEMP_SCALE),
                            static_cast<int32_t>(graph->max_temp * TEMP_SCALE));

    // Cache theme colors for draw callbacks (avoid per-frame theme lookups)
    graph->cached_grid_color = theme_manager_get_color("elevated_bg");
    graph->cached_graph_bg = theme_manager_get_color("screen_bg");

    // Style chart background (theme handles colors)
    lv_obj_set_style_bg_opa(graph->chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(graph->chart, graph->cached_graph_bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(graph->chart, 0, LV_PART_MAIN);
    // Use responsive spacing from theme constants
    int32_t space_md = theme_manager_get_spacing("space_md"); // 8/10/12px
    int32_t space_xs = theme_manager_get_spacing("space_xs"); // 4/5/6px for axis label gaps
    int32_t label_height = theme_manager_get_font_height(theme_manager_get_font("font_small"));
    int32_t y_axis_label_width = 40; // Width for Y-axis labels (fits "320°")

    lv_obj_set_style_pad_top(graph->chart, space_md, LV_PART_MAIN);
    lv_obj_set_style_pad_right(graph->chart, space_md, LV_PART_MAIN);
    // Extra left padding for Y-axis labels: label width + gap
    lv_obj_set_style_pad_left(graph->chart, y_axis_label_width + space_xs, LV_PART_MAIN);
    // Extra bottom padding for X-axis time labels: gap + label height
    // Use space_md for larger gap to accommodate 12-hour AM/PM format labels
    int32_t space_sm = theme_manager_get_spacing("space_sm"); // 6/8/10px
    lv_obj_set_style_pad_bottom(graph->chart, space_sm + label_height + space_md, LV_PART_MAIN);

    // Style division lines (theme handles colors)
    lv_obj_set_style_line_width(graph->chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_opa(graph->chart, LV_OPA_30, LV_PART_MAIN); // Subtle - 30% opacity

    // Style data series lines
    lv_obj_set_style_line_width(graph->chart, 2, LV_PART_ITEMS);          // Series line thickness
    lv_obj_set_style_line_opa(graph->chart, LV_OPA_COVER, LV_PART_ITEMS); // Full opacity for series

    // Hide point indicators (circles at each data point)
    lv_obj_set_style_width(graph->chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(graph->chart, 0, LV_PART_INDICATOR);

    // No LVGL cursors — we draw our own target lines in draw_target_lines_cb
    // constrained to the content area (LVGL cursors extend into Y-axis padding).

    // Disable LVGL's built-in division lines - we draw custom ones constrained to content area
    lv_chart_set_div_line_count(graph->chart, 0, 0);

    // Over-range masking: hide points exceeding Y-axis max during LVGL's line drawing
    // to prevent misleading clamped flat bars. Points are masked in DRAW_MAIN_BEGIN and
    // restored in DRAW_MAIN_END (before gradient callback) so gradients still fill to
    // chart top for over-range data — indicating temperature was present but off-scale.
    lv_obj_add_event_cb(graph->chart, mask_overrange_begin_cb, LV_EVENT_DRAW_MAIN_BEGIN, graph);
    lv_obj_add_event_cb(graph->chart, mask_overrange_end_cb, LV_EVENT_DRAW_MAIN_END, graph);

    // Gradient rendering: compute pixel positions from chart data in DRAW_MAIN_END.
    // Registered AFTER mask_overrange_end_cb so original values are restored before
    // gradient reads them — gradients will clamp to chart top (via lv_map) which is
    // the desired visual: gradient fills to top indicating off-scale data.
    lv_obj_add_event_cb(graph->chart, draw_gradient_cb, LV_EVENT_DRAW_MAIN_END, graph);

    // Store graph pointer in chart user data for retrieval
    lv_obj_set_user_data(graph->chart, graph);

    // Register delete callback to null out graph->chart on widget destruction
    lv_obj_add_event_cb(graph->chart, chart_delete_cb, LV_EVENT_DELETE, graph);

    // Register resize callback to recalculate value-based cursor positions
    lv_obj_add_event_cb(graph->chart, chart_resize_cb, LV_EVENT_SIZE_CHANGED, nullptr);

    // Register custom grid drawing callback (draws lines constrained to content area)
    lv_obj_add_event_cb(graph->chart, draw_grid_lines_cb, LV_EVENT_DRAW_MAIN, graph);

    // Register X-axis label draw callback (renders time labels directly on canvas)
    lv_obj_add_event_cb(graph->chart, draw_x_axis_labels_cb, LV_EVENT_DRAW_POST, graph);

    // Register Y-axis label draw callback (renders temperature labels directly on canvas)
    lv_obj_add_event_cb(graph->chart, draw_y_axis_labels_cb, LV_EVENT_DRAW_POST, graph);

    // Register custom target line draw callback (replaces LVGL's cursor rendering,
    // constrained to content area so lines don't bleed into Y-axis labels)
    lv_obj_add_event_cb(graph->chart, draw_target_lines_cb, LV_EVENT_DRAW_POST, graph);

    // Register legend draw callback (renders color-coded series chips in upper-left)
    lv_obj_add_event_cb(graph->chart, draw_legend_cb, LV_EVENT_DRAW_POST, graph);

    // Register tap-to-caption draw callback. Registered unconditionally (like the
    // other draw callbacks above) rather than gated on tooltip-enabled: it no-ops
    // whenever nothing is pinned, and nothing can be pinned while the tooltip is
    // disabled, so there is no behavior difference — only one fewer add/remove
    // pair to keep in sync with ui_temp_graph_set_tooltip_enabled.
    lv_obj_add_event_cb(graph->chart, helix::temp_graph_internal::temp_graph_tooltip_draw_cb,
                        LV_EVENT_DRAW_POST, graph);

    // Subscribe to theme changes for live color updates
    lv_subject_t* theme_subject = theme_manager_get_changed_subject();
    if (theme_subject) {
        // Tie observer to chart widget — auto-removed when chart is deleted
        graph->theme_observer =
            lv_subject_add_observer_obj(theme_subject, theme_change_cb, graph->chart, graph);
    }

    spdlog::debug("[TempGraph] Created: {} points, {:.0f}-{:.0f}°C range", graph->point_count,
                  graph->min_temp, graph->max_temp);

    // Transfer ownership to caller
    return graph_ptr.release();
}

// Destroy temperature graph widget
void ui_temp_graph_destroy(ui_temp_graph_t* graph) {
    if (!graph)
        return;

    // Unconditional — runs regardless of which teardown branch below fires.
    helix::temp_graph_internal::temp_graph_tooltip_destroy(graph);

    // Transfer ownership to RAII wrapper - automatic cleanup
    std::unique_ptr<ui_temp_graph_t> graph_ptr(graph);

    // Retract any pending out-of-render-pass gradient recompute. Without this it
    // could fire after the struct below is freed, a UAF. ~ui_temp_graph_t cancels
    // the timer as well; this is the explicit half, ahead of the teardown below.
    graph_ptr->gradient_refresh.cancel();

    // Drop this graph's over-range save slot so the keyed map doesn't retain a
    // stale key (the pointer is about to be freed).
    s_overrange_saved.erase(graph);

    // Only clean up series and chart if chart widget still exists.
    // Chart may already be deleted by LVGL parent cascade (chart_delete_cb nulls graph->chart).
    if (graph_ptr->chart) {
        lv_obj_t* chart = graph_ptr->chart;
        crash_handler::breadcrumb::note("tg", "destroy_async", reinterpret_cast<long>(chart));

        // Remove all series (chart-side cleanup).
        for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
            if (graph_ptr->series_meta[i].chart_series) {
                lv_chart_remove_series(chart, graph_ptr->series_meta[i].chart_series);
            }
        }

        // Sever every callback that captured `graph` as user_data so the deferred
        // chart deletion below cannot dereference graph after unique_ptr frees it.
        // chart_resize_cb is registered with nullptr user_data so it is safe to leave.
        lv_obj_remove_event_cb(chart, mask_overrange_begin_cb);
        lv_obj_remove_event_cb(chart, mask_overrange_end_cb);
        lv_obj_remove_event_cb(chart, draw_gradient_cb);
        lv_obj_remove_event_cb(chart, chart_delete_cb);
        lv_obj_remove_event_cb(chart, draw_grid_lines_cb);
        lv_obj_remove_event_cb(chart, draw_x_axis_labels_cb);
        lv_obj_remove_event_cb(chart, draw_y_axis_labels_cb);
        lv_obj_remove_event_cb(chart, draw_target_lines_cb);
        lv_obj_remove_event_cb(chart, draw_legend_cb);
        // temp_graph_tooltip_draw_cb and tooltip_press_cb are severed by
        // temp_graph_tooltip_destroy(), called unconditionally above — before this
        // block runs — so both are already gone here.
        lv_obj_set_user_data(chart, nullptr);

        // Theme observer's user_data points at `graph`; auto-removal does not fire
        // until the chart is actually deleted by the async path below, leaving a
        // window where a theme toggle would dereference freed graph. Remove now —
        // lv_observer_remove also strips the chart's unsubscribe_on_delete_cb, so
        // the eventual async deletion finds nothing to call.
        if (graph_ptr->theme_observer) {
            lv_observer_remove(graph_ptr->theme_observer);
            graph_ptr->theme_observer = nullptr;
        }

        // Free the gradient cache buffer now. The hidden gradient_canvas is a
        // child of the chart and will be removed by the async chart deletion
        // cascade below — just null our pointer so nothing dereferences it.
        graph_ptr->gradient_canvas = nullptr;
        if (graph_ptr->gradient_cache_buf) {
            lv_draw_buf_destroy(graph_ptr->gradient_cache_buf);
            graph_ptr->gradient_cache_buf = nullptr;
        }

        // Hide the chart before queuing the async delete: lv_refr's tree walk
        // skips LV_OBJ_FLAG_HIDDEN, so any display-refresh frame that fires
        // between now and lv_obj_delete_async_cb cannot send DRAW_POST_* events
        // into a chart whose per-instance event_list / class state we have
        // already torn down (bundle RP293UCW: SIGSEGV at lv_array_at NULL via
        // lv_event_send during render of an alive-but-stripped chart).
        lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);

        // Async deletion escapes any UpdateQueue::process_pending batch we may be
        // running inside (e.g., reconnect → connection_observer → rebuild → destroy).
        // Sync lv_obj_del here corrupts LVGL's global event list when other queued
        // entries follow — see L081 / prestonbrown/helixscreen#867 cluster.
        lv_obj_delete_async(chart);
    }

    // Free per-series target buffers unconditionally — runs even when the chart
    // was deleted by LVGL parent cascade before destroy() (chart_delete_cb null'd
    // graph->chart, so the if-block above is skipped). Without this the buffers
    // leak in the rebuild() path used by TempGraphController on reconnect.
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        delete[] graph_ptr->series_meta[i].target_deci_buf;
        graph_ptr->series_meta[i].target_deci_buf = nullptr;
    }

    // Defensive: free the gradient cache buffer if it somehow survived both the
    // chart-present path above and chart_delete_cb (e.g. destroy after a cascade
    // delete that didn't carry our user_data). Idempotent: pointer is nulled.
    if (graph_ptr->gradient_cache_buf) {
        lv_draw_buf_destroy(graph_ptr->gradient_cache_buf);
        graph_ptr->gradient_cache_buf = nullptr;
    }

    // graph_ptr automatically freed via ~unique_ptr()
    spdlog::trace("[TempGraph] Destroyed");
}

// Get underlying chart widget (nullptr if graph or chart was destroyed)
lv_obj_t* ui_temp_graph_get_chart(ui_temp_graph_t* graph) {
    return (graph && graph->chart) ? graph->chart : nullptr;
}

// Add a new temperature series
int ui_temp_graph_add_series(ui_temp_graph_t* graph, const char* name, lv_color_t color) {
    if (!graph || !graph->chart || !name) {
        spdlog::error("[TempGraph] NULL graph, chart, or name");
        return -1;
    }

    if (graph->series_count >= UI_TEMP_GRAPH_MAX_SERIES) {
        spdlog::error("[TempGraph] Maximum series count ({}) reached", UI_TEMP_GRAPH_MAX_SERIES);
        return -1;
    }

    // Find next available slot
    int slot = -1;
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        if (graph->series_meta[i].chart_series == nullptr) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        spdlog::error("[TempGraph] No available series slots");
        return -1;
    }

    // Create LVGL chart series
    lv_chart_series_t* ser = lv_chart_add_series(graph->chart, color, LV_CHART_AXIS_PRIMARY_Y);
    if (!ser) {
        spdlog::error("[TempGraph] Failed to create chart series");
        return -1;
    }

    // Initialize all points to POINT_NONE (no data) so empty chart doesn't show false history.
    // The gradient callback skips POINT_NONE values when iterating series data.
    lv_chart_set_all_values(graph->chart, ser, LV_CHART_POINT_NONE);

    // Initialize series metadata
    ui_temp_series_meta_t* meta = &graph->series_meta[slot];
    meta->id = graph->next_series_id++;
    meta->chart_series = ser;
    meta->color = color;
    strncpy(meta->name, name, sizeof(meta->name) - 1);
    meta->name[sizeof(meta->name) - 1] = '\0';
    meta->visible = true;
    meta->show_target = false;
    meta->target_temp = 0.0f;
    meta->gradient_bottom_opa = UI_TEMP_GRAPH_GRADIENT_BOTTOM_OPA;
    meta->gradient_top_opa = UI_TEMP_GRAPH_GRADIENT_TOP_OPA;
    meta->first_value_received = false;

    // Allocate target history buffer (zero-init = heater off sentinel)
    meta->target_deci_buf = new (std::nothrow) int16_t[static_cast<size_t>(graph->point_count)]();
    meta->target_head = 0;
    if (!meta->target_deci_buf) {
        spdlog::error("[TempGraph] Failed to allocate target buffer for series '{}'", name);
        lv_chart_remove_series(graph->chart, ser);
        memset(meta, 0, sizeof(ui_temp_series_meta_t));
        meta->chart_series = nullptr;
        return -1;
    }

    graph->series_count++;

    // New series introduces another gradient band.
    mark_gradient_cache_dirty(graph);

    spdlog::trace("[TempGraph] Added series {} '{}' (slot {}, color 0x{:06X})", meta->id,
                  meta->name, slot, lv_color_to_u32(color));

    return meta->id;
}

// Remove a temperature series
void ui_temp_graph_remove_series(ui_temp_graph_t* graph, int series_id) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    // Remove chart series
    lv_chart_remove_series(graph->chart, meta->chart_series);

    // Free target history buffer
    delete[] meta->target_deci_buf;

    // Clear metadata (also zeros target_deci_buf and target_head)
    memset(meta, 0, sizeof(ui_temp_series_meta_t));
    meta->chart_series = nullptr;

    graph->series_count--;

    // Removing a series drops its gradient band.
    mark_gradient_cache_dirty(graph);

    helix::temp_graph_internal::temp_graph_tooltip_on_series_hidden(graph, series_id);

    spdlog::trace("[TempGraph] Removed series {} ({} series remaining)", series_id,
                  graph->series_count);
}

// Show or hide a series
void ui_temp_graph_show_series(ui_temp_graph_t* graph, int series_id, bool visible) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    meta->visible = visible;

    // Use LVGL's public API to hide/show series
    lv_chart_hide_series(graph->chart, meta->chart_series, !visible);

    // Visibility change adds/removes a series' gradient band.
    mark_gradient_cache_dirty(graph);

    if (!visible) {
        helix::temp_graph_internal::temp_graph_tooltip_on_series_hidden(graph, series_id);
    }

    lv_obj_invalidate(graph->chart);
    spdlog::trace("[TempGraph] Series {} '{}' {}", series_id, meta->name,
                  visible ? "shown" : "hidden");
}

// Helper: push current target into the parallel buffer in lockstep with the LVGL
// chart's actuals push. Uses a shift-left store (NOT LVGL's ring buffer) so the
// draw callback can walk target_deci_buf[0..target_head-1] linearly, oldest-to-newest,
// without tracking the chart's start_point offset. Called from update_series*
// immediately after lv_chart_set_next_value.
static void push_target_sample(ui_temp_graph_t* graph, ui_temp_series_meta_t* meta) {
    if (!meta->target_deci_buf || graph->point_count <= 0)
        return;
    int16_t v = static_cast<int16_t>(meta->target_temp * TEMP_SCALE);
    if (meta->target_head < graph->point_count) {
        meta->target_deci_buf[meta->target_head++] = v;
    } else {
        memmove(meta->target_deci_buf, meta->target_deci_buf + 1,
                static_cast<size_t>(graph->point_count - 1) * sizeof(int16_t));
        meta->target_deci_buf[graph->point_count - 1] = v;
    }
}

// Add a single temperature point (push mode)
void ui_temp_graph_update_series(ui_temp_graph_t* graph, int series_id, float temp) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    // Add point to series (shifts old data left, stored as deci-degrees)
    lv_chart_set_next_value(graph->chart, meta->chart_series,
                            static_cast<int32_t>(temp * TEMP_SCALE));

    helix::temp_graph_internal::temp_graph_tooltip_on_sample_pushed(graph, series_id);

    // Mirror the push into the parallel target buffer.
    push_target_sample(graph, meta);

    // Update max visible temperature for gradient rendering
    update_max_visible_temp(graph);

    // New point shifted the curve — gradient must recompute.
    mark_gradient_cache_dirty(graph);

    // Whole-object invalidate so the series line, gradient, axis labels and target
    // trace all repaint together. lv_chart_set_next_value already invalidates in
    // shift mode, but issuing the full-object invalidate LAST guarantees the chart's
    // entire area is the final queued dirty region for this frame — under
    // LV_DISPLAY_RENDER_MODE_PARTIAL a stale/partial intermediate dirty rect could
    // otherwise repaint only the axis-label strip and leave the plot frozen (#979).
    lv_obj_invalidate(graph->chart);
}

// Add temperature point with timestamp (for X-axis labels)
void ui_temp_graph_update_series_with_time(ui_temp_graph_t* graph, int series_id, float temp,
                                           int64_t timestamp_ms) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    // On first real value, backfill all points to avoid spike from 0/uninitialized
    // This makes the graph start at the actual temperature instead of showing a ramp from 0
    if (!meta->first_value_received) {
        meta->first_value_received = true;
        lv_chart_set_all_values(graph->chart, meta->chart_series,
                                static_cast<int32_t>(temp * TEMP_SCALE));
        spdlog::trace("[TempGraph] Series {} '{}' backfilled with initial temp {:.1f}°C", series_id,
                      meta->name, temp);
    }

    // Track timestamp for X-axis label rendering
    graph->latest_point_time_ms = timestamp_ms;
    graph->visible_point_count++;

    // When buffer is full, oldest point scrolls off - update first_point_time_ms
    // First point timestamp is latest - (display period) when full, or the first timestamp received
    if (graph->first_point_time_ms == 0) {
        graph->first_point_time_ms = timestamp_ms;
    } else if (graph->visible_point_count > graph->point_count) {
        // Buffer is full, oldest point scrolled off
        // First visible point is now: latest - (point_count - 1) samples back
        // At one sample per SAMPLE_INTERVAL_SEC, that's (point_count - 1) × interval
        graph->first_point_time_ms = timestamp_ms - static_cast<int64_t>(graph->point_count - 1) *
                                                        UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC * 1000;
    }

    // Add point to series (shifts old data left, stored as deci-degrees)
    lv_chart_set_next_value(graph->chart, meta->chart_series,
                            static_cast<int32_t>(temp * TEMP_SCALE));

    helix::temp_graph_internal::temp_graph_tooltip_on_sample_pushed(graph, series_id);

    // Mirror the push into the parallel target buffer.
    push_target_sample(graph, meta);

    // Update max visible temperature for gradient rendering
    update_max_visible_temp(graph);

    // New point shifted the curve — gradient must recompute.
    mark_gradient_cache_dirty(graph);

    // Whole-object invalidate so the series line, gradient, axis labels and target
    // trace all repaint together as the final queued dirty region for this frame.
    // See ui_temp_graph_update_series above for the PARTIAL-render-mode rationale.
    lv_obj_invalidate(graph->chart);
}

// Replace all data points (array mode)
void ui_temp_graph_set_series_data(ui_temp_graph_t* graph, int series_id, const float* temps,
                                   int count) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta || !temps || count <= 0) {
        spdlog::error("[TempGraph] Invalid parameters");
        return;
    }

    // Clear existing data before setting new values
    lv_chart_set_all_values(graph->chart, meta->chart_series, LV_CHART_POINT_NONE);

    // Convert float array to int32_t array for LVGL API (using RAII)
    int points_to_copy = count > graph->point_count ? graph->point_count : count;
    auto values = std::make_unique<int32_t[]>(static_cast<size_t>(points_to_copy));
    if (!values) {
        spdlog::error("[TempGraph] Failed to allocate conversion buffer");
        return;
    }

    for (size_t i = 0; i < static_cast<size_t>(points_to_copy); i++) {
        values[i] = static_cast<int32_t>(temps[i] * TEMP_SCALE);
    }

    // Set data using public API
    lv_chart_set_series_values(graph->chart, meta->chart_series, values.get(),
                               static_cast<size_t>(points_to_copy));

    // values automatically freed via ~unique_ptr()

    // Mark as initialized: any subsequent update_series_with_time must NOT wipe the
    // chart via the "backfill from first sample" path — we just populated it.
    meta->first_value_received = true;

    lv_chart_refresh(graph->chart);

    // Update max visible temperature for gradient rendering
    update_max_visible_temp(graph);

    // Bulk data replacement reshapes the curve — gradient must recompute.
    mark_gradient_cache_dirty(graph);

    spdlog::trace("[TempGraph] Series {} '{}' data set ({} points)", series_id, meta->name,
                  points_to_copy);
}

// Replace all data + target history for a series (array mode, parallel arrays).
// Used by backfill paths that have both temp and target history per sample
// (e.g., TemperatureHistoryManager::get_samples_since() replay).
void ui_temp_graph_set_series_data_with_targets(ui_temp_graph_t* graph, int series_id,
                                                const float* temps, const float* targets,
                                                int count) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta || !temps || !targets || count <= 0) {
        spdlog::error("[TempGraph] Invalid parameters");
        return;
    }

    // Reuse existing temp data path (it already handles capping at point_count
    // and chart refresh).
    ui_temp_graph_set_series_data(graph, series_id, temps, count);

    // Mirror the same cap.
    int points_to_copy = count > graph->point_count ? graph->point_count : count;

    // Populate target buffer in lockstep. Zero-init any tail we don't fill.
    if (meta->target_deci_buf) {
        for (int i = 0; i < points_to_copy; i++) {
            meta->target_deci_buf[i] = static_cast<int16_t>(targets[i] * TEMP_SCALE);
        }
        if (points_to_copy < graph->point_count) {
            memset(meta->target_deci_buf + points_to_copy, 0,
                   static_cast<size_t>(graph->point_count - points_to_copy) * sizeof(int16_t));
        }
        meta->target_head = points_to_copy;

        // Pre-stage current target so the next push_target_sample (from a live
        // observer's update_series call) writes the correct value, not stale 0.
        // Without this, the next sample punches a 0 gap between the last replayed
        // target and the next live target, fragmenting the trace into two segments.
        if (points_to_copy > 0) {
            meta->target_temp = targets[points_to_copy - 1];
        }
    }

    spdlog::trace("[TempGraph] Series {} '{}' data+targets set ({} points)", series_id, meta->name,
                  points_to_copy);
}

// Set X-axis timestamp tracking fields directly (no chart-buffer side effects).
// Mirror of the X-axis update logic in ui_temp_graph_update_series_with_time,
// but bulk-form for replay paths that just called set_series_data*.
void ui_temp_graph_set_axis_timestamps(ui_temp_graph_t* graph, int64_t first_ts_ms,
                                       int64_t last_ts_ms, int count) {
    if (!graph || count <= 0)
        return;

    graph->latest_point_time_ms = last_ts_ms;
    graph->visible_point_count = count;

    if (count >= graph->point_count) {
        // Buffer is full; left edge is one full period before the right edge.
        // Matches update_series_with_time's full-buffer fallback.
        graph->first_point_time_ms = last_ts_ms - static_cast<int64_t>(graph->point_count - 1) *
                                                      UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC * 1000;
    } else {
        graph->first_point_time_ms = first_ts_ms;
    }
}

// Clear all data
void ui_temp_graph_clear(ui_temp_graph_t* graph) {
    if (!graph || !graph->chart)
        return;

    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[i];
        if (meta->chart_series) {
            lv_chart_set_all_values(graph->chart, meta->chart_series, LV_CHART_POINT_NONE);
            // Also wipe target history.
            if (meta->target_deci_buf) {
                memset(meta->target_deci_buf, 0,
                       static_cast<size_t>(graph->point_count) * sizeof(int16_t));
            }
            meta->target_head = 0;
        }
    }

    lv_chart_refresh(graph->chart);

    // Update max visible temperature for gradient rendering
    update_max_visible_temp(graph);

    spdlog::trace("[TempGraph] All data cleared");
}

// Clear data for a specific series
void ui_temp_graph_clear_series(ui_temp_graph_t* graph, int series_id) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    lv_chart_set_all_values(graph->chart, meta->chart_series, LV_CHART_POINT_NONE);

    // Wipe target history too.
    if (meta->target_deci_buf) {
        memset(meta->target_deci_buf, 0, static_cast<size_t>(graph->point_count) * sizeof(int16_t));
    }
    meta->target_head = 0;

    lv_chart_refresh(graph->chart);

    // Update max visible temperature for gradient rendering
    update_max_visible_temp(graph);

    spdlog::trace("[TempGraph] Series {} '{}' cleared", series_id, meta->name);
}

// Set target temperature and visibility
void ui_temp_graph_set_series_target(ui_temp_graph_t* graph, int series_id, float target,
                                     bool show) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    meta->target_temp = target;
    meta->show_target = show;
    lv_obj_invalidate(graph->chart);

    spdlog::trace("[TempGraph] Series {} target: {:.1f}°C ({})", series_id, target,
                  show ? "shown" : "hidden");
}

// Show or hide target temperature line
void ui_temp_graph_show_target(ui_temp_graph_t* graph, int series_id, bool show) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    ui_temp_graph_set_series_target(graph, series_id, meta->target_temp, show);
}

// Stage a new current target without pushing into the history buffer.
// The buffer push happens on the next actuals sample (push_target_sample).
void ui_temp_graph_set_current_target(ui_temp_graph_t* graph, int series_id, float target,
                                      bool show) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    meta->target_temp = target;
    meta->show_target = show;
    lv_obj_invalidate(graph->chart);

    spdlog::trace("[TempGraph] Series {} current target staged: {:.1f}°C ({})", series_id, target,
                  show ? "shown" : "hidden");
}

// Set Y-axis temperature range
void ui_temp_graph_set_temp_range(ui_temp_graph_t* graph, float min, float max) {
    if (!graph || !graph->chart || min >= max) {
        spdlog::error("[TempGraph] Invalid temperature range");
        return;
    }

    // Only invalidate the gradient cache if the range actually changed — the
    // curve→pixel mapping is unchanged otherwise, so a no-op call must not force
    // a recompute (that's the whole point of the cache).
    bool range_changed = (graph->min_temp != min) || (graph->max_temp != max);

    graph->min_temp = min;
    graph->max_temp = max;

    lv_chart_set_axis_range(graph->chart, LV_CHART_AXIS_PRIMARY_Y,
                            static_cast<int32_t>(min * TEMP_SCALE),
                            static_cast<int32_t>(max * TEMP_SCALE));

    if (range_changed)
        mark_gradient_cache_dirty(graph);

    lv_obj_invalidate(graph->chart);

    spdlog::trace("[TempGraph] Temperature range set: {:.0f} - {:.0f}°C", min, max);
}

// Set point count
void ui_temp_graph_set_point_count(ui_temp_graph_t* graph, int count) {
    if (!graph || !graph->chart || count <= 0) {
        spdlog::error("[TempGraph] Invalid point count");
        return;
    }

    graph->point_count = count;
    lv_chart_set_point_count(graph->chart, static_cast<uint32_t>(count));

    // Realloc per-series target buffers to match. We discard old history on resize —
    // the chart itself does the same (LVGL's set_point_count truncates / clears).
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        auto& m = graph->series_meta[i];
        if (!m.chart_series)
            continue;
        delete[] m.target_deci_buf;
        m.target_deci_buf = new (std::nothrow) int16_t[static_cast<size_t>(count)]();
        m.target_head = 0;
        if (!m.target_deci_buf) {
            spdlog::error("[TempGraph] Failed to realloc target buffer for series '{}'", m.name);
        }
    }

    spdlog::trace("[TempGraph] Point count set: {}", count);
}

// Set gradient opacity for a series
void ui_temp_graph_set_series_gradient(ui_temp_graph_t* graph, int series_id, lv_opa_t bottom_opa,
                                       lv_opa_t top_opa) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    meta->gradient_bottom_opa = bottom_opa;
    meta->gradient_top_opa = top_opa;

    // Gradient opacity changed — recompute the cached fill.
    mark_gradient_cache_dirty(graph);

    lv_obj_invalidate(graph->chart);

    spdlog::trace("[TempGraph] Series {} gradient: bottom={}%, top={}%", series_id,
                  (bottom_opa * 100) / 255, (top_opa * 100) / 255);
}

// Set Y-axis label configuration
void ui_temp_graph_set_y_axis(ui_temp_graph_t* graph, float increment, bool show) {
    if (!graph || !graph->chart) {
        spdlog::error("[TempGraph] NULL graph in set_y_axis");
        return;
    }

    graph->y_axis_increment = increment;
    graph->show_y_axis = show;

    // Force redraw to apply changes
    lv_obj_invalidate(graph->chart);

    spdlog::trace("[TempGraph] Y-axis config: increment={:.0f}°, show={}", increment, show);
}

// Set axis label font size
void ui_temp_graph_set_axis_size(ui_temp_graph_t* graph, const char* size) {
    if (!graph || !graph->chart) {
        spdlog::error("[TempGraph] NULL graph in set_axis_size");
        return;
    }

    // Map size name to font token using shared helper
    const char* font_token = theme_manager_size_to_font_token(size, "sm");

    // Y-axis width varies by size (smaller fonts need less space)
    int32_t y_axis_width = 40; // default for "sm"
    if (size) {
        if (strcmp(size, "xs") == 0) {
            y_axis_width = 30;
        } else if (strcmp(size, "md") == 0) {
            y_axis_width = 45;
        } else if (strcmp(size, "lg") == 0) {
            y_axis_width = 50;
        }
    }

    graph->axis_font = theme_manager_get_font(font_token);
    graph->y_axis_width = y_axis_width;

    // Recalculate padding to match new font size
    int32_t space_xs = theme_manager_get_spacing("space_xs");
    int32_t space_sm = theme_manager_get_spacing("space_sm");
    int32_t space_md = theme_manager_get_spacing("space_md");
    int32_t label_height = theme_manager_get_font_height(graph->axis_font);

    // Update padding (tighter for smaller sizes)
    // Top padding must accommodate the full top Y-axis label above the top grid line
    bool is_xs = size && strcmp(size, "xs") == 0;
    int32_t min_top_for_label = label_height;
    int32_t top_pad =
        is_xs ? LV_MAX(space_sm, min_top_for_label) : LV_MAX(space_md, min_top_for_label);
    int32_t left_pad = y_axis_width + space_sm; // Add gap between labels and chart
    int32_t bottom_pad =
        is_xs ? (space_xs + label_height + space_xs) : (space_sm + label_height + space_md);

    lv_obj_set_style_pad_top(graph->chart, top_pad, LV_PART_MAIN);
    lv_obj_set_style_pad_left(graph->chart, left_pad, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(graph->chart, bottom_pad, LV_PART_MAIN);

    lv_obj_invalidate(graph->chart);

    spdlog::trace("[TempGraph] Axis size: {} -> {} (y_width={}, label_h={})", size ? size : "null",
                  font_token, y_axis_width, label_height);
}

// Set which chart features are visible
void ui_temp_graph_set_features(ui_temp_graph_t* graph, uint32_t features) {
    if (!graph || !graph->chart) {
        spdlog::error("[TempGraph] NULL graph in set_features");
        return;
    }

    // LINES is always forced on — the chart must show data lines
    features |= TEMP_GRAPH_FEATURE_LINES;

    // If the gradient-relevant feature bit changed, the cached fill is stale.
    uint32_t prev_features = graph->features;
    graph->features = features;
    if ((prev_features & TEMP_GRAPH_FEATURE_GRADIENTS) !=
        (features & TEMP_GRAPH_FEATURE_GRADIENTS)) {
        mark_gradient_cache_dirty(graph);
    }

    // Toggle Y-axis via existing API
    bool want_y = (features & TEMP_GRAPH_FEATURE_Y_AXIS) != 0;
    graph->show_y_axis = want_y;

    // Toggle X-axis
    bool want_x = (features & TEMP_GRAPH_FEATURE_X_AXIS) != 0;
    graph->show_x_axis = want_x;

    // Adjust padding based on which axes are visible. Uses the font set by
    // set_axis_size() (which must be called first). This tightens padding when
    // axes are hidden (e.g., small widget without X/Y labels).
    int32_t space_xs = theme_manager_get_spacing("space_xs");
    int32_t space_sm = theme_manager_get_spacing("space_sm");
    int32_t label_height = theme_manager_get_font_height(graph->axis_font);

    // Left padding: reserve space for Y-axis labels, or use minimal padding
    int32_t left_pad = want_y ? (graph->y_axis_width + space_sm) : space_xs;
    lv_obj_set_style_pad_left(graph->chart, left_pad, LV_PART_MAIN);

    // Bottom padding: reserve space for X-axis labels, or use minimal padding
    int32_t bottom_pad = want_x ? (space_xs + label_height + space_xs) : space_xs;
    lv_obj_set_style_pad_bottom(graph->chart, bottom_pad, LV_PART_MAIN);

    // Top padding: reserve space for top Y-axis label, or use minimal padding
    int32_t top_pad = want_y ? LV_MAX(space_sm, label_height) : space_xs;
    lv_obj_set_style_pad_top(graph->chart, top_pad, LV_PART_MAIN);

    // Toggle gradient opacity: zero out when disabled, restore defaults when enabled
    bool want_gradients = (features & TEMP_GRAPH_FEATURE_GRADIENTS) != 0;
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[i];
        if (!meta->chart_series)
            continue;

        if (want_gradients) {
            // Restore default gradient opacities
            meta->gradient_top_opa = UI_TEMP_GRAPH_GRADIENT_TOP_OPA;
            meta->gradient_bottom_opa = UI_TEMP_GRAPH_GRADIENT_BOTTOM_OPA;
        } else {
            meta->gradient_top_opa = LV_OPA_TRANSP;
            meta->gradient_bottom_opa = LV_OPA_TRANSP;
        }
    }

    lv_obj_invalidate(graph->chart);

    spdlog::trace(
        "[TempGraph] Features set: 0x{:02x} (lines={} targets={} legend={} "
        "y_axis={} x_axis={} gradients={} readouts={})",
        features, (features & TEMP_GRAPH_FEATURE_LINES) != 0,
        (features & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0,
        (features & TEMP_GRAPH_FEATURE_LEGEND) != 0, (features & TEMP_GRAPH_FEATURE_Y_AXIS) != 0,
        (features & TEMP_GRAPH_FEATURE_X_AXIS) != 0, (features & TEMP_GRAPH_FEATURE_GRADIENTS) != 0,
        (features & TEMP_GRAPH_FEATURE_READOUTS) != 0);
}

// Get the current feature flags
uint32_t ui_temp_graph_get_features(ui_temp_graph_t* graph) {
    if (!graph) {
        return 0;
    }
    return graph->features;
}

// Gradient cache introspection (#979). See header for contract.
bool ui_temp_graph_gradient_cache_is_dirty(const ui_temp_graph_t* graph) {
    return graph ? graph->gradient_cache_dirty : false;
}

void ui_temp_graph_mark_gradient_cache_clean(ui_temp_graph_t* graph) {
    if (graph)
        graph->gradient_cache_dirty = false;
}
