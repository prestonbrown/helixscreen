// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ui_wizard_hardware_selector.h
 * @brief Shared utilities for wizard hardware selector screens
 *
 * Common dropdown population and config persistence logic for the wizard's
 * hardware selector screens (heater and LED select). A screen calls these
 * helpers instead of duplicating discovery/populate logic.
 */

#pragma once

#include "ui_wizard_helpers.h"

#include "lvgl/lvgl.h"

#include <functional>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerAPI;
}
class PrinterHardware;

/**
 * @brief Discover, populate, and restore a hardware selector dropdown
 *
 * The steps every caller needs:
 * 1. Discover hardware from IMoonrakerAPI (via moonraker_getter)
 * 2. Filter by object type and optional prefix
 * 3. Build dropdown options (with optional "None")
 * 4. Populate dropdown widget
 * 5. Restore saved config or guess fallback
 *
 * @param root Root widget containing the dropdown
 * @param dropdown_name Widget name to find (e.g., "hotend_heater_dropdown")
 * @param subject Subject to bind selection to
 * @param items_out Output vector for discovered items (needed for save later)
 * @param moonraker_getter Function to get the hardware list from the API
 * @param prefix_filter Optional prefix filter (e.g., "extruder", "heater_bed")
 * @param allow_none Add "None" option at end
 * @param config_key Config persistence key (e.g., "wizard.hotend.heater")
 * @param guess_fallback Optional fallback if config not found (uses PrinterHardware)
 * @param log_prefix Logging prefix (e.g., "[Wizard Hotend]")
 * @param display_name Optional label for each item (e.g., display_name_for(DeviceType::HEATER))
 * @return true if dropdown found and populated
 */
bool wizard_populate_hardware_dropdown(
    lv_obj_t* root, const char* dropdown_name, lv_subject_t* subject,
    std::vector<std::string>& items_out,
    std::function<const std::vector<std::string>&(helix::IMoonrakerAPI*)> moonraker_getter,
    const char* prefix_filter, bool allow_none, const char* config_key,
    std::function<std::string(const PrinterHardware&)> guess_fallback, const char* log_prefix,
    helix::ui::wizard::ItemNamer display_name = nullptr);
