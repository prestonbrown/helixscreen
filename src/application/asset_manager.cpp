// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "asset_manager.h"

#include "ui_fonts.h"

#include "core/data_root_resolver.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <lvgl.h>
#include <string>
#include <vector>

// Mirrors the same guard in cjk_font_manager.cpp and theme_manager.cpp. Without it, a build
// path that forgot -DHELIX_MAX_FONT_TIER would evaluate every guard below as
// `0 >= N` and silently register almost no fonts -- a blank UI rather than a
// build error. mk/cross.mk always passes it; this is the backstop for the day
// something doesn't.
#ifndef HELIX_MAX_FONT_TIER
#define HELIX_MAX_FONT_TIER 6 // default: all tiers (micro=0 .. xxlarge=6)
#endif

bool AssetManager::s_fonts_registered = false;
bool AssetManager::s_images_registered = false;
int AssetManager::s_registered_font_tier = -1;

namespace {

/// One text face name and the first breakpoint that registers it. Registering
/// only the faces a breakpoint references keeps larger tiers' .rodata pages
/// unfaulted; text_font_names_for_breakpoint() reads the same rows, and the CJK
/// filename bake and prune read its output (scripts/translations/registered_text_fonts.txt).
struct TextFontRow {
    const char* name;
    const lv_font_t* font;
    int first_tier;
};

// clang-format off
const TextFontRow kTextFonts[] = {
    // Micro-only 8px faces, registered whatever the startup tier: a name entry
    // costs ~100 bytes and faults no glyph pages, and the breakpoint can fall
    // at runtime to MICRO, where font_body_micro needs them (#1210).
    {"noto_sans_8", &noto_sans_8, 0},
    {"source_code_pro_8", &source_code_pro_8, 0},
    // "montserrat_*" names for XML compatibility, all backed by Noto Sans.
    {"montserrat_10", &noto_sans_10, 0},
    {"montserrat_12", &noto_sans_12, 0},
    {"montserrat_14", &noto_sans_14, 0},
    {"montserrat_16", &noto_sans_16, 0},
    {"montserrat_18", &noto_sans_18, 0},
    {"montserrat_20", &noto_sans_20, 0},
    {"montserrat_24", &noto_sans_24, 0},
    {"noto_sans_10", &noto_sans_10, 0},
    {"noto_sans_11", &noto_sans_11, 0},
    {"noto_sans_12", &noto_sans_12, 0},
    {"noto_sans_14", &noto_sans_14, 0},
    {"noto_sans_16", &noto_sans_16, 0},
    {"noto_sans_18", &noto_sans_18, 0},
    {"noto_sans_20", &noto_sans_20, 0},
    {"noto_sans_24", &noto_sans_24, 0},
    {"noto_sans_light_10", &noto_sans_light_10, 0},
    {"noto_sans_light_11", &noto_sans_light_11, 0},
    {"noto_sans_light_12", &noto_sans_light_12, 0},
    // Bold faces at every tier: C++ uses them directly (watchdog bold_16/24)
    // and so does XML (debug modal bold_28).
    {"noto_sans_bold_14", &noto_sans_bold_14, 0},
    {"noto_sans_bold_16", &noto_sans_bold_16, 0},
    {"noto_sans_bold_18", &noto_sans_bold_18, 0},
    {"noto_sans_bold_20", &noto_sans_bold_20, 0},
    {"noto_sans_bold_24", &noto_sans_bold_24, 0},
    {"noto_sans_bold_28", &noto_sans_bold_28, 0},
    {"source_code_pro_10", &source_code_pro_10, 0},
    {"source_code_pro_12", &source_code_pro_12, 0},
    {"source_code_pro_14", &source_code_pro_14, 0},
    {"source_code_pro_16", &source_code_pro_16, 0},
#if HELIX_MAX_FONT_TIER >= 3
    {"montserrat_26", &noto_sans_26, 3},             // font_heading_medium
    {"noto_sans_26", &noto_sans_26, 3},              // font_heading_medium
    {"noto_sans_light_16", &noto_sans_light_16, 3},  // font_small_medium
    // font_display_medium: the largest bold face, from MEDIUM up because that is
    // the first tier whose tokens name it.
    {"noto_sans_bold_40", &noto_sans_bold_40, 3},
#endif
#if HELIX_MAX_FONT_TIER >= 4
    {"montserrat_28", &noto_sans_28, 4},             // font_heading_large
    {"noto_sans_28", &noto_sans_28, 4},              // font_heading_large
    {"noto_sans_light_14", &noto_sans_light_14, 4},  // font_xs_large
    {"noto_sans_light_18", &noto_sans_light_18, 4},  // font_small_large
#endif
#if HELIX_MAX_FONT_TIER >= 5
    {"noto_sans_32", &noto_sans_32, 5},
    {"noto_sans_bold_32", &noto_sans_bold_32, 5},
    {"noto_sans_light_20", &noto_sans_light_20, 5},
    {"source_code_pro_18", &source_code_pro_18, 5},
#endif
#if HELIX_MAX_FONT_TIER >= 6
    {"noto_sans_40", &noto_sans_40, 6},
    {"noto_sans_light_26", &noto_sans_light_26, 6},
    {"source_code_pro_20", &source_code_pro_20, 6},
    {"source_code_pro_24", &source_code_pro_24, 6},
#if HELIX_HAS_HIDPI_FONTS
    // Rungs above the authored ladder, reachable only through the high-DPI UI
    // scale (DisplayMetrics). theme_manager adopts a scaled face only after
    // lv_xml_get_font_silent() finds it, so registering them is what makes the
    // scaled selection possible. A platform that does not link them declares
    // HELIX_HAS_HIDPI_FONTS=0; its panels have no scale above 1.0 to reach them.
    {"noto_sans_48", &noto_sans_48, 6},
    {"noto_sans_64", &noto_sans_64, 6},
    {"noto_sans_bold_48", &noto_sans_bold_48, 6},
    {"noto_sans_bold_64", &noto_sans_bold_64, 6},
    {"noto_sans_light_32", &noto_sans_light_32, 6},
    {"noto_sans_light_40", &noto_sans_light_40, 6},
#endif
#endif
};
// clang-format on

/// Register the text faces whose first breakpoint is @p first_tier.
int register_text_fonts(int first_tier) {
    int n = 0;
    for (const auto& row : kTextFonts) {
        if (row.first_tier == first_tier) {
            lv_xml_register_font(nullptr, row.name, row.font);
            n++;
        }
    }
    return n;
}

/// Faces every breakpoint needs. Registered once, on the first call.
int register_base_fonts() {
    // Material Design Icons, needed at all breakpoints (watchdog, XML, ...).
    // Source: https://pictogrammers.com/library/mdi/
    lv_xml_register_font(nullptr, "mdi_icons_64", &mdi_icons_64);
    lv_xml_register_font(nullptr, "mdi_icons_48", &mdi_icons_48);
    lv_xml_register_font(nullptr, "mdi_icons_32", &mdi_icons_32);
    lv_xml_register_font(nullptr, "mdi_icons_24", &mdi_icons_24);
    lv_xml_register_font(nullptr, "mdi_icons_16", &mdi_icons_16);
    lv_xml_register_font(nullptr, "mdi_icons_14", &mdi_icons_14);
    return 6 + register_text_fonts(0);
}

/// Icon faces above 64px, each registered wherever it is linked: with its tier,
/// or on a platform that links it below that tier (mk/fonts.mk EXTRA_ICON_FONTS).
void register_large_icon_fonts() {
#if HELIX_HAS_MDI_ICONS_80
    lv_xml_register_font(nullptr, "mdi_icons_80", &mdi_icons_80);
#endif
#if HELIX_HAS_MDI_ICONS_96
    lv_xml_register_font(nullptr, "mdi_icons_96", &mdi_icons_96);
#endif
#if HELIX_HAS_MDI_ICONS_128
    lv_xml_register_font(nullptr, "mdi_icons_128", &mdi_icons_128);
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
        // At every tier: a home tile grown past two cells asks for a face above
        // its tier's ladder (ui_tile_rung.h).
        register_large_icon_fonts();
    }
    for (int t = std::max<int>(prev + 1, to_int(UiBreakpoint::Tiny)); t <= tier; ++t) {
        registered += register_text_fonts(t);
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

std::vector<std::string> AssetManager::text_font_names_for_breakpoint(int tier) {
    std::vector<std::string> names;
    for (const auto& row : kTextFonts) {
        if (row.first_tier <= tier)
            names.emplace_back(row.name);
    }
    return names;
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
    // a deferral, not a latch: theme_manager_refresh_layout_constants()
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
