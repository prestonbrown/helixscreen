// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_appearance.cpp
 * @brief Implementation of AppearanceSettingsOverlay
 */

#include "ui_settings_appearance.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_theme_editor_overlay.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "border_radius_sizes.h"
#include "display_settings_manager.h"
#include "lvgl.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <string>

namespace helix::settings {

using helix::ui::event_checked;
using helix::ui::event_selected;

namespace {

bool same_palette(const helix::ModePalette& a, const helix::ModePalette& b) {
    for (size_t i = 0; i < helix::ModePalette::color_names().size(); ++i) {
        if (a.at(i) != b.at(i)) {
            return false;
        }
    }
    return true;
}

bool same_properties(const helix::ThemeProperties& a, const helix::ThemeProperties& b) {
    return a.border_radius_size == b.border_radius_size && a.border_width == b.border_width &&
           a.border_opacity == b.border_opacity && a.shadow_intensity == b.shadow_intensity &&
           a.shadow_opa == b.shadow_opa && a.shadow_offset_y == b.shadow_offset_y &&
           a.handle_style == b.handle_style && a.handle_color == b.handle_color;
}

/// Whether the screen already shows @p theme in @p dark mode, colours included:
/// the editor previews unsaved colours under the theme's own name.
bool theme_is_showing(const helix::ThemeData& theme, bool dark) {
    const helix::ThemeData& active = theme_manager_get_active_theme();
    return theme_manager_is_dark_mode() == dark && active.name == theme.name &&
           active.filename == theme.filename && same_palette(active.dark, theme.dark) &&
           same_palette(active.light, theme.light) &&
           same_properties(active.properties, theme.properties);
}

} // namespace

void AppearanceSettingsOverlay::init_subjects() {
    // Theme Apply button disabled subject (1=disabled initially)
    UI_MANAGED_SUBJECT_INT(theme_apply_disabled_subject_, 1, "theme_apply_disabled", subjects_);
}

void AppearanceSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_animations_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_animations_enabled(event_checked(e));
         }},
        {"on_glass_effects_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_glass_effects(event_checked(e));
         }},
        {"on_dark_mode_changed",
         [](lv_event_t* e) {
             bool enabled = event_checked(e);
             DisplaySettingsManager::instance().set_dark_mode(enabled);
             theme_manager_apply_theme(theme_manager_get_active_theme(), enabled);
         }},
        {"on_widget_labels_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_show_widget_labels(event_checked(e));
         }},
        {"on_bed_mesh_mode_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_bed_mesh_render_mode(event_selected(e));
         }},
        {"on_toolhead_style_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_toolhead_style(
                 SettingsManager::dropdown_index_to_toolhead_style(event_selected(e)));
         }},
        {"on_gcode_mode_changed",
         [](lv_event_t* e) {
             int index = event_selected(e);
#ifndef ENABLE_GLES_3D
             static const int kIndexToMode[] = {0, 2, 3}; // Auto, 2D Layers, Thumbnail Only
             int mode = (index >= 0 && index <= 2) ? kIndexToMode[index] : 0;
#else
             int mode = index;
#endif
             DisplaySettingsManager::instance().set_gcode_render_mode(mode);
         }},
        {"on_z_movement_style_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_z_movement_style(
                 static_cast<ZMovementStyle>(event_selected(e)));
         }},

        // Theme explorer
        {"on_theme_preset_changed",
         [](lv_event_t* e) {
             get_appearance_settings_overlay().handle_theme_preset_changed(event_selected(e));
         }},
        {"on_theme_settings_clicked",
         [](lv_event_t*) { get_appearance_settings_overlay().handle_theme_settings_clicked(); }},
        {"on_preview_dark_mode_toggled",
         [](lv_event_t* e) {
             get_appearance_settings_overlay().handle_preview_dark_mode_toggled(event_checked(e));
         }},
        {"on_edit_colors_clicked",
         [](lv_event_t*) { get_appearance_settings_overlay().handle_edit_colors_clicked(); }},
        {"on_preview_open_modal",
         [](lv_event_t*) {
             helix::ui::modal_confirm(
                 lv_tr("Sample Dialog"),
                 "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod "
                 "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim "
                 "veniam, quis nostrud exercitation ullamco laboris.",
                 ModalSeverity::Info, "OK", nullptr); // i18n: universal

             get_appearance_settings_overlay().apply_preview_palette_to_screen_popups();
         }},
        {"on_apply_theme_clicked",
         [](lv_event_t*) { get_appearance_settings_overlay().handle_apply_theme_clicked(); }},
    });
}

void AppearanceSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_toolhead_style_dropdown();
    init_gcode_mode_dropdown();
}

// Rows whose widget index is not the stored value (the toolhead style list is
// built at runtime, the G-code list loses "3D View" without GLES) are filled
// here; the other rows bind to their subjects in XML.
void AppearanceSettingsOverlay::init_toolhead_style_dropdown() {
    lv_obj_t* row = helix::ui::find_required(overlay_root_, "row_toolhead_style", get_name());
    if (lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name())) {
        lv_dropdown_set_options(dropdown, SettingsManager::get_toolhead_style_options().c_str());
        auto style = SettingsManager::instance().get_toolhead_style();
        lv_dropdown_set_selected(
            dropdown,
            static_cast<uint32_t>(SettingsManager::toolhead_style_to_dropdown_index(style)));
        spdlog::trace("[{}] Toolhead style dropdown initialized (style={}, dropdown_index={})",
                      get_name(), static_cast<int>(style),
                      SettingsManager::toolhead_style_to_dropdown_index(style));
    }
}

void AppearanceSettingsOverlay::init_gcode_mode_dropdown() {
    lv_obj_t* row = helix::ui::find_required(overlay_root_, "row_gcode_mode", get_name());
    if (lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name())) {
        auto& display_settings = DisplaySettingsManager::instance();
#ifndef ENABLE_GLES_3D
        // Without GLES, remove "3D View" option
        lv_dropdown_set_options(dropdown, (std::string(lv_tr("Auto")) + "\n" + lv_tr("2D Layers") +
                                           "\n" + lv_tr("Thumbnail Only"))
                                              .c_str());
        int mode = display_settings.get_gcode_render_mode();
        int index = 0; // Auto
        if (mode == 2)
            index = 1; // 2D Layers
        else if (mode == 3)
            index = 2; // Thumbnail Only
        lv_dropdown_set_selected(dropdown, index);
#else
        int mode = display_settings.get_gcode_render_mode();
        lv_dropdown_set_selected(dropdown, mode);
#endif
        spdlog::trace("[{}] G-code mode dropdown initialized", get_name());
    }
}

void AppearanceSettingsOverlay::init_theme_preset_dropdown(lv_obj_t* root) {
    if (!root)
        return;

    lv_obj_t* theme_preset_dropdown =
        helix::ui::find_required(root, "theme_preset_dropdown", get_name());
    if (theme_preset_dropdown) {
        std::string options = DisplaySettingsManager::instance().get_theme_options();
        lv_dropdown_set_options(theme_preset_dropdown, options.c_str());

        int current_index = DisplaySettingsManager::instance().get_theme_index();
        lv_dropdown_set_selected(theme_preset_dropdown, static_cast<uint32_t>(current_index));

        spdlog::debug("[{}] Theme dropdown initialized to index {} ({})", get_name(), current_index,
                      DisplaySettingsManager::instance().get_theme_name());
    }
}

// ============================================================================
// THEME EXPLORER
// ============================================================================

void AppearanceSettingsOverlay::handle_theme_preset_changed(int index) {
    if (theme_explorer_overlay_ && lv_obj_is_visible(theme_explorer_overlay_)) {
        handle_explorer_theme_changed(index);
        return;
    }

    DisplaySettingsManager::instance().set_theme_by_index(index);

    spdlog::info("[{}] Theme changed to index {} ({})", get_name(), index,
                 DisplaySettingsManager::instance().get_theme_name());
}

