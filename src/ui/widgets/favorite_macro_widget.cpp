// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "favorite_macro_widget.h"

#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace helix {
void register_favorite_macro_widgets() {
    for (int i = 1; i <= kMaxFavoriteMacroSlots; ++i) {
        std::string id = "favorite_macro_" + std::to_string(i);
        register_widget_factory(id, [id]() {
            return std::make_unique<FavoriteMacroWidget>(id);
        });
    }
    // Register XML callbacks early — before any XML is parsed
    lv_xml_register_event_cb(nullptr, "favorite_macro_clicked_cb",
                             FavoriteMacroWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_picker_backdrop_cb",
                             FavoriteMacroWidget::picker_backdrop_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_picker_done_cb",
                             FavoriteMacroWidget::picker_done_cb);
}
} // namespace helix

namespace {

// Curated icon list for the picker grid (verified against ui_icon_codepoints.h)
static const char* const kCuratedIcons[] = {
    "play",       "pause",     "stop",              "refresh",   "home",
    "cog",        "wrench",    "fan",               "thermometer", "lightbulb_outline",
    "power",      "bell",      "flash",             "water",     "fire",
    "printer_3d", "check",     "bed",               "filament",  "cooldown",
    "script_text","hourglass", "speed",             "arrow_up",  "arrow_down",
};
static constexpr size_t kCuratedIconCount = std::size(kCuratedIcons);

// Color palette for icon tinting in rainbow order + neutrals.
// First entry (0) = sentinel for theme secondary (default).
// 4×4 grid: rainbow row 1, rainbow row 2, warm/cool accents, neutrals.
static constexpr uint32_t kIconColors[] = {
    0xE53935,  // Red
    0xFF5722,  // Deep Orange
    0xFF9800,  // Orange
    0xFFC107,  // Amber
    0xFFEB3B,  // Yellow
    0x8BC34A,  // Lime
    0x43A047,  // Green
    0x009688,  // Teal
    0x00BCD4,  // Cyan
    0x1E88E5,  // Blue
    0x3F51B5,  // Indigo
    0x7B1FA2,  // Purple
    0xE91E63,  // Pink
    0xFFFFFF,  // White
    0x808080,  // Gray
    0x000000,  // sentinel: theme default (secondary variant)
};
static constexpr size_t kIconColorCount = std::size(kIconColors);

static constexpr int kIconCellSize = 36;
static constexpr int kColorSwatchSize = 28;

/// Apply highlight styling to an icon grid cell.
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

/// Apply highlight styling to a color swatch.
void apply_color_swatch_highlight(lv_obj_t* swatch, bool selected) {
    lv_obj_set_style_border_width(swatch, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(
        swatch,
        selected ? theme_manager_get_color("primary") : theme_manager_get_color("text_muted"), 0);
}

// File-local helper: single shared MacroParamModal instance.
// Using one instance avoids s_active_instance_ stomping when two widget slots
// both try to open param modals (the old code had two separate static locals).
helix::MacroParamModal& get_shared_param_modal() {
    static helix::MacroParamModal modal;
    return modal;
}

/// Resolve a responsive spacing token to pixels, with a fallback.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// Free heap-allocated macro name strings stored as user_data on picker rows.
/// Called from both dismiss_macro_picker() and the LV_EVENT_DELETE handler.
} // namespace

using namespace helix;

FavoriteMacroWidget* FavoriteMacroWidget::s_active_picker_ = nullptr;

FavoriteMacroWidget::FavoriteMacroWidget(const std::string& widget_id) : widget_id_(widget_id) {}

FavoriteMacroWidget::~FavoriteMacroWidget() {
    detach();
}

void FavoriteMacroWidget::set_config(const nlohmann::json& config) {
    if (config.contains("macro") && config["macro"].is_string()) {
        macro_name_ = config["macro"].get<std::string>();
    }
    if (config.contains("icon") && config["icon"].is_string()) {
        icon_name_ = config["icon"].get<std::string>();
    }
    if (config.contains("color") && config["color"].is_number_unsigned()) {
        icon_color_ = config["color"].get<uint32_t>();
    }
    spdlog::debug("[FavoriteMacroWidget] Config: {}={} icon={} color=0x{:06X}", widget_id_,
                  macro_name_, icon_name_.empty() ? "default" : icon_name_, icon_color_);
}

void FavoriteMacroWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);

        // Pressed feedback: dim the widget on touch
        lv_obj_set_style_opa(widget_obj_, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // Cache label pointers from XML
    icon_label_ = lv_obj_find_by_name(widget_obj_, "fav_macro_icon");
    name_label_ = lv_obj_find_by_name(widget_obj_, "fav_macro_name");

    update_display();

    spdlog::debug("[FavoriteMacroWidget] Attached {} (macro: {})", widget_id_,
                  macro_name_.empty() ? "none" : macro_name_);
}

