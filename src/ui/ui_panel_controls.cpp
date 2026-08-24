// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_controls.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_notification.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_bed_mesh.h"
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
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "operation_timeout_guard.h"
#include "panel_widgets/led_widget.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"
#include "temperature_service.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_cleanup_helpers.h"
#include "ui/ui_event_trampoline.h"
#include "ui/ui_lazy_panel_helper.h"
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
using helix::ui::observe_int_sync;
using helix::ui::observe_string;
using helix::ui::temperature::deci_to_degrees;

// Forward declarations for class-based API
class MotionPanel;
MotionPanel& get_global_motion_panel();

using helix::ui::position::format_position;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

ControlsPanel::ControlsPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // Dependencies passed for interface consistency
    // Child panels (motion, temp, extrusion) may use these when wired
}

ControlsPanel::~ControlsPanel() {
    // Detach the LED widget while LVGL is still valid (its dtor calls detach(),
    // but do it explicitly first so its observers are torn down before subjects).
    led_widget_.reset();

    deinit_subjects();

    // Clean up lazily-created overlay panels to prevent dangling LVGL objects
    // Note: safe_delete_obj handles shutdown guards (lv_is_initialized, is_destroying_all, etc.)
    using helix::ui::safe_delete_obj;
    safe_delete_obj(motion_panel_);
    safe_delete_obj(fan_control_panel_);
    safe_delete_obj(bed_mesh_panel_);
    safe_delete_obj(zoffset_panel_);
    safe_delete_obj(screws_panel_);
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
    UI_MANAGED_SUBJECT_STRING(nozzle_status_subject_, nozzle_status_buf_, lv_tr("Off"),
                              "controls_nozzle_status", subjects_);

    // Bed temperature display
    UI_MANAGED_SUBJECT_STRING(bed_temp_subject_, bed_temp_buf_, "—°C", "controls_bed_temp",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(bed_pct_subject_, 0, "controls_bed_pct", subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_status_subject_, bed_status_buf_, lv_tr("Off"),
                              "controls_bed_status", subjects_);

    // Chamber temperature display
    UI_MANAGED_SUBJECT_STRING(chamber_status_subject_, chamber_status_buf_, lv_tr("Off"),
                              "controls_chamber_status", subjects_);

    // Fan speed display
    UI_MANAGED_SUBJECT_STRING(fan_speed_subject_, fan_speed_buf_, lv_tr("Off"),
                              "controls_fan_speed", subjects_);
    UI_MANAGED_SUBJECT_INT(fan_pct_subject_, 0, "controls_fan_pct", subjects_);

    // Macro button visibility and names (for declarative binding)
    UI_MANAGED_SUBJECT_INT(macro_1_visible_, 0, "macro_1_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_2_visible_, 0, "macro_2_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_1_available_, 0, "macro_1_available", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_2_available_, 0, "macro_2_available", subjects_);
    UI_MANAGED_SUBJECT_STRING(macro_1_name_, macro_1_name_buf_, "", "macro_1_name", subjects_);
    UI_MANAGED_SUBJECT_STRING(macro_2_name_, macro_2_name_buf_, "", "macro_2_name", subjects_);

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

    // Speed/Flow override display subjects
    std::strcpy(speed_override_buf_, "100%");
    std::strcpy(flow_override_buf_, "100%");
    UI_MANAGED_SUBJECT_STRING(speed_override_subject_, speed_override_buf_, "100%",
                              "controls_speed_pct", subjects_);
    UI_MANAGED_SUBJECT_STRING(flow_override_subject_, flow_override_buf_, "100%",
                              "controls_flow_pct", subjects_);

    // Macro buttons 3 & 4 visibility and names
    UI_MANAGED_SUBJECT_INT(macro_3_visible_, 0, "macro_3_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_4_visible_, 0, "macro_4_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_3_available_, 0, "macro_3_available", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_4_available_, 0, "macro_4_available", subjects_);
    UI_MANAGED_SUBJECT_STRING(macro_3_name_, macro_3_name_buf_, "", "macro_3_name", subjects_);
    UI_MANAGED_SUBJECT_STRING(macro_4_name_, macro_4_name_buf_, "", "macro_4_name", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_header_visible_, 1, "macro_header_visible", subjects_);

    // Operation timeout guard (disables buttons while homing/QGL/Z-tilt in progress)
    operation_guard_.init_subject("controls_operation_in_progress", subjects_);

    // Z-offset display subject for live tuning
    std::strcpy(controls_z_offset_buf_, "+0.000mm");
    UI_MANAGED_SUBJECT_STRING(controls_z_offset_subject_, controls_z_offset_buf_, "+0.000mm",
                              "controls_z_offset", subjects_);

    // Observe homed_axes from PrinterState to update homing subjects using string observer
    homed_axes_observer_ = observe_string<ControlsPanel>(
        printer_state_.get_homed_axes_subject(), this,
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

    register_xml_callbacks({
        // Calibration button event callbacks (direct buttons in card, no modal)
        {"on_calibration_bed_mesh", on_calibration_bed_mesh},
        {"on_calibration_zoffset", on_calibration_zoffset},
        {"on_calibration_screws", on_calibration_screws},
        {"on_calibration_motors", on_calibration_motors},

        // Quick Actions: Home buttons
        {"on_controls_home_all", on_home_all},
        {"on_controls_home_x", on_home_x},
        {"on_controls_home_y", on_home_y},
        {"on_controls_home_xy", on_home_xy},
        {"on_controls_home_z", on_home_z},

        // Quick Actions: Leveling buttons (QGL / Z-Tilt)
        {"on_controls_qgl", on_qgl},
        {"on_controls_z_tilt", on_z_tilt},

        // Quick Actions: Macro buttons (unified callback with user_data index)
        {"on_controls_macro", on_macro},

        // Speed/Flow override buttons
        {"on_controls_speed_up", on_speed_up},
        {"on_controls_speed_down", on_speed_down},
        {"on_controls_flow_up", on_flow_up},
        {"on_controls_flow_down", on_flow_down},

        // Cooling: Fan slider
        {"on_controls_fan_slider", on_fan_slider_changed},

        // Z-Offset banner: Save button
        {"on_controls_save_z_offset", on_save_z_offset},

        // Z-Offset clickable row: Opens Print Tune overlay
        {"on_zoffset_tune", on_zoffset_tune},

        // Card click handlers (navigation to full overlay panels)
        {"on_controls_quick_actions", on_quick_actions_clicked},
        {"on_nozzle_temp_clicked", on_nozzle_temp_clicked},
        {"on_bed_temp_clicked", on_bed_temp_clicked},
        {"on_chamber_temp_clicked", on_chamber_temp_clicked},
        {"on_controls_cooling", on_cooling_clicked},
        // Pencil icon edit handlers (open temperature keypad)
        {"on_nozzle_target_edit", on_nozzle_target_edit},
        {"on_bed_target_edit", on_bed_target_edit},
        {"on_chamber_target_edit", on_chamber_target_edit},
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

    // Load quick button slot assignments from config
    // Config stores slot names like "clean_nozzle", "bed_level"
    if (Config* config = Config::get_instance()) {
        std::string slot1_name =
            config->get<std::string>("/standard_macros/quick_button_1", "clean_nozzle");
        std::string slot2_name =
            config->get<std::string>("/standard_macros/quick_button_2", "bed_level");
        std::string slot3_name = config->get<std::string>("/standard_macros/quick_button_3", "");
        std::string slot4_name = config->get<std::string>("/standard_macros/quick_button_4", "");

        macro_1_slot_ = StandardMacros::slot_from_name(slot1_name);
        macro_2_slot_ = StandardMacros::slot_from_name(slot2_name);
        macro_3_slot_ =
            slot3_name.empty() ? std::nullopt : StandardMacros::slot_from_name(slot3_name);
        macro_4_slot_ =
            slot4_name.empty() ? std::nullopt : StandardMacros::slot_from_name(slot4_name);

        spdlog::trace(
            "[{}] Quick buttons configured: slot1='{}', slot2='{}', slot3='{}', slot4='{}'",
            get_name(), slot1_name, slot2_name, slot3_name, slot4_name);
    } else {
        // Fallback: use CleanNozzle and BedLevel slots for 1 & 2, none for 3 & 4
        macro_1_slot_ = StandardMacroSlot::CleanNozzle;
        macro_2_slot_ = StandardMacroSlot::BedLevel;
        macro_3_slot_ = std::nullopt;
        macro_4_slot_ = std::nullopt;
        spdlog::warn("[{}] Config not available, using default macro slots", get_name());
    }

    // Refresh button labels and visibility based on current StandardMacros state
    refresh_macro_buttons();

    // Cache dynamic container for secondary fans
    FIND_WIDGET(secondary_fans_list_, panel_, "secondary_fans_list", get_name());
    if (secondary_fans_list_) {
        // Make the secondary fans list clickable to open the fan control overlay
        lv_obj_add_flag(secondary_fans_list_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(secondary_fans_list_, on_secondary_fans_clicked, LV_EVENT_CLICKED,
                            this);
    }

    // Cache dynamic container for secondary temperature sensors
    FIND_WIDGET(secondary_temps_list_, panel_, "secondary_temps_list", get_name());
    if (secondary_temps_list_) {
        // Make the secondary temps list clickable to open the sensor settings overlay
        lv_obj_add_flag(secondary_temps_list_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(secondary_temps_list_, on_secondary_temps_clicked, LV_EVENT_CLICKED,
                            this);
    }

    // LED quick-toggle cell (Calibration & Tools grid). Reuses LedWidget — the
    // same class that drives the home-dashboard light widget — so the bulb icon
    // reflects on/off + brightness + LED color and a tap toggles the light.
    // The cell itself is hidden unless an LED strip is controllable
    // (led_controllable binding in XML). attach() finds light_button/light_icon
    // by name and wires the click handler + observers.
    if (lv_obj_t* led_cell = lv_obj_find_by_name(panel_, "controls_led_cell")) {
        led_widget_ = std::make_unique<helix::LedWidget>(printer_state_, api_);
        led_widget_->attach(led_cell, parent_screen);
    }

    // Wire up card click handlers (cards need manual wiring for navigation)
    setup_card_handlers();

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

    // Populate secondary temperature sensors on initial setup
    populate_secondary_temps();

    spdlog::debug("[{}] Setup complete", get_name());
}

void ControlsPanel::on_activate() {
    active_ = true;

    // Reset coalescing flags to prevent stale state from a previous deactivation
    fans_rebuild_pending_ = false;
    temps_rebuild_pending_ = false;

    // Force-refresh all displays so UI catches up on state changes missed while hidden
    refresh_all_displays();

    // Refresh secondary fans list when panel becomes visible
    // This handles edge cases where:
    // 1. Fan discovery completed after initial setup
    // 2. User switched from one printer connection to another
    // 3. Observer callback was missed due to timing
    populate_secondary_fans();

    // Re-read quick button slot config — user may have changed settings
    if (Config* config = Config::get_instance()) {
        std::string slot1_name =
            config->get<std::string>("/standard_macros/quick_button_1", "clean_nozzle");
        std::string slot2_name =
            config->get<std::string>("/standard_macros/quick_button_2", "bed_level");
        std::string slot3_name = config->get<std::string>("/standard_macros/quick_button_3", "");
        std::string slot4_name = config->get<std::string>("/standard_macros/quick_button_4", "");

        macro_1_slot_ = StandardMacros::slot_from_name(slot1_name);
        macro_2_slot_ = StandardMacros::slot_from_name(slot2_name);
        macro_3_slot_ =
            slot3_name.empty() ? std::nullopt : StandardMacros::slot_from_name(slot3_name);
        macro_4_slot_ =
            slot4_name.empty() ? std::nullopt : StandardMacros::slot_from_name(slot4_name);
    }

    // Refresh macro buttons — picks up config changes and auto-detected macros
    refresh_macro_buttons();

    spdlog::trace("[{}] Panel activated, refreshed macro buttons", get_name());
}

void ControlsPanel::on_deactivate() {
    active_ = false;
    spdlog::trace("[{}] Panel deactivated, observer callbacks will skip UI updates", get_name());
}

void ControlsPanel::refresh_all_displays() {
    // Re-read cached values from subjects and update all formatted displays
    if (auto* subj = printer_state_.get_active_extruder_temp_subject()) {
        cached_extruder_temp_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.get_active_extruder_target_subject()) {
        cached_extruder_target_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.get_bed_temp_subject()) {
        cached_bed_temp_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.get_bed_target_subject()) {
        cached_bed_target_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.get_chamber_temp_subject()) {
        cached_chamber_temp_ = lv_subject_get_int(subj);
    }
    if (auto* subj = printer_state_.get_chamber_target_subject()) {
        cached_chamber_target_ = lv_subject_get_int(subj); // keypad seed
    }
    if (auto* subj = printer_state_.get_chamber_effective_target_subject()) {
        cached_chamber_effective_target_ = lv_subject_get_int(subj); // status display
    }
    if (auto* subj = printer_state_.get_chamber_mode_subject()) {
        cached_chamber_mode_ = lv_subject_get_int(subj); // M141 control mode
    }
    update_nozzle_temp_display();
    update_bed_temp_display();
    update_chamber_temp_display();
    update_fan_display();
    update_nozzle_label();
    update_speed_display();

    // Re-read position subjects
    if (auto* subj = printer_state_.get_gcode_position_x_subject()) {
        int centimm = lv_subject_get_int(subj);
        format_position(centimm, controls_pos_x_buf_, sizeof(controls_pos_x_buf_));
        lv_subject_copy_string(&controls_pos_x_subject_, controls_pos_x_buf_);
    }
    if (auto* subj = printer_state_.get_gcode_position_y_subject()) {
        int centimm = lv_subject_get_int(subj);
        format_position(centimm, controls_pos_y_buf_, sizeof(controls_pos_y_buf_));
        lv_subject_copy_string(&controls_pos_y_subject_, controls_pos_y_buf_);
    }
    if (auto* subj = printer_state_.get_gcode_position_z_subject()) {
        int centimm = lv_subject_get_int(subj);
        format_position(centimm, controls_pos_z_buf_, sizeof(controls_pos_z_buf_));
        lv_subject_copy_string(&controls_pos_z_subject_, controls_pos_z_buf_);
    }

    // Re-read Z-offset subjects
    if (auto* subj = printer_state_.get_pending_z_offset_delta_subject()) {
        update_z_offset_delta_display(lv_subject_get_int(subj));
    }
    update_controls_z_offset_display();

    spdlog::trace("[{}] All displays refreshed after activation", get_name());
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void ControlsPanel::setup_card_handlers() {
    // All card click handlers are now wired via XML event_cb - see init_subjects().
    // This function is retained for validation and debugging purposes.

    lv_obj_t* card_quick_actions = nullptr;
    lv_obj_t* card_temperatures = nullptr;
    lv_obj_t* card_cooling = nullptr;
    lv_obj_t* card_calibration = nullptr;

    FIND_WIDGET_OPTIONAL(card_quick_actions, panel_, "card_quick_actions");
    FIND_WIDGET_OPTIONAL(card_temperatures, panel_, "card_temperatures");
    FIND_WIDGET_OPTIONAL(card_cooling, panel_, "card_cooling");
    FIND_WIDGET_OPTIONAL(card_calibration, panel_, "card_calibration");

    if (!card_quick_actions || !card_temperatures || !card_cooling || !card_calibration) {
        spdlog::error("[{}] Failed to find all V2 cards", get_name());
        return;
    }

    spdlog::trace("[{}] V2 card navigation handlers validated (wired via XML event_cb)",
                  get_name());
}

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
    chamber_temp_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_chamber_temp_subject(chamber_temp_lifetime_), this,
        [](ControlsPanel* self, int value) {
            self->cached_chamber_temp_ = value;
            if (self->are_subjects_initialized() && self->active_)
                self->update_chamber_temp_display();
        },
        chamber_temp_lifetime_);
    // Raw heater target is kept for keypad seed only (shows the currently entered
    // heater setpoint when the user opens the keypad to edit the chamber target).
    chamber_target_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_chamber_target_subject(chamber_target_lifetime_), this,
        [](ControlsPanel* self, int value) {
            self->cached_chamber_target_ = value;
            // Status display uses cached_chamber_effective_target_, not this value.
        },
        chamber_target_lifetime_);
    // Effective target is the canonical display value: heater target when heating,
    // cooling-fan ceiling when maintaining, 0 when off — drives the status string.
    chamber_effective_target_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_chamber_effective_target_subject(chamber_effective_target_lifetime_),
        this,
        [](ControlsPanel* self, int value) {
            self->cached_chamber_effective_target_ = value;
            if (self->are_subjects_initialized() && self->active_)
                self->update_chamber_temp_display();
        },
        chamber_effective_target_lifetime_);
    // M141 control mode (Off/Heating/Maintaining) — needed so the status string
    // leads with the correct mode word rather than the raw thermal state.
    chamber_mode_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_chamber_mode_subject(chamber_mode_lifetime_), this,
        [](ControlsPanel* self, int value) {
            self->cached_chamber_mode_ = value;
            if (self->are_subjects_initialized() && self->active_)
                self->update_chamber_temp_display();
        },
        chamber_mode_lifetime_);

    // Subscribe to fan updates (skip formatting when hidden)
    fan_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_fan_speed_subject(), this,
        [](ControlsPanel* self, int /* value */) {
            if (self->active_)
                self->update_fan_display();
        },
        printer_state_.get_subjects_lifetime());

    // Subscribe to multi-fan list changes (fires when fans are discovered/updated)
    // Skip widget rebuilds when hidden; on_activate() calls populate_secondary_fans()
    fans_version_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_fans_version_subject(), this,
        [](ControlsPanel* self, int /* version */) {
            if (!self->active_)
                return;
            // Defer rebuild (#80) AND use safe_clean_children (#776): lifetime_.defer
            // moves the rebuild off the observer callback's stack, and
            // safe_clean_children escapes UpdateQueue::process_pending() so sync
            // lv_obj_clean() can't corrupt LVGL's event linked list.
            if (!self->fans_rebuild_pending_) {
                self->fans_rebuild_pending_ = true;
                self->lifetime_.defer("ControlsPanel::populate_secondary_fans", [self]() {
                    self->fans_rebuild_pending_ = false;
                    if (self->active_ && self->secondary_fans_list_)
                        self->populate_secondary_fans();
                });
            }
        });

    // Which macros a printer defines is not fixed for the life of a session: a
    // Klipper restart or a config change re-runs discovery, and StandardMacros
    // re-resolves every slot against the new list. Sampling once at setup() left
    // a button enabled for a macro that had gone away (and hidden for one that
    // had arrived) until the panel next deactivated.
    macros_version_observer_ = observe_int_sync<ControlsPanel>(
        StandardMacros::instance().get_macros_version_subject(), this,
        [](ControlsPanel* self, int /* version */) { self->refresh_macro_buttons(); },
        StandardMacros::instance().get_subjects_lifetime());

    // Subscribe to active tool changes for dynamic nozzle label
    active_tool_observer_ = observe_int_sync<ControlsPanel>(
        helix::ToolState::instance().get_active_tool_subject(), this,
        [](ControlsPanel* self, int /* tool_idx */) {
            if (self->active_)
                self->update_nozzle_label();
        },
        helix::ToolState::instance().get_subjects_lifetime());
    update_nozzle_label(); // Set initial value

    // Subscribe to temperature sensor count changes
    // Skip widget rebuilds when hidden; on_activate() calls populate_secondary_temps()
    temp_sensor_count_observer_ = observe_int_sync<ControlsPanel>(
        helix::sensors::TemperatureSensorManager::instance().get_sensor_count_subject(), this,
        [](ControlsPanel* self, int /* count */) {
            if (!self->active_)
                return;
            // Defer rebuild (#80) AND use safe_clean_children (#776): lifetime_.defer
            // moves the rebuild off the observer callback's stack, and
            // safe_clean_children escapes UpdateQueue::process_pending() so sync
            // lv_obj_clean() can't corrupt LVGL's event linked list.
            if (!self->temps_rebuild_pending_) {
                self->temps_rebuild_pending_ = true;
                self->lifetime_.defer("ControlsPanel::populate_secondary_temps", [self]() {
                    self->temps_rebuild_pending_ = false;
                    if (self->active_ && self->secondary_temps_list_)
                        self->populate_secondary_temps();
                });
            }
        });

    // Subscribe to pending Z-offset delta (for unsaved adjustment banner)
    pending_z_offset_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_pending_z_offset_delta_subject(), this,
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
    speed_factor_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_speed_factor_subject(), this,
        [](ControlsPanel* self, int /* value */) {
            if (self->active_)
                self->update_speed_display();
        },
        printer_state_.get_subjects_lifetime());

    // Subscribe to gcode Z-offset for live tuning display (skip formatting when hidden)
    gcode_z_offset_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_gcode_z_offset_subject(), this,
        [](ControlsPanel* self, int /* offset_microns */) {
            if (self->active_)
                self->update_controls_z_offset_display();
        },
        printer_state_.get_subjects_lifetime());

    // The displayed Z-offset switches source between the live and the
    // firmware-persisted reading, so all three inputs have to retrigger it.
    persisted_z_offset_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_persisted_z_offset_subject(), this,
        [](ControlsPanel* self, int /* offset_microns */) {
            if (self->active_)
                self->update_controls_z_offset_display();
        },
        printer_state_.get_subjects_lifetime());

    persisted_z_offset_valid_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_persisted_z_offset_valid_subject(), this,
        [](ControlsPanel* self, int /* valid */) {
            if (self->active_)
                self->update_controls_z_offset_display();
        },
        printer_state_.get_subjects_lifetime());

    z_offset_print_active_observer_ = observe_int_sync<ControlsPanel>(
        printer_state_.get_print_active_subject(), this,
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

void ControlsPanel::update_nozzle_temp_display() {
    auto result =
        helix::ui::temperature::heater_display(cached_extruder_temp_, cached_extruder_target_);

    std::snprintf(nozzle_temp_buf_, sizeof(nozzle_temp_buf_), "%s", result.temp.c_str());
    lv_subject_copy_string(&nozzle_temp_subject_, nozzle_temp_buf_);

    lv_subject_set_int(&nozzle_pct_subject_, result.pct);

    std::snprintf(nozzle_status_buf_, sizeof(nozzle_status_buf_), "%s", result.status.c_str());
    lv_subject_copy_string(&nozzle_status_subject_, nozzle_status_buf_);
}

void ControlsPanel::update_bed_temp_display() {
    auto result = helix::ui::temperature::heater_display(cached_bed_temp_, cached_bed_target_);

    std::snprintf(bed_temp_buf_, sizeof(bed_temp_buf_), "%s", result.temp.c_str());
    lv_subject_copy_string(&bed_temp_subject_, bed_temp_buf_);

    lv_subject_set_int(&bed_pct_subject_, result.pct);

    std::snprintf(bed_status_buf_, sizeof(bed_status_buf_), "%s", result.status.c_str());
    lv_subject_copy_string(&bed_status_subject_, bed_status_buf_);
}

void ControlsPanel::update_chamber_temp_display() {
    // Delegate to the shared helper so this panel and the temp-graph overlay
    // always produce identical output (single source of truth).  The mode drives
    // the leading word (Off/Heating/Maintaining); thermal progress ("Ready"/
    // "Cooling") is appended only when it adds information.
    auto status = helix::ui::temperature::chamber_status_text(
        cached_chamber_temp_, cached_chamber_effective_target_,
        static_cast<helix::ChamberMode>(cached_chamber_mode_));
    std::snprintf(chamber_status_buf_, sizeof(chamber_status_buf_), "%s", status.c_str());
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

    int fan_pct = printer_state_.get_fan_speed_subject()
                      ? lv_subject_get_int(printer_state_.get_fan_speed_subject())
                      : 0;

    if (fan_pct > 0) {
        helix::format::format_percent(fan_pct, fan_speed_buf_, sizeof(fan_speed_buf_));
    } else {
        std::snprintf(fan_speed_buf_, sizeof(fan_speed_buf_), "%s", lv_tr("Off"));
    }
    lv_subject_copy_string(&fan_speed_subject_, fan_speed_buf_);
    lv_subject_set_int(&fan_pct_subject_, fan_pct);
}

void ControlsPanel::update_macro_button(StandardMacros& macros,
                                        const std::optional<StandardMacroSlot>& slot,
                                        lv_subject_t& visible_subject,
                                        lv_subject_t& available_subject, lv_subject_t& name_subject,
                                        int button_num) {
    if (!slot) {
        lv_subject_set_int(&visible_subject, 0);
        lv_subject_set_int(&available_subject, 0);
        return;
    }

    const auto& info = macros.get(*slot);

    if (!info.is_empty()) {
        lv_subject_set_int(&visible_subject, 1);
        lv_subject_set_int(&available_subject, 1);
        lv_subject_copy_string(&name_subject, info.translated_name());
        spdlog::trace("[{}] Macro {}: '{}' → {}", get_name(), button_num, info.display_name,
                      info.get_macro());
        return;
    }

    if (info.has_missing_macro()) {
        // The user assigned this slot and the printer does not answer for it (a
        // preset can seed a macro name the machine never defined, and a Klipper
        // config change can retire one). Keep the button where they left it and
        // grey it out: a button that disappears reads as a bug in the screen,
        // while a disabled one points at the assignment that needs fixing.
        lv_subject_set_int(&visible_subject, 1);
        lv_subject_set_int(&available_subject, 0);
        lv_subject_copy_string(&name_subject, info.translated_name());
        spdlog::debug("[{}] Macro {} slot '{}' disabled: '{}' is not defined on this printer",
                      get_name(), button_num, info.slot_name, info.missing_macro);
        return;
    }

    lv_subject_set_int(&visible_subject, 0);
    lv_subject_set_int(&available_subject, 0);
    spdlog::trace("[{}] Macro {} slot '{}' is empty, hiding button", get_name(), button_num,
                  info.slot_name);
}

void ControlsPanel::refresh_macro_buttons() {
    auto& macros = StandardMacros::instance();

    // Arrays for iteration - slots, visible subjects, name subjects, button numbers
    const std::optional<StandardMacroSlot>* slots[] = {&macro_1_slot_, &macro_2_slot_,
                                                       &macro_3_slot_, &macro_4_slot_};
    lv_subject_t* visible_subjects[] = {&macro_1_visible_, &macro_2_visible_, &macro_3_visible_,
                                        &macro_4_visible_};
    lv_subject_t* available_subjects[] = {&macro_1_available_, &macro_2_available_,
                                          &macro_3_available_, &macro_4_available_};
    lv_subject_t* name_subjects[] = {&macro_1_name_, &macro_2_name_, &macro_3_name_,
                                     &macro_4_name_};

    for (size_t i = 0; i < 4; ++i) {
        update_macro_button(macros, *slots[i], *visible_subjects[i], *available_subjects[i],
                            *name_subjects[i], static_cast<int>(i + 1));
    }

    // Hide the Quick Actions header when row 2 is visible (macro 3 or 4) to save space
    bool row2_visible =
        lv_subject_get_int(&macro_3_visible_) == 1 || lv_subject_get_int(&macro_4_visible_) == 1;
    lv_subject_set_int(&macro_header_visible_, row2_visible ? 0 : 1);
}

/// @brief Priority score for fan display ordering on the cooling card.
/// Lower score = higher priority (shown first).
static int fan_display_priority(const helix::FanInfo& fan) {
    // Chamber fans are most interesting to users (enclosure management)
    // Use object_name (Moonraker identifier) rather than display_name to avoid localization issues
    if (fan.object_name.find("chamber") != std::string::npos) {
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
    // use-after-free when observe_int_sync callbacks fire after widget deletion.
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
    lv_obj_update_layout(secondary_fans_list_);
    helix::ui::safe_clean_children(secondary_fans_list_);

    // Collect non-part-cooling fans and sort by display priority
    const auto& fans = printer_state_.get_fans();
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

        // Create a row for this fan: [Name] [Speed%] [Icon]
        lv_obj_t* row = lv_obj_create(secondary_fans_list_);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_row(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        // Fan name label - 60% width, truncate with ellipsis if needed
        lv_obj_t* name_label = lv_label_create(row);
        lv_label_set_text(name_label, fan->display_name.c_str());
        lv_obj_set_width(name_label, LV_PCT(60));
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_text_font(name_label, theme_manager_get_font("font_small"), 0);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

        // Speed percentage label - right-aligned
        char speed_buf[16];
        if (fan->speed_percent > 0) {
            helix::format::format_percent(fan->speed_percent, speed_buf, sizeof(speed_buf));
        } else {
            std::snprintf(speed_buf, sizeof(speed_buf), "%s", lv_tr("Off"));
        }
        lv_obj_t* speed_label = lv_label_create(row);
        lv_label_set_text(speed_label, speed_buf);
        lv_obj_set_style_text_color(speed_label, theme_manager_get_color("text"), 0);
        lv_obj_set_style_text_font(speed_label, theme_manager_get_font("font_small"), 0);

        // Track this row for reactive speed updates
        secondary_fan_rows_.push_back({fan->object_name, speed_label});

        // Indicator icon: "A" circle for auto-controlled, › for controllable
        lv_obj_t* indicator = lv_label_create(row);
        if (fan->is_controllable) {
            lv_label_set_text(indicator, LV_SYMBOL_RIGHT);
        } else {
            lv_label_set_text(indicator, ui_icon::lookup_codepoint("alpha_a_circle"));
        }
        lv_obj_set_style_text_color(indicator, theme_manager_get_color("secondary"), 0);
        lv_obj_set_style_text_font(indicator, &mdi_icons_16, 0);

        visible_count++;
    }

    // Show "N additional fans >" row if there are more fans than visible
    int additional = static_cast<int>(secondary_fans.size()) - visible_count;
    if (additional > 0) {
        lv_obj_t* more_row = lv_obj_create(secondary_fans_list_);
        lv_obj_set_width(more_row, LV_PCT(100));
        lv_obj_set_height(more_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(more_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(more_row, 0, 0);
        lv_obj_set_style_pad_all(more_row, 0, 0);
        lv_obj_remove_flag(more_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(more_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(more_row, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_flex_flow(more_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(more_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        // "N additional fans" label
        char more_buf[32];
        std::snprintf(more_buf, sizeof(more_buf), lv_tr("%d additional fan%s"), additional,
                      additional == 1 ? "" : "s");
        lv_obj_t* more_label = lv_label_create(more_row);
        lv_label_set_text(more_label, more_buf);
        lv_obj_set_style_text_color(more_label, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_text_font(more_label, theme_manager_get_font("font_small"), 0);

        // Chevron right indicator
        lv_obj_t* chevron = lv_label_create(more_row);
        lv_label_set_text(chevron, ui_icon::lookup_codepoint("chevron_right"));
        lv_obj_set_style_text_color(chevron, theme_manager_get_color("secondary"), 0);
        lv_obj_set_style_text_font(chevron, &mdi_icons_16, 0);

        // Click is handled by the parent container's on_secondary_fans_clicked trampoline
        // (registered once in setup()). No per-child event callback needed.
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
    auto strategy = printer_state_.get_z_offset_calibration_strategy();
    if (helix::zoffset::is_auto_saved(strategy))
        return;

    int offset_microns = 0;
    if (auto* subj = printer_state_.get_gcode_z_offset_subject()) {
        offset_microns = lv_subject_get_int(subj);
    }

    if (offset_microns == 0) {
        spdlog::debug("[{}] No Z-offset adjustment to save", get_name());
        return;
    }

    spdlog::info("[{}] Save Z-offset clicked: {:+.3f}mm", get_name(),
                 static_cast<double>(offset_microns) / 1000.0);

    const char* confirm_msg =
        (strategy == ZOffsetCalibrationStrategy::PROBE_CALIBRATE)
            ? lv_tr("This will apply the Z-offset to your probe and restart Klipper to save the "
                    "configuration. The printer will briefly disconnect.")
            : lv_tr("This will apply the Z-offset to your endstop and restart Klipper to save the "
                    "configuration. The printer will briefly disconnect.");

    save_z_offset_confirmation_dialog_ = helix::ui::modal_show_confirmation(
        lv_tr("Save Z-Offset?"), confirm_msg, ModalSeverity::Warning, lv_tr("Save"),
        on_save_z_offset_confirm, on_save_z_offset_cancel, this);

    if (!save_z_offset_confirmation_dialog_) {
        LOG_ERROR_INTERNAL("Failed to create save Z-offset confirmation dialog");
        NOTIFY_ERROR(lv_tr("Failed to show confirmation dialog"));
        return;
    }

    spdlog::info("[{}] Save Z-offset confirmation dialog shown", get_name());
}

void ControlsPanel::handle_save_z_offset_confirm() {
    spdlog::debug("[{}] Save Z-offset confirmed", get_name());

    if (save_z_offset_guard_.is_active()) {
        spdlog::warn("[{}] Save Z-offset already in progress, ignoring", get_name());
        return;
    }

    // Bounded guard: SAVE_CONFIG restarts Klipper, which drops the in-flight RPC,
    // so the success/error callbacks below are not guaranteed to fire. Without a
    // timeout the button would stay disabled until the app restarts.
    save_z_offset_guard_.begin(SAVE_Z_OFFSET_TIMEOUT_MS, [this] {
        spdlog::warn("[{}] Save Z-offset guard timed out — re-enabling save", get_name());
    });

    save_z_offset_confirmation_dialog_.hide();

    if (!api_) {
        NOTIFY_ERROR(lv_tr("No printer connection"));
        save_z_offset_guard_.end();
        return;
    }

    int offset_microns = 0;
    if (auto* subj = printer_state_.get_gcode_z_offset_subject()) {
        offset_microns = lv_subject_get_int(subj);
    }
    double offset_mm = static_cast<double>(offset_microns) / 1000.0;

    auto strategy = printer_state_.get_z_offset_calibration_strategy();

    NOTIFY_INFO(lv_tr("Saving Z-offset..."));

    auto tok = lifetime_.token();
    helix::zoffset::apply_and_save(
        api_, strategy,
        [this, tok, offset_mm]() {
            tok.defer("ControlsPanel::save_z_offset_done", [this, offset_mm]() {
                NOTIFY_SUCCESS(lv_tr("Z-offset saved ({:+.3f}mm). Klipper restarting..."),
                               offset_mm);
                save_z_offset_guard_.end();
            });
        },
        [this, tok](const std::string& error) {
            tok.defer("ControlsPanel::save_z_offset_done", [this, error]() {
                NOTIFY_ERROR("{}", error);
                save_z_offset_guard_.end();
            });
        });
}

void ControlsPanel::handle_save_z_offset_cancel() {
    spdlog::debug("[{}] Save Z-offset cancelled", get_name());

    // ModalGuard handles cleanup
    save_z_offset_confirmation_dialog_.hide();
}

// ============================================================================
// V2 CARD CLICK HANDLERS
// ============================================================================

void ControlsPanel::handle_quick_actions_clicked() {
    helix::ui::lazy_create_and_push_overlay<MotionPanel>(get_global_motion_panel, motion_panel_,
                                                         parent_screen_, "Motion", get_name());
}

void ControlsPanel::handle_nozzle_temp_clicked() {
    spdlog::debug("[{}] Nozzle temp clicked - opening temperature graph", get_name());
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, parent_screen_);
}

void ControlsPanel::handle_bed_temp_clicked() {
    spdlog::debug("[{}] Bed temp clicked - opening temperature graph", get_name());
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, parent_screen_);
}

void ControlsPanel::handle_nozzle_target_edit() {
    int max_temp = nozzle_max_temp_;
    if (auto* c = controller()) {
        c->ensure_limits(helix::HeaterType::Nozzle);
        max_temp = static_cast<int>(c->keypad_range(helix::HeaterType::Nozzle).max);
    }
    show_temperature_keypad<&ControlsPanel::handle_custom_nozzle_confirmed>(
        "Nozzle Temperature", cached_extruder_target_, 200, max_temp);
}

void ControlsPanel::handle_bed_target_edit() {
    int max_temp = bed_max_temp_;
    if (auto* c = controller()) {
        c->ensure_limits(helix::HeaterType::Bed);
        max_temp = static_cast<int>(c->keypad_range(helix::HeaterType::Bed).max);
    }
    show_temperature_keypad<&ControlsPanel::handle_custom_bed_confirmed>(
        "Bed Temperature", cached_bed_target_, 60, max_temp);
}

void ControlsPanel::handle_chamber_target_edit() {
    int max_temp = chamber_max_temp_;
    if (auto* c = controller()) {
        c->ensure_limits(helix::HeaterType::Chamber);
        max_temp = static_cast<int>(c->keypad_range(helix::HeaterType::Chamber).max);
    }
    // Seed from the effective target (heater target when Heating, fan target when
    // Maintaining) so the keypad pre-fills the value the card already shows. The
    // raw heater target reads 0 during M141 maintain mode and would otherwise seed 0.
    show_temperature_keypad<&ControlsPanel::handle_custom_chamber_confirmed>(
        "Chamber Temperature", cached_chamber_effective_target_, 50, max_temp);
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

    // Create fan control overlay on first access (lazy initialization)
    if (!fan_control_panel_ && parent_screen_) {
        auto& overlay = get_fan_control_overlay();

        // Initialize subjects and callbacks if not already done
        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();

        // Pass the API reference for fan commands
        overlay.set_api(api_);

        // Create overlay UI
        fan_control_panel_ = overlay.create(parent_screen_);
        if (!fan_control_panel_) {
            NOTIFY_ERROR(lv_tr("Failed to load fan control overlay"));
            return;
        }

        // Register with NavigationManager for lifecycle callbacks
        NavigationManager::instance().register_overlay_instance(fan_control_panel_, &overlay);
    }

    if (fan_control_panel_) {
        // Update API reference in case it changed
        get_fan_control_overlay().set_api(api_);
        NavigationManager::instance().push_overlay(fan_control_panel_);
    }
}

// ============================================================================
// QUICK ACTION BUTTON HANDLERS
// ============================================================================

void ControlsPanel::handle_home_all() {
    spdlog::debug("[{}] Home All clicked", get_name());
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }
    if (api_) {
        operation_guard_.begin(300000, [] { NOTIFY_WARNING(lv_tr("Homing timed out")); });
        NOTIFY_INFO(lv_tr("Homing all axes..."));
        // bg_cb defers the whole callback body to the main thread atomically —
        // no bare bg-thread expired() check (L081 Mechanism C, hit on v0.99.60/ad5x).
        api_->motion().home_axes(
            "", lifetime_.bg_cb("ControlsPanel::home_all_ok", [this]() { operation_guard_.end(); }),
            lifetime_.bg_cb("ControlsPanel::home_all_err", [this](const MoonrakerError& err) {
                operation_guard_.end();
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    NOTIFY_WARNING(lv_tr("Homing may still be running — response timed out"));
                } else {
                    NOTIFY_ERROR(lv_tr("Homing failed: {}"), err.user_message());
                }
            }));
    }
}

void ControlsPanel::handle_home_x() {
    spdlog::debug("[{}] Home X clicked", get_name());
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }
    if (api_) {
        auto tok = lifetime_.token();
        operation_guard_.begin(300000, [] { NOTIFY_WARNING(lv_tr("Homing timed out")); });
        NOTIFY_INFO(lv_tr("Homing X..."));
        api_->motion().home_axes(
            "X",
            [this, tok]() {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
            },
            [this, tok](const MoonrakerError& err) {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    NOTIFY_WARNING(lv_tr("Homing may still be running — response timed out"));
                } else {
                    NOTIFY_ERROR(lv_tr("Homing failed: {}"), err.user_message());
                }
            });
    }
}

void ControlsPanel::handle_home_y() {
    spdlog::debug("[{}] Home Y clicked", get_name());
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }
    if (api_) {
        auto tok = lifetime_.token();
        operation_guard_.begin(300000, [] { NOTIFY_WARNING(lv_tr("Homing timed out")); });
        NOTIFY_INFO(lv_tr("Homing Y..."));
        api_->motion().home_axes(
            "Y",
            [this, tok]() {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
            },
            [this, tok](const MoonrakerError& err) {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    NOTIFY_WARNING(lv_tr("Homing may still be running — response timed out"));
                } else {
                    NOTIFY_ERROR(lv_tr("Homing failed: {}"), err.user_message());
                }
            });
    }
}

void ControlsPanel::handle_home_xy() {
    spdlog::debug("[{}] Home XY clicked", get_name());
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }
    if (api_) {
        auto tok = lifetime_.token();
        operation_guard_.begin(300000, [] { NOTIFY_WARNING(lv_tr("Homing timed out")); });
        NOTIFY_INFO(lv_tr("Homing XY..."));
        api_->motion().home_axes(
            "XY",
            [this, tok]() {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
            },
            [this, tok](const MoonrakerError& err) {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    NOTIFY_WARNING(lv_tr("Homing may still be running — response timed out"));
                } else {
                    NOTIFY_ERROR(lv_tr("Homing failed: {}"), err.user_message());
                }
            });
    }
}

void ControlsPanel::handle_home_z() {
    spdlog::debug("[{}] Home Z clicked", get_name());
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }
    if (api_) {
        auto tok = lifetime_.token();
        operation_guard_.begin(300000, [] { NOTIFY_WARNING(lv_tr("Homing timed out")); });
        NOTIFY_INFO(lv_tr("Homing Z..."));
        api_->motion().home_axes(
            "Z",
            [this, tok]() {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
            },
            [this, tok](const MoonrakerError& err) {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    NOTIFY_WARNING(lv_tr("Homing may still be running — response timed out"));
                } else {
                    NOTIFY_ERROR(lv_tr("Homing failed: {}"), err.user_message());
                }
            });
    }
}