void AppearanceSettingsOverlay::handle_explorer_theme_changed(int index) {
    if (index < 0 || index >= static_cast<int>(cached_themes_.size())) {
        spdlog::error("[{}] Invalid theme index {}", get_name(), index);
        return;
    }

    std::string theme_name = cached_themes_[index].filename;
    helix::ThemeData theme = helix::load_theme_from_file(theme_name);

    if (!theme.is_valid()) {
        spdlog::error("[{}] Failed to load theme '{}' for preview", get_name(), theme_name);
        return;
    }

    bool supports_dark = theme.supports_dark();
    bool supports_light = theme.supports_light();

    if (theme_explorer_overlay_) {
        lv_obj_t* dark_toggle = helix::ui::find_required(theme_explorer_overlay_,
                                                         "preview_dark_mode_toggle", get_name());
        lv_obj_t* toggle_container = helix::ui::find_required(
            theme_explorer_overlay_, "dark_mode_toggle_container", get_name());

        if (dark_toggle) {
            if (supports_dark && supports_light) {
                lv_obj_remove_state(dark_toggle, LV_STATE_DISABLED);
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' supports both modes, toggle enabled", get_name(),
                              theme_name);
            } else if (supports_dark) {
                lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
                lv_obj_add_state(dark_toggle, LV_STATE_CHECKED);
                preview_is_dark_ = true;
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' is dark-only, forcing dark mode", get_name(),
                              theme_name);
            } else if (supports_light) {
                lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
                lv_obj_remove_state(dark_toggle, LV_STATE_CHECKED);
                preview_is_dark_ = false;
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' is light-only, forcing light mode", get_name(),
                              theme_name);
            }
        }
    }

    // One apply per pick: each costs about a second on slow hardware.
    theme_manager_preview(theme, preview_is_dark_);

    lv_subject_set_int(&theme_apply_disabled_subject_, index == original_theme_index_ ? 1 : 0);

    spdlog::debug("[{}] Explorer preview: theme '{}' (index {})", get_name(), theme_name, index);
}

void AppearanceSettingsOverlay::handle_theme_settings_clicked() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Theme settings clicked without parent screen", get_name());
        return;
    }

    if (!theme_explorer_overlay_) {
        spdlog::debug("[{}] Creating theme explorer overlay...", get_name());
        theme_explorer_overlay_ =
            helix::ui::create_xml_hidden(parent_screen_, "theme_preview_overlay");
        if (!theme_explorer_overlay_) {
            spdlog::error("[{}] Failed to create theme explorer overlay", get_name());
            return;
        }

        helix::nav::register_overlay(theme_explorer_overlay_, nullptr);
        helix::nav::on_close(theme_explorer_overlay_, [this]() {
            if (!theme_is_showing(original_theme_, original_is_dark_)) {
                theme_manager_apply_theme(original_theme_, original_is_dark_);
            }
            if (theme_explorer_overlay_) {
                helix::ui::defocus_tree(theme_explorer_overlay_);
                lv_obj_add_flag(theme_explorer_overlay_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_t* to_delete = theme_explorer_overlay_;
                theme_explorer_overlay_ = nullptr;
                lv_obj_delete_async(to_delete);
            }
            cached_themes_.clear();
        });
    }

    sync_explorer_to_active_theme();

    helix::nav::push_overlay(theme_explorer_overlay_);
}

