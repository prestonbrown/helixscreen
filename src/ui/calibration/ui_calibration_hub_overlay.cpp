// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_calibration_hub_overlay.h"

#include "ui_callback_helpers.h"

#include "app_globals.h"
#include "calibration_catalog_inputs.h"
#include "calibration_history_store.h"
#include "core/static_panel_registry.h"
#include "core/static_subject_registry.h"
#include "locale_formats.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <fmt/format.h>

namespace helix::ui {

TileStatus tile_status(const calibration::StepInfo& step, const calibration::HistoryEntry* last,
                       const std::string* in_config) {
    using calibration::Tier;
    if (last) {
        const auto date = calibration::parse_iso_date(last->date);
        const std::string when = date ? locale::format_localized_month_day(&*date) : last->date;
        std::string text = last->summary.empty()
                               ? fmt::format(lv_tr("Done {}"), when)
                               : fmt::format(lv_tr("Done {} · {}"), when, last->summary);
        return {std::move(text), TileTone::Done};
    }
    if (in_config) {
        return {fmt::format(lv_tr("In config · {}"), *in_config), TileTone::Idle};
    }
    switch (step.tier) {
    case Tier::Required:
        return {lv_tr("Never run · required"), TileTone::Due};
    case Tier::Recommended:
        return {lv_tr("Never run"), TileTone::Due};
    case Tier::Optional:
        return {lv_tr("Never run · optional"), TileTone::Idle};
    case Tier::OnRequest:
        break;
    }
    return {lv_tr("Run when needed"), TileTone::Idle};
}

CalibrationHubOverlay& get_calibration_hub_overlay() {
    return helix::lazy_global<CalibrationHubOverlay>("CalibrationHubOverlay");
}

void CalibrationHubOverlay::init_subjects() {
    const auto& ids = calibration::all_step_ids();
    for (size_t i = 0; i < ids.size(); ++i) {
        names_[i * 3] = std::string("cal_shown_") + ids[i];
        names_[i * 3 + 1] = std::string("cal_tone_") + ids[i];
        names_[i * 3 + 2] = std::string("cal_status_") + ids[i];
        UI_MANAGED_SUBJECT_INT(shown_[i], 0, names_[i * 3].c_str(), subjects_);
        UI_MANAGED_SUBJECT_INT(tone_[i], 0, names_[i * 3 + 1].c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(status_[i], status_bufs_[i], "", names_[i * 3 + 2].c_str(),
                                  subjects_);
    }
    // Runs from StaticSubjectRegistry::deinit_all(), which may come after
    // StaticPanelRegistry has destroyed the overlay.
    StaticSubjectRegistry::instance().register_deinit("CalibrationHubOverlay", []() {
        if (auto* self = helix::lazy_global_if_exists<CalibrationHubOverlay>()) {
            self->subjects_.deinit_all();
            self->subjects_initialized_ = false;
        }
    });
}

void CalibrationHubOverlay::before_show() {
    refresh();
}

void CalibrationHubOverlay::on_activate() {
    OverlayBase::on_activate();
    refresh();
}

void CalibrationHubOverlay::refresh() {
    const auto steps = calibration::current_printer_steps(get_printer_state());
    const auto& history = calibration::CalibrationHistory::instance();
    const auto& in_config = get_printer_state().get_discovery().calibration_in_config();
    const auto& ids = calibration::all_step_ids();
    for (size_t i = 0; i < ids.size(); ++i) {
        const calibration::StepInfo* step = nullptr;
        for (const auto& s : steps) {
            if (std::strcmp(s.id, ids[i]) == 0) {
                step = &s;
                break;
            }
        }
        lv_subject_set_int(&shown_[i], step ? 1 : 0);
        if (!step) {
            continue;
        }
        const auto found = in_config.find(step->id);
        const TileStatus status = tile_status(*step, history.entry(step->id),
                                              found != in_config.end() ? &found->second : nullptr);
        lv_subject_set_int(&tone_[i], static_cast<int>(status.tone));
        lv_subject_set_string(&status_[i], status.text.c_str());
    }
    spdlog::debug("[{}] {} calibration step(s)", get_name(), steps.size());
}

void init_calibration_hub_row_handler() {
    register_xml_callbacks({
        {"on_calibration_hub_clicked",
         [](lv_event_t*) { get_calibration_hub_overlay().show(lv_screen_active()); }},
    });
}

} // namespace helix::ui
