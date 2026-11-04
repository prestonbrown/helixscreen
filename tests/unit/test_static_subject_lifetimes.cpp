// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_static_subject_lifetimes.cpp
 * @brief Entries registered once per process survive a printer switch.
 *
 * A printer switch re-runs only the rebuild path. Component registration and
 * anything behind a process-once guard does not run again, so an entry those
 * register must still be in the registry after the switch, or the next switch
 * and process exit find nothing to tear down.
 */

#include "ui_ams_lane_bar.h"
#include "ui_ams_lane_spool.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_color_picker.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/static_subject_registry.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

bool registered(const char* name) {
    const std::vector<std::string> names = StaticSubjectRegistry::instance().names();
    return std::find(names.begin(), names.end(), name) != names.end();
}

/// Apply a printer switch to @p name and report whether the entry is still registered.
bool survives_switch(const char* name) {
    REQUIRE(StaticSubjectRegistry::instance().deinit_one(
        name, StaticSubjectRegistry::Scope::PrinterSwitch));
    return registered(name);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Per-widget AMS cleanups run at a printer switch and stay registered",
                 "[ams][subjects][shutdown]") {
    ui_ams_slot_register(); // registers ams_lane_spool too
    ui_ams_lane_bar_register();

    for (const char* name : {"AmsSlotWidgets", "AmsLaneSpoolWidgets", "AmsLaneBarWidgets"}) {
        CAPTURE(name);
        CHECK(survives_switch(name));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Subjects registered with the XML components outlive a printer switch",
                 "[subjects][shutdown]") {
    // The fixture runs helix::register_xml_components(), which registers both.
    CHECK(survives_switch("XmlSubjects"));

    helix::ui::ensure_swatch_grid_subjects();
    CHECK(survives_switch("SwatchGridSubjects"));
    // Not run either: the colour grid's selection subject still resolves by name.
    CHECK(lv_xml_get_subject(nullptr, "color_picker_selected_swatch") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "AMS sidebar gating subjects outlive a printer switch",
                 "[ams][sidebar][subjects][shutdown]") {
    // Created by the AMS panels' process-once registration, which a switch
    // never re-runs.
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    for (const char* name : {"AmsSidebarButtonGatingReset", "AmsSidebarButtonGating"}) {
        CAPTURE(name);
        CHECK(survives_switch(name));
    }
    for (const char* subject :
         {"ams_sidebar_unload_disabled", "ams_sidebar_load_disabled", "ams_sidebar_reset_disabled",
          "ams_sidebar_check_gates_disabled", "ams_sidebar_load_hidden",
          "ams_sidebar_unload_hidden", "ams_sidebar_unload_picks", "ams_sidebar_mount_hidden",
          "ams_sidebar_mount_disabled", "ams_sidebar_park_hidden", "ams_sidebar_park_disabled",
          "ams_sidebar_action_reason", "ams_sidebar_action_reason_shown",
          "ams_sidebar_supports_reset"}) {
        CAPTURE(subject);
        CHECK(lv_xml_get_subject(nullptr, subject) != nullptr);
    }
}
