// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_helpers.h"

#include "config.h"
#include "device_display_name.h"
#include "printer_hardware.h"

#include <spdlog/spdlog.h>

namespace helix {
namespace ui {
namespace wizard {

std::string build_dropdown_options(const std::vector<std::string>& items,
                                   std::function<bool(const std::string&)> filter,
                                   bool include_none,
                                   std::optional<helix::DeviceType> device_type) {
    std::string options_str;

    // "None" goes FIRST for optional hardware (makes index 0 = safe default)
    if (include_none) {
        options_str = "None";
    }

    // Add filtered items
    for (const auto& item : items) {
        // Apply filter if provided
        if (filter && !filter(item)) {
            continue;
        }

        if (!options_str.empty()) {
            options_str += "\n";
        }

        // Transform to display name if device type is provided
        if (device_type.has_value()) {
            options_str += helix::get_display_name(item, device_type.value());
        } else {
            options_str += item;
        }
    }

    return options_str;
}

int find_item_index(const std::vector<std::string>& items, const std::string& name,
                    int default_index) {
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i] == name) {
            return static_cast<int>(i);
        }
    }
    return default_index;
}

int restore_dropdown_selection(lv_obj_t* dropdown, lv_subject_t* subject,
                               const std::vector<std::string>& items, const char* config_path,
                               const PrinterHardware* hw,
                               std::function<std::string(const PrinterHardware&)> guess_method_fn,
                               const char* log_prefix) {
    int selected_index = 0;

    // Count real items (excluding "None")
    size_t real_item_count =
        std::count_if(items.begin(), items.end(), [](const auto& s) { return s != "None"; });

    // Helper to try finding an item and log the result
    auto try_select = [&](const std::string& name, const char* reason) -> bool {
        if (name.empty())
            return false;
        int idx = find_item_index(items, name, -1);
        if (idx >= 0) {
            selected_index = idx;
            spdlog::debug("{} {}: {}", log_prefix, reason, name);
            return true;
        }
        return false;
    };

    // Find "None" index for optional hardware fallback
    int none_index = find_item_index(items, "None", -1);

    // Priority 1: If only ONE real hardware option, auto-select it
    // (handles non-standard names like "bed_heater" instead of "heater_bed")
    if (real_item_count == 1) {
        // Find the single non-None item
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i] != "None") {
                selected_index = static_cast<int>(i);
                spdlog::debug("{} Single option available, auto-selecting: {}", log_prefix,
                              items[i]);
                break;
            }
        }
    }
    // Priority 2: Try to restore from saved config
    else if (Config* config = Config::get_instance()) {
        std::string saved = config->get<std::string>(config->df() + config_path, "");
        if (!saved.empty() && try_select(saved, "Restored selection")) {
            // Found saved item
        } else {
            // Priority 3: Saved not found or empty - try guessing
            if (!saved.empty()) {
                spdlog::debug("{} Saved '{}' not in available hardware, trying auto-detect",
                              log_prefix, saved);
            }
            if (hw && guess_method_fn) {
                std::string guessed = guess_method_fn(*hw);
                if (!try_select(guessed, "Auto-selected") && none_index >= 0) {
                    // Guess returned empty or not found - select "None" for optional hardware
                    selected_index = none_index;
                    spdlog::debug("{} No match found, defaulting to None", log_prefix);
                }
            }
        }
    }

    // Update dropdown and subject
    if (dropdown) {
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(selected_index));
    }
    if (subject) {
        lv_subject_set_int(subject, selected_index);
    }

    spdlog::debug("{} Configured dropdown: {} options, selected index {}", log_prefix, items.size(),
                  selected_index);

    return selected_index;
}

bool save_dropdown_selection(lv_subject_t* subject, const std::vector<std::string>& items,
                             const char* config_path, const char* log_prefix) {
    if (!subject) {
        spdlog::warn("{} Cannot save selection: null subject", log_prefix);
        return false;
    }

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("{} Cannot save selection: config not available", log_prefix);
        return false;
    }

    // Get selection index from subject
    int index = lv_subject_get_int(subject);
    if (index < 0 || static_cast<size_t>(index) >= items.size()) {
        spdlog::warn("{} Cannot save selection: index {} out of range (0-{})", log_prefix, index,
                     items.size() - 1);
        return false;
    }

    // Save item name (not index) to config
    // "None" is a UI placeholder meaning "no selection" — save as empty string
    const std::string& item_name = items[static_cast<size_t>(index)];
    const std::string save_value = (item_name == "None") ? "" : item_name;
    config->set(config->df() + config_path, save_value);
    spdlog::debug("{} Saved selection: '{}'", log_prefix, save_value);

    return true;
}

void init_int_subject(lv_subject_t* subject, int32_t initial_value, const char* subject_name) {
    lv_subject_init_int(subject, initial_value);
    lv_xml_register_subject(nullptr, subject_name, subject);
}

} // namespace wizard
} // namespace ui
} // namespace helix
