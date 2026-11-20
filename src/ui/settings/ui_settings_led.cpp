// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_led.cpp
 * @brief Implementation of LedSettingsOverlay
 */

#include "ui_settings_led.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_led_chip_factory.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "core/update_queue.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "lvgl.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace helix::settings {

using helix::ui::event_checked;
using helix::ui::find_required;

/// State row definition table
struct StateRowInfo {
    const char* key;
    const char* display_name;
    const char* icon;
};

static constexpr StateRowInfo kStateRows[] = {
    {"idle", "Idle", "sleep"},
    {"heating", "Heating", "fire"},
    {"printing", "Printing", "printer_3d"},
    {"paused", "Paused", "pause"},
    {"error", "Error", "alert_circle"},
    {"complete", "Complete", "check_circle"},
};

static constexpr size_t kStateRowCount = sizeof(kStateRows) / sizeof(kStateRows[0]);

LedSettingsOverlay::~LedSettingsOverlay() {
    discard_unsaved_macro_entry();
}

void LedSettingsOverlay::init_subjects() {
    UI_MANAGED_SUBJECT_INT(auto_state_enabled_subject_, 0, "led_auto_state_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(led_on_at_start_subject_, 0, "led_on_at_start_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(startup_brightness_subject_, 0, "led_startup_brightness", subjects_);
    UI_MANAGED_SUBJECT_INT(print_macros_drive_leds_subject_, 0, "led_print_macros_drive_leds",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(auto_preset_count_, 0, "led_auto_preset_count", subjects_);

    UI_MANAGED_SUBJECT_STRING(macro_draft_name_, macro_draft_name_buf_, "", "led_macro_draft_name",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(macro_draft_type_, 0, "led_macro_draft_type", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_draft_on_sel_, 0, "led_macro_draft_on_sel", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_draft_off_sel_, 0, "led_macro_draft_off_sel", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_draft_toggle_sel_, 0, "led_macro_draft_toggle_sel", subjects_);
    UI_MANAGED_SUBJECT_STRING(macro_draft_on_text_, macro_draft_on_text_buf_, "",
                              "led_macro_draft_on_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(macro_draft_off_text_, macro_draft_off_text_buf_, "",
                              "led_macro_draft_off_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(macro_draft_toggle_text_, macro_draft_toggle_text_buf_, "",
                              "led_macro_draft_toggle_text", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_draft_preset_count_, 0, "led_macro_draft_preset_count", subjects_);
    UI_MANAGED_SUBJECT_STRING(macro_field_options_, macro_field_options_buf_, "",
                              "led_macro_field_options", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_count_, 0, "led_macro_count", subjects_);
    UI_MANAGED_SUBJECT_INT(macro_discovered_count_, 0, "led_macro_discovered_count", subjects_);

    UI_MANAGED_SUBJECT_STRING(auto_action_options_, auto_action_options_buf_, "",
                              "led_auto_action_options", subjects_);
    UI_MANAGED_SUBJECT_STRING(auto_effect_options_, auto_effect_options_buf_, "",
                              "led_auto_effect_options", subjects_);
    UI_MANAGED_SUBJECT_INT(auto_effect_count_, 0, "led_auto_effect_count", subjects_);
    UI_MANAGED_SUBJECT_STRING(auto_macro_options_, auto_macro_options_buf_, "",
                              "led_auto_macro_options", subjects_);
    UI_MANAGED_SUBJECT_INT(auto_macro_count_, 0, "led_auto_macro_count", subjects_);
    // The rows are static XML, so every slot they bind exists before the overlay is built.
    for (auto* pool :
         {&auto_action_pool_, &auto_kind_pool_, &auto_brightness_pool_, &auto_effect_sel_pool_,
          &auto_wled_pool_, &auto_wled_label_pool_, &auto_macro_sel_pool_,
          &auto_color_selected_pool_, &auto_detail_shown_pool_}) {
        pool->ensure_size(kStateRowCount);
    }
}

namespace {
int event_slider_value(lv_event_t* e) {
    return lv_slider_get_value(lv_event_get_current_target_obj(e));
}
} // namespace

void LedSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_led_on_at_start_changed",
         [](lv_event_t* e) {
             get_led_settings_overlay().handle_led_on_at_start_changed(event_checked(e));
         }},
        {"on_startup_brightness_changed",
         [](lv_event_t* e) {
             get_led_settings_overlay().handle_startup_brightness_changed(event_slider_value(e));
         }},
        {"on_startup_brightness_commit",
         [](lv_event_t* e) {
             get_led_settings_overlay().handle_startup_brightness_commit(event_slider_value(e));
         }},
        {"on_auto_state_changed",
         [](lv_event_t* e) {
             get_led_settings_overlay().handle_auto_state_changed(event_checked(e));
         }},
        {"on_add_macro_device",
         [](lv_event_t*) { get_led_settings_overlay().handle_add_macro_device(); }},
        {"led_auto_action_cb",
         [](lv_event_t* e) {
             if (auto row = helix::ui::event_user_int(e);
                 row && *row >= 0 && *row < static_cast<int>(kStateRowCount)) {
                 get_led_settings_overlay().handle_action_type_changed(
                     kStateRows[*row].key, helix::ui::event_selected(e));
             }
         }},
        {"led_auto_brightness_cb",
         [](lv_event_t* e) {
             if (auto row = helix::ui::event_user_int(e);
                 row && *row >= 0 && *row < static_cast<int>(kStateRowCount)) {
                 get_led_settings_overlay().handle_brightness_changed(kStateRows[*row].key,
                                                                      event_slider_value(e));
             }
         }},
        {"led_auto_effect_cb",
         [](lv_event_t* e) {
             auto row = helix::ui::event_user_int(e);
             const auto& effects = helix::led::LedController::instance().effects().effects();
             const int sel = helix::ui::event_selected(e);
             if (row && *row >= 0 && *row < static_cast<int>(kStateRowCount) && sel >= 0 &&
                 sel < static_cast<int>(effects.size())) {
                 get_led_settings_overlay().handle_effect_selected(kStateRows[*row].key,
                                                                   effects[sel].name);
             }
         }},
        {"led_auto_wled_cb",
         [](lv_event_t* e) {
             if (auto row = helix::ui::event_user_int(e);
                 row && *row >= 0 && *row < static_cast<int>(kStateRowCount)) {
                 get_led_settings_overlay().handle_wled_preset_selected(kStateRows[*row].key,
                                                                        event_slider_value(e));
             }
         }},
        {"led_auto_macro_cb",
         [](lv_event_t* e) {
             auto row = helix::ui::event_user_int(e);
             const auto& ms = helix::led::LedController::instance().macro().macros();
             const int sel = helix::ui::event_selected(e);
             if (row && *row >= 0 && *row < static_cast<int>(kStateRowCount) && sel >= 0 &&
                 sel < static_cast<int>(ms.size())) {
                 // on_macro or toggle_macro is the gcode identifier
                 get_led_settings_overlay().handle_macro_selected(
                     kStateRows[*row].key,
                     ms[sel].on_macro.empty() ? ms[sel].toggle_macro : ms[sel].on_macro);
             }
         }},
        {"led_macro_edit_cb",
         [](lv_event_t* e) {
             if (auto i = helix::ui::event_user_int(e))
                 get_led_settings_overlay().handle_edit_macro_device(*i);
         }},
        {"led_macro_delete_cb",
         [](lv_event_t* e) {
             if (auto i = helix::ui::event_user_int(e))
                 get_led_settings_overlay().handle_delete_macro_device(*i);
         }},
        {"led_macro_save_cb",
         [](lv_event_t*) {
             auto& overlay = get_led_settings_overlay();
             overlay.handle_save_macro_device(overlay.editing_macro_index_);
         }},
        {"led_macro_add_preset_cb",
         [](lv_event_t*) { get_led_settings_overlay().handle_add_macro_preset(); }},
        {"led_macro_preset_remove_cb",
         [](lv_event_t* e) {
             if (auto i = helix::ui::event_user_int(e))
                 get_led_settings_overlay().handle_remove_macro_preset(*i);
         }},
        {"led_auto_color_swatch_cb",
         [](lv_event_t* e) {
             get_led_settings_overlay().handle_color_swatch_clicked(
                 lv_event_get_current_target_obj(e), helix::ui::event_user_int(e).value_or(-1));
         }},
    });
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

