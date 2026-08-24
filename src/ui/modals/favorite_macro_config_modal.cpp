// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config_modal.h"

#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "favorite_macro_config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "static_subject_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

namespace helix {

namespace {
lv_subject_t s_tab_subject;
lv_subject_t s_skip_subject;
bool s_subjects_registered = false;

// Curated icon list for the picker grid (matches FavoriteMacroWidget).
static const char* const CURATED_ICONS[] = {
    "play",        "pause",     "stop",  "refresh",     "home",
    "cog",         "wrench",    "fan",   "thermometer", "lightbulb_outline",
    "power",       "bell",      "flash", "water",       "fire",
    "printer_3d",  "check",     "bed",   "filament",    "cooldown",
    "script_text", "hourglass", "speed", "arrow_up",    "arrow_down",
};
static constexpr size_t CURATED_ICON_COUNT = std::size(CURATED_ICONS);

// Color palette — first entry (0) = theme secondary sentinel.
static constexpr uint32_t ICON_COLORS[] = {
    0xE53935, // Red
    0xFF5722, // Deep Orange
    0xFF9800, // Orange
    0xFFC107, // Amber
    0xFFEB3B, // Yellow
    0x8BC34A, // Lime
    0x43A047, // Green
    0x009688, // Teal
    0x00BCD4, // Cyan
    0x1E88E5, // Blue
    0x3F51B5, // Indigo
    0x7B1FA2, // Purple
    0xE91E63, // Pink
    0xFFFFFF, // White
    0x808080, // Gray
    0x000000, // sentinel: theme default (secondary variant)
};
static constexpr size_t ICON_COLOR_COUNT = std::size(ICON_COLORS);

static constexpr int ICON_CELL_SIZE = 36;
static constexpr int COLOR_SWATCH_SIZE = 28;

void apply_icon_cell_highlight(lv_obj_t* cell, bool selected) {
    if (selected) {
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, theme_manager_get_color("primary"), 0);
        lv_obj_set_style_bg_opa(cell, 20, 0);
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("primary"), 0);
    } else {
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, 0, 0);
    }
}

void apply_color_swatch_highlight(lv_obj_t* swatch, bool selected) {
    lv_obj_set_style_border_width(swatch, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(
        swatch,
        selected ? theme_manager_get_color("primary") : theme_manager_get_color("text_muted"), 0);
}

} // namespace

FavoriteMacroConfigModal* FavoriteMacroConfigModal::s_active_ = nullptr;

void register_favorite_macro_config_subjects() {
    if (s_subjects_registered)
        return;
    lv_subject_init_int(&s_tab_subject, 0);
    lv_subject_init_int(&s_skip_subject, 0);
    lv_xml_register_subject(nullptr, "fav_macro_config_tab", &s_tab_subject);
    lv_xml_register_subject(nullptr, "fav_macro_skip_params", &s_skip_subject);
    StaticSubjectRegistry::instance().register_deinit("FavoriteMacroConfigSubjects", []() {
        lv_subject_deinit(&s_tab_subject);
        lv_subject_deinit(&s_skip_subject);
        s_subjects_registered = false;
    });
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
    skip_param_prompt_ = c.skip_param_prompt;
}

void FavoriteMacroConfigModal::persist() {
    auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id_);
    FavoriteMacroConfig c{macro_name_, icon_name_, icon_color_, skip_param_prompt_};
    wc.set_widget_config(widget_id_, favorite_macro_config_to_json(c));
    if (on_change_)
        on_change_();
}

void FavoriteMacroConfigModal::on_show() {
    load_config();
    macro_list_ = lv_obj_find_by_name(dialog(), "macro_list");
    icon_grid_ = lv_obj_find_by_name(dialog(), "icon_grid");
    color_grid_ = lv_obj_find_by_name(dialog(), "color_grid");
    lv_subject_set_int(&s_tab_subject, 0);
    lv_subject_set_int(&s_skip_subject, skip_param_prompt_ ? 1 : 0);
    populate_macro_list();
    populate_icon_grid();
    populate_color_grid();
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

void FavoriteMacroConfigModal::skip_params_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] skip_params_cb");
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (s_active_)
        s_active_->select_skip_param(lv_obj_has_state(sw, LV_STATE_CHECKED));
    LVGL_SAFE_EVENT_CB_END();
}

IMoonrakerAPI* FavoriteMacroConfigModal::get_api() const {
    return get_moonraker_api();
}