void FavoriteMacroWidget::detach() {
    *alive_ = false;
    dismiss_macro_picker();

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    icon_label_ = nullptr;
    name_label_ = nullptr;

    spdlog::debug("[FavoriteMacroWidget] Detached");
}

void FavoriteMacroWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                          int /*height_px*/) {
    if (!widget_obj_)
        return;

    bool tall = (rowspan >= 2);
    bool wide = (colspan >= 2);

    // Scale icon: md (32px) at 1×1, lg (48px) when tall or 2×2
    if (icon_label_) {
        const lv_font_t* icon_font = tall ? &mdi_icons_48 : &mdi_icons_32;
        lv_obj_set_style_text_font(icon_label_, icon_font, 0);
    }

    // Scale text: font_xs at 1×1, font_small when tall or wide
    if (name_label_) {
        const char* font_token = (tall || wide) ? "font_small" : "font_xs";
        const lv_font_t* text_font = theme_manager_get_font(font_token);
        if (text_font)
            lv_obj_set_style_text_font(name_label_, text_font, 0);
    }
}

bool FavoriteMacroWidget::on_edit_configure() {
    spdlog::info("[FavoriteMacroWidget] {} configure requested - showing picker", widget_id_);
    show_macro_picker();
    return false; // no rebuild needed — picker updates display in select_macro()
}

void FavoriteMacroWidget::handle_clicked() {
    if (macro_name_.empty()) {
        // No macro assigned — open picker to configure
        spdlog::info("[FavoriteMacroWidget] {} clicked (unconfigured) - showing picker",
                     widget_id_);
        show_macro_picker();
    } else {
        // Execute assigned macro
        spdlog::info("[FavoriteMacroWidget] {} clicked - executing {}", widget_id_, macro_name_);
        fetch_and_execute();
    }
}

MoonrakerAPI* FavoriteMacroWidget::get_api() const {
    return get_moonraker_api();
}

void FavoriteMacroWidget::update_display() {
    if (name_label_) {
        if (macro_name_.empty()) {
            lv_label_set_text(name_label_, lv_tr("Configure"));
        } else {
            std::string display = helix::get_display_name(macro_name_, helix::DeviceType::MACRO);
            lv_label_set_text(name_label_, display.c_str());
        }
    }

    if (icon_label_) {
        // Unconfigured: cog icon. Configured: custom icon or "play" default.
        const char* effective_icon = "cog";
        if (!macro_name_.empty())
            effective_icon = icon_name_.empty() ? "play" : icon_name_.c_str();
        ui_icon_set_source(icon_label_, effective_icon);

        // Apply custom color when configured, otherwise theme secondary
        if (icon_color_ != 0 && !macro_name_.empty()) {
            ui_icon_set_color(icon_label_, lv_color_hex(icon_color_), LV_OPA_COVER);
        } else {
            ui_icon_set_variant(icon_label_, "secondary");
        }
    }
}

void FavoriteMacroWidget::save_config() {
    nlohmann::json config;
    config["macro"] = macro_name_;
    if (!icon_name_.empty())
        config["icon"] = icon_name_;
    if (icon_color_ != 0)
        config["color"] = icon_color_;
    save_widget_config(config);
    spdlog::debug("[FavoriteMacroWidget] Saved config: {}={} icon={} color=0x{:06X}", widget_id_,
                  macro_name_, icon_name_.empty() ? "default" : icon_name_, icon_color_);
}

void FavoriteMacroWidget::fetch_and_execute() {
    MoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[FavoriteMacroWidget] No API available");
        return;
    }

    auto cached = MacroParamCache::instance().get(macro_name_);

    switch (cached.knowledge) {
    case MacroParamKnowledge::KNOWN_NO_PARAMS:
        execute_with_params({});
        break;
    case MacroParamKnowledge::KNOWN_PARAMS:
        if (parent_screen_) {
            std::weak_ptr<bool> weak_alive = alive_;
            get_shared_param_modal().show_for_macro(
                parent_screen_, macro_name_, cached.params,
                [this, weak_alive](const MacroParamResult& result) {
                    if (weak_alive.expired())
                        return;
                    execute_with_params(result);
                });
        }
        break;
    case MacroParamKnowledge::UNKNOWN:
        if (parent_screen_) {
            std::weak_ptr<bool> weak_alive = alive_;
            get_shared_param_modal().show_for_unknown_params(
                parent_screen_, macro_name_,
                [this, weak_alive](const MacroParamResult& result) {
                    if (weak_alive.expired())
                        return;
                    execute_with_params(result);
                });
        }
        break;
    }
}

