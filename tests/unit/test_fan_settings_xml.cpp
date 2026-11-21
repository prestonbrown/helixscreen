// SPDX-License-Identifier: GPL-3.0-or-later

// The fans overlay and its row render their fan marks through the <icon>
// widget (an MDI font glyph). "fan" is not a raster image, so an <lv_image
// src="fan"> spelling resolves against the image map, misses, and leaves a
// blank slot behind a warning - the shape seen in a field report.

#include "ui_nav_manager.h"
#include "ui_settings_fans.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "config.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <array>
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

TEST_CASE_METHOD(LVGLUITestFixture, "fan settings rows show each fan's speed", "[fans][settings]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(lv_screen_active());
    }
    NavigationManager::instance().set_panels(panels.data());

    auto& fans = get_printer_state().fan_state();
    fans.init_fans({"fan"});
    fans.update_fan_speed("fan", 0.5);

    auto& overlay = helix::settings::get_fan_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    lv_obj_t* list = lv_obj_find_by_name(lv_screen_active(), "controllable_fans_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 1);
    lv_obj_t* speed = lv_obj_find_by_name(lv_obj_get_child(list, 0), "speed_label");
    REQUIRE(speed != nullptr);
    CHECK(std::string(lv_label_get_text(speed)) == "50%");
    CHECK(lv_obj_find_by_name(lv_obj_get_child(list, 0), "fan_icon") != nullptr);
    CHECK(ScopedImageMissCounter::count() == 0);

    fans.update_fan_speed("fan", 0.0);
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
}

// Each row names its own fan, and a tap renames that fan: the row's index
// picks its fan out of both lists.
TEST_CASE_METHOD(LVGLUITestFixture, "fan settings: a tapped row renames its own fan",
                 "[fans][settings]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(lv_screen_active());
    }
    NavigationManager::instance().set_panels(panels.data());

    auto& fans = get_printer_state().fan_state();
    fans.init_fans({"fan", "fan_generic aux"});

    auto& overlay = helix::settings::get_fan_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    lv_obj_t* list = lv_obj_find_by_name(lv_screen_active(), "controllable_fans_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 2);
    lv_obj_t* row = lv_obj_get_child(list, 1);
    lv_obj_t* object = lv_obj_find_by_name(row, "object_label");
    lv_obj_t* name = lv_obj_find_by_name(row, "name_label");
    REQUIRE((object && name));
    const std::string row_object = lv_label_get_text(object);
    const std::string row_name = lv_label_get_text(name);
    CHECK((row_object == "fan" || row_object == "fan_generic aux"));
    CHECK_FALSE(row_name.empty());

    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    lv_subject_t* old_name = lv_xml_get_subject(nullptr, "fan_rename_old_name");
    REQUIRE(old_name != nullptr);
    CHECK(std::string(lv_subject_get_string(old_name)) == row_name);
    lv_obj_t* input = lv_obj_find_by_name(lv_screen_active(), "fan_rename_new_name_input");
    REQUIRE(input != nullptr);
    CHECK(std::string(lv_textarea_get_text(input)) == row_name);

    // The rename lands on the tapped row's fan.
    auto* config = helix::Config::get_instance();
    const std::string key = config->df() + "fans/names/" + row_object;
    const std::string original = config->get<std::string>(key, "");
    lv_textarea_set_text(input, "Renamed");
    overlay.confirm_rename();
    CHECK(config->get<std::string>(key, "") == "Renamed");
    config->set<std::string>(key, original);
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
}
