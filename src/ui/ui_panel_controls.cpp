// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_controls.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_modal.h"
#include "ui_motors_off.h"
#include "ui_notification.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_pa.h"
#include "ui_panel_calibration_tool_offset.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_motion.h"
#include "ui_panel_screws_tilt.h"
#include "ui_position_utils.h"
#include "ui_settings_sensors.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "format_utils.h"
#include "hardware_role_registry.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "operation_timeout_guard.h"
#include "printer_state.h"
#include "quick_action_slots.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"
#include "temperature_service.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_cleanup_helpers.h"
#include "ui/ui_widget_helpers.h"
#include "z_offset_utils.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm> // std::clamp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace helix;
using helix::ui::observe;
using helix::ui::temperature::deci_to_degrees;

// Forward declarations for class-based API
class MotionPanel;

using helix::ui::position::format_position;

namespace {

/// "Off" at zero, otherwise "N%": how every fan speed on this panel reads.
void format_fan_speed(int pct, char* buf, size_t size) {
    if (pct > 0) {
        helix::format::format_percent(pct, buf, size);
    } else {
        std::snprintf(buf, size, "%s", lv_tr("Off"));
    }
}

} // namespace

// ============================================================================
// CONSTRUCTOR
// ============================================================================

ControlsPanel::ControlsPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Dependencies passed for interface consistency
    // Child panels (motion, temp, extrusion) may use these when wired
}

ControlsPanel::~ControlsPanel() {
    quick_actions_.release_widgets();

    deinit_subjects();

    // Modal dialogs: ModalGuard handles cleanup automatically via RAII
    // See docs/DEVELOPER_QUICK_REFERENCE.md "Modal Dialog Lifecycle"
}

// ============================================================================
// DEPENDENCY INJECTION
// ============================================================================

void ControlsPanel::set_temp_control_panel(TemperatureService* temp_panel) {
    temp_control_panel_ = temp_panel;
    spdlog::trace("[{}] TemperatureService reference set", get_name());
}

