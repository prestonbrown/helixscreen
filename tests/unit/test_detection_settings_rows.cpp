// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_detection_settings_rows.cpp
 * @brief the detection rows in Settings > Safety exist only behind detection_available.
 *
 * The rows and their container carry no C++ visibility code: the container binds
 * LV_OBJ_FLAG_HIDDEN to detection_available, the pause row's own container binds
 * it to detection_pause_applicable (firmware-pausing sources ignore the setting,
 * so their row hides), and the pause row passes disabled="detection_enabled" so
 * its toggle greys out while detection is off. All XML-only wiring, so the proof
 * has to build the real overlay and read the widget flags back; an attribute
 * dropped at the component boundary would leave the rows always-visible with no
 * parse error.
 *
 * detection_available and detection_pause_applicable are owned by
 * DetectionManager, registered the first time a source registers. The U1 source
 * starts unavailable, so registering it creates both subjects at 0 without
 * seeding anything, and each section then writes the values it asserts against.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "detection_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "settings_manager.h"
#include "u1_stock_detection_source.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

struct DetectionRowsFixture : public LVGLUITestFixture {
    DetectionRowsFixture() {
        SettingsManager::instance().init_subjects();
        auto& m = helix::detection::DetectionManager::instance();
        // Registers detection_available into LVGL's global subject scope at 0
        // (the U1 source starts unavailable; nothing seeds).
        m.reset_for_test();
        m.register_source(std::make_unique<helix::detection::U1StockSource>(nullptr));
        avail_ = m.subject_detection_available();
        REQUIRE(avail_ != nullptr);

        enabled_ = lv_xml_get_subject(nullptr, "detection_enabled");
        pause_ = lv_xml_get_subject(nullptr, "detection_pause_on_detect");
        applicable_ = m.subject_detection_pause_applicable();
        REQUIRE(enabled_ != nullptr);
        REQUIRE(pause_ != nullptr);
        REQUIRE(applicable_ != nullptr);
    }

    ~DetectionRowsFixture() override {
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        // Back to the defaults other cases in this binary assume.
        lv_subject_set_int(avail_, 0);
        lv_subject_set_int(enabled_, 1);
        lv_subject_set_int(pause_, 1);
        lv_subject_set_int(applicable_, 0);
        UpdateQueue::instance().drain();
    }

    void build() {
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_safety_overlay", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(10);
    }

    lv_obj_t* container() {
        lv_obj_t* c = lv_obj_find_by_name(root_, "container_detection");
        REQUIRE(c != nullptr);
        return c;
    }

    lv_obj_t* pause_container() {
        lv_obj_t* c = lv_obj_find_by_name(root_, "container_detection_pause");
        REQUIRE(c != nullptr);
        return c;
    }

    lv_obj_t* pause_toggle() {
        lv_obj_t* row = lv_obj_find_by_name(root_, "row_detection_pause");
        REQUIRE(row != nullptr);
        lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
        REQUIRE(toggle != nullptr);
        return toggle;
    }

