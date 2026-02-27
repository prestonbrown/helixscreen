// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_led.cpp
 * @brief Implementation of LedSettingsOverlay
 */

#include "ui_settings_led.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_keyboard_manager.h"
#include "ui_led_chip_factory.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "device_display_name.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<LedSettingsOverlay> g_led_settings_overlay;

LedSettingsOverlay& get_led_settings_overlay() {
    if (!g_led_settings_overlay) {
        g_led_settings_overlay = std::make_unique<LedSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy("LedSettingsOverlay",
                                                         []() { g_led_settings_overlay.reset(); });
    }
    return *g_led_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

LedSettingsOverlay::LedSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

LedSettingsOverlay::~LedSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void LedSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(auto_state_enabled_subject_, 0, "led_auto_state_enabled", subjects_);
    });
}

void LedSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_led_on_at_start_changed", on_led_on_at_start_changed},
        {"on_auto_state_changed", on_auto_state_changed},
        {"on_add_macro_device", on_add_macro_device},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* LedSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "led_settings_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void LedSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Register for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack (on_activate will initialize widgets)
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void LedSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    populate_led_chips();
    init_led_on_at_start_toggle();
    init_auto_state_toggle();
    populate_auto_state_rows();
    populate_macro_devices();
}

void LedSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// INTERNAL METHODS
// ============================================================================

void LedSettingsOverlay::init_led_on_at_start_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_led_on_at_start");
    if (row) {
        lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
        if (toggle) {
            bool enabled = helix::led::LedController::instance().get_led_on_at_start();
            if (enabled) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}]   LED on at start toggle: {}", get_name(), enabled ? "ON" : "OFF");
        }
    }
}

void LedSettingsOverlay::init_auto_state_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_auto_state_enabled");
    if (row) {
        lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
        if (toggle) {
            bool enabled = helix::led::LedAutoState::instance().is_enabled();
            if (enabled) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            }
            // Sync visibility subject for auto-state rows container
            lv_subject_set_int(&auto_state_enabled_subject_, enabled ? 1 : 0);
            spdlog::trace("[{}]   Auto state toggle: {}", get_name(), enabled ? "ON" : "OFF");
        }
    }
}

void LedSettingsOverlay::populate_macro_devices() {
    if (!overlay_root_)
        return;

    // SAFETY: Defer the clean+rebuild. This function is called from many event
    // callbacks (dropdown change, remove/add/edit/save buttons) where the event-
    // firing widget is a child of the container being cleaned (issue #80).
    helix::ui::queue_update([this]() { populate_macro_devices_impl(); });
}

