// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_runout_guidance_gate.cpp
 * @brief FilamentRunoutHandler's pause-gated show/reset state machine
 *
 * Run with: ./build/bin/helix-tests "[runout_guidance][gate]"
 *
 * Drives the real handler (src/ui/filament/ui_filament_runout_handler.cpp:67-125) rather
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

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "detection_manager.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "runtime_config.h"
#include "test_helpers/registered_backend.h"
#include "u1_stock_detection_source.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

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
        helix::AmsState::instance().clear_backends();

        auto& fsm = FilamentSensorManager::instance();
        helix::PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        discover_filament_sensors(fsm, {SENSOR});
        fsm.set_role(SENSOR, static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        // Baseline present so the sensor is "available", then let each test choose.
        fsm.update_from_status(sensor_status(true));
        helix::PostUnloadGraceTestAccess::clear_startup_grace(fsm);
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
        helix::PostUnloadGraceTestAccess::reset(FilamentSensorManager::instance());
        helix::AmsState::instance().clear_backends();
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

// A Snapmaker U1 printing on head 2, with head 3's filament waiting in its
// feeder short of the toolhead (preload_finish), pauses for a reason that is
// not filament (a dirty bed). Head 3's toolhead switch reads empty, but head 3
// is not the one being fed.
TEST_CASE_METHOD(RunoutGuidanceFixture,
                 "a pause with only an idle head's switch empty shows no dialog; the fed "
                 "head's does",
                 "[runout_guidance][gate]") {
    helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> u1(nullptr, nullptr);
    auto head_switch = [](int head, bool detected) {
        return nlohmann::json{{"filament_motion_sensor e" + std::to_string(head) + "_filament",
                               {{"filament_detected", detected}, {"enabled", true}}}};
    };
    auto feed = [](const char* channel_state) {
        return nlohmann::json{{"filament_detected", true}, {"channel_state", channel_state}};
    };
    helix::SnapmakerTestAccess::handle_status(
        *u1, nlohmann::json{
                 {"filament_feed left",
                  {{"extruder0", feed("load_finish")}, {"extruder1", feed("load_finish")}}},
                 {"filament_feed right",
                  {{"extruder2", feed("load_finish")}, {"extruder3", feed("preload_finish")}}},
                 {"toolhead", {{"extruder", "extruder2"}}}});
    REQUIRE((*u1).get_current_slot() == 2);
    REQUIRE((*u1).get_slot_info(3).is_present());

    auto& fsm = FilamentSensorManager::instance();
    std::vector<std::string> heads;
    for (int h = 0; h < 4; ++h) {
        heads.push_back("filament_motion_sensor e" + std::to_string(h) + "_filament");
    }
    discover_filament_sensors(fsm, heads);
    for (int h = 0; h < 4; ++h) {
        fsm.set_role(heads[h], static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        const auto frame = head_switch(h, h != 3);
        helix::SnapmakerTestAccess::handle_status(*u1, frame);
        fsm.update_from_status(frame);
    }
    settle();
    REQUIRE(get_runtime_config()->should_show_runout_modal());
    REQUIRE(fsm.has_any_runout());

    transition(::PrintState::Preparing, ::PrintState::Paused);
    CHECK_FALSE(handler->is_modal_shown_for_pause());
    CHECK_FALSE(handler->is_modal_visible());

    // Resumed; then the head being fed runs out and the firmware pauses.
    transition(::PrintState::Paused, ::PrintState::Printing);
    const auto empty = head_switch(2, false);
    helix::SnapmakerTestAccess::handle_status(*u1, empty);
    fsm.update_from_status(empty);
    settle();
    transition(::PrintState::Printing, ::PrintState::Paused);
    CHECK(handler->is_modal_shown_for_pause());
    CHECK(handler->is_modal_visible());
}

// The U1's bed check pauses with exception 532 code 1. That pause is the
// detection's to explain: the runout dialog stays away even while a runout
// sensor reads empty.
TEST_CASE_METHOD(RunoutGuidanceFixture, "a pause a detection explains shows no runout dialog",
                 "[runout_guidance][gate][bed_check]") {
    auto& dm = helix::detection::DetectionManager::instance();
    dm.reset_for_test();
    auto source = std::make_unique<helix::detection::U1StockSource>(&get_printer_state());
    auto* u1 = source.get();
    u1->start();
    dm.register_source(std::move(source));
    u1->set_capable(true);

    set_filament_present(false);
    REQUIRE(FilamentSensorManager::instance().has_real_runout());
    get_printer_state().update_from_status(nlohmann::json{
        {"print_stats",
         {{"state", "paused"},
          {"exception",
           {{"id", 532}, {"index", 0}, {"code", 1}, {"message", "detected dirty bed"}}}}}});
    settle();
    REQUIRE(dm.pause_explained_by_detection());

    transition(::PrintState::Preparing, ::PrintState::Paused);
    CHECK_FALSE(handler->is_modal_shown_for_pause());
    CHECK_FALSE(handler->is_modal_visible());

    // The same empty sensor on a pause nothing else explains still asks.
    get_printer_state().update_from_status(nlohmann::json{
        {"print_stats", {{"state", "paused"}, {"exception", nlohmann::json::object()}}}});
    settle();
    REQUIRE_FALSE(dm.pause_explained_by_detection());
    transition(::PrintState::Printing, ::PrintState::Paused);
    CHECK(handler->is_modal_visible());

    get_printer_state().update_from_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});
    settle();
    dm.reset_for_test();
}

