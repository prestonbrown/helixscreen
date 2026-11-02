// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_tune_overlay.h"

#include "ui_callback_helpers.h"
#if !defined(HELIX_PLATFORM_ESP32)
#include "ui_overlay_pa_tuning.h"
#endif
#include "ui_z_offset_indicator.h"

#include "display_numbering.h"
#include "display_settings_manager.h"
#include "filament_database.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "tool_state.h"
#include "tune_controller.h"
#include "ui/ui_widget_helpers.h"
#include "z_offset_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace helix;

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrintTuneOverlay::~PrintTuneOverlay() {
    // Clean up observers before subjects
    speed_observer_.reset();
    live_velocity_observer_.reset();
    max_velocity_observer_.reset();
    extruder_vel_observer_.reset();
    units_observer_.reset();
    pa_observer_.reset();

    // Clean up subjects
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    spdlog::trace("[PrintTuneOverlay] Destroyed");
}

// ============================================================================
// SHOW / CREATE
// ============================================================================

void PrintTuneOverlay::show(lv_obj_t* parent_screen, IMoonrakerAPI* api,
                            PrinterState& printer_state) {
    api_ = api;
    printer_state_ = &printer_state;
    OverlayBase::show(parent_screen);
}

lv_obj_t* PrintTuneOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    setup_panel();
    return overlay_root_;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void PrintTuneOverlay::init_subjects() {
    // Initialize tune panel subjects
    UI_MANAGED_SUBJECT_STRING(tune_speed_subject_, tune_speed_buf_, "100%", "tune_speed_display",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_flow_subject_, tune_flow_buf_, "100%", "tune_flow_display",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_z_offset_subject_, tune_z_offset_buf_, "0.000mm",
                              "tune_z_offset_display", subjects_);

    // Actual speed/flow display subjects (read-only, updated by observers)
    UI_MANAGED_SUBJECT_STRING(tune_actual_speed_subject_, tune_actual_speed_buf_, "",
                              "tune_actual_speed_display", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_actual_flow_subject_, tune_actual_flow_buf_, "",
                              "tune_actual_flow_display", subjects_);

    UI_MANAGED_SUBJECT_STRING(tune_pa_subject_, tune_pa_buf_, "--", "tune_pa_display", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_pa_session_subject_, 0, "tune_pa_session_active", subjects_);

    // Z-offset direction button icons (kinematic-aware, like motion panel)
    UI_MANAGED_SUBJECT_STRING(z_closer_icon_subject_, z_closer_icon_buf_, "arrow_down",
                              "tune_z_closer_icon", subjects_);
    UI_MANAGED_SUBJECT_STRING(z_farther_icon_subject_, z_farther_icon_buf_, "arrow_up",
                              "tune_z_farther_icon", subjects_);

    // Which z-step amount is selected. Seeded from the last-persisted choice
    // rather than a hardcoded default so the panel reopens on the step the
    // user left it on.
    selected_z_step_idx_ = helix::zoffset::persisted_step_index();
    UI_MANAGED_SUBJECT_INT(z_step_selected_subject_, selected_z_step_idx_, "z_step_selected_idx",
                           subjects_);

    // Which z-offset the direction buttons drive. Always starts machine-wide:
    // that is the only target every printer has, and it is what the panel meant
    // before a tool changer was in the picture.
    UI_MANAGED_SUBJECT_INT(z_tune_target_subject_, 0, "z_tune_target", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_z_other_subject_, tune_z_other_buf_, "", "tune_z_other_display",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_z_tool_label_subject_, tune_z_tool_label_buf_, "T0",
                              "tune_z_tool_label", subjects_);
}

void PrintTuneOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_tune_speed_adjust",
         [](lv_event_t* e) {
             if (auto delta = helix::ui::event_user_int(e))
                 get_print_tune_overlay().handle_speed_adjust(*delta);
         }},
        {"on_tune_flow_adjust",
         [](lv_event_t* e) {
             if (auto delta = helix::ui::event_user_int(e))
                 get_print_tune_overlay().handle_flow_adjust(*delta);
         }},
        {"on_tune_reset_clicked", [](lv_event_t*) { get_print_tune_overlay().handle_reset(); }},
        {"on_tune_pa_adjust",
         [](lv_event_t* e) {
             if (auto steps = helix::ui::event_user_int(e))
                 get_print_tune_overlay().handle_pa_adjust(*steps);
         }},
        {"on_tune_open_pa_tuner",
         [](lv_event_t*) { get_print_tune_overlay().handle_open_pa_tuner(); }},
        {"on_tune_units_toggle",
         [](lv_event_t*) {
             auto& settings = helix::DisplaySettingsManager::instance();
             settings.set_speed_flow_physical_units(!settings.get_speed_flow_physical_units());
         }},
        // Z-offset step amount selector (user_data = index "0"-"3")
        {"on_tune_z_step",
         [](lv_event_t* e) {
             if (auto idx = helix::ui::event_user_int(e))
                 get_print_tune_overlay().handle_z_step_select(*idx);
         }},
        {"on_tune_z_target",
         [](lv_event_t* e) {
             if (auto target = helix::ui::event_user_int(e))
                 get_print_tune_overlay().handle_z_target_select(*target);
         }},
        // Z-offset direction adjust (user_data = "-1" closer or "1" farther)
        {"on_tune_z_adjust",
         [](lv_event_t* e) {
             if (auto dir = helix::ui::event_user_int(e))
                 get_print_tune_overlay().handle_z_adjust(*dir);
         }},
    });
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void PrintTuneOverlay::on_activate() {
    OverlayBase::on_activate();
    sync_to_state();
#if !defined(HELIX_PLATFORM_ESP32)
    lv_subject_set_int(&tune_pa_session_subject_,
                       helix::ui::get_global_pa_tuning_overlay().session_active() ? 1 : 0);
#endif
    spdlog::debug("[PrintTuneOverlay] Activated - synced to state");
}

void PrintTuneOverlay::on_deactivating(DeactivateReason) {
    spdlog::debug("[PrintTuneOverlay] Deactivated");
}

void PrintTuneOverlay::setup_panel() {
    // Update Z-offset icons based on printer kinematics
    update_z_offset_icons(overlay_root_);

    // Observe speed-related subjects for the live speed/flow readouts
    if (printer_state_) {
        speed_observer_ = helix::ui::observe<int>(
            printer_state_->motion_state().get_speed_factor_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_display(); },
            printer_state_->get_subjects_lifetime());
        live_velocity_observer_ = helix::ui::observe<int>(
            printer_state_->motion_state().get_live_velocity_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_display(); },
            printer_state_->get_subjects_lifetime());
        max_velocity_observer_ = helix::ui::observe<int>(
            printer_state_->motion_state().get_max_velocity_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_display(); },
            printer_state_->get_subjects_lifetime());
        extruder_vel_observer_ = helix::ui::observe<int>(
            printer_state_->motion_state().get_live_extruder_velocity_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_display(); },
            printer_state_->get_subjects_lifetime());
        pa_observer_ = helix::ui::observe<int>(
            printer_state_->temperature_state().get_active_extruder_pressure_advance_subject(),
            this, [](PrintTuneOverlay* self, int /*value*/) { self->update_pa_display(); },
            printer_state_->get_subjects_lifetime());
    }
    auto& display_settings = DisplaySettingsManager::instance();
    units_observer_ = helix::ui::observe<int>(
        display_settings.subject_speed_flow_physical_units(), this,
        [](PrintTuneOverlay* self, int /*value*/) { self->update_display(); },
        display_settings.get_subjects_lifetime());

    // Per-tool z-offset. ToolState's lifetime is passed on every one of these:
    // it is a singleton whose deinit_subjects() frees the observer nodes, and a
    // guard without the token would call lv_observer_remove() on freed memory
    // (CLAUDE.md rule 4).
    {
        auto& ts = helix::ToolState::instance();
        const auto ts_lifetime = ts.get_subjects_lifetime();
        // The offsets live in tools_, so tools_version_, bumped on every
        // change to them, is the re-render trigger for the per-tool Z
        // readouts.
        tools_version_observer_ = helix::ui::observe<int>(
            ts.get_tools_version_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_tool_z_displays(); },
            ts_lifetime);
        active_tool_observer_ = helix::ui::observe<int>(
            ts.get_active_tool_subject(), this,
            [](PrintTuneOverlay* self, int /*value*/) { self->update_tool_z_displays(); },
            ts_lifetime);
    }

    spdlog::debug("[PrintTuneOverlay] Panel setup complete");
}

