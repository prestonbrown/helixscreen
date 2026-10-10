// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_system_path_canvas.h"

#include "ui_filament_path_plan.h"
#include "ui_fonts.h"
#include "ui_system_path_plan.h"
#include "ui_toolhead_badge.h"

#include "clog_meter_geometry.h"
#include "display_numbering.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "nozzle_renderer_dispatch.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace helix::ui::syspath;
namespace fpath = helix::ui::fpath;

// Default dimensions
static constexpr int32_t DEFAULT_WIDTH = 300;
static constexpr int32_t DEFAULT_HEIGHT = 150;

// Registry of widget data
static std::unordered_map<lv_obj_t*, SystemPathData*> s_registry;

static SystemPathData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

const SystemPathData* helix::ui::syspath::system_path_data(lv_obj_t* obj) {
    return get_data(obj);
}

// Theme colors are read on every draw, so a theme or dark-mode switch repaints
// the live canvas in the new colors.
static void load_theme_colors(SystemPathData* data) {
    bool dark_mode = theme_manager_is_dark_mode();

    // Try theme-specific tokens first, fall back to standard tokens if they resolve to black
    data->color_idle =
        theme_manager_get_color(dark_mode ? "filament_idle_dark" : "filament_idle_light");
    if (data->color_idle.red == 0 && data->color_idle.green == 0 && data->color_idle.blue == 0) {
        data->color_idle = theme_manager_get_color("text_muted");
    }

    data->color_hub_bg =
        theme_manager_get_color(dark_mode ? "filament_hub_bg_dark" : "filament_hub_bg_light");
    if (data->color_hub_bg.red == 0 && data->color_hub_bg.green == 0 &&
        data->color_hub_bg.blue == 0) {
        data->color_hub_bg = theme_manager_get_color("card_bg");
    }

    data->color_hub_border = theme_manager_get_color(dark_mode ? "filament_hub_border_dark"
                                                               : "filament_hub_border_light");
    if (data->color_hub_border.red == 0 && data->color_hub_border.green == 0 &&
        data->color_hub_border.blue == 0) {
        data->color_hub_border = theme_manager_get_color("border");
    }

    data->color_text = theme_manager_get_color("text");
    data->color_error = theme_manager_get_color("filament_error");
    data->color_bg = theme_manager_get_color("card_bg");
    data->color_accent = helix::ui::tube_accent();
    data->toolhead_style = helix::SettingsManager::instance().get_effective_toolhead_style();
}

// Sizes and the label font are bound to the breakpoint, not the theme, so they
// are read once per widget.
static void load_theme_sizes(SystemPathData* data) {
    int32_t space_xs = theme_manager_get_spacing("space_xs");
    int32_t space_md = theme_manager_get_spacing("space_md");
    data->tube_gauge = fpath::tube_gauge_for_spacing(space_xs);
    data->sensor_radius = LV_MAX(4, space_xs);
    data->hub_width = LV_MAX(70, space_md * 6);
    data->hub_height = LV_MAX(24, space_md * 2);
    data->border_radius = LV_MAX(4, space_xs);
    data->extruder_scale = LV_MAX(8, space_md);
    data->space_md = space_md;

    const char* font_name = lv_xml_get_const(nullptr, "font_small");
    data->label_font = font_name ? lv_xml_get_font(nullptr, font_name) : &noto_sans_12;
}

// ============================================================================
// Drawing Helpers
// ============================================================================

