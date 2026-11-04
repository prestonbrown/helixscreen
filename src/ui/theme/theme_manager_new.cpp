// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme_manager.h"

// Forward declarations for style configure functions
namespace style_configs {
void configure_card(lv_style_t* s, const ThemePalette& p);
void configure_dialog(lv_style_t* s, const ThemePalette& p);
void configure_obj_base(lv_style_t* s, const ThemePalette& p);
void configure_input_bg(lv_style_t* s, const ThemePalette& p);
void configure_disabled(lv_style_t* s, const ThemePalette& p);
void configure_pressed(lv_style_t* s, const ThemePalette& p);
void configure_focused(lv_style_t* s, const ThemePalette& p);
void configure_text_primary(lv_style_t* s, const ThemePalette& p);
void configure_text_muted(lv_style_t* s, const ThemePalette& p);
void configure_text_subtle(lv_style_t* s, const ThemePalette& p);
void configure_icon_text(lv_style_t* s, const ThemePalette& p);
void configure_icon_primary(lv_style_t* s, const ThemePalette& p);
void configure_icon_secondary(lv_style_t* s, const ThemePalette& p);
void configure_icon_tertiary(lv_style_t* s, const ThemePalette& p);
void configure_icon_info(lv_style_t* s, const ThemePalette& p);
void configure_icon_success(lv_style_t* s, const ThemePalette& p);
void configure_icon_warning(lv_style_t* s, const ThemePalette& p);
void configure_icon_danger(lv_style_t* s, const ThemePalette& p);
void configure_button(lv_style_t* s, const ThemePalette& p);
void configure_button_primary(lv_style_t* s, const ThemePalette& p);
void configure_button_secondary(lv_style_t* s, const ThemePalette& p);
void configure_button_tertiary(lv_style_t* s, const ThemePalette& p);
void configure_button_danger(lv_style_t* s, const ThemePalette& p);
void configure_button_ghost(lv_style_t* s, const ThemePalette& p);
void configure_button_transparent(lv_style_t* s, const ThemePalette& p);
void configure_button_outline(lv_style_t* s, const ThemePalette& p);
void configure_button_success(lv_style_t* s, const ThemePalette& p);
void configure_button_warning(lv_style_t* s, const ThemePalette& p);
void configure_button_disabled(lv_style_t* s, const ThemePalette& p);
void configure_button_pressed(lv_style_t* s, const ThemePalette& p);
void configure_severity_info(lv_style_t* s, const ThemePalette& p);
void configure_severity_success(lv_style_t* s, const ThemePalette& p);
void configure_severity_warning(lv_style_t* s, const ThemePalette& p);
void configure_severity_danger(lv_style_t* s, const ThemePalette& p);
void configure_dropdown(lv_style_t* s, const ThemePalette& p);
void configure_checkbox(lv_style_t* s, const ThemePalette& p);
void configure_switch(lv_style_t* s, const ThemePalette& p);
void configure_slider(lv_style_t* s, const ThemePalette& p);
void configure_spinner(lv_style_t* s, const ThemePalette& p);
void configure_arc(lv_style_t* s, const ThemePalette& p);
} // namespace style_configs

ThemeManager& ThemeManager::instance() {
    static ThemeManager instance;
    return instance;
}

void ThemeManager::init() {
    if (initialized_)
        return;

    register_style_configs();

    // If palettes weren't explicitly set, apply sensible Nord defaults
    // This ensures tests and fallback scenarios have readable colors
    auto is_palette_empty = [](const ThemePalette& p) {
        // Check if text color is unset (all zeros = black = likely uninitialized)
        return p.text.red == 0 && p.text.green == 0 && p.text.blue == 0;
    };

    if (is_palette_empty(dark_palette_)) {
        // Dark palette: light-colored text for readability on dark backgrounds
        dark_palette_.text = lv_color_hex(0xECEFF4);       // Nord Snow Storm
        dark_palette_.text_muted = lv_color_hex(0xD8DEE9); // Nord Snow Storm (darker)
        dark_palette_.screen_bg = lv_color_hex(0x2E3440);  // Nord Polar Night
        dark_palette_.card_bg = lv_color_hex(0x3B4252);    // Nord Polar Night lighter
        dark_palette_.primary = lv_color_hex(0x5E81AC);    // Nord Frost
        dark_palette_.secondary = lv_color_hex(0x81A1C1);  // Nord Frost lighter
        dark_palette_.danger = lv_color_hex(0xBF616A);     // Nord Aurora Red
        dark_palette_.warning = lv_color_hex(0xEBCB8B);    // Nord Aurora Yellow
        dark_palette_.success = lv_color_hex(0xA3BE8C);    // Nord Aurora Green
        dark_palette_.info = lv_color_hex(0x88C0D0);       // Nord Frost
        dark_palette_.border = lv_color_hex(0x4C566A);     // Nord Polar Night lightest
    }

    if (is_palette_empty(light_palette_)) {
        // Light palette: dark-colored text for readability on light backgrounds
        light_palette_.text = lv_color_hex(0x2E3440);       // Nord Polar Night
        light_palette_.text_muted = lv_color_hex(0x4C566A); // Nord Polar Night (lighter)
        light_palette_.screen_bg = lv_color_hex(0xECEFF4);  // Nord Snow Storm
        light_palette_.card_bg = lv_color_hex(0xE5E9F0);    // Nord Snow Storm darker
        light_palette_.primary = lv_color_hex(0x5E81AC);    // Nord Frost
        light_palette_.secondary = lv_color_hex(0x81A1C1);  // Nord Frost lighter
        light_palette_.danger = lv_color_hex(0xBF616A);     // Nord Aurora Red
        light_palette_.warning = lv_color_hex(0xD08770);    // Nord Aurora Orange
        light_palette_.success = lv_color_hex(0xA3BE8C);    // Nord Aurora Green
        light_palette_.info = lv_color_hex(0x88C0D0);       // Nord Frost
        light_palette_.border = lv_color_hex(0xD8DEE9);     // Nord Snow Storm
    }

    // Apply current palette
    apply_palette(dark_mode_ ? dark_palette_ : light_palette_);
    initialized_ = true;
}

