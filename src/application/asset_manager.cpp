// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "asset_manager.h"

#include "ui_fonts.h"

#include "data_root_resolver.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

// Mirrors cjk_font_manager.cpp:10 and theme_manager.cpp:29. Without it, a build
// path that forgot -DHELIX_MAX_FONT_TIER would evaluate every guard below as
// `0 >= N` and silently register almost no fonts -- a blank UI rather than a
// build error. mk/cross.mk always passes it; this is the backstop for the day
// something doesn't.
#ifndef HELIX_MAX_FONT_TIER
#define HELIX_MAX_FONT_TIER 6 // default: all tiers (micro=0 .. xxlarge=6)
#endif

// Static member definitions
bool AssetManager::s_fonts_registered = false;
bool AssetManager::s_images_registered = false;
int AssetManager::s_registered_font_tier = -1;

namespace {

/// Faces every breakpoint needs. Registered once, on the first call.
int register_base_fonts() {
    int n = 0;
    auto reg = [&n](const char* name, const lv_font_t* font) {
        lv_xml_register_font(nullptr, name, font);
        n++;
    };

    // Material Design Icons (various sizes for different UI elements)
    // Source: https://pictogrammers.com/library/mdi/
    // All icon sizes needed at all breakpoints (used in watchdog, XML, etc.)
    reg("mdi_icons_64", &mdi_icons_64);
    reg("mdi_icons_48", &mdi_icons_48);
    reg("mdi_icons_32", &mdi_icons_32);
    reg("mdi_icons_24", &mdi_icons_24);
    reg("mdi_icons_16", &mdi_icons_16);
    reg("mdi_icons_14", &mdi_icons_14);

    // Micro-only 8px faces. Registered whatever the startup tier: a name entry
    // costs ~100 bytes and does not fault the glyph pages, and the symbols are
    // linked unconditionally anyway. Skipping them used to be free because the
    // breakpoint could never fall; now that it can, a window shrunk to MICRO
    // would find font_body_micro unresolvable and get bumped up to the _large
    // tier instead of down (#1210).
    reg("noto_sans_8", &noto_sans_8);
    reg("source_code_pro_8", &source_code_pro_8);

    // Montserrat text fonts - used by semantic text components. Sizes per breakpoint
    // (micro/tiny/small/medium/large/xlarge/xxlarge):
    // - text_heading uses font_heading  (14 / 14 / 20 / 26 / 28 / 32 / 40)
    // - text_body    uses font_body     (10 / 11 / 14 / 18 / 20 / 24 / 32)
    // - text_small   uses font_small    (10 / 11 / 12 / 16 / 18 / 20 / 26)
    // NOTE: Registering as "montserrat_*" for XML compatibility but using noto_sans_* fonts
    reg("montserrat_10", &noto_sans_10);
    reg("montserrat_12", &noto_sans_12);
    reg("montserrat_14", &noto_sans_14);
    reg("montserrat_16", &noto_sans_16);
    reg("montserrat_18", &noto_sans_18);
    reg("montserrat_20", &noto_sans_20);
    reg("montserrat_24", &noto_sans_24);

    // Noto Sans fonts - same sizes as Montserrat, with extended Unicode support
    // (includes ©®™€£¥°±•… and other symbols)
    reg("noto_sans_10", &noto_sans_10);
    reg("noto_sans_11", &noto_sans_11);
    reg("noto_sans_12", &noto_sans_12);
    reg("noto_sans_14", &noto_sans_14);
    reg("noto_sans_16", &noto_sans_16);
    reg("noto_sans_18", &noto_sans_18);
    reg("noto_sans_20", &noto_sans_20);
    reg("noto_sans_24", &noto_sans_24);

    // Noto Sans Light fonts (for text_small and text_xs)
    reg("noto_sans_light_10", &noto_sans_light_10);
    reg("noto_sans_light_11", &noto_sans_light_11);
    reg("noto_sans_light_12", &noto_sans_light_12);

    // Noto Sans Bold fonts — all registered unconditionally because they're
    // used directly in C++ (watchdog: bold_16/24) and XML (debug modal: bold_28)
    reg("noto_sans_bold_14", &noto_sans_bold_14);
    reg("noto_sans_bold_16", &noto_sans_bold_16);
    reg("noto_sans_bold_18", &noto_sans_bold_18);
    reg("noto_sans_bold_20", &noto_sans_bold_20);
    reg("noto_sans_bold_24", &noto_sans_bold_24);
    reg("noto_sans_bold_28", &noto_sans_bold_28);

    // Source Code Pro - Monospace (for console/terminal displays)
    reg("source_code_pro_10", &source_code_pro_10);
    reg("source_code_pro_12", &source_code_pro_12);
    reg("source_code_pro_14", &source_code_pro_14);
    reg("source_code_pro_16", &source_code_pro_16);

    return n;
}

/// Faces that MEDIUM (and up) is the first tier to reference.
int register_medium_tier_fonts() {
#if HELIX_MAX_FONT_TIER >= 3
    lv_xml_register_font(nullptr, "montserrat_26", &noto_sans_26);            // font_heading_medium
    lv_xml_register_font(nullptr, "noto_sans_26", &noto_sans_26);             // font_heading_medium
    lv_xml_register_font(nullptr, "noto_sans_light_16", &noto_sans_light_16); // font_small_medium
    return 3;
#else
    return 0;
#endif
}

/// Faces that LARGE (and up) is the first tier to reference.
int register_large_tier_fonts() {
#if HELIX_MAX_FONT_TIER >= 4
    lv_xml_register_font(nullptr, "montserrat_28", &noto_sans_28);            // font_heading_large
    lv_xml_register_font(nullptr, "noto_sans_28", &noto_sans_28);             // font_heading_large
    lv_xml_register_font(nullptr, "noto_sans_light_14", &noto_sans_light_14); // font_xs_large
    lv_xml_register_font(nullptr, "noto_sans_light_18", &noto_sans_light_18); // font_small_large
    return 4;
#else
    return 0;
#endif
}

/// XLarge tier fonts (HiDPI screens > LARGE_MAX on the constrained axis).
int register_xlarge_tier_fonts() {
#if HELIX_MAX_FONT_TIER >= 5
    lv_xml_register_font(nullptr, "noto_sans_32", &noto_sans_32);
    lv_xml_register_font(nullptr, "noto_sans_bold_32", &noto_sans_bold_32);
    lv_xml_register_font(nullptr, "noto_sans_light_20", &noto_sans_light_20);
    lv_xml_register_font(nullptr, "source_code_pro_18", &source_code_pro_18);
    lv_xml_register_font(nullptr, "mdi_icons_80", &mdi_icons_80);
    return 5;
#else
    return 0;
#endif
}

/// XXLarge tier fonts (HiDPI screens > XLARGE_MAX, e.g. 2560x1440).
int register_xxlarge_tier_fonts() {
#if HELIX_MAX_FONT_TIER >= 6
    lv_xml_register_font(nullptr, "noto_sans_40", &noto_sans_40);
    lv_xml_register_font(nullptr, "noto_sans_bold_40", &noto_sans_bold_40);
    lv_xml_register_font(nullptr, "noto_sans_light_26", &noto_sans_light_26);
    lv_xml_register_font(nullptr, "source_code_pro_20", &source_code_pro_20);
    lv_xml_register_font(nullptr, "source_code_pro_24", &source_code_pro_24);
    lv_xml_register_font(nullptr, "mdi_icons_96", &mdi_icons_96);
    lv_xml_register_font(nullptr, "mdi_icons_128", &mdi_icons_128);
    return 7;
#else
    return 0;
#endif
}

} // namespace

