// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail.h"

#include "ui/ams_drawing_utils.h"
#include "ui_ams_slot.h"
#include "ui_filament_path_canvas.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "printer_detector.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

// ============================================================================
// 3D Tray Box Drawing
// ============================================================================

/// Helper: create lv_point_precise_t from int32_t (avoids narrowing on float targets)
static inline lv_point_precise_t pt(int32_t x, int32_t y) {
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}

/// Shared geometry for 3D tray box draw callbacks
struct TrayBoxData {
    int32_t tray_height = 0; ///< Height of the front face
    int32_t dx = 0;          ///< Horizontal inset for back face (each side)
    int32_t dy = 0;          ///< Vertical offset for back face (upward)
    lv_color_t tray_bg = {}; ///< Tray fill color
    lv_opa_t tray_opa = 100; ///< Tray fill opacity
};

/// Oblique projection constants matching spool ELLIPSE_RATIO (0.45)
static constexpr int DEPTH_PCT = 40;      ///< Depth as % of tray height
static constexpr int DY_PCT = 45;         ///< Vertical depth as % of horizontal depth
static constexpr int MIN_DEPTH_PX = 4;
static constexpr int MIN_DY_PX = 2;

/// Lighten/darken amounts for 3D face shading
static constexpr uint8_t SHADE_BACK_WALL = 25;
static constexpr uint8_t SHADE_LEFT_WALL = 15;
static constexpr uint8_t SHADE_RIGHT_WALL = 15;
static constexpr uint8_t SHADE_FRONT_BORDER = 20;
static constexpr uint8_t SHADE_BRIGHT_EDGE = 30;
static constexpr uint8_t SHADE_DIM_EDGE = 15;

/// Static data shared between both tray draw callbacks.
/// Assumes only one ams_unit_detail component is active at a time
/// (AmsPanel and AmsOverviewPanel each show one unit detail; panels are singletons).
static TrayBoxData s_tray_box;

/// Resolved front/back face coordinates for oblique projection
struct TrayFaceCoords {
    int32_t ft, fb, fl, fr; ///< Front face: top, bottom, left, right
    int32_t bt, bb, bl, br; ///< Back face: top, bottom, left, right
};

/// Compute front and back face coordinates from object bounds and tray box geometry
static TrayFaceCoords compute_face_coords(const lv_area_t& coords) {
    TrayFaceCoords f;
    f.ft = coords.y2 - s_tray_box.tray_height + 1; // +1: LVGL coords are inclusive
    f.fb = coords.y2;
    f.fl = coords.x1;
    f.fr = coords.x2 - s_tray_box.dx;

    f.bt = f.ft - s_tray_box.dy;
    f.bb = f.fb - s_tray_box.dy;
    f.bl = f.fl + s_tray_box.dx;
    f.br = coords.x2;
    return f;
}

/// Draw a quad (parallelogram) as 2 triangles: p0-p1-p2 and p0-p2-p3
static void draw_quad(lv_layer_t* layer, lv_color_t color, lv_opa_t opa, int32_t x0, int32_t y0,
                      int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3) {
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = opa;

    lv_point_precise_set(&tri.p[0], x0, y0);
    lv_point_precise_set(&tri.p[1], x1, y1);
    lv_point_precise_set(&tri.p[2], x2, y2);
    lv_draw_triangle(layer, &tri);

    lv_point_precise_set(&tri.p[1], x2, y2);
    lv_point_precise_set(&tri.p[2], x3, y3);
    lv_draw_triangle(layer, &tri);
}

