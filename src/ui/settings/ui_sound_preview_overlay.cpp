// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_sound_preview_overlay.h"

#include "ui_event_safety.h"
#include "ui_utils.h"

#include "sound_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unordered_map>

namespace helix::settings {

void SoundPreviewOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_buttons();
}

void SoundPreviewOverlay::on_deactivating(DeactivateReason) {
    clear_buttons();
}

// ==========================================================================
// Button grid
// ==========================================================================

std::string SoundPreviewOverlay::display_name(const std::string& sound_name) {
    static const std::unordered_map<std::string, std::string> names = {
        {"button_tap", "Button Tap"},
        {"toggle_on", "Toggle On"},
        {"toggle_off", "Toggle Off"},
        {"nav_forward", "Nav Forward"},
        {"nav_back", "Nav Back"},
        {"dropdown_open", "Dropdown"},
        {"print_complete", "Print Complete"},
        {"print_cancelled", "Print Canceled"},
        {"error_alert", "Error Alert"},
        {"error_tone", "Error Tone"},
        {"alarm_urgent", "Alarm Urgent"},
        {"test_beep", "Test Beep"},
        {"startup", "Startup"},
    };

    auto it = names.find(sound_name);
    if (it != names.end())
        return it->second;

    // Auto-title-case unknown names from custom themes
    std::string result;
    bool capitalize = true;
    for (char c : sound_name) {
        if (c == '_') {
            result += ' ';
            capitalize = true;
        } else if (capitalize) {
            result += static_cast<char>(toupper(static_cast<unsigned char>(c)));
            capitalize = false;
        } else {
            result += c;
        }
    }
    return result;
}

void SoundPreviewOverlay::populate_buttons() {
    lv_obj_t* grid = helix::ui::find_required(overlay_root_, "sound_button_grid", get_name());
    if (!grid) {
        return;
    }

    auto sound_names = helix::SoundManager::instance().get_sound_names();
    spdlog::debug("[{}] Populating {} sound buttons", get_name(), sound_names.size());

    for (const auto& name : sound_names) {
        std::string label = display_name(name);
        const char* attrs[] = {"variant", "outline", "text", label.c_str(), nullptr};
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(grid, "ui_button", attrs));
        if (!btn) {
            spdlog::warn("[{}] Failed to create button for '{}'", get_name(), name);
            continue;
        }

        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        // Suppress default button_tap sound — preview plays its own sound
        lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_4);

        // Store sound name in a heap-allocated string, freed when button is deleted
        auto* sound_name_ptr = new std::string(name);
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[SoundPreviewOverlay] button_click");
                auto* sname = static_cast<std::string*>(lv_event_get_user_data(e));
                if (sname) {
                    spdlog::debug("[SoundPreviewOverlay] Playing '{}'", *sname);
                    helix::SoundManager::instance().play(*sname);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, sound_name_ptr);

        // Clean up the heap string when the button is deleted
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                auto* sname = static_cast<std::string*>(lv_event_get_user_data(e));
                delete sname;
            },
            LV_EVENT_DELETE, sound_name_ptr);
    }
}

void SoundPreviewOverlay::clear_buttons() {
    lv_obj_t* grid = helix::ui::find_required(overlay_root_, "sound_button_grid", get_name());
    if (grid) {
        helix::ui::safe_clean_children(grid);
    }
}

} // namespace helix::settings
