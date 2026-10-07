// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "platform_capabilities.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

using helix::PlatformTier;

namespace {
struct PressedProps {
    int32_t scale;
    int32_t outline_width;
    int32_t outline_pad;
};

// Configures the theme's Pressed role for `t`, as theme init would on that
// tier, and reads what a pressed ui_card gets from it.
PressedProps card_pressed_props(lv_obj_t* screen, PlatformTier t) {
    auto& tm = ThemeManager::instance();
    lv_style_t* pressed = tm.get_style(StyleRole::CardPressed);
    lv_style_reset(pressed);
    helix::configure_pressed_for_tier(pressed, t, &tm.current_palette());
    lv_obj_report_style_change(pressed);

    auto* card = static_cast<lv_obj_t*>(lv_xml_create(screen, "ui_card", nullptr));
    REQUIRE(card != nullptr);
    CHECK(lv_obj_get_style_outline_width(card, LV_PART_MAIN) == 0);
    lv_obj_add_state(card, LV_STATE_PRESSED);
    const PressedProps props{lv_obj_get_style_transform_scale_x(card, LV_PART_MAIN),
                             lv_obj_get_style_outline_width(card, LV_PART_MAIN),
                             lv_obj_get_style_outline_pad(card, LV_PART_MAIN)};
    lv_obj_delete(card);
    return props;
}

// The theme's Pressed style, as every lv_button gets it.
bool button_has_prop(PlatformTier t, lv_style_prop_t prop) {
    lv_style_t style;
    lv_style_init(&style);
    helix::configure_pressed_for_tier(&style, t);
    lv_style_value_t v;
    const bool found = lv_style_get_prop(&style, prop, &v) == LV_STYLE_RES_FOUND;
    lv_style_reset(&style);
    return found;
}
} // namespace

// A scaled card renders through a TRANSFORM layer, so the limited tiers press
// with an outline ring instead; the capable tier keeps the scale-down.
TEST_CASE_METHOD(LVGLUITestFixture, "ui_card: pressed state follows the theme's tier rule",
                 "[ui_card][platform_tier]") {
    const PressedProps standard = card_pressed_props(test_screen(), PlatformTier::STANDARD);
    CHECK(standard.scale < LV_SCALE_NONE);
    CHECK(standard.outline_width == 0);

    for (PlatformTier t : {PlatformTier::BASIC, PlatformTier::EMBEDDED}) {
        const PressedProps limited = card_pressed_props(test_screen(), t);
        CHECK(limited.scale == LV_SCALE_NONE);
        CHECK(limited.outline_width > 0);
        CHECK(limited.outline_pad >= 0); // children paint over an inset ring
    }

    auto& tm = ThemeManager::instance();
    lv_style_t* pressed = tm.get_style(StyleRole::CardPressed);
    lv_style_reset(pressed);
    helix::configure_pressed_for_tier(pressed, helix::PlatformCapabilities::detect().tier,
                                      &tm.current_palette());
    lv_obj_report_style_change(pressed);
}

TEST_CASE("full_style_effects_allowed: capable tier only", "[ui_card][platform_tier]") {
    CHECK(helix::full_style_effects_allowed(PlatformTier::STANDARD));
    CHECK_FALSE(helix::full_style_effects_allowed(PlatformTier::BASIC));
    CHECK_FALSE(helix::full_style_effects_allowed(PlatformTier::EMBEDDED));
}

// Buttons keep the LVGL theme's pressed recolor on every tier, so the limited
// tiers add nothing: a ring would box the flush navbar and icon buttons.
TEST_CASE("button Pressed style: scale on the capable tier, nothing extra on the rest",
          "[ui_card][platform_tier]") {
    CHECK(button_has_prop(PlatformTier::STANDARD, LV_STYLE_TRANSFORM_SCALE_X));
    CHECK_FALSE(button_has_prop(PlatformTier::STANDARD, LV_STYLE_OUTLINE_WIDTH));
    for (PlatformTier t : {PlatformTier::BASIC, PlatformTier::EMBEDDED}) {
        CHECK_FALSE(button_has_prop(t, LV_STYLE_TRANSFORM_SCALE_X));
        CHECK_FALSE(button_has_prop(t, LV_STYLE_OUTLINE_WIDTH));
    }
}
