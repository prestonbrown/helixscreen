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

#include "ui_ams_context_menu.h"
#include "ui_ams_lane_bar.h"
#include "ui_ams_lane_spool.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_color_picker.h"
#include "ui_guided_device_flow.h"
#include "ui_tile_edit_callout.h"

#include "../lvgl_ui_test_fixture.h"
#include "buffer_status_modal.h"
#include "core/static_subject_registry.h"
#include "detection_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lock_manager.h"
#include "print_completion.h"
#include "src/ui/panel_widgets/dryer_model.h"
#include "src/ui/panel_widgets/filament_sensor_widget.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "tile_color.h"

#include <algorithm>
#include <functional>
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

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Static subjects drop their names and observers at deinit and come back at init",
                 "[modal][ams][subjects][shutdown][1959]") {
    const auto attach_status = [](const char* flow) {
        const helix::ui::GuidedDeviceFlow* found = helix::ui::find_guided_device_flow(flow);
        REQUIRE(found != nullptr);
        found->attach_status(lv_screen_active());
    };
    struct Source {
        const char* registry_name;
        const char* subject;
        std::function<void()> init;
        bool on_demand; ///< Published once per process, so a printer switch spares it.
    };
    helix::ui::register_builtin_guided_device_flows();
    const Source sources[] = {
        {"BufferStatusModalSubjects", "buf_fault_distance", [] { BufferStatusModal modal; }, true},
        {"AmsContextMenuSubjects", "ams_slot_park",
         [] { helix::ui::AmsContextMenu::init_subjects(); }, true},
        {"DetectionManager", "detection_bed_check_switchable",
         [] { helix::detection::DetectionManager::instance().init(nullptr, nullptr); }, true},
#if HELIX_HAS_CFS
        {"CfsCutterSubjects", "cfs_cutter_status", [&] { attach_status("cfs_cutter"); }, true},
#endif
        {"IndxFlowSubjects", "indx_flow_status", [&] { attach_status("indx_cycle_all"); }, true},
        {"FilamentSensorWidget", "filament_tile_state",
         [] { helix::register_filament_sensor_widget(); }, false},
        {"DryerModelSubjects", "home_dryer_available", [] { helix::dryer_model::init_subjects(); },
         false},
        {"PrintCompletion", "print_completion_has_estimate",
         [] { ObserverGuard completion = init_print_completion_observer(); }, false},
        {"LockManager", "lock_pin_set", [] { helix::LockManager::instance().init_subjects(); },
         false},
        {"PrintStatusWidgetSubjects", "print_status_view",
         [] { PrintStatusWidget::init_static_subjects(); }, false},
        {"TileColorSubjects", "tile_color_selected", [] { helix::tile_color::register_subjects(); },
         true},
        {"TileEditCallout", "tile_callout_title",
         [] { helix::ui::TileEditCallout::register_callbacks(); }, true},
    };
    for (const Source& source : sources) {
        CAPTURE(source.registry_name);
        // From down, the init adds one entry: a second would tear these subjects down again.
        if (registered(source.registry_name)) {
            REQUIRE(StaticSubjectRegistry::instance().deinit_one(source.registry_name));
        }
        const size_t entries = StaticSubjectRegistry::instance().count();
        source.init();
        CHECK(StaticSubjectRegistry::instance().count() == entries + 1);
        if (source.on_demand) {
            CHECK(survives_switch(source.registry_name));
        }
        lv_subject_t* subject = lv_xml_get_subject(nullptr, source.subject);
        REQUIRE(subject != nullptr);
        lv_obj_t* bound = lv_obj_create(lv_screen_active());
        lv_subject_add_observer_obj(subject, [](lv_observer_t*, lv_subject_t*) {}, bound, nullptr);

        REQUIRE(StaticSubjectRegistry::instance().deinit_one(source.registry_name));
        CHECK(lv_xml_get_subject(nullptr, source.subject) == nullptr);
        CHECK(lv_ll_get_head(&subject->subs_ll) == nullptr);
        // Its observer went with the subject, so the object deletes without touching it.
        lv_obj_delete(bound);

        source.init();
        CHECK(registered(source.registry_name));
        CHECK(lv_xml_get_subject(nullptr, source.subject) == subject);
    }
}
