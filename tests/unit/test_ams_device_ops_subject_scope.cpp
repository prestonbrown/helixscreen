// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_device_ops_subject_scope.cpp
 * @brief The Device Operations overlay withdraws its subject names when it dies.
 *
 * Run with: ./build/bin/helix-tests "[ams][device-ops][subject-scope]"
 *
 * The overlay is a StaticPanelRegistry-owned singleton, so destroy_all() frees it
 * mid-process: on a soft restart, and in any test that drives that teardown. The
 * XML subject registry keeps resolving a name after the storage behind it is
 * gone, so a name left registered hands the next lv_xml_create() that binds it a
 * pointer into freed memory (prestonbrown/helixscreen#1536).
 */

#include "ui_ams_device_operations_overlay.h"

#include "../test_fixtures.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::get_ams_device_operations_overlay;

namespace {

/// Every name AmsDeviceOperationsOverlay::init_subjects() publishes.
const char* const kDeviceOpsSubjects[] = {
    "ams_device_ops_system_info",
    "ams_device_ops_status",
    "ams_device_ops_supports_bypass",
    "ams_device_ops_fw_supports_bypass",
    "ams_device_ops_hw_bypass_sensor",
    "ams_device_ops_supports_auto_heat",
    "ams_device_ops_has_backend",
    "ams_device_ops_unload_after_print_configurable",
    "ams_device_ops_bypass_is_virtual",
    "ams_device_ops_reports_spool_ids",
    "ams_device_ops_printer_retains_spool_info",
    "ams_device_ops_is_qidi",
    "ams_device_ops_qidi_eject_distance_display",
    "ams_device_ops_qidi_eject_velocity_display",
    "ams_device_ops_can_reset_endless_spool",
    "ams_device_ops_can_abort",
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "Device Operations subject names leave the XML scope with the overlay",
                 "[ams][device-ops][subject-scope]") {
    // Empty the registry first so the destroy_all() under test reaches this
    // overlay and nothing else.
    StaticPanelRegistry::instance().destroy_all();

    get_ams_device_operations_overlay().init_subjects();

    // The absence assertions below only mean something if the names were there
    // to withdraw.
    for (const char* name : kDeviceOpsSubjects) {
        INFO(name);
        REQUIRE(lv_xml_get_subject(nullptr, name) != nullptr);
    }

    StaticPanelRegistry::instance().destroy_all();

    for (const char* name : kDeviceOpsSubjects) {
        INFO(name);
        CHECK(lv_xml_get_subject(nullptr, name) == nullptr);
    }
}
