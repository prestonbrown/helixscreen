// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_font_tier_refresh.cpp
 * @brief A runtime breakpoint change must move the type, not just the boxes (#1210)
 *
 * Two mechanisms both stopped short of a runtime resize:
 *
 *  1. theme_manager_refresh_layout_constants() updated the `px` tokens and the
 *     ui_breakpoint subject but never re-ran the responsive *font* registration,
 *     so layout was sized for the new breakpoint while type stayed on the old one.
 *  2. AssetManager::register_fonts() latched on a bool and chose which font tiers
 *     exist in memory at all from the *startup* breakpoint. A breakpoint that rose
 *     at runtime therefore had no larger faces to point at — which is why (1)
 *     could not be fixed on its own without every raised token silently falling
 *     back to the _large tier.
 *
 * ui_switch_init_size_presets() had the same once-only shape and is covered here
 * through its observable effect: the size of a themed switch.
 */

#include "ui_breakpoint.h"
#include "ui_button.h"
#include "ui_icon.h"
#include "ui_switch.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "asset_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Read a token out of the "globals" XML scope. Fails the test if absent —
/// a missing font token is itself the bug this file guards.
std::string token(const char* name) {
    const char* v = lv_xml_get_const(nullptr, name);
    REQUIRE(v != nullptr);
    return std::string(v);
}

} // namespace

// ============================================================================
// Defect 2 — AssetManager must be re-entrant and tier-aware
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "A rising breakpoint registers the font tiers startup skipped",
                 "[application][assets][fonts][theme][1210]") {
    // Start from a small screen, as an AD5M or a 480x320 panel would.
    AssetManager::reset_for_test();
    const int base = AssetManager::register_fonts_for_tier(to_int(UiBreakpoint::Small));
    CHECK(base > 0);
    CHECK(AssetManager::fonts_registered());
    CHECK(AssetManager::registered_font_tier() == to_int(UiBreakpoint::Small));

    // Now the window is dragged up past every boundary at once.
    const int rise = AssetManager::register_fonts_for_tier(to_int(UiBreakpoint::XXLarge));
    CHECK(AssetManager::registered_font_tier() == to_int(UiBreakpoint::XXLarge));

    // Only the *additional* tiers, not the whole table again. The four gates
    // above SMALL contribute 3 (medium) + 4 (large) + 5 (xlarge) + 7 (xxlarge)
    // faces; update this number when a tier gains or loses a face.
#if HELIX_MAX_FONT_TIER >= 6
    CHECK(rise == 19);
#else
    CHECK(rise > 0);
#endif

    // One representative face per gate must now resolve.
    CHECK(lv_xml_get_font_silent(nullptr, "noto_sans_26") != nullptr); // medium gate
    CHECK(lv_xml_get_font_silent(nullptr, "noto_sans_28") != nullptr); // large gate
#if HELIX_MAX_FONT_TIER >= 5
    CHECK(lv_xml_get_font_silent(nullptr, "noto_sans_32") != nullptr); // xlarge gate
#endif
#if HELIX_MAX_FONT_TIER >= 6
    CHECK(lv_xml_get_font_silent(nullptr, "noto_sans_40") != nullptr); // xxlarge gate
#endif
}

TEST_CASE_METHOD(XMLTestFixture, "Re-registering at the same or a lower tier is a no-op",
                 "[application][assets][fonts][theme][1210]") {
    AssetManager::reset_for_test();
    CHECK(AssetManager::register_fonts_for_tier(to_int(UiBreakpoint::Large)) > 0);

    // Same tier again: nothing more to do.
    CHECK(AssetManager::register_fonts_for_tier(to_int(UiBreakpoint::Large)) == 0);

    // A shrink never unregisters and never re-registers — the faces are static
    // .rodata and live widgets hold pointers into them.
    CHECK(AssetManager::register_fonts_for_tier(to_int(UiBreakpoint::Micro)) == 0);
    CHECK(AssetManager::register_fonts_for_tier(to_int(UiBreakpoint::Small)) == 0);
    CHECK(AssetManager::registered_font_tier() == to_int(UiBreakpoint::Large));

    // The two micro-only 8px faces come with the first call whatever the tier,
    // so a downward move to MICRO still finds its tokens resolvable.
    CHECK(lv_xml_get_font_silent(nullptr, "noto_sans_8") != nullptr);
}

