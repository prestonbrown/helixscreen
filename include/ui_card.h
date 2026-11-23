// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * Register the ui_card component with the LVGL XML system
 * Must be called before any XML files using <ui_card> are registered
 *
 * Card colors are fetched dynamically via theme_manager_get_color("card_bg")
 * which automatically uses light/dark variants based on current theme mode.
 * A card is not clickable unless its XML sets clickable="true" (or C++ sets
 * the flag with the handler it attaches): a card without an action neither
 * presses nor takes the touch from the container behind it.
 */
void ui_card_register(void);

#ifdef __cplusplus
}
#endif
