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
 * The card takes ThemeManager's shared StyleRole::Card style (CardPressed while
 * pressed), so it follows theme and dark-mode changes.
 */
void ui_card_register(void);

#ifdef __cplusplus
}
#endif
