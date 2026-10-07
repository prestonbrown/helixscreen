// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_fans.cpp
 * @brief Implementation of FanSettingsOverlay
 */

#include "ui_settings_fans.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_modal.h"
#include "ui_status_pill.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::settings {

using helix::ui::find_required;

void FanSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_fan_rename_confirm", [](lv_event_t*) { get_fan_settings_overlay().confirm_rename(); }},
        {"on_fan_rename_cancel", [](lv_event_t*) { get_fan_settings_overlay().cancel_rename(); }},
    });
}

lv_obj_t* FanSettingsOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    // Containers for dynamic row population
    controllable_list_ = find_required(overlay_root_, "controllable_fans_list", get_name());
    auto_list_ = find_required(overlay_root_, "auto_fans_list", get_name());
    no_fans_placeholder_ = find_required(overlay_root_, "no_fans_placeholder", get_name());
    return overlay_root_;
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void FanSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_fans();
}

void FanSettingsOverlay::on_deactivating(DeactivateReason) {
    cancel_rename(); // Dismiss rename modal if open
}

// ============================================================================
// FAN TYPE HELPERS
// ============================================================================

namespace {

/// Convert FanType to a short display string for the type pill
const char* fan_type_label(helix::FanType type) {
    switch (type) {
    case helix::FanType::PART_COOLING:
        return lv_tr("Part");
    case helix::FanType::HEATER_FAN:
        return lv_tr("Heater");
    case helix::FanType::CONTROLLER_FAN:
        return lv_tr("Controller");
    case helix::FanType::TEMPERATURE_FAN:
        return lv_tr("Temp");
    case helix::FanType::GENERIC_FAN:
        return lv_tr("Generic");
    case helix::FanType::OUTPUT_PIN_FAN:
        return lv_tr("Output Pin");
    }
    return lv_tr("Fan");
}

} // namespace

// ============================================================================
// FAN LIST POPULATION
// ============================================================================

void FanSettingsOverlay::update_section_count(const char* badge_name, size_t count) {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, badge_name);
    if (badge) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", count);
        ui_status_pill_set_text(badge, buf);
    }
}

void FanSettingsOverlay::populate_fan_list(lv_obj_t* list, bool controllable) {
    if (!list)
        return;

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(list, i);
        helix::ui::safe_delete(child);
    }

    auto& fans = get_printer_state().fan_state().get_fans();
    size_t count = 0;

    for (const auto& fan : fans) {
        if (fan.is_controllable != controllable) {
            continue;
        }

        // Build speed string
        char speed_buf[16];
        snprintf(speed_buf, sizeof(speed_buf), "%d%%", fan.speed_percent);

        // Create row from XML component
        const char* type_label = fan_type_label(fan.type);
        const char* attrs[] = {
            "fan_name",   fan.display_name.c_str(), "fan_type", type_label,
            "fan_object", fan.object_name.c_str(),  nullptr,
        };
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(list, "fan_settings_row", attrs));
        if (!row) {
            spdlog::warn("[{}] Failed to create row for fan: {}", get_name(), fan.object_name);
            continue;
        }

        // Update the speed label (not bound to a subject here — refreshed on populate)
        lv_obj_t* speed_label = helix::ui::find_required(row, "speed_label", get_name());
        if (speed_label) {
            lv_label_set_text(speed_label, speed_buf);
        }

        // Wire click callback on the entire row for rename
        {
            auto* obj_name = new std::string(fan.object_name);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            // Touch feedback: dim on press
            lv_obj_set_style_opa(row, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_add_event_cb(
                row,
                [](lv_event_t* e) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[FanSettingsOverlay] row clicked");
                    auto* name = static_cast<std::string*>(lv_event_get_user_data(e));
                    if (name) {
                        lv_obj_t* r = lv_event_get_current_target_obj(e);
                        lv_obj_t* label = helix::ui::find_required(r, "name_label", "Fans");
                        const char* current = label ? lv_label_get_text(label) : "";
                        get_fan_settings_overlay().handle_fan_rename(*name, current ? current : "");
                    }
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_CLICKED, obj_name);

            lv_obj_add_event_cb(
                row,
                [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
                LV_EVENT_DELETE, obj_name);
        }

        ++count;
    }

    spdlog::debug("[{}] Populated {} {} fans", get_name(), count,
                  controllable ? "controllable" : "auto");
}