// ============================================================================
// INTERNAL METHODS
// ============================================================================

void LedSettingsOverlay::init_led_on_at_start_toggle() {
    if (!overlay_root_)
        return;

    auto& ctrl = helix::led::LedController::instance();

    // The toggle row binds this subject, and the slider row hides on it
    lv_subject_set_int(&led_on_at_start_subject_, ctrl.get_led_on_at_start() ? 1 : 0);
    // The slider row's readout binds this one
    lv_subject_set_int(&startup_brightness_subject_, ctrl.get_startup_brightness());

    lv_obj_t* brightness_row = find_required(overlay_root_, "row_startup_brightness", get_name());
    if (brightness_row) {
        lv_obj_t* slider = helix::ui::find_required(brightness_row, "slider", get_name());
        if (slider) {
            lv_slider_set_value(slider, ctrl.get_startup_brightness(), LV_ANIM_OFF);
        }
    }
}

void LedSettingsOverlay::init_auto_state_toggle() {
    // The toggle row binds this subject, and the rows container hides on it
    lv_subject_set_int(&auto_state_enabled_subject_,
                       helix::led::LedAutoState::instance().is_enabled() ? 1 : 0);
    lv_subject_set_int(&print_macros_drive_leds_subject_,
                       get_printer_state().get_discovery().print_macros_drive_leds() ? 1 : 0);
}

