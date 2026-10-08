// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_preferences.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_settings_appearance.h"
#include "ui_settings_display.h"
#include "ui_settings_printing.h"
#include "ui_settings_sound.h"
#include "ui_wizard_helpers.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "observer_factory.h"
#include "sound_manager.h"
#include "static_panel_registry.h"
#include "wizard_step_registry.h"

#include <spdlog/spdlog.h>

namespace helix::wizard {

PreferenceRows preference_rows(bool is_subsequent_printer, bool bypass_is_virtual,
                               bool reports_spool_ids, bool retains_spool_info,
                               bool has_sound_backend) {
    PreferenceRows r;
    r.global = !is_subsequent_printer;
    r.bypass_spool = bypass_is_virtual;
    // Firmware that keeps the spool itself leaves the toggle with nothing to do.
    r.keep_spool_info = reports_spool_ids && !retains_spool_info;
    r.sounds = !is_subsequent_printer && has_sound_backend;
    return r;
}

WizardPreferencesStep* get_wizard_preferences_step() {
    return &lazy_global<WizardPreferencesStep>("WizardPreferencesStep");
}

void WizardPreferencesStep::init_subjects() {
    subjects_.deinit_all();

    helix::ui::wizard::init_int_subject(subjects_, &show_global_, 0, "wizard_prefs_show_global");
    helix::ui::wizard::init_int_subject(subjects_, &show_bypass_spool_, 0,
                                        "wizard_prefs_show_bypass_spool");
    helix::ui::wizard::init_int_subject(subjects_, &show_keep_spool_info_, 0,
                                        "wizard_prefs_show_keep_spool_info");
    helix::ui::wizard::init_int_subject(subjects_, &show_sounds_, 0, "wizard_prefs_show_sounds");
    update_rows();
}

void WizardPreferencesStep::update_rows() {
    const auto ctx = build_context();
    AmsBackend* backend = AmsState::instance().get_backend();
    const auto rows = preference_rows(
        ctx.is_subsequent_printer, backend && backend->bypass_is_virtual(),
        backend && backend->printer_reports_spool_ids(),
        backend && backend->printer_retains_spool_info(), SoundManager::instance().has_backend());
    lv_subject_set_int(&show_global_, rows.global ? 1 : 0);
    lv_subject_set_int(&show_bypass_spool_, rows.bypass_spool ? 1 : 0);
    lv_subject_set_int(&show_keep_spool_info_, rows.keep_spool_info ? 1 : 0);
    lv_subject_set_int(&show_sounds_, rows.sounds ? 1 : 0);
}

void WizardPreferencesStep::register_callbacks() {
    // The rows reuse their overlay twins' callbacks, which a settings overlay
    // registers only when first opened.
    helix::settings::get_printing_settings_overlay().register_callbacks();
    helix::settings::get_appearance_settings_overlay().register_callbacks();
    helix::settings::get_display_settings_overlay().register_callbacks();
    helix::settings::get_sound_settings_overlay().register_callbacks();
    helix::ui::get_ams_device_operations_overlay().register_callbacks();
}

lv_obj_t* WizardPreferencesStep::create(lv_obj_t* parent) {
    init_subjects();
    // AMS discovery can finish after this page is up, so the AMS rows follow the backend.
    auto& ams = AmsState::instance();
    auto refresh = [](WizardPreferencesStep* self, int) { self->update_rows(); };
    backend_count_observer_ = helix::ui::observe<int>(ams.get_backend_count_subject(), this,
                                                      refresh, ams.get_subjects_lifetime());
    ams_type_observer_ = helix::ui::observe<int>(ams.get_ams_type_subject(), this, refresh,
                                                 ams.get_subjects_lifetime());
    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_preferences", nullptr));
    if (!root_) {
        spdlog::error("[{}] Failed to create screen from XML", log_name());
    }
    return root_;
}

void WizardPreferencesStep::cleanup() {
    backend_count_observer_.reset();
    ams_type_observer_.reset();
    root_ = nullptr;
}

} // namespace helix::wizard
