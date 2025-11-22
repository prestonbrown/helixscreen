// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "lvgl/lvgl.h"

/**
 * @brief Navigation panel identifiers
 *
 * Order matches app_layout.xml panel children for index-based access.
 */
typedef enum {
    UI_PANEL_HOME,         ///< Panel 0: Home
    UI_PANEL_PRINT_SELECT, ///< Panel 1: Print Select (beneath Home)
    UI_PANEL_CONTROLS,     ///< Panel 2: Controls
    UI_PANEL_FILAMENT,     ///< Panel 3: Filament
    UI_PANEL_SETTINGS,     ///< Panel 4: Settings
    UI_PANEL_ADVANCED,     ///< Panel 5: Advanced
    UI_PANEL_COUNT         ///< Total number of panels
} ui_panel_id_t;

/**
 * @brief Initialize navigation system with reactive subjects
 *
 * Sets up reactive subjects for icon colors and panel visibility.
 * MUST be called BEFORE creating navigation bar XML to ensure
 * bindings can connect to subjects.
 *
 * Call order: ui_nav_init() → create XML → ui_nav_wire_events()
 */
void ui_nav_init();

/**
 * @brief Wire up event handlers to navigation bar widget
 *
 * Attaches click handlers to navbar icons for panel switching.
 * Call this after creating navigation_bar component from XML.
 *
 * @param navbar Navigation bar widget created from XML
 */
void ui_nav_wire_events(lv_obj_t* navbar);

/**
 * @brief Set active panel
 *
 * Updates active panel state and triggers reactive icon color updates
 * via subject notifications. Also manages panel visibility.
 *
 * @param panel_id Panel identifier to activate
 */
void ui_nav_set_active(ui_panel_id_t panel_id);

/**
 * @brief Get current active panel
 *
 * @return Currently active panel identifier
 */
ui_panel_id_t ui_nav_get_active();

/**
 * @brief Register panel widgets for show/hide management
 *
 * Stores references to panel widgets so navigation system can
 * control their visibility. Array should have UI_PANEL_COUNT
 * elements; NULL entries are allowed for not-yet-created panels.
 *
 * @param panels Array of panel widgets (size: UI_PANEL_COUNT)
 */
void ui_nav_set_panels(lv_obj_t** panels);

/**
 * @brief Set app_layout widget reference
 *
 * Stores reference to prevent hiding app_layout when dismissing
 * overlay panels. App layout should remain visible at all times.
 *
 * @param app_layout Main application layout widget
 */
void ui_nav_set_app_layout(lv_obj_t* app_layout);

/**
 * @brief Initialize overlay backdrop widget
 *
 * Creates a shared backdrop widget used by all overlay panels.
 * Should be called during ui_nav_init() to create the backdrop.
 * The backdrop is hidden by default and shown/hidden by push_overlay/go_back.
 *
 * @param screen Screen to add backdrop to
 */
void ui_nav_init_overlay_backdrop(lv_obj_t* screen);

/**
 * @brief Push overlay panel onto navigation history stack
 *
 * Shows the overlay panel and pushes it onto history stack.
 * Used for modal panels (motion, temp, extrusion, etc.) that
 * appear over main navigation. Automatically shows the shared backdrop.
 *
 * When overlay's back button is pressed, ui_nav_go_back()
 * restores the previous panel and hides the backdrop if no overlays remain.
 *
 * @param overlay_panel Overlay panel widget to show
 */
void ui_nav_push_overlay(lv_obj_t* overlay_panel);

/**
 * @brief Navigate back to previous panel
 *
 * Pops current overlay from history stack, hides it, and
 * shows the previous panel. Used for back button handling.
 *
 * @return true if navigation occurred, false if history empty
 */
bool ui_nav_go_back();
