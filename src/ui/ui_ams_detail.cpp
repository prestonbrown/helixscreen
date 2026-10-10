// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail.h"

#include "ui_ams_slot.h"
#include "ui_bypass_spool_widget.h"
#include "ui_effects.h"
#include "ui_error_reporting.h"
#include "ui_filament_path_canvas.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "ams_tray_projection.h"
#include "app_globals.h" // get_printer_state: the print lifecycle the clear guard reads
#include "buffer_reading.h"
#include "clog_meter_geometry.h"
#include "display_numbering.h"
#include "filament_op_dispatch.h"      // EXTERNAL_SPOOL_SLOT: the bypass sentinel
#include "filament_op_slot_resolver.h" // clear_spool_blocked_by_print: the print guard
#include "filament_tube_stroker.h"
#include "printer_detector.h"
#include "printer_state.h" // PrinterState, complete for get_print_lifecycle()
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Dry-box unit: box, spools on its floor, glass lid
// ============================================================================
// One camera (ams_tray_projection.h) for the spools, their box and the lid.
// The geometry is computed by ams_detail_update_tray() relative to the
// slot_container and drawn by two callbacks: the inside faces behind the
// spools (slot_grid DRAW_MAIN), the front, side and glass in front of them
// (slot_tray DRAW_POST).

namespace tray = helix::ui::tray;

namespace {

constexpr int CAP_POINTS = 17;
constexpr int MAX_LIDS = AMS_DETAIL_MAX_SLOTS;

// Opacities of the faces and glass, dark theme first.
struct TrayOpacities {
    lv_opa_t front, edge, shell, glass, cap, glass_edge;
    float sheen;
};
constexpr TrayOpacities DARK_OPA{140, 140, 204, 15, 36, 153, 0.26f};
constexpr TrayOpacities LIGHT_OPA{140, 153, 204, 13, 26, 140, 0.60f};

struct TrayColors {
    lv_color_t front, wall, floor, back, side, edge, shell, glass, glass_edge;
};

/// Everything the two draw callbacks need, relative to slot_container's origin.
/// One detail view is shown at a time (AmsPanel and AmsOverviewPanel are
/// singletons and each shows one unit), so one copy serves both.
struct TrayState {
    bool valid = false;
    lv_obj_t* container = nullptr; // slot_container the geometry is relative to
    tray::TrayBox box{};
    tray::LidMode lid = tray::LidMode::None;
    float lid_h = 0;
    float lane_half = 0;
    int lane_count = 0;
    float lane_x[AMS_DETAIL_MAX_SLOTS] = {}; // front-plane slot centres
    TrayColors color{};
    TrayOpacities opa = DARK_OPA;
};

TrayState s_tray;

lv_color_t tray_token(const char* name, bool dark) {
    char key[48];
    snprintf(key, sizeof(key), "%s_%s", name, dark ? "dark" : "light");
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("ams_unit_detail");
    const char* hex = scope ? lv_xml_get_const(scope, key) : nullptr;
    return hex ? theme_manager_parse_hex_color(hex) : theme_manager_get_color("card_bg");
}

TrayColors load_tray_colors(bool dark) {
    return {tray_token("tray_front", dark),     tray_token("tray_wall", dark),
            tray_token("tray_floor", dark),     tray_token("tray_back", dark),
            tray_token("tray_side", dark),      tray_token("tray_edge", dark),
            tray_token("tray_shell", dark),     tray_token("tray_glass", dark),
            tray_token("tray_glass_edge", dark)};
}

// Read on every draw, so a theme or dark-mode switch repaints the tray in the
// new colors.
void load_tray_look() {
    const bool dark = theme_manager_is_dark_mode();
    s_tray.color = load_tray_colors(dark);
    s_tray.opa = dark ? DARK_OPA : LIGHT_OPA;
}

lv_point_precise_t to_screen(lv_point_t origin, tray::PointF p) {
    return {static_cast<lv_value_precise_t>(origin.x + p.x),
            static_cast<lv_value_precise_t>(origin.y + p.y)};
}

/// A convex polygon as a triangle fan.
void fill_convex(lv_layer_t* layer, lv_point_t origin, const tray::PointF* pts, int n,
                 lv_color_t color, lv_opa_t opa) {
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = opa;
    for (int i = 1; i + 1 < n; ++i) {
        tri.p[0] = to_screen(origin, pts[0]);
        tri.p[1] = to_screen(origin, pts[i]);
        tri.p[2] = to_screen(origin, pts[i + 1]);
        lv_draw_triangle(layer, &tri);
    }
}

void stroke(lv_layer_t* layer, lv_point_t origin, const tray::PointF* pts, int n, bool closed,
            lv_color_t color, lv_opa_t opa) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.opa = opa;
    line.width = 1;
    line.round_start = 1;
    line.round_end = 1;
    const int segments = closed ? n : n - 1;
    for (int i = 0; i < segments; ++i) {
        line.p1 = to_screen(origin, pts[i]);
        line.p2 = to_screen(origin, pts[(i + 1) % n]);
        lv_draw_line(layer, &line);
    }
}

void edge(lv_layer_t* layer, lv_point_t origin, tray::PointF a, tray::PointF b, lv_color_t color,
          lv_opa_t opa) {
    const tray::PointF pts[2] = {a, b};
    stroke(layer, origin, pts, 2, false, color, opa);
}

/// One lid: its two caps and their convex hull.
struct Lid {
    tray::TrayBox box; // fl/fr are the lid's ends
    tray::PointF left[CAP_POINTS], right[CAP_POINTS];
    tray::PointF hull[2 * CAP_POINTS];
    int hull_n = 0;
};

