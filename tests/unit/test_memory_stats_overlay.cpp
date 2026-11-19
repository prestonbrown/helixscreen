// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_memory_stats.h"

#include "../lvgl_ui_test_fixture.h"
#include "theme_manager.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "Memory stats overlay shows its readings through subjects",
                 "[memory_stats]") {
    auto& overlay = MemoryStatsOverlay::instance();
    overlay.init(nullptr, true);
    lv_obj_t* root = lv_obj_find_by_name(lv_layer_top(), "memory_stats_overlay");
    REQUIRE(root != nullptr);
    auto text = [root](const char* name) {
        return std::string(lv_label_get_text(lv_obj_find_by_name(root, name)));
    };
    auto color_is = [root](const char* name, const char* token) {
        lv_obj_t* label = lv_obj_find_by_name(root, name);
        return lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN),
                           theme_manager_get_color(token));
    };

    // show() reads /proc at once; a reading replaces the "--" placeholder.
    CHECK(text("rss_value") != "--");
    CHECK(text("hwm_value") != "--");
    CHECK(text("delta_value") != "--");
    CHECK(text("pressure_value") == "OK");
    CHECK(color_is("pressure_value", "success"));

    // Each tone selects its own text color.
    lv_subject_t* tone = lv_xml_get_subject(nullptr, "memory_stats_delta_tone");
    REQUIRE(tone != nullptr);
    const char* tokens[] = {"success", "warning", "danger"};
    for (int t = 2; t >= 0; --t) {
        lv_subject_set_int(tone, t);
        CHECK(color_is("delta_value", tokens[t]));
    }

    lv_obj_delete(root);
    overlay.shutdown();
}
