// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

/**
 * Initialize header_bar component system
 * Registers global resize handler
 * Call this once during app initialization
 */
void ui_component_header_bar_init();

/**
 * Setup a header_bar instance for responsive height management
 * Call this in panel setup functions after finding the header widget
 * @param header The header_bar widget instance
 * @param screen The parent screen for measuring height
 */
void ui_component_header_bar_setup(lv_obj_t* header, lv_obj_t* screen);