void LedSettingsOverlay::populate_macro_devices() {
    if (!overlay_root_)
        return;

    // SAFETY: Defer the clean+rebuild. This function is called from many event
    // callbacks (dropdown change, remove/add/edit/save buttons) where the event-
    // firing widget is a child of the container being cleaned (issue #80).
    lifetime_.defer("LedSettingsOverlay::populate_macro_devices", [this]() {
        if (cleanup_called())
            return;
        populate_macro_devices_impl();
    });
}

namespace {

const char* macro_type_label(helix::led::MacroLedType type) {
    switch (type) {
    case helix::led::MacroLedType::ON_OFF:
        return lv_tr("On/Off");
    case helix::led::MacroLedType::TOGGLE:
        return lv_tr("Toggle");
    case helix::led::MacroLedType::PRESET:
        return lv_tr("Preset");
    }
    return "";
}

/// The type dropdown's rows, in led_macro_edit_form.xml order.
constexpr helix::led::MacroLedType kMacroTypeRows[] = {helix::led::MacroLedType::ON_OFF,
                                                       helix::led::MacroLedType::TOGGLE,
                                                       helix::led::MacroLedType::PRESET};

int macro_type_row(helix::led::MacroLedType type) {
    for (int i = 0; i < 3; ++i) {
        if (kMacroTypeRows[i] == type)
            return i;
    }
    return 0;
}

} // namespace