static void draw_hub_box(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t width, int32_t height,
                         lv_color_t bg_color, lv_color_t border_color, lv_color_t text_color,
                         const lv_font_t* font, int32_t radius, const char* label) {
    // Background
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = bg_color;
    fill_dsc.radius = radius;

    lv_area_t box_area = {cx - width / 2, cy - height / 2, cx + width / 2, cy + height / 2};
    lv_draw_fill(layer, &fill_dsc, &box_area);

    // Border
    lv_draw_border_dsc_t border_dsc;
    lv_draw_border_dsc_init(&border_dsc);
    border_dsc.color = border_color;
    border_dsc.width = 2;
    border_dsc.radius = radius;
    lv_draw_border(layer, &border_dsc, &box_area);

    // Label
    if (label && label[0] && font) {
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = text_color;
        label_dsc.font = font;
        label_dsc.align = LV_TEXT_ALIGN_CENTER;
        label_dsc.text = label;

        int32_t font_h = lv_font_get_line_height(font);
        lv_area_t label_area = {cx - width / 2, cy - font_h / 2, cx + width / 2, cy + font_h / 2};
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

// A hub's buffer: a labeled box on the output line between the hub and the
// nozzle, tinted by the buffer severity.
static void draw_buffer_box(lv_layer_t* layer, const SystemPathData* data, int32_t cx, int32_t cy,
                            int32_t box_w, int32_t box_h, int fault, const char* label) {
    lv_color_t bg = data->color_hub_bg;
    if (fault > 0) {
        const auto status = static_cast<helix::ui::ClogMeterStatus>(std::min(fault, 2));
        bg = lv_color_mix(theme_manager_get_color(helix::ui::buffer_status_token(status)), bg, 85);
    }
    draw_hub_box(layer, cx, cy, box_w, box_h, bg, data->color_hub_border, data->color_text,
                 data->label_font, data->border_radius, label);
}

// The single-toolhead layout's buffer, halfway down the trunk.
static void draw_trunk_buffer(lv_layer_t* layer, const SystemPathData* data, const SysLayout& L) {
    const int32_t hub_bottom = L.hub_y + L.hub_h / 2;
    const int32_t nozzle_top = L.nozzle_y - data->extruder_scale * 2;
    draw_buffer_box(layer, data, L.center_x, hub_bottom + (nozzle_top - hub_bottom) / 2,
                    data->hub_width * 2 / 3, L.hub_h * 2 / 3, data->buffer_fault,
                    data->buffer_label);
}

// ============================================================================
// Draw
// ============================================================================
// system_path_draw_cb plans every tube and sensor band (plan_overview), paints
// them with the detail views' painter, then draws the boxes, the box-edge
// bands, the toolheads and their badges on top.

// Status text centered along the bottom edge (multi-tool layout).
static void draw_status_centered(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    if (!data->status_text[0] || !data->label_font)
        return;
    lv_draw_label_dsc_t status_dsc;
    lv_draw_label_dsc_init(&status_dsc);
    status_dsc.color = data->color_text;
    status_dsc.font = data->label_font;
    status_dsc.align = LV_TEXT_ALIGN_CENTER;
    status_dsc.text = data->status_text;

    int32_t font_h = lv_font_get_line_height(data->label_font);
    int32_t status_y = L.y_off + L.height - font_h - 2;
    lv_area_t status_area = {L.x_off + 4, status_y, L.x_off + L.width - 4, status_y + font_h};
    lv_draw_label(layer, &status_dsc, &status_area);
}

// Status text right-aligned beside the nozzle (single-tool layout).
static void draw_status_beside_nozzle(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    if (!data->status_text[0] || !data->label_font)
        return;
    lv_draw_label_dsc_t status_dsc;
    lv_draw_label_dsc_init(&status_dsc);
    status_dsc.color = data->color_text;
    status_dsc.font = data->label_font;
    status_dsc.align = LV_TEXT_ALIGN_RIGHT;
    status_dsc.text = data->status_text;

    int32_t font_h = lv_font_get_line_height(data->label_font);
    int32_t label_right = L.center_x - data->extruder_scale * 3;
    int32_t label_left = L.x_off + 4;
    lv_area_t status_area = {label_left, L.nozzle_y - font_h / 2, label_right,
                             L.nozzle_y + font_h / 2};
    lv_draw_label(layer, &status_dsc, &status_area);
}

// Multi-tool hubs: "H" on a MIXED unit's stem, "Hub" on a HUB unit's, each HUB
// hub with its buffer box below it.
static void draw_mini_hubs(lv_layer_t* layer, const SystemPathData* data,
                           const OverviewBoxes& boxes) {
    for (int i = 0; i < data->unit_count && i < SystemPathData::MAX_UNITS; i++) {
        const HubInfo& hi = boxes.hubs[i];
        if (!hi.valid)
            continue;
        const char* hub_label = (data->unit_topology[i] == 3) ? "H" : "Hub";
        draw_hub_box(layer, hi.hub_x, hi.mini_hub_y, hi.mini_hub_w, hi.mini_hub_h, hi.hub_bg_color,
                     data->color_hub_border, data->color_text, data->label_font,
                     data->border_radius, hub_label);
        if (hi.buffer_h > 0) {
            draw_buffer_box(layer, data, hi.hub_x, hi.buffer_y, hi.buffer_w, hi.buffer_h,
                            data->unit_buffer_fault[i], data->unit_buffer_label[i]);
        }
    }
}

// Tool nozzles along the bottom row, then each one's corner badge.
static void draw_tool_row(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    const int32_t small_scale = small_tool_scale(*data);
    const lv_color_t active = lv_color_hex(data->active_color);
    const int tools = LV_MIN(data->total_tools, SystemPathData::MAX_TOOLS);
    for (int t = 0; t < tools; ++t) {
        int32_t tool_x = calc_tool_x(t, data->total_tools, L.x_off, L.width);
        bool is_active_tool = (t == data->active_tool) && data->filament_loaded;
        draw_nozzle_for_style(layer, tool_x, L.tools_y,
                              is_active_tool ? std::optional(active) : std::nullopt, small_scale);
    }
    const helix::ui::ToolBadgeLook look = helix::ui::tool_badge_look();
    for (int t = 0; t < tools; ++t) {
        int32_t tool_x = calc_tool_x(t, data->total_tools, L.x_off, L.width);
        helix::ui::draw_toolhead_badge(
            layer, look,
            helix::ui::toolhead_bounds(data->toolhead_style, tool_x, L.tools_y, small_scale),
            data->tool_labels[t]);
    }
}

// The single nozzle, badged with its virtual tool.
static void draw_single_nozzle(lv_layer_t* layer, SystemPathData* data, const SysLayout& L) {
    const bool unit_active = data->active_unit >= 0 && data->filament_loaded;
    const bool bp_active = data->bypass_active && data->filament_loaded;
    const lv_color_t noz_color =
        bp_active ? lv_color_hex(data->bypass_color) : lv_color_hex(data->active_color);
    draw_nozzle_for_style(layer, L.center_x, L.nozzle_y,
                          (unit_active || bp_active) ? std::optional(noz_color) : std::nullopt,
                          data->extruder_scale);

    // Only when multiple slots feed one toolhead
    if (data->total_tools <= 1 && data->current_tool >= 0) {
        helix::ui::draw_toolhead_badge(layer, helix::ui::tool_badge_look(),
                                       helix::ui::toolhead_bounds(data->toolhead_style, L.center_x,
                                                                  L.nozzle_y, data->extruder_scale),
                                       data->current_tool_label);
    }
}

static void system_path_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    SystemPathData* data = get_data(obj);
    if (!data)
        return;

    if (data->unit_count <= 0) {
        spdlog::trace("[SystemPath] No units to draw");
        return;
    }
    load_theme_colors(data);

    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    const SysLayout L = compute_sys_layout(*data, obj_coords);

    PathPlan& plan = fpath::plan_scratch();
    OverviewBoxes boxes;
    plan_overview(*data, L, plan, boxes);
    const fpath::TubePalette pal = overview_palette(*data);
    fpath::paint_tubes(layer, plan, pal);

    if (L.multi_tool) {
        draw_mini_hubs(layer, data, boxes);
        fpath::paint_box_bands(layer, plan, pal);
        draw_tool_row(layer, data, L);
        draw_status_centered(layer, data, L);
    } else {
        draw_hub_box(layer, L.center_x, L.hub_y, data->hub_width, L.hub_h, boxes.combiner_bg,
                     data->color_hub_border, data->color_text, data->label_font,
                     data->border_radius, "Hub");
        if (data->has_buffer)
            draw_trunk_buffer(layer, data, L);
        fpath::paint_box_bands(layer, plan, pal);
        draw_single_nozzle(layer, data, L);
        draw_status_beside_nozzle(layer, data, L);
    }

    spdlog::trace("[SystemPath] Draw: units={}, active={}, loaded={}, tools={}, active_tool={}, "
                  "current_tool={}, bypass={}(active={}), routes={}, bands={}",
                  data->unit_count, data->active_unit, data->filament_loaded, data->total_tools,
                  data->active_tool, data->current_tool, data->has_bypass, data->bypass_active,
                  plan.route_count, plan.band_count);
}

// ============================================================================
// Event Handlers
// ============================================================================

// Bypass spool clicks are handled by the BypassSpoolWidgets overlay the panel
// places on top of this canvas — see ui_bypass_spool_widget.h.

static void system_path_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto it = s_registry.find(obj);
    if (it != s_registry.end()) {
        std::unique_ptr<SystemPathData> data(it->second);
        s_registry.erase(it);
        // data automatically freed when unique_ptr goes out of scope
    }
}

