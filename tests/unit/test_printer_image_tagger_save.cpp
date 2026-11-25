// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_image_tagger_save.cpp
 * @brief The printer image tagger's Save: a labelled header action that shows
 *        once every prompt is answered, inside the screen at 480x272 and
 *        800x480, and whose tap stores the tags.
 */

#include "ui_button.h"
#include "ui_nav_manager.h"
#include "ui_overlay_printer_image_tagger.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/scoped_geometry.h"
#include "../ui_test_utils.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "printer_image_regions.h"

#include <array>
#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::settings;

namespace {

struct ScreenSize {
    const char* what;
    int32_t w;
    int32_t h;
};

constexpr ScreenSize kSizes[] = {
    {"480x272", 480, 272},
    {"800x480", 800, 480},
};

constexpr const char* kKey = "custom:tagger-save-test";

bool shown(lv_obj_t* obj) {
    for (lv_obj_t* p = obj; p != nullptr; p = lv_obj_get_parent(p)) {
        if (lv_obj_is_hidden(p)) {
            return false;
        }
    }
    return true;
}

bool inside_screen(lv_obj_t* obj, const ScreenSize& s) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a.x1 >= 0 && a.y1 >= 0 && a.x2 < s.w && a.y2 < s.h;
}

class TaggerSaveFixture : public LVGLUITestFixture {
  protected:
    TaggerSaveFixture() {
        for (auto& p : panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels_.data());
    }

    ~TaggerSaveFixture() override {
        UITest::cleanup();
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    lv_obj_t* find(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(lv_screen_active(), name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    void open_tagger() {
        ImageTagTarget target;
        target.path = "A:assets/images/printers/tagger-save-test.png";
        target.key = kKey;
        target.natural_w = 300;
        target.natural_h = 150;
        REQUIRE(get_printer_image_tagger_overlay().show(lv_screen_active(), target));
        settle();
        lv_obj_update_layout(lv_screen_active());
        UITest::init(lv_screen_active());
    }

    /// Answers every prompt with a tap on the middle of the image.
    void tap_every_prompt() {
        lv_obj_t* img = find("tagger_image");
        lv_area_t a;
        lv_obj_get_coords(img, &a);
        for (int i = 0; i < ImageTagSession::kPromptCount; ++i) {
            REQUIRE(UITest::click_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2));
            settle();
        }
        lv_obj_update_layout(lv_screen_active());
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

} // namespace

TEST_CASE_METHOD(TaggerSaveFixture,
                 "Tagger Save is a labelled header action that shows once every prompt is "
                 "answered, and its tap stores the tags",
                 "[image_tagger][save_header]") {
    for (const ScreenSize& s : kSizes) {
        INFO(s.what);
        const ConfigDirGuard cfg("tagger_save");
        helix::test::ScopedGeometry screen(s.w, s.h);
        open_tagger();

        lv_obj_t* save = find("btn_tagger_save");
        CHECK_FALSE(shown(save));

        tap_every_prompt();
        REQUIRE(shown(save));
        CHECK(inside_screen(save, s));
        lv_obj_t* label = ui_button_get_label(save);
        REQUIRE(label != nullptr);
        CHECK(shown(label));
        // The header holds it: the bottom row no longer carries a Save.
        CHECK_FALSE(lv_obj_find_by_name(find("tagger_buttons"), "btn_tagger_save"));

        CHECK(lookup_user_image_regions(kKey, 300, 150) == nullptr);
        REQUIRE(UITest::click(save));
        settle();
        CHECK(lookup_user_image_regions(kKey, 300, 150) != nullptr);

        reset_user_image_regions(kKey);
        for (lv_obj_t* orphan : StaticPanelRegistry::instance().destroy_all()) {
            lv_obj_delete(orphan);
        }
        settle();
    }
}