helix::TemperatureController* ControlsPanel::controller() const {
    return temp_control_panel_ ? temp_control_panel_->controller() : nullptr;
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void ControlsPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize dashboard display subjects for card live data
    // Using UI_MANAGED_SUBJECT_* macros for automatic RAII cleanup via SubjectManager

    // Nozzle label (dynamic for multi-tool)
    UI_MANAGED_SUBJECT_STRING(nozzle_label_subject_, nozzle_label_buf_, lv_tr("Nozzle"),
                              "controls_nozzle_label", subjects_);

    // Nozzle temperature display
    UI_MANAGED_SUBJECT_STRING(nozzle_temp_subject_, nozzle_temp_buf_, "—°C", "controls_nozzle_temp",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(nozzle_pct_subject_, 0, "controls_nozzle_pct", subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_status_subject_, nozzle_status_buf_, "",
                              "controls_nozzle_status", subjects_);
    UI_MANAGED_SUBJECT_INT(nozzle_status_state_subject_, 0, "controls_nozzle_status_state",
                           subjects_);

    // Bed temperature display
    UI_MANAGED_SUBJECT_STRING(bed_temp_subject_, bed_temp_buf_, "—°C", "controls_bed_temp",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(bed_pct_subject_, 0, "controls_bed_pct", subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_status_subject_, bed_status_buf_, "", "controls_bed_status",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(bed_status_state_subject_, 0, "controls_bed_status_state", subjects_);

    // Chamber temperature display
    UI_MANAGED_SUBJECT_STRING(chamber_status_subject_, chamber_status_buf_, "",
                              "controls_chamber_status", subjects_);
    UI_MANAGED_SUBJECT_INT(chamber_status_state_subject_, 0, "controls_chamber_status_state",
                           subjects_);

    // Sensors beyond the dedicated rows ("N more sensors" link on the temperature card)
    UI_MANAGED_SUBJECT_STRING(more_sensors_subject_, more_sensors_buf_, "", "controls_more_sensors",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(more_sensors_count_, 0, "controls_more_sensors_count", subjects_);

    // Fan speed display
    UI_MANAGED_SUBJECT_STRING(fan_speed_subject_, fan_speed_buf_, lv_tr("Off"),
                              "controls_fan_speed", subjects_);
    UI_MANAGED_SUBJECT_INT(fan_pct_subject_, 0, "controls_fan_pct", subjects_);

    quick_actions_.init_subjects(subjects_);

    // Z-Offset delta display (for banner showing unsaved adjustment)
    UI_MANAGED_SUBJECT_STRING(z_offset_delta_display_subject_, z_offset_delta_display_buf_, "",
                              "z_offset_delta_display", subjects_);

    // Homing status subjects for bind_style visual feedback
    UI_MANAGED_SUBJECT_INT(x_homed_, 0, "x_homed", subjects_);
    UI_MANAGED_SUBJECT_INT(y_homed_, 0, "y_homed", subjects_);
    UI_MANAGED_SUBJECT_INT(xy_homed_, 0, "xy_homed", subjects_);
    UI_MANAGED_SUBJECT_INT(z_homed_, 0, "z_homed", subjects_);
    UI_MANAGED_SUBJECT_INT(all_homed_, 0, "all_homed", subjects_);

    // Position display subjects for Position card
    // Format: numeric value only (axis label is static in XML for proper alignment)
    std::strcpy(controls_pos_x_buf_, "   —   mm");
    std::strcpy(controls_pos_y_buf_, "   —   mm");
    std::strcpy(controls_pos_z_buf_, "   —   mm");
    UI_MANAGED_SUBJECT_STRING(controls_pos_x_subject_, controls_pos_x_buf_, "   —   mm",
                              "controls_pos_x", subjects_);
    UI_MANAGED_SUBJECT_STRING(controls_pos_y_subject_, controls_pos_y_buf_, "   —   mm",
                              "controls_pos_y", subjects_);
    UI_MANAGED_SUBJECT_STRING(controls_pos_z_subject_, controls_pos_z_buf_, "   —   mm",
                              "controls_pos_z", subjects_);

    // Speed override display subject
    std::strcpy(speed_override_buf_, "100%");
    UI_MANAGED_SUBJECT_STRING(speed_override_subject_, speed_override_buf_, "100%",
                              "controls_speed_pct", subjects_);

    // Operation timeout guard (disables buttons while homing/QGL/Z-tilt in progress)
    operation_guard_.init_subject("controls_operation_in_progress", subjects_);

    // Z-offset display subject for live tuning
    std::strcpy(controls_z_offset_buf_, "+0.000mm");
    UI_MANAGED_SUBJECT_STRING(controls_z_offset_subject_, controls_z_offset_buf_, "+0.000mm",
                              "controls_z_offset", subjects_);

    // Observe homed_axes from PrinterState to update homing subjects using string observer
    homed_axes_observer_ = observe<const char*>(
        printer_state_.motion_state().get_homed_axes_subject(), this,
        [](ControlsPanel* self, const char* axes) {
            bool has_x = strchr(axes, 'x') != nullptr;
            bool has_y = strchr(axes, 'y') != nullptr;
            bool has_z = strchr(axes, 'z') != nullptr;

            int x = has_x ? 1 : 0;
            int y = has_y ? 1 : 0;
            int xy = (has_x && has_y) ? 1 : 0;
            int z = has_z ? 1 : 0;
            int all = (has_x && has_y && has_z) ? 1 : 0;

            // Only update if changed (avoid unnecessary redraws)
            bool changed = false;
            if (lv_subject_get_int(&self->x_homed_) != x) {
                lv_subject_set_int(&self->x_homed_, x);
                changed = true;
            }
            if (lv_subject_get_int(&self->y_homed_) != y) {
                lv_subject_set_int(&self->y_homed_, y);
                changed = true;
            }
            if (lv_subject_get_int(&self->xy_homed_) != xy) {
                lv_subject_set_int(&self->xy_homed_, xy);
                changed = true;
            }
            if (lv_subject_get_int(&self->z_homed_) != z) {
                lv_subject_set_int(&self->z_homed_, z);
                changed = true;
            }
            if (lv_subject_get_int(&self->all_homed_) != all) {
                lv_subject_set_int(&self->all_homed_, all);
                changed = true;
            }

            if (changed) {
                spdlog::info("[ControlsPanel] Homing status changed: x={}, y={}, z={}, all={} "
                             "(axes='{}')",
                             x, y, z, all, axes);
            }
        },
        printer_state_.get_subjects_lifetime());

    // on_controls_macro parses its slot from user_data, so it stays a named handler.
    register_xml_callbacks({
        {"on_calibration_bed_mesh",
         [](lv_event_t*) { get_global_controls_panel().handle_calibration_bed_mesh(); }},
        {"on_calibration_zoffset",
         [](lv_event_t*) { get_global_controls_panel().handle_calibration_zoffset(); }},
        {"on_calibration_tool_offsets",
         [](lv_event_t*) { get_global_controls_panel().handle_calibration_tool_offsets(); }},
        {"on_calibration_pa",
         [](lv_event_t*) { get_global_controls_panel().handle_calibration_pa(); }},
        {"on_calibration_screws",
         [](lv_event_t*) { get_global_controls_panel().handle_calibration_screws(); }},
        {"on_calibration_motors",
         [](lv_event_t*) { get_global_controls_panel().handle_calibration_motors(); }},

        {"on_controls_home_all",
         [](lv_event_t*) { get_global_controls_panel().handle_home_all(); }},
        {"on_controls_home_x", [](lv_event_t*) { get_global_controls_panel().handle_home_x(); }},
        {"on_controls_home_y", [](lv_event_t*) { get_global_controls_panel().handle_home_y(); }},
        {"on_controls_home_xy", [](lv_event_t*) { get_global_controls_panel().handle_home_xy(); }},
        {"on_controls_home_z", [](lv_event_t*) { get_global_controls_panel().handle_home_z(); }},
        {"on_controls_qgl", [](lv_event_t*) { get_global_controls_panel().handle_qgl(); }},
        {"on_controls_z_tilt", [](lv_event_t*) { get_global_controls_panel().handle_z_tilt(); }},
        {"on_controls_macro", on_macro},
        {"on_controls_fan_slider",
         [](lv_event_t* e) {
             get_global_controls_panel().handle_fan_slider_changed(
                 lv_slider_get_value(lv_event_get_target_obj(e)));
         }},

        {"on_controls_save_z_offset",
         [](lv_event_t*) { get_global_controls_panel().handle_save_z_offset(); }},
        {"on_zoffset_tune", [](lv_event_t*) { get_global_controls_panel().handle_zoffset_tune(); }},

        // Cards and rows that open a full overlay
        {"on_controls_quick_actions",
         [](lv_event_t*) { get_global_controls_panel().handle_quick_actions_clicked(); }},
        {"on_nozzle_temp_clicked",
         [](lv_event_t*) { get_global_controls_panel().handle_nozzle_temp_clicked(); }},
        {"on_bed_temp_clicked",
         [](lv_event_t*) { get_global_controls_panel().handle_bed_temp_clicked(); }},
        {"on_chamber_temp_clicked",
         [](lv_event_t*) { get_global_controls_panel().handle_chamber_temp_clicked(); }},
        {"on_controls_cooling",
         [](lv_event_t*) { get_global_controls_panel().handle_cooling_clicked(); }},
        {"on_controls_more_sensors",
         [](lv_event_t*) { get_global_controls_panel().handle_secondary_temps_clicked(); }},
        {"on_controls_secondary_fans",
         [](lv_event_t*) { get_global_controls_panel().handle_secondary_fans_clicked(); }},

        // Pencil icons: open the temperature keypad
        {"on_nozzle_target_edit",
         [](lv_event_t*) { get_global_controls_panel().handle_nozzle_target_edit(); }},
        {"on_bed_target_edit",
         [](lv_event_t*) { get_global_controls_panel().handle_bed_target_edit(); }},
        {"on_chamber_target_edit",
         [](lv_event_t*) { get_global_controls_panel().handle_chamber_target_edit(); }},
    });

    subjects_initialized_ = true;
    spdlog::trace("[{}] Dashboard subjects initialized", get_name());
}

void ControlsPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    temp_observers_.clear();
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[Controls Panel] Subjects deinitialized ({} subjects)", subjects_.count());
}

void ControlsPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    quick_actions_.setup(panel_, parent_screen, printer_state_, api_);

    // Cache dynamic container for secondary fans
    // required-names: controls_panel
    secondary_fans_list_ = helix::ui::find_required(panel_, "secondary_fans_list", get_name());

    // Bind heating icon animators for nozzle/bed/chamber status visualization.
    // The binder owns its own temperature observers, so the panel does not need
    // to feed them from update_*_temp_display().
    nozzle_icon_binder_.bind(panel_, printer_state_, helix::HeaterType::Nozzle);
    bed_icon_binder_.bind(panel_, printer_state_, helix::HeaterType::Bed);
    chamber_icon_binder_.bind(panel_, printer_state_, helix::HeaterType::Chamber);

    // Register observers for live data updates
    register_observers();

    // Populate secondary fans on initial setup (will be empty until discovery)
    populate_secondary_fans();

    update_more_sensors();

    spdlog::debug("[{}] Setup complete", get_name());
}

void ControlsPanel::on_activate() {
    active_ = true;

    // Reset coalescing flags to prevent stale state from a previous deactivation
    fans_rebuild_pending_ = false;

    // Force-refresh all displays so UI catches up on state changes missed while hidden
    refresh_all_displays();

    // Refresh secondary fans list when panel becomes visible
    // This handles edge cases where:
    // 1. Fan discovery completed after initial setup
    // 2. User switched from one printer connection to another
    // 3. Observer callback was missed due to timing
    populate_secondary_fans();

    // Re-read the slot config: the user may have changed settings
    quick_actions_.reload();

    spdlog::trace("[{}] Panel activated", get_name());
}

void ControlsPanel::on_deactivating(DeactivateReason) {
    active_ = false;
    spdlog::trace("[{}] Panel deactivated, observer callbacks will skip UI updates", get_name());
}

void ControlsPanel::refresh_all_displays() {
    // Re-read cached values from subjects and update all formatted displays
    if (auto* subj = printer_state_.temperature_state().get_active_extruder_temp_subject()) {
        cached_extruder_temp_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.temperature_state().get_active_extruder_target_subject()) {
        cached_extruder_target_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.temperature_state().get_bed_temp_subject()) {
        cached_bed_temp_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.temperature_state().get_bed_target_subject()) {
        cached_bed_target_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.temperature_state().get_chamber_temp_subject()) {
        cached_chamber_temp_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.temperature_state().get_chamber_target_subject()) {
        cached_chamber_target_ = lv_subject_get_int(subj); // keypad seed
    }
    if (auto* subj = printer_state_.temperature_state().get_chamber_effective_target_subject()) {
        cached_chamber_effective_target_ = lv_subject_get_int(subj); // status display
    }
    if (auto* subj = printer_state_.temperature_state().get_chamber_mode_subject()) {
        cached_chamber_mode_ = lv_subject_get_int(subj); // M141 control mode
    }
    update_nozzle_temp_display();
    update_bed_temp_display();
    update_chamber_temp_display();
    update_fan_display();
    update_nozzle_label();
    update_speed_display();

    // Re-read position subjects
    if (auto* subj = printer_state_.motion_state().get_gcode_position_x_subject()) {
        int centimm = lv_subject_get_int(subj);
        format_position(centimm, controls_pos_x_buf_, sizeof(controls_pos_x_buf_));
        lv_subject_copy_string(&controls_pos_x_subject_, controls_pos_x_buf_);
    }
    if (auto* subj = printer_state_.motion_state().get_gcode_position_y_subject()) {
        int centimm = lv_subject_get_int(subj);
        format_position(centimm, controls_pos_y_buf_, sizeof(controls_pos_y_buf_));
        lv_subject_copy_string(&controls_pos_y_subject_, controls_pos_y_buf_);
    }
    if (auto* subj = printer_state_.motion_state().get_gcode_position_z_subject()) {
        int centimm = lv_subject_get_int(subj);
        format_position(centimm, controls_pos_z_buf_, sizeof(controls_pos_z_buf_));
        lv_subject_copy_string(&controls_pos_z_subject_, controls_pos_z_buf_);
    }

    // Re-read Z-offset subjects
    if (auto* subj = printer_state_.motion_state().get_pending_z_offset_delta_subject()) {
        update_z_offset_delta_display(lv_subject_get_int(subj));
    }
    update_controls_z_offset_display();

    spdlog::trace("[{}] All displays refreshed after activation", get_name());
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void ControlsPanel::register_observers() {
    // Subscribe to temperature updates using bundle (replaces 4 individual observers)
    // Always cache the raw value; skip expensive formatting when panel is hidden
    temp_observers_.setup_sync(
        this, printer_state_,
        [](ControlsPanel* self, int value) {
            self->cached_extruder_temp_ = value;
            if (self->active_)
                self->update_nozzle_temp_display();
        },
        [](ControlsPanel* self, int value) {
            self->cached_extruder_target_ = value;
            if (self->active_)
                self->update_nozzle_temp_display();
        },
        [](ControlsPanel* self, int value) {
            self->cached_bed_temp_ = value;
            if (self->active_)
                self->update_bed_temp_display();
        },
        [](ControlsPanel* self, int value) {
            self->cached_bed_target_ = value;
            if (self->active_)
                self->update_bed_temp_display();
        });

    // Subscribe to chamber temperature (current, raw heater target, and effective target).
    // Note: We check are_subjects_initialized() because observers may fire immediately
    // upon registration, but subjects aren't initialized until init_subjects() is called.
    chamber_temp_observer_ = observe<int>(
        printer_state_.temperature_state().get_chamber_temp_subject(chamber_temp_lifetime_), this,
        [](ControlsPanel* self, int value) {
            self->cached_chamber_temp_ = value;
            if (self->are_subjects_initialized() && self->active_)
                self->update_chamber_temp_display();
        },
        chamber_temp_lifetime_);
    // Raw heater target is kept for keypad seed only (shows the currently entered
    // heater setpoint when the user opens the keypad to edit the chamber target).
    chamber_target_observer_ = observe<int>(
        printer_state_.temperature_state().get_chamber_target_subject(chamber_target_lifetime_),
        this,
        [](ControlsPanel* self, int value) {
            self->cached_chamber_target_ = value;
            // Status display uses cached_chamber_effective_target_, not this value.
        },
        chamber_target_lifetime_);
    // Effective target is the canonical display value: heater target when heating,
    // cooling-fan ceiling when maintaining, 0 when off — drives the status string.
    chamber_effective_target_observer_ = observe<int>(
        printer_state_.temperature_state().get_chamber_effective_target_subject(
            chamber_effective_target_lifetime_),
        this,
        [](ControlsPanel* self, int value) {
            self->cached_chamber_effective_target_ = value;
            if (self->are_subjects_initialized() && self->active_)
                self->update_chamber_temp_display();
        },
        chamber_effective_target_lifetime_);
    // M141 control mode (Off/Heating/Maintaining) — needed so the status string
    // leads with the correct mode word rather than the raw thermal state.
    chamber_mode_observer_ = observe<int>(
        printer_state_.temperature_state().get_chamber_mode_subject(chamber_mode_lifetime_), this,
        [](ControlsPanel* self, int value) {
            self->cached_chamber_mode_ = value;
            if (self->are_subjects_initialized() && self->active_)
                self->update_chamber_temp_display();
        },
        chamber_mode_lifetime_);

    // Subscribe to fan updates (skip formatting when hidden)
    fan_observer_ = observe<int>(
        printer_state_.fan_state().get_fan_speed_subject(), this,
        [](ControlsPanel* self, int /* value */) {
            if (self->active_)
                self->update_fan_display();
        },
        printer_state_.get_subjects_lifetime());

    // Subscribe to multi-fan list changes (fires when fans are discovered/updated)
    // Skip widget rebuilds when hidden; on_activate() calls populate_secondary_fans()
    fans_version_observer_ = observe<int>(
        printer_state_.fan_state().get_fans_version_subject(), this,
        [](ControlsPanel* self, int /* version */) {
            if (!self->active_)
                return;
            // Defer rebuild (#80) AND use safe_clean_children (#776): object_lifetime_.defer
            // moves the rebuild off the observer callback's stack, and
            // safe_clean_children escapes UpdateQueue::process_pending() so sync
            // lv_obj_clean() can't corrupt LVGL's event linked list.
            if (!self->fans_rebuild_pending_) {
                self->fans_rebuild_pending_ = true;
                self->object_lifetime_.defer("ControlsPanel::populate_secondary_fans", [self]() {
                    self->fans_rebuild_pending_ = false;
                    if (self->active_ && self->secondary_fans_list_)
                        self->populate_secondary_fans();
                });
            }
        },
        printer_state_.get_subjects_lifetime());

    // Subscribe to active tool changes for dynamic nozzle label
    active_tool_observer_ = observe<int>(
        helix::ToolState::instance().get_active_tool_subject(), this,
        [](ControlsPanel* self, int /* tool_idx */) {
            if (self->active_)
                self->update_nozzle_label();
        },
        helix::ToolState::instance().get_subjects_lifetime());
    update_nozzle_label(); // Set initial value

    // Sensor discovery can land off the main thread; the link's subjects are touched there.
    temp_sensor_count_observer_ = observe<int>(
        helix::sensors::TemperatureSensorManager::instance().get_sensor_count_subject(), this,
        [](ControlsPanel* self, int /* count */) {
            self->object_lifetime_.defer("ControlsPanel::update_more_sensors",
                                         [self]() { self->update_more_sensors(); });
        },
        helix::sensors::TemperatureSensorManager::instance().get_subjects_lifetime());

    // Subscribe to pending Z-offset delta (for unsaved adjustment banner)
    pending_z_offset_observer_ = observe<int>(
        printer_state_.motion_state().get_pending_z_offset_delta_subject(), this,
        [](ControlsPanel* self, int delta_microns) {
            if (self->active_)
                self->update_z_offset_delta_display(delta_microns);
        },
        printer_state_.get_subjects_lifetime());

    // Subscribe to gcode position updates for Position card using bundle (commanded position in
    // centimillimeters). Skip formatting when hidden — positions update very frequently.
    pos_observers_.setup_sync(
        this, printer_state_,
        [](ControlsPanel* self, int centimm) {
            if (!self->active_)
                return;
            format_position(centimm, self->controls_pos_x_buf_, sizeof(self->controls_pos_x_buf_));
            lv_subject_copy_string(&self->controls_pos_x_subject_, self->controls_pos_x_buf_);
        },
        [](ControlsPanel* self, int centimm) {
            if (!self->active_)
                return;
            format_position(centimm, self->controls_pos_y_buf_, sizeof(self->controls_pos_y_buf_));
            lv_subject_copy_string(&self->controls_pos_y_subject_, self->controls_pos_y_buf_);
        },
        [](ControlsPanel* self, int centimm) {
            if (!self->active_)
                return;
            format_position(centimm, self->controls_pos_z_buf_, sizeof(self->controls_pos_z_buf_));
            lv_subject_copy_string(&self->controls_pos_z_subject_, self->controls_pos_z_buf_);
        });

    // Subscribe to speed/flow factor updates (skip formatting when hidden)
    speed_factor_observer_ = observe<int>(
        printer_state_.motion_state().get_speed_factor_subject(), this,
        [](ControlsPanel* self, int /* value */) {
            if (self->active_)
                self->update_speed_display();
        },
        printer_state_.get_subjects_lifetime());

    // Subscribe to gcode Z-offset for live tuning display (skip formatting when hidden)
    gcode_z_offset_observer_ = observe<int>(
        printer_state_.motion_state().get_gcode_z_offset_subject(), this,
        [](ControlsPanel* self, int /* offset_microns */) {
            if (self->active_)
                self->update_controls_z_offset_display();
        },
        printer_state_.get_subjects_lifetime());

    // The displayed Z-offset switches source between the live and the
    // firmware-persisted reading, so all three inputs have to retrigger it.
    persisted_z_offset_observer_ = observe<int>(
        printer_state_.motion_state().get_persisted_z_offset_subject(), this,
        [](ControlsPanel* self, int /* offset_microns */) {
            if (self->active_)
                self->update_controls_z_offset_display();
        },
        printer_state_.get_subjects_lifetime());

    persisted_z_offset_valid_observer_ = observe<int>(
        printer_state_.motion_state().get_persisted_z_offset_valid_subject(), this,
        [](ControlsPanel* self, int /* valid */) {
            if (self->active_)
                self->update_controls_z_offset_display();
        },
        printer_state_.get_subjects_lifetime());

    z_offset_print_active_observer_ = observe<int>(
        printer_state_.print_state().get_print_active_subject(), this,
        [](ControlsPanel* self, int /* print_active */) {
            if (self->active_)
                self->update_controls_z_offset_display();
        },
        printer_state_.get_subjects_lifetime());

    spdlog::trace("[{}] Observers registered for dashboard live data", get_name());
}

// ============================================================================
// DISPLAY UPDATE HELPERS
// ============================================================================

void ControlsPanel::update_nozzle_label() {
    auto label = helix::ToolState::instance().nozzle_label();
    std::snprintf(nozzle_label_buf_, sizeof(nozzle_label_buf_), "%s", label.c_str());
    if (subjects_initialized_) {
        lv_subject_copy_string(&nozzle_label_subject_, nozzle_label_buf_);
    }
}

void ControlsPanel::update_more_sensors() {
    if (!subjects_initialized_) {
        return;
    }
    // The chamber sensor has its own row on the card, so it is not counted.
    int count = 0;
    for (const auto& sensor :
         helix::sensors::TemperatureSensorManager::instance().get_sensors_sorted()) {
        if (sensor.enabled && sensor.role != helix::sensors::TemperatureSensorRole::CHAMBER) {
            ++count;
        }
    }
    std::snprintf(more_sensors_buf_, sizeof(more_sensors_buf_), lv_tr("%d more sensors"), count);
    lv_subject_copy_string(&more_sensors_subject_, more_sensors_buf_);
    lv_subject_set_int(&more_sensors_count_, count);
}

void ControlsPanel::update_nozzle_temp_display() {
    auto result =
        helix::ui::temperature::heater_display(cached_extruder_temp_, cached_extruder_target_);

    std::snprintf(nozzle_temp_buf_, sizeof(nozzle_temp_buf_), "%s", result.temp.c_str());
    lv_subject_copy_string(&nozzle_temp_subject_, nozzle_temp_buf_);

    lv_subject_set_int(&nozzle_pct_subject_, result.pct);

    auto nozzle = helix::ui::temperature::classify_heater_status(
        cached_extruder_temp_, cached_extruder_target_,
        lv_subject_get_int(printer_state_.get_heater_power_subject(helix::HeaterType::Nozzle)));
    lv_subject_set_int(&nozzle_status_state_subject_, static_cast<int>(nozzle.state));
    std::snprintf(nozzle_status_buf_, sizeof(nozzle_status_buf_), "%s", nozzle.duty.c_str());
    lv_subject_copy_string(&nozzle_status_subject_, nozzle_status_buf_);
}

void ControlsPanel::update_bed_temp_display() {
    auto result = helix::ui::temperature::heater_display(cached_bed_temp_, cached_bed_target_);

    std::snprintf(bed_temp_buf_, sizeof(bed_temp_buf_), "%s", result.temp.c_str());
    lv_subject_copy_string(&bed_temp_subject_, bed_temp_buf_);

    lv_subject_set_int(&bed_pct_subject_, result.pct);

    auto bed = helix::ui::temperature::classify_heater_status(
        cached_bed_temp_, cached_bed_target_,
        lv_subject_get_int(printer_state_.get_heater_power_subject(helix::HeaterType::Bed)));
    lv_subject_set_int(&bed_status_state_subject_, static_cast<int>(bed.state));
    std::snprintf(bed_status_buf_, sizeof(bed_status_buf_), "%s", bed.duty.c_str());
    lv_subject_copy_string(&bed_status_subject_, bed_status_buf_);
}

void ControlsPanel::update_chamber_temp_display() {
    // Delegate to the shared classifier so this panel and the temp-graph overlay
    // always produce identical output (single source of truth). Maintaining
    // mode treats the target as a cooling ceiling, so the classifier needs the
    // mode, not just the numbers.
    auto chamber = helix::ui::temperature::classify_heater_status(
        cached_chamber_temp_, cached_chamber_effective_target_,
        lv_subject_get_int(printer_state_.get_heater_power_subject(helix::HeaterType::Chamber)),
        static_cast<helix::ChamberMode>(cached_chamber_mode_));
    lv_subject_set_int(&chamber_status_state_subject_, static_cast<int>(chamber.state));
    std::snprintf(chamber_status_buf_, sizeof(chamber_status_buf_), "%s", chamber.duty.c_str());
    lv_subject_copy_string(&chamber_status_subject_, chamber_status_buf_);
}

void ControlsPanel::update_fan_display() {
    // Suppress Moonraker-driven updates while the user is actively dragging the slider
    // or within a short window after release, to prevent jumpy snap-back from stale values
    constexpr uint32_t suppression_ms = 1500;
    if (last_fan_slider_input_ > 0 && (lv_tick_get() - last_fan_slider_input_) < suppression_ms) {
        spdlog::trace("[{}] Suppressed fan display update - within {}ms of last slider input",
                      get_name(), suppression_ms);
        return;
    }

    int fan_pct = printer_state_.fan_state().get_fan_speed_subject()
                      ? lv_subject_get_int(printer_state_.fan_state().get_fan_speed_subject())
                      : 0;

    format_fan_speed(fan_pct, fan_speed_buf_, sizeof(fan_speed_buf_));
    lv_subject_copy_string(&fan_speed_subject_, fan_speed_buf_);
    lv_subject_set_int(&fan_pct_subject_, fan_pct);
}

/// @brief Priority score for fan display ordering on the cooling card.
/// Lower score = higher priority (shown first).
static int fan_display_priority(const helix::FanInfo& fan) {
    // Chamber-role fans are most interesting to users (enclosure management), by the
    // same rule the hardware role registry applies. Matches the Moonraker object name,
    // never the localized display name.
    if (helix::role_descriptor(helix::HardwareRoleId::ChamberFan)->is_candidate(fan.object_name)) {
        return 0;
    }
    // Controllable generic fans next (user can interact)
    if (fan.is_controllable) {
        return 1;
    }
    // Heater fans (auto, but important to see status)
    if (fan.type == helix::FanType::HEATER_FAN) {
        return 2;
    }
    // Controller fans last (board cooling, least interesting)
    return 3;
}

void ControlsPanel::populate_secondary_fans() {
    if (!secondary_fans_list_) {
        return;
    }

    // Bump generation counter FIRST — any in-flight deferred callbacks from previous
    // observers will see a stale generation and skip their update. This prevents
    // use-after-free when observe<int> callbacks fire after widget deletion.
    ++fan_populate_gen_;

    // Cleanup order: lifetimes → observers → tracking → hide → delete widgets.
    // Reset the dynamic-subject lifetime tokens BEFORE the observers so each guard's
    // weak_ptr is already expired when reset() runs — otherwise reset() calls
    // lv_observer_remove() on a subject that may have been freed by fan rediscovery.
    secondary_fan_lifetimes_.clear();
    for (auto& obs : secondary_fan_observers_) {
        obs.reset();
    }
    secondary_fan_observers_.clear();
    secondary_fan_rows_.clear();
    lv_obj_add_flag(secondary_fans_list_, LV_OBJ_FLAG_HIDDEN);
    helix::ui::safe_clean_children(secondary_fans_list_);

    // Collect non-part-cooling fans and sort by display priority
    const auto& fans = printer_state_.fan_state().get_fans();
    std::vector<const helix::FanInfo*> secondary_fans;
    for (const auto& fan : fans) {
        if (fan.type != helix::FanType::PART_COOLING) {
            secondary_fans.push_back(&fan);
        }
    }
    std::sort(secondary_fans.begin(), secondary_fans.end(),
              [](const helix::FanInfo* a, const helix::FanInfo* b) {
                  return fan_display_priority(*a) < fan_display_priority(*b);
              });

    constexpr int max_visible = 2;
    int visible_count = 0;

    for (const auto* fan : secondary_fans) {
        if (visible_count >= max_visible) {
            break;
        }

        char speed_buf[16];
        format_fan_speed(fan->speed_percent, speed_buf, sizeof(speed_buf));
        // A controllable fan shows a chevron, an automatic one an "A" badge.
        const char* attrs[] = {
            "fan_name",       fan->display_name.c_str(),
            "fan_speed",      speed_buf,
            "indicator_icon", fan->is_controllable ? "chevron_right" : "alpha_a_circle",
            nullptr};
        auto* row =
            static_cast<lv_obj_t*>(lv_xml_create(secondary_fans_list_, "controls_fan_row", attrs));
        if (!row) {
            spdlog::warn("[{}] Failed to create row for fan '{}'", get_name(), fan->object_name);
            continue;
        }

        // Track this row for reactive speed updates
        secondary_fan_rows_.push_back(
            {fan->object_name, helix::ui::find_required(row, "fan_speed_label", get_name())});
        visible_count++;
    }

    // Show "N additional fans >" row if there are more fans than visible
    int additional = static_cast<int>(secondary_fans.size()) - visible_count;
    if (additional > 0) {
        char more_buf[32];
        std::snprintf(more_buf, sizeof(more_buf), lv_tr("%d additional fan%s"), additional,
                      additional == 1 ? "" : "s");
        const char* attrs[] = {"caption", more_buf, nullptr};
        lv_xml_create(secondary_fans_list_, "controls_fan_more_row", attrs);
    }

    // Subscribe to per-fan speed subjects for reactive updates
    subscribe_to_secondary_fan_speeds();

    // Unhide container now that repopulation is complete
    lv_obj_remove_flag(secondary_fans_list_, LV_OBJ_FLAG_HIDDEN);

    spdlog::trace("[{}] Populated {} secondary fans ({} visible, {} additional)", get_name(),
                  secondary_fans.size(), visible_count, additional);
}

void ControlsPanel::update_z_offset_delta_display(int delta_microns) {
    helix::zoffset::format_delta(delta_microns, z_offset_delta_display_buf_,
                                 sizeof(z_offset_delta_display_buf_));
    lv_subject_copy_string(&z_offset_delta_display_subject_, z_offset_delta_display_buf_);
    spdlog::trace("[{}] Z-offset delta display updated: '{}'", get_name(),
                  z_offset_delta_display_buf_);
}

void ControlsPanel::update_controls_z_offset_display() {
    // ZMOD's END_PRINT/CANCEL_PRINT zero gcode_move's offset and START_PRINT
    // re-applies the stored one, so while idle the live reading is 0.000 and the
    // persisted value is what the next print will actually use.
    const int offset_microns = helix::zoffset::displayed_z_offset_microns(printer_state_);

    auto* bp_subj = theme_manager_get_breakpoint_subject();
    auto bp = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    if (bp == UiBreakpoint::Tiny || bp == UiBreakpoint::Micro) {
        helix::zoffset::format_offset_compact(offset_microns, controls_z_offset_buf_,
                                              sizeof(controls_z_offset_buf_));
    } else {
        helix::zoffset::format_offset(offset_microns, controls_z_offset_buf_,
                                      sizeof(controls_z_offset_buf_));
    }
    lv_subject_copy_string(&controls_z_offset_subject_, controls_z_offset_buf_);
}

void ControlsPanel::handle_zoffset_tune() {
    spdlog::debug("[{}] Z-offset tune clicked - opening Print Tune overlay", get_name());

    // Use singleton - handles lazy init, subject registration, and nav push
    get_print_tune_overlay().show(parent_screen_, api_, printer_state_);
}

void ControlsPanel::handle_save_z_offset() {
    helix::zoffset::save_dirty_offsets_shared();
}

// ============================================================================
// V2 CARD CLICK HANDLERS
// ============================================================================

void ControlsPanel::handle_quick_actions_clicked() {
    get_global_motion_panel().show(parent_screen_);
}

void ControlsPanel::handle_nozzle_temp_clicked() {
    spdlog::debug("[{}] Nozzle temp clicked - opening temperature graph", get_name());
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, parent_screen_);
}

void ControlsPanel::handle_bed_temp_clicked() {
    spdlog::debug("[{}] Bed temp clicked - opening temperature graph", get_name());
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, parent_screen_);
}

int ControlsPanel::target_edit_max(helix::HeaterType type, int fallback_deg) const {
    return static_cast<int>(
        helix::keypad_ceiling(controller(), type, static_cast<float>(fallback_deg)));
}

void ControlsPanel::handle_nozzle_target_edit() {
    show_temperature_keypad<&ControlsPanel::handle_custom_nozzle_confirmed>(
        "Nozzle Temperature", cached_extruder_target_, 200,
        target_edit_max(helix::HeaterType::Nozzle, nozzle_max_temp_));
}

void ControlsPanel::handle_bed_target_edit() {
    show_temperature_keypad<&ControlsPanel::handle_custom_bed_confirmed>(
        "Bed Temperature", cached_bed_target_, 60,
        target_edit_max(helix::HeaterType::Bed, bed_max_temp_));
}

void ControlsPanel::handle_chamber_target_edit() {
    // Seed from the effective target (heater target when Heating, fan target when
    // Maintaining) so the keypad pre-fills the value the card already shows. The
    // raw heater target reads 0 during M141 maintain mode and would otherwise seed 0.
    show_temperature_keypad<&ControlsPanel::handle_custom_chamber_confirmed>(
        "Chamber Temperature", cached_chamber_effective_target_, 50,
        target_edit_max(helix::HeaterType::Chamber, chamber_max_temp_));
}

void ControlsPanel::handle_custom_nozzle_confirmed(float value) {
    spdlog::info("[{}] Custom nozzle temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    // Convert degrees to decidegrees for storage (matches PrinterState internal format)
    cached_extruder_target_ = helix::units::to_decidegrees(value);

    if (auto* c = controller()) {
        c->set_target(helix::HeaterType::Nozzle, value,
                      {.toast = true, .on_success = [target = static_cast<int>(value)]() {
                           NOTIFY_SUCCESS(lv_tr("Nozzle target set to {}°C"), target);
                       }});
    }
}

void ControlsPanel::handle_custom_bed_confirmed(float value) {
    spdlog::info("[{}] Custom bed temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    // Convert degrees to decidegrees for storage (matches PrinterState internal format)
    cached_bed_target_ = helix::units::to_decidegrees(value);

    if (auto* c = controller()) {
        c->set_target(helix::HeaterType::Bed, value,
                      {.toast = true, .on_success = [target = static_cast<int>(value)]() {
                           NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), target);
                       }});
    }
}

void ControlsPanel::handle_custom_chamber_confirmed(float value) {
    spdlog::info("[{}] Custom chamber temperature confirmed: {}°C", get_name(),
                 static_cast<int>(value));

    cached_chamber_target_ = helix::units::to_decidegrees(value);

    if (auto* c = controller()) {
        c->set_target(helix::HeaterType::Chamber, value,
                      {.toast = true, .on_success = [target = static_cast<int>(value)]() {
                           NOTIFY_SUCCESS(lv_tr("Chamber target set to {}°C"), target);
                       }});
    }
}

void ControlsPanel::handle_chamber_temp_clicked() {
    spdlog::debug("[{}] Chamber temp clicked - opening temperature graph", get_name());
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Chamber, parent_screen_);
}

