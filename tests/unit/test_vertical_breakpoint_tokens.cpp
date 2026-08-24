// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_vertical_breakpoint_tokens.cpp
 * @brief Height tokens must resolve from the vertical axis, not the narrow one (#1209)
 *
 * responsive_dimension() returns min(width, height), and that single scalar used
 * to drive every responsive `px` token. Right for anything the narrow axis
 * constrains — fonts, horizontal padding, column counts — and wrong for
 * everything vertical: a 320x1480 panel classifies from 320 (TINY) and inherits
 * 32px buttons despite having 1480px of height to spend.
 *
 * Nine tokens now resolve from a second, vertical ladder. Both ladders share
 * breakpoint_for() and theme_manager_get_breakpoint_suffix() verbatim; only the
 * scalar fed in differs. On landscape and square displays min(w,h) == h, so the
 * two ladders agree by construction and nothing moves — the landscape cases here
 * are the zero-regression pin, and are the most important assertions in the file.
 *
 * The `ui_breakpoint` subject deliberately keeps holding the cramped tier: every
 * bind_flag_if_eq / bind_style_if ref_value in ui_xml/ is written against it.
 */

#include "ui_breakpoint.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <string>
#include <unordered_map>

#include "../catch_amalgamated.hpp"

namespace {

/// Height tokens on the vertical ladder: the seven migrated in #1209 plus the
/// two dialog content-cap siblings (values from ui_xml/globals.xml at the tiers
/// this file exercises). The pinned/tall ladders are 85%-cap minus measured
/// chrome (#1277) — both are vertical maxima and were briefly missing here,
/// which made them resolve from the cramped axis on portrait panels.
struct HeightToken {
    const char* name;
    const char* tiny;    // narrow axis 273-390
    const char* medium;  // 461-550  (800x480)
    const char* large;   // 551-700  (1024x600)
    const char* xxlarge; // > 1000   (the 1480px axis of a 320x1480 panel)
};

constexpr HeightToken HEIGHT_TOKENS[] = {
    {"button_height", "32", "52", "72", "96"},
    {"button_height_sm", "28", "40", "40", "56"},
    {"button_height_lg", "40", "70", "96", "128"},
    {"header_height", "32", "56", "60", "80"},
    {"input_height", "48", "52", "56", "64"},
    {"temp_card_height", "48", "72", "80", "112"},
    {"dialog_content_max", "200", "320", "440", "800"},
    {"dialog_content_pinned_max", "139", "207", "272", "414"},
    {"dialog_content_tall_chrome_max", "146", "229", "282", "545"},
    // spinner_lg has no _micro/_tiny variant, so TINY falls back inward to
    // spinner_lg_small = 48 — which is exactly why the two tight portrait sizes
    // (240x320, 480x272) are unchanged by putting it on this ladder.
    {"spinner_lg", "48", "56", "64", "96"},
};

/// Bufferless display: the axis accessors only read the resolution.
lv_display_t* make_test_display(int32_t w, int32_t h) {
    return lv_display_create(w, h);
}

/// Read a token out of the "globals" XML scope, failing the test if absent.
std::string token(const char* name) {
    const char* v = lv_xml_get_const(nullptr, name);
    REQUIRE(v != nullptr);
    return std::string(v);
}

std::string resolved(const std::unordered_map<std::string, std::string>& m, const char* name) {
    auto it = m.find(name);
    REQUIRE(it != m.end());
    return it->second;
}

} // namespace

// ============================================================================
// Stage 1 — the vertical accessor
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "responsive_vertical_dimension returns the vertical axis",
                 "[theme][breakpoints][1209]") {
    SECTION("landscape — the vertical axis is also the narrow one") {
        lv_display_t* d = make_test_display(800, 480);
        CHECK(responsive_vertical_dimension(d) == 480);
        CHECK(responsive_vertical_dimension(d) == responsive_dimension(d));
        lv_display_delete(d);

        d = make_test_display(1024, 600);
        CHECK(responsive_vertical_dimension(d) == 600);
        CHECK(responsive_vertical_dimension(d) == responsive_dimension(d));
        lv_display_delete(d);
    }

    SECTION("portrait — the two axes diverge, which is the whole point") {
        lv_display_t* d = make_test_display(320, 1480);
        CHECK(responsive_vertical_dimension(d) == 1480);
        CHECK(responsive_dimension(d) == 320);
        CHECK(breakpoint_for(responsive_vertical_dimension(d)) == UiBreakpoint::XXLarge);
        CHECK(breakpoint_for(responsive_dimension(d)) == UiBreakpoint::Tiny);
        lv_display_delete(d);

        d = make_test_display(480, 800);
        CHECK(responsive_vertical_dimension(d) == 800);
        CHECK(responsive_dimension(d) == 480);
        lv_display_delete(d);
    }

    SECTION("square — both axes agree") {
        lv_display_t* d = make_test_display(480, 480);
        CHECK(responsive_vertical_dimension(d) == 480);
        CHECK(responsive_vertical_dimension(d) == responsive_dimension(d));
        lv_display_delete(d);
    }

    SECTION("null display falls back to the default display") {
        CHECK(responsive_vertical_dimension(nullptr) == TEST_DISPLAY_HEIGHT);
    }
}