void LedSettingsOverlay::populate_macro_devices_impl() {
    if (!overlay_root_)
        return;

    lv_obj_t* container = find_required(overlay_root_, "macro_devices_container", get_name());
    if (!container)
        return;

    helix::ui::safe_clean_children(container);

    auto& led_ctrl = helix::led::LedController::instance();
    const auto& macros = led_ctrl.configured_macros();
    const auto& discovered = led_ctrl.discovered_macros();

    // Every macro field offers what discovery found, then "Custom..." for the
    // names keyword detection misses.
    std::string options;
    for (const auto& m : discovered) {
        options += m;
        options += "\n";
    }
    options += lv_tr("Custom...");
    lv_subject_set_string(&macro_field_options_, options.c_str());
    lv_subject_set_int(&macro_discovered_count_, static_cast<int>(discovered.size()));
    lv_subject_set_int(&macro_count_, static_cast<int>(macros.size()));

    for (int i = 0; i < static_cast<int>(macros.size()); i++) {
        const auto& macro = macros[i];
        const std::string index = std::to_string(i);
        const std::string summary = helix::led::macro_device_note(macro);
        const char* attrs[] = {
            "index",
            index.c_str(),
            "device_name",
            macro.display_name.empty() ? lv_tr("(unnamed)") : macro.display_name.c_str(),
            "type_label",
            macro_type_label(macro.type),
            "summary",
            summary.c_str(),
            "editing",
            i == editing_macro_index_ ? "1" : "0",
            nullptr,
        };
        if (!lv_xml_create(container, "setting_macro_card", attrs)) {
            spdlog::error("[{}] Failed to build macro card for device {} from XML", get_name(), i);
        }
    }

    spdlog::debug("[{}] Macro devices populated ({} devices, editing={})", get_name(),
                  macros.size(), editing_macro_index_);
}

void LedSettingsOverlay::load_macro_draft(int index) {
    auto& led_ctrl = helix::led::LedController::instance();
    const auto& macros = led_ctrl.configured_macros();
    if (index < 0 || index >= static_cast<int>(macros.size()))
        return;
    const auto& macro = macros[index];
    const auto& discovered = led_ctrl.discovered_macros();

    lv_subject_set_string(&macro_draft_name_, macro.display_name.c_str());
    lv_subject_set_int(&macro_draft_type_, macro_type_row(macro.type));

    auto load_field = [&discovered](const std::string& stored, helix::Subject& sel,
                                    helix::Subject& text) {
        const auto view = helix::led::macro_field_view(stored, discovered);
        lv_subject_set_int(&sel, view.dropdown_index);
        lv_subject_set_string(&text, view.typed.c_str());
    };
    load_field(macro.on_macro, macro_draft_on_sel_, macro_draft_on_text_);
    load_field(macro.off_macro, macro_draft_off_sel_, macro_draft_off_text_);
    load_field(macro.toggle_macro, macro_draft_toggle_sel_, macro_draft_toggle_text_);

    macro_draft_preset_sel_pool_.ensure_size(macro.presets.size());
    macro_draft_preset_text_pool_.ensure_size(macro.presets.size());
    for (size_t p = 0; p < macro.presets.size(); ++p) {
        const auto view = helix::led::macro_field_view(macro.presets[p], discovered);
        macro_draft_preset_sel_pool_.set_int(p, view.dropdown_index);
        macro_draft_preset_text_pool_.set_string(p, view.typed);
    }
    lv_subject_set_int(&macro_draft_preset_count_, static_cast<int>(macro.presets.size()));
}

void LedSettingsOverlay::handle_add_macro_preset() {
    const auto& discovered = helix::led::LedController::instance().discovered_macros();
    const auto n = static_cast<size_t>(lv_subject_get_int(&macro_draft_preset_count_));
    macro_draft_preset_sel_pool_.ensure_size(n + 1);
    macro_draft_preset_text_pool_.ensure_size(n + 1);
    const auto view = helix::led::macro_field_view("", discovered);
    macro_draft_preset_sel_pool_.set_int(n, view.dropdown_index);
    macro_draft_preset_text_pool_.set_string(n, view.typed);
    lv_subject_set_int(&macro_draft_preset_count_, static_cast<int>(n + 1));
}