float cross(tray::PointF o, tray::PointF a, tray::PointF b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

void build_lid(const tray::TrayBox& unit_box, float lid_h, float x0, float x1, Lid& lid) {
    lid.box = unit_box;
    lid.box.fl = x0;
    lid.box.fr = x1;
    tray::cap_polyline(unit_box, lid_h, x0, lid.left, CAP_POINTS);
    tray::cap_polyline(unit_box, lid_h, x1, lid.right, CAP_POINTS);
    // Monotone-chain hull; the right cap is the left one shifted along x.
    tray::PointF pts[2 * CAP_POINTS];
    for (int i = 0; i < CAP_POINTS; ++i) {
        pts[i] = lid.left[i];
        pts[CAP_POINTS + i] = lid.right[i];
    }
    std::sort(pts, pts + 2 * CAP_POINTS, [](tray::PointF a, tray::PointF b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    int k = 0;
    for (int i = 0; i < 2 * CAP_POINTS; ++i) {
        while (k >= 2 && cross(lid.hull[k - 2], lid.hull[k - 1], pts[i]) <= 0)
            --k;
        lid.hull[k++] = pts[i];
    }
    for (int i = 2 * CAP_POINTS - 2, lower = k + 1; i >= 0; --i) {
        while (k >= lower && cross(lid.hull[k - 2], lid.hull[k - 1], pts[i]) <= 0)
            --k;
        lid.hull[k++] = pts[i];
    }
    lid.hull_n = k - 1;
}

/// The lid spans: one over the row, or one per lane.
int lid_spans(float* x0, float* x1) {
    if (s_tray.lid == tray::LidMode::Unit) {
        x0[0] = s_tray.box.fl;
        x1[0] = s_tray.box.fr;
        return 1;
    }
    if (s_tray.lid != tray::LidMode::PerLane)
        return 0;
    for (int i = 0; i < s_tray.lane_count; ++i) {
        x0[i] = s_tray.lane_x[i] - s_tray.lane_half;
        x1[i] = s_tray.lane_x[i] + s_tray.lane_half;
    }
    return s_tray.lane_count;
}

lv_point_t container_origin(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(lv_obj_get_parent(obj), &a);
    return {a.x1, a.y1};
}

/// Inside faces and the lid's interior, behind the spools.
void tray_back_draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer || !s_tray.valid)
        return;
    load_tray_look();
    const lv_point_t o = container_origin(lv_event_get_target_obj(e));
    const tray::TrayFaces f = tray::tray_faces(s_tray.box);
    const TrayColors& c = s_tray.color;
    const bool simple = helix::ui::reduced_effects();

    if (!simple) {
        fill_convex(layer, o, f.back_wall, 4, c.back, LV_OPA_COVER);
        fill_convex(layer, o, f.floor, 4, c.floor, LV_OPA_COVER);
        fill_convex(layer, o, f.left_wall, 4, c.wall, LV_OPA_COVER);
    }
    edge(layer, o, f.back_wall[0], f.back_wall[1], c.edge, s_tray.opa.edge);

    if (simple || s_tray.lid == tray::LidMode::None)
        return;
    // The tinted interior shell, then the back wall again: it stands in front
    // of the lid's lower interior.
    float x0[MAX_LIDS], x1[MAX_LIDS];
    const int lids = lid_spans(x0, x1);
    Lid lid;
    for (int i = 0; i < lids; ++i) {
        build_lid(s_tray.box, s_tray.lid_h, x0[i], x1[i], lid);
        fill_convex(layer, o, lid.hull, lid.hull_n, c.shell, s_tray.opa.shell);
    }
    fill_convex(layer, o, f.back_wall, 4, c.back, LV_OPA_COVER);
    edge(layer, o, f.back_wall[0], f.back_wall[1], c.edge, s_tray.opa.edge);
}

/// The sheen: one diffuse band, a horizontal gradient per row.
void draw_sheen(lv_layer_t* layer, lv_point_t o, const Lid& lid) {
    tray::SheenSpan span{};
    tray::SheenRow rows[tray::SHEEN_MAX_ROWS];
    const int n = tray::sheen_rows(lid.box, s_tray.lid_h, s_tray.opa.sheen, span, rows);
    const float width = span.x1 - span.x0;
    if (n == 0 || width < 1)
        return;
    auto frac = [&](float x) { return (uint8_t)std::lround(255.0f * (x - span.x0) / width); };
    for (int i = 0; i < n; ++i) {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_opa = LV_OPA_COVER;
        const lv_color_t white = lv_color_white();
        const lv_color_t colors[4] = {white, white, white, white};
        const lv_opa_t opas[4] = {0, rows[i].opa, rows[i].opa, 0};
        const uint8_t fracs[4] = {0, frac(span.full0), frac(span.full1), 255};
        lv_grad_init_stops(&rect.bg_grad, colors, opas, fracs, 4);
        lv_grad_horizontal_init(&rect.bg_grad);
        const int32_t y = o.y + (int32_t)std::floor(rows[i].y);
        const lv_area_t area = {o.x + (int32_t)span.x0, y, o.x + (int32_t)span.x1, y};
        lv_draw_rect(layer, &rect, &area);
    }
}

/// Front wall, right side, edges and the glass lid, in front of the spools.
void tray_front_draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer || !s_tray.valid)
        return;
    load_tray_look();
    const lv_point_t o = container_origin(lv_event_get_target_obj(e));
    const tray::TrayFaces f = tray::tray_faces(s_tray.box);
    const TrayColors& c = s_tray.color;
    const bool simple = helix::ui::reduced_effects();

    if (!simple) {
        lv_draw_rect_dsc_t front;
        lv_draw_rect_dsc_init(&front);
        front.bg_color = c.front;
        front.bg_opa = s_tray.opa.front;
        const lv_area_t area = {o.x + (int32_t)f.front[0].x, o.y + (int32_t)f.front[0].y,
                                o.x + (int32_t)f.front[2].x, o.y + (int32_t)f.front[2].y};
        lv_draw_rect(layer, &front, &area);
        fill_convex(layer, o, f.right_side, 4, c.side, LV_OPA_COVER);
    }
    const tray::PointF fl_t = f.front[0], fr_t = f.front[1], fr_b = f.front[2], fl_b = f.front[3];
    const tray::PointF bl_t = f.back_wall[0], br_t = f.back_wall[1], br_b = f.back_wall[2],
                       bl_b = f.back_wall[3];
    const tray::PointF edges[][2] = {{fl_t, fr_t}, {fl_b, fr_b}, {fl_t, fl_b},
                                     {fr_t, fr_b}, {fr_t, br_t}, {fr_b, br_b},
                                     {br_t, br_b}, {fl_t, bl_t}, {fl_b, bl_b}};
    for (const auto& ab : edges)
        edge(layer, o, ab[0], ab[1], c.edge, s_tray.opa.edge);

    float x0[MAX_LIDS], x1[MAX_LIDS];
    const int lids = lid_spans(x0, x1);
    Lid lid;
    const TrayOpacities& a = s_tray.opa;
    for (int i = 0; i < lids; ++i) {
        build_lid(s_tray.box, s_tray.lid_h, x0[i], x1[i], lid);
        if (!simple) {
            fill_convex(layer, o, lid.hull, lid.hull_n, c.glass, a.glass);
            fill_convex(layer, o, lid.right, CAP_POINTS, c.glass, a.cap);
            draw_sheen(layer, o, lid);
        }
        stroke(layer, o, lid.hull, lid.hull_n, true, c.glass_edge, a.glass_edge);
        stroke(layer, o, lid.right, CAP_POINTS, false, c.glass_edge,
               (lv_opa_t)(a.glass_edge * 7 / 10));
        stroke(layer, o, lid.left, CAP_POINTS, false, c.glass_edge,
               (lv_opa_t)(a.glass_edge * 3 / 10));
    }
}

