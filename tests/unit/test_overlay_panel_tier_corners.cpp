// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "platform_capabilities.h"

#include "../catch_amalgamated.hpp"

using helix::PlatformTier;

// Limited tiers trade the panel's rounded corners for a panel LVGL can treat as
// covering what is behind it; capable tiers keep the theme radius.
TEST_CASE_METHOD(LVGLUITestFixture, "overlay_panel: square corners on the limited tiers only",
                 "[overlay][platform_tier]") {
    lv_subject_t* tier = lv_xml_get_subject(nullptr, "platform_tier");
    REQUIRE(tier != nullptr);
    const int saved = lv_subject_get_int(tier);

    auto* panel = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "overlay_panel", nullptr));
    REQUIRE(panel != nullptr);

    lv_subject_set_int(tier, static_cast<int>(PlatformTier::STANDARD));
    const int32_t rounded = lv_obj_get_style_radius(panel, LV_PART_MAIN);
    CHECK(rounded > 0);

    lv_subject_set_int(tier, static_cast<int>(PlatformTier::BASIC));
    CHECK(lv_obj_get_style_radius(panel, LV_PART_MAIN) == 0);

    lv_subject_set_int(tier, static_cast<int>(PlatformTier::EMBEDDED));
    CHECK(lv_obj_get_style_radius(panel, LV_PART_MAIN) == 0);

    lv_subject_set_int(tier, static_cast<int>(PlatformTier::STANDARD));
    CHECK(lv_obj_get_style_radius(panel, LV_PART_MAIN) == rounded);

    lv_obj_delete(panel);
    lv_subject_set_int(tier, saved);
    helix::ui::UpdateQueue::instance().drain();
}