void AppearanceSettingsOverlay::sync_explorer_to_active_theme() {
    if (!theme_explorer_overlay_)
        return;

    init_theme_preset_dropdown(theme_explorer_overlay_);

    cached_themes_ = helix::discover_themes(helix::get_themes_directory());

    original_theme_index_ = DisplaySettingsManager::instance().get_theme_index();
    original_theme_ = theme_manager_get_active_theme();
    original_is_dark_ = theme_manager_is_dark_mode();

    preview_is_dark_ = original_is_dark_;
    lv_obj_t* dark_toggle =
        helix::ui::find_required(theme_explorer_overlay_, "preview_dark_mode_toggle", get_name());
    if (dark_toggle) {
        if (preview_is_dark_) {
            lv_obj_add_state(dark_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(dark_toggle, LV_STATE_CHECKED);
        }

        bool supports_dark = theme_manager_supports_dark_mode();
        bool supports_light = theme_manager_supports_light_mode();
        if (supports_dark && supports_light) {
            lv_obj_remove_state(dark_toggle, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
        }
    }

    lv_subject_set_int(&theme_apply_disabled_subject_, 1);
}

void AppearanceSettingsOverlay::handle_apply_theme_clicked() {
    lv_obj_t* dropdown =
        helix::ui::find_required(theme_explorer_overlay_, "theme_preset_dropdown", get_name());
    if (!dropdown) {
        spdlog::warn("[{}] Apply clicked but dropdown not found", get_name());
        return;
    }

    int selected_index = lv_dropdown_get_selected(dropdown);

    DisplaySettingsManager::instance().set_theme_by_index(selected_index);
    std::string theme_name = DisplaySettingsManager::instance().get_theme_name();

    // The preview already shows this theme; committing it makes closing the
    // explorer a no-op rather than a revert.
    original_theme_index_ = selected_index;
    original_theme_ = theme_manager_get_active_theme();
    original_is_dark_ = theme_manager_is_dark_mode();

    spdlog::info("[{}] Theme '{}' applied (index {})", get_name(), theme_name, selected_index);

    std::string display_name = theme_name;
    if (selected_index >= 0 && selected_index < static_cast<int>(cached_themes_.size())) {
        display_name = cached_themes_[selected_index].display_name;
    }
    std::string toast_msg = "Theme set to " + display_name;
    ToastManager::instance().show(ToastSeverity::SUCCESS, toast_msg.c_str());

    helix::nav::go_back();
}

void AppearanceSettingsOverlay::handle_edit_colors_clicked() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Theme settings clicked without parent screen", get_name());
        return;
    }

    // The editor loads the active theme itself in on_activate().
    auto& editor = get_theme_editor_overlay();
    editor.set_editing_dark_mode(preview_is_dark_);
    editor.show(parent_screen_);
}

void AppearanceSettingsOverlay::handle_preview_dark_mode_toggled(bool is_dark) {
    preview_is_dark_ = is_dark;

    if (!theme_explorer_overlay_) {
        return;
    }

    lv_obj_t* dropdown =
        helix::ui::find_required(theme_explorer_overlay_, "theme_preset_dropdown", get_name());
    if (!dropdown) {
        return;
    }

    int selected_index = lv_dropdown_get_selected(dropdown);
    if (selected_index < 0 || selected_index >= static_cast<int>(cached_themes_.size())) {
        return;
    }

    helix::ThemeData theme = helix::load_theme_from_file(cached_themes_[selected_index].filename);

    if (!theme.is_valid()) {
        return;
    }

    theme_manager_preview(theme, is_dark);

    spdlog::debug("[{}] Preview dark mode toggled to {}", get_name(), is_dark ? "dark" : "light");
}

void AppearanceSettingsOverlay::apply_preview_palette_to_screen_popups() {
    if (!theme_explorer_overlay_ || cached_themes_.empty()) {
        return;
    }

    lv_obj_t* dropdown =
        helix::ui::find_required(theme_explorer_overlay_, "theme_preset_dropdown", get_name());
    if (!dropdown) {
        return;
    }

    uint32_t selected_index = lv_dropdown_get_selected(dropdown);
    if (selected_index >= cached_themes_.size()) {
        return;
    }

    helix::ThemeData theme = helix::load_theme_from_file(cached_themes_[selected_index].filename);
    if (!theme.is_valid()) {
        return;
    }

    const helix::ModePalette* palette = nullptr;
    if (preview_is_dark_ && theme.supports_dark()) {
        palette = &theme.dark;
    } else if (!preview_is_dark_ && theme.supports_light()) {
        palette = &theme.light;
    } else {
        palette = theme.supports_dark() ? &theme.dark : &theme.light;
    }

    theme_apply_palette_to_screen_dropdowns(*palette);

    lv_obj_t* modal_dialog = lv_obj_find_by_name(lv_screen_active(), "modal_dialog");
    if (modal_dialog) {
        const char* suffix = theme_manager_get_breakpoint_suffix(responsive_dimension(nullptr));
        int radius_px =
            helix::BorderRadiusSizes::pixels(theme.properties.border_radius_size, suffix);
        lv_obj_set_style_radius(modal_dialog, radius_px, LV_PART_MAIN);
    }
}

} // namespace helix::settings
