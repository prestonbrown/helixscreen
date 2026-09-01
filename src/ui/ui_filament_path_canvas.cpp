// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// filament_path_canvas widget: data registry, theme loading, LVGL lifecycle
// events, click dispatch, XML registration, and the public C API.
//
// The drawing itself lives in the sibling units (see the file map in
// ui_filament_path_internal.h): topology renderers, glyphs, animations, and
// the layered canvas machinery. This file owns the widget's state block and
// routes LVGL events into those units.

#include "ui_filament_path_canvas.h"

#include "ui_filament_path_internal.h"
#include "ui_fonts.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace helix::ui::fpath;

// ============================================================================
// Registry + theme
// ============================================================================

static std::unordered_map<lv_obj_t*, FilamentPathData*> s_registry;

namespace helix::ui::fpath {

FilamentPathData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

} // namespace helix::ui::fpath

// Load theme-aware colors, fonts, and sizes
static void load_theme_colors(FilamentPathData* data) {
    bool dark_mode = theme_manager_is_dark_mode();
    ThemeCache& theme = data->theme;

    // Use theme tokens with dark/light mode awareness
    theme.color_idle =
        theme_manager_get_color(dark_mode ? "filament_idle_dark" : "filament_idle_light");
    theme.color_error = theme_manager_get_color("filament_error");
    theme.color_hub_bg =
        theme_manager_get_color(dark_mode ? "filament_hub_bg_dark" : "filament_hub_bg_light");
    theme.color_hub_border = theme_manager_get_color(dark_mode ? "filament_hub_border_dark"
                                                               : "filament_hub_border_light");
    theme.color_nozzle = lv_color_hex(NOZZLE_UNLOADED_COLOR);
    theme.color_text = theme_manager_get_color("text");
    theme.color_bg = theme_manager_get_color("card_bg");
    theme.color_success = theme_manager_get_color("success");

    // Get responsive sizing from theme
    int32_t space_xs = theme_manager_get_spacing("space_xs");
    int32_t space_md = theme_manager_get_spacing("space_md");

    // Scale line widths based on spacing (responsive)
    theme.line_width_idle = LV_MAX(2, space_xs / 2);
    theme.line_width_active = LV_MAX(3, space_xs - 3);
    theme.sensor_radius = LV_MAX(4, space_xs);
    theme.hub_width = LV_MAX(50, space_md * 5);
    theme.border_radius = LV_MAX(4, space_xs);
    theme.extruder_scale = LV_MAX(8, space_md); // Extruder scales with space_md

    // Get responsive font from globals.xml (font_small → responsive variant)
    const char* font_name = lv_xml_get_const(nullptr, "font_small");
    theme.label_font = font_name ? lv_xml_get_font(nullptr, font_name) : &noto_sans_12;

    spdlog::trace("[FilamentPath] Theme colors loaded (dark={}, font={})", dark_mode,
                  font_name ? font_name : "fallback");
}

// ============================================================================
// Geometry helpers (shared with the topology renderers)
// ============================================================================

namespace helix::ui::fpath {

// Get slot center X relative to the canvas left edge.
// Primary: uses cached spool_container pointers for pixel-perfect alignment.
// Fallback: computes position from slot_width/overlap when slot_grid unavailable.
int32_t get_slot_x(const FilamentPathData* data, int slot_index, int32_t canvas_x1) {
    if (slot_index >= 0 && slot_index < FilamentPathData::MAX_SLOTS) {
        lv_obj_t* spool_cont = data->spool_containers[slot_index];
        if (spool_cont) {
            // When spool_container is hidden (cleared slot), its flex-computed coords
            // are invalid. Use the parent slot widget center instead — it always stays
            // visible and at the correct fixed width.
            lv_obj_t* target = lv_obj_has_flag(spool_cont, LV_OBJ_FLAG_HIDDEN)
                                   ? lv_obj_get_parent(spool_cont)
                                   : spool_cont;
            if (target) {
                lv_area_t coords;
                lv_obj_get_coords(target, &coords);
                return (coords.x1 + coords.x2) / 2 - canvas_x1;
            }
        }
    }

    // Fallback: computed position (no slot_grid available)
    int32_t slot_width = data->slot_width;
    if (data->slot_count <= 1) {
        return slot_width / 2;
    }
    int32_t slot_spacing = slot_width - data->slot_overlap;
    return slot_width / 2 + slot_index * slot_spacing;
}

} // namespace helix::ui::fpath

