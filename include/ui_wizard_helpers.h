// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "device_display_name.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class PrinterHardware;

/**
 * @file ui_wizard_helpers.h
 * @brief Common helper functions for wizard screen implementations
 *
 * Provides reusable utilities for dropdown population, config management,
 * and subject initialization to reduce code duplication across wizard screens.
 */

namespace helix {
namespace ui {
namespace wizard {
/// Maps a Klipper object name to the label its dropdown row shows.
using ItemNamer = std::function<std::string(const std::string&)>;

/// The ItemNamer that applies get_display_name() for @p type.
ItemNamer display_name_for(helix::DeviceType type);

/**
 * @brief Build dropdown options string from items vector
 *
 * Creates a newline-delimited string suitable for lv_dropdown_set_options(),
 * optionally filtering items and appending "None" option.
 *
 * @param items Vector of item names to include
 * @param filter Optional filter function (return true to include item)
 * @param include_none Whether to append "None" option at end
 * @param display_name Optional label for each item; items show as-is without one
 *                     (e.g., "heater_fan hotend_fan" -> "Hotend Fan")
 * @return Newline-delimited dropdown options string
 */
std::string build_dropdown_options(const std::vector<std::string>& items,
                                   std::function<bool(const std::string&)> filter = nullptr,
                                   bool include_none = true, ItemNamer display_name = nullptr);

/**
 * @brief Find item index in vector by name
 *
 * Searches for exact match of item name in vector.
 *
 * @param items Vector to search
 * @param name Item name to find
 * @param default_index Fallback index if not found
 * @return Index of item, or default_index if not found
 */
int find_item_index(const std::vector<std::string>& items, const std::string& name,
                    int default_index = 0);

/**
 * @brief Restore dropdown selection from config with guessing fallback
 *
 * Attempts to restore saved selection from config. If no saved value exists,
 * calls the specified guessing method on PrinterHardware. Updates both the
 * dropdown widget and subject with selected index.
 *
 * @param dropdown LVGL dropdown widget
 * @param subject Subject to update with selected index
 * @param items Item names vector (must match dropdown options order)
 * @param config_path JSON path to saved value (e.g., "/printer/bed_heater")
 * @param hw PrinterHardware instance for hardware guessing (can be nullptr)
 * @param guess_method_fn Function to call on PrinterHardware for guess (can be nullptr)
 * @param log_prefix Logging prefix (e.g., "[Wizard Bed]")
 * @return Selected index
 */
int restore_dropdown_selection(lv_obj_t* dropdown, lv_subject_t* subject,
                               const std::vector<std::string>& items, const char* config_path,
                               const PrinterHardware* hw,
                               std::function<std::string(const PrinterHardware&)> guess_method_fn,
                               const char* log_prefix);

/**
 * @brief Save dropdown selection to config by index
 *
 * Converts subject index to item name and saves to config.
 *
 * @param subject Subject containing selected index
 * @param items Item names vector (must match dropdown order)
 * @param config_path JSON path to save to (e.g., "/printer/bed_heater")
 * @param log_prefix Logging prefix (e.g., "[Wizard Bed]")
 * @return true if saved successfully, false otherwise
 */
bool save_dropdown_selection(lv_subject_t* subject, const std::vector<std::string>& items,
                             const char* config_path, const char* log_prefix);

/**
 * @brief Initialize and register int subject
 *
 * Initializes integer subject and registers it with LVGL XML system.
 *
 * @param subject Subject to initialize
 * @param initial_value Initial value
 * @param subject_name XML registration name (e.g., "bed_heater_selected")
 *
 * Registers with @p subjects, whose deinit_all() withdraws the XML name.
 */
void init_int_subject(SubjectManager& subjects, lv_subject_t* subject, int32_t initial_value,
                      const char* subject_name);
} // namespace wizard
} // namespace ui
} // namespace helix