// ============================================================================
// XML Widget Interface
// ============================================================================

static void* system_path_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create(static_cast<lv_obj_t*>(parent));
    if (!obj)
        return nullptr;

    auto data_ptr = std::make_unique<SystemPathData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    load_theme_colors(data);
    load_theme_sizes(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, system_path_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, system_path_delete_cb, LV_EVENT_DELETE, nullptr);
    // Click handling on the canvas is no longer needed — bypass clicks are
    // captured by the BypassSpoolWidgets overlay the panel places on top.

    spdlog::debug("[SystemPath] Created widget via XML");
    return obj;
}

static void system_path_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);
    if (!obj)
        return;

    lv_xml_obj_apply(state, attrs);

    auto* data = get_data(obj);
    if (!data)
        return;

    bool needs_redraw = false;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "unit_count") == 0) {
            data->unit_count = LV_CLAMP(atoi(value), 0, SystemPathData::MAX_UNITS);
            needs_redraw = true;
        } else if (strcmp(name, "active_unit") == 0) {
            data->active_unit = atoi(value);
            needs_redraw = true;
        } else if (strcmp(name, "active_color") == 0) {
            data->active_color = strtoul(value, nullptr, 0);
            needs_redraw = true;
        } else if (strcmp(name, "filament_loaded") == 0) {
            data->filament_loaded = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        }
    }

    if (needs_redraw) {
        lv_obj_invalidate(obj);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_system_path_canvas_register(void) {
    lv_xml_register_widget("system_path_canvas", system_path_xml_create, system_path_xml_apply);
    spdlog::info("[SystemPath] Registered system_path_canvas widget with XML system");
}

lv_obj_t* ui_system_path_canvas_create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[SystemPath] Cannot create: parent is null");
        return nullptr;
    }

    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj) {
        spdlog::error("[SystemPath] Failed to create object");
        return nullptr;
    }

    auto data_ptr = std::make_unique<SystemPathData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    load_theme_colors(data);
    load_theme_sizes(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, system_path_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, system_path_delete_cb, LV_EVENT_DELETE, nullptr);
    // Click handling on the canvas is no longer needed — bypass clicks are
    // captured by the BypassSpoolWidgets overlay the panel places on top.

    spdlog::debug("[SystemPath] Created widget programmatically");
    return obj;
}

