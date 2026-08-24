// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esp_lcd_panel_ops.h"

// Brings up the RGB panel (reset, esp_lcd config, bounce buffers) and the
// LEDC backlight at full brightness. Aborts on failure (nothing to fall
// back to on a display device).
esp_lcd_panel_handle_t board_display_init(void);

// 0-100. LEDC 11-bit duty under the hood.
void board_display_backlight(uint8_t percent);