void ControlsPanel::handle_cooling_clicked() {
    // Redirect to FanControlOverlay which handles all fans (part cooling + secondary)
    spdlog::debug("[{}] Cooling card clicked - opening Fan Control overlay", get_name());
    handle_secondary_fans_clicked();
}

void ControlsPanel::handle_secondary_fans_clicked() {
    spdlog::debug("[{}] Secondary fans clicked - opening Fan Control overlay", get_name());

    if (!helix::open_fan_control_overlay(parent_screen_)) {
        NOTIFY_ERROR(lv_tr("Failed to load fan control overlay"));
    }
}

// ============================================================================
// QUICK ACTION BUTTON HANDLERS
// ============================================================================

void ControlsPanel::run_quick_action(uint32_t timeout_ms, const QuickActionText& text,
                                     const QuickActionDispatch& dispatch) {
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }
    if (!api_) {
        NOTIFY_WARNING(lv_tr("Printer connection unavailable"));
        return;
    }

    operation_guard_.begin(
        timeout_ms, [msg = text.guard_timed_out] { NOTIFY_WARNING(fmt::runtime(msg.c_str())); });
    NOTIFY_INFO(fmt::runtime(text.started.c_str()));

    // Moonraker replies land on a network thread. bg_cb defers the whole body to
    // the main thread atomically, which a bare expired() check followed by an
    // inline mutation does not (L081 Mechanism C).
    dispatch(object_lifetime_.bg_cb("ControlsPanel::quick_action_ok",
                                    [this, msg = text.completed]() {
                                        operation_guard_.end();
                                        NOTIFY_SUCCESS(fmt::runtime(msg.c_str()));
                                    }),
             object_lifetime_.bg_cb(
                 "ControlsPanel::quick_action_error", [this, text](const MoonrakerError& err) {
                     operation_guard_.end();
                     if (err.type == MoonrakerErrorType::TIMEOUT) {
                         NOTIFY_WARNING(fmt::runtime(text.rpc_timed_out.c_str()));
                     } else {
                         NOTIFY_ERROR(fmt::runtime(text.failed_fmt.c_str()),
                                      err.localized_message());
                     }
                 }));
}

