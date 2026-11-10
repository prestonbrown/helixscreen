// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cjk_filename_glyphs.h"

#include "cjk_font_manager.h"
#include "core/gcode_filename.h"
#include "core/update_queue.h"
#include "theme_manager.h"

#include <atomic>

namespace helix {

namespace {

// text_body, text_small and text_xs: the widgets every filename label is.
void load_for_filename_fonts() {
    system::CjkFontManager::instance().ensure_filename_glyphs({theme_manager_get_font("font_body"),
                                                               theme_manager_get_font("font_small"),
                                                               theme_manager_get_font("font_xs")});
}

std::atomic<bool> g_load_queued{false};

void on_displayed_filename(std::string_view /*name*/) {
    if (ui::UpdateQueue::is_main_thread()) {
        load_for_filename_fonts();
        return;
    }
    // A filename formatted off the main thread reaches its label through the
    // queue, so a load queued now still lands before that label renders.
    if (!g_load_queued.exchange(true)) {
        ui::queue_update("cjk_filename_glyphs", [] {
            g_load_queued.store(false);
            load_for_filename_fonts();
        });
    }
}

} // namespace

void install_cjk_filename_glyphs() {
    gcode::set_displayed_filename_hook(&on_displayed_filename);
}

void refresh_cjk_filename_glyphs() {
    if (!system::CjkFontManager::instance().filename_glyph_sizes().empty())
        load_for_filename_fonts();
}

} // namespace helix
