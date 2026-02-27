// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail.h"

#include "ui_ams_slot.h"
#include "ui_filament_path_canvas.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "printer_detector.h"

#include <spdlog/spdlog.h>

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

    lv_obj_set_height(w.slot_tray, tray_height);
    lv_obj_align(w.slot_tray, LV_ALIGN_BOTTOM_MID, 0, 0);

    spdlog::debug("[AmsDetail] Tray sized to {}px (1/4 of {}px grid)", tray_height, grid_height);
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

    // Use Stealthburner toolhead for Voron printers
    if (PrinterDetector::is_voron_printer()) {
        ui_filament_path_canvas_set_faceted_toolhead(canvas, true);
    }

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
        if (unit.buffer_health.has_value() && unit.buffer_health->fault_detection_enabled) {
            if (unit.buffer_health->distance_to_fault > 0.0f) {
                buffer_fault = 1; // Approaching fault — yellow tint
            }
        }
    }
    ui_filament_path_canvas_set_buffer_fault_state(canvas, buffer_fault);

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
