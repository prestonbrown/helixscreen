// SPDX-License-Identifier: GPL-3.0-or-later
//
// BTT K-Touch board table. Pin map verified on-device by firmware/ktouch-probe
// (color bars + GT911 product-ID read). New boards add a sibling header and a
// -DHELIX_BOARD=<name> selection; they are a config entry, not a fork.
#pragma once

#define BOARD_NAME "btt-ktouch"

#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480
#define BOARD_LCD_PCLK_HZ 14800000
#define BOARD_LCD_HSYNC_PW 4
#define BOARD_LCD_HSYNC_BP 16
#define BOARD_LCD_HSYNC_FP 16
#define BOARD_LCD_VSYNC_PW 4
#define BOARD_LCD_VSYNC_BP 32
#define BOARD_LCD_VSYNC_FP 32

#define BOARD_LCD_PIN_RESET 46
#define BOARD_LCD_PIN_DE 38
#define BOARD_LCD_PIN_PCLK 5
// R0-R4, G0-G5, B0-B4 (16-bit RGB565 parallel)
#define BOARD_LCD_DATA_PINS {17, 18, 48, 47, 39, 11, 12, 13, 14, 15, 16, 6, 7, 8, 9, 10}

#define BOARD_BACKLIGHT_PIN 21

#define BOARD_TOUCH_I2C_SCL 1
#define BOARD_TOUCH_I2C_SDA 2
#define BOARD_TOUCH_PIN_IRQ 40
#define BOARD_TOUCH_PIN_RST 41
// GT911 at 0x5D (IRQ held low through reset); only device on the bus.
