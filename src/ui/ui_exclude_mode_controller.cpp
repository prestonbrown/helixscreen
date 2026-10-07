// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_mode_controller.h"

#include "ui_gcode_viewer.h"

#include "gcode_parser.h"
#include "layout_manager.h"
#include "observer_factory.h"
#include "print_status_layout_decision.h"
#include "printer_excluded_objects_state.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

ExcludeModeController::~ExcludeModeController() {
    // The viewer can outlive us; it must not call back into a freed controller.
    if (viewer_ && lv_is_initialized()) {
        ui_gcode_viewer_set_object_tap_callback(viewer_, nullptr, nullptr);
    }
}

void ExcludeModeController::show(const ExcludeModeTargets& targets,
                                 PrinterExcludedObjectsState* state, ExcludeTapMode mode,
                                 ObjectTapFn on_tap) {
    if (is_open() || !state || !targets.columns) {
        return;
    }
    state_ = state;
    viewer_ = targets.gcode_viewer;
    map_active_ = targets.map_active;
    on_tap_ = std::move(on_tap);
    const ObjectTapFn forward = [this](const std::string& name) {
        if (on_tap_) {
            on_tap_(name);
        }
    };

    if (targets.thumbnail_mode && targets.card) {
        // Set before the map exists, so no frame shows it over a live thumbnail.
        if (map_active_) {
            lv_subject_set_int(map_active_, 1);
        }
        map_view_ = std::make_unique<ExcludeObjectMapView>();
        map_view_->set_close_callback([this]() { hide(); });
        // The map copies what it draws; the viewer may free its parse while the map is open.
        map_view_->create(targets.card, state_, targets.bed_w_mm, targets.bed_h_mm, forward, mode,
                          viewer_ ? ui_gcode_viewer_get_parsed_file(viewer_) : nullptr);
        // The list's X closes the whole mode; one dismiss control is enough.
        if (auto* map_root = map_view_->root()) {
            if (lv_obj_t* map_close = lv_obj_find_by_name(map_root, "close_btn")) {
                lv_obj_add_flag(map_close, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Landscape covers the right-hand column by ratio; portrait sizes the list
    // to the control stack it covers, up to the preview card, so measure both.
    const bool portrait = helix::is_portrait_layout(helix::LayoutManager::instance().type());
    int32_t controls_h = 0;
    int32_t content_h = 0;
    int32_t gap = 0;
    int32_t room_h = 0;
    if (portrait) {
        lv_obj_update_layout(targets.columns);
        if (targets.controls_name) {
            if (lv_obj_t* controls = lv_obj_find_by_name(targets.columns, targets.controls_name)) {
                controls_h = lv_obj_get_height(controls);
            }
        }
        content_h = lv_obj_get_content_height(targets.columns);
        gap = lv_obj_get_style_pad_row(targets.columns, LV_PART_MAIN);
        if (targets.card) {
            lv_area_t columns_area;
            lv_area_t card_area;
            lv_obj_get_content_coords(targets.columns, &columns_area);
            lv_obj_get_coords(targets.card, &card_area);
            room_h = columns_area.y2 - card_area.y2 - gap;
        }
    }

    side_list_ = std::make_unique<ExcludeObjectSideList>();
    side_list_->set_close_callback([this]() { hide(); });
    side_list_->set_gcode_viewer(viewer_);
    side_list_->create(targets.columns, state_, forward, mode,
                       exclude_side_list_geometry(portrait, controls_h, content_h, gap, room_h));

    // Installed whatever the mode, so switching thumbnail -> 2D/3D while open
    // still routes render taps.
    if (viewer_) {
        ui_gcode_viewer_set_object_tap_callback(viewer_, on_viewer_tap, this);
        ui_gcode_viewer_set_excluded_badges_pickable(viewer_, mode == ExcludeTapMode::Toggle);
    }

    const auto refresh = [](ExcludeModeController* self, int) { self->refresh_render_badges(); };
    excluded_obs_ = observe<int>(state_->get_excluded_objects_version_subject(), this, refresh,
                                 state_->get_subjects_lifetime());
    defined_obs_ = observe<int>(state_->get_defined_objects_version_subject(), this, refresh,
                                state_->get_subjects_lifetime());
    refresh_render_badges();
    spdlog::debug("[ExcludeMode] Opened ({})", targets.thumbnail_mode ? "map" : "render");
}

void ExcludeModeController::hide() {
    excluded_obs_.reset();
    defined_obs_.reset();
    if (viewer_) {
        ui_gcode_viewer_set_object_tap_callback(viewer_, nullptr, nullptr);
        ui_gcode_viewer_set_excluded_badges_pickable(viewer_, false);
        ui_gcode_viewer_set_highlighted_objects(viewer_, {});
        ui_gcode_viewer_set_object_badges(viewer_, {});
    }
    if (side_list_) {
        side_list_->destroy();
        side_list_.reset();
    }
    if (map_view_) {
        map_view_->destroy();
        map_view_.reset();
    }
    if (map_active_) {
        lv_subject_set_int(map_active_, 0);
    }
    viewer_ = nullptr;
    map_active_ = nullptr;
    state_ = nullptr;
    on_tap_ = nullptr;
}

void ExcludeModeController::refresh_render_badges() {
    if (!viewer_ || !state_ || !is_open()) {
        return;
    }
    ui_gcode_viewer_set_object_badges(
        viewer_, compute_object_badges(*state_, ui_gcode_viewer_get_parsed_file(viewer_)));
}

void ExcludeModeController::on_viewer_tap(lv_obj_t* /*viewer*/, const char* name, void* user_data) {
    auto* self = static_cast<ExcludeModeController*>(user_data);
    if (!self || !name || name[0] == '\0' || !self->on_tap_) {
        return;
    }
    spdlog::info("[ExcludeMode] Viewer tap on object: '{}'", name);
    self->on_tap_(std::string(name));
}

} // namespace helix::ui
