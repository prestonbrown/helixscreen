// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail.h"

#include "ui_ams_detail_tray.h"
#include "ui_ams_edit_follow_up.h"
#include "ui_ams_lane_spool.h"
#include "ui_ams_slot.h"
#include "ui_bypass_spool_widget.h"
#include "ui_effects.h"
#include "ui_error_reporting.h"
#include "ui_filament_path_canvas.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "ams_dryer.h"
#include "ams_gate_selector.h"
#include "ams_lane_recovery.h"
#include "ams_state.h"
#include "ams_tray_projection.h"
#include "ams_tray_shell_draw.h"
#include "app_globals.h" // get_printer_state: the print lifecycle the clear guard reads
#include "buffer_reading.h"
#include "clog_meter_geometry.h"
#include "display_numbering.h"
#include "filament_op_dispatch.h"      // kExternalSpoolSlot: the bypass sentinel
#include "filament_op_slot_resolver.h" // clear_spool_blocked_by_print: the print guard
#include "filament_tube_stroker.h"
#include "notice.h"
#include "panel_widget_size.h"
#include "printer_detector.h"
#include "printer_state.h" // PrinterState, complete for get_print_lifecycle()
#include "src/ui/filament/ui_filament_path_plan.h"
#include "theme_manager.h"
#include "tool_change_request.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using helix::ui::ams_tray::s_tray;
using helix::ui::ams_tray::tray_back_draw_cb;
using helix::ui::ams_tray::tray_front_draw_cb;
namespace tray = helix::ui::tray;

namespace {

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

void clear_translate(lv_obj_t* obj) {
    lv_obj_set_style_translate_x(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_y(obj, 0, LV_PART_MAIN);
}

// Tool changers have no housing around their spools.
/// The shell for a view; the whole-backend view of a one-unit system counts as that unit.
const tray::UnitShell* view_shell(int unit_index, bool has_dryer) {
    auto* backend = helix::AmsState::instance().get_backend(0);
    if (!backend)
        return nullptr;
    const helix::AmsSystemInfo info = backend->get_system_info();
    if (unit_index < 0 && info.units.size() == 1)
        unit_index = 0;
    return tray::unit_shell_for(info, unit_index, has_dryer);
}

bool unit_has_physical_tray() {
    auto* backend = helix::AmsState::instance().get_backend(0);
    return !backend || backend->has_physical_tray();
}

/// The lid the unit's climate data calls for: any reading gets glass. Asked
/// in flat style too, where per-lane readings show without the box.
tray::LidMode unit_lid_mode(int unit_index) {
    auto* backend = helix::AmsState::instance().get_backend(0);
    helix::AmsUnit unit;
    bool dryer = false;
    if (backend) {
        const helix::AmsSystemInfo info = backend->get_system_info();
        const int u = unit_index >= 0 ? unit_index : 0;
        if (u < static_cast<int>(info.units.size()))
            unit = info.units[u];
        dryer = helix::dryer_info(*backend, u).supported;
    }
    return tray::lid_mode(unit, unit_has_physical_tray(), dryer);
}

/// Under per-lane lids each lane's humidity sits above its label.
void show_lane_humidity(const helix::ui::WidgetRef slot_widgets[], int n, tray::LidMode lid) {
    for (int i = 0; i < n; ++i) {
        if (slot_widgets[i])
            ui_ams_slot_set_lane_humidity_visible(slot_widgets[i], lid == tray::LidMode::PerLane);
    }
}

// Flat spools are drawn without the box, as on a unit with no housing.
bool draws_tray() {
    return unit_has_physical_tray() && helix::ui::ams_spool_style_3d();
}

/// With no box to stand against, the environment pill stands just past the
/// last lane's spool and badge, as tall as the spools and level with them.
void place_env_pill_beside_spools(lv_obj_t* pill, lv_obj_t* container,
                                  const helix::ui::WidgetRef slot_widgets[], int n) {
    if (!pill || lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN))
        return;
    clear_translate(pill);
    int32_t top = INT32_MAX, bottom = INT32_MIN, lanes_right = INT32_MIN;
    for (int i = 0; i < n; ++i) {
        lv_obj_t* spool =
            slot_widgets[i] ? lv_obj_find_by_name(slot_widgets[i], "spool_graphic") : nullptr;
        if (!spool)
            continue;
        lv_area_t a;
        lv_obj_get_coords(spool, &a);
        top = std::min(top, a.y1);
        bottom = std::max(bottom, a.y2);
        lanes_right = std::max(lanes_right, a.x2);
        if (lv_obj_t* badge = ui_ams_slot_get_status_badge(slot_widgets[i])) {
            lv_obj_get_coords(badge, &a);
            lanes_right = std::max(lanes_right, a.x2);
        }
    }
    if (top > bottom) {
        lv_obj_remove_local_style_prop(pill, LV_STYLE_HEIGHT, LV_PART_MAIN);
        return;
    }
    // The badge layer clips at the container's edge.
    lv_area_t c;
    lv_obj_get_coords(container, &c);
    lanes_right = std::min(lanes_right, c.x2);
    // DECLARATIVE_OK: measured layout; the pill is as tall as the spools beside it
    lv_obj_set_height(pill, bottom - top + 1);
    lv_obj_update_layout(pill);
    lv_area_t a;
    lv_obj_get_coords(pill, &a);
    const int32_t left = lanes_right + 1 + theme_manager_get_spacing("space_xs");
    lv_obj_set_style_translate_x(pill, std::min<int32_t>(0, left - a.x1), LV_PART_MAIN);
    lv_obj_set_style_translate_y(pill, top - a.y1, LV_PART_MAIN);
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
        // The press lands on whichever child was hit, so a pressed-state
        // selector on the spool would miss most taps.
        lv_obj_set_style_opa(spool, 140, 0); // DECLARATIVE_OK: press on any child
    }
}