int AssetManager::register_fonts_for_tier(int tier) {
    // Clamp rather than reject: callers derive this from a display resolution
    // and a bogus one should degrade to "the roomiest tier we know", not crash.
    if (tier < to_int(UiBreakpoint::Micro))
        tier = to_int(UiBreakpoint::Micro);
    if (tier > to_int(UiBreakpoint::XXLarge))
        tier = to_int(UiBreakpoint::XXLarge);

    const int prev = s_registered_font_tier;
    if (tier <= prev) {
        // Same or smaller screen. Nothing to add, and nothing may be removed:
        // the lv_font_t objects are static .rodata and every widget built at the
        // old tier still holds a pointer into them.
        spdlog::trace("[AssetManager] Font tier {} already covered by tier {}, skipping", tier,
                      prev);
        return 0;
    }

    int registered = 0;
    if (prev < 0) {
        registered += register_base_fonts();
    }
    if (tier >= to_int(UiBreakpoint::Medium) && prev < to_int(UiBreakpoint::Medium)) {
        registered += register_medium_tier_fonts();
    }
    if (tier >= to_int(UiBreakpoint::Large) && prev < to_int(UiBreakpoint::Large)) {
        registered += register_large_tier_fonts();
    }
    if (tier >= to_int(UiBreakpoint::XLarge) && prev < to_int(UiBreakpoint::XLarge)) {
        registered += register_xlarge_tier_fonts();
    }
    if (tier >= to_int(UiBreakpoint::XXLarge) && prev < to_int(UiBreakpoint::XXLarge)) {
        registered += register_xxlarge_tier_fonts();
    }

    s_registered_font_tier = tier;
    s_fonts_registered = true;

    if (prev < 0) {
        spdlog::info("[AssetManager] Fonts registered for tier {} ({} faces; larger tiers "
                     "deferred until the breakpoint needs them)",
                     tier, registered);
    } else {
        spdlog::info("[AssetManager] Breakpoint rose {}→{}: registered {} additional font faces",
                     prev, tier, registered);
    }
    return registered;
}

