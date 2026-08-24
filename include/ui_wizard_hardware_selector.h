// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ui_wizard_hardware_selector.h
 * @brief Shared utilities for wizard hardware selector screens
 *
 * Eliminates ~700 lines of duplicate code across 4 wizard screens by providing
 * common dropdown population and config persistence logic.
 *
 * **Pattern Before (246 lines per file):**
 * - ui_wizard_hotend_select.cpp
 * - ui_wizard_bed_select.cpp
 * - ui_wizard_fan_select.cpp
 * - ui_wizard_led_select.cpp
 *
 * **Pattern After (~50 lines per file):**
 * Just call these helpers instead of duplicating discovery/populate logic.
 */

#pragma once

#include "device_display_name.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Forward declarations
class IMoonrakerAPI;
class PrinterHardware;

/**
 * @brief Generic dropdown change callback for wizard selectors
 *
 * Updates subject when dropdown selection changes. Use with lv_xml_register_event_cb().
 *
 * **User data:** Pass lv_subject_t* as user_data when registering callback.
 *
 * @param e Event with dropdown target
 */
void wizard_hardware_dropdown_changed_cb(lv_event_t* e);

/**
 * @brief Discover, populate, and restore a hardware selector dropdown
 *
 * This consolidates the pattern used in all 4 wizard selector screens:
 * 1. Discover hardware from MoonrakerClient
 * 2. Filter by object type and optional prefix
 * 3. Build dropdown options (with optional "None")
 * 4. Populate dropdown widget
 * 5. Restore saved config or guess fallback
 *
 * @param root Root widget containing the dropdown
 * @param dropdown_name Widget name to find (e.g., "hotend_heater_dropdown")
 * @param subject Subject to bind selection to
 * @param items_out Output vector for discovered items (needed for save later)
 * @param moonraker_getter Function to get hardware list from client
 * @param prefix_filter Optional prefix filter (e.g., "extruder", "heater_bed")
 * @param allow_none Add "None" option at end
 * @param config_key Config persistence key (e.g., "wizard.hotend.heater")
 * @param guess_fallback Optional fallback if config not found (uses PrinterHardware)
 * @param log_prefix Logging prefix (e.g., "[Wizard Hotend]")
 * @param device_type Optional device type for friendly name display (e.g., DeviceType::HEATER)
 * @return true if dropdown found and populated
 */
bool wizard_populate_hardware_dropdown(
    lv_obj_t* root, const char* dropdown_name, lv_subject_t* subject,
    std::vector<std::string>& items_out,
    std::function<const std::vector<std::string>&(IMoonrakerAPI*)> moonraker_getter,
    const char* prefix_filter, bool allow_none, const char* config_key,
    std::function<std::string(const PrinterHardware&)> guess_fallback, const char* log_prefix,
    std::optional<helix::DeviceType> device_type = std::nullopt);
