// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - Fatal Error Display Implementation
//
// BOOTSTRAP COMPONENT: Runs BEFORE the XML/theme system is loaded.
// Cannot use theme_manager_get_color() or design tokens - they require globals.xml to be loaded.
// Colors below are hardcoded but named for clarity and to match theme token semantics.

#include "ui_fatal_error.h"

#include "ui_fonts.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <lvgl.h>

// =============================================================================
// Bootstrap Color Constants
// =============================================================================
// These colors are used before the theme system is available. They are named
// to match theme token semantics for documentation purposes. When the theme
// system is initialized, these same semantic names (danger, warning, etc.)
// are available via theme_manager_get_color().
//
// Color mapping to theme tokens:
//   kBootstrapDangerDark  -> "danger" (dark variant, screen background)
//   kBootstrapCardBg      -> "card_bg" (container background)
//   kBootstrapDanger      -> "danger" (border, icon)
//   kBootstrapText        -> "text" (title text)
//   kBootstrapTextMuted   -> "text_muted" (message text)
//   kBootstrapWarning     -> "warning" (troubleshooting header)
//   kBootstrapTextSubtle  -> "text_subtle" (suggestion text)
// =============================================================================
namespace {
constexpr uint32_t kBootstrapDangerDark = 0x8B0000; // Dark red for error screen background
constexpr uint32_t kBootstrapCardBg = 0x2D2D2D;     // Dark gray card background
constexpr uint32_t kBootstrapDanger = 0xFF4444;     // Bright red for danger/error emphasis
constexpr uint32_t kBootstrapText = 0xFFFFFF;       // White text (light variant)
constexpr uint32_t kBootstrapTextMuted = 0xCCCCCC;  // Muted gray for secondary text
constexpr uint32_t kBootstrapWarning = 0xFFCC00;    // Yellow/gold for warnings
constexpr uint32_t kBootstrapTextSubtle = 0xAAAAAA; // Subtle gray for tertiary text
} // namespace

// Portable timing functions
#ifdef HELIX_DISPLAY_SDL
#include <SDL.h>
inline uint32_t fatal_get_ticks() {
    return SDL_GetTicks();
}
inline void fatal_delay(uint32_t ms) {
    SDL_Delay(ms);
}
#else
#include <time.h>
inline uint32_t fatal_get_ticks() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
inline void fatal_delay(uint32_t ms) {
    struct timespec ts = {static_cast<time_t>(ms / 1000),
                          static_cast<long>((ms % 1000) * 1000000L)};
    nanosleep(&ts, nullptr);
}
#endif

void ui_show_fatal_error(const char* title, const char* message, const char* const* suggestions,
                         uint32_t display_ms) {
    lv_obj_t* screen = lv_screen_active();

    // Red background to indicate error
    lv_obj_set_style_bg_color(screen, lv_color_hex(kBootstrapDangerDark), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Container for content
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_PCT(90), LV_PCT(90));
    lv_obj_center(container);
    lv_obj_set_style_bg_color(container, lv_color_hex(kBootstrapCardBg), 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_border_color(container, lv_color_hex(kBootstrapDanger), 0);
    lv_obj_set_style_radius(container, 8, 0);
    lv_obj_set_style_pad_all(container, 20, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Warning icon (using MDI icon font)
    lv_obj_t* icon = lv_label_create(container);
    lv_label_set_text(icon, ICON_TRIANGLE_EXCLAMATION);
    lv_obj_set_style_text_font(icon, &mdi_icons_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(kBootstrapDanger), 0);

    // Title
    lv_obj_t* title_label = lv_label_create(container);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &noto_sans_24, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(kBootstrapText), 0);
    lv_obj_set_style_pad_top(title_label, 10, 0);

    // Message
    lv_obj_t* msg_label = lv_label_create(container);
    lv_label_set_text(msg_label, message);
    lv_obj_set_style_text_font(msg_label, &noto_sans_16, 0);
    lv_obj_set_style_text_color(msg_label, lv_color_hex(kBootstrapTextMuted), 0);
    lv_obj_set_style_pad_top(msg_label, 15, 0);
    lv_obj_set_width(msg_label, LV_PCT(100));
    lv_label_set_long_mode(msg_label, LV_LABEL_LONG_WRAP);

    // Suggestions header and list
    if (suggestions && suggestions[0]) {
        lv_obj_t* suggest_header = lv_label_create(container);
        lv_label_set_text(suggest_header, lv_tr("Troubleshooting:"));
        lv_obj_set_style_text_font(suggest_header, &noto_sans_14, 0);
        lv_obj_set_style_text_color(suggest_header, lv_color_hex(kBootstrapWarning), 0);
        lv_obj_set_style_pad_top(suggest_header, 20, 0);

        // List suggestions
        for (int i = 0; suggestions[i] != nullptr; i++) {
            lv_obj_t* suggest = lv_label_create(container);
            lv_label_set_text_fmt(suggest, "• %s", suggestions[i]);
            lv_obj_set_style_text_font(suggest, &noto_sans_12, 0);
            lv_obj_set_style_text_color(suggest, lv_color_hex(kBootstrapTextSubtle), 0);
            lv_obj_set_style_pad_top(suggest, 5, 0);
            lv_obj_set_width(suggest, LV_PCT(100));
            lv_label_set_long_mode(suggest, LV_LABEL_LONG_WRAP);
        }
    }

    // Run LVGL to display the error
    uint32_t start = fatal_get_ticks();
    while (display_ms == 0 || (fatal_get_ticks() - start) < display_ms) {
        lv_timer_handler();
        fatal_delay(10);
    }
}
