// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_environment_overlay_live_refresh.cpp
 * @brief Regression test — AmsEnvironmentOverlay must track live environment data
 *
 * Bug: the overlay pulled backend->get_system_info() exactly once in its
 * show()/refresh() path with no observer, so its temperature/humidity readouts
 * froze the moment it opened while live data kept flowing through AmsState.
 *
 * Fix: on_activate() subscribes observers to the per-unit environment indicator
 * subjects (env_ind_temp_text / env_ind_humidity_text) and the system dryer
 * subjects (dryer_active / dryer_current_temp) — NOT slots_version, which only
 * bumps on slot-data changes, not environment changes — and re-pulls via
 * refresh(); on_deactivate() drops the observers.
 *
 * The test drives the real overlay against a mock backend whose unit-0
 * temperature is injectable. It shows the overlay at temperature A, asserts the
 * bound temp subject reflects A, then changes the backend to temperature B,
 * runs the production state-change path (sync_from_backend() updates the
 * env_ind_temp_text subject the overlay observes) + drains UpdateQueue, and
 * asserts the overlay now reflects B. Pre-fix the readout stays at A and the
 * final assertion fails.
 */

#include "ui_ams_environment_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Mock backend that lets the test pin unit 0's reported environment temperature,
// so the displayed readout has a single deterministic source we can change between
// value A and value B. Everything else is the stock realistic mock behavior.
class TempInjectMock : public AmsBackendMock {
  public:
    explicit TempInjectMock(int slot_count) : AmsBackendMock(slot_count) {}

    void set_test_temp(float temp_c) {
        test_temp_c_ = temp_c;
    }

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        if (!info.units.empty() && info.units[0].environment.has_value()) {
            info.units[0].environment->temperature_c = test_temp_c_;
        }
        return info;
    }

  private:
    float test_temp_c_ = 0.0f;
};

// Read a string subject from LVGL's global XML scope (where the overlay registers
// its bound subjects). Returns empty string if not found.
std::string subject_string(const char* name) {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
    if (!subj) {
        return {};
    }
    const char* s = lv_subject_get_string(subj);
    return s ? std::string(s) : std::string();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsEnvironmentOverlay re-pulls environment data on AMS state change",
                 "[ui_integration][ams][regression]") {
    using helix::ui::get_ams_environment_overlay;

    // ------------------------------------------------------------------
    // 1. Install a started mock with passive environment sensors and pin
    //    unit-0 temperature to value A (30 C) before building the overlay.
    // ------------------------------------------------------------------
    constexpr float TEMP_A = 30.0f;
    constexpr float TEMP_B = 45.0f;

    auto mock = std::make_unique<TempInjectMock>(4);
    mock->set_environment_mode("passive"); // per-unit temp/humidity, no dryer
    mock->set_test_temp(TEMP_A);
    REQUIRE(mock->start().success());

    auto* backend = mock.get();
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true); // before XML creation so bindings resolve
    AmsState::instance().sync_from_backend();

    // ------------------------------------------------------------------
    // 2. Build and activate the real overlay (unit 0). on_activate() is the
    //    hook under test — it subscribes the environment/dryer subject observers
    //    and does the immediate refresh().
    // ------------------------------------------------------------------
    // LVGLUITestFixture registers all production XML components (overlay_panel,
    // header_bar, dividers, ui_card/ui_button, text_*), and create() self-registers
    // ams_environment_overlay.xml, so lv_xml_create() can resolve the full tree.
    auto& overlay = get_ams_environment_overlay();
    overlay.init_subjects();
    overlay.register_callbacks();
    REQUIRE(overlay.create(test_screen()) != nullptr);

    overlay.refresh();     // mirrors show()'s initial pull
    overlay.on_activate(); // subscribes the live-update observer
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);

    // The readout starts at value A. A "--" here means the harness never got
    // environment data — do NOT weaken this; the test must start from a real read.
    REQUIRE(subject_string("ams_env_overlay_temp_text").rfind("30", 0) == 0);

    // ------------------------------------------------------------------
    // 3. Change the backend to value B and fire the production state-change
    //    path: sync_from_backend() updates env_ind_temp_text_, which the overlay's
    //    observer watches. Drain the queue so the deferred refresh runs.
    // ------------------------------------------------------------------
    backend->set_test_temp(TEMP_B);
    AmsState::instance().sync_from_backend(); // updates env_ind_temp_text subject
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);

    // ------------------------------------------------------------------
    // 4. THE REGRESSION ASSERTION. Pre-fix (no observer) the readout stays at
    //    "30" and this fails. Post-fix the observer re-pulled and shows "45".
    // ------------------------------------------------------------------
    REQUIRE(subject_string("ams_env_overlay_temp_text").rfind("45", 0) == 0);

    // Tear down: drop the observer, hide the overlay widget before the fixture
    // destroys the screen/state.
    overlay.on_deactivate();
    helix::ui::UpdateQueue::instance().drain();
    AmsState::instance().set_backend(nullptr);
}