void ui_system_path_canvas_set_unit_count(lv_obj_t* obj, int count) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int clamped = LV_CLAMP(count, 0, SystemPathData::MAX_UNITS);
    if (data->unit_count == clamped)
        return;
    data->unit_count = clamped;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_x(lv_obj_t* obj, int unit_index, int32_t center_x) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_x_positions[unit_index] == center_x)
        return;
    data->unit_x_positions[unit_index] = center_x;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_active_unit(lv_obj_t* obj, int unit_index) {
    auto* data = get_data(obj);
    if (!data || data->active_unit == unit_index)
        return;
    data->active_unit = unit_index;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_active_color(lv_obj_t* obj, uint32_t color) {
    auto* data = get_data(obj);
    if (!data || data->active_color == color)
        return;
    data->active_color = color;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_filament_loaded(lv_obj_t* obj, bool loaded) {
    auto* data = get_data(obj);
    if (!data || data->filament_loaded == loaded)
        return;
    data->filament_loaded = loaded;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_status_text(lv_obj_t* obj, const char* text) {
    auto* data = get_data(obj);
    if (!data)
        return;
    const char* new_text = text ? text : "";
    if (strcmp(data->status_text, new_text) == 0)
        return;
    snprintf(data->status_text, sizeof(data->status_text), "%s", new_text);
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_bypass(lv_obj_t* obj, bool has_bypass, bool bypass_active,
                                      uint32_t bypass_color) {
    auto* data = get_data(obj);
    if (!data)
        return;
    if (data->has_bypass == has_bypass && data->bypass_active == bypass_active &&
        data->bypass_color == bypass_color)
        return;
    data->has_bypass = has_bypass;
    data->bypass_active = bypass_active;
    data->bypass_color = bypass_color;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_hub_sensor(lv_obj_t* obj, int unit_index, bool has_sensor,
                                               bool triggered) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_has_hub_sensor[unit_index] == has_sensor &&
        data->unit_hub_triggered[unit_index] == triggered)
        return;
    data->unit_has_hub_sensor[unit_index] = has_sensor;
    data->unit_hub_triggered[unit_index] = triggered;
    lv_obj_invalidate(obj);
}

namespace helix::ui {
static void copy_label(char* dst, size_t cap, const char* label) {
    snprintf(dst, cap, "%s", label ? label : "");
}

void ui_system_path_canvas_set_buffer(lv_obj_t* obj, bool present, int fault, const char* label) {
    auto* data = get_data(obj);
    if (!data)
        return;
    char text[sizeof(data->buffer_label)];
    copy_label(text, sizeof(text), label);
    if (data->has_buffer == present && data->buffer_fault == fault &&
        strcmp(data->buffer_label, text) == 0)
        return;
    data->has_buffer = present;
    data->buffer_fault = fault;
    memcpy(data->buffer_label, text, sizeof(text));
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_hub(lv_obj_t* obj, int unit_index, int hub_group, bool present,
                                        int fault, const char* label) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    char text[sizeof(data->unit_buffer_label[0])];
    copy_label(text, sizeof(text), label);
    if (data->unit_hub_group[unit_index] == hub_group &&
        data->unit_has_buffer[unit_index] == present &&
        data->unit_buffer_fault[unit_index] == fault &&
        strcmp(data->unit_buffer_label[unit_index], text) == 0)
        return;
    data->unit_hub_group[unit_index] = hub_group;
    data->unit_has_buffer[unit_index] = present;
    data->unit_buffer_fault[unit_index] = fault;
    memcpy(data->unit_buffer_label[unit_index], text, sizeof(text));
    lv_obj_invalidate(obj);
}
} // namespace helix::ui

void ui_system_path_canvas_set_toolhead_sensor(lv_obj_t* obj, bool has_toolhead_sensor) {
    auto* data = get_data(obj);
    if (!data || data->has_toolhead_sensor == has_toolhead_sensor)
        return;
    data->has_toolhead_sensor = has_toolhead_sensor;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_filament_segment(lv_obj_t* obj, int segment) {
    auto* data = get_data(obj);
    const auto seg = static_cast<helix::PathSegment>(segment);
    if (!data || data->filament_segment == seg)
        return;
    data->filament_segment = seg;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_error_segment(lv_obj_t* obj, int segment) {
    auto* data = get_data(obj);
    const auto seg = static_cast<helix::PathSegment>(segment);
    if (!data || data->error_segment == seg)
        return;
    data->error_segment = seg;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_lane(lv_obj_t* obj, int unit_index, int segment,
                                         uint32_t color) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    const auto seg = static_cast<helix::PathSegment>(segment);
    if (data->unit_lane_segment[unit_index] == seg && data->unit_lane_color[unit_index] == color)
        return;
    data->unit_lane_segment[unit_index] = seg;
    data->unit_lane_color[unit_index] = color;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_unit_tools(lv_obj_t* obj, int unit_index, int tool_count,
                                          int first_tool) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_tool_count[unit_index] == tool_count &&
        data->unit_first_tool[unit_index] == first_tool)
        return;
    data->unit_tool_count[unit_index] = tool_count;
    data->unit_first_tool[unit_index] = first_tool;
    lv_obj_invalidate(obj);
}

namespace helix::ui {
void ui_system_path_canvas_set_unit_absent(lv_obj_t* obj, int unit_index, bool absent) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_absent[unit_index] == absent)
        return;
    data->unit_absent[unit_index] = absent;
    lv_obj_invalidate(obj);
}
} // namespace helix::ui

void ui_system_path_canvas_set_unit_topology(lv_obj_t* obj, int unit_index, int topology) {
    auto* data = get_data(obj);
    if (!data || unit_index < 0 || unit_index >= SystemPathData::MAX_UNITS)
        return;
    if (data->unit_topology[unit_index] == topology)
        return;
    data->unit_topology[unit_index] = topology;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_total_tools(lv_obj_t* obj, int total_tools) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int clamped = LV_CLAMP(total_tools, 0, SystemPathData::MAX_TOOLS);
    if (data->total_tools == clamped)
        return;
    data->total_tools = clamped;
    if (!data->has_virtual_numbers) {
        for (int i = 0; i < data->total_tools; ++i) {
            snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "%c%d",
                     data->tool_label_prefix, helix::ui::lane_number(i));
        }
    }
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_active_tool(lv_obj_t* obj, int tool_index) {
    auto* data = get_data(obj);
    if (!data || data->active_tool == tool_index)
        return;
    data->active_tool = tool_index;
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_current_tool(lv_obj_t* obj, int tool_index) {
    auto* data = get_data(obj);
    if (!data || data->current_tool == tool_index)
        return;
    data->current_tool = tool_index;
    if (tool_index >= 0) {
        // Single-nozzle mode keeps the AFC lane alias on purpose, and so ignores
        // tool_label_prefix. With one extruder the three numbering systems cannot
        // disagree about *which* toolhead is meant (#1229), so the alias is the
        // only informative number available — an extruder identity here would be
        // a constant "E0". Multi-nozzle badges go through tool_labels[] instead.
        snprintf(data->current_tool_label, sizeof(data->current_tool_label), "%s",
                 helix::ui::tool_label(tool_index).c_str());
    } else {
        data->current_tool_label[0] = '\0';
    }
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_tool_label_prefix(lv_obj_t* obj, char prefix) {
    auto* data = get_data(obj);
    if (!data || prefix == '\0' || data->tool_label_prefix == prefix)
        return;
    data->tool_label_prefix = prefix;
    // Reformat in place: the numbers can be unchanged while the letter flips,
    // and set_tool_virtual_numbers() short-circuits on unchanged numbers.
    for (int i = 0; i < SystemPathData::MAX_TOOLS; ++i) {
        const int n =
            data->has_virtual_numbers ? data->tool_virtual_number[i] : helix::ui::lane_number(i);
        snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "%c%d", prefix, n);
    }
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_tool_virtual_numbers(lv_obj_t* obj, const int* numbers, int count) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int n = LV_MIN(count, SystemPathData::MAX_TOOLS);
    bool changed = (data->has_virtual_numbers != (n > 0));
    if (!changed) {
        for (int i = 0; i < n && !changed; ++i) {
            if (data->tool_virtual_number[i] != numbers[i])
                changed = true;
        }
    }
    if (!changed)
        return;
    const char prefix = data->tool_label_prefix;
    for (int i = 0; i < n; ++i) {
        data->tool_virtual_number[i] = numbers[i];
        snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "%c%d", prefix, numbers[i]);
    }
    // Clear remaining entries
    for (int i = n; i < SystemPathData::MAX_TOOLS; ++i) {
        const int fallback_n = helix::ui::lane_number(i);
        data->tool_virtual_number[i] = fallback_n;
        snprintf(data->tool_labels[i], sizeof(data->tool_labels[i]), "%c%d", prefix, fallback_n);
    }
    data->has_virtual_numbers = (n > 0);
    lv_obj_invalidate(obj);
}

void ui_system_path_canvas_set_bypass_has_spool(lv_obj_t* obj, bool has_spool) {
    auto* data = get_data(obj);
    if (data && data->bypass_has_spool != has_spool) {
        data->bypass_has_spool = has_spool;
        lv_obj_invalidate(obj);
    }
}

bool ui_system_path_canvas_get_bypass_merge_pos(lv_obj_t* obj, int32_t* cx_out, int32_t* cy_out) {
    auto* data = get_data(obj);
    if (!data || !data->has_bypass) {
        return false;
    }
    // Note: this intentionally serves both single-tool AND multi-tool (toolhead
    // row) layouts. compute_bypass_geometry() places the spool clear of the
    // rightmost toolhead, so the panel-side overlay no longer collides when many
    // tools span the canvas (e.g. HTLF + toolchanger, 7 tools). In multi-tool
    // mode the draw callback omits the bypass merge tube entirely (each tool has
    // its own path), so the overlay deliberately shows a floating, tube-less
    // bypass spool — this getter still returns its anchor for that overlay.
    lv_obj_update_layout(obj);
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    if (lv_area_get_width(&obj_coords) <= 0 || lv_area_get_height(&obj_coords) <= 0) {
        return false;
    }
    BypassGeometry bg = compute_bypass_geometry(*data, obj_coords);
    if (cx_out) {
        *cx_out = bg.bypass_x;
    }
    if (cy_out) {
        *cy_out = bg.merge_y;
    }
    return true;
}

void ui_system_path_canvas_refresh(lv_obj_t* obj) {
    lv_obj_invalidate(obj);
}