// ============================================================================
// Defect 1 — the refresh path must move the font tokens
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "Refresh moves the font tokens, not just the px tokens",
                 "[theme][fonts][1210]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    // Values come from ui_xml/globals.xml: font_body_micro / font_heading_micro.
    {
        ScopedResolution micro(disp, 480, 272);
        theme_manager_refresh_layout_constants(disp);
        CHECK(token("font_body") == "noto_sans_12");
        CHECK(token("font_heading") == "noto_sans_14");
    }

    // Min axis 800 → XLARGE. font_heading_xlarge is noto_sans_32, a face the
    // startup registration skips on an 800x480 panel — so this only passes if
    // the refresh registered the tier before re-pointing the token at it.
    {
        ScopedResolution xlarge(disp, 800, 1024);
        theme_manager_refresh_layout_constants(disp);
        CHECK(token("font_body") == "noto_sans_24");
        CHECK(token("font_heading") == "noto_sans_32");
        CHECK(lv_xml_get_font_silent(nullptr, "noto_sans_32") != nullptr);
    }

    // Put the token table back where the rest of the suite expects it.
    theme_manager_refresh_layout_constants(disp);
    CHECK(token("font_body") == "noto_sans_18");
}

// ============================================================================
// Same shape again — the <icon> widget memoizes icon_font_* per size
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Icon faces follow a runtime breakpoint change",
                 "[ui_icon][theme][fonts][1210]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    const char* attrs[] = {"src", "home", "size", "sm", nullptr};
    const char* btn_attrs[] = {"text", "Home", "icon", "home", nullptr};

    /// The face a <ui_button icon="..."> put on its icon child, which resolves
    /// icon_font_sm through a second, independent process-lifetime cache.
    auto button_icon_face = [&](lv_obj_t* parent) {
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(parent, "ui_button", btn_attrs));
        REQUIRE(btn != nullptr);
        lv_obj_t* icon = ui_button_get_icon(btn);
        REQUIRE(icon != nullptr);
        const lv_font_t* f = lv_obj_get_style_text_font(icon, LV_PART_MAIN);
        lv_obj_delete(btn);
        return f;
    };

    // ui_icon caches the resolved face per size for the life of the process, so
    // whichever test in this binary built the first size="sm" icon decided this
    // one too until the refresh started invalidating the cache. That made the
    // widget content-fit sweep pass alone and fail in a shard: it measures every
    // home widget at eight resolutions in one process, and the icons kept
    // whatever face the first geometry to run happened to pin.
    const lv_font_t* micro_face = nullptr;
    {
        ScopedResolution micro(disp, 480, 272);
        theme_manager_refresh_layout_constants(disp);
        CHECK(token("icon_font_sm") == "mdi_icons_16");

        lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
        REQUIRE(icon != nullptr);
        micro_face = lv_obj_get_style_text_font(icon, LV_PART_MAIN);
        REQUIRE(micro_face != nullptr);
        CHECK(micro_face == lv_xml_get_font(nullptr, "mdi_icons_16"));
        lv_obj_delete(icon);

        CHECK(button_icon_face(test_screen()) == lv_xml_get_font(nullptr, "mdi_icons_16"));
    }

    {
        ScopedResolution xlarge(disp, 800, 1024);
        theme_manager_refresh_layout_constants(disp);
        CHECK(token("icon_font_sm") == "mdi_icons_32");

        lv_obj_t* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
        REQUIRE(icon != nullptr);
        const lv_font_t* xlarge_face = lv_obj_get_style_text_font(icon, LV_PART_MAIN);
        REQUIRE(xlarge_face != nullptr);

        // The token moved, so the glyphs must have moved with it: an icon still
        // drawn in the 16px face is 8px narrower than the box built around it.
        CHECK(xlarge_face != micro_face);
        CHECK(xlarge_face == lv_xml_get_font(nullptr, "mdi_icons_32"));
        CHECK(lv_font_get_line_height(xlarge_face) > lv_font_get_line_height(micro_face));
        lv_obj_delete(icon);

        CHECK(button_icon_face(test_screen()) == lv_xml_get_font(nullptr, "mdi_icons_32"));
    }

    // Put the token table back where the rest of the suite expects it.
    theme_manager_refresh_layout_constants(disp);
}

// ============================================================================
// Notes section of #1210 — ui_switch_init_size_presets() had the same shape
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "Switch size presets follow a runtime breakpoint change",
                 "[ui_switch][theme][fonts][1210]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    {
        ScopedResolution micro(disp, 480, 272);
        theme_manager_refresh_layout_constants(disp);
        lv_obj_t* sw = ui_switch_create_themed(test_screen(), "medium");
        REQUIRE(sw != nullptr);
        CHECK(lv_obj_get_style_width(sw, LV_PART_MAIN) == 40);
        CHECK(lv_obj_get_style_height(sw, LV_PART_MAIN) == 20);
        lv_obj_delete(sw);
    }

    {
        ScopedResolution medium(disp, 800, 480);
        theme_manager_refresh_layout_constants(disp);
        lv_obj_t* sw = ui_switch_create_themed(test_screen(), "medium");
        REQUIRE(sw != nullptr);
        CHECK(lv_obj_get_style_width(sw, LV_PART_MAIN) == 80);
        CHECK(lv_obj_get_style_height(sw, LV_PART_MAIN) == 40);
        lv_obj_delete(sw);
    }
}
