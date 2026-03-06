// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "asset_manager.h"

#include "theme_manager.h"
#include "ui_fonts.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

// Static member definitions
bool AssetManager::s_fonts_registered = false;
bool AssetManager::s_images_registered = false;

void AssetManager::register_fonts() {
    if (s_fonts_registered) {
        spdlog::debug("[AssetManager] Fonts already registered, skipping");
        return;
    }

    // Determine breakpoint from the current display's vertical resolution.
    // Fonts only used at larger breakpoints are skipped to save memory
    // (~500-800KB of .rodata pages that won't be faulted in).
    int32_t ver_res = 0;
    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        ver_res = lv_display_get_vertical_resolution(disp);
    }
    const bool is_medium_plus = (ver_res > UI_BREAKPOINT_SMALL_MAX);
    const bool is_large_plus = (ver_res > UI_BREAKPOINT_MEDIUM_MAX);

    int skipped = 0;

    spdlog::trace("[AssetManager] Registering fonts (ver_res={}, medium+={}, large+={})",
                  ver_res, is_medium_plus, is_large_plus);

    // Material Design Icons (various sizes for different UI elements)
    // Source: https://pictogrammers.com/library/mdi/
    // All icon sizes needed at all breakpoints (used in watchdog, XML, etc.)
    lv_xml_register_font(nullptr, "mdi_icons_64", &mdi_icons_64);
    lv_xml_register_font(nullptr, "mdi_icons_48", &mdi_icons_48);
    lv_xml_register_font(nullptr, "mdi_icons_32", &mdi_icons_32);
    lv_xml_register_font(nullptr, "mdi_icons_24", &mdi_icons_24);
    lv_xml_register_font(nullptr, "mdi_icons_16", &mdi_icons_16);
    lv_xml_register_font(nullptr, "mdi_icons_14", &mdi_icons_14);

    // Montserrat text fonts - used by semantic text components:
    // - text_heading uses font_heading (14/20/26/28 for tiny/small/medium/large)
    // - text_body uses font_body (11/14/18/20 for tiny/small/medium/large)
    // - text_small uses font_small (11/12/16/18 for tiny/small/medium/large)
    // NOTE: Registering as "montserrat_*" for XML compatibility but using noto_sans_* fonts
    lv_xml_register_font(nullptr, "montserrat_10", &noto_sans_10);
    lv_xml_register_font(nullptr, "montserrat_12", &noto_sans_12);
    lv_xml_register_font(nullptr, "montserrat_14", &noto_sans_14);
    lv_xml_register_font(nullptr, "montserrat_16", &noto_sans_16);
    lv_xml_register_font(nullptr, "montserrat_18", &noto_sans_18);
    lv_xml_register_font(nullptr, "montserrat_20", &noto_sans_20);
    lv_xml_register_font(nullptr, "montserrat_24", &noto_sans_24);
    // montserrat_26: only font_heading_medium
    if (is_medium_plus) {
        lv_xml_register_font(nullptr, "montserrat_26", &noto_sans_26);
    } else {
        skipped++;
    }
    // montserrat_28: only font_heading_large
    if (is_large_plus) {
        lv_xml_register_font(nullptr, "montserrat_28", &noto_sans_28);
    } else {
        skipped++;
    }

    // Noto Sans fonts - same sizes as Montserrat, with extended Unicode support
    // (includes ©®™€£¥°±•… and other symbols)
    lv_xml_register_font(nullptr, "noto_sans_10", &noto_sans_10);
    lv_xml_register_font(nullptr, "noto_sans_11", &noto_sans_11);
    lv_xml_register_font(nullptr, "noto_sans_12", &noto_sans_12);
    lv_xml_register_font(nullptr, "noto_sans_14", &noto_sans_14);
    lv_xml_register_font(nullptr, "noto_sans_16", &noto_sans_16);
    lv_xml_register_font(nullptr, "noto_sans_18", &noto_sans_18);
    lv_xml_register_font(nullptr, "noto_sans_20", &noto_sans_20);
    lv_xml_register_font(nullptr, "noto_sans_24", &noto_sans_24);
    // noto_sans_26: only font_heading_medium
    if (is_medium_plus) {
        lv_xml_register_font(nullptr, "noto_sans_26", &noto_sans_26);
    } else {
        skipped++;
    }
    // noto_sans_28: only font_heading_large
    if (is_large_plus) {
        lv_xml_register_font(nullptr, "noto_sans_28", &noto_sans_28);
    } else {
        skipped++;
    }

    // Noto Sans Light fonts (for text_small and text_xs)
    lv_xml_register_font(nullptr, "noto_sans_light_10", &noto_sans_light_10);
    lv_xml_register_font(nullptr, "noto_sans_light_11", &noto_sans_light_11);
    lv_xml_register_font(nullptr, "noto_sans_light_12", &noto_sans_light_12);
    // noto_sans_light_14: only font_xs_large
    if (is_large_plus) {
        lv_xml_register_font(nullptr, "noto_sans_light_14", &noto_sans_light_14);
    } else {
        skipped++;
    }
    // noto_sans_light_16: only font_small_medium
    if (is_medium_plus) {
        lv_xml_register_font(nullptr, "noto_sans_light_16", &noto_sans_light_16);
    } else {
        skipped++;
    }
    // noto_sans_light_18: only font_small_large
    if (is_large_plus) {
        lv_xml_register_font(nullptr, "noto_sans_light_18", &noto_sans_light_18);
    } else {
        skipped++;
    }

    // Noto Sans Bold fonts — all registered unconditionally because they're
    // used directly in C++ (watchdog: bold_16/24) and XML (debug modal: bold_28)
    lv_xml_register_font(nullptr, "noto_sans_bold_14", &noto_sans_bold_14);
    lv_xml_register_font(nullptr, "noto_sans_bold_16", &noto_sans_bold_16);
    lv_xml_register_font(nullptr, "noto_sans_bold_18", &noto_sans_bold_18);
    lv_xml_register_font(nullptr, "noto_sans_bold_20", &noto_sans_bold_20);
    lv_xml_register_font(nullptr, "noto_sans_bold_24", &noto_sans_bold_24);
    lv_xml_register_font(nullptr, "noto_sans_bold_28", &noto_sans_bold_28);

    // Source Code Pro - Monospace (for console/terminal displays)
    lv_xml_register_font(nullptr, "source_code_pro_10", &source_code_pro_10);
    lv_xml_register_font(nullptr, "source_code_pro_12", &source_code_pro_12);
    lv_xml_register_font(nullptr, "source_code_pro_14", &source_code_pro_14);
    lv_xml_register_font(nullptr, "source_code_pro_16", &source_code_pro_16);

    s_fonts_registered = true;
    if (skipped > 0) {
        spdlog::info("[AssetManager] Fonts registered ({} skipped for breakpoint)", skipped);
    } else {
        spdlog::trace("[AssetManager] All fonts registered (large+ breakpoint)");
    }
}

