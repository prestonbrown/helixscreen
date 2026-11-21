// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_wizard_calibration.h"

#include "ui_calibration_run_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "calibration_catalog_inputs.h"
#include "calibration_run.h"
#include "core/static_panel_registry.h"
#include "printer_state.h"
#include "system/subject_registry.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

extern helix::Subject wizard_show_skip;

WizardCalibrationStep* get_wizard_calibration_step() {
    return &helix::lazy_global<WizardCalibrationStep>("WizardCalibrationStep");
}

void WizardCalibrationStep::init_subjects() {
    using namespace helix::calibration;
    std::vector<StepInfo> offered = guided_run_steps(current_printer_steps(get_printer_state()));
    std::vector<StepInfo> picked;
    std::string names;
    for (const auto& s : offered) {
        if (picked_by_default(s)) {
            picked.push_back(s);
            names += names.empty() ? lv_tr(s.name) : std::string(", ") + lv_tr(s.name);
        }
    }
    const std::string time =
        fmt::format(fmt::runtime(lv_tr("About {} min. You are needed for the first {} min.")),
                    total_minutes(picked), hands_on_minutes(picked));
    if (steps_text_.is_live()) {
        lv_subject_set_string(&steps_text_, names.c_str());
        lv_subject_set_string(&time_text_, time.c_str());
        return;
    }
    UI_SUBJECT_INIT_AND_REGISTER_STRING(steps_text_, steps_text_buf_, names.c_str(),
                                        "wizard_calibration_steps");
    UI_SUBJECT_INIT_AND_REGISTER_STRING(time_text_, time_text_buf_, time.c_str(),
                                        "wizard_calibration_time");
}

void WizardCalibrationStep::register_callbacks() {
    register_xml_callbacks({
        {"on_wizard_calibrate_now",
         [](lv_event_t*) {
             spdlog::info("[WizardCalibration] Calibrate now: finishing setup first");
             ui_wizard_complete_then([]() { helix::ui::get_calibration_run_overlay().open(); });
         }},
    });
}

lv_obj_t* WizardCalibrationStep::create(lv_obj_t* parent) {
    init_subjects();
    // The footer's Finish reads "Skip for now": finishing here skips the run.
    lv_subject_set_int(&wizard_show_skip, 1);
    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_calibration", nullptr));
    if (!root_) {
        spdlog::error("[{}] Failed to create screen from XML", log_name());
    }
    return root_;
}

void WizardCalibrationStep::cleanup() {
    lv_subject_set_int(&wizard_show_skip, 0);
    root_ = nullptr;
}