void LedSettingsOverlay::populate_macro_devices_impl() {
    if (!overlay_root_)
        return;

    lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "macro_devices_container");
    if (!container)
        return;

    lv_obj_clean(container);

    auto& led_ctrl = helix::led::LedController::instance();
    const auto& macros = led_ctrl.configured_macros();
    const auto& discovered = led_ctrl.discovered_macros();

    if (macros.empty()) {
        // Empty state message
        const char* attrs[] = {
            "label",     "No macro devices configured",
            "label_tag", "No macro devices configured",
            "icon",      "code_braces",
            "value",     "Tap + to add",
            nullptr,
        };
        lv_xml_create(container, "setting_info_row", attrs);

        if (discovered.empty()) {
            // Warn that no macros were detected on the printer
            auto* note = lv_label_create(container);
            lv_label_set_text(note, "No LED macros detected on your printer. "
                                    "Add Klipper macros for LED control first.");
            lv_obj_set_width(note, lv_pct(100));
            lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(note, theme_manager_get_color("text_subtle"), 0);
            lv_obj_set_style_pad_left(note, 24, 0);
            lv_obj_set_style_pad_right(note, 24, 0);
        }

        spdlog::debug("[{}] No macro devices to display", get_name());
        return;
    }

    auto text_color = theme_manager_get_color("text");
    auto text_muted = theme_manager_get_color("text_subtle");

    for (int i = 0; i < static_cast<int>(macros.size()); i++) {
        const auto& macro = macros[i];
        bool is_editing = (i == editing_macro_index_);

        // --- Card container ---
        auto* card =
            static_cast<lv_obj_t*>(lv_xml_create(container, "setting_macro_card", nullptr));

        // --- Header row (collapsed view) ---
        auto* header_row = lv_obj_create(card);
        lv_obj_set_width(header_row, lv_pct(100));
        lv_obj_set_height(header_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(header_row, 0, 0);
        lv_obj_set_style_pad_gap(header_row, 8, 0);
        lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(header_row, 0, 0);
        lv_obj_remove_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);

        // Device name
        auto* name_label = lv_label_create(header_row);
        lv_label_set_text(name_label,
                          macro.display_name.empty() ? "(unnamed)" : macro.display_name.c_str());
        lv_obj_set_style_text_color(name_label, text_color, 0);

        // Type badge
        std::string type_str;
        switch (macro.type) {
        case helix::led::MacroLedType::ON_OFF:
            type_str = "On/Off";
            break;
        case helix::led::MacroLedType::TOGGLE:
            type_str = "Toggle";
            break;
        case helix::led::MacroLedType::PRESET:
            type_str = "Preset";
            break;
        }
        auto* badge = lv_label_create(header_row);
        lv_label_set_text(badge, type_str.c_str());
        lv_obj_set_style_text_color(badge, text_muted, 0);

        // Flex spacer
        auto* spacer = lv_obj_create(header_row);
        lv_obj_set_flex_grow(spacer, 1);
        lv_obj_set_height(spacer, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_set_style_pad_all(spacer, 0, 0);
        lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

        // Edit button
        auto* edit_btn = lv_button_create(header_row);
        lv_obj_set_size(edit_btn, 36, 36);
        lv_obj_set_style_bg_opa(edit_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(edit_btn, 0, 0);
        const char* edit_icon_attrs[] = {"src",     "pencil",   "size", "sm",
                                         "variant", "tertiary", nullptr};
        lv_xml_create(edit_btn, "icon", edit_icon_attrs);

        // TODO: lv_obj user_data is safe on these buttons because they're
        // plain lv_button_create() (not XML widgets). If buttons are ever
        // created via lv_xml_create() from a component that claims user_data,
        // move to event callback user_data instead. See L069.
        auto* edit_idx = new int(i);
        lv_obj_set_user_data(edit_btn, edit_idx);
        lv_obj_add_event_cb(
            edit_btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] edit_macro_device");
                auto* idx = static_cast<int*>(
                    lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
                if (idx) {
                    get_led_settings_overlay().handle_edit_macro_device(*idx);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, edit_idx);
        lv_obj_add_event_cb(
            edit_btn, [](lv_event_t* e) { delete static_cast<int*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, edit_idx);

        // Delete button
        auto* del_btn = lv_button_create(header_row);
        lv_obj_set_size(del_btn, 36, 36);
        lv_obj_set_style_bg_opa(del_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(del_btn, 0, 0);
        const char* del_icon_attrs[] = {"src",     "delete",    "size", "sm",
                                        "variant", "secondary", nullptr};
        lv_xml_create(del_btn, "icon", del_icon_attrs);

        auto* del_idx = new int(i);
        lv_obj_set_user_data(del_btn, del_idx);
        lv_obj_add_event_cb(
            del_btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] delete_macro_device");
                auto* idx = static_cast<int*>(
                    lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
                if (idx) {
                    get_led_settings_overlay().handle_delete_macro_device(*idx);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, del_idx);
        lv_obj_add_event_cb(
            del_btn, [](lv_event_t* e) { delete static_cast<int*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, del_idx);

        // --- Macro summary line (shown when NOT editing) ---
        if (!is_editing) {
            std::string summary;
            switch (macro.type) {
            case helix::led::MacroLedType::ON_OFF:
                summary =
                    fmt::format("ON: {} | OFF: {}", macro.on_macro.empty() ? "—" : macro.on_macro,
                                macro.off_macro.empty() ? "—" : macro.off_macro);
                break;
            case helix::led::MacroLedType::TOGGLE:
                summary = fmt::format("TOGGLE: {}",
                                      macro.toggle_macro.empty() ? "—" : macro.toggle_macro);
                break;
            case helix::led::MacroLedType::PRESET:
                if (macro.presets.empty()) {
                    summary = "No presets configured";
                } else {
                    summary = fmt::format("{} preset{}", macro.presets.size(),
                                          macro.presets.size() == 1 ? "" : "s");
                }
                break;
            }

            auto* summary_label = lv_label_create(card);
            lv_label_set_text(summary_label, summary.c_str());
            lv_label_set_long_mode(summary_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(summary_label, lv_pct(100));
            lv_obj_set_style_text_color(summary_label, text_muted, 0);
        }

        // --- Edit controls (shown when editing) ---
        if (is_editing) {
            auto* edit_container = lv_obj_create(card);
            lv_obj_set_width(edit_container, lv_pct(100));
            lv_obj_set_height(edit_container, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(edit_container, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(edit_container, 0, 0);
            lv_obj_set_style_pad_gap(edit_container, 8, 0);
            lv_obj_set_style_bg_opa(edit_container, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(edit_container, 0, 0);
            lv_obj_remove_flag(edit_container, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_name(edit_container, fmt::format("macro_edit_{}", i).c_str());

            rebuild_macro_edit_controls(edit_container, i);
        }
    }

    spdlog::debug("[{}] Macro devices populated ({} devices, editing={})", get_name(),
                  macros.size(), editing_macro_index_);
}

void LedSettingsOverlay::rebuild_macro_edit_controls(lv_obj_t* container, int index) {
    if (!container)
        return;

    lv_obj_clean(container);

    auto& led_ctrl = helix::led::LedController::instance();
    const auto& macros = led_ctrl.configured_macros();
    if (index < 0 || index >= static_cast<int>(macros.size()))
        return;

    const auto& macro = macros[index];
    const auto& discovered = led_ctrl.discovered_macros();

    auto primary_color = theme_manager_get_color("primary");

    // --- Name input ---
    const char* name_attrs[] = {"label", "Name:", "placeholder", "Device name", nullptr};
    auto* name_row =
        static_cast<lv_obj_t*>(lv_xml_create(container, "setting_form_input", name_attrs));
    auto* name_ta = lv_obj_find_by_name(name_row, "input");
    lv_obj_set_name(name_ta, "macro_name_input");
    lv_textarea_set_text(name_ta, macro.display_name.c_str());
    KeyboardManager::instance().register_textarea(name_ta);

    // --- Type dropdown ---
    const char* type_attrs[] = {"label", "Type:", nullptr};
    auto* type_row =
        static_cast<lv_obj_t*>(lv_xml_create(container, "setting_form_dropdown", type_attrs));
    auto* type_dd = lv_obj_find_by_name(type_row, "dropdown");
    std::string type_options = std::string(lv_tr("On/Off (state-aware)")) + "\n" +
                               lv_tr("Toggle (fire-and-forget)") + "\n" + lv_tr("Preset");
    lv_dropdown_set_options(type_dd, type_options.c_str());
    lv_obj_set_name(type_dd, "macro_type_dropdown");

    // Set current type
    switch (macro.type) {
    case helix::led::MacroLedType::ON_OFF:
        lv_dropdown_set_selected(type_dd, 0);
        break;
    case helix::led::MacroLedType::TOGGLE:
        lv_dropdown_set_selected(type_dd, 1);
        break;
    case helix::led::MacroLedType::PRESET:
        lv_dropdown_set_selected(type_dd, 2);
        break;
    }

    // Type help text (form rebuilds on type change, so static text is fine)
    auto* type_help = lv_label_create(container);
    lv_obj_set_width(type_help, lv_pct(100));
    lv_label_set_long_mode(type_help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(type_help, theme_manager_get_color("text_subtle"), 0);
    lv_obj_set_style_pad_left(type_help, 24, 0);
    lv_obj_set_style_pad_right(type_help, 24, 0);
    switch (macro.type) {
    case helix::led::MacroLedType::ON_OFF:
        lv_label_set_text(type_help,
                          lv_tr("Separate on/off macros. HelixScreen tracks the state."));
        break;
    case helix::led::MacroLedType::TOGGLE:
        lv_label_set_text(type_help, lv_tr("Single toggle macro. State is not tracked."));
        break;
    case helix::led::MacroLedType::PRESET:
        lv_label_set_text(type_help, lv_tr("Named actions in the LED control panel (long-press)."));
        break;
    }

    // When type changes, rebuild the edit controls
    auto* type_idx = new int(index);
    lv_obj_set_user_data(type_dd, type_idx);
    lv_obj_add_event_cb(
        type_dd,
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] macro_type_changed");
            auto* dd = static_cast<lv_obj_t*>(lv_event_get_target(e));
            auto* idx = static_cast<int*>(lv_obj_get_user_data(dd));
            if (idx) {
                auto& overlay = get_led_settings_overlay();
                auto& ctrl = helix::led::LedController::instance();
                auto updated = ctrl.configured_macros();
                if (*idx >= 0 && *idx < static_cast<int>(updated.size())) {
                    int sel = lv_dropdown_get_selected(dd);
                    switch (sel) {
                    case 0:
                        updated[*idx].type = helix::led::MacroLedType::ON_OFF;
                        break;
                    case 1:
                        updated[*idx].type = helix::led::MacroLedType::TOGGLE;
                        break;
                    case 2:
                        updated[*idx].type = helix::led::MacroLedType::PRESET;
                        break;
                    }
                    ctrl.set_configured_macros(updated);
                    // Rebuild cards to update macro field visibility
                    overlay.populate_macro_devices();
                }
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_VALUE_CHANGED, type_idx);
    lv_obj_add_event_cb(
        type_dd, [](lv_event_t* e) { delete static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, type_idx);

    // Build macro dropdown options from discovered macros
    std::string macro_options;
    for (const auto& m : discovered) {
        if (!macro_options.empty())
            macro_options += "\n";
        macro_options += m;
    }

    // Helper lambda to find index of a macro in discovered list
    auto find_macro_idx = [&discovered](const std::string& name) -> int {
        for (size_t j = 0; j < discovered.size(); j++) {
            if (discovered[j] == name)
                return static_cast<int>(j);
        }
        return 0;
    };

    if (discovered.empty()) {
        auto* no_macros = lv_label_create(container);
        lv_label_set_text(no_macros, lv_tr("No LED macros detected on your printer."));
        lv_label_set_long_mode(no_macros, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(no_macros, lv_pct(100));
        lv_obj_set_style_text_color(no_macros, theme_manager_get_color("text_subtle"), 0);
    } else {
        // --- Type-specific macro fields ---
        switch (macro.type) {
        case helix::led::MacroLedType::ON_OFF: {
            // On Macro dropdown
            const char* on_attrs[] = {"label", "On:", nullptr};
            auto* on_row =
                static_cast<lv_obj_t*>(lv_xml_create(container, "setting_form_dropdown", on_attrs));
            auto* on_dd = lv_obj_find_by_name(on_row, "dropdown");
            lv_dropdown_set_options(on_dd, macro_options.c_str());
            lv_obj_set_name(on_dd, "macro_on_dropdown");
            if (!macro.on_macro.empty()) {
                lv_dropdown_set_selected(on_dd, find_macro_idx(macro.on_macro));
            }

            // Off Macro dropdown
            const char* off_attrs[] = {"label", "Off:", nullptr};
            auto* off_row = static_cast<lv_obj_t*>(
                lv_xml_create(container, "setting_form_dropdown", off_attrs));
            auto* off_dd = lv_obj_find_by_name(off_row, "dropdown");
            lv_dropdown_set_options(off_dd, macro_options.c_str());
            lv_obj_set_name(off_dd, "macro_off_dropdown");
            if (!macro.off_macro.empty()) {
                lv_dropdown_set_selected(off_dd, find_macro_idx(macro.off_macro));
            }
            break;
        }
        case helix::led::MacroLedType::TOGGLE: {
            const char* toggle_attrs[] = {"label", "Toggle:", nullptr};
            auto* toggle_row = static_cast<lv_obj_t*>(
                lv_xml_create(container, "setting_form_dropdown", toggle_attrs));
            auto* toggle_dd = lv_obj_find_by_name(toggle_row, "dropdown");
            lv_dropdown_set_options(toggle_dd, macro_options.c_str());
            lv_obj_set_name(toggle_dd, "macro_toggle_dropdown");
            if (!macro.toggle_macro.empty()) {
                lv_dropdown_set_selected(toggle_dd, find_macro_idx(macro.toggle_macro));
            }
            break;
        }
        case helix::led::MacroLedType::PRESET: {
            // Preset rows: each with a macro dropdown + remove button
            for (int p = 0; p < static_cast<int>(macro.presets.size()); p++) {
                const auto& preset_macro = macro.presets[p];

                auto* preset_row = lv_obj_create(container);
                lv_obj_set_width(preset_row, lv_pct(100));
                lv_obj_set_height(preset_row, LV_SIZE_CONTENT);
                lv_obj_set_flex_flow(preset_row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(preset_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_pad_all(preset_row, 0, 0);
                lv_obj_set_style_pad_gap(preset_row, 4, 0);
                lv_obj_set_style_bg_opa(preset_row, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(preset_row, 0, 0);
                lv_obj_remove_flag(preset_row, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_name(preset_row,
                                fmt::format("macro_preset_row_{}_{}", index, p).c_str());

                // Preset macro dropdown
                auto* pmacro_dd = lv_dropdown_create(preset_row);
                lv_dropdown_set_options(pmacro_dd, macro_options.c_str());
                lv_obj_set_width(pmacro_dd, lv_pct(60));
                lv_obj_set_style_border_width(pmacro_dd, 0, 0);
                lv_obj_set_name(pmacro_dd, fmt::format("preset_macro_{}_{}", index, p).c_str());
                if (!preset_macro.empty()) {
                    lv_dropdown_set_selected(pmacro_dd, find_macro_idx(preset_macro));
                }

                // Remove preset button
                auto* remove_btn = lv_button_create(preset_row);
                lv_obj_set_size(remove_btn, 32, 32);
                lv_obj_set_style_bg_opa(remove_btn, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(remove_btn, 0, 0);
                const char* rm_icon_attrs[] = {"src",     "close",     "size", "xs",
                                               "variant", "secondary", nullptr};
                lv_xml_create(remove_btn, "icon", rm_icon_attrs);

                struct PresetRemoveData {
                    int device_idx;
                    int preset_idx;
                };
                auto* rm_data = new PresetRemoveData{index, p};
                lv_obj_set_user_data(remove_btn, rm_data);
                lv_obj_add_event_cb(
                    remove_btn,
                    [](lv_event_t* e) {
                        LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] remove_preset");
                        auto* data = static_cast<PresetRemoveData*>(lv_obj_get_user_data(
                            static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
                        if (data) {
                            auto& ctrl = helix::led::LedController::instance();
                            auto updated = ctrl.configured_macros();
                            if (data->device_idx >= 0 &&
                                data->device_idx < static_cast<int>(updated.size())) {
                                auto& presets = updated[data->device_idx].presets;
                                if (data->preset_idx >= 0 &&
                                    data->preset_idx < static_cast<int>(presets.size())) {
                                    presets.erase(presets.begin() + data->preset_idx);
                                    ctrl.set_configured_macros(updated);
                                    ctrl.save_config();

                                    // Rebuild macro backend so overlay sees the change
                                    ctrl.rebuild_macro_backend();

                                    get_led_settings_overlay().populate_macro_devices();
                                }
                            }
                        }
                        LVGL_SAFE_EVENT_CB_END();
                    },
                    LV_EVENT_CLICKED, rm_data);
                lv_obj_add_event_cb(
                    remove_btn,
                    [](lv_event_t* e) {
                        delete static_cast<PresetRemoveData*>(lv_event_get_user_data(e));
                    },
                    LV_EVENT_DELETE, rm_data);
            }

            // "Add Preset" button
            auto* add_preset_btn = lv_button_create(container);
            lv_obj_set_width(add_preset_btn, lv_pct(100));
            lv_obj_set_height(add_preset_btn, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(add_preset_btn, LV_OPA_10, 0);
            lv_obj_set_style_border_width(add_preset_btn, 1, 0);
            lv_obj_set_style_border_color(add_preset_btn, theme_manager_get_color("border"), 0);
            lv_obj_set_style_radius(add_preset_btn, 6, 0);
            lv_obj_set_style_pad_all(add_preset_btn, 8, 0);

            auto* add_preset_lbl = lv_label_create(add_preset_btn);
            lv_label_set_text(add_preset_lbl, "+ Add Preset");
            lv_obj_set_style_text_color(add_preset_lbl, primary_color, 0);
            lv_obj_center(add_preset_lbl);

            auto* ap_idx = new int(index);
            lv_obj_set_user_data(add_preset_btn, ap_idx);
            lv_obj_add_event_cb(
                add_preset_btn,
                [](lv_event_t* e) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] add_preset");
                    auto* idx = static_cast<int*>(lv_obj_get_user_data(
                        static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
                    if (idx) {
                        auto& ctrl = helix::led::LedController::instance();
                        auto updated = ctrl.configured_macros();
                        if (*idx >= 0 && *idx < static_cast<int>(updated.size())) {
                            updated[*idx].presets.emplace_back("");
                            ctrl.set_configured_macros(updated);
                            get_led_settings_overlay().populate_macro_devices();
                        }
                    }
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_CLICKED, ap_idx);
            lv_obj_add_event_cb(
                add_preset_btn,
                [](lv_event_t* e) { delete static_cast<int*>(lv_event_get_user_data(e)); },
                LV_EVENT_DELETE, ap_idx);
            break;
        }
        }
    }

    // --- Save button ---
    auto* save_row = lv_obj_create(container);
    lv_obj_set_width(save_row, lv_pct(100));
    lv_obj_set_height(save_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(save_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(save_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(save_row, 0, 0);
    lv_obj_set_style_bg_opa(save_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(save_row, 0, 0);
    lv_obj_remove_flag(save_row, LV_OBJ_FLAG_SCROLLABLE);

    auto* save_btn = lv_button_create(save_row);
    lv_obj_set_size(save_btn, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_bg_color(save_btn, primary_color, 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(save_btn, 6, 0);
    lv_obj_set_style_pad_left(save_btn, 16, 0);
    lv_obj_set_style_pad_right(save_btn, 16, 0);

    auto* save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, lv_tr("Save"));
    lv_obj_set_style_text_color(save_lbl, lv_color_white(), 0);
    lv_obj_center(save_lbl);

    auto* save_idx = new int(index);
    lv_obj_set_user_data(save_btn, save_idx);
    lv_obj_add_event_cb(
        save_btn,
        [](lv_event_t* e) {
            LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] save_macro_device");
            auto* idx = static_cast<int*>(
                lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
            if (idx) {
                get_led_settings_overlay().handle_save_macro_device(*idx);
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, save_idx);
    lv_obj_add_event_cb(
        save_btn, [](lv_event_t* e) { delete static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, save_idx);
}

// ============================================================================
// MACRO DEVICE HANDLERS
// ============================================================================

void LedSettingsOverlay::handle_add_macro_device() {
    spdlog::info("[{}] Adding new macro device", get_name());

    auto& ctrl = helix::led::LedController::instance();
    auto updated = ctrl.configured_macros();

    helix::led::LedMacroInfo new_device;
    new_device.display_name = "";
    new_device.type = helix::led::MacroLedType::ON_OFF;
    updated.push_back(new_device);

    ctrl.set_configured_macros(updated);
    ctrl.save_config();

    // Open in edit mode
    editing_macro_index_ = static_cast<int>(updated.size()) - 1;
    populate_macro_devices();
}

void LedSettingsOverlay::handle_edit_macro_device(int index) {
    spdlog::info("[{}] Editing macro device {}", get_name(), index);

    if (editing_macro_index_ == index) {
        // Toggle off
        editing_macro_index_ = -1;
    } else {
        editing_macro_index_ = index;
    }
    populate_macro_devices();
}

void LedSettingsOverlay::handle_delete_macro_device(int index) {
    auto& ctrl = helix::led::LedController::instance();
    auto updated = ctrl.configured_macros();

    if (index < 0 || index >= static_cast<int>(updated.size())) {
        spdlog::warn("[{}] Delete macro device: invalid index {}", get_name(), index);
        return;
    }

    spdlog::info("[{}] Deleting macro device {}: '{}'", get_name(), index,
                 updated[index].display_name);

    updated.erase(updated.begin() + index);
    ctrl.set_configured_macros(updated);
    ctrl.save_config();

    // Rebuild macro backend with remaining macros
    ctrl.rebuild_macro_backend();

    // Reset editing state
    editing_macro_index_ = -1;
    populate_macro_devices();
}

void LedSettingsOverlay::handle_save_macro_device(int index) {
    auto& ctrl = helix::led::LedController::instance();
    auto updated = ctrl.configured_macros();
    const auto& discovered = ctrl.discovered_macros();

    if (index < 0 || index >= static_cast<int>(updated.size())) {
        spdlog::warn("[{}] Save macro device: invalid index {}", get_name(), index);
        return;
    }

    if (!overlay_root_)
        return;

    // Find the edit container for this device
    std::string edit_name = fmt::format("macro_edit_{}", index);
    lv_obj_t* edit_container = lv_obj_find_by_name(overlay_root_, edit_name.c_str());
    if (!edit_container) {
        spdlog::warn("[{}] Cannot find edit container for device {}", get_name(), index);
        return;
    }

    // Read name from textarea
    lv_obj_t* name_ta = lv_obj_find_by_name(edit_container, "macro_name_input");
    if (name_ta) {
        updated[index].display_name = lv_textarea_get_text(name_ta);
    }

    // Read type from dropdown
    lv_obj_t* type_dd = lv_obj_find_by_name(edit_container, "macro_type_dropdown");
    if (type_dd) {
        int sel = lv_dropdown_get_selected(type_dd);
        switch (sel) {
        case 0:
            updated[index].type = helix::led::MacroLedType::ON_OFF;
            break;
        case 1:
            updated[index].type = helix::led::MacroLedType::TOGGLE;
            break;
        case 2:
            updated[index].type = helix::led::MacroLedType::PRESET;
            break;
        }
    }

    // Helper to get macro name from dropdown selection
    auto get_macro_from_dd = [&discovered](lv_obj_t* dd) -> std::string {
        if (!dd || discovered.empty())
            return "";
        int sel = lv_dropdown_get_selected(dd);
        if (sel >= 0 && sel < static_cast<int>(discovered.size())) {
            return discovered[sel];
        }
        return "";
    };

    // Read type-specific fields
    switch (updated[index].type) {
    case helix::led::MacroLedType::ON_OFF: {
        lv_obj_t* on_dd = lv_obj_find_by_name(edit_container, "macro_on_dropdown");
        lv_obj_t* off_dd = lv_obj_find_by_name(edit_container, "macro_off_dropdown");
        updated[index].on_macro = get_macro_from_dd(on_dd);
        updated[index].off_macro = get_macro_from_dd(off_dd);
        updated[index].toggle_macro.clear();
        updated[index].presets.clear();
        break;
    }
    case helix::led::MacroLedType::TOGGLE: {
        lv_obj_t* toggle_dd = lv_obj_find_by_name(edit_container, "macro_toggle_dropdown");
        updated[index].toggle_macro = get_macro_from_dd(toggle_dd);
        updated[index].on_macro.clear();
        updated[index].off_macro.clear();
        updated[index].presets.clear();
        break;
    }
    case helix::led::MacroLedType::PRESET: {
        updated[index].on_macro.clear();
        updated[index].off_macro.clear();
        updated[index].toggle_macro.clear();
        updated[index].presets.clear();

        // Read preset rows
        for (int p = 0; p < 50; p++) { // reasonable upper bound
            std::string pmacro_key = fmt::format("preset_macro_{}_{}", index, p);
            lv_obj_t* pmacro = lv_obj_find_by_name(overlay_root_, pmacro_key.c_str());
            if (!pmacro)
                break;

            std::string preset_macro = get_macro_from_dd(pmacro);
            updated[index].presets.emplace_back(preset_macro);
        }
        break;
    }
    }

    // --- Validation ---
    // Trim and check for empty name
    std::string display_name = updated[index].display_name;
    // Trim whitespace
    auto start = display_name.find_first_not_of(" \t\n\r");
    auto end = display_name.find_last_not_of(" \t\n\r");
    display_name = (start == std::string::npos) ? "" : display_name.substr(start, end - start + 1);
    updated[index].display_name = display_name;

    if (display_name.empty()) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Device name is required"));
        return;
    }

    // Check for duplicate on/off macros
    if (updated[index].type == helix::led::MacroLedType::ON_OFF &&
        updated[index].on_macro == updated[index].off_macro && !updated[index].on_macro.empty()) {
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("On and Off macros must be different"));
        return;
    }

    spdlog::info("[{}] Saved macro device {}: '{}' type={}", get_name(), index,
                 updated[index].display_name, static_cast<int>(updated[index].type));

    ctrl.set_configured_macros(updated);
    ctrl.save_config();

    // Rebuild macro backend
    ctrl.rebuild_macro_backend();

    // Exit edit mode
    editing_macro_index_ = -1;
    populate_macro_devices();
}

// ============================================================================
// LED CHIP SELECTION
// ============================================================================

void LedSettingsOverlay::populate_led_chips() {
    if (!overlay_root_)
        return;

    // SAFETY: Defer the clean+rebuild. Called from handle_led_chip_toggle() where
    // the clicked chip is a child of chip_container being cleaned (issue #80).
    helix::ui::queue_update([this]() { populate_led_chips_impl(); });
}

void LedSettingsOverlay::populate_led_chips_impl() {
    if (!overlay_root_)
        return;

    lv_obj_t* led_chip_row = lv_obj_find_by_name(overlay_root_, "row_led_select");
    if (!led_chip_row)
        return;

    lv_obj_t* chip_container = lv_obj_find_by_name(led_chip_row, "chip_container");
    if (!chip_container || !lv_obj_is_valid(chip_container)) {
        spdlog::warn("[{}] LED chip row found but chip_container invalid/missing", get_name());
        return;
    }

    lv_obj_clean(chip_container);

    // Source LED list from all backends (native + WLED + macros)
    auto& led_ctrl = helix::led::LedController::instance();
    discovered_leds_.clear();
    for (const auto& strip : led_ctrl.all_selectable_strips()) {
        discovered_leds_.push_back(strip.id);
    }

    // Load selected LEDs from LedController
    selected_leds_.clear();
    for (const auto& strip_id : led_ctrl.selected_strips()) {
        selected_leds_.insert(strip_id);
    }

    // Create chips for each discovered LED
    for (const auto& led : discovered_leds_) {
        bool selected = selected_leds_.count(led) > 0;
        std::string display_name = helix::get_display_name(led, helix::DeviceType::LED);

        helix::ui::create_led_chip(
            chip_container, led, display_name, selected,
            [this](const std::string& led_name) { handle_led_chip_clicked(led_name); });
    }

    spdlog::debug("[{}] LED chips populated ({} LEDs, {} selected)", get_name(),
                  discovered_leds_.size(), selected_leds_.size());
}

void LedSettingsOverlay::handle_led_chip_clicked(const std::string& led_name) {
    // Toggle selection
    if (selected_leds_.count(led_name) > 0) {
        selected_leds_.erase(led_name);
        spdlog::info("[{}] LED deselected: {}", get_name(), led_name);
    } else {
        selected_leds_.insert(led_name);
        spdlog::info("[{}] LED selected: {}", get_name(), led_name);
    }

    // Save via LedController
    std::vector<std::string> strips_vec(selected_leds_.begin(), selected_leds_.end());
    helix::led::LedController::instance().set_selected_strips(strips_vec);
    helix::led::LedController::instance().save_config();

    // Rebuild chips to update visual state
    populate_led_chips();
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void LedSettingsOverlay::handle_led_on_at_start_changed(bool enabled) {
    spdlog::info("[{}] LED on at start toggled: {}", get_name(), enabled ? "ON" : "OFF");
    helix::led::LedController::instance().set_led_on_at_start(enabled);
    helix::led::LedController::instance().save_config();
}

void LedSettingsOverlay::handle_auto_state_changed(bool enabled) {
    spdlog::info("[{}] Auto state toggled: {}", get_name(), enabled ? "ON" : "OFF");
    helix::led::LedAutoState::instance().set_enabled(enabled);
    helix::led::LedAutoState::instance().save_config();

    // Update visibility subject so the rows container shows/hides
    lv_subject_set_int(&auto_state_enabled_subject_, enabled ? 1 : 0);

    // Populate rows when enabling (they may not exist yet)
    if (enabled) {
        populate_auto_state_rows();
    }
}

// ============================================================================
// AUTO-STATE MAPPING EDITOR
// ============================================================================

/// State row definition table
struct StateRowInfo {
    const char* key;
    const char* display_name;
    const char* icon;
};

static constexpr StateRowInfo STATE_ROWS[] = {
    {"idle", "Idle", "sleep"},
    {"heating", "Heating", "fire"},
    {"printing", "Printing", "printer_3d"},
    {"paused", "Paused", "pause"},
    {"error", "Error", "alert_circle"},
    {"complete", "Complete", "check_circle"},
};

void LedSettingsOverlay::populate_auto_state_rows() {
    if (!overlay_root_)
        return;

    lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "auto_state_rows_container");
    if (!container)
        return;

    // Clear existing rows
    lv_obj_clean(container);

    auto& auto_state = helix::led::LedAutoState::instance();

    // Build capability-filtered action type options (shared across all rows)
    action_type_options_.clear();
    action_type_options_.push_back("off");
    action_type_options_.push_back("brightness");

    auto& ctrl = helix::led::LedController::instance();
    bool has_color = false;
    for (const auto& strip_id : ctrl.selected_strips()) {
        for (const auto& s : ctrl.native().strips()) {
            if (s.id == strip_id && s.supports_color) {
                has_color = true;
                break;
            }
        }
        if (has_color)
            break;
    }
    if (has_color)
        action_type_options_.push_back("color");
    if (ctrl.effects().is_available())
        action_type_options_.push_back("effect");
    if (ctrl.wled().is_available())
        action_type_options_.push_back("wled_preset");
    if (ctrl.macro().is_available())
        action_type_options_.push_back("macro");

    // Build dropdown options string
    std::string options_str;
    for (size_t i = 0; i < action_type_options_.size(); i++) {
        if (i > 0)
            options_str += "\n";
        const auto& opt = action_type_options_[i];
        if (opt == "off")
            options_str += "Off";
        else if (opt == "brightness")
            options_str += "Brightness";
        else if (opt == "color")
            options_str += "Color";
        else if (opt == "effect")
            options_str += "Effect";
        else if (opt == "wled_preset")
            options_str += "WLED Preset";
        else if (opt == "macro")
            options_str += "Macro";
    }

    for (const auto& state : STATE_ROWS) {
        std::string key = state.key;

        // Get current action for this state
        const auto* mapping = auto_state.get_mapping(key);
        helix::led::LedStateAction action;
        if (mapping) {
            action = *mapping;
        }

        // --- Main row: icon + label + dropdown (always visible) ---
        const char* row_attrs[] = {"label", state.display_name, "icon", state.icon, nullptr};
        auto* row =
            static_cast<lv_obj_t*>(lv_xml_create(container, "setting_state_row", row_attrs));
        auto* dropdown = lv_obj_find_by_name(row, "dropdown");
        lv_dropdown_set_options(dropdown, options_str.c_str());

        // Set dropdown to current action type
        for (size_t i = 0; i < action_type_options_.size(); i++) {
            if (action_type_options_[i] == action.action_type) {
                lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(i));
                break;
            }
        }

        // Store key + options for callbacks
        struct DropdownData {
            std::string state_key;
            std::vector<std::string> options;
        };
        auto* dd_data = new DropdownData{key, action_type_options_};
        lv_obj_set_user_data(dropdown, dd_data);

        lv_obj_add_event_cb(
            dropdown,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] action_type_changed");
                auto* dd = static_cast<lv_obj_t*>(lv_event_get_target(e));
                auto* data = static_cast<DropdownData*>(lv_obj_get_user_data(dd));
                if (data) {
                    int idx = lv_dropdown_get_selected(dd);
                    get_led_settings_overlay().handle_action_type_changed(data->state_key, idx);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_VALUE_CHANGED, dd_data);

        lv_obj_add_event_cb(
            dropdown,
            [](lv_event_t* e) { delete static_cast<DropdownData*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, dd_data);

        // --- Detail row (shown/hidden based on action type) ---
        bool needs_detail = (action.action_type != "off" && !action.action_type.empty());

        auto* detail =
            static_cast<lv_obj_t*>(lv_xml_create(container, "setting_detail_panel", nullptr));
        lv_obj_set_name(detail, fmt::format("detail_{}", key).c_str());
        auto* ctx_container = lv_obj_find_by_name(detail, "controls");
        lv_obj_set_name(ctx_container, fmt::format("ctx_{}", key).c_str());

        if (!needs_detail) {
            lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
        }

        // Populate contextual controls for current action type
        rebuild_contextual_controls(key, ctx_container);
    }

    spdlog::debug("[{}] Auto-state rows populated ({} states)", get_name(),
                  sizeof(STATE_ROWS) / sizeof(STATE_ROWS[0]));
}

void LedSettingsOverlay::rebuild_contextual_controls(const std::string& state_key,
                                                     lv_obj_t* container) {
    if (!container)
        return;

    lv_obj_clean(container);

    auto& auto_state = helix::led::LedAutoState::instance();
    const auto* mapping = auto_state.get_mapping(state_key);
    helix::led::LedStateAction action;
    if (mapping) {
        action = *mapping;
    }

    if (action.action_type == "off" || action.action_type.empty()) {
        // No controls needed for "Off"
        return;
    }

    if (action.action_type == "brightness") {
        // Brightness slider
        lv_obj_t* slider = lv_slider_create(container);
        lv_obj_set_width(slider, lv_pct(100));
        lv_slider_set_range(slider, 0, 100);
        lv_slider_set_value(slider, action.brightness, LV_ANIM_OFF);

        auto* slider_key = new std::string(state_key);
        lv_obj_set_user_data(slider, slider_key);

        lv_obj_add_event_cb(
            slider,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] brightness_changed");
                auto* s = static_cast<lv_obj_t*>(lv_event_get_target(e));
                auto* data = static_cast<std::string*>(lv_obj_get_user_data(s));
                if (data) {
                    int value = lv_slider_get_value(s);
                    get_led_settings_overlay().handle_brightness_changed(*data, value);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_VALUE_CHANGED, slider_key);

        lv_obj_add_event_cb(
            slider,
            [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, slider_key);

        return;
    }

    if (action.action_type == "color") {
        // Color preset swatches row
        lv_obj_t* swatch_row = lv_obj_create(container);
        lv_obj_set_width(swatch_row, lv_pct(100));
        lv_obj_set_height(swatch_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(swatch_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(swatch_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(swatch_row, 8, 0);
        lv_obj_set_style_pad_all(swatch_row, 0, 0);
        lv_obj_set_style_bg_opa(swatch_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(swatch_row, 0, 0);
        lv_obj_remove_flag(swatch_row, LV_OBJ_FLAG_SCROLLABLE);

        auto& ctrl = helix::led::LedController::instance();
        const auto& presets = ctrl.color_presets();

        for (uint32_t color : presets) {
            lv_obj_t* swatch = lv_obj_create(swatch_row);
            lv_obj_set_size(swatch, 32, 32);
            lv_obj_set_style_radius(swatch, 4, 0);
            lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(
                swatch, lv_color_make((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF), 0);
            lv_obj_set_style_border_width(swatch, 1, 0);
            lv_obj_set_style_border_color(swatch, theme_manager_get_color("border"), 0);
            lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);

            // Highlight the active color
            if (color == action.color) {
                lv_obj_set_style_border_width(swatch, 2, 0);
                lv_obj_set_style_border_color(swatch, theme_manager_get_color("primary"), 0);
            }

            struct SwatchData {
                std::string key;
                uint32_t color;
            };
            auto* sdata = new SwatchData{state_key, color};
            lv_obj_set_user_data(swatch, sdata);

            lv_obj_add_event_cb(
                swatch,
                [](lv_event_t* e) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] color_selected");
                    auto* data = static_cast<SwatchData*>(lv_obj_get_user_data(
                        static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
                    if (data) {
                        get_led_settings_overlay().handle_color_selected(data->key, data->color);
                    }
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_CLICKED, sdata);

            lv_obj_add_event_cb(
                swatch,
                [](lv_event_t* e) { delete static_cast<SwatchData*>(lv_event_get_user_data(e)); },
                LV_EVENT_DELETE, sdata);
        }

        // Brightness slider below color swatches
        lv_obj_t* slider = lv_slider_create(container);
        lv_obj_set_width(slider, lv_pct(100));
        lv_slider_set_range(slider, 0, 100);
        lv_slider_set_value(slider, action.brightness, LV_ANIM_OFF);

        auto* slider_key = new std::string(state_key);
        lv_obj_set_user_data(slider, slider_key);

        lv_obj_add_event_cb(
            slider,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] color_brightness_changed");
                auto* s = static_cast<lv_obj_t*>(lv_event_get_target(e));
                auto* data = static_cast<std::string*>(lv_obj_get_user_data(s));
                if (data) {
                    int value = lv_slider_get_value(s);
                    get_led_settings_overlay().handle_brightness_changed(*data, value);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_VALUE_CHANGED, slider_key);

        lv_obj_add_event_cb(
            slider,
            [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, slider_key);

        return;
    }

    if (action.action_type == "effect") {
        auto& ctrl = helix::led::LedController::instance();
        const auto& effects = ctrl.effects().effects();

        if (effects.empty()) {
            lv_obj_t* label = lv_label_create(container);
            lv_label_set_text(label, lv_tr("No effects available"));
            lv_obj_set_style_text_color(label, theme_manager_get_color("text_subtle"), 0);
            return;
        }

        // Build dropdown options
        lv_obj_t* dropdown = lv_dropdown_create(container);
        std::string opts;
        int selected_idx = 0;
        for (size_t i = 0; i < effects.size(); i++) {
            if (i > 0)
                opts += "\n";
            opts += effects[i].display_name;
            if (effects[i].name == action.effect_name) {
                selected_idx = static_cast<int>(i);
            }
        }
        lv_dropdown_set_options(dropdown, opts.c_str());
        lv_dropdown_set_selected(dropdown, selected_idx);
        lv_obj_set_width(dropdown, lv_pct(100));
        lv_obj_set_style_border_width(dropdown, 0, 0);

        auto* dd_key = new std::string(state_key);
        lv_obj_set_user_data(dropdown, dd_key);

        lv_obj_add_event_cb(
            dropdown,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] effect_selected");
                auto* dd = static_cast<lv_obj_t*>(lv_event_get_target(e));
                auto* data = static_cast<std::string*>(lv_obj_get_user_data(dd));
                if (data) {
                    int idx = lv_dropdown_get_selected(dd);
                    auto& ctrl = helix::led::LedController::instance();
                    const auto& effs = ctrl.effects().effects();
                    if (idx >= 0 && idx < static_cast<int>(effs.size())) {
                        get_led_settings_overlay().handle_effect_selected(*data, effs[idx].name);
                    }
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_VALUE_CHANGED, dd_key);

        lv_obj_add_event_cb(
            dropdown,
            [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, dd_key);

        return;
    }

    if (action.action_type == "wled_preset") {
        // Simple spinbox / text for preset ID
        lv_obj_t* label = lv_label_create(container);
        lv_label_set_text(label, fmt::format("Preset ID: {}", action.wled_preset).c_str());
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);

        lv_obj_t* slider = lv_slider_create(container);
        lv_obj_set_width(slider, lv_pct(100));
        lv_slider_set_range(slider, 1, 50);
        lv_slider_set_value(slider, action.wled_preset > 0 ? action.wled_preset : 1, LV_ANIM_OFF);

        struct WledSliderData {
            std::string key;
            lv_obj_t* label;
        };
        auto* ws_data = new WledSliderData{state_key, label};
        lv_obj_set_user_data(slider, ws_data);

        lv_obj_add_event_cb(
            slider,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] wled_preset_changed");
                auto* s = static_cast<lv_obj_t*>(lv_event_get_target(e));
                auto* data = static_cast<WledSliderData*>(lv_obj_get_user_data(s));
                if (data) {
                    int value = lv_slider_get_value(s);
                    lv_label_set_text(data->label, fmt::format("Preset ID: {}", value).c_str());
                    get_led_settings_overlay().handle_wled_preset_selected(data->key, value);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_VALUE_CHANGED, ws_data);

        lv_obj_add_event_cb(
            slider,
            [](lv_event_t* e) { delete static_cast<WledSliderData*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, ws_data);

        return;
    }

    if (action.action_type == "macro") {
        auto& ctrl = helix::led::LedController::instance();
        const auto& macros = ctrl.macro().macros();

        if (macros.empty()) {
            lv_obj_t* label = lv_label_create(container);
            lv_label_set_text(label, lv_tr("No macros available"));
            lv_obj_set_style_text_color(label, theme_manager_get_color("text_subtle"), 0);
            return;
        }

        lv_obj_t* dropdown = lv_dropdown_create(container);
        std::string opts;
        int selected_idx = 0;
        for (size_t i = 0; i < macros.size(); i++) {
            if (i > 0)
                opts += "\n";
            opts += macros[i].display_name;
            if (macros[i].on_macro == action.macro_gcode ||
                macros[i].toggle_macro == action.macro_gcode) {
                selected_idx = static_cast<int>(i);
            }
        }
        lv_dropdown_set_options(dropdown, opts.c_str());
        lv_dropdown_set_selected(dropdown, selected_idx);
        lv_obj_set_width(dropdown, lv_pct(100));
        lv_obj_set_style_border_width(dropdown, 0, 0);

        auto* dd_key = new std::string(state_key);
        lv_obj_set_user_data(dropdown, dd_key);

        lv_obj_add_event_cb(
            dropdown,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] macro_selected");
                auto* dd = static_cast<lv_obj_t*>(lv_event_get_target(e));
                auto* data = static_cast<std::string*>(lv_obj_get_user_data(dd));
                if (data) {
                    int idx = lv_dropdown_get_selected(dd);
                    auto& ctrl = helix::led::LedController::instance();
                    const auto& ms = ctrl.macro().macros();
                    if (idx >= 0 && idx < static_cast<int>(ms.size())) {
                        // Use on_macro or toggle_macro as the gcode identifier
                        std::string gcode =
                            ms[idx].on_macro.empty() ? ms[idx].toggle_macro : ms[idx].on_macro;
                        get_led_settings_overlay().handle_macro_selected(*data, gcode);
                    }
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_VALUE_CHANGED, dd_key);

        lv_obj_add_event_cb(
            dropdown,
            [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
            LV_EVENT_DELETE, dd_key);

        return;
    }
}

void LedSettingsOverlay::handle_action_type_changed(const std::string& state_key,
                                                    int dropdown_index) {
    // Use the stored action_type_options_ to map dropdown index to type string
    if (dropdown_index < 0 || dropdown_index >= static_cast<int>(action_type_options_.size())) {
        return;
    }

    std::string new_type = action_type_options_[dropdown_index];
    spdlog::info("[{}] Action type changed for '{}': {}", get_name(), state_key, new_type);

    // Build new action with sensible defaults
    helix::led::LedStateAction action;
    action.action_type = new_type;

    if (new_type == "brightness") {
        action.brightness = 100;
    } else if (new_type == "color") {
        action.color = 0xFFFFFF;
        action.brightness = 100;
    } else if (new_type == "effect") {
        auto& ctrl = helix::led::LedController::instance();
        const auto& effects = ctrl.effects().effects();
        if (!effects.empty()) {
            action.effect_name = effects[0].name;
        }
    } else if (new_type == "wled_preset") {
        action.wled_preset = 1;
    } else if (new_type == "macro") {
        auto& ctrl = helix::led::LedController::instance();
        const auto& macros = ctrl.macro().macros();
        if (!macros.empty()) {
            action.macro_gcode =
                macros[0].on_macro.empty() ? macros[0].toggle_macro : macros[0].on_macro;
        }
    }

    helix::led::LedAutoState::instance().set_mapping(state_key, action);
    save_and_evaluate(state_key);

    // Show or hide the detail row based on new action type
    std::string detail_name = fmt::format("detail_{}", state_key);
    lv_obj_t* detail = lv_obj_find_by_name(overlay_root_, detail_name.c_str());
    if (detail) {
        if (new_type == "off") {
            lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(detail, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // SAFETY: Defer rebuild — the dropdown that fired this VALUE_CHANGED event is a
    // child of the container that rebuild_contextual_controls() will clean (issue #80).
    helix::ui::queue_update([this, state_key]() {
        if (!overlay_root_)
            return;
        std::string ctx_name = fmt::format("ctx_{}", state_key);
        lv_obj_t* ctx = lv_obj_find_by_name(overlay_root_, ctx_name.c_str());
        rebuild_contextual_controls(state_key, ctx);
    });
}

void LedSettingsOverlay::handle_brightness_changed(const std::string& state_key, int value) {
    auto& auto_state = helix::led::LedAutoState::instance();
    const auto* existing = auto_state.get_mapping(state_key);
    helix::led::LedStateAction action;
    if (existing) {
        action = *existing;
    }
    action.brightness = value;
    auto_state.set_mapping(state_key, action);
    save_and_evaluate(state_key);
}

void LedSettingsOverlay::handle_color_selected(const std::string& state_key, uint32_t color) {
    spdlog::info("[{}] Color selected for '{}': 0x{:06X}", get_name(), state_key, color);

    auto& auto_state = helix::led::LedAutoState::instance();
    const auto* existing = auto_state.get_mapping(state_key);
    helix::led::LedStateAction action;
    if (existing) {
        action = *existing;
    }
    action.action_type = "color";
    action.color = color;
    auto_state.set_mapping(state_key, action);
    save_and_evaluate(state_key);

    // SAFETY: Defer rebuild — the clicked color swatch is a child of the container
    // that rebuild_contextual_controls() will clean. Destroying it mid-callback
    // causes use-after-free (see issue #80).
    helix::ui::queue_update([this, state_key]() {
        if (!overlay_root_)
            return;
        std::string ctx_name = fmt::format("ctx_{}", state_key);
        lv_obj_t* ctx = lv_obj_find_by_name(overlay_root_, ctx_name.c_str());
        rebuild_contextual_controls(state_key, ctx);
    });
}

void LedSettingsOverlay::handle_effect_selected(const std::string& state_key,
                                                const std::string& name) {
    spdlog::info("[{}] Effect selected for '{}': {}", get_name(), state_key, name);

    auto& auto_state = helix::led::LedAutoState::instance();
    helix::led::LedStateAction action;
    action.action_type = "effect";
    action.effect_name = name;
    auto_state.set_mapping(state_key, action);
    save_and_evaluate(state_key);
}

void LedSettingsOverlay::handle_wled_preset_selected(const std::string& state_key, int preset_id) {
    spdlog::info("[{}] WLED preset selected for '{}': {}", get_name(), state_key, preset_id);

    auto& auto_state = helix::led::LedAutoState::instance();
    helix::led::LedStateAction action;
    action.action_type = "wled_preset";
    action.wled_preset = preset_id;
    auto_state.set_mapping(state_key, action);
    save_and_evaluate(state_key);
}

void LedSettingsOverlay::handle_macro_selected(const std::string& state_key,
                                               const std::string& gcode) {
    spdlog::info("[{}] Macro selected for '{}': {}", get_name(), state_key, gcode);

    auto& auto_state = helix::led::LedAutoState::instance();
    helix::led::LedStateAction action;
    action.action_type = "macro";
    action.macro_gcode = gcode;
    auto_state.set_mapping(state_key, action);
    save_and_evaluate(state_key);
}

void LedSettingsOverlay::save_and_evaluate(const std::string& state_key) {
    (void)state_key; // Used for logging context only
    helix::led::LedAutoState::instance().save_config();
    helix::led::LedAutoState::instance().evaluate();
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void LedSettingsOverlay::on_led_on_at_start_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] on_led_on_at_start_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_led_settings_overlay().handle_led_on_at_start_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void LedSettingsOverlay::on_auto_state_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] on_auto_state_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_led_settings_overlay().handle_auto_state_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void LedSettingsOverlay::on_add_macro_device(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedSettingsOverlay] on_add_macro_device");
    (void)e;
    get_led_settings_overlay().handle_add_macro_device();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