void FavoriteMacroWidget::execute_with_params(const MacroParamResult& result) {
    MoonrakerAPI* api = get_api();
    if (!api) {
        return;
    }

    // Build gcode: SET_GCODE_VARIABLE commands for variable overrides, then macro call
    std::string gcode;
    for (const auto& [key, value] : result.variables) {
        // Variable names are lowercase in Klipper
        std::string var_lower = key;
        std::transform(var_lower.begin(), var_lower.end(), var_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        gcode += "SET_GCODE_VARIABLE MACRO=" + macro_name_ + " VARIABLE=" + var_lower +
                 " VALUE=" + value + "\n";
    }

    gcode += macro_name_;
    for (const auto& [key, value] : result.params) {
        gcode += " " + key + "=" + value;
    }

    spdlog::info("[FavoriteMacroWidget] Executing: {}", gcode);

    std::string macro_name_copy = macro_name_;
    api->execute_gcode(
        gcode,
        [macro_name_copy]() {
            spdlog::info("[FavoriteMacroWidget] {} executed successfully", macro_name_copy);
        },
        [macro_name_copy](const MoonrakerError& err) {
            spdlog::error("[FavoriteMacroWidget] {} failed: {}", macro_name_copy, err.message);
        });
}

void FavoriteMacroWidget::show_macro_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    // Dismiss any other widget's picker before opening ours
    if (s_active_picker_ && s_active_picker_ != this) {
        s_active_picker_->dismiss_macro_picker();
    }

    MoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[FavoriteMacroWidget] No API available for macro picker");
        return;
    }

    const auto& macros = api->hardware().macros();
    if (macros.empty()) {
        spdlog::warn("[FavoriteMacroWidget] No macros available");
        return;
    }

    // Create picker from XML
    picker_backdrop_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "favorite_macro_picker", nullptr));
    if (!picker_backdrop_) {
        spdlog::error("[FavoriteMacroWidget] Failed to create picker from XML");
        return;
    }

    // Find containers from XML
    picker_macro_list_ = lv_obj_find_by_name(picker_backdrop_, "macro_list");
    picker_icon_grid_ = lv_obj_find_by_name(picker_backdrop_, "icon_grid");
    picker_color_grid_ = lv_obj_find_by_name(picker_backdrop_, "color_grid");

    if (!picker_macro_list_) {
        spdlog::error("[FavoriteMacroWidget] macro_list not found in picker XML");
        helix::ui::safe_delete(picker_backdrop_);
        picker_backdrop_ = nullptr;
        return;
    }

    int space_xs = resolve_space_token("space_xs", 4);
    int space_md = resolve_space_token("space_md", 10);

    int screen_h = lv_obj_get_height(parent_screen_);

    // Cap macro list and icon grid heights based on screen size
    lv_obj_set_style_max_height(picker_macro_list_, screen_h * 55 / 100, 0);
    if (picker_icon_grid_) {
        lv_obj_set_style_max_height(picker_icon_grid_, screen_h * 30 / 100, 0);
    }

    // Sort macros alphabetically and populate
    std::vector<std::string> sorted_macros(macros.begin(), macros.end());
    std::sort(sorted_macros.begin(), sorted_macros.end());
    populate_macro_list(picker_macro_list_, sorted_macros);

    // Populate icon and color grids
    if (picker_icon_grid_)
        populate_icon_grid(picker_icon_grid_);
    if (picker_color_grid_)
        populate_color_grid(picker_color_grid_);

    s_active_picker_ = this;

    // Self-clearing delete callback — if LVGL deletes picker_backdrop_ via parent
    // deletion (e.g., user navigates away), clear our pointer to prevent dangling access
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* e) {
            auto* self = static_cast<FavoriteMacroWidget*>(lv_event_get_user_data(e));
            if (!self)
                return;
            self->picker_backdrop_ = nullptr;
            self->picker_icon_grid_ = nullptr;
            self->picker_color_grid_ = nullptr;
            self->picker_macro_list_ = nullptr;
            if (s_active_picker_ == self) {
                s_active_picker_ = nullptr;
            }
        },
        LV_EVENT_DELETE, this);

    // Position the context menu card near the widget
    lv_obj_t* card = lv_obj_find_by_name(picker_backdrop_, "context_menu");
    if (card && widget_obj_) {
        // Cap card height at 80% of screen for small displays
        lv_obj_set_style_max_height(card, screen_h * 80 / 100, 0);

        int screen_w = lv_obj_get_width(parent_screen_);

        lv_area_t widget_area;
        lv_obj_get_coords(widget_obj_, &widget_area);

        // Wider card for two-column layout
        int card_w = std::clamp(screen_w * 60 / 100, 300, 480);
        lv_obj_set_width(card, card_w);
        int card_x = (widget_area.x1 + widget_area.x2) / 2 - card_w / 2;
        int card_y = widget_area.y2 + space_xs;
        int max_card_h = screen_h * 80 / 100;

        // Clamp to screen bounds
        if (card_x < space_md)
            card_x = space_md;
        if (card_x + card_w > screen_w - space_md)
            card_x = screen_w - card_w - space_md;
        if (card_y + max_card_h > screen_h - space_md) {
            card_y = widget_area.y1 - max_card_h - space_xs;
            if (card_y < space_md)
                card_y = space_md;
        }

        lv_obj_set_pos(card, card_x, card_y);
    }

    spdlog::debug("[FavoriteMacroWidget] Picker shown with {} macros", sorted_macros.size());
}

