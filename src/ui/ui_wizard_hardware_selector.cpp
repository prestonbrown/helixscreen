// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_hardware_selector.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_wizard_helpers.h"

#include "app_globals.h"
#include "config.h"
#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "printer_hardware.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <optional>

void wizard_hardware_dropdown_changed_cb(lv_event_t* e) {
    lv_obj_t* dropdown = (lv_obj_t*)lv_event_get_target(e);
    lv_subject_t* subject = (lv_subject_t*)lv_event_get_user_data(e);

    if (!subject) {
        spdlog::error("[Wizard Hardware] Dropdown callback missing subject user_data");
        return;
    }

    uint16_t selected_index = static_cast<uint16_t>(lv_dropdown_get_selected(dropdown));
    lv_subject_set_int(subject, selected_index);
}

bool wizard_populate_hardware_dropdown(
    lv_obj_t* root, const char* dropdown_name, lv_subject_t* subject,
    std::vector<std::string>& items_out,
    std::function<const std::vector<std::string>&(IMoonrakerAPI*)> moonraker_getter,
    const char* prefix_filter, bool allow_none, const char* config_key,
    std::function<std::string(const PrinterHardware&)> guess_fallback, const char* log_prefix,
    std::optional<helix::DeviceType> device_type) {
    if (!root || !dropdown_name || !subject) {
        spdlog::error("{} Invalid parameters for dropdown population", log_prefix);
        return false;
    }

    // Get Moonraker API for hardware discovery
    IMoonrakerAPI* api = get_moonraker_api();

    // Clear and build items list
    items_out.clear();
    if (api) {
        const auto& hardware_list = moonraker_getter(api);
        for (const auto& item : hardware_list) {
            // Apply prefix filter if specified
            if (prefix_filter && item.find(prefix_filter) == std::string::npos) {
                continue;
            }
            items_out.push_back(item);
        }
    }

    // Build dropdown options string with friendly display names if device type is specified
    std::string options_str = helix::ui::wizard::build_dropdown_options(
        items_out,
        nullptr, // No additional filter (already filtered above)
        allow_none, device_type);

    // Add "None" to items vector FIRST if needed (to match dropdown order)
    if (allow_none) {
        items_out.insert(items_out.begin(), "None");
    }

    // Find and configure dropdown
    lv_obj_t* dropdown = lv_obj_find_by_name(root, dropdown_name);
    if (!dropdown) {
        spdlog::warn("{} Dropdown '{}' not found in screen", log_prefix, dropdown_name);
        return false;
    }

    lv_dropdown_set_options(dropdown, options_str.c_str());

    // Theme handles dropdown chevron symbol and MDI font automatically
    // via LV_SYMBOL_DOWN override in lv_conf.h and helix_theme.c

    // Create PrinterHardware for guessing fallback
    const PrinterHardware* hw = nullptr;
    std::unique_ptr<PrinterHardware> hw_instance;
    if (api && guess_fallback) {
        hw_instance =
            std::make_unique<PrinterHardware>(api->hardware().heaters(), api->hardware().sensors(),
                                              api->hardware().fans(), api->hardware().leds());
        hw = hw_instance.get();
    }

    // Restore saved selection with guessing fallback
    helix::ui::wizard::restore_dropdown_selection(dropdown, subject, items_out, config_key, hw,
                                                  guess_fallback, log_prefix);

    spdlog::debug("{} Populated dropdown '{}' with {} items", log_prefix, dropdown_name,
                  items_out.size());
    return true;
}
