// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_env_overlay_unit_binding.cpp
 * @brief Regression test — the environment overlay must read the unit it opened
 *
 * ams_environment_overlay.xml drives every readout from an ams_env_overlay_*
 * subject that AmsEnvironmentOverlay recomputes for whichever unit show() was
 * called with — except the humidity row and the Material Comfort strip, which
 * were bound straight to `ams_env_ind_0_humidity_visible`. Opening unit 1's
 * overlay therefore asked unit 0 whether a humidity sensor exists: a rig whose
 * second box has a sensor and whose first does not showed "42%" with the row
 * hidden, and the reverse pairing showed an empty row for a box with no sensor.
 *
 * The fix gives the overlay its own ams_env_overlay_humidity_visible subject,
 * set from the shown unit's environment, matching how every other value in that
 * overlay is produced.
 */

#include "ui_ams_environment_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Two units whose humidity sensors differ, so a binding that reads the wrong
/// unit produces a different answer than one that reads the right one.
/// Unit 0: temperature only. Unit 1: temperature + humidity.
class SplitHumidityMock : public AmsBackendMock {
  public:
    SplitHumidityMock() : AmsBackendMock(8) {}

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        info.units.clear();
        for (int u = 0; u < 2; ++u) {
            AmsUnit unit;
            unit.unit_index = u;
            unit.display_name = "Unit " + std::to_string(u + 1);
            unit.slot_count = 4;
            unit.first_slot_global_index = u * 4;
            unit.connected = true;

            EnvironmentData env;
            env.temperature_c = 25.0f + static_cast<float>(u);
            env.has_humidity = (u == 1);
            env.humidity_pct = (u == 1) ? 42.0f : 0.0f;
            unit.environment = env;

            info.units.push_back(unit);
        }
        info.total_slots = 8;
        return info;
    }
};

int subject_int(const char* name) {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
    REQUIRE(subj != nullptr);
    return lv_subject_get_int(subj);
}

/// Install the split-humidity backend and republish AmsState's subjects from it.
///
/// Order matters and is not obvious: deinit_subjects() calls clear_backends(),
/// which stops and destroys whatever backend is installed, and init_subjects()
/// then creates a factory backend because backends_ is empty. Installing the
/// mock first therefore throws it away — the subjects get published from the
/// factory's single unit instead, silently, with no error anywhere.
void install_split_humidity_backend() {
    // Deinit first: a previous test may have left the singleton initialized with
    // register_xml = false, in which case init_subjects() early-returns and the
    // ams_env_ind_* names never reach LVGL's XML scope.
    AmsState::instance().deinit_subjects();

    auto mock = std::make_unique<SplitHumidityMock>();
    REQUIRE(mock->start().success());
    AmsState::instance().set_backend(std::move(mock));

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    // Guard the ordering above: if the mock ever gets swapped out again, this
    // fails here instead of as a confusing subject-value mismatch downstream.
    AmsBackend* installed = AmsState::instance().get_backend();
    REQUIRE(installed != nullptr);
    REQUIRE(installed->get_system_info().units.size() == 2);
}

/// The overlay is a process-lifetime singleton whose widgets belong to whichever
/// test screen built it. Drop any instance an earlier case left behind so
/// create() runs against this fixture's screen instead of a freed one.
void reset_overlay_singleton() {
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

/// What the overlay decided about humidity for the unit it was opened on: the
/// subject the C++ side publishes, and the two widgets the XML gates on it.
/// Both halves are measured because asserting only the subject would let a
/// revert of the XML binding pass — the binding is where the bug lived.
struct HumidityRowState {
    int subject = -1;
    bool readout_hidden = true;
    bool comfort_strip_hidden = true;
};

bool widget_hidden(LVGLUITestFixture& fixture, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(fixture.test_screen(), name);
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Open the overlay on `unit`, read the humidity state, then tear the push down.
HumidityRowState humidity_state_for_unit(LVGLUITestFixture& fixture, int unit) {
    auto& overlay = helix::ui::get_ams_environment_overlay();
    overlay.show(fixture.test_screen(), unit);
    helix::ui::UpdateQueue::instance().drain();
    fixture.process_lvgl(10);

    HumidityRowState state;
    state.subject = subject_int("ams_env_overlay_humidity_visible");
    state.readout_hidden = widget_hidden(fixture, "humidity_readout");
    state.comfort_strip_hidden = widget_hidden(fixture, "comfort_strip");

    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();
    fixture.process_lvgl(10);
    return state;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Environment overlay shows the humidity row for the unit it was opened for",
                 "[ui_integration][ams][regression]") {
    reset_overlay_singleton();
    install_split_humidity_backend();

    // Precondition: the per-unit indicator subjects disagree, which is the whole
    // point of the fixture. If these ever match, the test below proves nothing.
    REQUIRE(subject_int("ams_env_ind_0_humidity_visible") == 0);
    REQUIRE(subject_int("ams_env_ind_1_humidity_visible") == 1);

    // THE REGRESSION ASSERTION. Pre-fix both widgets were bound to
    // ams_env_ind_0_humidity_visible, so unit 1's overlay hid a row whose
    // reading ("42%") was right there next to it.
    const HumidityRowState state = humidity_state_for_unit(*this, 1);
    CHECK(state.subject == 1);
    CHECK_FALSE(state.readout_hidden);
    CHECK_FALSE(state.comfort_strip_hidden);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Environment overlay hides the humidity row for a unit with no sensor",
                 "[ui_integration][ams][regression]") {
    reset_overlay_singleton();
    install_split_humidity_backend();

    REQUIRE(subject_int("ams_env_ind_0_humidity_visible") == 0);
    REQUIRE(subject_int("ams_env_ind_1_humidity_visible") == 1);

    // The other direction — a subject wired to unit 1, or one left permanently
    // on, would show an empty humidity row for a box that has no sensor.
    const HumidityRowState state = humidity_state_for_unit(*this, 0);
    CHECK(state.subject == 0);
    CHECK(state.readout_hidden);
    CHECK(state.comfort_strip_hidden);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}