void FavoriteMacroConfigModal::populate_macro_list() {
    if (!macro_list_)
        return;
    IMoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[FavoriteMacroConfigModal] No API available for macro list");
        return;
    }

    const auto& macros = api->hardware().macros();
    std::vector<std::string> sorted(macros.begin(), macros.end());
    std::sort(sorted.begin(), sorted.end());

    for (const auto& macro : sorted) {
        if (!macro.empty() && macro[0] == '_')
            continue;

        bool is_selected = (macro == macro_name_);
        std::string display = helix::get_display_name(macro, helix::DeviceType::MACRO);

        lv_obj_t* row = lv_obj_create(macro_list_);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_xs"), 0);
        lv_obj_set_style_pad_gap(row, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);

        lv_obj_set_style_bg_color(row, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, display.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        auto* macro_name_copy = new std::string(macro);
        lv_obj_set_user_data(row, macro_name_copy);

        lv_obj_add_event_cb(
            row, [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, macro_name_copy);

        lv_obj_add_event_cb(row, FavoriteMacroConfigModal::macro_row_cb, LV_EVENT_CLICKED, nullptr);
    }

    spdlog::debug("[FavoriteMacroConfigModal] Populated macro list ({} macros)", sorted.size());
}

void FavoriteMacroConfigModal::populate_icon_grid() {
    if (!icon_grid_)
        return;

    std::string effective = icon_name_.empty() ? "play" : icon_name_;

    for (size_t i = 0; i < CURATED_ICON_COUNT; ++i) {
        lv_obj_t* cell = lv_obj_create(icon_grid_);
        lv_obj_set_size(cell, ICON_CELL_SIZE, ICON_CELL_SIZE);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(cell, 0, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);

        lv_obj_set_style_bg_color(cell, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

        apply_icon_cell_highlight(cell, CURATED_ICONS[i] == effective);

        const char* cp = ui_icon::lookup_codepoint(CURATED_ICONS[i]);
        if (cp) {
            lv_obj_t* icon = lv_label_create(cell);
            lv_label_set_text(icon, cp);
            lv_obj_set_style_text_font(icon, &mdi_icons_24, 0);
            lv_obj_set_style_text_color(icon, theme_manager_get_color("text"), 0);
            lv_obj_center(icon);
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(cell, FavoriteMacroConfigModal::icon_cell_cb, LV_EVENT_CLICKED,
                            nullptr);
    }

    spdlog::debug("[FavoriteMacroConfigModal] Populated icon grid ({} icons)", CURATED_ICON_COUNT);
}

void FavoriteMacroConfigModal::populate_color_grid() {
    if (!color_grid_)
        return;

    for (size_t i = 0; i < ICON_COLOR_COUNT; ++i) {
        lv_obj_t* swatch = lv_obj_create(color_grid_);
        lv_obj_set_size(swatch, COLOR_SWATCH_SIZE, COLOR_SWATCH_SIZE);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(swatch, 4, 0);
        lv_obj_set_style_pad_all(swatch, 0, 0);

        if (ICON_COLORS[i] == 0) {
            lv_obj_set_style_bg_color(swatch, theme_manager_get_color("secondary"), 0);
        } else {
            lv_obj_set_style_bg_color(swatch, lv_color_hex(ICON_COLORS[i]), 0);
        }
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);

        lv_obj_set_style_bg_opa(swatch, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);

        apply_color_swatch_highlight(swatch, ICON_COLORS[i] == icon_color_);

        lv_obj_set_user_data(swatch,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(ICON_COLORS[i])));
        lv_obj_add_event_cb(swatch, FavoriteMacroConfigModal::color_swatch_cb, LV_EVENT_CLICKED,
                            nullptr);
    }

    spdlog::debug("[FavoriteMacroConfigModal] Populated color grid ({} swatches)",
                  ICON_COLOR_COUNT);
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
        uint32_t count = lv_obj_get_child_count(icon_grid_);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* cell = lv_obj_get_child(icon_grid_, i);
            auto idx = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell)));
            apply_icon_cell_highlight(cell,
                                      idx < CURATED_ICON_COUNT && CURATED_ICONS[idx] == effective);
        }
    }

    if (color_grid_) {
        uint32_t count = lv_obj_get_child_count(color_grid_);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* swatch = lv_obj_get_child(color_grid_, i);
            auto color =
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(swatch)));
            apply_color_swatch_highlight(swatch, color == icon_color_);
        }
    }
}

void FavoriteMacroConfigModal::select_macro(const std::string& name) {
    macro_name_ = name;
    persist();
    refresh_highlights();
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

void FavoriteMacroConfigModal::select_skip_param(bool enabled) {
    skip_param_prompt_ = enabled;
    persist();
}

void FavoriteMacroConfigModal::macro_row_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] macro_row_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
    if (name_ptr && s_active_) {
        s_active_->select_macro(*name_ptr);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::icon_cell_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] icon_cell_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto idx = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    if (idx < CURATED_ICON_COUNT && s_active_) {
        s_active_->select_icon(CURATED_ICONS[idx]);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::color_swatch_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] color_swatch_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto color = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)));
    if (s_active_) {
        s_active_->select_color(color);
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