void PrintTuneOverlay::sync_to_state() {
    if (!printer_state_) {
        return;
    }

    // Get current values from PrinterState
    int speed = lv_subject_get_int(printer_state_->motion_state().get_speed_factor_subject());
    int flow = lv_subject_get_int(printer_state_->motion_state().get_flow_factor_subject());

    // Update our cached values and displays
    speed_percent_ = speed;
    flow_percent_ = flow;
    update_display();

    // Sync Z offset from PrinterState. Firmware that persists the offset itself
    // (ZMOD) zeroes the live one outside a print, so adjusting from it while idle
    // would baby-step away from a phantom zero.
    int z_offset_microns = helix::zoffset::displayed_z_offset_microns(*printer_state_);
    update_z_offset_display(z_offset_microns);
    // Opening the overlay starts a new tuning session; travel is bounded from here.
    session_base_z_offset_ = current_z_offset_;

    // Sync the visual indicator
    if (lv_obj_t* indicator =
            helix::ui::find_required(overlay_root_, "z_offset_indicator", get_name())) {
        ui_z_offset_indicator_set_value(indicator, z_offset_microns);
    }

    update_pa_display();

    spdlog::debug("[PrintTuneOverlay] Synced to state: speed={}%, flow={}%", speed, flow);
}

// ============================================================================
// ICON UPDATES
// ============================================================================

void PrintTuneOverlay::update_z_offset_icons(lv_obj_t* /*panel*/) {
    if (!printer_state_) {
        spdlog::warn("[PrintTuneOverlay] Cannot update icons - no printer_state_");
        return;
    }

    // Get kinematics type from PrinterState
    // 0 = unknown, 1 = bed moves Z (CoreXY), 2 = head moves Z (Cartesian/Delta)
    int kin =
        lv_subject_get_int(printer_state_->capabilities_state().subject(Capability::BedMoves));
    bool bed_moves_z = (kin == 1);

    // Set icon names via string subjects (bind_icon in XML)
    // Closer = more squish. On bed-moves-Z (CoreXY): bed rises (expand_up).
    // On head-moves-Z (Cartesian): head descends (arrow_down).
    const char* closer_icon = bed_moves_z ? "arrow_expand_up" : "arrow_down";
    const char* farther_icon = bed_moves_z ? "arrow_expand_down" : "arrow_up";

    std::strncpy(z_closer_icon_buf_, closer_icon, sizeof(z_closer_icon_buf_) - 1);
    lv_subject_copy_string(&z_closer_icon_subject_, z_closer_icon_buf_);
    std::strncpy(z_farther_icon_buf_, farther_icon, sizeof(z_farther_icon_buf_) - 1);
    lv_subject_copy_string(&z_farther_icon_subject_, z_farther_icon_buf_);

    spdlog::debug("[PrintTuneOverlay] Z-offset icons: closer={}, farther={}", closer_icon,
                  farther_icon);
}

// ============================================================================
// DISPLAY UPDATES
// ============================================================================

