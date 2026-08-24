// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>

// I2C bus + GT911 + LVGL pointer indev. Call on the UI pthread after lv_init.
// Returns false if the controller could not be brought up (typically a
// degraded touch ribbon failing the GT911 probe); in that case no indev is
// registered and the board runs display-only rather than resetting.
bool touch_input_init(void);

// Whether touch_input_init() registered a working pointer indev. The main/-side
// query for any future consumer; the boot warning path gets the value pushed
// down via app_boot_set_touch_available() instead (main depends on helixapp,
// never the reverse). No extern "C" guard, like its main/ siblings — include
// from C only.
bool touch_input_available(void);