void ControlsPanel::handle_qgl() {
    spdlog::debug("[{}] QGL clicked", get_name());
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }
    if (api_) {
        auto tok = lifetime_.token();
        operation_guard_.begin(600000, [] { NOTIFY_WARNING(lv_tr("QGL timed out")); });
        NOTIFY_INFO(lv_tr("Quad Gantry Level started..."));
        api_->execute_gcode(
            "QUAD_GANTRY_LEVEL",
            [this, tok]() {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
                NOTIFY_SUCCESS(lv_tr("Quad Gantry Level complete"));
            },
            [this, tok](const MoonrakerError& err) {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    NOTIFY_WARNING(lv_tr("QGL may still be running — response timed out"));
                } else {
                    NOTIFY_ERROR(lv_tr("QGL failed: {}"), err.user_message());
                }
            },
            MoonrakerAdvancedAPI::LEVELING_TIMEOUT_MS);
    }
}

void ControlsPanel::handle_z_tilt() {
    spdlog::debug("[{}] Z-Tilt clicked", get_name());
    if (operation_guard_.is_active()) {
        NOTIFY_WARNING(lv_tr("Operation already in progress"));
        return;
    }
    if (api_) {
        auto tok = lifetime_.token();
        operation_guard_.begin(600000, [] { NOTIFY_WARNING(lv_tr("Z-Tilt timed out")); });
        NOTIFY_INFO(lv_tr("Z-Tilt Adjust started..."));
        api_->execute_gcode(
            "Z_TILT_ADJUST",
            [this, tok]() {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
                NOTIFY_SUCCESS(lv_tr("Z-Tilt Adjust complete"));
            },
            [this, tok](const MoonrakerError& err) {
                tok.defer("ControlsPanel::operation_guard_end",
                          [this]() { operation_guard_.end(); });
                if (err.type == MoonrakerErrorType::TIMEOUT) {
                    NOTIFY_WARNING(lv_tr("Z-Tilt may still be running — response timed out"));
                } else {
                    NOTIFY_ERROR(lv_tr("Z-Tilt failed: {}"), err.user_message());
                }
            },
            MoonrakerAdvancedAPI::LEVELING_TIMEOUT_MS);
    }
}