// The five homing buttons share one set of strings for everything except the
// "Homing X..." line: an axis that failed, timed out or finished did so as part
// of the same homing move, and naming the axis twice buys the user nothing.
ControlsPanel::QuickActionText ControlsPanel::homing_text(const char* started) {
    return {started, lv_tr("Homing complete"), lv_tr("Homing timed out"),
            lv_tr("Homing may still be running — response timed out"), lv_tr("Homing failed: {}")};
}

void ControlsPanel::home_axes_action(const char* axes, const char* started_toast) {
    spdlog::debug("[{}] Home {} clicked", get_name(), axes[0] ? axes : "All");
    run_quick_action(
        IMoonrakerAPI::HOMING_TIMEOUT_MS, homing_text(started_toast),
        [this, axes](IMoonrakerAPI::SuccessCallback ok, IMoonrakerAPI::ErrorCallback err) {
            api_->motion().home_axes(axes, std::move(ok), std::move(err));
        });
}

void ControlsPanel::handle_home_all() {
    home_axes_action("", lv_tr("Homing all axes..."));
}

void ControlsPanel::handle_home_x() {
    home_axes_action("X", lv_tr("Homing X..."));
}

void ControlsPanel::handle_home_y() {
    home_axes_action("Y", lv_tr("Homing Y..."));
}

