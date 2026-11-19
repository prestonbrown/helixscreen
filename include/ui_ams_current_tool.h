// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

/**
 * @file ui_ams_current_tool.h
 * @brief AMS current tool indicator widget
 *
 * Compact display showing the currently active tool/filament during a print.
 * Displays tool number (T0, T1, etc.) with a color swatch showing the filament color.
 *
 * Auto-hides when:
 * - No AMS is available (slot_count == 0)
 * - No tool is active (current_tool == -1)
 *
 * Click opens the AMS panel for slot management.
 *
 * Usage:
 * 1. Call ui_ams_current_tool_init() during app startup (registers callbacks)
 * 2. Create widget via XML: lv_xml_create(parent, "ams_current_tool", NULL)
 * 3. Call ui_ams_current_tool_setup(widget) to initialize color binding
 *
 * @code{.cpp}
 * // At startup (xml_registration.cpp):
 * ui_ams_current_tool_init();
 *
 * // When creating the widget:
 * lv_obj_t* widget = lv_xml_create(parent, "ams_current_tool", NULL);
 * ui_ams_current_tool_setup(widget);
 * @endcode
 */

/**
 * @brief Initialize the ams_current_tool module
 *
 * Registers XML event callbacks. Must be called once during app startup,
 * BEFORE creating any ams_current_tool widgets.
 */
void ui_ams_current_tool_init();

/**
 * @brief Set up a created ams_current_tool widget
 *
 * Initializes color swatch binding and cleanup callbacks.
 * Call after lv_xml_create() for each ams_current_tool instance.
 *
 * @param widget The ams_current_tool widget created via lv_xml_create()
 */
void ui_ams_current_tool_setup(lv_obj_t* widget);