void ControlsPanel::execute_macro(size_t index) {
    const std::optional<StandardMacroSlot>* slots[] = {&macro_1_slot_, &macro_2_slot_,
                                                       &macro_3_slot_, &macro_4_slot_};
    if (index >= 4) {
        spdlog::warn("[{}] Invalid macro index: {}", get_name(), index);
        return;
    }

    const auto& slot = *slots[index];
    if (!slot) {
        spdlog::debug("[{}] Macro {} clicked but no slot configured", get_name(),
                      static_cast<int>(index + 1));
        return;
    }

    // Backstop for the XML disabled binding. LVGL suppresses CLICKED on a
    // LV_STATE_DISABLED object, so this normally cannot be reached from touch —
    // but execute_macro() is also the entry point for the remote-control server
    // and any future caller, and dispatching a macro the printer does not define
    // is exactly the silent failure this gate exists to stop.
    const auto& gate_info = StandardMacros::instance().get(*slot);
    if (gate_info.is_empty() && gate_info.has_missing_macro()) {
        spdlog::warn("[{}] Macro {} slot '{}' names '{}', which this printer does not define",
                     get_name(), static_cast<int>(index + 1), gate_info.slot_name,
                     gate_info.missing_macro);
        NOTIFY_WARNING(lv_tr("{} is not set up on this printer"), gate_info.translated_name());
        return;
    }

    if (!helix::SafetySettingsManager::instance().get_macro_require_confirmation()) {
        do_execute_macro(index);
        return;
    }

    const auto& info = StandardMacros::instance().get(*slot);
    pending_macro_run_index_ = index;
    std::string msg = fmt::format(lv_tr("Run {}?"), info.translated_name());
    macro_run_confirmation_dialog_ = helix::ui::modal_show_confirmation(
        lv_tr("Run Macro?"), msg.c_str(), ModalSeverity::Info, lv_tr("Run"),
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("[ControlsPanel] macro_run_confirm_cb");
            auto* self = static_cast<ControlsPanel*>(lv_event_get_user_data(e));
            size_t idx = self->pending_macro_run_index_;
            self->macro_run_confirmation_dialog_.hide();
            self->do_execute_macro(idx);
            LVGL_SAFE_EVENT_CB_END();
        },
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("[ControlsPanel] macro_run_cancel_cb");
            auto* self = static_cast<ControlsPanel*>(lv_event_get_user_data(e));
            self->macro_run_confirmation_dialog_.hide();
            LVGL_SAFE_EVENT_CB_END();
        },
        this);
}

