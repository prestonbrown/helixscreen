// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_hardware_camera_row.cpp
 * @brief The Hardware page offers a Camera row only where a camera view can open
 *
 * Moonraker reports a webcam on printers whose HelixScreen build has the camera
 * view compiled out (HELIX_HAS_CAMERA=0). There the row would open nothing.
 *
 * @see src/app_globals.cpp - platform_camera_supported
 */

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {

struct HardwareCameraRowFixture : public LVGLUITestFixture {
    HardwareCameraRowFixture() {
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_hardware_overlay", nullptr));
    }

    ~HardwareCameraRowFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
    }

    static lv_subject_t* subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return s;
    }

    // The whole container hides, without a webcam or without camera support:
    // a hidden row in a visible container still leaves a gap in the list.
    bool camera_row_hidden() const {
        lv_obj_t* container = lv_obj_find_by_name(root_, "container_camera");
        REQUIRE(container != nullptr);
        return lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(HardwareCameraRowFixture, "Camera support subject matches the build",
                 "[settings][camera]") {
    CHECK(lv_subject_get_int(subject("platform_camera_supported")) == HELIX_HAS_CAMERA);
}

TEST_CASE_METHOD(HardwareCameraRowFixture,
                 "Hardware page: the Camera row needs a webcam and a camera build",
                 "[settings][camera]") {
    REQUIRE(root_ != nullptr);
    lv_subject_t* webcam = subject("printer_has_webcam");
    lv_subject_t* supported = subject("platform_camera_supported");
    const int webcam_before = lv_subject_get_int(webcam);
    const int supported_before = lv_subject_get_int(supported);

    lv_subject_set_int(webcam, 1);
    lv_subject_set_int(supported, 1);
    process_lvgl(5);
    CHECK_FALSE(camera_row_hidden());

    lv_subject_set_int(supported, 0);
    process_lvgl(5);
    CHECK(camera_row_hidden());

    lv_subject_set_int(supported, 1);
    lv_subject_set_int(webcam, 0);
    process_lvgl(5);
    CHECK(camera_row_hidden());

    lv_subject_set_int(webcam, webcam_before);
    lv_subject_set_int(supported, supported_before);
}