// Pure coordinate hit-test for an axis-aligned box. Mirrors the inline
// distance checks used by the buffer/bypass hit-tests in the click handler,
// extracted here so it can be unit-tested without an LVGL display.
// NOTE: argument order is width-then-height.
bool helix::ui::hub_box_hit(lv_point_t p, int32_t cx, int32_t cy, int32_t w, int32_t h,
                            int32_t margin) {
    return (abs(p.x - cx) <= w / 2 + margin) && (abs(p.y - cy) <= h / 2 + margin);
}

// ============================================================================
// Event handlers
// ============================================================================

// DRAW_POST: child canvases hold the heavyweight static + state-tied painting
// (refreshed by the layers module); this pass only contributes the cheap
// per-frame animation overlay on top.
static void filament_path_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    render_animation_overlay(obj, layer, data);
}

static void filament_path_click_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    FilamentPathData* data = get_data(obj);
    if (!data)
        return;

    lv_point_t point;
    lv_indev_t* indev = lv_indev_active();
    lv_indev_get_point(indev, &point);

    // Get widget dimensions
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // For PARALLEL topology (tool changers), accept clicks on toolheads AND the
    // filament line/spool area (top half of canvas, above the sensor dots)
    if (data->topology == static_cast<int>(PathTopology::PARALLEL) && data->slot_callback) {
        int32_t toolhead_y = y_off + (int32_t)(height * PARALLEL_TOOLHEAD_Y_RATIO);
        int32_t sensor_y = y_off + (int32_t)(height * PARALLEL_SENSOR_Y_RATIO);
        int32_t tool_scale = LV_MAX(6, data->theme.extruder_scale * 2 / 3);

        // Toolhead click area (bottom half)
        int32_t hit_radius_y = tool_scale * 4;
        if (abs(point.y - toolhead_y) < hit_radius_y) {
            for (int i = 0; i < data->slot_count; i++) {
                int32_t slot_x = x_off + get_slot_x(data, i, x_off);
                int32_t hit_radius_x = LV_MAX(20, tool_scale * 3);
                if (abs(point.x - slot_x) < hit_radius_x) {
                    spdlog::debug("[FilamentPath] Toolhead {} clicked (parallel topology)", i);
                    data->slot_callback(i, data->slot_user_data);
                    return;
                }
            }
        }

        // Spool/filament line click area (top of canvas down to sensor dots)
        if (point.y < sensor_y) {
            for (int i = 0; i < data->slot_count; i++) {
                int32_t slot_x = x_off + get_slot_x(data, i, x_off);
                int32_t hit_radius_x = LV_MAX(20, tool_scale * 3);
                if (abs(point.x - slot_x) < hit_radius_x) {
                    spdlog::debug("[FilamentPath] Spool {} clicked (parallel topology)", i);
                    data->slot_callback(i, data->slot_user_data);
                    return;
                }
            }
        }
    }

    // Check if buffer coil was clicked. The renderer records the exact drawn
    // box in data->hits.buffer (absolute coords); reading it avoids
    // re-deriving the clamped box dimensions and slot-midpoint center_x.
    // buffer_valid is set only when the box was actually drawn this pass.
    if (data->buffer_present && data->buffer_callback && data->hits.buffer_valid) {
        const lv_area_t& r = data->hits.buffer;
        int32_t cx = (r.x1 + r.x2) / 2;
        int32_t cy = (r.y1 + r.y2) / 2;
        if (helix::ui::hub_box_hit(point, cx, cy, r.x2 - r.x1, r.y2 - r.y1, 4)) {
            spdlog::debug("[FilamentPath] Buffer coil clicked");
            data->buffer_callback(data->buffer_user_data);
            return;
        }
    }

    // Check if the selector/hub box was clicked. The renderer records the exact
    // drawn box in data->hits.hub (absolute coords) — the single source of
    // truth — so we never re-derive geometry that could drift from what's on
    // screen. hub_valid is set only when a box was actually drawn this pass
    // (LINEAR selector / HUB), so PARALLEL is naturally excluded.
    if (data->hub_callback && data->hits.hub_valid) {
        const lv_area_t& r = data->hits.hub;
        int32_t cx = (r.x1 + r.x2) / 2;
        int32_t cy = (r.y1 + r.y2) / 2;
        if (helix::ui::hub_box_hit(point, cx, cy, r.x2 - r.x1, r.y2 - r.y1, 4)) {
            spdlog::debug("[FilamentPath] Selector/hub box clicked");
            data->hub_callback(point, data->hub_user_data);
            return;
        }
    }

    // Check if bypass spool box was clicked (right side) — check before entry area.
    // The renderer records the exact hit region in data->hits.bypass
    // (absolute coords); bypass_valid is set only when the bypass section was
    // actually drawn (!hub_only && show_bypass), keeping the hit-test in lockstep
    // with visibility. The rect's half-extents already encode the original
    // full-extent bounds (sensor_r*3 / sensor_r*4), so read with margin 0.
    if (data->show_bypass && data->bypass_callback && data->hits.bypass_valid) {
        const lv_area_t& r = data->hits.bypass;
        int32_t cx = (r.x1 + r.x2) / 2;
        int32_t cy = (r.y1 + r.y2) / 2;
        if (helix::ui::hub_box_hit(point, cx, cy, r.x2 - r.x1, r.y2 - r.y1, 0)) {
            spdlog::debug("[FilamentPath] Bypass spool box clicked");
            data->bypass_callback(data->bypass_user_data);
            return;
        }
    }

    // Check if click is in the entry area (top portion)
    int32_t entry_y = y_off + (int32_t)(height * ENTRY_Y_RATIO);
    int32_t prep_y = y_off + (int32_t)(height * PREP_Y_RATIO);

    if (point.y < entry_y - ENTRY_HIT_MARGIN_TOP || point.y > prep_y + ENTRY_HIT_MARGIN_BOTTOM)
        return; // Click not in entry area

    // Find which slot was clicked
    if (data->slot_callback) {
        for (int i = 0; i < data->slot_count; i++) {
            int32_t slot_x = x_off + get_slot_x(data, i, x_off);
            if (abs(point.x - slot_x) < 20) {
                spdlog::debug("[FilamentPath] Slot {} clicked", i);
                data->slot_callback(i, data->slot_user_data);
                return;
            }
        }
    }
}

