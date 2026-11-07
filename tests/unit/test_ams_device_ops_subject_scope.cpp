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
    "ams_device_ops_supports_bypass",
    "ams_device_ops_fw_supports_bypass",
    "ams_device_ops_hw_bypass_sensor",
    "ams_device_ops_supports_auto_heat",
    "ams_device_ops_has_backend",
    "ams_device_ops_bypass_is_virtual",
    "ams_device_ops_record_only_identity",
    "ams_device_ops_printer_retains_spool_info",
    "ams_device_ops_can_reset_endless_spool",
    "ams_device_ops_can_abort",
    "ams_device_ops_can_home",
    "ams_device_ops_can_recover",
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

namespace {
int g_status_notifies = 0;
void count_notify(lv_observer_t*, lv_subject_t*) {
    ++g_status_notifies;
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "Device Operations initialises its subjects once when pre-inited before show",
                 "[ams][device-ops][subject-scope]") {
    StaticPanelRegistry::instance().destroy_all();
    auto& overlay = get_ams_device_operations_overlay();

    // The outside path: panels pre-init before show().
    overlay.init_subjects();
    REQUIRE(overlay.are_subjects_initialized());

    // A second init through show() would re-init the subject and drop this observer.
    lv_subject_t* status = lv_xml_get_subject(nullptr, "ams_device_ops_system_info");
    REQUIRE(status != nullptr);
    g_status_notifies = 0;
    lv_observer_t* obs = lv_subject_add_observer(status, count_notify, nullptr);
    g_status_notifies = 0;

    REQUIRE(register_component("header_bar"));
    REQUIRE(register_component("ams_device_operations"));
    overlay.show(test_screen());
    overlay.show(test_screen());

    lv_subject_notify(status);
    CHECK(g_status_notifies >= 1);
    CHECK(lv_xml_get_subject(nullptr, "ams_device_ops_system_info") == status);
    lv_observer_remove(obs);

    StaticPanelRegistry::instance().destroy_all();
    CHECK(lv_xml_get_subject(nullptr, "ams_device_ops_system_info") == nullptr);
}