namespace {
nlohmann::json print_stats(nlohmann::json fields) {
    return nlohmann::json{{"print_stats", std::move(fields)}};
}
nlohmann::json defect(int code, const char* message) {
    return {{"id", 532}, {"index", 0}, {"code", code}, {"message", message}};
}
} // namespace

// An exception describes the pause it came with. A later pause whose frames
// carry none is a pause of its own, and a runout there still gets guidance.
TEST_CASE_METHOD(RunoutGuidanceFixture, "a plain pause after a detection's pause is not explained",
                 "[runout_guidance][gate][bed_check]") {
    auto& dm = helix::detection::DetectionManager::instance();
    dm.reset_for_test();
    auto source = std::make_unique<helix::detection::U1StockSource>(&get_printer_state());
    auto* u1 = source.get();
    u1->start();
    dm.register_source(std::move(source));
    u1->set_capable(true);
    auto& ps = get_printer_state();

    ps.update_from_status(
        print_stats({{"state", "printing"}, {"exception", nlohmann::json::object()}}));
    ps.update_from_status(
        print_stats({{"state", "paused"}, {"exception", defect(2, "detected noodle")}}));
    settle();
    REQUIRE(dm.pause_explained_by_detection());

    // Resumed: the firmware leaves its exception as it was, so no frame
    // clears it.
    ps.update_from_status(print_stats({{"state", "printing"}}));
    settle();
    CHECK_FALSE(dm.pause_explained_by_detection());

    set_filament_present(false);
    ps.update_from_status(print_stats({{"state", "paused"}}));
    settle();
    CHECK_FALSE(dm.pause_explained_by_detection());
    transition(::PrintState::Printing, ::PrintState::Paused);
    CHECK(handler->is_modal_visible());

    ps.update_from_status(print_stats({{"state", "standby"}}));
    settle();
    dm.reset_for_test();
}

// The exception can land a frame after the pause, once the guidance is up
// for it. The detection then closes it, whichever defect it is.
TEST_CASE_METHOD(RunoutGuidanceFixture,
                 "a detection arriving after the pause closes the runout dialog",
                 "[runout_guidance][gate][bed_check]") {
    const auto [code, message] = GENERATE(std::pair<int, const char*>{1, "detected dirty bed"},
                                          std::pair<int, const char*>{2, "detected noodle"});
    auto& dm = helix::detection::DetectionManager::instance();
    dm.reset_for_test();
    auto source = std::make_unique<helix::detection::U1StockSource>(&get_printer_state());
    auto* u1 = source.get();
    u1->start();
    dm.register_source(std::move(source));
    u1->set_capable(true);
    auto& ps = get_printer_state();

    set_filament_present(false);
    ps.update_from_status(
        print_stats({{"state", "printing"}, {"exception", nlohmann::json::object()}}));
    ps.update_from_status(print_stats({{"state", "paused"}}));
    settle();
    transition(::PrintState::Printing, ::PrintState::Paused);
    REQUIRE(handler->is_modal_visible());

    ps.update_from_status(print_stats({{"exception", defect(code, message)}}));
    settle();
    CHECK(dm.pause_explained_by_detection());
    CHECK_FALSE(handler->is_modal_visible());

    ps.update_from_status(print_stats({{"state", "standby"}}));
    settle();
    dm.reset_for_test();
}