static void filament_path_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto it = s_registry.find(obj);
    if (it != s_registry.end()) {
        // Stop any running animations before deleting
        std::unique_ptr<FilamentPathData> data(it->second);
        if (data) {
            delete_all_animations(obj);
            // Cancel any pending canvas refresh + free draw buffers.
            layered_teardown(obj, data.get());
        }
        s_registry.erase(it);
        // data automatically freed when unique_ptr goes out of scope
    }
}

// ============================================================================
// Widget construction (shared by XML create and the programmatic API)
// ============================================================================

static lv_obj_t* create_widget(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj)
        return nullptr;

    auto data_ptr = std::make_unique<FilamentPathData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    // Load theme-aware colors, fonts, and sizes
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, filament_path_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, filament_path_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(obj, filament_path_delete_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(obj, layered_size_changed_cb, LV_EVENT_SIZE_CHANGED, nullptr);

    if (!layered_setup_canvases(obj, data)) {
        spdlog::error("[FilamentPath] Canvas setup failed — widget will be blank");
    }
    return obj;
}

// ============================================================================
// XML Widget Interface
// ============================================================================

static void* filament_path_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = create_widget(static_cast<lv_obj_t*>(parent));
    if (!obj)
        return nullptr;

    spdlog::debug("[FilamentPath] Created widget");
    return obj;
}