void ControlsPanel::handle_home_xy() {
    home_axes_action("XY", lv_tr("Homing XY..."));
}

void ControlsPanel::handle_home_z() {
    home_axes_action("Z", lv_tr("Homing Z..."));
}

void ControlsPanel::handle_qgl() {
    spdlog::debug("[{}] QGL clicked", get_name());
    run_quick_action(IAdvancedAPI::LEVELING_TIMEOUT_MS,
                     {lv_tr("Quad Gantry Level started..."), lv_tr("Quad Gantry Level complete"),
                      lv_tr("QGL timed out"),
                      lv_tr("QGL may still be running — response timed out"),
                      lv_tr("QGL failed: {}")},
                     [this](IMoonrakerAPI::SuccessCallback ok, IMoonrakerAPI::ErrorCallback err) {
                         api_->execute_gcode("QUAD_GANTRY_LEVEL", std::move(ok), std::move(err),
                                             IAdvancedAPI::LEVELING_TIMEOUT_MS);
                     });
}

void ControlsPanel::handle_z_tilt() {
    spdlog::debug("[{}] Z-Tilt clicked", get_name());
    run_quick_action(IAdvancedAPI::LEVELING_TIMEOUT_MS,
                     {lv_tr("Z-Tilt Adjust started..."), lv_tr("Z-Tilt Adjust complete"),
                      lv_tr("Z-Tilt timed out"),
                      lv_tr("Z-Tilt may still be running — response timed out"),
                      lv_tr("Z-Tilt failed: {}")},
                     [this](IMoonrakerAPI::SuccessCallback ok, IMoonrakerAPI::ErrorCallback err) {
                         api_->execute_gcode("Z_TILT_ADJUST", std::move(ok), std::move(err),
                                             IAdvancedAPI::LEVELING_TIMEOUT_MS);
                     });
}