void ThemeManager::shutdown() {
    for (auto& entry : styles_) {
        lv_style_reset(&entry.style);
    }
    initialized_ = false;
}

lv_style_t* ThemeManager::get_style(StyleRole role) {
    auto idx = static_cast<size_t>(role);
    if (idx >= styles_.size())
        return nullptr;
    return &styles_[idx].style;
}

void ThemeManager::register_style_configs() {
    using namespace style_configs;

    // Initialize all styles
    for (size_t i = 0; i < styles_.size(); ++i) {
        styles_[i].role = static_cast<StyleRole>(i);
        lv_style_init(&styles_[i].style);
    }

    // Register configure functions
    styles_[static_cast<size_t>(StyleRole::Card)].configure = configure_card;
    styles_[static_cast<size_t>(StyleRole::Dialog)].configure = configure_dialog;
    styles_[static_cast<size_t>(StyleRole::ObjBase)].configure = configure_obj_base;
    styles_[static_cast<size_t>(StyleRole::InputBg)].configure = configure_input_bg;
    styles_[static_cast<size_t>(StyleRole::Disabled)].configure = configure_disabled;
    styles_[static_cast<size_t>(StyleRole::Pressed)].configure = configure_pressed;
    styles_[static_cast<size_t>(StyleRole::CardPressed)].configure = helix::configure_card_pressed;
    styles_[static_cast<size_t>(StyleRole::Focused)].configure = configure_focused;

    styles_[static_cast<size_t>(StyleRole::TextPrimary)].configure = configure_text_primary;
    styles_[static_cast<size_t>(StyleRole::TextMuted)].configure = configure_text_muted;
    styles_[static_cast<size_t>(StyleRole::TextSubtle)].configure = configure_text_subtle;

    styles_[static_cast<size_t>(StyleRole::IconText)].configure = configure_icon_text;
    styles_[static_cast<size_t>(StyleRole::IconPrimary)].configure = configure_icon_primary;
    styles_[static_cast<size_t>(StyleRole::IconSecondary)].configure = configure_icon_secondary;
    styles_[static_cast<size_t>(StyleRole::IconTertiary)].configure = configure_icon_tertiary;
    styles_[static_cast<size_t>(StyleRole::IconInfo)].configure = configure_icon_info;
    styles_[static_cast<size_t>(StyleRole::IconSuccess)].configure = configure_icon_success;
    styles_[static_cast<size_t>(StyleRole::IconWarning)].configure = configure_icon_warning;
    styles_[static_cast<size_t>(StyleRole::IconDanger)].configure = configure_icon_danger;

    styles_[static_cast<size_t>(StyleRole::Button)].configure = configure_button;
    styles_[static_cast<size_t>(StyleRole::ButtonPrimary)].configure = configure_button_primary;
    styles_[static_cast<size_t>(StyleRole::ButtonSecondary)].configure = configure_button_secondary;
    styles_[static_cast<size_t>(StyleRole::ButtonTertiary)].configure = configure_button_tertiary;
    styles_[static_cast<size_t>(StyleRole::ButtonDanger)].configure = configure_button_danger;
    styles_[static_cast<size_t>(StyleRole::ButtonGhost)].configure = configure_button_ghost;
    styles_[static_cast<size_t>(StyleRole::ButtonTransparent)].configure =
        configure_button_transparent;
    styles_[static_cast<size_t>(StyleRole::ButtonOutline)].configure = configure_button_outline;
    styles_[static_cast<size_t>(StyleRole::ButtonSuccess)].configure = configure_button_success;
    styles_[static_cast<size_t>(StyleRole::ButtonWarning)].configure = configure_button_warning;
    styles_[static_cast<size_t>(StyleRole::ButtonDisabled)].configure = configure_button_disabled;
    styles_[static_cast<size_t>(StyleRole::ButtonPressed)].configure = configure_button_pressed;

    styles_[static_cast<size_t>(StyleRole::SeverityInfo)].configure = configure_severity_info;
    styles_[static_cast<size_t>(StyleRole::SeveritySuccess)].configure = configure_severity_success;
    styles_[static_cast<size_t>(StyleRole::SeverityWarning)].configure = configure_severity_warning;
    styles_[static_cast<size_t>(StyleRole::SeverityDanger)].configure = configure_severity_danger;

    styles_[static_cast<size_t>(StyleRole::Dropdown)].configure = configure_dropdown;
    styles_[static_cast<size_t>(StyleRole::Checkbox)].configure = configure_checkbox;
    styles_[static_cast<size_t>(StyleRole::Switch)].configure = configure_switch;
    styles_[static_cast<size_t>(StyleRole::Slider)].configure = configure_slider;
    styles_[static_cast<size_t>(StyleRole::Spinner)].configure = configure_spinner;
    styles_[static_cast<size_t>(StyleRole::Arc)].configure = configure_arc;
}