int AssetManager::registered_font_tier() {
    return s_registered_font_tier;
}

void AssetManager::reset_for_test() {
    s_fonts_registered = false;
    s_registered_font_tier = -1;
}

void AssetManager::register_fonts() {
    // Determine breakpoint from the constrained axis (min of width/height), so a
    // portrait screen skips the large-font tiers its narrow axis never uses
    // instead of loading them off its tall axis. Landscape is unchanged (min is
    // already the height). Matches responsive_dimension() in theme_manager.
    // Fonts only used at larger breakpoints are skipped to save memory
    // (~500-800KB of .rodata pages that won't be faulted in) — but the skip is
    // now a deferral, not a latch: theme_manager_refresh_layout_constants()
    // calls back here when a runtime resize raises the breakpoint (#1210).
    const int32_t resp_res = responsive_dimension(nullptr);
    register_fonts_for_tier(to_int(breakpoint_for(resp_res)));
}

void AssetManager::register_images() {
    if (s_images_registered) {
        spdlog::debug("[AssetManager] Images already registered, skipping");
        return;
    }

    spdlog::trace("[AssetManager] Registering images...");

    // reg_img routes the bundle-relative source through asset_component_uri so
    // the registered src resolves against the mount on firmware
    // (/assets/assets/images/...) and stays byte-identical on desktop
    // (asset_root "."). lv_xml_register_image lv_strdup's the src, so passing a
    // temporary c_str() is safe. The registration NAME is left unchanged — the
    // "path-as-name" entries keep their "A:assets/images/..." literal so existing
    // XML / lv_image_set_src references by that name still resolve.
    auto reg_img = [](const char* name, const char* rel) {
        lv_xml_register_image(nullptr, name, helix::asset_component_uri(rel).c_str());
    };

    // Branding
    reg_img("A:assets/images/helixscreen-logo.png", "assets/images/helixscreen-logo.png");
    reg_img("A:assets/images/about-logo.bin", "assets/images/about-logo.bin");

    // Printer and UI images
    reg_img("A:assets/images/printer_400.png", "assets/images/printer_400.png");
    reg_img("filament_spool", "assets/images/filament_spool.png");
    // Tintable Spoolman mark (AMS editor identity chip)
    reg_img("spoolman_mark", "assets/images/ams/spoolman_24.png");
    reg_img("A:assets/images/placeholder_thumb_centered.png",
            "assets/images/placeholder_thumb_centered.png");
    reg_img("A:assets/images/thumbnail-gradient-bg.png", "assets/images/thumbnail-gradient-bg.png");
    reg_img("A:assets/images/benchy_thumbnail_white.png",
            "assets/images/benchy_thumbnail_white.png");
    reg_img("A:assets/images/prerendered/benchy_thumbnail_white.bin",
            "assets/images/prerendered/benchy_thumbnail_white.bin");

    // Flag icons (language chooser wizard) - pre-rendered ARGB8888 32x24
    reg_img("flag_en", "assets/images/flags/flag_en.bin");
    reg_img("flag_de", "assets/images/flags/flag_de.bin");
    reg_img("flag_fr", "assets/images/flags/flag_fr.bin");
    reg_img("flag_es", "assets/images/flags/flag_es.bin");
    reg_img("flag_ru", "assets/images/flags/flag_ru.bin");
    reg_img("flag_pt", "assets/images/flags/flag_pt.bin");
    reg_img("flag_it", "assets/images/flags/flag_it.bin");
    reg_img("flag_zh", "assets/images/flags/flag_zh.bin");
    reg_img("flag_ja", "assets/images/flags/flag_ja.bin");

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
