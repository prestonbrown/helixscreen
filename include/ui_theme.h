#pragma once

#include "lvgl/lvgl.h"

// Color scheme constants
#define UI_COLOR_NAV_BG        lv_color_hex(0x242424)     // rgb(36, 36, 36) - Nav bar background
#define UI_COLOR_PRIMARY       lv_color_hex(0x6F111A)     // rgb(111, 17, 26) - Dark cherry red
#define UI_COLOR_PANEL_BG      lv_color_hex(0x141414)     // rgb(20, 20, 20) - Main panel background
#define UI_COLOR_TEXT_PRIMARY  lv_color_hex(0xFFFFFF)     // White text
#define UI_COLOR_TEXT_SECONDARY lv_color_hex(0xAAAAAA)     // Gray text

// Layout constants
#define UI_NAV_WIDTH_PERCENT   10                          // Nav bar is 1/10th of screen width
#define UI_NAV_ICON_SIZE       64                          // Base icon size for 1024x800
#define UI_NAV_PADDING         16                          // Padding between elements

// Screen size targets
#define UI_SCREEN_LARGE_W      1280
#define UI_SCREEN_LARGE_H      720
#define UI_SCREEN_MEDIUM_W     1024
#define UI_SCREEN_MEDIUM_H     800
#define UI_SCREEN_SMALL_W      800
#define UI_SCREEN_SMALL_H      480

// Calculate nav width based on actual screen
#define UI_NAV_WIDTH(screen_w) ((screen_w) / 10)