void ThemeManager::apply_palette(const ThemePalette& palette) {
    current_palette_ = palette;
    current_palette_.glass = glass_;
    current_palette_.dark = dark_mode_;
    for (auto& entry : styles_) {
        if (entry.configure) {
            lv_style_reset(&entry.style);
            entry.configure(&entry.style, current_palette_);
        }
    }
}

void ThemeManager::set_glass(bool on) {
    if (glass_ == on) {
        return;
    }
    glass_ = on;
    if (initialized_) {
        // A preview keeps showing the palette being previewed, now with glass.
        const ThemePalette palette = current_palette_;
        apply_palette(palette);
        lv_obj_report_style_change(nullptr);
    }
}

void ThemeManager::set_dark_mode(bool dark) {
    if (dark_mode_ == dark && initialized_)
        return;
    dark_mode_ = dark;

    if (initialized_) {
        apply_palette(dark ? dark_palette_ : light_palette_);
        // Trigger LVGL widget refresh
        lv_obj_report_style_change(nullptr);
    }
}

void ThemeManager::set_palettes(const ThemePalette& light, const ThemePalette& dark) {
    light_palette_ = light;
    dark_palette_ = dark;
    // Update current palette if already initialized
    if (initialized_) {
        apply_palette(dark_mode_ ? dark_palette_ : light_palette_);
        // Invalidate LVGL style caches so widgets re-read updated shared styles.
        // Without this, cached radius from LVGL default theme's btn style can
        // persist after our shared Button style is reset+reconfigured.
        lv_obj_report_style_change(nullptr);
    }
}

lv_color_t ThemeManager::get_color(std::string_view name) const {
    // Map name to palette field
    if (name == "screen_bg")
        return current_palette_.screen_bg;
    if (name == "overlay_bg")
        return current_palette_.overlay_bg;
    if (name == "card_bg")
        return current_palette_.card_bg;
    if (name == "elevated_bg")
        return current_palette_.elevated_bg;
    if (name == "border")
        return current_palette_.border;
    if (name == "text")
        return current_palette_.text;
    if (name == "text_muted")
        return current_palette_.text_muted;
    if (name == "text_subtle")
        return current_palette_.text_subtle;
    if (name == "primary")
        return current_palette_.primary;
    if (name == "secondary")
        return current_palette_.secondary;
    if (name == "tertiary")
        return current_palette_.tertiary;
    if (name == "info")
        return current_palette_.info;
    if (name == "success")
        return current_palette_.success;
    if (name == "warning")
        return current_palette_.warning;
    if (name == "danger")
        return current_palette_.danger;
    if (name == "focus")
        return current_palette_.focus;

    // Unknown color - return magenta for debugging
    return lv_color_hex(0xFF00FF);
}

void ThemeManager::preview_palette(const ThemePalette& palette) {
    previewing_ = true;
    apply_palette(palette);
    lv_obj_report_style_change(nullptr);
}

void ThemeManager::cancel_preview() {
    if (!previewing_)
        return;
    previewing_ = false;
    apply_palette(dark_mode_ ? dark_palette_ : light_palette_);
    lv_obj_report_style_change(nullptr);
}