// ============================================================================
// Stage 2 — which tokens follow which ladder
// ============================================================================

TEST_CASE("Only the declared height tokens follow the vertical ladder",
          "[theme][breakpoints][1209]") {
    for (const auto& t : HEIGHT_TOKENS) {
        INFO("token: " << t.name);
        CHECK(theme_manager_token_uses_vertical_axis(t.name));
    }

    // Spacing stays axis-neutral on purpose: `space_*` feeds pad_top/bottom and
    // pad_left/right alike, so splitting it means classifying every `#space_*`
    // reference site in ui_xml/.
    CHECK_FALSE(theme_manager_token_uses_vertical_axis("space_lg"));
    CHECK_FALSE(theme_manager_token_uses_vertical_axis("space_md"));
    // Widths are horizontal, not vertical.
    CHECK_FALSE(theme_manager_token_uses_vertical_axis("nav_width"));
    CHECK_FALSE(theme_manager_token_uses_vertical_axis("field_w_num"));
    // Prefix matching would be wrong — the list is exact names.
    CHECK_FALSE(theme_manager_token_uses_vertical_axis("button_height_xl"));
    CHECK_FALSE(theme_manager_token_uses_vertical_axis(""));
    CHECK_FALSE(theme_manager_token_uses_vertical_axis(nullptr));
}

TEST_CASE_METHOD(XMLTestFixture, "A tall portrait panel gets full-height controls",
                 "[theme][breakpoints][1209]") {
    // 320x1480 Waveshare 11.9": cramped axis 320 → TINY, vertical axis 1480 → XXLARGE.
    lv_display_t* d = make_test_display(320, 1480);
    const auto tokens = theme_manager_resolve_px_tokens(d);

    for (const auto& t : HEIGHT_TOKENS) {
        INFO("token: " << t.name);
        CHECK(resolved(tokens, t.name) == t.xxlarge);
    }

    // Everything else still resolves from the cramped axis.
    CHECK(resolved(tokens, "space_lg") == "8");     // _tiny
    CHECK(resolved(tokens, "space_md") == "6");     // _tiny
    CHECK(resolved(tokens, "field_w_num") == "76"); // no _tiny → falls back to _small

    lv_display_delete(d);
}

