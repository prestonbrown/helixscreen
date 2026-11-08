// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_hardware_sensors_row.cpp
 * @brief The Hardware page always offers the Sensors row
 *
 * The Sensors screen holds every sensor type and the chamber sensor and heater
 * assignments, so no one sensor type decides whether it can be reached.
 */

// TEST_MIRROR_OK: exercises the shipped ui_xml layout, which has no header to include

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "Hardware page: Sensors row shows with no filament sensor",
                 "[settings][sensors_row]") {
    lv_subject_t* filament_sensors = lv_xml_get_subject(nullptr, "filament_sensor_count");
    REQUIRE(filament_sensors != nullptr);
    const int before = lv_subject_get_int(filament_sensors);
    lv_subject_set_int(filament_sensors, 0);

    auto* root = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "settings_hardware_overlay", nullptr));
    REQUIRE(root != nullptr);
    lv_obj_t* container = lv_obj_find_by_name(root, "container_filament_sensors");
    REQUIRE(container != nullptr);
    CHECK_FALSE(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(root);
    lv_subject_set_int(filament_sensors, before);
}
