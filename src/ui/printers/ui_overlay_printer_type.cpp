// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_overlay_printer_type.cpp
 * @brief Implementation of PrinterTypeOverlay
 *
 * A scrollable list of the printer database's models, filtered by the detected
 * kinematics exactly as the setup wizard filters it. Rows are created from the
 * printer_image_list_item XML component - it is a generic label + callback row
 * despite the name, and its checked-state styling is already what a selection
 * list wants, so this reuses it rather than shipping an identical twin.
 */

#include "ui_overlay_printer_type.h"

#include "ui_callback_helpers.h"
#include "ui_nav.h"

#include "app_globals.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "ui/ui_widget_helpers.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>

namespace helix::settings {

using helix::ui::find_required;

// ============================================================================
// OVERLAY BASE INTERFACE
// ============================================================================

void PrinterTypeOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_printer_type_row_clicked",
         [](lv_event_t* e) {
             auto* row = lv_event_get_current_target_obj(e);
             auto* name = row ? static_cast<const char*>(lv_obj_get_user_data(row)) : nullptr;
             if (name) {
                 get_printer_type_overlay().handle_type_selected(std::string(name));
             }
         }},
    });
}

void PrinterTypeOverlay::before_show() {
    // Match the wizard's filter so both surfaces offer the same candidates.
    // An empty filter means "unfiltered", which is the right fallback when
    // Moonraker has not reported kinematics.
    kinematics_filter_.clear();
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        kinematics_filter_ = api->hardware().kinematics();
    }
}

void PrinterTypeOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_type_list();
}

// ============================================================================
// LIST
// ============================================================================

void PrinterTypeOverlay::populate_type_list() {
    if (!overlay_root_) {
        return;
    }

    lv_obj_t* list = find_required(overlay_root_, "printer_type_list", get_name());
    if (!list) {
        return;
    }

    lv_obj_clean(list);

    const auto& names = PrinterDetector::get_list_names(kinematics_filter_);
    for (const auto& name : names) {
        const char* attrs[] = {"label_text", name.c_str(), "callback",
                               "on_printer_type_row_clicked", nullptr};
        lv_obj_t* row =
            static_cast<lv_obj_t*>(lv_xml_create(list, "printer_image_list_item", attrs));
        if (!row) {
            spdlog::warn("[{}] Failed to create row for '{}'", get_name(), name);
            continue;
        }

        // Carry the model name on the row rather than reading it back off the
        // label: a label in dots long_mode returns its ELLIPSIZED text, which
        // would silently mis-key the longer model names. Safe on user_data
        // because printer_image_list_item extends lv_button, which does not
        // claim it - same reasoning and same caveat as PrinterImageOverlay.
        char* name_copy = strdup(name.c_str());
        if (!name_copy) {
            spdlog::error("[{}] Failed to allocate row name", get_name());
            continue;
        }
        lv_obj_set_user_data(row, name_copy);

        // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative form.
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                auto* obj = lv_event_get_current_target_obj(ev);
                if (void* data = lv_obj_get_user_data(obj)) {
                    free(data);
                }
            },
            LV_EVENT_DELETE, nullptr);
    }

    std::string current;
    Config* config = Config::get_instance();
    current = config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");

    update_selection_indicator(current);

    spdlog::debug("[{}] Populated {} models (kinematics filter '{}'), current '{}'", get_name(),
                  names.size(), kinematics_filter_, current);
}

void PrinterTypeOverlay::update_selection_indicator(const std::string& active_type) {
    lv_obj_t* list = find_required(overlay_root_, "printer_type_list", get_name());
    if (!list) {
        return;
    }

    uint32_t count = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* row = lv_obj_get_child(list, static_cast<int32_t>(i));
        if (!row) {
            continue;
        }
        auto* name = static_cast<const char*>(lv_obj_get_user_data(row));
        if (name && active_type == name) {
            lv_obj_add_state(row, LV_STATE_CHECKED);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        } else {
            lv_obj_remove_state(row, LV_STATE_CHECKED);
        }
    }
}

// ============================================================================
// SELECTION
// ============================================================================

void PrinterTypeOverlay::handle_type_selected(const std::string& type_name) {
    if (type_name.empty()) {
        return;
    }

    Config* config = Config::get_instance();
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] Cannot apply '{}' - config or Moonraker unavailable", get_name(),
                     type_name);
        return;
    }

    PrinterDetector::apply_type_choice(config, type_name, api->hardware());

    update_selection_indicator(type_name);
    helix::nav::go_back();
}

} // namespace helix::settings
