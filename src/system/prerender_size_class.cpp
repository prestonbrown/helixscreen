// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "prerender_size_class.h"

#include <cstring>

namespace helix {

const char* get_splash_size_name(int screen_width) {
    if (screen_width < 600) {
        return "tiny"; // 480x320 class
    } else if (screen_width < 900) {
        return "small"; // 800x480 class
    } else if (screen_width < 1100) {
        return "medium"; // 1024x600 class
    } else {
        return "large"; // 1280x720+ class
    }
}

const char* get_splash_3d_size_name(int screen_width, int screen_height) {
    // Wide but very short, e.g. 1920x440
    if (screen_width >= 1100 && screen_height < 500) {
        return "ultrawide";
    }

    if (screen_width < 600) {
        // 480x400 panels take a taller composite than 480x320 ones. No device
        // in assets/config/platforms.json currently selects tiny_alt.
        return (screen_height >= 380) ? "tiny_alt" : "tiny";
    } else if (screen_width < 900) {
        return "small"; // 800x480 class
    } else if (screen_width < 1100) {
        return "medium"; // 1024x600 class
    } else {
        return "large"; // 1280x720+ class
    }
}

int get_splash_3d_target_height(const char* size_name) {
    // Must match SCREEN_SIZES in scripts/gen_splash_3d.py
    if (strcmp(size_name, "tiny") == 0)
        return 320;
    if (strcmp(size_name, "tiny_alt") == 0)
        return 400;
    if (strcmp(size_name, "small") == 0)
        return 480;
    if (strcmp(size_name, "medium") == 0)
        return 600;
    if (strcmp(size_name, "large") == 0)
        return 720;
    if (strcmp(size_name, "ultrawide") == 0)
        return 440;
    return 0; // Unknown — caller should fall back to runtime scaling
}

int get_printer_image_size(int screen_width) {
    return (screen_width >= 600) ? 300 : 150;
}

} // namespace helix