/// Draw callback for back wall -- attached to slot_grid via LV_EVENT_DRAW_MAIN
/// so it renders BEHIND the spool child widgets (open-top box, rear wall).
static void tray_back_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer || s_tray_box.dy <= 0)
        return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    auto f = compute_face_coords(coords);

    lv_color_t bg = s_tray_box.tray_bg;
    lv_opa_t opa = s_tray_box.tray_opa;

    // Back wall parallelogram: front_TL -> front_TR -> back_TR -> back_TL
    draw_quad(layer, ams_draw::lighten_color(bg, SHADE_BACK_WALL), opa, f.fl, f.ft, f.fr, f.ft,
              f.br, f.bt, f.bl, f.bt);

    // Dim edge highlights on back wall (behind spools)
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = ams_draw::lighten_color(bg, SHADE_DIM_EDGE);
    line_dsc.opa = LV_OPA_40;
    line_dsc.width = 1;

    line_dsc.p1 = pt(f.bl, f.bt);
    line_dsc.p2 = pt(f.br, f.bt);
    lv_draw_line(layer, &line_dsc);

    line_dsc.p1 = pt(f.bl, f.bb);
    line_dsc.p2 = pt(f.br, f.bb);
    lv_draw_line(layer, &line_dsc);
}

/// Draw callback for front face + side walls (rendered IN FRONT of spools)
static void tray_front_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer || s_tray_box.dy <= 0)
        return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    auto f = compute_face_coords(coords);

    lv_color_t bg = s_tray_box.tray_bg;
    lv_opa_t opa = s_tray_box.tray_opa;

    // Edge line styles -- bright for visible edges, dim for obscured
    lv_draw_line_dsc_t bright_edge;
    lv_draw_line_dsc_init(&bright_edge);
    bright_edge.color = ams_draw::lighten_color(bg, SHADE_BRIGHT_EDGE);
    bright_edge.opa = LV_OPA_60;
    bright_edge.width = 1;

    lv_draw_line_dsc_t dim_edge;
    lv_draw_line_dsc_init(&dim_edge);
    dim_edge.color = ams_draw::lighten_color(bg, SHADE_DIM_EDGE);
    dim_edge.opa = LV_OPA_40;
    dim_edge.width = 1;

    // Left side wall (receding -- darker, dim edges)
    draw_quad(layer, ams_draw::darken_color(bg, SHADE_LEFT_WALL), opa, f.fl, f.ft, f.bl, f.bt,
              f.bl, f.bb, f.fl, f.fb);
    dim_edge.p1 = pt(f.fl, f.ft);
    dim_edge.p2 = pt(f.bl, f.bt);
    lv_draw_line(layer, &dim_edge);
    dim_edge.p1 = pt(f.fl, f.fb);
    dim_edge.p2 = pt(f.bl, f.bb);
    lv_draw_line(layer, &dim_edge);
    dim_edge.p1 = pt(f.bl, f.bt);
    dim_edge.p2 = pt(f.bl, f.bb);
    lv_draw_line(layer, &dim_edge);

    // Right side wall (visible -- lighter, bright edges)
    draw_quad(layer, ams_draw::lighten_color(bg, SHADE_RIGHT_WALL), opa, f.fr, f.ft, f.br, f.bt,
              f.br, f.bb, f.fr, f.fb);
    bright_edge.p1 = pt(f.fr, f.ft);
    bright_edge.p2 = pt(f.br, f.bt);
    lv_draw_line(layer, &bright_edge);
    bright_edge.p1 = pt(f.fr, f.fb);
    bright_edge.p2 = pt(f.br, f.bb);
    lv_draw_line(layer, &bright_edge);

    // Front face (main rectangle, drawn last so it's on top)
    lv_area_t front_area = {f.fl, f.ft, f.fr, f.fb};
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = bg;
    rect_dsc.bg_opa = opa;
    rect_dsc.radius = 0;
    rect_dsc.border_color = ams_draw::lighten_color(bg, SHADE_FRONT_BORDER);
    rect_dsc.border_opa = LV_OPA_60;
    rect_dsc.border_width = 1;
    rect_dsc.border_side = LV_BORDER_SIDE_TOP;
    lv_draw_rect(layer, &rect_dsc, &front_area);
}

AmsDetailWidgets ams_detail_find_widgets(lv_obj_t* root) {
    AmsDetailWidgets w;
    if (!root)
        return w;

    w.root = root;
    w.slot_grid = lv_obj_find_by_name(root, "slot_grid");
    w.slot_tray = lv_obj_find_by_name(root, "slot_tray");
    w.labels_layer = lv_obj_find_by_name(root, "labels_layer");
    w.badge_layer = lv_obj_find_by_name(root, "badge_layer");

    if (!w.slot_grid) {
        spdlog::warn("[AmsDetail] slot_grid not found in ams_unit_detail");
    }

    return w;
}