// ============================================================================
// SPEED/FLOW OVERRIDE HANDLERS
// ============================================================================

void ControlsPanel::update_speed_display() {
    int speed_pct = 100;
    if (auto* speed_subj = printer_state_.motion_state().get_speed_factor_subject()) {
        speed_pct = lv_subject_get_int(speed_subj);
    }
    helix::format::format_percent(speed_pct, speed_override_buf_, sizeof(speed_override_buf_));
    lv_subject_copy_string(&speed_override_subject_, speed_override_buf_);
}

// ============================================================================
// FAN SLIDER HANDLER
// ============================================================================

void ControlsPanel::handle_fan_slider_changed(int value) {
    // Defensive validation - slider should already be 0-100 but clamp anyway
    value = std::clamp(value, 0, 100);
    last_fan_slider_input_ = lv_tick_get();
    spdlog::debug("[{}] Fan slider changed to {}%", get_name(), value);

    // Optimistic update - show new value immediately without waiting for Moonraker
    format_fan_speed(value, fan_speed_buf_, sizeof(fan_speed_buf_));
    lv_subject_copy_string(&fan_speed_subject_, fan_speed_buf_);
    lv_subject_set_int(&fan_pct_subject_, value);

    if (api_) {
        api_->set_fan_speed(
            "fan", static_cast<double>(value), []() { /* Silent success */ },
            [](const MoonrakerError& err) {
                helix::ui::notify_error_tr(TR_NOOP("Fan control failed: {}"), err);
            });
    }
}

