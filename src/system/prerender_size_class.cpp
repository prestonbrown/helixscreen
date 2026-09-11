// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "prerender_size_class.h"

#include "ui_breakpoint.h"

#include <algorithm>
#include <cstring>

namespace helix {

const char* get_splash_size_name(int screen_width, int screen_height) {
    return get_splash_3d_size_name(screen_width, screen_height);
}

const char* get_splash_3d_size_name(int screen_width, int screen_height) {
    // A bar display is wide and very short. Its canvas looks nothing like the
    // squarer panel its narrow axis would otherwise place it beside, so it gets
    // its own class rather than a tier.
    if (screen_width >= 1100 && screen_height < 500) {
        return "ultrawide";
    }

    // breakpoint_for() is the project's one resolution ladder, and it keys off
    // the narrow axis - which is exactly what a full-screen canvas has to fit
    // into. Deriving the asset class from it is what keeps "medium" meaning the
    // same resolution here as it does in a layout override.
    //
    // The xxlarge slot repeats "xlarge" because nothing is composited above it:
    // a larger panel takes the biggest canvas there is and the caller scales,
    // which is what get_splash_3d_target_height() lets it detect.
    return responsive_pick<const char*>(breakpoint_for(std::min(screen_width, screen_height)),
                                        "micro", "tiny", "small", "medium", "large", "xlarge",
                                        "xlarge");
}

int get_splash_3d_target_height(const char* size_name) {
    // Must match SCREEN_SIZES in scripts/gen_splash_3d.py
    if (strcmp(size_name, "micro") == 0)
        return 272;
    if (strcmp(size_name, "tiny") == 0)
        return 320;
    if (strcmp(size_name, "small") == 0)
        return 400;
    if (strcmp(size_name, "medium") == 0)
        return 480;
    if (strcmp(size_name, "large") == 0)
        return 600;
    if (strcmp(size_name, "xlarge") == 0)
        return 720;
    if (strcmp(size_name, "ultrawide") == 0)
        return 440;
    return 0; // Unknown — caller should fall back to runtime scaling
}

int get_printer_image_size(int screen_width) {
    // Width, not the narrow axis: this sizes a widget's artwork against the
    // horizontal room it is given, rather than picking a full-screen canvas.
    return (screen_width >= 600) ? 300 : 150;
}

} // namespace helix
