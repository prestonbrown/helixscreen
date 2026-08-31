// SPDX-License-Identifier: GPL-3.0-or-later

// TEST_MIRROR_OK: an XML-fixture test - the shipped code under test is the
// fans XML itself plus the production <icon> widget it instantiates, driven
// through the fixture's component registration; there is no production
// symbol to include.

// The fans overlay and its row render their fan marks through the <icon>
// widget (an MDI font glyph). "fan" is not a raster image, so an <lv_image
// src="fan"> spelling resolves against the image map, misses, and leaves a
// blank slot behind a warning - the shape seen in bundle CSLYH92R.

#include "../test_fixtures.h"
#include "../ui_test_utils.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

int g_image_miss_lines = 0;

void image_miss_log_cb(lv_log_level_t level, const char* buf) {
    if (level == LV_LOG_LEVEL_WARN &&
        std::string(buf).find("No image was found with name") != std::string::npos) {
        ++g_image_miss_lines;
    }
}

// Counts WARN lines about missing images while alive. No other test installs
// a print callback, so restoring nullptr (LVGL's default path) is faithful.
class ScopedImageMissCounter {
  public:
    ScopedImageMissCounter() {
        g_image_miss_lines = 0;
        lv_log_register_print_cb(image_miss_log_cb);
    }
    ~ScopedImageMissCounter() {
        lv_log_register_print_cb(nullptr);
    }
    ScopedImageMissCounter(const ScopedImageMissCounter&) = delete;
    ScopedImageMissCounter& operator=(const ScopedImageMissCounter&) = delete;

    static int count() {
        return g_image_miss_lines;
    }
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "fan settings XML renders its fan marks through the icon font",
                 "[fans][xml][.xml_required]") {
    ScopedImageMissCounter counter;

    // The row: one fan mark per discovered fan, created repeatedly at runtime.
    REQUIRE(register_component("fan_settings_row"));
    lv_obj_t* row = create_component("fan_settings_row");
    REQUIRE(row != nullptr);
    REQUIRE(lv_obj_find_by_name(row, "fan_icon") != nullptr);

    // The overlay: section headers plus the no-fans empty state.
    REQUIRE(register_component("fan_settings_overlay"));
    lv_obj_t* overlay = create_component("fan_settings_overlay");
    REQUIRE(overlay != nullptr);

    REQUIRE(ScopedImageMissCounter::count() == 0);
}