TEST_CASE_METHOD(XMLTestFixture, "Landscape token resolution is byte-identical to before",
                 "[theme][breakpoints][1209]") {
    SECTION("800x480 — MEDIUM on both axes") {
        lv_display_t* d = make_test_display(800, 480);
        const auto tokens = theme_manager_resolve_px_tokens(d);
        for (const auto& t : HEIGHT_TOKENS) {
            INFO("token: " << t.name);
            CHECK(resolved(tokens, t.name) == t.medium);
        }
        CHECK(resolved(tokens, "space_lg") == "16");
        CHECK(resolved(tokens, "field_w_num") == "96");
        lv_display_delete(d);
    }

    SECTION("1024x600 — LARGE on both axes") {
        lv_display_t* d = make_test_display(1024, 600);
        const auto tokens = theme_manager_resolve_px_tokens(d);
        for (const auto& t : HEIGHT_TOKENS) {
            INFO("token: " << t.name);
            CHECK(resolved(tokens, t.name) == t.large);
        }
        CHECK(resolved(tokens, "space_lg") == "20");
        CHECK(resolved(tokens, "field_w_num") == "110");
        lv_display_delete(d);
    }

    SECTION("480x320 — TINY on both axes, the smallest shipping panel") {
        lv_display_t* d = make_test_display(480, 320);
        const auto tokens = theme_manager_resolve_px_tokens(d);
        for (const auto& t : HEIGHT_TOKENS) {
            INFO("token: " << t.name);
            CHECK(resolved(tokens, t.name) == t.tiny);
        }
        CHECK(resolved(tokens, "space_lg") == "8");
        lv_display_delete(d);
    }
}

// ============================================================================
// Both registration sites must agree — a token that gets the vertical tier at
// startup and the cramped tier on resize is worse than not doing this at all.
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "Startup registration matches the shared resolver",
                 "[theme][breakpoints][1209]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    // The consts in the globals scope were written by the startup path
    // (theme_manager_init → theme_manager_register_responsive_spacing) at the
    // fixture's 800x480. They must be exactly what the resolver says.
    const auto tokens = theme_manager_resolve_px_tokens(disp);
    for (const auto& t : HEIGHT_TOKENS) {
        INFO("token: " << t.name);
        CHECK(token(t.name) == resolved(tokens, t.name));
        CHECK(token(t.name) == t.medium);
    }
    CHECK(token("space_lg") == resolved(tokens, "space_lg"));
    CHECK(token("nav_width") == resolved(tokens, "nav_width"));
}

TEST_CASE_METHOD(XMLTestFixture, "The refresh path applies exactly what the resolver returns",
                 "[theme][breakpoints][1209]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    {
        ScopedResolution portrait(disp, 320, 1480);
        theme_manager_refresh_layout_constants(disp);

        const auto tokens = theme_manager_resolve_px_tokens(disp);
        for (const auto& [name, value] : tokens) {
            INFO("token: " << name);
            CHECK(token(name.c_str()) == value);
        }

        // Spelled out, so a resolver that agreed with a broken applier still fails.
        CHECK(token("button_height") == "96");
        CHECK(token("dialog_content_max") == "800");
        CHECK(token("space_lg") == "8");

        // The subject XML binds against keeps the cramped tier.
        lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
        REQUIRE(bp != nullptr);
        CHECK(lv_subject_get_int(bp) == to_int(UiBreakpoint::Tiny));
    }

    // Put the token table back where the rest of the suite expects it.
    theme_manager_refresh_layout_constants(disp);
    CHECK(token("button_height") == "52");
}

TEST_CASE_METHOD(XMLTestFixture, "nav_width keeps its own horizontal ladder across a refresh",
                 "[theme][breakpoints][1209]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    {
        // 2400x480 ultrawide: the nav strip stays slim (_small, 76px) rather
        // than taking the 104px the narrow axis (480 → MEDIUM) would pick.
        ScopedResolution ultrawide(disp, 2400, 480);
        theme_manager_refresh_layout_constants(disp);
        CHECK(token("nav_width") == "76");
    }

    theme_manager_refresh_layout_constants(disp);
    CHECK(token("nav_width") == "76"); // 800x480 → hor ≤ 900 → _small
}