void FanSettingsOverlay::populate_fans() {
    if (!overlay_root_)
        return;

    auto& fans = get_printer_state().fan_state().get_fans();

    // Count fans by category
    size_t controllable_count = 0;
    size_t auto_count = 0;
    for (const auto& fan : fans) {
        if (fan.is_controllable) {
            ++controllable_count;
        } else {
            ++auto_count;
        }
    }

    // Update section badges
    update_section_count("controllable_fan_count", controllable_count);
    update_section_count("auto_fan_count", auto_count);

    // Show/hide sections based on fan counts
    lv_obj_t* controllable_section =
        find_required(overlay_root_, "controllable_section", get_name());
    lv_obj_t* auto_section = find_required(overlay_root_, "auto_section", get_name());

    if (controllable_section) {
        lv_obj_set_flag(controllable_section, LV_OBJ_FLAG_HIDDEN, controllable_count == 0);
    }
    if (auto_section) {
        lv_obj_set_flag(auto_section, LV_OBJ_FLAG_HIDDEN, auto_count == 0);
    }

    // Show empty state if no fans at all
    if (no_fans_placeholder_) {
        lv_obj_set_flag(no_fans_placeholder_, LV_OBJ_FLAG_HIDDEN, !fans.empty());
    }

    // Populate row lists
    populate_fan_list(controllable_list_, true);
    populate_fan_list(auto_list_, false);
}

// ============================================================================
// FAN RENAME
// ============================================================================

void FanSettingsOverlay::handle_fan_rename(const std::string& object_name,
                                           const std::string& current_name) {
    spdlog::info("[{}] Rename requested: '{}' (current: '{}')", get_name(), object_name,
                 current_name);

    // Lazy-init the subject on first use (avoids startup init order issues)
    if (!rename_subject_initialized_) {
        UI_MANAGED_SUBJECT_STRING(fan_rename_old_name_, rename_old_name_buf_, "",
                                  "fan_rename_old_name", subjects_);
        rename_subject_initialized_ = true;
    }

    pending_rename_object_ = object_name;
    lv_subject_copy_string(&fan_rename_old_name_, current_name.c_str());

    rename_modal_ = helix::ui::modal_show("fan_rename_modal");
    if (!rename_modal_) {
        spdlog::error("[{}] Failed to show fan_rename_modal", get_name());
        return;
    }

    // Pre-fill input with current name
    lv_obj_t* input = lv_obj_find_by_name(rename_modal_, "fan_rename_new_name_input");
    if (input) {
        lv_textarea_set_text(input, current_name.c_str());
    }
}

void FanSettingsOverlay::confirm_rename() {
    if (pending_rename_object_.empty()) {
        spdlog::warn("[{}] Rename confirmed with no fan pending", get_name());
        cancel_rename();
        return;
    }

    // Search the modal this overlay opened before falling back to a shared root.
    // Modals are parented to the active screen and moved to the foreground, so a modal
    // still finishing its exit sits earlier in child order than the live one, and
    // lv_obj_find_by_name is a depth-first first-match walk that would return its input.
    lv_obj_t* input = nullptr;
    if (rename_modal_) {
        input = lv_obj_find_by_name(rename_modal_, "fan_rename_new_name_input");
    }
    if (!input) {
        input = lv_obj_find_by_name(lv_layer_top(), "fan_rename_new_name_input");
    }
    if (!input) {
        input = lv_obj_find_by_name(lv_screen_active(), "fan_rename_new_name_input");
    }

    if (!input) {
        spdlog::error("[{}] Rename input not found, leaving '{}' unchanged", get_name(),
                      pending_rename_object_);
        cancel_rename();
        return;
    }

    // An empty name is a request to revert: PrinterFanState::rename_fan restores the
    // role or auto-generated name for the object.
    const char* text = lv_textarea_get_text(input);
    std::string new_name = text ? text : "";

    get_printer_state().fan_state().rename_fan(pending_rename_object_, new_name);
    spdlog::info("[{}] Renamed '{}' -> '{}'", get_name(), pending_rename_object_, new_name);

    pending_rename_object_.clear();

    if (rename_modal_) {
        helix::ui::modal_hide(rename_modal_);
        rename_modal_ = nullptr;
    }

    // Refresh settings list to show new name
    if (overlay_root_) {
        populate_fans();
    }
}

void FanSettingsOverlay::cancel_rename() {
    spdlog::debug("[{}] Rename modal dismissed (pending: '{}')", get_name(),
                  pending_rename_object_);
    pending_rename_object_.clear();

    if (rename_modal_) {
        helix::ui::modal_hide(rename_modal_);
        rename_modal_ = nullptr;
    }
}

} // namespace helix::settings
