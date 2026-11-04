// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config_modal.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_icon_picker.h"
#include "ui_picker_option_row.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "favorite_macro_config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "macro_param_defaults.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "printer_discovery.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"
#include "tile_color.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

namespace helix {

namespace {
lv_subject_t s_tab_subject{};
lv_subject_t s_require_confirm_subject{};
/// 1 while the configured macro takes parameters (declared or free-form), so
/// the Options tab shows the "Default Parameters" row only for those.
lv_subject_t s_has_param_defaults_subject{};
bool s_subjects_registered = false;
SubjectManager s_subjects;

// Curated icon list for the picker grid (matches FavoriteMacroWidget).
static const char* const kCuratedIcons[] = {
    "play",        "pause",     "stop",  "refresh",     "home",
    "cog",         "wrench",    "fan",   "thermometer", "lightbulb_outline",
    "power",       "bell",      "flash", "water",       "fire",
    "printer_3d",  "check",     "bed",   "filament",    "cooldown",
    "script_text", "hourglass", "speed", "arrow_up",    "arrow_down",
};
static constexpr size_t kCuratedIconCount = std::size(kCuratedIcons);

} // namespace

FavoriteMacroConfigModal* FavoriteMacroConfigModal::s_active_ = nullptr;

void register_favorite_macro_config_subjects() {
    if (s_subjects_registered)
        return;
    lv_subject_init_int(&s_tab_subject, 0);
    // Default 1 mirrors FavoriteMacroConfig::require_confirmation, so the toggle
    // reads correctly even before on_show() loads a widget's config.
    lv_subject_init_int(&s_require_confirm_subject, 1);
    lv_subject_init_int(&s_has_param_defaults_subject, 0);
    s_subjects.publish("fav_macro_config_tab", &s_tab_subject);
    s_subjects.publish("fav_macro_require_confirm", &s_require_confirm_subject);
    s_subjects.publish("fav_macro_has_param_defaults", &s_has_param_defaults_subject);
    // Registered once with the widget factories, so it outlives a printer switch.
    StaticSubjectRegistry::instance().register_deinit(
        "FavoriteMacroConfigSubjects",
        []() {
            s_subjects.deinit_all();
            s_subjects_registered = false;
        },
        StaticSubjectRegistry::Lifetime::Process);
    tile_color::register_subjects();
    s_subjects_registered = true;
}

FavoriteMacroConfigModal::FavoriteMacroConfigModal(const std::string& widget_id,
                                                   const std::string& panel_id,
                                                   ChangeCallback on_change)
    : widget_id_(widget_id), panel_id_(panel_id), on_change_(std::move(on_change)) {
    s_active_ = this;
}

FavoriteMacroConfigModal::~FavoriteMacroConfigModal() {
    if (s_active_ == this)
        s_active_ = nullptr;
}

void FavoriteMacroConfigModal::load_config() {
    auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id_);
    auto c = favorite_macro_config_from_json(wc.get_widget_config(widget_id_));
    macro_name_ = c.macro;
    icon_name_ = c.icon;
    icon_color_ = c.color;
    require_confirmation_ = c.require_confirmation;
}

void FavoriteMacroConfigModal::persist() {
    auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id_);
    FavoriteMacroConfig c{macro_name_, icon_name_, icon_color_, require_confirmation_};
    wc.set_widget_config(widget_id_, favorite_macro_config_to_json(c));
    if (on_change_)
        on_change_();
}

void FavoriteMacroConfigModal::on_show() {
    load_config();
    macro_list_ = helix::ui::find_required(dialog(), "macro_list", get_name());
    icon_grid_ = helix::ui::find_required(dialog(), "icon_grid", get_name());
    lv_subject_set_int(&s_tab_subject, 0);
    lv_subject_set_int(&s_require_confirm_subject, require_confirmation_ ? 1 : 0);
    refresh_param_defaults_visible();
    populate_macro_list();
    populate_icon_grid();
    tile_color::publish_palette(icon_color_);
    spdlog::debug("[FavoriteMacroConfig] Opened: widget={}, macro={}", widget_id_, macro_name_);
}