void FavoriteMacroWidget::dismiss_macro_picker() {
    if (!picker_backdrop_) {
        return;
    }

    // Nullify pointers BEFORE delete — the DELETE handler does string cleanup
    // and pointer clearing as a safety net (also handles parent-deletion case)
    lv_obj_t* backdrop = picker_backdrop_;
    picker_backdrop_ = nullptr;
    picker_icon_grid_ = nullptr;
    picker_color_grid_ = nullptr;
    picker_macro_list_ = nullptr;
    s_active_picker_ = nullptr;

    if (lv_obj_is_valid(backdrop)) {
        helix::ui::safe_delete(backdrop);
    }

    spdlog::debug("[FavoriteMacroWidget] Picker dismissed");
}

void FavoriteMacroWidget::select_macro(const std::string& name) {
    macro_name_ = name;

    update_display();
    save_config();
    refresh_picker_highlights();

    spdlog::info("[FavoriteMacroWidget] {} selected macro: {}", widget_id_, name);
}

void FavoriteMacroWidget::select_icon(const std::string& name) {
    // Store empty for the default icon to avoid persisting the default name
    icon_name_ = (name == "play") ? "" : name;
    update_display();
    save_config();
    refresh_picker_highlights();
    spdlog::info("[FavoriteMacroWidget] {} selected icon: {}", widget_id_,
                 icon_name_.empty() ? "play (default)" : icon_name_);
}

void FavoriteMacroWidget::select_color(uint32_t color) {
    icon_color_ = color;
    update_display();
    save_config();
    refresh_picker_highlights();
    spdlog::info("[FavoriteMacroWidget] {} selected color: 0x{:06X}", widget_id_, color);
}

void FavoriteMacroWidget::populate_macro_list(lv_obj_t* list,
                                               const std::vector<std::string>& macros) {
    int space_xs = resolve_space_token("space_xs", 4);
    int space_sm = resolve_space_token("space_sm", 6);

    for (const auto& macro : macros) {
        if (!macro.empty() && macro[0] == '_')
            continue;
        bool is_selected = (macro == macro_name_);
        std::string display = helix::get_display_name(macro, helix::DeviceType::MACRO);

        lv_obj_t* row = lv_obj_create(list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // Highlight selected row
        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);

        // Pressed feedback
        lv_obj_set_style_bg_color(row, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

        // Macro display name
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, display.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        // [L071] child click passthrough
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store macro name for click handler
        auto* macro_name_copy = new std::string(macro);
        lv_obj_set_user_data(row, macro_name_copy);

        // Free heap string when row is deleted
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, macro_name_copy);

        // Click no longer auto-dismisses — picker stays open for icon/color selection
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] macro_row_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
                if (!name_ptr)
                    return;

                if (FavoriteMacroWidget::s_active_picker_) {
                    std::string selected = *name_ptr;
                    FavoriteMacroWidget::s_active_picker_->select_macro(selected);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }
}