/// Centre of the slot's spool graphic, in slot_container coordinates.
bool spool_centre(lv_obj_t* slot, lv_point_t origin, float& x, float& y, int32_t& size) {
    lv_obj_t* spool = lv_obj_find_by_name(slot, "spool_graphic");
    if (!spool)
        spool = lv_obj_find_by_name(slot, "lane_spool");
    if (!spool)
        return false;
    lv_area_t a;
    lv_obj_get_coords(spool, &a);
    x = (a.x1 + a.x2 + 1) / 2.0f - origin.x;
    y = (a.y1 + a.y2 + 1) / 2.0f - origin.y;
    size = lv_area_get_width(&a);
    return true;
}

/// Lay a widget's top-left at (x, y) in absolute coords by translation, so the
/// flex layout around it is untouched.
void translate_to(lv_obj_t* obj, int32_t x, int32_t y, bool move_x) {
    lv_obj_set_style_translate_x(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_y(obj, 0, LV_PART_MAIN);
    lv_obj_update_layout(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (move_x)
        lv_obj_set_style_translate_x(obj, x - a.x1, LV_PART_MAIN);
    lv_obj_set_style_translate_y(obj, y - a.y1, LV_PART_MAIN);
}

} // namespace

AmsDetailWidgets ams_detail_find_widgets(lv_obj_t* root) {
    AmsDetailWidgets w;
    if (!root)
        return w;

    w.root = root;
    w.slot_grid = lv_obj_find_by_name(root, "slot_grid");
    w.slot_tray = lv_obj_find_by_name(root, "slot_tray");
    w.labels_layer = lv_obj_find_by_name(root, "labels_layer");
    w.badge_layer = lv_obj_find_by_name(root, "badge_layer");
    w.env_indicator = lv_obj_find_by_name(root, "env_indicator");

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
    helix::AmsSystemInfo info;

    auto* backend = helix::AmsState::instance().get_backend();
    if (backend) {
        info = backend->get_system_info();
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
    helix::ui::ams_detail_sync_slot_states(slot_widgets, count);

    // Calculate and apply slot sizing
    lv_obj_t* slot_area = lv_obj_get_parent(w.slot_grid);
    lv_obj_update_layout(slot_area);
    int32_t available_width = lv_obj_get_content_width(slot_area);
    result.layout = helix::ui::ams_detail_slot_layout(available_width, count);

    lv_obj_set_style_pad_column(w.slot_grid, result.layout.overlap > 0 ? -result.layout.overlap : 0,
                                LV_PART_MAIN);

    // Center the row (and the box around it) in the slot container
    lv_obj_set_style_pad_left(w.slot_grid, result.layout.centering_offset, LV_PART_MAIN);

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

void helix::ui::ams_detail_sync_slot_states(lv_obj_t* slot_widgets[], int slot_count) {
    auto* backend = helix::AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    const helix::AmsSystemInfo info = backend->get_system_info();
    for (int i = 0; i < slot_count; ++i) {
        lv_obj_t* slot = slot_widgets[i];
        if (!slot) {
            continue;
        }
        // A bay of a box that is not on the bus keeps its place in the row but
        // takes the disabled state, like its unit's card: dimmed, and a tap
        // reaches nothing.
        const int global_index =
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(slot)));
        if (info.slot_absent(global_index)) {
            lv_obj_add_state(slot, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(slot, LV_STATE_DISABLED);
        }
    }
}

void ams_detail_destroy_slots(AmsDetailWidgets& w, lv_obj_t* slot_widgets[], int& slot_count) {
    (void)w; // Reserved for future use (e.g. labels_layer cleanup)

    if (slot_count <= 0)
        return;

    // Reparent all slots into a hidden condemned container, then delete it
    // in one deferred pass.  This avoids two problems with the old per-slot
    // safe_delete() loop:
    //   1. Repeated defocus_tree() calls that corrupt the LVGL group linked
    //      list (crash b05adc0e).
    //   2. Synchronous lv_obj_delete() inside UpdateQueue::process_pending(),
    //      which can corrupt LVGL's event linked list.
    lv_obj_t* condemned = lv_obj_create(lv_screen_active());
    lv_obj_add_flag(condemned, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(condemned, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(condemned, 0, 0);

    for (int i = 0; i < slot_count; ++i) {
        if (slot_widgets[i] && lv_obj_is_valid(slot_widgets[i])) {
            // Null out pointers to reparented badges/labels BEFORE deferred
            // deletion.  These widgets live on badge_layer/labels_layer which
            // will be cleaned synchronously in ams_detail_update_badges/labels,
            // but the slot's DELETE event (and unregister_slot_data) won't fire
            // until the condemned container is actually deleted.  Without this,
            // deferred observer callbacks find dangling pointers (#604).
            ui_ams_slot_detach_layers(slot_widgets[i]);
            lv_obj_set_parent(slot_widgets[i], condemned);
        }
        slot_widgets[i] = nullptr;
    }
    slot_count = 0;

    helix::ui::defocus_tree(condemned);
    helix::ui::safe_delete_deferred(condemned);
}

void helix::ui::ams_detail_sync_lane_entry(lv_obj_t* canvas, lv_obj_t* slot_grid) {
    if (!canvas)
        return;
    int32_t offset = INT32_MIN;
    if (slot_grid && s_tray.valid && s_tray.container == lv_obj_get_parent(slot_grid)) {
        lv_area_t c, g;
        lv_obj_get_coords(s_tray.container, &c);
        lv_obj_get_coords(slot_grid, &g);
        offset = c.y1 + (int32_t)std::lround(s_tray.box.fb) - g.y1;
    }
    ui_filament_path_canvas_set_lane_entry(canvas, offset);
}

bool helix::ui::ams_detail_error_in_view(const helix::AmsSystemInfo& info, int unit_index) {
    if (unit_index < 0 || unit_index >= static_cast<int>(info.units.size()))
        return true;
    const auto& unit = info.units[unit_index];
    auto in_unit = [&](int global) {
        return global >= unit.first_slot_global_index &&
               global < unit.first_slot_global_index + unit.slot_count;
    };
    if (info.current_slot >= 0)
        return in_unit(info.current_slot);
    if (info.units.size() == 1)
        return true;
    // No lane is loaded: the error is this unit's if one of its slots reports one.
    for (int s = 0; s < unit.slot_count; ++s) {
        const helix::SlotInfo* slot = info.get_slot_global(unit.first_slot_global_index + s);
        if (slot && slot->error.has_value())
            return true;
    }
    return false;
}

AmsSlotLayout helix::ui::ams_detail_slot_layout(int32_t available_width, int slot_count) {
    auto* backend = helix::AmsState::instance().get_backend(0);
    if (backend && !backend->has_physical_tray())
        return calculate_ams_slot_layout(available_width, slot_count);
    const float spool = (float)theme_manager_get_spacing("ams_slot_spool_size");
    if (spool <= 0)
        return calculate_ams_slot_layout(available_width, slot_count);
    // The box reaches past the outer slots: its first lid starts S/4 - 1 left of
    // the row (for any pitch), and its right side face ends S/4 right of it.
    // The readout's gap is its own margin in the row. Spools stand at the
    // tray's pitch rather than spreading across the width, centered with the box.
    const float skew = tray::DEPTH_SKEW * tray::box_depth(spool);
    const int32_t lead = (int32_t)std::ceil(std::max(0.0f, skew / 4 - 1));
    const int32_t tail = (int32_t)std::ceil(skew / 4);
    AmsSlotLayout layout =
        calculate_ams_slot_layout(std::max<int32_t>(0, available_width - lead - tail), slot_count,
                                  (int32_t)tray::spool_pitch(spool));
    layout.centering_offset += lead;
    return layout;
}

bool helix::ui::ams_detail_tray_geometry(tray::TrayBox& box, tray::LidMode& lid, float& lid_height,
                                         float& lane_half_width) {
    if (!s_tray.valid)
        return false;
    box = s_tray.box;
    lid = s_tray.lid;
    lid_height = s_tray.lid_h;
    lane_half_width = s_tray.lane_half;
    return true;
}

void ams_detail_update_tray(AmsDetailWidgets& w, lv_obj_t* const slot_widgets[], int slot_count,
                            int unit_index) {
    if (!w.slot_tray || !w.slot_grid)
        return;
    s_tray.valid = false;

    // Tool changers don't have a physical tray/housing
    auto* backend = helix::AmsState::instance().get_backend(0);
    if (backend && !backend->has_physical_tray()) {
        lv_obj_add_flag(w.slot_tray, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(w.slot_tray, LV_OBJ_FLAG_HIDDEN);

    // Attach draw callbacks once per object instance. This function runs on every
    // panel rebuild, so remove-then-add is what keeps it idempotent:
    // lv_obj_remove_event_cb() strips every prior registration of that callback
    // function, leaving exactly one after the add.
    // Neither caller invokes this from inside a draw dispatch of these objects, so
    // mutating their event lists here is safe.
    lv_obj_remove_event_cb(w.slot_grid, tray_back_draw_cb);
    lv_obj_add_event_cb(w.slot_grid, tray_back_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_remove_event_cb(w.slot_tray, tray_front_draw_cb);
    lv_obj_add_event_cb(w.slot_tray, tray_front_draw_cb, LV_EVENT_DRAW_POST, nullptr);

    lv_obj_t* container = lv_obj_get_parent(w.slot_grid);
    if (!container || !slot_widgets || slot_count <= 0)
        return;
    const int n = std::min(slot_count, AMS_DETAIL_MAX_SLOTS);

    // Any climate data gets glass.
    helix::AmsUnit unit;
    bool dryer = false;
    if (backend) {
        const helix::AmsSystemInfo info = backend->get_system_info();
        const int u = unit_index >= 0 ? unit_index : 0;
        if (u < static_cast<int>(info.units.size()))
            unit = info.units[u];
        dryer = backend->get_dryer_info(u).supported;
    }
    const tray::LidMode lid = tray::lid_mode(unit, true, dryer);
    const bool per_lane = lid == tray::LidMode::PerLane;

    // Under per-lane lids each lane's humidity sits above its label.
    for (int i = 0; i < n; ++i) {
        if (slot_widgets[i])
            ui_ams_slot_set_lane_humidity_visible(slot_widgets[i], per_lane);
    }
    lv_obj_update_layout(container);
    lv_area_t c;
    lv_obj_get_coords(container, &c);
    const lv_point_t origin = {c.x1, c.y1};

    // Spools as laid out: front-plane centres sit S/2 left of the drawn ones.
    float cx[AMS_DETAIL_MAX_SLOTS], cy_sum = 0;
    int32_t spool_size = 0;
    for (int i = 0; i < n; ++i) {
        float y = 0;
        if (!slot_widgets[i] || !spool_centre(slot_widgets[i], origin, cx[i], y, spool_size))
            return;
        cy_sum += y;
    }
    const float size = (float)spool_size;
    const float flange_ry = tray::SPOOL_FLANGE_RADIUS * size;
    tray::TrayBox box{};
    box.depth = tray::box_depth(size);
    box.rise = tray::box_rise(box.depth);
    box.back_extra = (float)theme_manager_get_spacing("space_md");
    const float skew = tray::DEPTH_SKEW * box.depth;
    for (int i = 0; i < n; ++i)
        s_tray.lane_x[i] = cx[i] - skew / 2;
    const float spacing = n > 1 ? s_tray.lane_x[1] - s_tray.lane_x[0] : tray::spool_pitch(size);
    const float half = tray::lane_lid_half_width(spacing, box);

    // The spools stand on the floor: their mid-depth centre is RISE/2 above the
    // front-plane centre, whose flange bottom sits SPOOL_FLOOR_GAP above FB.
    box.fb = cy_sum / n + flange_ry + tray::SPOOL_FLOOR_GAP + box.rise / 2;
    lv_obj_update_layout(w.slot_grid);
    const float front_h = std::max(20.0f, lv_obj_get_height(w.slot_grid) / 4.0f);
    box.ft = box.fb - front_h;
    box.fl = s_tray.lane_x[0] - half;
    box.fr = s_tray.lane_x[n - 1] + half;

    s_tray.lid = lid;
    s_tray.lid_h = tray::lid_height(box, flange_ry);
    s_tray.lane_half = half;
    s_tray.lane_count = n;
    s_tray.box = box;
    s_tray.container = container;
    s_tray.valid = true;

    // Labels sit space_md above the unit's top, lane humidity above them.
    // Without a lid the spools rise above the back wall; the labels clear both.
    const bool has_lid = s_tray.lid != tray::LidMode::None;
    const float spool_top = cy_sum / n - flange_ry;
    const float top = has_lid ? tray::unit_top_y(box, s_tray.lid_h, true)
                              : std::min(tray::unit_top_y(box, 0, false), spool_top);
    const int32_t gap = theme_manager_get_spacing("space_md");
    const int32_t label_bottom = origin.y + (int32_t)std::lround(top) - gap;
    if (n <= 4) {
        for (int i = 0; i < n; ++i) {
            lv_obj_t* label = lv_obj_find_by_name(slot_widgets[i], "material_label");
            if (!label)
                continue;
            lv_obj_set_style_translate_y(label, 0, LV_PART_MAIN);
            lv_obj_update_layout(label);
            lv_area_t la;
            lv_obj_get_coords(label, &la);
            lv_obj_set_style_translate_y(label, label_bottom - la.y2, LV_PART_MAIN);
            if (lv_obj_t* row = ui_ams_slot_get_lane_humidity(slot_widgets[i]))
                lv_obj_set_style_translate_y(row, label_bottom - la.y2, LV_PART_MAIN);
        }
    } else if (w.labels_layer) {
        lv_obj_set_style_translate_y(w.labels_layer, 0, LV_PART_MAIN);
        lv_obj_update_layout(w.labels_layer);
        int32_t lowest = INT32_MIN;
        for (uint32_t i = 0; i < lv_obj_get_child_count(w.labels_layer); ++i) {
            lv_area_t la;
            lv_obj_get_coords(lv_obj_get_child(w.labels_layer, (int32_t)i), &la);
            lowest = std::max<int32_t>(lowest, la.y2);
        }
        if (lowest != INT32_MIN)
            lv_obj_set_style_translate_y(w.labels_layer, label_bottom - lowest, LV_PART_MAIN);
    }
    // The unit readout stands beside the drum, right of the back-right corner.
    if (w.env_indicator && !lv_obj_has_flag(w.env_indicator, LV_OBJ_FLAG_HIDDEN)) {
        const tray::PointF br_t = tray::tray_faces(box).back_wall[1];
        translate_to(w.env_indicator, origin.x + (int32_t)std::lround(br_t.x) + gap,
                     origin.y + (int32_t)std::lround(top) - 4, true);
    }

    lv_obj_invalidate(container);
    spdlog::debug("[AmsDetail] Tray: box x {:.1f}..{:.1f} y {:.1f}..{:.1f}, depth {:.1f}, lid {}",
                  box.fl, box.fr, box.ft, box.fb, box.depth, static_cast<int>(s_tray.lid));
}

void ams_detail_update_labels(AmsDetailWidgets& w, lv_obj_t* slot_widgets[], int slot_count,
                              const AmsSlotLayout& layout) {
    if (!w.labels_layer || slot_count <= 4)
        return;

    helix::ui::safe_clean_children(w.labels_layer);

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

    // Clean stale badges from previous unit view (badges are reparented here
    // from slot widgets, so they persist across unit switches if not cleaned)
    helix::ui::safe_clean_children(w.badge_layer);

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

    auto* backend = helix::AmsState::instance().get_backend();
    if (!backend)
        return;

    helix::AmsSystemInfo info = backend->get_system_info();

    // Hub-only mode: slots -> hub and its output stub, skip downstream
    ui_filament_path_canvas_set_hub_only(canvas, hub_only);
    // A unit-scoped view asks its own unit; the all-units view any unit.
    if (unit_index >= 0 && unit_index < static_cast<int>(info.units.size())) {
        const auto& unit = info.units[unit_index];
        ui_filament_path_canvas_set_hub_sensor(canvas, unit.has_hub_sensor,
                                               unit.hub_sensor_triggered);
    } else {
        ui_filament_path_canvas_set_hub_sensor(
            canvas,
            std::any_of(info.units.begin(), info.units.end(),
                        [](const helix::AmsUnit& u) { return u.has_hub_sensor; }),
            std::any_of(info.units.begin(), info.units.end(),
                        [](const helix::AmsUnit& u) { return u.hub_sensor_triggered; }));
    }

    // Hide the bypass path for backends that don't support it (e.g. tool
    // changers) — and on AFC while bypass is disengaged, since AFC reports a
    // virtual bypass whether or not one is wired (#1229).
    ui_filament_path_canvas_set_show_bypass(canvas, helix::ui::bypass_node_visible_for(backend));

    // Determine slot count and offset for this unit
    int slot_count = info.total_slots;
    int slot_offset = 0;
    if (unit_index >= 0 && unit_index < static_cast<int>(info.units.size())) {
        slot_count = info.units[unit_index].slot_count;
        slot_offset = info.units[unit_index].first_slot_global_index;
    }

    ui_filament_path_canvas_set_slot_count(canvas, slot_count);
    helix::PathTopology topo =
        (unit_index >= 0) ? backend->get_unit_topology(unit_index) : backend->get_topology();
    // A passthrough selector with an on-head combiner draws as the merge fan
    // with the hub at the toolhead: the slots ARE the selector's per-lane
    // outputs, and each tube runs the full height to the combiner.
    if (backend->hub_on_toolhead())
        topo = helix::PathTopology::HUB;
    ui_filament_path_canvas_set_topology(canvas, static_cast<int>(topo));
    ui_filament_path_canvas_set_hub_on_toolhead(canvas, backend->hub_on_toolhead());

    // A unit-scoped view asks its own unit; the all-units view asks whether any has one.
    bool has_toolhead_sensor = false;
    if (unit_index >= 0 && unit_index < static_cast<int>(info.units.size())) {
        has_toolhead_sensor = info.units[unit_index].has_toolhead_sensor;
    } else {
        has_toolhead_sensor =
            std::any_of(info.units.begin(), info.units.end(),
                        [](const helix::AmsUnit& u) { return u.has_toolhead_sensor; });
    }
    ui_filament_path_canvas_set_toolhead_sensor(canvas, has_toolhead_sensor);

    // Pass slot_grid reference so draw callback can read actual slot positions
    // at render time — avoids setup-vs-draw timing mismatches across breakpoints.
    if (slot_grid) {
        ui_filament_path_canvas_set_slot_grid(canvas, slot_grid);
        helix::ui::ams_detail_sync_lane_entry(canvas, slot_grid);

        // Still set slot_width/overlap as fallback for get_slot_x() computed positions
        lv_obj_t* slot_area = lv_obj_get_parent(slot_grid);
        lv_obj_update_layout(slot_area);
        int32_t available_width = lv_obj_get_content_width(slot_area);
        auto layout = helix::ui::ams_detail_slot_layout(available_width, slot_count);
        ui_filament_path_canvas_set_slot_width(canvas, layout.slot_width);
        ui_filament_path_canvas_set_slot_overlap(canvas, layout.overlap);
    }

    // Map active slot to local index for unit-scoped views
    int active_slot = info.path_active_slot();
    if (unit_index >= 0) {
        int local_active = active_slot - slot_offset;
        active_slot = (local_active >= 0 && local_active < slot_count) ? local_active : -1;
    }
    ui_filament_path_canvas_set_active_slot(canvas, active_slot);

    // Set filament color from active slot
    int global_active = (unit_index >= 0) ? active_slot + slot_offset : active_slot;
    if (global_active >= 0) {
        helix::SlotInfo slot_info = backend->get_slot_info(global_active);
        ui_filament_path_canvas_set_filament_color(canvas, slot_info.color_rgb);
    }

    // Clear eject mode once the eject operation has completed (action returned
    // to IDLE and filament is no longer in the lane).
    helix::AmsAction action = backend->get_current_action();
    if (action == helix::AmsAction::IDLE) {
        ui_filament_path_canvas_set_eject_mode(canvas, false);
    }

    // Set filament and error segments
    helix::PathSegment segment = backend->get_filament_segment();
    ui_filament_path_canvas_set_filament_segment(canvas, static_cast<int>(segment));

    // Bowden progress fills the output tube from the hub: as far as a load has
    // pushed, or what an unload has yet to pull back.
    {
        const int progress = backend->get_bowden_progress();
        int fill = -1;
        if (progress >= 0 && action == helix::AmsAction::LOADING)
            fill = progress;
        else if (progress >= 0 && action == helix::AmsAction::UNLOADING)
            fill = 100 - progress;
        ui_filament_path_canvas_set_bowden_fill(canvas, fill);
    }

    // The system's error, only when it belongs to this view's unit.
    helix::PathSegment error_seg = backend->infer_error_segment();
    if (!helix::ui::ams_detail_error_in_view(info, unit_index))
        error_seg = helix::PathSegment::NONE;
    ui_filament_path_canvas_set_error_segment(canvas, static_cast<int>(error_seg));

    // Set per-slot prep and load sensor capability flags
    for (int i = 0; i < slot_count; ++i) {
        ui_filament_path_canvas_set_slot_prep_sensor(
            canvas, i, backend->slot_has_prep_sensor(slot_offset + i));
        ui_filament_path_canvas_set_slot_load_sensor(
            canvas, i, backend->slot_has_load_sensor(slot_offset + i));
        const auto& err = backend->get_slot_info(slot_offset + i).error;
        ui_filament_path_canvas_set_slot_error(
            canvas, i, err.has_value() && err->severity == helix::SlotError::ERROR);
    }

    // Plumb per-slot metadata (mapped_tool, extruder identity, hub routing) to
    // path canvas. The extruder name is what actually names a toolhead; the
    // mapped_tool alias stays as the fallback for backends that publish neither.
    if (unit_index >= 0 && unit_index < static_cast<int>(info.units.size())) {
        const auto& unit = info.units[unit_index];
        std::vector<int> extruder_tools(static_cast<size_t>(slot_count), -1);
        for (int i = 0; i < slot_count; ++i) {
            int gi = slot_offset + i;
            helix::SlotInfo slot = backend->get_slot_info(gi);
            ui_filament_path_canvas_set_slot_mapped_tool(canvas, i, slot.mapped_tool);
            if (const auto n = helix::tool_number_for_extruder(slot.extruder_name)) {
                extruder_tools[static_cast<size_t>(i)] = *n;
            }
            if (i < static_cast<int>(unit.lane_is_hub_routed.size())) {
                ui_filament_path_canvas_set_slot_hub_routed(canvas, i, unit.lane_is_hub_routed[i]);
            }
        }
        ui_filament_path_canvas_set_extruder_tools(canvas, extruder_tools.data(), slot_count);
    }

    // Set per-slot filament states (using local indices for unit-scoped views).
    // Every slot is set, empty ones too, so an update that changes nothing
    // repaints nothing.
    for (int i = 0; i < slot_count; ++i) {
        int global_idx = i + slot_offset;
        helix::PathSegment slot_seg = backend->get_slot_filament_segment(global_idx);
        uint32_t color = 0x808080;
        if (slot_seg != helix::PathSegment::NONE) {
            color = backend->get_slot_info(global_idx).color_rgb;
        }
        ui_filament_path_canvas_set_slot_filament(canvas, i, static_cast<int>(slot_seg), color);
    }

    // The buffer box: AFC buffer health, Happy Hare sync feedback, or a
    // filament pressure sensor, tinted by the buffer bands.
    const helix::ui::BufferBoxState box = helix::ui::ams_detail_buffer_box(info, unit_index);
    ui_filament_path_canvas_set_buffer_fault_state(canvas, box.fault);
    ui_filament_path_canvas_set_buffer_info(canvas, box.present, box.state, box.label);
    ui_filament_path_canvas_set_buffer_bias(canvas, box.bias);

    // Set external spool color and assignment state. Only while bypass is
    // actually engaged: an assigned external spool is not in the filament path
    // until it is selected, and drawing it anyway put a loaded lane's material
    // on the bypass node (#1229 defect 5).
    auto ext_spool = helix::AmsState::instance().get_external_spool_info();
    const bool show_bypass_spool =
        ext_spool.has_value() && helix::ui::bypass_node_visible_for(backend);
    ui_filament_path_canvas_set_bypass_has_spool(canvas, show_bypass_spool);
    if (show_bypass_spool) {
        ui_filament_path_canvas_set_bypass_color(canvas, ext_spool->color_rgb);
    }

    spdlog::debug("[AmsDetail] Path canvas configured: slots={}, unit={}, hub_only={}", slot_count,
                  unit_index, hub_only);
}

void ams_detail_pre_show_env_indicator(AmsDetailWidgets& w, int unit_index) {
    if (!w.env_indicator)
        return;

    const int u = (unit_index >= 0) ? unit_index : 0; // -1 == whole/single-unit → unit 0
    helix::AmsState::instance().set_detail_env_unit(u);
    lv_obj_set_user_data(w.env_indicator, reinterpret_cast<void*>(static_cast<intptr_t>(u)));

    auto* backend = helix::AmsState::instance().get_backend();
    if (backend && backend->has_environment_sensors()) {
        lv_obj_remove_flag(w.env_indicator, LV_OBJ_FLAG_HIDDEN);
        // Force layout on the root (flex row container) so the indicator's
        // content width is resolved before slot creation reads available_width.
        if (w.root) {
            lv_obj_update_layout(w.root);
            int32_t indicator_w = lv_obj_get_width(w.env_indicator);
            spdlog::debug("[AmsDetail] Pre-showed env indicator (width={}px) for flex layout",
                          indicator_w);
        }
    } else {
        lv_obj_add_flag(w.env_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// Shared Context-Menu Dispatch
// ============================================================================

namespace helix {
namespace ui {

BufferBoxState ams_detail_buffer_box(const AmsSystemInfo& info, int unit_index) {
    BufferBoxState box;
    // The AFC buffer rows describe one unit: the one asked for, else the one
    // the system reading came from.
    const AmsUnit* unit = info.get_unit(buffer_view_unit(info, unit_index));
    if (unit && unit->buffer_health) {
        const BufferHealth& h = *unit->buffer_health;
        box.present = true;
        if (h.state == "Advancing") {
            box.state = 1;
        } else if (h.state == "Trailing") {
            box.state = 2;
        }
        box.fault = static_cast<int>(buffer_fault_status(h));
    }
    if (!box.present && info.type == AmsType::HAPPY_HARE) {
        const auto& sf = info.sync_feedback_state;
        if (!sf.empty() && sf != "disabled") {
            box.present = true;
            if (sf == "compressed") {
                box.state = 1;
            } else if (sf == "tension") {
                box.state = 2;
            }
        }
    }

    const BufferReading reading = buffer_reading(info, unit_index);
    if (reading.source == BufferSource::Fps) {
        box.present = true;
        box.label = "FPS"; // i18n: do not translate - hardware abbreviation
    }
    if (reading.is_fill()) {
        // One-sided pressure: only the rails tint, and there is no bias to
        // interpolate a color from.
        box.fault = std::max(box.fault, static_cast<int>(reading.status));
        if (box.fault == 0) {
            box.fault = -1; // regulating: nothing to color
        }
    } else if (reading.has_slider) {
        box.bias = reading.bias;
        box.fault = std::max(box.fault, static_cast<int>(reading.status));
    } else if (reading.source == BufferSource::Fps && box.fault == 0) {
        box.fault = -1; // no set point: nothing to judge the pressure against
    }
    return box;
}

bool ams_dispatch_backend_action(AmsContextMenu::MenuAction action, int slot,
                                 lv_obj_t* path_canvas) {
    using MenuAction = AmsContextMenu::MenuAction;

    switch (action) {
    case MenuAction::EJECT:
    case MenuAction::RECOVER_POSITION:
    case MenuAction::SELECT_GATE:
    case MenuAction::CHECK_GATE:
    case MenuAction::PRELOAD:
    case MenuAction::CLEAR_SPOOL:
        break;
    default:
        return false; // caller's own switch owns it
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return true;
    }

    switch (action) {
    case MenuAction::EJECT: {
        // Flip the canvas before the call: a backend that dispatches
        // asynchronously animates while in flight, and the error arm below
        // undoes it for the refusals that return synchronously.
        if (path_canvas) {
            ui_filament_path_canvas_set_eject_mode(path_canvas, true);
        }
        AmsError error = backend->eject_lane(slot);
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Eject failed"));
            if (path_canvas) {
                ui_filament_path_canvas_set_eject_mode(path_canvas, false);
            }
        }
        break;
    }

    case MenuAction::RECOVER_POSITION: {
        AmsError error = backend->recover_lane_position(slot);
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Recovery failed"));
        }
        break;
    }

    case MenuAction::SELECT_GATE: {
        AmsError error = backend->select_gate(slot);
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Select slot failed"));
        }
        break;
    }

    case MenuAction::CHECK_GATE: {
        AmsError error = backend->check_gate(slot);
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Check slot failed"));
        }
        break;
    }

    case MenuAction::PRELOAD: {
        AmsError error = backend->preload_lane(slot);
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Preload failed"));
        }
        break;
    }

    case MenuAction::CLEAR_SPOOL: {
        // This function is public and takes a raw slot index, so the bypass
        // sentinel reaches it. The external spool is not one of the backend's
        // lanes: it carries no number to name, and apply_user_edit() refuses the
        // index, so it clears through its own store instead.
        if (slot == EXTERNAL_SPOOL_SLOT) {
            AmsState::instance().commit_external_spool_edit(SlotInfo{});
            NOTIFY_INFO(lv_tr("External spool cleared"));
            break;
        }

        // The menu greys its Clear button for this case, but this function is
        // public and its callers can hold a menu built before the print
        // started, so the guard here is the authority: the lane a job is
        // drawing from is the lane whose material and colour the print's own
        // surfaces are displaying (prestonbrown/helixscreen#1661).
        if (helix::ui::clear_spool_blocked_by_print(
                get_printer_state().print_state().get_print_lifecycle(),
                backend->slot_is_actively_loaded(slot))) {
            NOTIFY_WARNING("{}", helix::ui::clear_spool_blocked_hint(backend->lane_noun(), slot));
            break;
        }

        // Clear spool assignment: reset material/color/spool data, keep slot status.
        // Routed through AmsState::commit_slot_edit so the Spoolman server's
        // active spool and the identity cache clear too: a spool left active
        // server-side is re-asserted into the UI on the next start. The
        // PRE-WIPE info is passed as `original` because the commit's unlink
        // arm keys off original.spoolman_id.
        SlotInfo original = backend->get_slot_info(slot);
        SlotInfo cleared = original;
        cleared.material.clear();
        cleared.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        cleared.color_name.clear();
        cleared.multi_color_hexes.clear();
        cleared.brand.clear();
        // The catalog pick names a product of the material cleared above.
        cleared.catalog_id.clear();
        cleared.product_name.clear();
        // Drops spoolman_id AND the filament/vendor handles: left behind,
        // they feed a later repoint comparison against a spool this lane is
        // no longer linked to.
        cleared.clear_spoolman_link();
        cleared.remaining_weight_g = -1;
        cleared.total_weight_g = -1;
        auto error = AmsState::instance().commit_slot_edit(slot, original, cleared);
        if (error.success() || error.partially_applied) {
            // The commit clears what an edit can state - and, unlinked, a
            // colour pick, a typed weight or a colour name never engages as a
            // clear, so the statement leaves the lane's standing user record
            // holding them. Dropping that record whole is what makes the live
            // lane read what a restart would show (prestonbrown/helixscreen#1661).
            // A partial commit has already written HelixScreen's own layer and
            // its message says so, so the record drops for it too.
            // clear_slot_override() carries no server unlink and no ToolState
            // clear, which is why it rides behind the commit, never instead
            // of it. Backends whose firmware keeps its own per-slot profile
            // clear that here too.
            backend->clear_slot_override(slot);
        }
        if (error.success()) {
            NOTIFY_INFO(lv_tr("{} spool cleared"),
                        helix::ui::lane_label(backend->lane_noun(), slot));
        } else {
            notify_ams_error(error, lv_tr("Clear failed"));
        }
        break;
    }

    default:
        break; // unreachable: filtered by the guard switch above
    }

    return true;
}

namespace {

/// What a lane showed when its "same spool?" notice went up, keyed by slot.
/// Clear acts only while the lane still shows it: a later read or edit has
/// already answered the question the notice asked.
struct InsertOfferSnapshot {
    std::string material;
    uint32_t color_rgb = AMS_DEFAULT_SLOT_COLOR;
    int spoolman_id = 0;

    static InsertOfferSnapshot of(const SlotInfo& info) {
        return {info.material, info.color_rgb, info.spoolman_id};
    }
    bool operator==(const InsertOfferSnapshot& o) const {
        return material == o.material && color_rgb == o.color_rgb && spoolman_id == o.spoolman_id;
    }
};

struct InsertOffer {
    InsertOfferSnapshot shown;
    std::chrono::steady_clock::time_point asked_at;
};

/// A flapping gate sensor reports an insert on every rising edge, so the same
/// lane, still showing the same details, is asked about at most once per this
/// window. Without it, each flap re-raises a notice the user just dismissed.
constexpr auto kInsertOfferQuiet = std::chrono::minutes(5);

std::unordered_map<int, InsertOffer>& insert_offers() {
    static std::unordered_map<int, InsertOffer> offers;
    return offers;
}

void clear_if_lane_unchanged(int slot) {
    auto& offers = insert_offers();
    const auto it = offers.find(slot);
    if (it == offers.end()) {
        return;
    }
    const InsertOfferSnapshot shown = it->second.shown;
    offers.erase(it);
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || !(InsertOfferSnapshot::of(backend->get_slot_info(slot)) == shown)) {
        spdlog::debug("[AMS] Slot {} changed since the same-spool notice; Clear ignored", slot);
        return;
    }
    ams_dispatch_backend_action(AmsContextMenu::MenuAction::CLEAR_SPOOL, slot, nullptr);
}

} // namespace

void reset_insert_offers_for_test() {
    insert_offers().clear();
}

void offer_clear_after_unverified_insert(int slot) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend ||
        clear_spool_blocked_by_print(get_printer_state().print_state().get_print_lifecycle(),
                                     backend->slot_is_actively_loaded(slot))) {
        return;
    }
    // A lane with no details has nothing the new spool could contradict.
    const SlotInfo info = backend->get_slot_info(slot);
    if (!info.has_filament_info() && info.spoolman_id <= 0) {
        return;
    }
    const auto snapshot = InsertOfferSnapshot::of(info);
    const auto now = std::chrono::steady_clock::now();
    auto& offers = insert_offers();
    if (const auto it = offers.find(slot); it != offers.end() && it->second.shown == snapshot &&
                                           now - it->second.asked_at < kInsertOfferQuiet) {
        spdlog::debug("[AMS] Slot {} same-spool notice already asked; not re-raised", slot);
        return;
    }
    offers[slot] = {snapshot, now};
    const std::string message =
        fmt::format(lv_tr("Same spool in {}? Tap Clear if it is a new one."),
                    lane_label(backend->lane_noun(), slot));
    // The slot rides in user_data by value: the toast can outlive any object
    // that could own it, and the dispatch re-checks every guard at tap time.
    ToastManager::instance().show_with_action(
        ToastSeverity::INFO, message.c_str(), lv_tr("Clear"),
        [](void* user_data) {
            clear_if_lane_unchanged(static_cast<int>(reinterpret_cast<intptr_t>(user_data)));
        },
        reinterpret_cast<void*>(static_cast<intptr_t>(slot)), 10000);
}

} // namespace ui
} // namespace helix