    lv_obj_t* root_ = nullptr;
    lv_subject_t* avail_ = nullptr;
    lv_subject_t* enabled_ = nullptr;
    lv_subject_t* pause_ = nullptr;
    lv_subject_t* applicable_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(DetectionRowsFixture, "Detection settings rows follow detection_available",
                 "[detection][settings][safety][binding]") {
    SECTION("no capable source: the container hides both rows") {
        lv_subject_set_int(avail_, 0);
        build();
        CHECK(lv_obj_has_flag(container(), LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("a capable source shows the rows") {
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(applicable_, 1);
        build();
        CHECK_FALSE(lv_obj_has_flag(container(), LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(pause_container(), LV_OBJ_FLAG_HIDDEN));
        CHECK(pause_toggle() != nullptr);
    }

    SECTION("a firmware-pausing source hides the pause row") {
        // The U1's firmware pauses by itself; the toggle cannot prevent that,
        // so detection_pause_applicable stays 0 and the row's container hides
        // while the enabled row stays visible.
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(applicable_, 0);
        build();
        CHECK_FALSE(lv_obj_has_flag(container(), LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(pause_container(), LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("pause row reappears when a HelixScreen-pausing source shows up") {
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(applicable_, 0);
        build();
        REQUIRE(lv_obj_has_flag(pause_container(), LV_OBJ_FLAG_HIDDEN));

        lv_subject_set_int(applicable_, 1);
        process_lvgl(10);
        CHECK_FALSE(lv_obj_has_flag(pause_container(), LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("pause row is disabled while detection is off") {
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(applicable_, 1);
        lv_subject_set_int(enabled_, 0);
        build();
        CHECK(lv_obj_has_state(pause_toggle(), LV_STATE_DISABLED));
    }

    SECTION("pause row is live: turning detection off disables it in place") {
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(applicable_, 1);
        lv_subject_set_int(enabled_, 1);
        build();
        REQUIRE_FALSE(lv_obj_has_state(pause_toggle(), LV_STATE_DISABLED));

        lv_subject_set_int(enabled_, 0);
        process_lvgl(10);
        CHECK(lv_obj_has_state(pause_toggle(), LV_STATE_DISABLED));
    }

    SECTION("pause row greys only through the binding, not the row object") {
        // The `disabled` prop is consumed by $param substitution inside the
        // component; the instantiation site must not also apply it to the row
        // root as the engine's native boolean `disabled` attribute, which
        // would statically disable the row and dim the whole subtree whatever
        // the subject says.
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(applicable_, 1);
        lv_subject_set_int(enabled_, 1);
        build();
        lv_obj_t* row = lv_obj_find_by_name(root_, "row_detection_pause");
        REQUIRE(row != nullptr);
        CHECK(lv_obj_get_state(row) == 0);
        CHECK_FALSE(lv_obj_has_state(pause_toggle(), LV_STATE_DISABLED));
    }
}

// The bed-check row stands apart from the spaghetti rows: it is gated on a
// printer that can switch its own bed check, not on detection at large.
TEST_CASE_METHOD(DetectionRowsFixture, "Bed check row follows detection_bed_check_available",
                 "[detection][settings][safety][binding][bed_check]") {
    auto& m = helix::detection::DetectionManager::instance();
    lv_subject_t* bed_available = m.subject_bed_check_available();
    lv_subject_t* bed_enabled = m.subject_bed_check_enabled();
    auto bed_container = [this] {
        lv_obj_t* c = lv_obj_find_by_name(root_, "container_bed_check");
        REQUIRE(c != nullptr);
        return c;
    };
    auto bed_toggle = [this] {
        lv_obj_t* row = lv_obj_find_by_name(root_, "row_bed_check");
        REQUIRE(row != nullptr);
        lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
        REQUIRE(toggle != nullptr);
        return toggle;
    };

    SECTION("detection without a bed check: spaghetti rows show, the bed row hides") {
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(bed_available, 0);
        build();
        CHECK_FALSE(lv_obj_has_flag(container(), LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(bed_container(), LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("a printer with a bed check shows the row, toggled to its state") {
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(bed_available, 1);
        lv_subject_set_int(bed_enabled, 0);
        build();
        CHECK_FALSE(lv_obj_has_flag(bed_container(), LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_state(bed_toggle(), LV_STATE_CHECKED));

        lv_subject_set_int(bed_enabled, 1);
        process_lvgl(10);
        CHECK(lv_obj_has_state(bed_toggle(), LV_STATE_CHECKED));
        CHECK_FALSE(lv_obj_has_state(bed_toggle(), LV_STATE_DISABLED));
    }

    SECTION("a detection module switched off as a whole greys the row") {
        lv_subject_set_int(avail_, 1);
        lv_subject_set_int(bed_available, 1);
        lv_subject_set_int(bed_enabled, 0);
        lv_subject_set_int(m.subject_bed_check_switchable(), 0);
        build();
        CHECK_FALSE(lv_obj_has_flag(bed_container(), LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_state(bed_toggle(), LV_STATE_DISABLED));
    }

    lv_subject_set_int(bed_available, 0);
    lv_subject_set_int(bed_enabled, 1);
    lv_subject_set_int(m.subject_bed_check_switchable(), 1);
}