void ControlsPanel::do_execute_macro(size_t index) {
    const std::optional<StandardMacroSlot>* slots[] = {&macro_1_slot_, &macro_2_slot_,
                                                       &macro_3_slot_, &macro_4_slot_};
    if (index >= 4) {
        return;
    }
    const auto& slot = *slots[index];
    if (!slot) {
        return;
    }

    const auto& info = StandardMacros::instance().get(*slot);
    int button_num = static_cast<int>(index + 1);
    spdlog::debug("[{}] Macro {} clicked, executing slot '{}' → {}", get_name(), button_num,
                  info.slot_name, info.get_macro());

    NOTIFY_INFO(lv_tr("Running {}..."), info.translated_name());
    if (!StandardMacros::instance().execute(
            *slot, api_,
            [name = std::string(info.translated_name())]() {
                NOTIFY_SUCCESS(lv_tr("{} complete"), name);
            },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Macro failed: {}"), err.user_message());
            })) {
        NOTIFY_WARNING(lv_tr("{} macro not configured"), info.translated_name());
    }
}

// ============================================================================
// SPEED/FLOW OVERRIDE HANDLERS
// ============================================================================

void ControlsPanel::update_speed_display() {
    int speed_pct = 100;
    if (auto* speed_subj = printer_state_.get_speed_factor_subject()) {
        speed_pct = lv_subject_get_int(speed_subj);
    }
    helix::format::format_percent(speed_pct, speed_override_buf_, sizeof(speed_override_buf_));
    lv_subject_copy_string(&speed_override_subject_, speed_override_buf_);
}