void PrintTuneOverlay::update_display() {
    if (!subjects_initialized_) {
        return;
    }

    int live_velocity = 0;
    int max_velocity = 0;
    int vel_centimm = 0;
    float diameter_mm = filament::kDefaultDiameterMm;
    if (printer_state_) {
        diameter_mm = printer_state_->get_discovery().filament_diameter_mm();
        live_velocity =
            lv_subject_get_int(printer_state_->motion_state().get_live_velocity_subject());
        max_velocity =
            lv_subject_get_int(printer_state_->motion_state().get_max_velocity_subject());
        vel_centimm =
            lv_subject_get_int(printer_state_->motion_state().get_live_extruder_velocity_subject());
    }

    const auto percent = helix::tune::status_speed_flow_text(false, speed_percent_, flow_percent_,
                                                             0, 0, diameter_mm);
    const auto live = helix::tune::status_speed_flow_text(true, speed_percent_, flow_percent_,
                                                          live_velocity, vel_centimm, diameter_mm);

    // The same preference Print Status shows decides which of each pair leads.
    // As the secondary readout, live speed carries the machine limit and an
    // idle extruder's flow is left blank rather than showing 0.0.
    if (DisplaySettingsManager::instance().get_speed_flow_physical_units()) {
        std::snprintf(tune_speed_buf_, sizeof(tune_speed_buf_), "%s", live.speed.c_str());
        std::snprintf(tune_flow_buf_, sizeof(tune_flow_buf_), "%s", live.flow.c_str());
        std::snprintf(tune_actual_speed_buf_, sizeof(tune_actual_speed_buf_), "%s",
                      percent.speed.c_str());
        std::snprintf(tune_actual_flow_buf_, sizeof(tune_actual_flow_buf_), "%s",
                      percent.flow.c_str());
    } else {
        std::snprintf(tune_speed_buf_, sizeof(tune_speed_buf_), "%s", percent.speed.c_str());
        std::snprintf(tune_flow_buf_, sizeof(tune_flow_buf_), "%s", percent.flow.c_str());
        if (max_velocity > 0) {
            std::snprintf(tune_actual_speed_buf_, sizeof(tune_actual_speed_buf_), "%d / %d mm/s",
                          live_velocity, max_velocity);
        } else {
            std::snprintf(tune_actual_speed_buf_, sizeof(tune_actual_speed_buf_), "%s",
                          live.speed.c_str());
        }
        if (helix::tune::volumetric_flow_mm3_s(vel_centimm, diameter_mm) >= 0.1) {
            std::snprintf(tune_actual_flow_buf_, sizeof(tune_actual_flow_buf_), "%s",
                          live.flow.c_str());
        } else {
            tune_actual_flow_buf_[0] = '\0';
        }
    }
    lv_subject_copy_string(&tune_speed_subject_, tune_speed_buf_);
    lv_subject_copy_string(&tune_flow_subject_, tune_flow_buf_);
    lv_subject_copy_string(&tune_actual_speed_subject_, tune_actual_speed_buf_);
    lv_subject_copy_string(&tune_actual_flow_subject_, tune_actual_flow_buf_);
}

void PrintTuneOverlay::update_speed_flow_display(int speed_percent, int flow_percent) {
    speed_percent_ = speed_percent;
    flow_percent_ = flow_percent;

    if (subjects_initialized_) {
        update_display();
    }
}

