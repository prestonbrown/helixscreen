/**
 * @file ui_panel_glyphs.h
 * @brief LVGL Symbol Glyphs Display Panel
 *
 * Displays all LVGL built-in symbol glyphs with their symbolic names
 * in a scrollable interface. Useful for reference and testing.
 *
 * Copyright (C) 2025 Preston Brown
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UI_PANEL_GLYPHS_H
#define UI_PANEL_GLYPHS_H

#include <lvgl/lvgl.h>

/**
 * @brief Create and initialize the glyphs panel
 *
 * Creates a panel displaying all LVGL symbol glyphs with their names.
 * The panel features:
 * - Scrollable vertical list of all symbols
 * - Each entry shows icon + symbolic name (e.g., "LV_SYMBOL_AUDIO")
 * - Count of total symbols in header
 * - Proper theming via globals.xml constants
 *
 * @param parent Parent object to attach panel to
 * @return lv_obj_t* The created panel object
 */
lv_obj_t* ui_panel_glyphs_create(lv_obj_t* parent);

#endif // UI_PANEL_GLYPHS_H
