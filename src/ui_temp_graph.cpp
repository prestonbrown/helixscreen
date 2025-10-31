/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "ui_temp_graph.h"
#include "ui_theme.h"
#include <spdlog/spdlog.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// Helper: Find series metadata by ID
static ui_temp_series_meta_t* find_series(ui_temp_graph_t* graph, int series_id) {
    if (!graph || series_id < 0 || series_id >= UI_TEMP_GRAPH_MAX_SERIES) {
        return nullptr;
    }

    for (int i = 0; i < graph->series_count; i++) {
        if (graph->series_meta[i].id == series_id && graph->series_meta[i].chart_series != nullptr) {
            return &graph->series_meta[i];
        }
    }
    return nullptr;
}

// Event callback for drawing gradient fills under curves (LVGL 9 draw task system)
// TODO: Currently disabled - needs complete rewrite for LVGL 9's new draw task system
#if 0
static void draw_gradient_fill_cb(lv_event_t* e) {

    // Get chart dimensions
    lv_area_t chart_area;
    lv_obj_get_coords(chart, &chart_area);

    // Get chart padding to find data area (for Y coordinates only - X comes from API)
    lv_coord_t pad_top = lv_obj_get_style_pad_top(chart, LV_PART_MAIN);
    lv_coord_t pad_bottom = lv_obj_get_style_pad_bottom(chart, LV_PART_MAIN);

    lv_coord_t data_h = lv_area_get_height(&chart_area) - pad_top - pad_bottom;

    // Data area coordinates
    lv_coord_t data_y1 = chart_area.y1 + pad_top;
    lv_coord_t data_y2 = data_y1 + data_h;

    // Get point count from chart
    uint32_t point_cnt = lv_chart_get_point_count(chart);
    if (point_cnt == 0) return;

    // Draw gradient fill for each visible series
    for (int i = 0; i < graph->series_count; i++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[i];
        if (!meta->visible || !meta->chart_series) continue;

        // Draw triangular strips to fill area under curve with gradient
        // We'll draw vertical strips for each segment between data points
        for (uint32_t pt = 0; pt < point_cnt - 1; pt++) {
            // Check if either point has no data (read-only access to internal array)
            if (meta->chart_series->y_points[pt] == LV_CHART_POINT_NONE ||
                meta->chart_series->y_points[pt + 1] == LV_CHART_POINT_NONE) {
                continue;
            }

            // Get pixel positions from LVGL's public API
            lv_point_t pos1, pos2;
            lv_chart_get_point_pos_by_id(chart, meta->chart_series, pt, &pos1);
            lv_chart_get_point_pos_by_id(chart, meta->chart_series, pt + 1, &pos2);

            // Use the X,Y coordinates directly (LVGL returns absolute screen coords)
            lv_coord_t x1 = pos1.x;
            lv_coord_t y1 = pos1.y;
            lv_coord_t x2 = pos2.x;
            lv_coord_t y2 = pos2.y;

            // Clamp Y values to data area (handle out-of-range data gracefully)
            if (y1 < data_y1) y1 = data_y1;
            if (y2 < data_y1) y2 = data_y1;
            if (y1 > data_y2) y1 = data_y2;
            if (y2 > data_y2) y2 = data_y2;

            // Draw trapezoid (two triangles) for this segment
            // Triangle 1: (x1,y1) -> (x2,y2) -> (x1,data_y2)
            lv_draw_triangle_dsc_t tri1_dsc;
            lv_draw_triangle_dsc_init(&tri1_dsc);
            tri1_dsc.p[0].x = x1; tri1_dsc.p[0].y = y1;      // Top left
            tri1_dsc.p[1].x = x2; tri1_dsc.p[1].y = y2;      // Top right
            tri1_dsc.p[2].x = x1; tri1_dsc.p[2].y = data_y2; // Bottom left
            tri1_dsc.grad.dir = LV_GRAD_DIR_VER;
            tri1_dsc.grad.stops[0].color = meta->color;
            tri1_dsc.grad.stops[0].opa = meta->gradient_bottom_opa;
            tri1_dsc.grad.stops[0].frac = 0;
            tri1_dsc.grad.stops[1].color = meta->color;
            tri1_dsc.grad.stops[1].opa = meta->gradient_top_opa;
            tri1_dsc.grad.stops[1].frac = 255;
            tri1_dsc.grad.stops_count = 2;

            // Triangle 2: (x2,y2) -> (x2,data_y2) -> (x1,data_y2)
            lv_draw_triangle_dsc_t tri2_dsc;
            lv_draw_triangle_dsc_init(&tri2_dsc);
            tri2_dsc.p[0].x = x2; tri2_dsc.p[0].y = y2;      // Top right
            tri2_dsc.p[1].x = x2; tri2_dsc.p[1].y = data_y2; // Bottom right
            tri2_dsc.p[2].x = x1; tri2_dsc.p[2].y = data_y2; // Bottom left
            tri2_dsc.grad.dir = LV_GRAD_DIR_VER;
            tri2_dsc.grad.stops[0].color = meta->color;
            tri2_dsc.grad.stops[0].opa = meta->gradient_bottom_opa;
            tri2_dsc.grad.stops[0].frac = 0;
            tri2_dsc.grad.stops[1].color = meta->color;
            tri2_dsc.grad.stops[1].opa = meta->gradient_top_opa;
            tri2_dsc.grad.stops[1].frac = 255;
            tri2_dsc.grad.stops_count = 2;

            // Draw both triangles
            lv_draw_triangle(layer, &tri1_dsc);
            lv_draw_triangle(layer, &tri2_dsc);
        }
    }
}
#endif  // Gradient fill disabled