// ============================================================================
// CALIBRATION HANDLERS
// ============================================================================

void ControlsPanel::handle_motors_clicked() {
    spdlog::debug("[{}] Motors Disable card clicked - showing confirmation", get_name());
    helix::ui::show_motors_off_confirm(api_, motors_confirmation_dialog_, object_lifetime_.token());
}

void ControlsPanel::handle_calibration_bed_mesh() {
#if defined(HELIX_PLATFORM_ESP32)
    // Bed-mesh calibration is excluded from the v1 Core+AMS cut; its panel is a
    // null-vtable link stub. Toast instead of the LoadProhibited crash.
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    get_global_bed_mesh_panel().show(parent_screen_);
}

void ControlsPanel::handle_calibration_tool_offsets() {
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    helix::ui::get_global_tool_offset_cal_panel().show(parent_screen_);
}

void ControlsPanel::handle_calibration_pa() {
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    helix::ui::get_global_pa_cal_panel().set_api(get_moonraker_api());
    helix::ui::get_global_pa_cal_panel().show(parent_screen_);
}

void ControlsPanel::handle_calibration_zoffset() {
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    // Set the Moonraker client before lazy creation so it's available when calibration starts
    get_global_zoffset_cal_panel().set_api(get_moonraker_api());
    get_global_zoffset_cal_panel().show(parent_screen_);
}

