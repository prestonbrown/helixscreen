// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_chamber_dryer_modal.h"

#include "app_globals.h"
#include "exception_policy.h"
#include "temperature_controller.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <memory>

namespace helix::ui {

std::string ChamberDryerModal::preset_label(const DryingPreset& preset, const DryerInfo& dryer) {
    const int hours = dryer.clamp_duration(preset.duration_min) / 60;
    return fmt::format("{} {}°C/{}h", preset.name,
                       static_cast<int>(dryer.clamp_temp(preset.temp_c)), hours);
}

bool ChamberDryerModal::show_owned() {
    auto* tc = get_temperature_controller();
    if (!tc || !tc->chamber_dryer().supported) {
        spdlog::warn("[ChamberDryerModal] no chamber dryer to drive");
        return false;
    }
    // A mistranslated {} must never abort through the LVGL C dispatch frame:
    // the unformatted text stands in if formatting fails.
    std::string bed_label = lv_tr("Heat the bed to {}°C");
    helix::contain_exceptions("[ChamberDryerModal] Bed label format", [&] {
        bed_label = fmt::format(fmt::runtime(lv_tr("Heat the bed to {}°C")),
                                tc->chamber_dryer_bed_assist_c());
    });
    const char* attrs[] = {"bed_label", bed_label.c_str(), nullptr};

    auto modal = std::make_unique<ChamberDryerModal>();
    if (!modal->show(lv_screen_active(), attrs)) {
        return false; // the unique_ptr frees the never-shown instance
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

void ChamberDryerModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    auto* tc = get_temperature_controller();
    lv_obj_t* dropdown = find_widget("preset_dropdown");
    if (!tc || !dropdown) {
        return;
    }
    const DryerInfo dryer = tc->chamber_dryer();
    presets_ = get_default_drying_presets();
    std::string options;
    for (const auto& preset : presets_) {
        if (!options.empty()) {
            options += '\n';
        }
        options += preset_label(preset, dryer);
    }
    lv_dropdown_set_options(dropdown, options.c_str());
}

void ChamberDryerModal::on_ok() {
    auto* tc = get_temperature_controller();
    lv_obj_t* dropdown = find_widget("preset_dropdown");
    const auto index = dropdown ? lv_dropdown_get_selected(dropdown) : 0;
    if (tc && index < presets_.size()) {
        lv_obj_t* bed_switch = find_widget("bed_assist_switch");
        const bool heat_bed = bed_switch && lv_obj_has_state(bed_switch, LV_STATE_CHECKED);
        const auto& preset = presets_[index];
        tc->start_chamber_drying(preset.temp_c, preset.duration_min, heat_bed);
    }
    hide();
}

} // namespace helix::ui
