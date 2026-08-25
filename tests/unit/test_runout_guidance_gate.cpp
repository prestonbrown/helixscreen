// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_runout_guidance_gate.cpp
 * @brief FilamentRunoutHandler's pause-gated show/reset state machine
 *
 * Run with: ./build/bin/helix-tests "[runout_guidance][gate]"
 *
 * Drives the real handler (src/ui/ui_filament_runout_handler.cpp:67-125) rather
 * than a model of it. Two coupled decisions:
 *
 *   check_and_show_runout_guidance() — show at most ONCE per pause event, and
 *       only when a loaded lane actually lost filament (has_real_runout).
 *   on_print_state_changed()        — a transition OUT of the pause (Printing,
 *       Idle, Complete, Cancelled, Error) clears the once-per-pause latch and
 *       hides a modal still on screen.
 *
 * Without the latch reset, a print that pauses -> resumes -> pauses again on a
 * second runout shows no guidance the second time. Without the latch itself,
 * every status push while paused re-shows the dialog on top of itself.
 *
 * The dialog's six action buttons (Load / Unload / Purge / Resume / Cancel) are
 * covered against the same real handler in test_filament_dispatch_surfaces.cpp.
 */

#include "ui_filament_runout_handler.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "runtime_config.h"

#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::FilamentSensorManager;
using helix::FilamentSensorRole;
using helix::ui::FilamentRunoutHandler;

namespace {

/// A plain single-extruder runout printer: one RUNOUT-roled switch sensor and no
/// AMS backend. That shape leaves has_real_runout() unscoped (no lane to check
/// against) and should_show_runout_modal() true, so the guards under test are the
/// only thing standing between a pause and the dialog.
class RunoutGuidanceFixture : public LVGLUITestFixture {
  public:
    static constexpr const char* SENSOR = "filament_switch_sensor runout_sensor";

    RunoutGuidanceFixture() {
        AmsState::instance().clear_backends();

        auto& fsm = FilamentSensorManager::instance();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        fsm.discover_sensors({SENSOR});
        fsm.set_sensor_role(SENSOR, FilamentSensorRole::RUNOUT);
        // Baseline present so the sensor is "available", then let each test choose.
        fsm.update_from_status(sensor_status(true));
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
        settle();
        REQUIRE_FALSE(fsm.is_in_startup_grace_period());
        REQUIRE(get_runtime_config()->should_show_runout_modal());

        handler = std::make_unique<FilamentRunoutHandler>(api());
    }

    ~RunoutGuidanceFixture() override {
        if (handler) {
            handler->hide_modal();
            handler.reset();
        }
        settle();
        PostUnloadGraceTestAccess::reset(FilamentSensorManager::instance());
        AmsState::instance().clear_backends();
    }

    static nlohmann::json sensor_status(bool detected) {
        return nlohmann::json{{SENSOR, {{"filament_detected", detected}, {"enabled", true}}}};
    }

    /// Flip the physical sensor. `false` is the runout the guards look for.
    void set_filament_present(bool present) {
        FilamentSensorManager::instance().update_from_status(sensor_status(present));
        settle();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    /// The only entry point production uses.
    void transition(::PrintState from, ::PrintState to) {
        handler->on_print_state_changed(from, to);
        settle();
    }

    std::unique_ptr<FilamentRunoutHandler> handler;
};

} // namespace

TEST_CASE_METHOD(RunoutGuidanceFixture, "pausing on a real runout shows the guidance dialog",
                 "[runout_guidance][gate]") {
    set_filament_present(false);
    REQUIRE(FilamentSensorManager::instance().has_real_runout());

    transition(::PrintState::Printing, ::PrintState::Paused);

    CHECK(handler->is_modal_shown_for_pause());
    CHECK(handler->is_modal_visible());
}

TEST_CASE_METHOD(RunoutGuidanceFixture, "pausing with filament present shows nothing",
                 "[runout_guidance][gate]") {
    // A pause for any other reason — M600 outside a runout, a user tap, a
    // filament-agnostic error. The dialog must not ambush it.
    set_filament_present(true);
    REQUIRE_FALSE(FilamentSensorManager::instance().has_real_runout());

    transition(::PrintState::Printing, ::PrintState::Paused);

    CHECK_FALSE(handler->is_modal_shown_for_pause());
    CHECK_FALSE(handler->is_modal_visible());
}

TEST_CASE_METHOD(RunoutGuidanceFixture, "the dialog shows at most once per pause event",
                 "[runout_guidance][gate]") {
    set_filament_present(false);
    transition(::PrintState::Printing, ::PrintState::Paused);
    REQUIRE(handler->is_modal_visible());

    // The user dismissed it with OK, then another status push re-enters Paused.
    // Klipper republishes print_stats on every temperature tick, so this is the
    // common case, not an edge one.
    handler->hide_modal();
    settle();
    REQUIRE_FALSE(handler->is_modal_visible());

    transition(::PrintState::Paused, ::PrintState::Paused);

    CHECK_FALSE(handler->is_modal_visible());
    CHECK(handler->is_modal_shown_for_pause()); // latch still held
}

TEST_CASE_METHOD(RunoutGuidanceFixture, "resuming clears the latch and hides the dialog",
                 "[runout_guidance][gate]") {
    set_filament_present(false);
    transition(::PrintState::Printing, ::PrintState::Paused);
    REQUIRE(handler->is_modal_visible());

    // User loaded filament and hit Resume.
    set_filament_present(true);
    transition(::PrintState::Paused, ::PrintState::Printing);

    CHECK_FALSE(handler->is_modal_shown_for_pause());
    CHECK_FALSE(handler->is_modal_visible());

    // Second runout later in the same print must get guidance again — this is
    // what the latch reset buys.
    set_filament_present(false);
    transition(::PrintState::Printing, ::PrintState::Paused);

    CHECK(handler->is_modal_shown_for_pause());
    CHECK(handler->is_modal_visible());
}

TEST_CASE_METHOD(RunoutGuidanceFixture, "every terminal state clears the latch, not just Printing",
                 "[runout_guidance][gate]") {
    // Cancelling from the dialog ends the job at Cancelled, never passing through
    // Printing. If only Printing reset the latch, the next print's first runout
    // would be silent.
    const auto terminal =
        GENERATE(::PrintState::Idle, ::PrintState::Complete, ::PrintState::Cancelled,
                 ::PrintState::Error, ::PrintState::Printing);

    set_filament_present(false);
    transition(::PrintState::Printing, ::PrintState::Paused);
    REQUIRE(handler->is_modal_shown_for_pause());
    REQUIRE(handler->is_modal_visible());

    transition(::PrintState::Paused, terminal);

    CHECK_FALSE(handler->is_modal_shown_for_pause());
    CHECK_FALSE(handler->is_modal_visible());
}