static void slot_released_cb(lv_event_t* e) {
    lv_obj_t* slot = lv_event_get_current_target_obj(e);
    lv_obj_t* spool = lv_obj_find_by_name(slot, "spool_container");
    if (spool) {
        lv_obj_set_style_opa(spool, LV_OPA_COVER, 0); // DECLARATIVE_OK: see slot_pressed_cb
    }
}

static void ams_detail_add_slot_press_feedback(lv_obj_t* slot) {
    lv_obj_add_event_cb(slot, slot_pressed_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(slot, slot_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slot, slot_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

AmsDetailSlotResult ams_detail_create_slots(AmsDetailWidgets& w,
                                            helix::ui::WidgetRef slot_widgets[], int max_slots,
                                            int unit_index, lv_event_cb_t click_cb,
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
            const auto band = helix::ui::ams_slot_hit_band(result.layout, i, count);
            helix::ui::ams_slot_set_hit_band(slot_widgets[i], band.left_inset, band.right_inset);
        }
    }

    spdlog::debug("[AmsDetail] Created {} slots (offset={}, width={}, overlap={}, center_pad={})",
                  count, slot_offset, result.layout.slot_width, result.layout.overlap,
                  result.layout.centering_offset);

    return result;
}

void helix::ui::ams_detail_sync_slot_states(helix::ui::WidgetRef slot_widgets[], int slot_count) {
    const auto info = helix::AmsState::instance().system_info();
    if (!info) {
        return;
    }
    ams_detail_sync_slot_states(slot_widgets, slot_count, *info);
}

void helix::ui::ams_detail_sync_slot_states(helix::ui::WidgetRef slot_widgets[], int slot_count,
                                            const helix::AmsSystemInfo& info) {
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

void ams_detail_destroy_slots(AmsDetailWidgets& w, helix::ui::WidgetRef slot_widgets[],
                              int& slot_count) {
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
        if (slot_widgets[i]) {
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
    } else if (slot_grid && !draws_tray()) {
        // With no housing the lanes leave from under the spools' flanges.
        lv_obj_update_layout(slot_grid);
        lv_area_t g;
        lv_obj_get_coords(slot_grid, &g);
        float x = 0, y = 0;
        int32_t size = 0;
        if (spool_centre(slot_grid, {g.x1, g.y1}, x, y, size))
            offset = (int32_t)std::lround(y + tray::kSpoolFlangeRadius * size);
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

bool helix::ui::unit_reports_climate(int unit_index) {
    return unit_lid_mode(unit_index) != tray::LidMode::None;
}

AmsSlotLayout helix::ui::ams_detail_slot_layout(int32_t available_width, int slot_count) {
    if (!unit_has_physical_tray())
        return calculate_ams_slot_layout(available_width, slot_count);
    const float spool = (float)theme_manager_get_spacing("ams_slot_spool_size");
    if (spool <= 0)
        return calculate_ams_slot_layout(available_width, slot_count);
    // Flat spools keep the unit's compact row with no box to clear: a ring,
    // its badge slack and a gap apart.
    if (!draws_tray())
        return calculate_ams_slot_layout(available_width, slot_count,
                                         (int32_t)spool + helix::ui::kAmsLaneSpoolBadgeMarginPx +
                                             theme_manager_get_spacing("space_xs"));
    // The box reaches past the outer slots: its first lid starts S/4 - 1 left of
    // the row (for any pitch), and its right side face ends S/4 right of it.
    // Spools stand at the tray's pitch rather than spreading across the width,
    // centered with the box.
    const float skew = tray::kDepthSkew * tray::box_depth(spool);
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

void ams_detail_update_tray(AmsDetailWidgets& w, const helix::ui::WidgetRef slot_widgets[],
                            int slot_count, int unit_index) {
    if (!w.slot_tray || !w.slot_grid)
        return;
    s_tray.valid = false;
    lv_obj_t* container = lv_obj_get_parent(w.slot_grid);

    if (!draws_tray()) {
        // The tray is a draw-hook canvas; this renderer decides per unit
        // whether it has anything to draw.
        lv_obj_add_flag(w.slot_tray, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: draw-hook canvas
        if (!container || !slot_widgets || slot_count <= 0)
            return;
        const int n = std::min(slot_count, kAmsDetailMaxSlots);
        show_lane_humidity(slot_widgets, n, unit_lid_mode(unit_index));
        lv_obj_update_layout(container);
        place_env_pill_beside_spools(w.env_indicator, container, slot_widgets, n);
        return;
    }
    lv_obj_remove_flag(w.slot_tray, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: as above

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

    if (!container || !slot_widgets || slot_count <= 0)
        return;
    const int n = std::min(slot_count, kAmsDetailMaxSlots);

    const tray::LidMode climate_lid = unit_lid_mode(unit_index);
    const tray::UnitShell* shell = view_shell(unit_index, climate_lid != tray::LidMode::None);
    const tray::LidMode lid = shell ? tray::lid_mode(*shell) : climate_lid;
    show_lane_humidity(slot_widgets, n, lid);
    lv_obj_update_layout(container);
    lv_area_t c;
    lv_obj_get_coords(container, &c);
    const lv_point_t origin = {c.x1, c.y1};

    // Spools as laid out: front-plane centres sit S/2 left of the drawn ones.
    float cx[kAmsDetailMaxSlots], cy_sum = 0;
    int32_t spool_size = 0;
    for (int i = 0; i < n; ++i) {
        float y = 0;
        if (!slot_widgets[i] || !spool_centre(slot_widgets[i], origin, cx[i], y, spool_size))
            return;
        cy_sum += y;
    }
    const float size = (float)spool_size;
    const float flange_ry = tray::kSpoolFlangeRadius * size;
    tray::TrayBox box{};
    box.depth = tray::box_depth(size);
    box.rise = tray::box_rise(box.depth);
    const float shell_scale = size / tray::kShellReferenceSpool;
    box.back_extra =
        shell ? shell->back_extra_px * shell_scale : (float)theme_manager_get_spacing("space_md");
    const float skew = tray::kDepthSkew * box.depth;
    for (int i = 0; i < n; ++i)
        s_tray.lane_x[i] = cx[i] - skew / 2;
    const float spacing = n > 1 ? s_tray.lane_x[1] - s_tray.lane_x[0] : tray::spool_pitch(size);
    const float half = tray::lane_lid_half_width(spacing, box);

    // The spools stand on the floor: their mid-depth centre is RISE/2 above the
    // front-plane centre, whose flange bottom sits the floor gap above FB.
    box.fb = cy_sum / n + flange_ry + tray::spool_floor_gap(shell, shell_scale) + box.rise / 2;
    lv_obj_update_layout(w.slot_grid);
    const float front_h = shell ? shell->front_wall_px * shell_scale
                                : std::max(20.0f, lv_obj_get_height(w.slot_grid) / 6.0f);
    box.ft = box.fb - front_h;
    // A unit with a box per lane ends its base with the outer boxes.
    const float cap = shell && !shell->lid_color ? tray::cap_width(*shell, size, spacing) : half;
    box.fl = s_tray.lane_x[0] - cap;
    box.fr = s_tray.lane_x[n - 1] + cap;

    s_tray.lid = lid;
    s_tray.lid_h = tray::lid_height(box, flange_ry);
    if (shell) {
        float crest = 0;
        for (int i = 0; i < shell->dome_n; ++i)
            crest = std::max(crest, shell->dome[i].height_px);
        s_tray.lid_h = crest * shell_scale - box.back_extra / 2.0f;
    }
    s_tray.shell = shell;
    s_tray.spool_size = size;
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
            if (lv_obj_t* name = lv_obj_find_by_name(slot_widgets[i], "name_label"))
                lv_obj_set_style_translate_y(name, label_bottom - la.y2, LV_PART_MAIN);
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
    // Each lane's number stands just right of its tube, inside its bay; the pill clears them.
    float badge_end = 0;
    if (w.badge_layer) {
        lv_area_t layer;
        lv_obj_get_coords(w.badge_layer, &layer);
        const float inset = (float)theme_manager_get_spacing("space_xxs");
        for (int i = 0; i < n; ++i) {
            lv_obj_t* badge = ui_ams_slot_get_status_badge(slot_widgets[i]);
            if (!badge || lv_obj_get_parent(badge) != w.badge_layer)
                continue;
            lv_obj_update_layout(badge);
            const float bw = (float)lv_obj_get_width(badge);
            const tray::PointF p =
                tray::lane_badge_origin(box, cx[i], std::min(spacing / 2.0f, bw + 2 * inset), bw,
                                        (float)lv_obj_get_height(badge), inset);
            badge_end = std::max(badge_end, p.x + bw + inset);
            // The badge's own translate offsets it from its position.
            lv_obj_set_pos(badge,
                           origin.x + (int32_t)std::lround(p.x) - layer.x1 -
                               lv_obj_get_style_translate_x(badge, LV_PART_MAIN),
                           origin.y + (int32_t)std::lround(p.y) - layer.y1 -
                               lv_obj_get_style_translate_y(badge, LV_PART_MAIN));
        }
    }

    // A unit with its own drawing keeps its pill low and tucked over the end of the body,
    // where it covers neither a spool nor the lid's outline.
    if (shell && w.env_indicator && !lv_obj_has_flag(w.env_indicator, LV_OBJ_FLAG_HIDDEN)) {
        const int32_t tuck = theme_manager_get_spacing("space_sm");
        // DECLARATIVE_OK: measured layout; the pill's place depends on the drawn unit
        lv_obj_set_height(w.env_indicator, LV_SIZE_CONTENT);
        clear_translate(w.env_indicator);
        lv_obj_update_layout(w.env_indicator);
        lv_area_t a;
        lv_obj_get_coords(w.env_indicator, &a);
        const int32_t left =
            origin.x + (int32_t)std::lround(tray::pill_left_x(box, badge_end + tuck)) - tuck;
        const int32_t pill_bottom = origin.y + (int32_t)std::lround(box.fb) + tuck;
        lv_obj_set_style_translate_x(w.env_indicator, left - a.x1, LV_PART_MAIN);
        lv_obj_set_style_translate_y(w.env_indicator, pill_bottom - a.y2, LV_PART_MAIN);
    } else if (w.env_indicator && !lv_obj_has_flag(w.env_indicator, LV_OBJ_FLAG_HIDDEN)) {
        const int32_t pill_top = origin.y + (int32_t)std::lround(top);
        const int32_t pill_bottom = origin.y + (int32_t)std::lround(box.fb);
        const int32_t right = origin.x + (int32_t)std::lround(tray::pill_left_x(box, badge_end));
        // DECLARATIVE_OK: measured layout; the pill is as tall as the box it stands against
        lv_obj_set_height(w.env_indicator, std::max<int32_t>(0, pill_bottom - pill_top + 1));
        clear_translate(w.env_indicator);
        lv_obj_update_layout(w.env_indicator);
        lv_area_t a;
        lv_obj_get_coords(w.env_indicator, &a);
        lv_obj_set_style_translate_x(w.env_indicator, right - a.x1, LV_PART_MAIN);
        lv_obj_set_style_translate_y(w.env_indicator, pill_top - a.y1, LV_PART_MAIN);
    }

    lv_obj_invalidate(container);
    spdlog::debug("[AmsDetail] Tray: box x {:.1f}..{:.1f} y {:.1f}..{:.1f}, depth {:.1f}, lid {}",
                  box.fl, box.fr, box.ft, box.fb, box.depth, static_cast<int>(s_tray.lid));
}

void ams_detail_update_labels(AmsDetailWidgets& w, helix::ui::WidgetRef slot_widgets[],
                              int slot_count, const AmsSlotLayout& layout) {
    if (!w.labels_layer || slot_count <= 4)
        return;

    helix::ui::safe_clean_children(w.labels_layer);

    int32_t slot_spacing = layout.slot_width - layout.overlap;
    const int32_t label_width =
        helix::ui::ams_stagger_label_width(slot_spacing, helix::widget_size::current_breakpoint(),
                                           theme_manager_get_spacing("space_xs"));

    for (int i = 0; i < slot_count; ++i) {
        if (slot_widgets[i]) {
            // Formula matches slot_grid flex positions, plus centering offset
            int32_t slot_center_x =
                layout.centering_offset + layout.slot_width / 2 + i * slot_spacing;
            ui_ams_slot_move_label_to_layer(slot_widgets[i], w.labels_layer, slot_center_x,
                                            label_width);
        }
    }

    spdlog::debug("[AmsDetail] Moved {} labels to overlay layer", slot_count);
}

void ams_detail_update_badges(AmsDetailWidgets& w, helix::ui::WidgetRef slot_widgets[],
                              int slot_count, const AmsSlotLayout& layout) {
    if (!w.badge_layer)
        return;

    // Clean stale badges from previous unit view (badges are reparented here
    // from slot widgets, so they persist across unit switches if not cleaned)
    helix::ui::safe_clean_children(w.badge_layer);

    int32_t slot_spacing = layout.slot_width - layout.overlap;
    lv_obj_update_layout(w.badge_layer);

    for (int i = 0; i < slot_count; ++i) {
        if (slot_widgets[i]) {
            int32_t slot_center_x =
                layout.centering_offset + layout.slot_width / 2 + i * slot_spacing;
            ui_ams_slot_move_badge_to_layer(slot_widgets[i], w.badge_layer, slot_center_x);
        }
    }

    spdlog::debug("[AmsDetail] Moved {} badges to overlay layer", slot_count);
}

void ams_detail_setup_path_canvas(lv_obj_t* canvas, lv_obj_t* slot_grid, int unit_index) {
    if (!canvas)
        return;

    auto* backend = helix::AmsState::instance().get_backend();
    const auto info = helix::AmsState::instance().system_info();
    if (!backend || !info)
        return;

    ams_detail_setup_path_canvas(canvas, slot_grid, unit_index, backend, *info);
}

// NAMESPACE_OK: an overload of the global three-argument ams_detail_setup_path_canvas
void ams_detail_setup_path_canvas(lv_obj_t* canvas, lv_obj_t* slot_grid, int unit_index,
                                  const helix::AmsBackend* backend,
                                  const helix::AmsSystemInfo& info) {
    if (!canvas || !backend)
        return;

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
    ui_filament_path_canvas_set_show_bypass(canvas,
                                            helix::ui::bypass_node_visible_for(backend, info));

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
    // Whatever moves inside a unit with a built-in selector, one line leaves its middle.
    // A unit's enclosed and open drawings merge their lanes alike, so no climate lookup.
    const tray::UnitShell* drawn_unit = tray::unit_shell_for(
        info, (unit_index < 0 && info.units.size() == 1) ? 0 : unit_index, false);
    const tray::LaneJoin join = helix::ui::fpath::drawn_lane_join(
        drawn_unit ? drawn_unit->lane_join : tray::LaneJoin::Hub, topo);
    if (join == tray::LaneJoin::Junction || join == tray::LaneJoin::Inside)
        topo = helix::PathTopology::HUB;
    ui_filament_path_canvas_set_topology(canvas, static_cast<int>(topo));
    ui_filament_path_canvas_set_hub_on_toolhead(canvas, backend->hub_on_toolhead());
    ui_filament_path_canvas_set_lane_join(canvas, join);
    // Only the gate counts the rail art exists for trade the bar for a rail.
    const tray::RailFaceArt* rail = drawn_unit && topo == helix::PathTopology::LINEAR
                                        ? tray::unit_shell_face_art(*drawn_unit, slot_count)
                                        : nullptr;
    ui_filament_path_canvas_set_selector_rail(canvas, rail ? drawn_unit->id : nullptr, rail);
    // A unit the overview joins to no nozzle (an unwired hub) ends at its hub
    // here too: the overview's own rule decides both. A hub mounted on the head feeds
    // it by construction, and the all-units view always reaches a toolhead.
    const bool linear_hub = topo == helix::PathTopology::HUB || topo == helix::PathTopology::LINEAR;
    const bool feeds_toolhead = !linear_hub || unit_index < 0 || backend->hub_on_toolhead() ||
                                helix::ui::unit_feeds_toolhead(info, backend, unit_index);
    ui_filament_path_canvas_set_feeds_toolhead(canvas, feeds_toolhead);

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

    // Map active slot to local index for unit-scoped views. A unit the current lane is not in
    // still owns its trunk when one of its lanes is through the hub, seated in its own tool.
    int active_slot = info.path_active_slot();
    helix::PathSegment segment = backend->get_filament_segment();
    if (unit_index >= 0) {
        int local_active = active_slot - slot_offset;
        active_slot = (local_active >= 0 && local_active < slot_count) ? local_active : -1;
        const helix::ui::UnitLane own =
            active_slot < 0 && unit_index < static_cast<int>(info.units.size())
                ? helix::ui::unit_furthest_lane(*backend, info.units[unit_index])
                : helix::ui::UnitLane{};
        if (own.segment > helix::PathSegment::HUB) {
            active_slot = own.global - slot_offset;
            segment = own.segment;
        }
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
    ui_filament_path_canvas_set_filament_segment(canvas, static_cast<int>(segment));

    // Bowden progress fills the output tube from the hub: as far as a load has
    // pushed, or what an unload has yet to pull back.
    {
        const helix::IGateSelector* const gates = backend->as<helix::IGateSelector>();
        const int progress = gates ? gates->get_bowden_progress() : -1;
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
        const std::string caption = helix::ui::upstream_lane_caption(info, *backend, global_idx);
        ui_filament_path_canvas_set_slot_upstream(canvas, i,
                                                  caption.empty() ? nullptr : caption.c_str());
    }

    // The buffer box: a unit buffer's health, a system buffer's motion, or a
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
    const bool show_bypass_spool = ext_spool.has_value() && feeds_toolhead &&
                                   helix::ui::bypass_node_visible_for(backend, info);
    ui_filament_path_canvas_set_bypass_has_spool(canvas, show_bypass_spool);
    if (show_bypass_spool) {
        ui_filament_path_canvas_set_bypass_color(canvas, ext_spool->color_rgb);
    }

    lv_area_t toolhead{}, canvas_area{};
    const bool has_toolhead = ui_filament_path_canvas_get_toolhead_box(canvas, &toolhead);
    lv_obj_get_coords(canvas, &canvas_area);
    spdlog::debug("[AmsDetail] Path canvas configured: slots={}, unit={}, canvas y {}..{}, "
                  "toolhead y {}..{}",
                  slot_count, unit_index, canvas_area.y1, canvas_area.y2,
                  has_toolhead ? toolhead.y1 : -1, has_toolhead ? toolhead.y2 : -1);
}

void ams_detail_pre_show_env_indicator(AmsDetailWidgets& w, int unit_index) {
    if (!w.env_indicator)
        return;

    const int u = (unit_index >= 0) ? unit_index : 0; // -1 == whole/single-unit → unit 0
    helix::AmsState::instance().set_detail_env_unit(u);
    lv_obj_set_user_data(w.env_indicator, reinterpret_cast<void*>(static_cast<intptr_t>(u)));

    // Shown ahead of its first reading, so the flex layout that sizes the slots
    // reserves its width; ams_env_ind_detail_visible takes over from the next
    // reading on.
    auto* backend = helix::AmsState::instance().get_backend();
    if (backend && backend->has_environment_sensors()) {
        lv_obj_remove_flag(w.env_indicator, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: layout
        spdlog::debug("[AmsDetail] Pre-showed env indicator for unit {}", u);
    } else {
        lv_obj_add_flag(w.env_indicator, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: as above
    }
}

// ============================================================================
// Shared Context-Menu Dispatch
// ============================================================================

namespace helix {
namespace ui {

BufferBoxState ams_detail_buffer_box(const AmsSystemInfo& info, int unit_index) {
    BufferBoxState box;
    // A unit's buffer describes one unit: the one asked for, else the one the
    // system reading came from. Without one, a buffer serving the whole system.
    const auto motion_state = [](BufferMotion m) {
        return m == BufferMotion::Advancing ? 1 : m == BufferMotion::Trailing ? 2 : 0;
    };
    const AmsUnit* unit = info.get_unit(buffer_view_unit(info, unit_index));
    if (unit && unit->buffer_health) {
        const BufferHealth& h = *unit->buffer_health;
        box.present = true;
        box.state = motion_state(h.motion);
        box.fault = static_cast<int>(buffer_fault_status(h));
    }
    if (!box.present && info.system_buffer_motion) {
        box.present = true;
        box.state = motion_state(*info.system_buffer_motion);
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

UnitLane unit_furthest_lane(const AmsBackend& backend, const AmsUnit& unit) {
    UnitLane lane;
    for (int s = 0; s < unit.slot_count; ++s) {
        const int global = unit.first_slot_global_index + s;
        const PathSegment seg = backend.get_slot_filament_segment(global);
        if (seg > lane.segment)
            lane = {global, seg, backend.get_slot_info(global).color_rgb};
    }
    return lane;
}

bool ams_dispatch_backend_action(AmsContextMenu::MenuAction action, int slot,
                                 lv_obj_t* path_canvas) {
    using MenuAction = AmsContextMenu::MenuAction;

    switch (action) {
    case MenuAction::EJECT:
    case MenuAction::RECOVER_POSITION:
    case MenuAction::CHECK_GATE:
    case MenuAction::PRELOAD:
    case MenuAction::CLEAR_SPOOL:
    case MenuAction::MOUNT:
    case MenuAction::PARK:
    case MenuAction::CLEAR_FAULT:
        break;
    default:
        return false; // caller's own switch owns it
    }

    // This function is public and takes a raw slot index, so the bypass sentinel
    // reaches it. The external spool is not one of the backend's lanes: it carries
    // no number to name, apply_user_edit() refuses the index, and a printer with no
    // filament system has one too, so it clears through its own store first.
    if (action == MenuAction::CLEAR_SPOOL && slot == kExternalSpoolSlot) {
        AmsState::instance().commit_external_spool_edit(SlotInfo{});
        NOTIFY_INFO(lv_tr("External spool cleared"));
        return true;
    }

    auto* backend = helix::AmsState::instance().get_backend(0);
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
        ILaneRecovery* const recovery = backend->as<ILaneRecovery>();
        AmsError error =
            recovery ? recovery->recover_lane_position(slot)
                     : AmsErrorHelper::not_supported("Lane position recovery not supported");
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Recovery failed"));
        }
        break;
    }

    case MenuAction::CLEAR_FAULT: {
        AmsError error = backend->clear_fault(slot);
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Clear failed"));
        }
        break;
    }

    case MenuAction::MOUNT: {
        // The toolhead tap's own verdict: refusals and the paused-job
        // confirmation are the same as from any other tool control.
        if (const auto head = backend->head_for_slot(slot)) {
            offer_tool_change(get_printer_state(), *head);
        }
        break;
    }

    case MenuAction::PARK:
        request_tool_park(get_printer_state());
        break;

    case MenuAction::CHECK_GATE: {
        IGateSelector* const gates = backend->as<IGateSelector>();
        AmsError error = gates ? gates->check_gate(slot)
                               : AmsErrorHelper::not_supported("Gate check not supported");
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Check slot failed"));
        }
        break;
    }

    case MenuAction::PRELOAD: {
        IGateSelector* const gates = backend->as<IGateSelector>();
        AmsError error = gates ? gates->preload_lane(slot)
                               : AmsErrorHelper::not_supported("Lane preload not supported");
        if (error.result != AmsResult::SUCCESS) {
            notify_ams_error(error, lv_tr("Preload failed"));
        }
        break;
    }

    case MenuAction::CLEAR_SPOOL: {
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
        // The menu greys Clear with this reason; a caller holding an older
        // menu is refused here, quietly, since the reason is already shown.
        if (const std::string refusal = backend->clear_spool_refusal(slot); !refusal.empty()) {
            spdlog::info("[AMS] Clear of slot {} refused: {}", slot, refusal);
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
        cleared.color_rgb = kAmsDefaultSlotColor;
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
            helix::ui::offer_edit_follow_up(slot, original, cleared);
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
                        helix::ui::active_tool_lane_label(backend->lane_noun(), slot));
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
    uint32_t color_rgb = kAmsDefaultSlotColor;
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

std::unordered_set<int>& insert_prompt_claims() {
    static std::unordered_set<int> claims;
    return claims;
}

void clear_if_lane_unchanged(int slot) {
    auto& offers = insert_offers();
    const auto it = offers.find(slot);
    if (it == offers.end()) {
        return;
    }
    const InsertOfferSnapshot shown = it->second.shown;
    offers.erase(it);
    auto* backend = helix::AmsState::instance().get_backend(0);
    if (!backend || !(InsertOfferSnapshot::of(backend->get_slot_info(slot)) == shown)) {
        spdlog::debug("[AMS] Slot {} changed since the same-spool notice; Clear ignored", slot);
        return;
    }
    ams_dispatch_backend_action(AmsContextMenu::MenuAction::CLEAR_SPOOL, slot, nullptr);
}

} // namespace

void reset_insert_offers_for_test() {
    insert_offers().clear();
    insert_prompt_claims().clear();
}

void claim_insert_prompt(int slot) {
    insert_prompt_claims().insert(slot);
}

void release_insert_prompt(int slot) {
    insert_prompt_claims().erase(slot);
}

void release_all_insert_prompts() {
    insert_prompt_claims().clear();
}

bool insert_prompt_claimed(int slot) {
    return insert_prompt_claims().count(slot) != 0;
}

void offer_clear_after_unverified_insert(int slot) {
    if (insert_prompt_claimed(slot)) {
        spdlog::debug("[AMS] Slot {} insert belongs to the slot editor; no same-spool notice",
                      slot);
        return;
    }
    auto* backend = helix::AmsState::instance().get_backend(0);
    if (!backend ||
        clear_spool_blocked_by_print(get_printer_state().print_state().get_print_lifecycle(),
                                     backend->slot_is_actively_loaded(slot)) ||
        !backend->clear_spool_refusal(slot).empty()) {
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
                    active_tool_lane_label(backend->lane_noun(), slot));
    // The slot rides in user_data by value: the toast can outlive any object
    // that could own it, and the dispatch re-checks every guard at tap time.
    ToastManager::instance().show_with_action(
        ToastSeverity::INFO, message.c_str(), lv_tr("Clear"),
        [](void* user_data) {
            clear_if_lane_unchanged(static_cast<int>(reinterpret_cast<intptr_t>(user_data)));
        },
        reinterpret_cast<void*>(static_cast<intptr_t>(slot)), 10000);
}

namespace {
const UiRequestRegistration s_insert_offer_registration{
    UiRequestKind::OfferClearAfterUnverifiedInsert,
    [](const UiRequest& request) { offer_clear_after_unverified_insert(request.slot); }};
} // namespace

} // namespace ui
} // namespace helix
