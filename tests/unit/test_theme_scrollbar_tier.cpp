// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "platform_capabilities.h"

#include "../catch_amalgamated.hpp"

using helix::PlatformTier;

namespace {
/// Whether a new container's scrollbar looks different while it scrolls.
bool scrollbar_restyles_while_scrolling(lv_obj_t* screen, lv_subject_t* tier, PlatformTier t) {
    lv_subject_set_int(tier, static_cast<int>(t));
    lv_obj_t* box = lv_obj_create(screen);
    const lv_opa_t resting = lv_obj_get_style_bg_opa(box, LV_PART_SCROLLBAR);
    lv_obj_add_state(box, LV_STATE_SCROLLED);
    const lv_opa_t scrolling = lv_obj_get_style_bg_opa(box, LV_PART_SCROLLBAR);
    lv_obj_delete(box);
    return resting != scrolling;
}
} // namespace

// Restyling the scrollbar redraws the whole scroller when a drag starts and
// stops, so the limited tiers keep the resting scrollbar.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "theme: scrollbar restyles while scrolling on the capable tier only",
                 "[theme][platform_tier]") {
    lv_subject_t* tier = lv_xml_get_subject(nullptr, "platform_tier");
    REQUIRE(tier != nullptr);
    const int saved = lv_subject_get_int(tier);

    CHECK(scrollbar_restyles_while_scrolling(test_screen(), tier, PlatformTier::STANDARD));
    CHECK_FALSE(scrollbar_restyles_while_scrolling(test_screen(), tier, PlatformTier::BASIC));
    CHECK_FALSE(scrollbar_restyles_while_scrolling(test_screen(), tier, PlatformTier::EMBEDDED));

    lv_subject_set_int(tier, saved);
    helix::ui::UpdateQueue::instance().drain();
}
