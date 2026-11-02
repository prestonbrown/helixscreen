// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_stepper_resonance_cal_modal.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"

#include "app_globals.h"
#include "display_numbering.h"
#include "macro_executor.h"
#include "printer_state.h"
#include "stepper_resonance_calibration.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <memory>
#include <string>

namespace helix::ui {

namespace cal = helix::stepper_resonance_cal;

namespace {

/// Rpc ceiling: the sweep makes a few hundred moves per motor, and a per-tool
/// routine adds a tool change. A restarting routine's expiry is absorbed as
/// its restart (classify_macro_rpc_failure).
constexpr uint32_t kCalibrationTimeoutMs = 20 * 60 * 1000;

std::string confirm_message(const cal::Procedure& procedure) {
    std::string message = lv_tr("The printer moves the toolhead and measures its motors.");
    if (procedure.restarts_klipper) {
        message += " ";
        message += lv_tr("It saves the result and restarts Klipper when it finishes.");
    }
    return message;
}

void on_row_clicked(lv_event_t*) {
    const cal::Procedure procedure = cal::resolve(get_printer_state().get_discovery());
    if (!procedure.present()) {
        return;
    }
    const std::string message = confirm_message(procedure);
    const size_t tools = helix::ToolState::instance().tools().size();
    if (procedure.per_tool && tools > 1) {
        if (!StepperResonanceCalModal::show_owned(tools, message.c_str())) {
            NOTIFY_ERROR(lv_tr("Failed to show confirmation dialog"));
        }
        return;
    }
    modal_confirm(lv_tr(cal::entry_name()), message.c_str(), ModalSeverity::Warning, lv_tr("Start"),
                  []() { start_stepper_resonance_cal(0); });
}

} // namespace

bool StepperResonanceCalModal::show_owned(size_t tool_count, const char* message) {
    const char* attrs[] = {"message", message, nullptr};
    auto modal = std::make_unique<StepperResonanceCalModal>(tool_count);
    if (!modal->show(lv_screen_active(), attrs)) {
        return false; // the unique_ptr frees the never-shown instance
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

void StepperResonanceCalModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    lv_obj_t* dropdown = find_widget("tool_dropdown");
    if (!dropdown) {
        return;
    }
    std::string options;
    for (size_t i = 0; i < tool_count_; ++i) {
        if (i > 0) {
            options += "\n";
        }
        options += tool_label(static_cast<int>(i));
    }
    lv_dropdown_set_options(dropdown, options.c_str());
    lv_dropdown_set_selected(dropdown, 0);
}

void StepperResonanceCalModal::on_ok() {
    lv_obj_t* dropdown = find_widget("tool_dropdown");
    const int tool = dropdown ? static_cast<int>(lv_dropdown_get_selected(dropdown)) : 0;
    hide();
    start_stepper_resonance_cal(tool);
}

void start_stepper_resonance_cal(int tool) {
    const PrinterDiscovery& hw = get_printer_state().get_discovery();
    const cal::Procedure procedure = cal::resolve(hw);
    MachineGcode cmd;
    cmd.gcode = cal::calibrate_gcode(hw, tool);
    if (cmd.gcode.empty()) {
        spdlog::warn("[StepperResonanceCal] Nothing to send for tool {}", tool);
        return;
    }
    cmd.log_name = procedure.name;
    cmd.display_name = lv_tr(cal::entry_name());
    cmd.host_effect =
        procedure.restarts_klipper ? MacroHostEffect::Restarts : MacroHostEffect::None;
    cmd.timeout_ms = kCalibrationTimeoutMs;
    start_calibration_gcode(get_moonraker_api(), cmd, procedure.needs_home,
                            "[StepperResonanceCal]");
}

void init_stepper_resonance_cal_row_handler() {
    register_xml_callbacks({{"on_stepper_resonance_cal_clicked", on_row_clicked}});
}

} // namespace helix::ui