// Create temperature graph widget
ui_temp_graph_t* ui_temp_graph_create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[TempGraph] NULL parent");
        return nullptr;
    }

    // Allocate graph structure
    ui_temp_graph_t* graph = (ui_temp_graph_t*)malloc(sizeof(ui_temp_graph_t));
    if (!graph) {
        spdlog::error("[TempGraph] Failed to allocate graph structure");
        return nullptr;
    }

    memset(graph, 0, sizeof(ui_temp_graph_t));

    // Initialize defaults
    graph->point_count = UI_TEMP_GRAPH_DEFAULT_POINTS;
    graph->min_temp = UI_TEMP_GRAPH_DEFAULT_MIN_TEMP;
    graph->max_temp = UI_TEMP_GRAPH_DEFAULT_MAX_TEMP;
    graph->series_count = 0;
    graph->next_series_id = 0;

    // Create LVGL chart
    graph->chart = lv_chart_create(parent);
    if (!graph->chart) {
        spdlog::error("[TempGraph] Failed to create chart widget");
        free(graph);
        return nullptr;
    }

    // Configure chart
    lv_chart_set_type(graph->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(graph->chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(graph->chart, graph->point_count);

    // Set Y-axis range
    lv_chart_set_axis_range(graph->chart, LV_CHART_AXIS_PRIMARY_Y,
                            (int32_t)graph->min_temp, (int32_t)graph->max_temp);

    // Style chart background (theme handles colors)
    lv_obj_set_style_bg_opa(graph->chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(graph->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(graph->chart, 12, LV_PART_MAIN);

    // Style division lines (theme handles colors)
    lv_obj_set_style_line_width(graph->chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_opa(graph->chart, LV_OPA_30, LV_PART_MAIN);  // Subtle - 30% opacity

    // Style data series lines
    lv_obj_set_style_line_width(graph->chart, 2, LV_PART_ITEMS);  // Series line thickness
    lv_obj_set_style_line_opa(graph->chart, LV_OPA_COVER, LV_PART_ITEMS);  // Full opacity for series

    // Hide point indicators (circles at each data point)
    lv_obj_set_style_width(graph->chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(graph->chart, 0, LV_PART_INDICATOR);

    // Configure division line count
    lv_chart_set_div_line_count(graph->chart, 5, 10);  // 5 horizontal, 10 vertical division lines

    // TODO: Gradient fill temporarily disabled - requires LVGL 9 draw task system rewrite
    // The LVGL 8-style event-based custom drawing doesn't work in LVGL 9
    // Need to implement using lv_draw_task_t API with LV_EVENT_DRAW_TASK_ADDED
    //
    // lv_obj_add_flag(graph->chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    // lv_obj_add_event_cb(graph->chart, draw_gradient_fill_cb, LV_EVENT_DRAW_TASK_ADDED, graph);

    // Store graph pointer in chart user data for retrieval
    lv_obj_set_user_data(graph->chart, graph);

    spdlog::info("[TempGraph] Created: {} points, {:.0f}-{:.0f}°C range",
                 graph->point_count, graph->min_temp, graph->max_temp);

    return graph;
}

// Destroy temperature graph widget
void ui_temp_graph_destroy(ui_temp_graph_t* graph) {
    if (!graph) return;

    // Remove all series (cursors will be cleaned up automatically)
    for (int i = 0; i < graph->series_count; i++) {
        if (graph->series_meta[i].chart_series) {
            lv_chart_remove_series(graph->chart, graph->series_meta[i].chart_series);
        }
    }

    // Delete chart widget
    if (graph->chart) {
        lv_obj_del(graph->chart);
    }

    free(graph);
    spdlog::debug("[TempGraph] Destroyed");
}

// Get underlying chart widget
lv_obj_t* ui_temp_graph_get_chart(ui_temp_graph_t* graph) {
    return graph ? graph->chart : nullptr;
}

// Add a new temperature series
int ui_temp_graph_add_series(ui_temp_graph_t* graph, const char* name, lv_color_t color) {
    if (!graph || !name) {
        spdlog::error("[TempGraph] NULL graph or name");
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

    // Create target temperature cursor (horizontal line, initially hidden)
    meta->target_cursor = lv_chart_add_cursor(graph->chart, color, LV_DIR_HOR);
    if (meta->target_cursor) {
        lv_chart_set_cursor_point(graph->chart, meta->target_cursor, ser, 0);
    }

    graph->series_count++;

    spdlog::debug("[TempGraph] Added series {} '{}' (slot {}, color 0x{:06X})",
                  meta->id, meta->name, slot, lv_color_to_u32(color));

    return meta->id;
}

// Remove a temperature series
void ui_temp_graph_remove_series(ui_temp_graph_t* graph, int series_id) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    // Remove cursor (if exists)
    if (meta->target_cursor) {
        // LVGL doesn't have lv_chart_remove_cursor, cursor is auto-freed with chart
        meta->target_cursor = nullptr;
    }

    // Remove chart series
    lv_chart_remove_series(graph->chart, meta->chart_series);

    // Clear metadata
    memset(meta, 0, sizeof(ui_temp_series_meta_t));
    meta->chart_series = nullptr;

    graph->series_count--;

    spdlog::debug("[TempGraph] Removed series {} ({} series remaining)", series_id, graph->series_count);
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

    lv_obj_invalidate(graph->chart);
    spdlog::debug("[TempGraph] Series {} '{}' {}", series_id, meta->name, visible ? "shown" : "hidden");
}

// Add a single temperature point (push mode)
void ui_temp_graph_update_series(ui_temp_graph_t* graph, int series_id, float temp) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    // Add point to series (shifts old data left)
    lv_chart_set_next_value(graph->chart, meta->chart_series, (int32_t)temp);
}

// Replace all data points (array mode)
void ui_temp_graph_set_series_data(ui_temp_graph_t* graph, int series_id, const float* temps, int count) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta || !temps || count <= 0) {
        spdlog::error("[TempGraph] Invalid parameters");
        return;
    }

    // Clear existing data using public API
    lv_chart_set_all_values(graph->chart, meta->chart_series, LV_CHART_POINT_NONE);

    // Convert float array to int32_t array for LVGL API
    int points_to_copy = count > graph->point_count ? graph->point_count : count;
    int32_t* values = (int32_t*)malloc(points_to_copy * sizeof(int32_t));
    if (!values) {
        spdlog::error("[TempGraph] Failed to allocate conversion buffer");
        return;
    }

    for (int i = 0; i < points_to_copy; i++) {
        values[i] = (int32_t)temps[i];
    }

    // Set data using public API
    lv_chart_set_series_values(graph->chart, meta->chart_series, values, points_to_copy);

    free(values);

    lv_chart_refresh(graph->chart);
    spdlog::debug("[TempGraph] Series {} '{}' data set ({} points)", series_id, meta->name, points_to_copy);
}

// Clear all data
void ui_temp_graph_clear(ui_temp_graph_t* graph) {
    if (!graph) return;

    for (int i = 0; i < graph->series_count; i++) {
        ui_temp_series_meta_t* meta = &graph->series_meta[i];
        if (meta->chart_series) {
            lv_chart_set_all_values(graph->chart, meta->chart_series, LV_CHART_POINT_NONE);
        }
    }

    lv_chart_refresh(graph->chart);
    spdlog::debug("[TempGraph] All data cleared");
}

// Clear data for a specific series
void ui_temp_graph_clear_series(ui_temp_graph_t* graph, int series_id) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    lv_chart_set_all_values(graph->chart, meta->chart_series, LV_CHART_POINT_NONE);

    lv_chart_refresh(graph->chart);
    spdlog::debug("[TempGraph] Series {} '{}' cleared", series_id, meta->name);
}

// Set target temperature and visibility
void ui_temp_graph_set_series_target(ui_temp_graph_t* graph, int series_id, float target, bool show) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    meta->target_temp = target;
    meta->show_target = show;

    if (meta->target_cursor && show) {
        // Update cursor position using public API
        lv_chart_set_cursor_pos_y(graph->chart, meta->target_cursor, (int32_t)target);

        lv_obj_invalidate(graph->chart);
    }

    spdlog::debug("[TempGraph] Series {} target: {:.1f}°C ({})", series_id, target, show ? "shown" : "hidden");
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

// Set Y-axis temperature range
void ui_temp_graph_set_temp_range(ui_temp_graph_t* graph, float min, float max) {
    if (!graph || min >= max) {
        spdlog::error("[TempGraph] Invalid temperature range");
        return;
    }

    graph->min_temp = min;
    graph->max_temp = max;

    lv_chart_set_axis_range(graph->chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)min, (int32_t)max);

    spdlog::debug("[TempGraph] Temperature range set: {:.0f} - {:.0f}°C", min, max);
}

// Set point count
void ui_temp_graph_set_point_count(ui_temp_graph_t* graph, int count) {
    if (!graph || count <= 0) {
        spdlog::error("[TempGraph] Invalid point count");
        return;
    }

    graph->point_count = count;
    lv_chart_set_point_count(graph->chart, count);

    spdlog::debug("[TempGraph] Point count set: {}", count);
}

// Set gradient opacity for a series
void ui_temp_graph_set_series_gradient(ui_temp_graph_t* graph, int series_id, lv_opa_t bottom_opa, lv_opa_t top_opa) {
    ui_temp_series_meta_t* meta = find_series(graph, series_id);
    if (!meta) {
        spdlog::error("[TempGraph] Series {} not found", series_id);
        return;
    }

    meta->gradient_bottom_opa = bottom_opa;
    meta->gradient_top_opa = top_opa;

    lv_obj_invalidate(graph->chart);

    spdlog::debug("[TempGraph] Series {} gradient: bottom={}%, top={}%",
                  series_id, (bottom_opa * 100) / 255, (top_opa * 100) / 255);
}
