// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_screen_power.h"

#include "ui_settings_display.h"
#include "ui_wizard_helpers.h"

#include "core/static_panel_registry.h"

#include <spdlog/spdlog.h>

namespace helix::wizard {

using helix::settings::UiScaleOffer;

WizardScreenPowerStep* get_wizard_screen_power_step() {
    return &lazy_global<WizardScreenPowerStep>("WizardScreenPowerStep");
}

void WizardScreenPowerStep::init_subjects() {
    subjects_.deinit_all();
    const bool choice = helix::settings::fitting_ui_scale_has_choice();
    helix::ui::wizard::init_int_subject(subjects_, show_ui_scale_, choice ? 1 : 0,
                                        "wizard_screen_show_ui_scale");
}

void WizardScreenPowerStep::register_callbacks() {
    // The rows reuse the Display overlay's callbacks, which it registers only when first opened.
    helix::settings::get_display_settings_overlay().register_callbacks();
}

lv_obj_t* WizardScreenPowerStep::create(lv_obj_t* parent) {
    init_subjects();
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_screen_power", nullptr));
    if (!root) {
        spdlog::error("[{}] Failed to create screen from XML", log_name());
        return nullptr;
    }
    helix::settings::select_brightness_slider(root);
    helix::settings::populate_screensaver_row(root);
    helix::settings::select_dim_rows(root);
    helix::settings::select_sleep_row(root);
    helix::settings::populate_ui_scale_row(root, UiScaleOffer::Fitting);
    return root;
}

void WizardScreenPowerStep::cleanup() {}

} // namespace helix::wizard
