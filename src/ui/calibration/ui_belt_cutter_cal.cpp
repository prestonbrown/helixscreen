// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_belt_cutter_cal.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"

#include "app_globals.h"
#include "bed_plate_context.h"
#include "macro_executor.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <memory>
#include <string>

namespace helix::ui {

namespace cal = helix::belt_cutter_cal;

namespace {

/// Rpc ceilings. Tensioning settles and adjusts each belt over many small
/// moves; the cutter search steps the toolhead 0.1 mm at a time.
constexpr uint32_t kBeltTensionTimeoutMs = 15 * 60 * 1000;
constexpr uint32_t kCutterCalibrationTimeoutMs = 5 * 60 * 1000;

/// The dropdown's rows, in BeltAxes order.
constexpr cal::BeltAxes kAxisOptions[] = {cal::BeltAxes::Both, cal::BeltAxes::X, cal::BeltAxes::Y};

/// Owns nothing: the rows' follow-ups reach only global state.
AsyncLifetimeGuard s_row_lifetime;

void confirm_cutter_calibration() {
    modal_confirm(lv_tr(cal::cutter_entry_name()),
                  lv_tr("The printer homes X and Y if needed, then drives the toolhead into the "
                        "cutter to find its position. Unload the filament first."),
                  ModalSeverity::Warning, lv_tr("Start"), []() {
                      start_cutter_calibration(get_moonraker_api(),
                                               get_printer_state().get_discovery());
                  });
}

void on_belt_row_clicked(lv_event_t*) {
    if (!cal::can_tension_belts(get_printer_state().get_discovery())) {
        return;
    }
    ensure_bed_clear_then(s_row_lifetime.token(), []() {
        if (!BeltTensionAutoModal::show_owned()) {
            NOTIFY_ERROR(lv_tr("Failed to show confirmation dialog"));
        }
    });
}

void on_cutter_row_clicked(lv_event_t*) {
    if (!cal::can_calibrate_cutter(get_printer_state().get_discovery())) {
        return;
    }
    ensure_bed_clear_then(s_row_lifetime.token(), confirm_cutter_calibration);
}

void send(IMoonrakerAPI* api, std::string gcode, const char* log_name, const char* display_key,
          uint32_t timeout_ms) {
    MachineGcode cmd;
    cmd.gcode = std::move(gcode);
    cmd.log_name = log_name;
    cmd.display_name = lv_tr(display_key);
    cmd.timeout_ms = timeout_ms;
    // Both routines home X and Y themselves.
    start_calibration_gcode(api, cmd, /*needs_home=*/false, "[BeltCutterCal]");
}

} // namespace

bool BeltTensionAutoModal::show_owned() {
    auto modal = std::make_unique<BeltTensionAutoModal>();
    if (!modal->show(lv_screen_active())) {
        return false; // the unique_ptr frees the never-shown instance
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

void BeltTensionAutoModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    lv_obj_t* dropdown = find_widget("axes_dropdown");
    if (!dropdown) {
        return;
    }
    const std::string options =
        std::string(lv_tr("Both belts")) + "\n" + lv_tr("X belt") + "\n" + lv_tr("Y belt");
    lv_dropdown_set_options(dropdown, options.c_str());
    lv_dropdown_set_selected(dropdown, 0);
}

void BeltTensionAutoModal::on_ok() {
    lv_obj_t* dropdown = find_widget("axes_dropdown");
    const uint32_t index = dropdown ? lv_dropdown_get_selected(dropdown) : 0;
    const cal::BeltAxes axes =
        index < std::size(kAxisOptions) ? kAxisOptions[index] : cal::BeltAxes::Both;
    hide();
    start_belt_tension(get_moonraker_api(), get_printer_state().get_discovery(), axes);
}

void start_belt_tension(IMoonrakerAPI* api, const PrinterDiscovery& hw, cal::BeltAxes axes) {
    std::string gcode = cal::belt_tension_gcode(hw, axes);
    if (gcode.empty()) {
        spdlog::warn("[BeltCutterCal] No belt tensioning on this printer");
        return;
    }
    spdlog::info("[BeltCutterCal] {} belt tensioning: {}", cal::provider_name(hw), gcode);
    send(api, std::move(gcode), "belt tensioning", cal::belt_entry_name(), kBeltTensionTimeoutMs);
}

void start_cutter_calibration(IMoonrakerAPI* api, const PrinterDiscovery& hw) {
    std::string gcode = cal::cutter_calibration_gcode(hw);
    if (gcode.empty()) {
        spdlog::warn("[BeltCutterCal] No cutter calibration on this printer");
        return;
    }
    spdlog::info("[BeltCutterCal] {} cutter calibration: {}", cal::provider_name(hw), gcode);
    send(api, std::move(gcode), "cutter calibration", cal::cutter_entry_name(),
         kCutterCalibrationTimeoutMs);
}

void init_belt_cutter_cal_row_handlers() {
    register_xml_callbacks({{"on_belt_tension_auto_clicked", on_belt_row_clicked},
                            {"on_cutter_calibration_clicked", on_cutter_row_clicked}});
}

} // namespace helix::ui