void ControlsPanel::handle_calibration_screws() {
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    get_global_screws_tilt_panel().set_client(get_moonraker_client(), get_moonraker_api());
    get_global_screws_tilt_panel().show(parent_screen_);
}

void ControlsPanel::handle_calibration_motors() {
    spdlog::debug("[{}] Disable Motors button clicked", get_name());
    handle_motors_clicked();
}

// ============================================================================
// XML CALLBACKS
// ============================================================================

// Unified macro callback - extracts index from user_data
void ControlsPanel::on_macro(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ControlsPanel] on_macro");
    const char* index_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (index_str) {
        size_t index = strtoul(index_str, nullptr, 10);
        auto& panel = get_global_controls_panel();
        panel.quick_actions_.execute(index, panel.api_, panel.object_lifetime_.token());
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ControlsPanel::subscribe_to_secondary_fan_speeds() {
    using helix::ui::observe;
    secondary_fan_observers_.reserve(secondary_fan_rows_.size());
    secondary_fan_lifetimes_.reserve(secondary_fan_rows_.size());

    const uint32_t gen = fan_populate_gen_;
    for (const auto& row : secondary_fan_rows_) {
        // Per-fan speed subjects are dynamic (freed + recreated on fan rediscovery).
        // The lifetime token MUST outlive the paired observer, so it lives in a member
        // vector alongside secondary_fan_observers_ — never a stack local (that would
        // expire the guard's weak_ptr immediately, leaving a dangling observer that
        // corrupts the subject's observer list when reset() later removes it).
        SubjectLifetime& lifetime = secondary_fan_lifetimes_.emplace_back();
        if (auto* subject =
                printer_state_.fan_state().get_fan_speed_subject(row.object_name, lifetime)) {
            secondary_fan_observers_.push_back(observe<int>(
                subject, this,
                [name = row.object_name, gen](ControlsPanel* self, int speed_pct) {
                    if (gen != self->fan_populate_gen_)
                        return; // stale callback — widgets gone
                    if (!self->active_)
                        return; // skip label update when hidden
                    self->update_secondary_fan_speed(name, speed_pct);
                },
                lifetime));
            spdlog::trace("[{}] Subscribed to speed subject for secondary fan '{}'", get_name(),
                          row.object_name);
        } else {
            // No subject for this fan — drop the just-added lifetime to keep the
            // lifetimes/observers vectors aligned.
            secondary_fan_lifetimes_.pop_back();
        }
    }

    spdlog::trace("[{}] Subscribed to {} secondary fan speed subjects", get_name(),
                  secondary_fan_observers_.size());
}

void ControlsPanel::update_secondary_fan_speed(const std::string& object_name, int speed_pct) {
    for (const auto& row : secondary_fan_rows_) {
        if (row.object_name == object_name && row.speed_label) {
            if (!lv_obj_is_valid(row.speed_label)) {
                spdlog::debug("[{}] Stale speed_label for fan '{}', skipping update", get_name(),
                              object_name);
                break;
            }
            char speed_buf[16];
            format_fan_speed(speed_pct, speed_buf, sizeof(speed_buf));
            lv_label_set_text(row.speed_label, speed_buf);
            spdlog::trace("[{}] Updated secondary fan '{}' speed to {}", get_name(), object_name,
                          speed_buf);
            break;
        }
    }
}

// ============================================================================
// MORE-SENSORS LINK
// ============================================================================

void ControlsPanel::handle_secondary_temps_clicked() {
    spdlog::debug("[{}] Secondary temps overflow clicked - opening sensors overlay", get_name());
    auto& overlay = helix::settings::get_sensor_settings_overlay();
    overlay.show(parent_screen_);
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

ControlsPanel& get_global_controls_panel() {
    return helix::lazy_global<ControlsPanel>("ControlsPanel", get_printer_state(), nullptr);
}