void LedSettingsOverlay::handle_remove_macro_preset(int preset_index) {
    const int n = lv_subject_get_int(&macro_draft_preset_count_);
    if (preset_index < 0 || preset_index >= n)
        return;
    // Rows below the removed one move up a slot; the repeat then rebuilds one
    // row shorter.
    for (int p = preset_index; p + 1 < n; ++p) {
        macro_draft_preset_sel_pool_.set_int(
            p, lv_subject_get_int(macro_draft_preset_sel_pool_.at(p + 1)));
        macro_draft_preset_text_pool_.set_string(
            p, lv_subject_get_string(macro_draft_preset_text_pool_.at(p + 1)));
    }
    lv_subject_set_int(&macro_draft_preset_count_, n - 1);
}

// ============================================================================
// MACRO DEVICE HANDLERS
// ============================================================================

void LedSettingsOverlay::discard_unsaved_macro_entry() {
    if (editing_macro_index_ < 0)
        return;

    auto& ctrl = helix::led::LedController::instance();
    auto macros = ctrl.configured_macros();

    if (editing_macro_index_ >= static_cast<int>(macros.size()))
        return;

    const auto& entry = macros[editing_macro_index_];
    if (entry.display_name.empty()) {
        spdlog::info("[{}] Discarding unsaved empty macro at index {}", get_name(),
                     editing_macro_index_);
        macros.erase(macros.begin() + editing_macro_index_);
        ctrl.set_configured_macros(macros);
        editing_macro_index_ = -1;
    }
}

void LedSettingsOverlay::handle_add_macro_device() {
    spdlog::info("[{}] Adding new macro device", get_name());

    // Drafts now survive in the controller's list, so a second + press while one
    // is still blank would stack another "(unnamed)" card behind it.
    discard_unsaved_macro_entry();

    auto& ctrl = helix::led::LedController::instance();
    auto updated = ctrl.configured_macros();

    helix::led::LedMacroInfo new_device;
    new_device.display_name = "";
    new_device.type = helix::led::MacroLedType::ON_OFF;
    updated.push_back(new_device);

    // Only update in-memory list — don't persist to disk until user clicks Save.
    // This prevents "ghost" empty entries from surviving app restarts.
    ctrl.set_configured_macros(updated);

    // Open in edit mode
    editing_macro_index_ = static_cast<int>(updated.size()) - 1;
    load_macro_draft(editing_macro_index_);
    populate_macro_devices();
}