void FavoriteMacroConfigModal::tab_macro_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] tab_macro_cb");
    lv_subject_set_int(&s_tab_subject, 0);
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::tab_appearance_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] tab_appearance_cb");
    lv_subject_set_int(&s_tab_subject, 1);
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::tab_options_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] tab_options_cb");
    lv_subject_set_int(&s_tab_subject, 2);
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::close_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] close_cb");
    if (s_active_)
        s_active_->hide();
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::require_confirm_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] require_confirm_cb");
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    // The switch owns its own checked state; push it back to the subject so the
    // bound consequence hint in the Options tab follows the toggle.
    lv_subject_set_int(&s_require_confirm_subject, enabled ? 1 : 0);
    if (s_active_)
        s_active_->select_require_confirmation(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

IMoonrakerAPI* FavoriteMacroConfigModal::get_api() const {
    return get_moonraker_api();
}

void FavoriteMacroConfigModal::refresh_param_defaults_visible() {
    const bool has_params =
        !macro_name_.empty() && helix::macro_takes_saved_defaults(
                                    helix::MacroParamCache::instance().get(macro_name_).knowledge);
    lv_subject_set_int(&s_has_param_defaults_subject, has_params ? 1 : 0);
}

void FavoriteMacroConfigModal::defaults_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] defaults_cb");
    if (s_active_)
        s_active_->open_defaults_editor();
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::open_defaults_editor() {
    auto cached = helix::MacroParamCache::instance().get(macro_name_);
    if (!helix::macro_takes_saved_defaults(cached.knowledge)) {
        // The Options row is hidden for these macros; this guards a stale
        // subject value after the configured macro changed.
        spdlog::debug("[FavoriteMacroConfigModal] No defaults editor for '{}' (no parameters)",
                      macro_name_);
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        spdlog::warn("[FavoriteMacroConfigModal] No active screen for the defaults editor");
        return;
    }

    std::string name = macro_name_;
    param_modal_.show_for_defaults(screen, name, cached.params,
                                   helix::MacroParamDefaults::instance().get(name),
                                   [name](const helix::MacroParamDefaultRecord& record) {
                                       helix::MacroParamDefaults::instance().set(name, record);
                                   });
}

void FavoriteMacroConfigModal::populate_macro_list() {
    if (!macro_list_)
        return;
    IMoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[FavoriteMacroConfigModal] No API available for macro list");
        return;
    }

    const std::vector<std::string> sorted = helix::pickable_macro_names(api->hardware().macros());

    for (const auto& macro : sorted) {
        bool is_selected = (macro == macro_name_);
        std::string display = helix::get_display_name(macro, helix::DeviceType::MACRO);

        lv_obj_t* row =
            helix::ui::create_picker_option_row(macro_list_, display.c_str(), is_selected,
                                                [macro]() {
                                                    if (s_active_) {
                                                        s_active_->select_macro(macro);
                                                    }
                                                },
                                                {"pad_all", "#space_xs", "pad_gap", "#space_xxs"});
        if (!row) {
            continue;
        }

        // refresh_highlights() reads the row's macro name back.
        auto* macro_name_copy = new std::string(macro);
        lv_obj_set_user_data(row, macro_name_copy);
        lv_obj_add_event_cb(
            row, [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, macro_name_copy);
    }

    spdlog::debug("[FavoriteMacroConfigModal] Populated macro list ({} macros)", sorted.size());
}

void FavoriteMacroConfigModal::populate_icon_grid() {
    if (!icon_grid_)
        return;

    std::string effective = icon_name_.empty() ? "play" : icon_name_;

    helix::ui::populate_icon_grid(icon_grid_, kCuratedIcons, kCuratedIconCount, effective,
                                  [this](const char* name) { select_icon(name); });

    spdlog::debug("[FavoriteMacroConfigModal] Populated icon grid ({} icons)", kCuratedIconCount);
}

void FavoriteMacroConfigModal::refresh_highlights() {
    if (macro_list_) {
        uint32_t count = lv_obj_get_child_count(macro_list_);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* row = lv_obj_get_child(macro_list_, i);
            auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
            bool selected = (name_ptr && *name_ptr == macro_name_);
            lv_obj_set_style_bg_opa(row, selected ? 30 : 0, 0);
        }
    }

    if (icon_grid_) {
        std::string effective = icon_name_.empty() ? "play" : icon_name_;
        helix::ui::refresh_icon_grid(icon_grid_, effective);
    }

    tile_color::publish_selection(icon_color_);
}

void FavoriteMacroConfigModal::select_macro(const std::string& name) {
    macro_name_ = name;
    persist();
    refresh_highlights();
    // The Options tab's "Default Parameters" row follows the new macro's
    // parameter knowledge.
    refresh_param_defaults_visible();
    spdlog::info("[FavoriteMacroConfigModal] Selected macro: {}", name);
}

void FavoriteMacroConfigModal::select_icon(const std::string& name) {
    icon_name_ = (name == "play") ? "" : name;
    persist();
    refresh_highlights();
    spdlog::info("[FavoriteMacroConfigModal] Selected icon: {}",
                 icon_name_.empty() ? "play (default)" : icon_name_);
}

void FavoriteMacroConfigModal::select_color(uint32_t color) {
    icon_color_ = color;
    persist();
    refresh_highlights();
    spdlog::info("[FavoriteMacroConfigModal] Selected color: 0x{:06X}", color);
}

void FavoriteMacroConfigModal::select_require_confirmation(bool enabled) {
    require_confirmation_ = enabled;
    persist();
    spdlog::info("[FavoriteMacroConfigModal] Require confirmation: {}", enabled ? "on" : "off");
}

void FavoriteMacroConfigModal::color_swatch_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] color_swatch_cb");
    const int index = helix::ui::event_user_int(e).value_or(-1);
    if (s_active_ && index >= 0 && index < static_cast<int>(tile_color::palette_size())) {
        s_active_->select_color(tile_color::palette_at(index));
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