/// Dim spool on press, restore on release — visual feedback for slot taps.
/// LVGL canvas widgets don't support transform_scale (raw pixel buffers),
/// so we use opacity for the flash effect.
static void slot_pressed_cb(lv_event_t* e) {
    lv_obj_t* slot = lv_event_get_current_target_obj(e);
    lv_obj_t* spool = lv_obj_find_by_name(slot, "spool_container");
    if (spool) {
        lv_obj_set_style_opa(spool, 140, 0);
    }
}

static void slot_released_cb(lv_event_t* e) {
    lv_obj_t* slot = lv_event_get_current_target_obj(e);
    lv_obj_t* spool = lv_obj_find_by_name(slot, "spool_container");
    if (spool) {
        lv_obj_set_style_opa(spool, LV_OPA_COVER, 0);
    }
}

static void ams_detail_add_slot_press_feedback(lv_obj_t* slot) {
    lv_obj_add_event_cb(slot, slot_pressed_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(slot, slot_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slot, slot_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

AmsDetailSlotResult ams_detail_create_slots(AmsDetailWidgets& w, lv_obj_t* slot_widgets[],
                                            int max_slots, int unit_index, lv_event_cb_t click_cb,
                                            void* user_data) {
    AmsDetailSlotResult result;

    if (!w.slot_grid)
        return result;

    // Determine slot count and offset from backend
    int count = 0;
    int slot_offset = 0;

    auto* backend = AmsState::instance().get_backend();
    if (backend) {
        AmsSystemInfo info = backend->get_system_info();
        if (unit_index >= 0 && unit_index < static_cast<int>(info.units.size())) {
            count = info.units[unit_index].slot_count;
            slot_offset = info.units[unit_index].first_slot_global_index;
        } else {
            count = info.total_slots;
        }
    }

    if (count <= 0)
        return result;
    if (count > max_slots) {
        spdlog::warn("[AmsDetail] Clamping slot_count {} to max {}", count, max_slots);
        count = max_slots;
    }

    // Create slot widgets via XML system
    for (int i = 0; i < count; ++i) {
        lv_obj_t* slot = static_cast<lv_obj_t*>(lv_xml_create(w.slot_grid, "ams_slot", nullptr));
        if (!slot) {
            spdlog::error("[AmsDetail] Failed to create ams_slot for index {}", i);
            continue;
        }

        int global_index = i + slot_offset;
        ui_ams_slot_set_index(slot, global_index);
        ui_ams_slot_set_layout_info(slot, i, count);

        slot_widgets[i] = slot;
        lv_obj_set_user_data(slot, reinterpret_cast<void*>(static_cast<intptr_t>(global_index)));
        lv_obj_add_event_cb(slot, click_cb, LV_EVENT_CLICKED, user_data);

        // Add visual press feedback (opacity flash on touch)
        ams_detail_add_slot_press_feedback(slot);
    }

    result.slot_count = count;

    // Calculate and apply slot sizing
    lv_obj_t* slot_area = lv_obj_get_parent(w.slot_grid);
    lv_obj_update_layout(slot_area);
    int32_t available_width = lv_obj_get_content_width(slot_area);
    result.layout = calculate_ams_slot_layout(available_width, count);

    lv_obj_set_style_pad_column(w.slot_grid, result.layout.overlap > 0 ? -result.layout.overlap : 0,
                                LV_PART_MAIN);

    // Center slots within the tray by adding left padding for the rounding remainder
    if (result.layout.centering_offset > 0) {
        lv_obj_set_style_pad_left(w.slot_grid, result.layout.centering_offset, LV_PART_MAIN);
    }

    for (int i = 0; i < count; ++i) {
        if (slot_widgets[i]) {
            lv_obj_set_width(slot_widgets[i], result.layout.slot_width);
        }
    }

    spdlog::debug("[AmsDetail] Created {} slots (offset={}, width={}, overlap={}, center_pad={})",
                  count, slot_offset, result.layout.slot_width, result.layout.overlap,
                  result.layout.centering_offset);

    return result;
}

void ams_detail_destroy_slots(AmsDetailWidgets& w, lv_obj_t* slot_widgets[], int& slot_count) {
    (void)w; // Reserved for future use (e.g. labels_layer cleanup)

    for (int i = 0; i < slot_count; ++i) {
        helix::ui::safe_delete(slot_widgets[i]);
        slot_widgets[i] = nullptr;
    }
    slot_count = 0;
}

void ams_detail_update_tray(AmsDetailWidgets& w) {
    if (!w.slot_tray || !w.slot_grid)
        return;

    // Tool changers don't have a physical tray/housing
    auto* backend = AmsState::instance().get_backend(0);
    if (backend && backend->get_type() == AmsType::TOOL_CHANGER) {
        lv_obj_add_flag(w.slot_tray, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(w.slot_tray, LV_OBJ_FLAG_HIDDEN);

    lv_obj_update_layout(w.slot_grid);
    int32_t grid_height = lv_obj_get_height(w.slot_grid);
    if (grid_height <= 0)
        return;

    int32_t tray_height = grid_height / 4;
    if (tray_height < 20)
        tray_height = 20;

    // 3D depth matching spool oblique projection (ELLIPSE_RATIO = 0.45)
    int32_t depth = std::max(tray_height * DEPTH_PCT / 100, MIN_DEPTH_PX);
    int32_t dx = depth;
    int32_t dy = std::max(depth * DY_PCT / 100, MIN_DY_PX);

    // Update shared draw data
    s_tray_box.tray_height = tray_height;
    s_tray_box.dx = dx;
    s_tray_box.dy = dy;
    s_tray_box.tray_bg = lv_color_hex(0x505050); // mirrors XML const #tray_bg
    s_tray_box.tray_opa = 100;                   // mirrors XML const #tray_bg_opa

    // Objects enlarged: +dy height for top wall, +dx width for right side wall
    // Front face at LEFT portion; back face shifts RIGHT by dx
    int32_t total_height = tray_height + dy;

    // Keep tray at 100% width. The front face is inset by dx on the right
    // so the right side wall fits within the same bounds.
    lv_obj_set_height(w.slot_tray, total_height);
    lv_obj_align(w.slot_tray, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Attach draw callbacks once per object instance.
    // Use LV_OBJ_FLAG_USER_1 as guard — user_data may already be set by XML (L069).

    // Back wall on slot_grid (DRAW_MAIN = behind spool children)
    if (!lv_obj_has_flag(w.slot_grid, LV_OBJ_FLAG_USER_1)) {
        lv_obj_add_flag(w.slot_grid, LV_OBJ_FLAG_USER_1);
        lv_obj_add_event_cb(w.slot_grid, tray_back_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    }
    // Front face + side walls on slot_tray (IN FRONT of spools)
    if (!lv_obj_has_flag(w.slot_tray, LV_OBJ_FLAG_USER_1)) {
        lv_obj_add_flag(w.slot_tray, LV_OBJ_FLAG_USER_1);
        lv_obj_add_event_cb(w.slot_tray, tray_front_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    }

    spdlog::debug("[AmsDetail] Tray 3D box: {}px front, depth={}, dx={}, dy={}", tray_height, depth,
                  dx, dy);
}

void ams_detail_update_labels(AmsDetailWidgets& w, lv_obj_t* slot_widgets[], int slot_count,
                              const AmsSlotLayout& layout) {
    if (!w.labels_layer || slot_count <= 4)
        return;

    lv_obj_clean(w.labels_layer);

    int32_t slot_spacing = layout.slot_width - layout.overlap;

    for (int i = 0; i < slot_count; ++i) {
        if (slot_widgets[i]) {
            // Formula matches slot_grid flex positions, plus centering offset
            int32_t slot_center_x =
                layout.centering_offset + layout.slot_width / 2 + i * slot_spacing;
            ui_ams_slot_move_label_to_layer(slot_widgets[i], w.labels_layer, slot_center_x);
        }
    }

    spdlog::debug("[AmsDetail] Moved {} labels to overlay layer", slot_count);
}

void ams_detail_update_badges(AmsDetailWidgets& w, lv_obj_t* slot_widgets[], int slot_count,
                              const AmsSlotLayout& layout) {
    if (!w.badge_layer)
        return;

    int32_t slot_spacing = layout.slot_width - layout.overlap;

    for (int i = 0; i < slot_count; ++i) {
        if (slot_widgets[i]) {
            int32_t slot_center_x =
                layout.centering_offset + layout.slot_width / 2 + i * slot_spacing;
            ui_ams_slot_move_badge_to_layer(slot_widgets[i], w.badge_layer, slot_center_x);
        }
    }

    spdlog::debug("[AmsDetail] Moved {} badges to overlay layer", slot_count);
}

void ams_detail_setup_path_canvas(lv_obj_t* canvas, lv_obj_t* slot_grid, int unit_index,
                                  bool hub_only) {
    if (!canvas)
        return;

    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;

    AmsSystemInfo info = backend->get_system_info();

    // Hub-only mode: only draw slots -> hub, skip downstream
    ui_filament_path_canvas_set_hub_only(canvas, hub_only);

    // Hide bypass path for backends that don't support it (e.g. tool changers)
    ui_filament_path_canvas_set_show_bypass(canvas, info.supports_bypass);

    // Determine slot count and offset for this unit
    int slot_count = info.total_slots;
    int slot_offset = 0;
    if (unit_index >= 0 && unit_index < static_cast<int>(info.units.size())) {
        slot_count = info.units[unit_index].slot_count;
        slot_offset = info.units[unit_index].first_slot_global_index;
    }

    ui_filament_path_canvas_set_slot_count(canvas, slot_count);
    PathTopology topo =
        (unit_index >= 0) ? backend->get_unit_topology(unit_index) : backend->get_topology();
    ui_filament_path_canvas_set_topology(canvas, static_cast<int>(topo));

    // Pass slot_grid reference so draw callback can read actual slot positions
    // at render time — avoids setup-vs-draw timing mismatches across breakpoints.
    if (slot_grid) {
        ui_filament_path_canvas_set_slot_grid(canvas, slot_grid);

        // Still set slot_width/overlap as fallback for get_slot_x() computed positions
        lv_obj_t* slot_area = lv_obj_get_parent(slot_grid);
        lv_obj_update_layout(slot_area);
        int32_t available_width = lv_obj_get_content_width(slot_area);
        auto layout = calculate_ams_slot_layout(available_width, slot_count);
        ui_filament_path_canvas_set_slot_width(canvas, layout.slot_width);
        ui_filament_path_canvas_set_slot_overlap(canvas, layout.overlap);
    }

    // Map active slot to local index for unit-scoped views
    int active_slot = info.current_slot;
    if (unit_index >= 0) {
        int local_active = info.current_slot - slot_offset;
        active_slot = (local_active >= 0 && local_active < slot_count) ? local_active : -1;
    }
    ui_filament_path_canvas_set_active_slot(canvas, active_slot);

    // Set filament color from active slot
    int global_active = (unit_index >= 0) ? active_slot + slot_offset : active_slot;
    if (global_active >= 0) {
        SlotInfo slot_info = backend->get_slot_info(global_active);
        ui_filament_path_canvas_set_filament_color(canvas, slot_info.color_rgb);
    }

    // Set filament and error segments
    PathSegment segment = backend->get_filament_segment();
    ui_filament_path_canvas_set_filament_segment(canvas, static_cast<int>(segment));

    PathSegment error_seg = backend->infer_error_segment();
    ui_filament_path_canvas_set_error_segment(canvas, static_cast<int>(error_seg));

    // Set per-slot prep sensor capability flags
    for (int i = 0; i < slot_count; ++i) {
        bool has_prep = backend->slot_has_prep_sensor(slot_offset + i);
        ui_filament_path_canvas_set_slot_prep_sensor(canvas, i, has_prep);
    }

    // Set per-slot filament states (using local indices for unit-scoped views)
    ui_filament_path_canvas_clear_slot_filaments(canvas);
    for (int i = 0; i < slot_count; ++i) {
        int global_idx = i + slot_offset;
        PathSegment slot_seg = backend->get_slot_filament_segment(global_idx);
        if (slot_seg != PathSegment::NONE) {
            SlotInfo si = backend->get_slot_info(global_idx);
            ui_filament_path_canvas_set_slot_filament(canvas, i, static_cast<int>(slot_seg),
                                                      si.color_rgb);
        }
    }

    // Set buffer fault state on hub (AFC TurtleNeck buffer health)
    // unit_index == -1 means single-unit view (use unit 0)
    int buffer_fault = 0; // 0=healthy, 1=warning, 2=fault
    int effective_unit = (unit_index >= 0) ? unit_index : 0;
    if (effective_unit < static_cast<int>(info.units.size())) {
        const auto& unit = info.units[effective_unit];
        if (unit.buffer_health.has_value() && unit.buffer_health->fault_detection_enabled &&
            unit.buffer_health->distance_to_fault >= 0.0f) {
            if (unit.buffer_health->distance_to_fault >= 50.0f) {
                buffer_fault = 2; // At or past fault threshold — red tint
            } else if (unit.buffer_health->distance_to_fault > 0.0f) {
                buffer_fault = 1; // Approaching fault — yellow tint
            }
        }
    }
    // HH sync feedback → fault state based on bias magnitude
    // Use same thresholds as buffer meter: <0.3 green, 0.3-0.7 orange, >0.7 red
    if (buffer_fault == 0 && info.type == AmsType::HAPPY_HARE &&
        info.sync_feedback_bias > -1.5f) {
        float abs_bias = std::fabs(info.sync_feedback_bias);
        if (abs_bias >= 0.7f) {
            buffer_fault = 2;
        } else if (abs_bias >= 0.3f) {
            buffer_fault = 1;
        }
    }

    ui_filament_path_canvas_set_buffer_fault_state(canvas, buffer_fault);

    // Determine buffer presence and state for path canvas visualization
    bool buffer_present = false;
    int buffer_state = 0; // 0=neutral, 1=compressed, 2=tension

    // AFC: buffer present when buffer_health populated
    if (effective_unit < static_cast<int>(info.units.size())) {
        const auto& unit = info.units[effective_unit];
        if (unit.buffer_health.has_value()) {
            buffer_present = true;
            const auto& st = unit.buffer_health->state;
            if (st == "Advancing")
                buffer_state = 1; // Compressed/tight
            else if (st == "Trailing")
                buffer_state = 2; // Tension/stretched
        }
    }

    // HH: sync_feedback_state indicates buffer
    if (!buffer_present && info.type == AmsType::HAPPY_HARE) {
        const auto& sf = info.sync_feedback_state;
        if (!sf.empty() && sf != "disabled") {
            buffer_present = true;
            if (sf == "compressed")
                buffer_state = 1;
            else if (sf == "tension")
                buffer_state = 2;
        }
    }

    ui_filament_path_canvas_set_buffer_info(canvas, buffer_present, buffer_state);

    // Set proportional bias for Happy Hare
    if (info.type == AmsType::HAPPY_HARE && info.sync_feedback_bias > -1.5f) {
        ui_filament_path_canvas_set_buffer_bias(canvas, info.sync_feedback_bias);
    } else {
        ui_filament_path_canvas_set_buffer_bias(canvas, -2.0f); // discrete mode
    }

    // Set external spool color and assignment state
    auto ext_spool = AmsState::instance().get_external_spool_info();
    ui_filament_path_canvas_set_bypass_has_spool(canvas, ext_spool.has_value());
    if (ext_spool.has_value()) {
        ui_filament_path_canvas_set_bypass_color(canvas, ext_spool->color_rgb);
    }

    ui_filament_path_canvas_refresh(canvas);

    spdlog::debug("[AmsDetail] Path canvas configured: slots={}, unit={}, hub_only={}", slot_count,
                  unit_index, hub_only);
}