void AssetManager::register_images() {
    if (s_images_registered) {
        spdlog::debug("[AssetManager] Images already registered, skipping");
        return;
    }

    spdlog::trace("[AssetManager] Registering images...");

    // Branding
    lv_xml_register_image(nullptr, "A:assets/images/helixscreen-logo.png",
                          "A:assets/images/helixscreen-logo.png");
    lv_xml_register_image(nullptr, "A:assets/images/about-logo.bin",
                          "A:assets/images/about-logo.bin");

    // Printer and UI images
    lv_xml_register_image(nullptr, "A:assets/images/printer_400.png",
                          "A:assets/images/printer_400.png");
    lv_xml_register_image(nullptr, "filament_spool", "A:assets/images/filament_spool.png");
    lv_xml_register_image(nullptr, "A:assets/images/placeholder_thumb_centered.png",
                          "A:assets/images/placeholder_thumb_centered.png");
    lv_xml_register_image(nullptr, "A:assets/images/thumbnail-gradient-bg.png",
                          "A:assets/images/thumbnail-gradient-bg.png");
    lv_xml_register_image(nullptr, "A:assets/images/thumbnail-placeholder.png",
                          "A:assets/images/thumbnail-placeholder.png");
    lv_xml_register_image(nullptr, "A:assets/images/thumbnail-placeholder-160.png",
                          "A:assets/images/thumbnail-placeholder-160.png");
    lv_xml_register_image(nullptr, "A:assets/images/benchy_thumbnail_white.png",
                          "A:assets/images/benchy_thumbnail_white.png");

    // Pre-rendered gradient backgrounds (LVGL native .bin format for fast blitting)
    // Only medium variants are used by XML (card: print_file_card, panel: print_status/detail/history)
    // Theme manager swaps -dark/-light suffixes at runtime
    lv_xml_register_image(nullptr, "A:assets/images/gradient-card-medium-dark.bin",
                          "A:assets/images/gradient-card-medium-dark.bin");
    lv_xml_register_image(nullptr, "A:assets/images/gradient-card-medium-light.bin",
                          "A:assets/images/gradient-card-medium-light.bin");
    lv_xml_register_image(nullptr, "A:assets/images/gradient-panel-medium-dark.bin",
                          "A:assets/images/gradient-panel-medium-dark.bin");
    lv_xml_register_image(nullptr, "A:assets/images/gradient-panel-medium-light.bin",
                          "A:assets/images/gradient-panel-medium-light.bin");
    // Pre-rendered placeholder thumbnails (for file cards without embedded thumbnails)
    lv_xml_register_image(nullptr, "A:assets/images/prerendered/thumbnail-placeholder-160.bin",
                          "A:assets/images/prerendered/thumbnail-placeholder-160.bin");
    lv_xml_register_image(nullptr, "A:assets/images/prerendered/benchy_thumbnail_white.bin",
                          "A:assets/images/prerendered/benchy_thumbnail_white.bin");

    // Flag icons (language chooser wizard) - pre-rendered ARGB8888 32x24
    lv_xml_register_image(nullptr, "flag_en", "A:assets/images/flags/flag_en.bin");
    lv_xml_register_image(nullptr, "flag_de", "A:assets/images/flags/flag_de.bin");
    lv_xml_register_image(nullptr, "flag_fr", "A:assets/images/flags/flag_fr.bin");
    lv_xml_register_image(nullptr, "flag_es", "A:assets/images/flags/flag_es.bin");
    lv_xml_register_image(nullptr, "flag_ru", "A:assets/images/flags/flag_ru.bin");
    lv_xml_register_image(nullptr, "flag_pt", "A:assets/images/flags/flag_pt.bin");
    lv_xml_register_image(nullptr, "flag_it", "A:assets/images/flags/flag_it.bin");
    lv_xml_register_image(nullptr, "flag_zh", "A:assets/images/flags/flag_zh.bin");
    lv_xml_register_image(nullptr, "flag_ja", "A:assets/images/flags/flag_ja.bin");

    s_images_registered = true;
    spdlog::trace("[AssetManager] Images registered successfully");
}

void AssetManager::register_all() {
    register_fonts();
    register_images();
}

bool AssetManager::fonts_registered() {
    return s_fonts_registered;
}

bool AssetManager::images_registered() {
    return s_images_registered;
}