void LedSettingsOverlay::handle_edit_macro_device(int index) {
    spdlog::info("[{}] Editing macro device {}", get_name(), index);

    // If we were editing a different entry, discard it if it was never saved (empty name)
    discard_unsaved_macro_entry();

    if (editing_macro_index_ == index) {
        // Toggle off
        editing_macro_index_ = -1;
    } else {
        editing_macro_index_ = index;
        load_macro_draft(index);
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

    std::string deleted_name = updated[index].display_name;
    spdlog::info("[{}] Deleting macro device {}: '{}'", get_name(), index, deleted_name);

    updated.erase(updated.begin() + index);
    ctrl.set_configured_macros(updated);

    // Drop the deleted macro from Automatic LED Control's targets.
    std::string macro_strip_id = "macro:" + deleted_name;
    auto& auto_state = helix::led::LedAutoState::instance();
    auto strips = auto_state.strips();
    auto it = std::find(strips.begin(), strips.end(), macro_strip_id);
    if (it != strips.end()) {
        strips.erase(it);
        auto_state.set_strips(strips);
        auto_state.save_config();
    }

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

    // The draft the edit form is bound to. Typed text wins over the dropdown,
    // so a macro the detector never offered is still storable.
    auto read_field = [&discovered](helix::Subject& text, helix::Subject& sel) -> std::string {
        return helix::led::resolve_macro_field(lv_subject_get_string(&text),
                                               lv_subject_get_int(&sel), discovered);
    };
    updated[index].display_name = lv_subject_get_string(&macro_draft_name_);
    const int type_row = lv_subject_get_int(&macro_draft_type_);
    updated[index].type = kMacroTypeRows[type_row >= 0 && type_row < 3 ? type_row : 0];
    updated[index].on_macro.clear();
    updated[index].off_macro.clear();
    updated[index].toggle_macro.clear();
    updated[index].presets.clear();
    switch (updated[index].type) {
    case helix::led::MacroLedType::ON_OFF:
        updated[index].on_macro = read_field(macro_draft_on_text_, macro_draft_on_sel_);
        updated[index].off_macro = read_field(macro_draft_off_text_, macro_draft_off_sel_);
        break;
    case helix::led::MacroLedType::TOGGLE:
        updated[index].toggle_macro = read_field(macro_draft_toggle_text_, macro_draft_toggle_sel_);
        break;
    case helix::led::MacroLedType::PRESET: {
        const int n = lv_subject_get_int(&macro_draft_preset_count_);
        for (int p = 0; p < n; ++p) {
            updated[index].presets.emplace_back(helix::led::resolve_macro_field(
                lv_subject_get_string(macro_draft_preset_text_pool_.at(p)),
                lv_subject_get_int(macro_draft_preset_sel_pool_.at(p)), discovered));
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
    lifetime_.defer("LedSettingsOverlay::populate_led_chips", [this]() {
        if (cleanup_called())
            return;
        populate_led_chips_impl();
    });
}

void LedSettingsOverlay::populate_led_chips_impl() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = find_required(overlay_root_, "row_auto_state_strips", get_name());
    if (!row)
        return;

    lv_obj_t* chip_container = helix::ui::find_required(row, "chip_container", get_name());
    if (!chip_container) {
        spdlog::warn("[{}] Applies-to chip row found but chip_container missing", get_name());
        return;
    }

    helix::ui::safe_clean_children(chip_container);

    // Chip list spans all backends (native + WLED + macros); selection is
    // Automatic LED Control's current targets.
    auto& led_ctrl = helix::led::LedController::instance();
    const auto targets = helix::led::LedAutoState::instance().targets();

    size_t selected_count = 0;
    for (const auto& strip : led_ctrl.all_selectable_strips()) {
        bool selected = std::find(targets.begin(), targets.end(), strip.id) != targets.end();
        selected_count += selected ? 1 : 0;
        std::string display_name = helix::led::device_display_name(strip);

        helix::ui::create_led_chip(
            chip_container, strip.id, display_name, selected,
            [this](const std::string& led_name) { handle_led_chip_clicked(led_name); });
    }

    spdlog::debug("[{}] Applies-to chips populated ({} selected)", get_name(), selected_count);
}

void LedSettingsOverlay::handle_led_chip_clicked(const std::string& led_name) {
    auto& as = helix::led::LedAutoState::instance();
    // The saved list, not targets(): a strip not discovered yet stays selected.
    as.set_strips(
        helix::led::toggle_target(as.strips().empty() ? as.targets() : as.strips(), led_name));
    as.save_config();
    as.evaluate();

    populate_led_chips();
    populate_auto_state_rows();
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void LedSettingsOverlay::handle_led_on_at_start_changed(bool enabled) {
    spdlog::info("[{}] LED on at start toggled: {}", get_name(), enabled ? "ON" : "OFF");
    helix::led::LedController::instance().set_led_on_at_start(enabled);
    helix::led::LedController::instance().save_config();

    // Drive visibility of the startup brightness slider
    lv_subject_set_int(&led_on_at_start_subject_, enabled ? 1 : 0);
}

void LedSettingsOverlay::handle_startup_brightness_commit(int value) {
    spdlog::info("[{}] Startup brightness committed: {}%", get_name(), value);
    helix::led::LedController::instance().set_startup_brightness(value);
    helix::led::LedController::instance().save_config();
}

void LedSettingsOverlay::handle_startup_brightness_changed(int value) {
    // Per drag tick: in-memory value + readout only. save_config() writes
    // settings.json (double fsync + rolling backup) and belongs on release,
    // which handle_startup_brightness_commit() handles.
    spdlog::debug("[{}] Startup brightness preview: {}%", get_name(), value);
    helix::led::LedController::instance().set_startup_brightness(value);
    lv_subject_set_int(&startup_brightness_subject_, value);
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

static std::optional<size_t> state_row_index(const std::string& key) {
    for (size_t i = 0; i < kStateRowCount; ++i) {
        if (key == kStateRows[i].key) {
            return i;
        }
    }
    return std::nullopt;
}

/// The index of @p color among the LED colour presets, or -1.
static int preset_index_of(uint32_t color) {
    const auto& presets = helix::led::LedController::instance().color_presets();
    const auto it = std::find(presets.begin(), presets.end(), color);
    return it == presets.end() ? -1 : static_cast<int>(it - presets.begin());
}

void LedSettingsOverlay::publish_auto_color_presets() {
    const auto& presets = helix::led::LedController::instance().color_presets();
    auto_preset_color_pool_.ensure_size(presets.size());
    for (size_t i = 0; i < presets.size(); ++i) {
        auto_preset_color_pool_.set_color(i, presets[i]);
    }
    lv_subject_set_int(&auto_preset_count_, static_cast<int>(presets.size()));
}

void LedSettingsOverlay::handle_color_swatch_clicked(lv_obj_t* swatch, int preset_index) {
    const auto& presets = helix::led::LedController::instance().color_presets();
    if (preset_index < 0 || preset_index >= static_cast<int>(presets.size())) {
        return;
    }
    // The swatches sit inside their row's ctx_<state> container.
    for (lv_obj_t* obj = swatch; obj != nullptr; obj = lv_obj_get_parent(obj)) {
        const char* name = lv_obj_get_name(obj);
        if (name != nullptr && std::strncmp(name, "ctx_", 4) == 0) {
            handle_color_selected(name + 4, presets[static_cast<size_t>(preset_index)]);
            return;
        }
    }
    spdlog::warn("[{}] Color swatch {} is in no auto-state row", get_name(), preset_index);
}

namespace {

/// led_auto_kind_<row>'s value for an action type, as led_auto_state_row.xml reads it.
int action_kind(const std::string& type) {
    static constexpr const char* kKinds[] = {"off",    "brightness",  "color",
                                             "effect", "wled_preset", "macro"};
    for (int i = 0; i < 6; ++i) {
        if (type == kKinds[i])
            return i;
    }
    return 0;
}

} // namespace

void LedSettingsOverlay::populate_auto_state_rows() {
    if (!overlay_root_)
        return;

    auto& auto_state = helix::led::LedAutoState::instance();
    publish_auto_color_presets();

    // Capability-filtered action types, shared by every row's dropdown
    action_type_options_.clear();
    action_type_options_.push_back("off");
    action_type_options_.push_back("brightness");

    auto& ctrl = helix::led::LedController::instance();
    const auto targets = auto_state.targets();
    bool has_color = false;
    bool has_wled = false;
    for (const auto& strip_id : targets) {
        const auto* s = helix::led::find_strip(ctrl.native().strips(), strip_id);
        if (s != nullptr && s->supports_color) {
            has_color = true;
        }
        if (ctrl.backend_for_strip(strip_id) == helix::led::LedBackendType::WLED) {
            has_wled = true;
        }
    }
    if (has_color)
        action_type_options_.push_back("color");
    if (ctrl.effects().is_available())
        action_type_options_.push_back("effect");
    if (has_wled)
        action_type_options_.push_back("wled_preset");
    if (ctrl.macro().is_available())
        action_type_options_.push_back("macro");

    std::string options_str;
    for (size_t i = 0; i < action_type_options_.size(); i++) {
        if (i > 0)
            options_str += "\n";
        const auto& opt = action_type_options_[i];
        if (opt == "off")
            options_str += lv_tr("Off");
        else if (opt == "brightness")
            options_str += lv_tr("Brightness");
        else if (opt == "color")
            options_str += lv_tr("Color");
        else if (opt == "effect")
            options_str += lv_tr("Effect");
        else if (opt == "wled_preset")
            options_str += lv_tr("WLED Preset");
        else if (opt == "macro")
            options_str += lv_tr("Macro");
    }
    lv_subject_set_string(&auto_action_options_, options_str.c_str());

    std::string effect_opts;
    const auto& effects = ctrl.effects().effects();
    for (size_t i = 0; i < effects.size(); i++) {
        if (i > 0)
            effect_opts += "\n";
        effect_opts += effects[i].display_name;
    }
    lv_subject_set_string(&auto_effect_options_, effect_opts.c_str());
    lv_subject_set_int(&auto_effect_count_, static_cast<int>(effects.size()));

    std::string macro_opts;
    const auto& macros = ctrl.macro().macros();
    for (size_t i = 0; i < macros.size(); i++) {
        if (i > 0)
            macro_opts += "\n";
        macro_opts += macros[i].display_name;
    }
    lv_subject_set_string(&auto_macro_options_, macro_opts.c_str());
    lv_subject_set_int(&auto_macro_count_, static_cast<int>(macros.size()));

    for (size_t row = 0; row < kStateRowCount; ++row) {
        const auto* mapping = auto_state.get_mapping(kStateRows[row].key);
        publish_auto_state_row(row, mapping ? *mapping : helix::led::LedStateAction{});
    }

    spdlog::debug("[{}] Auto-state rows populated ({} states)", get_name(), kStateRowCount);
}

void LedSettingsOverlay::publish_auto_state_row(size_t row,
                                                const helix::led::LedStateAction& action) {
    int action_row = 0;
    for (size_t i = 0; i < action_type_options_.size(); i++) {
        if (action_type_options_[i] == action.action_type) {
            action_row = static_cast<int>(i);
            break;
        }
    }
    auto_action_pool_.set_int(row, action_row);
    const int kind = action_kind(action.action_type);
    auto_kind_pool_.set_int(row, kind);
    auto_detail_shown_pool_.set_int(row, kind != 0 ? 1 : 0);
    auto_brightness_pool_.set_int(row, action.brightness);
    auto_color_selected_pool_.set_int(row, preset_index_of(action.color));

    auto& ctrl = helix::led::LedController::instance();
    int effect_row = 0;
    const auto& effects = ctrl.effects().effects();
    for (size_t i = 0; i < effects.size(); i++) {
        if (effects[i].name == action.effect_name)
            effect_row = static_cast<int>(i);
    }
    auto_effect_sel_pool_.set_int(row, effect_row);

    const int wled = action.wled_preset > 0 ? action.wled_preset : 1;
    auto_wled_pool_.set_int(row, wled);
    auto_wled_label_pool_.set_string(row, fmt::format(lv_tr("Preset ID: {}"), wled));

    int macro_row = 0;
    const auto& macros = ctrl.macro().macros();
    for (size_t i = 0; i < macros.size(); i++) {
        if (macros[i].on_macro == action.macro_gcode ||
            macros[i].toggle_macro == action.macro_gcode)
            macro_row = static_cast<int>(i);
    }
    auto_macro_sel_pool_.set_int(row, macro_row);
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

    if (const auto row = state_row_index(state_key)) {
        publish_auto_state_row(*row, action);
    }
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

    const auto row = state_row_index(state_key);
    if (row && *row < auto_color_selected_pool_.size()) {
        auto_color_selected_pool_.set_int(*row, preset_index_of(color));
    }
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
    if (const auto row = state_row_index(state_key)) {
        auto_wled_label_pool_.set_string(*row, fmt::format(lv_tr("Preset ID: {}"), preset_id));
    }
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

} // namespace helix::settings