static void filament_path_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
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

        if (strcmp(name, "topology") == 0) {
            if (strcmp(value, "linear") == 0 || strcmp(value, "0") == 0) {
                data->topology = 0;
            } else {
                data->topology = 1; // default to hub
            }
            needs_redraw = true;
        } else if (strcmp(name, "slot_count") == 0) {
            data->slot_count = LV_CLAMP(atoi(value), 1, 16);
            needs_redraw = true;
        } else if (strcmp(name, "active_slot") == 0) {
            data->active_slot = atoi(value);
            needs_redraw = true;
        } else if (strcmp(name, "filament_segment") == 0) {
            data->filament_segment = LV_CLAMP(atoi(value), 0, PATH_SEGMENT_COUNT - 1);
            needs_redraw = true;
        } else if (strcmp(name, "error_segment") == 0) {
            data->error_segment = LV_CLAMP(atoi(value), 0, PATH_SEGMENT_COUNT - 1);
            needs_redraw = true;
        } else if (strcmp(name, "anim_progress") == 0) {
            data->anim.progress = LV_CLAMP(atoi(value), 0, 100);
            needs_redraw = true;
        } else if (strcmp(name, "filament_color") == 0) {
            data->filament_color = strtoul(value, nullptr, 0);
            needs_redraw = true;
        } else if (strcmp(name, "bypass_active") == 0) {
            data->bypass_active = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        } else if (strcmp(name, "show_bypass") == 0) {
            data->show_bypass = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        } else if (strcmp(name, "hub_only") == 0) {
            data->hub_only = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            needs_redraw = true;
        }
    }

    if (needs_redraw) {
        layered_mark_dirty(obj, true, true);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_filament_path_canvas_register(void) {
    lv_xml_register_widget("filament_path_canvas", filament_path_xml_create,
                           filament_path_xml_apply);
    spdlog::info("[FilamentPath] Registered filament_path_canvas widget with XML system");
}

lv_obj_t* ui_filament_path_canvas_create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[FilamentPath] Cannot create: parent is null");
        return nullptr;
    }

    lv_obj_t* obj = create_widget(parent);
    if (!obj) {
        spdlog::error("[FilamentPath] Failed to create object");
        return nullptr;
    }

    spdlog::debug("[FilamentPath] Created widget programmatically");
    return obj;
}