void FavoriteMacroWidget::populate_icon_grid(lv_obj_t* grid) {
    std::string effective = icon_name_.empty() ? "play" : icon_name_;

    for (size_t i = 0; i < kCuratedIconCount; ++i) {
        lv_obj_t* cell = lv_obj_create(grid);
        lv_obj_set_size(cell, kIconCellSize, kIconCellSize);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(cell, 0, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);

        // Pressed feedback
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

        apply_icon_cell_highlight(cell, kCuratedIcons[i] == effective);

        // Icon glyph
        const char* cp = ui_icon::lookup_codepoint(kCuratedIcons[i]);
        if (cp) {
            lv_obj_t* icon = lv_label_create(cell);
            lv_label_set_text(icon, cp);
            lv_obj_set_style_text_font(icon, &mdi_icons_24, 0);
            lv_obj_set_style_text_color(icon, theme_manager_get_color("text"), 0);
            lv_obj_center(icon);
            // [L071] child click passthrough
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        // Store index as user_data (no heap alloc needed)
        lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        lv_obj_add_event_cb(
            cell,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] icon_cell_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto idx = static_cast<size_t>(
                    reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                if (idx < kCuratedIconCount && FavoriteMacroWidget::s_active_picker_) {
                    FavoriteMacroWidget::s_active_picker_->select_icon(kCuratedIcons[idx]);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }
}

void FavoriteMacroWidget::populate_color_grid(lv_obj_t* grid) {
    for (size_t i = 0; i < kIconColorCount; ++i) {
        lv_obj_t* swatch = lv_obj_create(grid);
        lv_obj_set_size(swatch, kColorSwatchSize, kColorSwatchSize);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(swatch, 4, 0);
        lv_obj_set_style_pad_all(swatch, 0, 0);

        // Swatch color: first entry uses theme secondary visually
        if (kIconColors[i] == 0) {
            lv_obj_set_style_bg_color(swatch, theme_manager_get_color("secondary"), 0);
        } else {
            lv_obj_set_style_bg_color(swatch, lv_color_hex(kIconColors[i]), 0);
        }
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);

        // Pressed feedback
        lv_obj_set_style_bg_opa(swatch, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);

        apply_color_swatch_highlight(swatch, kIconColors[i] == icon_color_);

        // Store color value as user_data
        lv_obj_set_user_data(swatch,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(kIconColors[i])));

        lv_obj_add_event_cb(
            swatch,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] color_swatch_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto color = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)));
                if (FavoriteMacroWidget::s_active_picker_) {
                    FavoriteMacroWidget::s_active_picker_->select_color(color);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }
}

void FavoriteMacroWidget::refresh_picker_highlights() {
    // Update macro list row highlights
    if (picker_macro_list_) {
        uint32_t count = lv_obj_get_child_count(picker_macro_list_);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* row = lv_obj_get_child(picker_macro_list_, i);
            auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
            bool selected = (name_ptr && *name_ptr == macro_name_);
            lv_obj_set_style_bg_opa(row, selected ? 30 : 0, 0);
        }
    }

    // Update icon grid highlights
    if (picker_icon_grid_) {
        std::string effective = icon_name_.empty() ? "play" : icon_name_;
        uint32_t count = lv_obj_get_child_count(picker_icon_grid_);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* cell = lv_obj_get_child(picker_icon_grid_, i);
            auto idx = static_cast<size_t>(
                reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell)));
            apply_icon_cell_highlight(
                cell, idx < kCuratedIconCount && kCuratedIcons[idx] == effective);
        }
    }

    // Update color grid highlights
    if (picker_color_grid_) {
        uint32_t count = lv_obj_get_child_count(picker_color_grid_);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* swatch = lv_obj_get_child(picker_color_grid_, i);
            auto color = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(lv_obj_get_user_data(swatch)));
            apply_color_swatch_highlight(swatch, color == icon_color_);
        }
    }
}

void FavoriteMacroWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] clicked_cb");
    auto* widget = panel_widget_from_event<FavoriteMacroWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroWidget::picker_backdrop_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] picker_backdrop_cb");
    if (s_active_picker_) {
        s_active_picker_->dismiss_macro_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroWidget::picker_done_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroWidget] picker_done_cb");
    if (s_active_picker_) {
        s_active_picker_->dismiss_macro_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}
