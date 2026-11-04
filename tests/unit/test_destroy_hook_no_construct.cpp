// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_destroy_hook_no_construct.cpp
 * @brief Subject teardown hooks act on the panel instance that exists and never
 *        build one (prestonbrown/helixscreen#1810).
 *
 * HomePanel and BedMeshPanel register a StaticPanelRegistry destroy hook from
 * init_subjects(); BeltTensionPanel registers a StaticSubjectRegistry deinit
 * hook, which runs after destroy_all() has freed every panel. Each hook reaches
 * the process-wide instance. A hook that reached it through the constructing
 * lazy_global getter would build a whole panel during shutdown, against a
 * PrinterState and LVGL that are being torn down.
 *
 * "Constructed" is observed through lazy_global_if_exists<T>(): the only way
 * the slot becomes non-null is lazy_global<T> building an instance.
 */

#include "ui_panel_bed_mesh.h"
#include "ui_panel_belt_tension.h"
#include "ui_panel_home.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::lazy_global_if_exists;
using helix::ui::UpdateQueue;

namespace {

/// The global panels are built against the process-wide PrinterState, so its
/// subjects must exist; every case starts and ends with no lazy_global alive.
class DestroyHookFixture : public LVGLUITestFixture {
  public:
    DestroyHookFixture() {
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        free_globals();
    }

    ~DestroyHookFixture() override {
        free_globals();
    }

    static void free_globals() {
        UpdateQueue::instance().drain();
        StaticPanelRegistry::instance().destroy_all();
        UpdateQueue::instance().drain();
    }

  protected:
    // Written by a check hook registered in the test body. Members, not
    // locals: a failed REQUIRE skips the test's destroy_all(), and the
    // destructor's free_globals() then runs the hook after the body's locals
    // are gone.
    const void* expected_panel_ = nullptr;
    bool alive_at_check_ = false;
    bool subject_at_check_ = true;
};

bool xml_subject_exists(const char* name) {
    return lv_xml_get_subject(nullptr, name) != nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// HomePanel ("HomePanelSubjects", StaticPanelRegistry)
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(DestroyHookFixture, "HomePanelSubjects hook builds no panel when none was built",
                 "[destroy_hook][home]") {
    REQUIRE(lazy_global_if_exists<HomePanel>() == nullptr);
    {
        HomePanel local(get_printer_state(), nullptr);
        local.init_subjects();
        StaticPanelRegistry::instance().destroy_all();
        CHECK(lazy_global_if_exists<HomePanel>() == nullptr);
    }
    CHECK(lazy_global_if_exists<HomePanel>() == nullptr);
}

TEST_CASE_METHOD(DestroyHookFixture,
                 "HomePanelSubjects hook builds no panel after the global was destroyed",
                 "[destroy_hook][home]") {
    // The hook registers before the global does, so destroy_all() frees the
    // global first and then runs the hook against an empty slot.
    {
        HomePanel local(get_printer_state(), nullptr);
        local.init_subjects();
    }
    get_global_home_panel();
    REQUIRE(lazy_global_if_exists<HomePanel>() != nullptr);

    StaticPanelRegistry::instance().destroy_all();
    CHECK(lazy_global_if_exists<HomePanel>() == nullptr);
}

TEST_CASE_METHOD(DestroyHookFixture, "HomePanelSubjects hook deinits the live global's subjects",
                 "[destroy_hook][home]") {
    HomePanel& panel = get_global_home_panel();
    expected_panel_ = &panel;
    // Runs between the subjects hook and the panel's own destruction.
    StaticPanelRegistry::instance().register_destroy("HomeHookCheck", [this] {
        alive_at_check_ = lazy_global_if_exists<HomePanel>() == expected_panel_;
        subject_at_check_ = xml_subject_exists("home_populated_pages");
    });
    panel.init_subjects();
    REQUIRE(xml_subject_exists("home_populated_pages"));

    StaticPanelRegistry::instance().destroy_all();
    CHECK(alive_at_check_);
    CHECK_FALSE(subject_at_check_);
}

// ---------------------------------------------------------------------------
// BedMeshPanel ("BedMeshPanelSubjects", StaticPanelRegistry)
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(DestroyHookFixture,
                 "BedMeshPanelSubjects hook builds no panel when none was built",
                 "[destroy_hook][bed_mesh]") {
    REQUIRE(lazy_global_if_exists<BedMeshPanel>() == nullptr);
    {
        BedMeshPanel local;
        local.init_subjects();
        StaticPanelRegistry::instance().destroy_all();
        CHECK(lazy_global_if_exists<BedMeshPanel>() == nullptr);
    }
    CHECK(lazy_global_if_exists<BedMeshPanel>() == nullptr);
}

TEST_CASE_METHOD(DestroyHookFixture,
                 "BedMeshPanelSubjects hook builds no panel after the global was destroyed",
                 "[destroy_hook][bed_mesh]") {
    {
        BedMeshPanel local;
        local.init_subjects();
    }
    get_global_bed_mesh_panel();
    REQUIRE(lazy_global_if_exists<BedMeshPanel>() != nullptr);

    StaticPanelRegistry::instance().destroy_all();
    CHECK(lazy_global_if_exists<BedMeshPanel>() == nullptr);
}

TEST_CASE_METHOD(DestroyHookFixture, "BedMeshPanelSubjects hook deinits the live global's subjects",
                 "[destroy_hook][bed_mesh]") {
    BedMeshPanel& panel = get_global_bed_mesh_panel();
    expected_panel_ = &panel;
    StaticPanelRegistry::instance().register_destroy("BedMeshHookCheck", [this] {
        alive_at_check_ = lazy_global_if_exists<BedMeshPanel>() == expected_panel_;
        subject_at_check_ = xml_subject_exists("bed_mesh_calibrate_state");
    });
    panel.init_subjects();
    REQUIRE(xml_subject_exists("bed_mesh_calibrate_state"));

    StaticPanelRegistry::instance().destroy_all();
    CHECK(alive_at_check_);
    CHECK_FALSE(subject_at_check_);
}

// ---------------------------------------------------------------------------
// BeltTensionPanel ("BeltTensionPanel", StaticSubjectRegistry)
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(DestroyHookFixture,
                 "BeltTensionPanel deinit hook builds no panel when none was built",
                 "[destroy_hook][belt_tension]") {
    REQUIRE(lazy_global_if_exists<BeltTensionPanel>() == nullptr);
    {
        BeltTensionPanel local;
        local.init_subjects();
        REQUIRE(StaticSubjectRegistry::instance().deinit_one("BeltTensionPanel"));
        CHECK(lazy_global_if_exists<BeltTensionPanel>() == nullptr);
    }
    CHECK(lazy_global_if_exists<BeltTensionPanel>() == nullptr);
}

TEST_CASE_METHOD(DestroyHookFixture,
                 "BeltTensionPanel deinit hook builds no panel after destroy_all freed it",
                 "[destroy_hook][belt_tension]") {
    // Application::shutdown() order: panels first, then subject deinit.
    get_global_belt_tension_panel().init_subjects();
    StaticPanelRegistry::instance().destroy_all();
    REQUIRE(lazy_global_if_exists<BeltTensionPanel>() == nullptr);

    REQUIRE(StaticSubjectRegistry::instance().deinit_one("BeltTensionPanel"));
    CHECK(lazy_global_if_exists<BeltTensionPanel>() == nullptr);
}

TEST_CASE_METHOD(DestroyHookFixture,
                 "BeltTensionPanel deinit hook deinits the live global's subjects",
                 "[destroy_hook][belt_tension]") {
    BeltTensionPanel& panel = get_global_belt_tension_panel();
    panel.init_subjects();
    REQUIRE(xml_subject_exists("bt_verdict"));

    REQUIRE(StaticSubjectRegistry::instance().deinit_one("BeltTensionPanel"));
    CHECK(lazy_global_if_exists<BeltTensionPanel>() == &panel);
    CHECK_FALSE(xml_subject_exists("bt_verdict"));
}
