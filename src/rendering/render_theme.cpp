// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render_theme.h"

namespace helix::render_theme {

namespace {
ColorSource g_source = nullptr;
} // namespace

void set_color_source(ColorSource source) {
    g_source = source;
}

lv_color_t color(const char* token) {
    return g_source ? g_source(token) : lv_color_hex(0x000000);
}

} // namespace helix::render_theme
