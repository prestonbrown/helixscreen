// SPDX-License-Identifier: GPL-3.0-or-later
#include "moonraker_error.h"

#include "lvgl/src/others/translation/lv_translation.h"

// Out of line so moonraker_error.h stays free of LVGL: the ESP32 network
// component includes it without an lv_conf.h.
std::string MoonrakerError::localized_message() const {
    const char* tag = display_tag();
    return tag ? std::string(lv_tr(tag)) : message;
}