void ui_filament_path_canvas_set_topology(lv_obj_t* obj, int topology) {
    auto* data = get_data(obj);
    if (!data || data->topology == topology)
        return;
    data->topology = topology;
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_hub_on_toolhead(lv_obj_t* obj, bool on_toolhead) {
    auto* data = get_data(obj);
    if (!data || data->hub_on_toolhead == on_toolhead)
        return;
    data->hub_on_toolhead = on_toolhead;
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_slot_count(lv_obj_t* obj, int count) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int clamped = LV_CLAMP(count, 1, 16);
    if (data->slot_count == clamped)
        return;
    data->slot_count = clamped;
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_slot_overlap(lv_obj_t* obj, int32_t overlap) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int32_t clamped = LV_MAX(overlap, 0);
    if (data->slot_overlap == clamped)
        return;
    data->slot_overlap = clamped;
    spdlog::trace("[FilamentPath] Slot overlap set to {}px", data->slot_overlap);
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_slot_width(lv_obj_t* obj, int32_t width) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int32_t clamped = LV_MAX(width, 20); // Minimum 20px
    if (data->slot_width == clamped)
        return;
    data->slot_width = clamped;
    spdlog::trace("[FilamentPath] Slot width set to {}px", data->slot_width);
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_slot_grid(lv_obj_t* obj, lv_obj_t* slot_grid) {
    auto* data = get_data(obj);
    if (!data)
        return;

    data->slot_grid = slot_grid;

    // Pre-cache spool_container pointers to avoid per-frame lv_obj_find_by_name
    std::memset(data->spool_containers, 0, sizeof(data->spool_containers));
    if (slot_grid) {
        int child_count =
            LV_MIN((int)lv_obj_get_child_count(slot_grid), FilamentPathData::MAX_SLOTS);
        for (int i = 0; i < child_count; i++) {
            lv_obj_t* slot = lv_obj_get_child(slot_grid, i);
            if (slot) {
                data->spool_containers[i] = lv_obj_find_by_name(slot, "spool_container");
            }
        }
        spdlog::debug("[FilamentPath] Cached {} spool_container pointers from slot_grid",
                      child_count);
    }
}

void ui_filament_path_canvas_set_active_slot(lv_obj_t* obj, int slot) {
    auto* data = get_data(obj);
    if (!data || data->active_slot == slot)
        return;

    int old_slot = data->active_slot;
    data->active_slot = slot;

    // LINEAR topology: animate output_x sliding to new slot position
    if (data->topology == 0 && slot >= 0 && old_slot >= 0) {
        lv_area_t coords;
        lv_obj_get_coords(obj, &coords);
        int32_t x_off = coords.x1;
        int32_t new_x = x_off + get_slot_x(data, slot, x_off);
        int32_t old_x = data->anim.output_x_current;
        if (old_x == 0)
            old_x = x_off + get_slot_x(data, old_slot, x_off);
        start_output_x_animation(obj, data, old_x, new_x);
    }

    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_filament_segment(lv_obj_t* obj, int segment) {
    auto* data = get_data(obj);
    if (!data)
        return;

    int new_segment = LV_CLAMP(segment, 0, PATH_SEGMENT_COUNT - 1);

    // When not in eject mode and the active slot has a prep sensor, clamp the
    // minimum displayed segment to LANE so the retract animation stops at the
    // lane sensor instead of overshooting past the slot/prep sensor.
    if (!data->eject_mode && data->active_slot >= 0 &&
        data->active_slot < FilamentPathData::MAX_SLOTS &&
        data->slot_has_prep_sensor[data->active_slot] && new_segment > 0 &&
        new_segment < static_cast<int>(PathSegment::LANE)) {
        new_segment = static_cast<int>(PathSegment::LANE);
    }

    int old_segment = data->filament_segment;

    if (new_segment == old_segment)
        return;

    // Start animation from old to new segment
    start_segment_animation(obj, data, old_segment, new_segment);
    data->filament_segment = new_segment;
    spdlog::debug("[FilamentPath] Segment changed: {} -> {} (animating)", old_segment, new_segment);

    // Stop flow animation when filament reaches a terminal position via a
    // single-step transition (normal operation). Big jumps (e.g., 0->7 initial
    // setup) are not real flow operations -- don't stop flow for those.
    if (data->anim.flow_active) {
        int step = std::abs(new_segment - old_segment);
        bool is_terminal = (new_segment == 0 || new_segment == PATH_SEGMENT_COUNT - 1);
        if (is_terminal && step <= 2) {
            stop_flow_animation(obj, data);
        }
    }

    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_error_segment(lv_obj_t* obj, int segment) {
    auto* data = get_data(obj);
    if (!data)
        return;

    int new_error = LV_CLAMP(segment, 0, PATH_SEGMENT_COUNT - 1);
    int old_error = data->error_segment;

    if (new_error == old_error)
        return;

    data->error_segment = new_error;

    // Start or stop error pulse animation
    if (new_error > 0 && old_error == 0) {
        // Error appeared - start pulsing
        start_error_pulse(obj, data);
        spdlog::debug("[FilamentPath] Error at segment {} - starting pulse", new_error);
    } else if (new_error == 0 && old_error > 0) {
        // Error cleared - stop pulsing
        stop_error_pulse(obj, data);
        spdlog::debug("[FilamentPath] Error cleared - stopping pulse");
    }

    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_anim_progress(lv_obj_t* obj, int progress) {
    auto* data = get_data(obj);
    if (!data)
        return;
    int clamped = LV_CLAMP(progress, 0, 100);
    if (data->anim.progress == clamped)
        return;
    data->anim.progress = clamped;
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_filament_color(lv_obj_t* obj, uint32_t color) {
    auto* data = get_data(obj);
    if (!data || data->filament_color == color)
        return;
    data->filament_color = color;
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_refresh(lv_obj_t* obj) {
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_slot_callback(lv_obj_t* obj, filament_path_slot_cb_t cb,
                                               void* user_data) {
    auto* data = get_data(obj);
    if (data) {
        data->slot_callback = cb;
        data->slot_user_data = user_data;
    }
}

void ui_filament_path_canvas_set_hub_callback(lv_obj_t* obj, hub_callback_t cb, void* user_data) {
    auto* data = get_data(obj);
    if (data) {
        data->hub_callback = cb;
        data->hub_user_data = user_data;
    }
}

void ui_filament_path_canvas_animate_segment(lv_obj_t* obj, int from_segment, int to_segment) {
    auto* data = get_data(obj);
    if (!data)
        return;

    int from = LV_CLAMP(from_segment, 0, PATH_SEGMENT_COUNT - 1);
    int to = LV_CLAMP(to_segment, 0, PATH_SEGMENT_COUNT - 1);

    if (from != to) {
        start_segment_animation(obj, data, from, to);
        data->filament_segment = to;
    }
}

bool ui_filament_path_canvas_is_animating(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (!data)
        return false;

    return data->anim.segment_active || data->anim.error_pulse_active || data->anim.flow_active;
}

void ui_filament_path_canvas_stop_animations(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (!data)
        return;

    stop_segment_animation(obj, data);
    stop_error_pulse(obj, data);
    stop_flow_animation(obj, data);
    stop_heat_pulse(obj, data);
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_slot_filament(lv_obj_t* obj, int slot_index, int segment,
                                               uint32_t color) {
    auto* data = get_data(obj);
    if (!data || slot_index < 0 || slot_index >= FilamentPathData::MAX_SLOTS)
        return;

    auto& state = data->slot_filament_states[slot_index];
    PathSegment new_segment = static_cast<PathSegment>(segment);

    if (state.segment != new_segment || state.color != color) {
        state.segment = new_segment;
        state.color = color;
        spdlog::trace("[FilamentPath] Slot {} filament: segment={}, color=0x{:06X}", slot_index,
                      segment, color);
        layered_mark_dirty(obj, true, true);
    }
}

int ui_filament_path_canvas_get_slot_filament(lv_obj_t* obj, int slot_index) {
    auto* data = get_data(obj);
    if (!data || slot_index < 0 || slot_index >= FilamentPathData::MAX_SLOTS)
        return static_cast<int>(PathSegment::NONE);
    return static_cast<int>(data->slot_filament_states[slot_index].segment);
}

void ui_filament_path_canvas_set_slot_prep_sensor(lv_obj_t* obj, int slot, bool has_sensor) {
    auto* data = get_data(obj);
    if (!data || slot < 0 || slot >= FilamentPathData::MAX_SLOTS)
        return;
    if (data->slot_has_prep_sensor[slot] != has_sensor) {
        data->slot_has_prep_sensor[slot] = has_sensor;
        spdlog::trace("[FilamentPath] Slot {} prep sensor: {}", slot, has_sensor);
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_slot_mapped_tool(lv_obj_t* obj, int slot, int tool) {
    auto* data = get_data(obj);
    if (!data || slot < 0 || slot >= FilamentPathData::MAX_SLOTS)
        return;
    if (data->mapped_tool[slot] != tool) {
        data->mapped_tool[slot] = tool;
        layered_mark_dirty(obj, true, true);
    }
}

// Declared in ui_filament_path_internal.h inside helix::ui::fpath; the
// ui_filament_path_canvas_* entry points around it are deliberately global.
namespace helix::ui::fpath {

void format_tool_badge_label(const FilamentPathData* data, int lane, int fallback_tool, char* out,
                             size_t out_size) {
    if (data && data->use_extruder_identity && lane >= 0 && lane < FilamentPathData::MAX_SLOTS &&
        data->extruder_tool[lane] >= 0) {
        snprintf(out, out_size, "E%d", data->extruder_tool[lane]);
        return;
    }
    snprintf(out, out_size, "T%d", fallback_tool);
}

} // namespace helix::ui::fpath

void ui_filament_path_canvas_set_extruder_tools(lv_obj_t* obj, const int* tools, int count) {
    auto* data = get_data(obj);
    if (!data)
        return;

    const int n = LV_MIN(count, FilamentPathData::MAX_SLOTS);
    // Identity is all-or-nothing per unit: badging some toolheads by extruder
    // and the rest by lane alias would be worse than being uniformly legacy.
    bool complete = (n > 0 && n >= data->slot_count);
    for (int i = 0; i < n && complete; ++i) {
        complete = (tools[i] >= 0);
    }

    bool changed = (data->use_extruder_identity != complete);
    for (int i = 0; i < FilamentPathData::MAX_SLOTS; ++i) {
        const int v = (i < n) ? tools[i] : -1;
        if (data->extruder_tool[i] != v) {
            data->extruder_tool[i] = v;
            changed = true;
        }
    }
    data->use_extruder_identity = complete;
    if (changed) {
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_slot_hub_routed(lv_obj_t* obj, int slot, bool is_hub) {
    auto* data = get_data(obj);
    if (!data || slot < 0 || slot >= FilamentPathData::MAX_SLOTS)
        return;
    if (data->slot_is_hub_routed[slot] != is_hub) {
        data->slot_is_hub_routed[slot] = is_hub;
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_eject_mode(lv_obj_t* obj, bool eject) {
    auto* data = get_data(obj);
    if (!data)
        return;
    data->eject_mode = eject;
}

void ui_filament_path_canvas_clear_slot_filaments(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (!data)
        return;

    bool changed = false;
    for (int i = 0; i < FilamentPathData::MAX_SLOTS; i++) {
        if (data->slot_filament_states[i].segment != PathSegment::NONE) {
            data->slot_filament_states[i].segment = PathSegment::NONE;
            data->slot_filament_states[i].color = 0x808080;
            changed = true;
        }
    }

    if (changed) {
        spdlog::trace("[FilamentPath] Cleared all slot filament states");
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_show_bypass(lv_obj_t* obj, bool show) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->show_bypass != show) {
        data->show_bypass = show;
        spdlog::debug("[FilamentPath] Show bypass: {}", show ? "yes" : "no");
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_bypass_active(lv_obj_t* obj, bool active) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->bypass_active != active) {
        data->bypass_active = active;
        spdlog::debug("[FilamentPath] Bypass mode: {}", active ? "active" : "inactive");
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_bypass_callback(lv_obj_t* obj, filament_path_bypass_cb_t cb,
                                                 void* user_data) {
    auto* data = get_data(obj);
    if (data) {
        data->bypass_callback = cb;
        data->bypass_user_data = user_data;
    }
}

void ui_filament_path_canvas_set_buffer_callback(lv_obj_t* obj, filament_path_buffer_cb_t cb,
                                                 void* user_data) {
    auto* data = get_data(obj);
    if (data) {
        data->buffer_callback = cb;
        data->buffer_user_data = user_data;
    }
}

void ui_filament_path_canvas_set_hub_only(lv_obj_t* obj, bool hub_only) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->hub_only != hub_only) {
        data->hub_only = hub_only;
        spdlog::debug("[FilamentPath] Hub-only mode: {}", hub_only ? "on" : "off");
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_heat_active(lv_obj_t* obj, bool active) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->anim.heat_active != active) {
        data->anim.heat_active = active;

        if (active) {
            start_heat_pulse(obj, data);
            spdlog::debug("[FilamentPath] Heat glow: active");
        } else {
            stop_heat_pulse(obj, data);
            spdlog::debug("[FilamentPath] Heat glow: inactive");
        }

        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_buffer_fault_state(lv_obj_t* obj, int state) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->buffer_fault_state != state) {
        data->buffer_fault_state = state;
        spdlog::debug("[FilamentPath] Buffer fault state: {}", state);
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_buffer_info(lv_obj_t* obj, bool present, int state) {
    auto* data = get_data(obj);
    if (!data)
        return;

    state = LV_CLAMP(0, state, 2);
    if (data->buffer_present != present || data->buffer_state != state) {
        data->buffer_present = present;
        data->buffer_state = state;
        spdlog::debug("[FilamentPath] Buffer info: present={}, state={}", present, state);
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_buffer_bias(lv_obj_t* obj, float bias) {
    auto* data = get_data(obj);
    if (data) {
        data->buffer_bias = bias;
        layered_mark_dirty(obj, true, true);
    }
}

void ui_filament_path_canvas_set_bypass_color(lv_obj_t* obj, uint32_t color) {
    auto* data = get_data(obj);
    if (!data || data->bypass_color == color)
        return;
    data->bypass_color = color;
    layered_mark_dirty(obj, true, true);
}

void ui_filament_path_canvas_set_bypass_has_spool(lv_obj_t* obj, bool has_spool) {
    auto* data = get_data(obj);
    if (!data || data->bypass_has_spool == has_spool)
        return;
    data->bypass_has_spool = has_spool;
    layered_mark_dirty(obj, true, true);
}

bool ui_filament_path_canvas_get_bypass_merge_pos(lv_obj_t* obj, int32_t* cx_out, int32_t* cy_out) {
    auto* data = get_data(obj);
    if (!data || data->hub_only || !data->show_bypass) {
        return false;
    }
    lv_obj_update_layout(obj);
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int32_t width = lv_area_get_width(&obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (cx_out) {
        *cx_out = obj_coords.x1 + (int32_t)(width * BYPASS_X_RATIO);
    }
    if (cy_out) {
        *cy_out = obj_coords.y1 + (int32_t)(height * BYPASS_MERGE_Y_RATIO);
    }
    return true;
}
