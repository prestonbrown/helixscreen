// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The nav bar's inactive icon must never render at or above the active icon's
// size. Both sides are responsive ladders in ui_xml/navigation_bar.xml and
// ui_xml/globals.xml, so the relationship holds only by agreement between two
// separately edited tables. Retuning one without the other reads as correct in
// isolation and produces a nav bar with no size differentiation at all.

#include "../../include/ui_nav_manager.h"
#include "../lvgl_ui_test_fixture.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <string>
#include <unordered_map>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

const char* SUFFIXES[] = {"_micro", "_tiny", "_small", "_medium", "_large", "_xlarge", "_xxlarge"};

// Ordering of the icon size rungs. A higher index is a larger glyph.
int rung_index(const std::string& rung) {
    if (rung == "xs")
        return 0;
    if (rung == "sm")
        return 1;
    if (rung == "md")
        return 2;
    if (rung == "lg")
        return 3;
    if (rung == "xl")
        return 4;
    return -1;
}

std::string lookup(const std::unordered_map<std::string, std::string>& m, const char* key) {
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

} // namespace

TEST_CASE("nav inactive icon size stays below the active icon size", "[theme][navbar]") {
    for (const char* suffix : SUFFIXES) {
        auto strings = theme_manager_parse_all_xml_for_suffix("ui_xml", "string", suffix);

        const std::string active = lookup(strings, "icon_size");
        const std::string inactive = lookup(strings, "icon_size_nav_inactive");

        INFO("suffix=" << suffix << " icon_size=" << active
                       << " icon_size_nav_inactive=" << inactive);

        REQUIRE_FALSE(active.empty());
        REQUIRE_FALSE(inactive.empty());

        const int active_rung = rung_index(active);
        const int inactive_rung = rung_index(inactive);
        REQUIRE(active_rung >= 0);
        REQUIRE(inactive_rung >= 0);

        if (std::string(suffix) == "_micro") {
            // micro is the deliberate exception. One rung below md is sm, which
            // resolves to a 16px face, and a 16px outline glyph has strokes too
            // thin to read inside a 42px nav strip. It matches instead, and the
            // glyph and color carry the differentiation there.
            REQUIRE(inactive_rung == active_rung);
        } else {
            REQUIRE(inactive_rung < active_rung);
        }
    }
}

namespace {

/// Instantiates the real navigation_bar component so the crossfade case below
/// can read actual computed style properties, not just the XML tokens above.
class NavIconCrossfadeFixture : public LVGLUITestFixture {
  public:
    NavIconCrossfadeFixture() {
        navbar_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "navigation_bar", nullptr));
    }

    ~NavIconCrossfadeFixture() override {
        if (navbar_) {
            lv_obj_delete(navbar_);
            navbar_ = nullptr;
        }
    }

    lv_obj_t* icon(const char* name) {
        return lv_obj_find_by_name(navbar_, name);
    }

    lv_obj_t* navbar_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(NavIconCrossfadeFixture,
                 "nav icon pairs crossfade via checked state instead of hidden-flag swap",
                 "[theme][navbar]") {
    REQUIRE(navbar_ != nullptr);

    NavigationManager::instance().set_active(PanelId::Home);

    lv_obj_t* active = icon("nav_icon_home_active");
    lv_obj_t* inactive = icon("nav_icon_home_inactive");
    REQUIRE(active != nullptr);
    REQUIRE(inactive != nullptr);

    // A regression back to hidden-flag exclusivity removes one of the pair
    // from the tree instead of merely changing its opacity.
    REQUIRE_FALSE(lv_obj_has_flag(active, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(inactive, LV_OBJ_FLAG_HIDDEN));

    REQUIRE(lv_obj_has_state(active, LV_STATE_CHECKED));
    REQUIRE_FALSE(lv_obj_has_state(inactive, LV_STATE_CHECKED));

    const lv_opa_t active_opa = lv_obj_get_style_text_opa(active, LV_PART_MAIN);
    const lv_opa_t inactive_opa = lv_obj_get_style_text_opa(inactive, LV_PART_MAIN);
    INFO("active_opa=" << (int)active_opa << " inactive_opa=" << (int)inactive_opa);
    REQUIRE(active_opa > inactive_opa);

    // Switching panels flips which icon is checked, and its opacity with it.
    NavigationManager::instance().set_active(PanelId::Controls);
    REQUIRE_FALSE(lv_obj_has_state(active, LV_STATE_CHECKED));
    REQUIRE(lv_obj_get_style_text_opa(active, LV_PART_MAIN) <
            lv_obj_get_style_text_opa(inactive, LV_PART_MAIN));
}