void PrintTuneOverlay::update_z_offset_display(int microns) {
    // Update display from PrinterState (microns -> mm). Deliberately does NOT
    // re-baseline the travel guard: PrintStatusPanel calls this from a
    // gcode_z_offset observer, which our own adjust fires, so re-baselining
    // here would reset the guard after every single step.
    current_z_offset_ = microns / 1000.0;

    if (subjects_initialized_) {
        // Not written directly: with the tool target selected the heading
        // belongs to the tool, and clobbering it here would put the
        // machine-wide value back under a T<n> label.
        update_tool_z_displays();
    }

    spdlog::trace("[PrintTuneOverlay] Z-offset display updated: {}um ({}mm)", microns,
                  current_z_offset_);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void PrintTuneOverlay::handle_speed_adjust(int delta) {
    speed_percent_ = helix::tune::clamp_speed_percent(speed_percent_ + delta);
    update_display();
    helix::tune::set_speed_percent(api_, speed_percent_);
}

void PrintTuneOverlay::handle_flow_adjust(int delta) {
    flow_percent_ = helix::tune::clamp_flow_percent(flow_percent_ + delta);
    update_display();
    helix::tune::set_flow_percent(api_, flow_percent_);
}

void PrintTuneOverlay::update_pa_display() {
    if (!subjects_initialized_ || !printer_state_) {
        return;
    }
    const int value = lv_subject_get_int(
        printer_state_->temperature_state().get_active_extruder_pressure_advance_subject());
    lv_strlcpy(tune_pa_buf_, helix::tune::pressure_advance_text(value).c_str(),
               sizeof(tune_pa_buf_));
    lv_subject_copy_string(&tune_pa_subject_, tune_pa_buf_);
}

void PrintTuneOverlay::handle_pa_adjust(int steps) {
    if (!printer_state_) {
        return;
    }
#if !defined(HELIX_PLATFORM_ESP32)
    // During a tuning run the nudge is the tuner's: it stops the ramp, which
    // would otherwise overwrite this value at the next band.
    if (helix::ui::get_global_pa_tuning_overlay().nudge_by(steps * helix::tune::kPaTuneStep)) {
        return;
    }
#endif
    auto& temps = printer_state_->temperature_state();
    const auto current = helix::tune::pressure_advance_from_subject(
        lv_subject_get_int(temps.get_active_extruder_pressure_advance_subject()));
    if (!current) {
        return; // nothing reported yet: a nudge would be relative to a guess
    }
    // The readout follows the printer's report, not this request.
    helix::tune::set_pressure_advance(api_, temps.active_extruder_name(),
                                      *current + steps * helix::tune::kPaTuneStep);
}

void PrintTuneOverlay::handle_open_pa_tuner() {
#if !defined(HELIX_PLATFORM_ESP32)
    auto& tuner = helix::ui::get_global_pa_tuning_overlay();
    if (tuner.session_active()) {
        tuner.set_api(api_);
        tuner.show(parent_screen_);
    }
#endif
}

void PrintTuneOverlay::handle_reset() {
    speed_percent_ = 100;
    flow_percent_ = 100;
    update_display();
    helix::tune::set_speed_percent(api_, 100);
    helix::tune::set_flow_percent(api_, 100);
}

void PrintTuneOverlay::handle_z_offset_changed(double delta) {
    const auto r = helix::zoffset::adjust(api_, printer_state_, session_base_z_offset_,
                                          current_z_offset_, delta);
    if (r.clamped_to_noop) {
        return; // already at the session-travel limit
    }
    current_z_offset_ = r.new_offset_mm;
    update_tool_z_displays();

    if (lv_obj_t* indicator =
            helix::ui::find_required(overlay_root_, "z_offset_indicator", get_name())) {
        ui_z_offset_indicator_set_value(indicator,
                                        static_cast<int>(std::lround(current_z_offset_ * 1000.0)));
        ui_z_offset_indicator_flash_direction(indicator, r.applied_delta_mm > 0 ? 1 : -1);
    }
}

void PrintTuneOverlay::handle_z_step_select(int idx) {
    if (idx < 0 || idx >= static_cast<int>(std::size(helix::zoffset::kZStepAmountsMm))) {
        spdlog::warn("[PrintTuneOverlay] Invalid step index: {}", idx);
        return;
    }
    selected_z_step_idx_ = idx;
    helix::zoffset::set_persisted_step_index(idx);
    lv_subject_set_int(&z_step_selected_subject_, idx);

    spdlog::debug("[PrintTuneOverlay] Z-offset step selected: {}mm",
                  helix::zoffset::kZStepAmountsMm[idx]);
}

void PrintTuneOverlay::handle_z_target_select(int target) {
    if (target != 0 && target != 1) {
        spdlog::warn("[PrintTuneOverlay] Invalid z-offset target: {}", target);
        return;
    }
    // Deliberately NOT persisted, unlike the step amount. The tool target is
    // only meaningful while a specific tool is mounted, and reopening the panel
    // pointed at a tool the user has since swapped away from would aim the
    // buttons somewhere they are not looking.
    lv_subject_set_int(&z_tune_target_subject_, target);
    update_tool_z_displays();
    spdlog::debug("[PrintTuneOverlay] Z-offset target: {}", target == 0 ? "machine" : "tool");
}

void PrintTuneOverlay::update_tool_z_displays() {
    if (!subjects_initialized_) {
        return;
    }
    auto& ts = helix::ToolState::instance();
    const int tool_index = lv_subject_get_int(ts.get_active_tool_subject());
    const bool per_tool =
        lv_subject_get_int(ts.get_per_tool_axis_supported_subject(helix::Axis::Z)) == 1;
    const bool tool_known = ts.tool_offset_known(tool_index, helix::Axis::Z);

    // A per-tool Z offset belongs to a physical toolhead, so the label is the
    // tool's physical position, not its gcode identity.
    const auto* active_tool_info = ts.active_tool();
    const std::string tool_display_label =
        active_tool_info ? active_tool_info->display_label
                         : helix::ui::lane_label(helix::ui::active_tool_noun(), tool_index);
    std::snprintf(tune_z_tool_label_buf_, sizeof(tune_z_tool_label_buf_), "%s",
                  tool_display_label.c_str());
    lv_subject_copy_string(&tune_z_tool_label_subject_, tune_z_tool_label_buf_);

    char global_mm[16];
    helix::format::format_distance_mm(current_z_offset_, 3, global_mm, sizeof(global_mm));

    char tool_mm[16] = {};
    if (tool_known) {
        helix::format::format_distance_mm(ts.tool_offset_mm(tool_index, helix::Axis::Z), 3, tool_mm,
                                          sizeof(tool_mm));
    }

    // The HEADING carries whichever target the buttons drive. Showing the
    // machine-wide value there while the user is adjusting a tool was the whole
    // bug: the tool's own number appeared nowhere on screen, so a press moved a
    // value the panel never displayed.
    const bool on_tool = per_tool && lv_subject_get_int(&z_tune_target_subject_) == 1;
    if (on_tool) {
        // "--" rather than 0.000: nothing has been reported for this tool, and
        // 0 is a legitimate offset we must not fake.
        lv_strlcpy(tune_z_offset_buf_, tool_known ? tool_mm : "--", sizeof(tune_z_offset_buf_));
    } else {
        lv_strlcpy(tune_z_offset_buf_, global_mm, sizeof(tune_z_offset_buf_));
    }
    lv_subject_copy_string(&tune_z_offset_subject_, tune_z_offset_buf_);

    // The muted label carries the target the buttons are NOT on, so both
    // numbers are readable at once. Empty on a printer with only one of them.
    if (!per_tool) {
        tune_z_other_buf_[0] = '\0';
    } else if (on_tool) {
        std::snprintf(tune_z_other_buf_, sizeof(tune_z_other_buf_), "%s %s", lv_tr("Global"),
                      global_mm);
    } else if (tool_known) {
        std::snprintf(tune_z_other_buf_, sizeof(tune_z_other_buf_), "%s %s",
                      tool_display_label.c_str(), tool_mm);
    } else {
        tune_z_other_buf_[0] = '\0';
    }
    lv_subject_copy_string(&tune_z_other_subject_, tune_z_other_buf_);
}

void PrintTuneOverlay::handle_z_adjust(int direction) {
    const double amount = helix::zoffset::kZStepAmountsMm[selected_z_step_idx_];
    if (lv_subject_get_int(&z_tune_target_subject_) == 1) {
        handle_tool_z_offset_changed(direction * amount);
        return;
    }
    handle_z_offset_changed(direction * amount);
}

void PrintTuneOverlay::handle_tool_z_offset_changed(double delta) {
    const int tool_index =
        lv_subject_get_int(helix::ToolState::instance().get_active_tool_subject());
    if (!helix::zoffset::adjust_tool_offset(api_, printer_state_, tool_index, delta)) {
        return;
    }
    update_tool_z_displays();

    if (lv_obj_t* indicator =
            helix::ui::find_required(overlay_root_, "z_offset_indicator", get_name())) {
        ui_z_offset_indicator_flash_direction(indicator, delta > 0 ? 1 : -1);
    }
}