void ControlsPanel::update_flow_display() {
    // Flow factor is stored as percentage (100 = 100%)
    int flow_pct = 100;
    // Note: PrinterState may need a get_extrude_factor_subject() method
    // For now, we'll initialize to 100% and update when that's available
    helix::format::format_percent(flow_pct, flow_override_buf_, sizeof(flow_override_buf_));
    lv_subject_copy_string(&flow_override_subject_, flow_override_buf_);
}

void ControlsPanel::handle_speed_up() {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("No printer connection"));
        return;
    }

    int current = 100;
    if (auto* speed_subj = printer_state_.get_speed_factor_subject()) {
        current = lv_subject_get_int(speed_subj);
    }

    int new_speed = std::min(current + 10, 200); // Cap at 200%
    spdlog::debug("[{}] Speed up: {} → {}", get_name(), current, new_speed);

    char gcode[32];
    std::snprintf(gcode, sizeof(gcode), "M220 S%d", new_speed);
    api_->execute_gcode(
        gcode, []() { /* Silent success */ },
        [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Speed change failed: {}"), err.user_message());
        });
}

void ControlsPanel::handle_speed_down() {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("No printer connection"));
        return;
    }

    int current = 100;
    if (auto* speed_subj = printer_state_.get_speed_factor_subject()) {
        current = lv_subject_get_int(speed_subj);
    }

    int new_speed = std::max(current - 10, 10); // Floor at 10%
    spdlog::debug("[{}] Speed down: {} → {}", get_name(), current, new_speed);

    char gcode[32];
    std::snprintf(gcode, sizeof(gcode), "M220 S%d", new_speed);
    api_->execute_gcode(
        gcode, []() { /* Silent success */ },
        [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Speed change failed: {}"), err.user_message());
        });
}

