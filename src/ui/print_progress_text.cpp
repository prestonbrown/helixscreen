// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_progress_text.h"

#include "ui_format_utils.h"

#include "display_settings_manager.h"
#include "format_utils.h"
#include "tune_controller.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>

namespace helix::ui {

void PrintProgressText::init_subjects(SubjectManager& subjects) {
    UI_MANAGED_SUBJECT_STRING(layer_text_subject_, layer_text_buf_, "0 / 0", "print_layer_text",
                              subjects);
    UI_MANAGED_SUBJECT_STRING(filament_used_text_subject_, filament_used_text_buf_, "",
                              "print_filament_used_text", subjects);
    UI_MANAGED_SUBJECT_STRING(elapsed_subject_, elapsed_buf_, "0h 00m", "print_elapsed", subjects);
    UI_MANAGED_SUBJECT_STRING(remaining_subject_, remaining_buf_, "0h 00m", "print_remaining",
                              subjects);
    UI_MANAGED_SUBJECT_STRING(eta_subject_, eta_buf_, "", "print_eta", subjects);
}

void PrintProgressText::refresh_layer() {
    std::string text = format_layer_progress_compact(
        lifecycle_.current_layer(), lifecycle_.total_layers(),
        printer_state_.print_state().layer_is_accurate(),
        lv_subject_get_int(printer_state_.motion_state().get_gcode_position_z_subject()));
    std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), "%s", text.c_str());
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);
}

void PrintProgressText::clear_layer() {
    std::snprintf(layer_text_buf_, sizeof(layer_text_buf_), " ");
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);
}

void PrintProgressText::refresh_filament_used() {
    int filament_mm =
        lv_subject_get_int(printer_state_.print_state().get_print_filament_used_subject());
    if (filament_mm > 0) {
        std::string fil_str =
            helix::format::format_filament_length(static_cast<double>(filament_mm));
        std::strncpy(filament_used_text_buf_, fil_str.c_str(), sizeof(filament_used_text_buf_) - 1);
        filament_used_text_buf_[sizeof(filament_used_text_buf_) - 1] = '\0';
    } else {
        filament_used_text_buf_[0] = '\0';
    }
    lv_subject_copy_string(&filament_used_text_subject_, filament_used_text_buf_);
}

void PrintProgressText::refresh_speed_flow() {
    auto& motion = printer_state_.motion_state();
    const double flow = helix::tune::volumetric_flow_mm3_s(
        lv_subject_get_int(motion.get_live_extruder_velocity_subject()),
        printer_state_.get_discovery().filament_diameter_mm());
    std::snprintf(
        speed_buf_, sizeof(speed_buf_), "%s",
        helix::tune::live_rates_text(lv_subject_get_int(motion.get_live_velocity_subject()), flow)
            .c_str());
}

void PrintProgressText::show_elapsed(int seconds) {
    std::string formatted = helix::format::duration_padded(seconds);
    std::snprintf(elapsed_buf_, sizeof(elapsed_buf_), "%s", formatted.c_str());
    lv_subject_copy_string(&elapsed_subject_, elapsed_buf_);
}

void PrintProgressText::show_remaining(int seconds) {
    std::string formatted = helix::format::duration_padded(seconds);
    std::snprintf(remaining_buf_, sizeof(remaining_buf_), "%s", formatted.c_str());
    lv_subject_copy_string(&remaining_subject_, remaining_buf_);
}

void PrintProgressText::show_time_left(int seconds) {
    show_remaining(seconds);

    bool use_24h = DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_24;
    auto eta_str = helix::format::eta_clock_time(seconds, 0, use_24h);
    std::snprintf(eta_buf_, sizeof(eta_buf_), "%s", eta_str.c_str());
    lv_subject_copy_string(&eta_subject_, eta_buf_);
    spdlog::trace("[PrintProgressText] Time remaining updated: {}s, ETA: {}", seconds, eta_buf_);
}

} // namespace helix::ui
