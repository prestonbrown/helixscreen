// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_appearance.h"

#include "ui_callback_helpers.h"
#include "ui_settings_appearance.h"
#include "ui_settings_language_time.h"
#include "ui_wizard_helpers.h"

#include "config.h"
#include "core/static_panel_registry.h"
#include "system_settings_manager.h"
#include "theme_loader.h"
#include "theme_manager.h"
#include "timezone_env.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::wizard {

namespace {
constexpr const char* kTimeFormatKey = "/display/time_format";
} // namespace

TimeFormat default_time_format_for_language(const std::string& language) {
    return (language.empty() || language == "en") ? TimeFormat::HOUR_12 : TimeFormat::HOUR_24;
}

bool timezone_needs_attention(bool follows_system, bool system_zone_known) {
    return follows_system && !system_zone_known;
}

WizardAppearanceStep* get_wizard_appearance_step() {
    return &lazy_global<WizardAppearanceStep>("WizardAppearanceStep");
}

void WizardAppearanceStep::init_subjects() {
    subjects_.deinit_all();
    const bool attention =
        timezone_needs_attention(DisplaySettingsManager::instance().get_timezone().empty(),
                                 helix::timezone_env::system_zone_known());
    helix::ui::wizard::init_int_subject(subjects_, &tz_attention_, attention ? 1 : 0,
                                        "wizard_appearance_tz_attention");
}

void WizardAppearanceStep::apply_theme_by_index(int index) {
    const auto themes = helix::discover_themes(helix::get_themes_directory());
    if (index < 0 || index >= static_cast<int>(themes.size())) {
        spdlog::warn("[WizardAppearance] Theme index {} out of range ({} themes)", index,
                     themes.size());
        return;
    }
    helix::ThemeData theme = helix::load_theme_from_file(themes[index].filename);
    if (!theme.is_valid()) {
        spdlog::error("[WizardAppearance] Failed to load theme '{}'", themes[index].filename);
        return;
    }
    auto& display = DisplaySettingsManager::instance();
    display.set_theme_by_index(index);
    theme_manager_apply_theme(theme, display.get_dark_mode());
    display.on_theme_changed();
    spdlog::info("[WizardAppearance] Theme set to '{}'", themes[index].filename);
}

void WizardAppearanceStep::register_callbacks() {
    // The rows reuse their overlay twins' callbacks, which a settings overlay
    // registers only when first opened.
    helix::settings::get_language_time_settings_overlay().register_callbacks();
    helix::settings::get_appearance_settings_overlay().register_callbacks();
    register_xml_callbacks({
        {"on_wizard_theme_changed",
         [](lv_event_t* e) { apply_theme_by_index(helix::ui::event_selected(e)); }},
    });
}

lv_obj_t* WizardAppearanceStep::create(lv_obj_t* parent) {
    init_subjects();

    // An unpicked clock format takes the chosen language's, persisted so
    // Settings shows the same value.
    Config* config = Config::get_instance();
    if (config && !config->exists(kTimeFormatKey)) {
        DisplaySettingsManager::instance().set_time_format(
            default_time_format_for_language(SystemSettingsManager::instance().get_language()));
    }

    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_appearance", nullptr));
    if (!root_) {
        spdlog::error("[{}] Failed to create screen from XML", log_name());
        return nullptr;
    }

    helix::settings::populate_timezone_row(
        helix::ui::find_required(root_, "row_timezone", log_name()), log_name());

    lv_obj_t* theme_row = helix::ui::find_required(root_, "row_theme", log_name());
    if (lv_obj_t* dropdown =
            theme_row ? helix::ui::find_required(theme_row, "dropdown", log_name()) : nullptr) {
        auto& display = DisplaySettingsManager::instance();
        lv_dropdown_set_options(dropdown, display.get_theme_options().c_str());
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(display.get_theme_index()));
    }
    return root_;
}

void WizardAppearanceStep::cleanup() {
    root_ = nullptr;
}

} // namespace helix::wizard