void ControlsPanel::handle_flow_up() {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("No printer connection"));
        return;
    }

    // For now, track locally; ideally this would come from PrinterState
    static int current_flow = 100;
    int new_flow = std::min(current_flow + 5, 150); // Cap at 150%
    spdlog::debug("[{}] Flow up: {} → {}", get_name(), current_flow, new_flow);
    current_flow = new_flow;

    char gcode[32];
    std::snprintf(gcode, sizeof(gcode), "M221 S%d", new_flow);
    auto tok = lifetime_.token();
    api_->execute_gcode(
        gcode,
        [this, tok, new_flow]() {
            tok.defer("ControlsPanel::flow_display_update", [this, new_flow]() {
                helix::format::format_percent(new_flow, flow_override_buf_,
                                              sizeof(flow_override_buf_));
                lv_subject_copy_string(&flow_override_subject_, flow_override_buf_);
            });
        },
        [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Flow change failed: {}"), err.user_message());
        });
}

void ControlsPanel::handle_flow_down() {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("No printer connection"));
        return;
    }

    // For now, track locally; ideally this would come from PrinterState
    static int current_flow = 100;
    int new_flow = std::max(current_flow - 5, 50); // Floor at 50%
    spdlog::debug("[{}] Flow down: {} → {}", get_name(), current_flow, new_flow);
    current_flow = new_flow;

    char gcode[32];
    std::snprintf(gcode, sizeof(gcode), "M221 S%d", new_flow);
    auto tok = lifetime_.token();
    api_->execute_gcode(
        gcode,
        [this, tok, new_flow]() {
            tok.defer("ControlsPanel::flow_display_update", [this, new_flow]() {
                helix::format::format_percent(new_flow, flow_override_buf_,
                                              sizeof(flow_override_buf_));
                lv_subject_copy_string(&flow_override_subject_, flow_override_buf_);
            });
        },
        [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Flow change failed: {}"), err.user_message());
        });
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
    if (value > 0) {
        helix::format::format_percent(value, fan_speed_buf_, sizeof(fan_speed_buf_));
    } else {
        std::snprintf(fan_speed_buf_, sizeof(fan_speed_buf_), "%s", lv_tr("Off"));
    }
    lv_subject_copy_string(&fan_speed_subject_, fan_speed_buf_);
    lv_subject_set_int(&fan_pct_subject_, value);

    if (api_) {
        api_->set_fan_speed(
            "fan", static_cast<double>(value), []() { /* Silent success */ },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Fan control failed: {}"), err.user_message());
            });
    }
}

// ============================================================================
// CALIBRATION HANDLERS
// ============================================================================

void ControlsPanel::handle_motors_clicked() {
    spdlog::debug("[{}] Motors Disable card clicked - showing confirmation", get_name());

    // ModalGuard's operator= hides any previous dialog before assigning new one
    motors_confirmation_dialog_ = helix::ui::modal_show_confirmation(
        lv_tr("Disable Motors?"), lv_tr("Release all stepper motors. Position will be lost."),
        ModalSeverity::Warning, lv_tr("Disable"), on_motors_confirm, on_motors_cancel, this);

    if (!motors_confirmation_dialog_) {
        LOG_ERROR_INTERNAL("Failed to create motors confirmation dialog");
        NOTIFY_ERROR(lv_tr("Failed to show confirmation dialog"));
        return;
    }

    spdlog::info("[{}] Motors confirmation dialog shown", get_name());
}

void ControlsPanel::handle_motors_confirm() {
    spdlog::debug("[{}] Motors disable confirmed", get_name());

    // Hide dialog first - ModalGuard handles cleanup
    motors_confirmation_dialog_.hide();

    // Send M84 command to disable motors
    if (api_) {
        NOTIFY_INFO(lv_tr("Disabling motors..."));
        api_->execute_gcode(
            "M84", // Klipper command to disable steppers
            []() { NOTIFY_SUCCESS(lv_tr("Motors disabled")); },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Motors disable failed: {}"), err.message);
            });
    }
}

void ControlsPanel::handle_motors_cancel() {
    spdlog::debug("[{}] Motors disable cancelled", get_name());

    // ModalGuard handles cleanup
    motors_confirmation_dialog_.hide();
}

void ControlsPanel::handle_calibration_bed_mesh() {
#if defined(HELIX_PLATFORM_ESP32)
    // Bed-mesh calibration is excluded from the v1 Core+AMS cut; its panel is a
    // null-vtable link stub. Toast instead of the LoadProhibited crash.
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    helix::ui::lazy_create_and_push_overlay<BedMeshPanel>(
        get_global_bed_mesh_panel, bed_mesh_panel_, parent_screen_, "Bed Mesh", get_name(), true);
}

void ControlsPanel::handle_calibration_zoffset() {
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    // Set the Moonraker client before lazy creation so it's available when calibration starts
    get_global_zoffset_cal_panel().set_api(get_moonraker_api());
    helix::ui::lazy_create_and_push_overlay<ZOffsetCalibrationPanel>(
        get_global_zoffset_cal_panel, zoffset_panel_, parent_screen_, "Z-Offset Calibration",
        get_name());
}

void ControlsPanel::handle_calibration_screws() {
#if defined(HELIX_PLATFORM_ESP32)
    helix::ui::show_feature_unavailable_toast();
    return;
#endif
    get_global_screws_tilt_panel().set_client(get_moonraker_client(), get_moonraker_api());
    helix::ui::lazy_create_and_push_overlay<ScrewsTiltPanel>(
        get_global_screws_tilt_panel, screws_panel_, parent_screen_, "Bed Screws", get_name());
}

void ControlsPanel::handle_calibration_motors() {
    spdlog::debug("[{}] Disable Motors button clicked", get_name());
    handle_motors_clicked();
}

// ============================================================================
// V2 CARD CLICK TRAMPOLINES (XML event_cb - use global accessor)
// ============================================================================

PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, quick_actions_clicked)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, nozzle_temp_clicked)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, bed_temp_clicked)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, chamber_temp_clicked)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, cooling_clicked)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, secondary_fans_clicked)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, secondary_temps_clicked)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, nozzle_target_edit)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, bed_target_edit)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, chamber_target_edit)

PANEL_TRAMPOLINE_USERDATA(ControlsPanel, motors_confirm)
PANEL_TRAMPOLINE_USERDATA(ControlsPanel, motors_cancel)
PANEL_TRAMPOLINE_USERDATA(ControlsPanel, save_z_offset_confirm)
PANEL_TRAMPOLINE_USERDATA(ControlsPanel, save_z_offset_cancel)

// ============================================================================
// CALIBRATION BUTTON TRAMPOLINES (XML event_cb - use global accessor)
// ============================================================================

PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, calibration_bed_mesh)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, calibration_zoffset)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, calibration_screws)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, calibration_motors)

// ============================================================================
// V2 BUTTON TRAMPOLINES (XML event_cb - use global accessor)
// ============================================================================

PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, home_all)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, home_x)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, home_y)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, home_xy)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, home_z)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, qgl)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, z_tilt)
// Unified macro callback - extracts index from user_data
void ControlsPanel::on_macro(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ControlsPanel] on_macro");
    const char* index_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (index_str) {
        size_t index = strtoul(index_str, nullptr, 10);
        get_global_controls_panel().execute_macro(index);
    }
    LVGL_SAFE_EVENT_CB_END();
}

PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, speed_up)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, speed_down)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, flow_up)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, flow_down)
PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, zoffset_tune)

// Cannot use macro - has extra logic to extract slider value
void ControlsPanel::on_fan_slider_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ControlsPanel] on_fan_slider_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = lv_slider_get_value(slider);
    get_global_controls_panel().handle_fan_slider_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

PANEL_TRAMPOLINE(ControlsPanel, get_global_controls_panel, save_z_offset)

void ControlsPanel::subscribe_to_secondary_fan_speeds() {
    using helix::ui::observe_int_sync;
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
        if (auto* subject = printer_state_.get_fan_speed_subject(row.object_name, lifetime)) {
            secondary_fan_observers_.push_back(observe_int_sync<ControlsPanel>(
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
            if (speed_pct > 0) {
                helix::format::format_percent(speed_pct, speed_buf, sizeof(speed_buf));
            } else {
                std::snprintf(speed_buf, sizeof(speed_buf), "%s", lv_tr("Off"));
            }
            lv_label_set_text(row.speed_label, speed_buf);
            spdlog::trace("[{}] Updated secondary fan '{}' speed to {}", get_name(), object_name,
                          speed_buf);
            break;
        }
    }
}

// ============================================================================
// SECONDARY TEMPERATURE SENSORS (overflow list on temperature card)
// ============================================================================

void ControlsPanel::populate_secondary_temps() {
    if (!secondary_temps_list_) {
        return;
    }

    // Bump generation counter FIRST — stale deferred callbacks will skip
    ++temp_populate_gen_;

    // Cleanup order: observers first, then tracking, then widgets.
    // Use reset() not release() — subjects are alive, must properly unsubscribe
    for (auto& obs : secondary_temp_observers_) {
        obs.reset();
    }
    secondary_temp_observers_.clear();
    secondary_temp_rows_.clear();
    lv_obj_add_flag(secondary_temps_list_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(secondary_temps_list_);
    helix::ui::safe_clean_children(secondary_temps_list_);

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = tsm.get_sensors_sorted();

    // Filter to only enabled sensors (chamber is already shown as a dedicated row)
    std::vector<helix::sensors::TemperatureSensorConfig> visible;
    for (const auto& s : sensors) {
        if (s.enabled && s.role != helix::sensors::TemperatureSensorRole::CHAMBER) {
            visible.push_back(s);
        }
    }

    // Dashboard shows only the overflow link - full list is on the temp panel
    constexpr int max_visible = 0;
    int visible_count = 0;

    for (const auto& sensor : visible) {
        if (visible_count >= max_visible) {
            break;
        }

        // Create a row: [Name] [Temp C] [thermometer icon]
        lv_obj_t* row = lv_obj_create(secondary_temps_list_);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_row(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        // Sensor name label - 60% width, truncate with ellipsis
        lv_obj_t* name_label = lv_label_create(row);
        lv_label_set_text(name_label, sensor.display_name.c_str());
        lv_obj_set_width(name_label, LV_PCT(60));
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_text_font(name_label, theme_manager_get_font("font_small"), 0);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

        // Temperature value label - read initial value from subject.
        // SubjectLifetime is local — valid for one-shot read only, not observation.
        SubjectLifetime lt;
        auto* subj = tsm.get_temp_subject(sensor.klipper_name, lt);
        int decidegrees = subj ? lv_subject_get_int(subj) : 0;
        char temp_buf[16];
        helix::ui::temperature::format_temperature(
            helix::ui::temperature::deci_to_degrees(decidegrees), temp_buf, sizeof(temp_buf));
        lv_obj_t* temp_label = lv_label_create(row);
        lv_label_set_text(temp_label, temp_buf);
        lv_obj_set_style_text_color(temp_label, theme_manager_get_color("text"), 0);
        lv_obj_set_style_text_font(temp_label, theme_manager_get_font("font_small"), 0);

        // Track for reactive updates
        secondary_temp_rows_.push_back({sensor.klipper_name, temp_label});

        // Thermometer icon
        lv_obj_t* icon = lv_label_create(row);
        lv_label_set_text(icon, ui_icon::lookup_codepoint("thermometer"));
        lv_obj_set_style_text_color(icon, theme_manager_get_color("secondary"), 0);
        lv_obj_set_style_text_font(icon, &mdi_icons_16, 0);

        visible_count++;
    }

    // "N additional sensors >" overflow row
    int additional = static_cast<int>(visible.size()) - visible_count;
    if (additional > 0) {
        lv_obj_t* more_row = lv_obj_create(secondary_temps_list_);
        lv_obj_set_width(more_row, LV_PCT(100));
        lv_obj_set_height(more_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(more_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(more_row, 0, 0);
        lv_obj_set_style_pad_all(more_row, 0, 0);
        lv_obj_remove_flag(more_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(more_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(more_row, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_flex_flow(more_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(more_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        char more_buf[48];
        std::snprintf(more_buf, sizeof(more_buf), lv_tr("%d more sensors"), additional);
        lv_obj_t* more_label = lv_label_create(more_row);
        lv_label_set_text(more_label, more_buf);
        lv_obj_set_style_text_color(more_label, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_text_font(more_label, theme_manager_get_font("font_small"), 0);

        lv_obj_t* chevron = lv_label_create(more_row);
        lv_label_set_text(chevron, ui_icon::lookup_codepoint("chevron_right"));
        lv_obj_set_style_text_color(chevron, theme_manager_get_color("secondary"), 0);
        lv_obj_set_style_text_font(chevron, &mdi_icons_16, 0);

        // Click is handled by the parent container's on_secondary_temps_clicked trampoline
        // (registered once in setup()). No per-child event callback needed.
    }

    subscribe_to_secondary_temp_subjects();

    // Unhide container now that repopulation is complete
    lv_obj_remove_flag(secondary_temps_list_, LV_OBJ_FLAG_HIDDEN);

    spdlog::trace("[{}] Populated {} secondary temp sensors ({} visible, {} additional)",
                  get_name(), visible.size(), visible_count, additional);
}

void ControlsPanel::handle_secondary_temps_clicked() {
    spdlog::debug("[{}] Secondary temps overflow clicked - opening sensors overlay", get_name());
    auto& overlay = helix::settings::get_sensor_settings_overlay();
    overlay.show(parent_screen_);
}

void ControlsPanel::subscribe_to_secondary_temp_subjects() {
    using helix::ui::observe_int_sync;
    secondary_temp_observers_.reserve(secondary_temp_rows_.size());

    const uint32_t gen = temp_populate_gen_;
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    for (const auto& row : secondary_temp_rows_) {
        SubjectLifetime lifetime;
        if (auto* subject = tsm.get_temp_subject(row.klipper_name, lifetime)) {
            secondary_temp_observers_.push_back(observe_int_sync<ControlsPanel>(
                subject, this,
                [name = row.klipper_name, gen](ControlsPanel* self, int decidegrees) {
                    if (gen != self->temp_populate_gen_)
                        return; // stale callback — widgets gone
                    if (!self->active_)
                        return; // skip label update when hidden
                    self->update_secondary_temp(name, decidegrees);
                },
                lifetime));
            spdlog::trace("[{}] Subscribed to temp subject for sensor '{}'", get_name(),
                          row.klipper_name);
        }
    }

    spdlog::trace("[{}] Subscribed to {} secondary temp sensor subjects", get_name(),
                  secondary_temp_observers_.size());
}

void ControlsPanel::update_secondary_temp(const std::string& klipper_name, int decidegrees) {
    for (const auto& row : secondary_temp_rows_) {
        if (row.klipper_name == klipper_name && row.temp_label) {
            char temp_buf[16];
            helix::ui::temperature::format_temperature(
                helix::ui::temperature::deci_to_degrees(decidegrees), temp_buf, sizeof(temp_buf));
            lv_label_set_text(row.temp_label, temp_buf);
            spdlog::trace("[{}] Updated secondary temp '{}' to {}", get_name(), klipper_name,
                          temp_buf);
            break;
        }
    }
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

static std::unique_ptr<ControlsPanel> g_controls_panel;

ControlsPanel& get_global_controls_panel() {
    if (!g_controls_panel) {
        g_controls_panel = std::make_unique<ControlsPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("ControlsPanel",
                                                         []() { g_controls_panel.reset(); });
    }
    return *g_controls_panel;
}
