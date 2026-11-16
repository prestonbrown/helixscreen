// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_drying_controller.cpp
 * @brief BedDryingController: the clearance move, the persisted latch, the
 *        timer, the flip reminder, the end of the run and the cool-down
 *        (prestonbrown/helixscreen#1730).
 */

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_state.h"
#include "bed_drying_controller.h"
#include "chamber_heater_backend.h"
#include "config.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "temperature_controller.h"
#include "test_helpers/config_test_access.h"
#include "test_helpers/configfile_mock_client.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::bed_drying;

namespace {

constexpr long long kStart = 1'000'000;
constexpr long long kHours12 = 12 * 3600;

void drain() {
    ui::UpdateQueue::instance().drain_until_empty();
}

struct BedDryingFixture : public LVGLTestFixture {
    ConfigfileMockClient client;
    PrinterState state;
    MoonrakerAPI api;
    TemperatureController tc;
    long long now = kStart;
    std::unique_ptr<BedDryingController> ctrl;
    std::vector<std::string> infos;

    BedDryingFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state), tc(state, &api) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        SettingsManager::instance().clear_bed_drying_record();
        client.config_sections = {{"idle_timeout", {{"timeout", "300"}}}};
        frame({{"toolhead",
                {{"axis_minimum", {-175, -175, 0, 0}},
                 {"axis_maximum", {175, 175, 240, 0}},
                 {"homed_axes", "xyz"}}},
               {"heater_bed", {{"target", 0.0}, {"temperature", 25.0}}}});
        ctrl = make_controller();
        ui::set_test_notification_info_hook([this](const std::string& m) { infos.push_back(m); });
        client.clear_gcode_script_history();
    }

    ~BedDryingFixture() override {
        ui::set_test_notification_info_hook(nullptr);
        ctrl.reset();
        state.print_state().set_spool_latch(false);
        SettingsManager::instance().clear_bed_drying_record();
        drain();
    }

    std::unique_ptr<BedDryingController> make_controller() {
        auto c = std::make_unique<BedDryingController>(state, &api, &tc, [this] { return now; });
        c->init_subjects();
        return c;
    }

    void frame(const nlohmann::json& status) {
        state.update_from_status(status);
        drain();
    }

    void bed(double target, double temp) {
        frame({{"heater_bed", {{"target", target}, {"temperature", temp}}}});
    }

    bool sent(const std::string& needle) const {
        const auto& h = client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        });
    }

    /// A plain chamber heater, no dryer, whose config caps it at 60 C.
    void discover_chamber_heater() {
        SettingsManager::instance().set_chamber_heater_assignment("auto");
        client.config_sections["heater_generic chamber"] = {{"max_temp", "60"}};
        PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json{"heater_generic chamber", "extruder", "heater_bed"});
        state.set_hardware(hw);
        REQUIRE(tc.resolved_name(HeaterType::Chamber) == "heater_generic chamber");
    }

    void chamber_target(double target) {
        frame({{"heater_generic chamber", {{"target", target}, {"temperature", 30.0}}}});
    }

    /// Prepare and place a run of @p m with the chamber option on.
    void start_with_chamber(const Material& m) {
        bool ready = false;
        ctrl->prepare(m, true, [&] { ready = true; }, nullptr);
        drain();
        REQUIRE(ready);
        REQUIRE(ctrl->confirm_placed());
        drain();
    }

    /// Prepare and place a PLA run.
    void start_pla() {
        bool ready = false;
        ctrl->prepare(kMaterials[0], false, [&] { ready = true; }, nullptr);
        drain();
        REQUIRE(ready);
        REQUIRE(ctrl->confirm_placed());
        drain();
    }
};

} // namespace

TEST_CASE_METHOD(BedDryingFixture, "prepare moves the plate to the far end of Z and parks",
                 "[bed_drying][1730]") {
    bool ready = false;
    ctrl->prepare(kMaterials[0], false, [&] { ready = true; }, nullptr);
    drain();

    CHECK(ready);
    CHECK_FALSE(sent("G28"));
    CHECK(sent("G1 Z220.0 F600"));
    CHECK(sent("G1 X0.0 Y165.0 F6000"));
    CHECK(sent("M400"));
    // Spools can land on the plate once the place prompt is up, so the latch is
    // set and saved before it opens; nothing heats yet.
    CHECK(state.print_state().spool_latch_active());
    CHECK(SettingsManager::instance().get_bed_drying_record().placing);
    CHECK(ctrl->state() == BedDryingController::State::Placing);
    CHECK_FALSE(sent("heater_bed"));
    // The motors stay held while the spools go on.
    CHECK(sent("SET_IDLE_TIMEOUT TIMEOUT=86400"));
}

TEST_CASE_METHOD(BedDryingFixture, "only 'no spools placed' clears the latch from placing",
                 "[bed_drying][1730]") {
    ctrl->prepare(kMaterials[0], false, nullptr, nullptr);
    drain();
    REQUIRE(state.print_state().spool_latch_active());
    ctrl->cancel_placement();
    drain();
    CHECK_FALSE(state.print_state().spool_latch_active());
    CHECK_FALSE(SettingsManager::instance().get_bed_drying_record().latched);
    CHECK(sent("SET_IDLE_TIMEOUT TIMEOUT=300"));
}

TEST_CASE_METHOD(BedDryingFixture, "a chamber appliance gets the air temperature, not the bed's",
                 "[bed_drying][1730]") {
    tc.set_chamber_dryer(chamber::backend_by_id("panda_breath"), true);
    bool ready = false;
    ctrl->prepare(kMaterials[0], true, [&] { ready = true; }, nullptr);
    drain();
    REQUIRE(ready);
    REQUIRE(ctrl->confirm_placed());
    drain();
    CHECK(sent("PANDA_BREATH_DRY_START TEMP=50"));
    CHECK_FALSE(sent("PANDA_BREATH_DRY_START TEMP=70"));
    tc.set_chamber_dryer(nullptr);
}

TEST_CASE_METHOD(BedDryingFixture, "a restore before Klipper is ready defers the end of the run",
                 "[bed_drying][1730]") {
    start_pla();
    ctrl.reset();
    state.print_state().set_spool_latch(false);
    client.clear_gcode_script_history();
    state.set_klippy_state_sync(KlippyState::STARTUP);

    now = kStart + kHours12 + 60;
    ctrl = make_controller();
    ctrl->restore();
    drain();
    CHECK(state.print_state().spool_latch_active());
    CHECK_FALSE(sent("SET_IDLE_TIMEOUT"));
    CHECK(ctrl->state() == BedDryingController::State::Running);

    state.set_klippy_state_sync(KlippyState::READY);
    ctrl->tick(now);
    drain();
    CHECK(ctrl->state() != BedDryingController::State::Running);
    CHECK(sent("SET_IDLE_TIMEOUT TIMEOUT=86400"));
}

TEST_CASE_METHOD(BedDryingFixture, "prepare parks over the plate, not in overtravel past it",
                 "[bed_drying][1730]") {
    // Snapmaker U1: Y travels to 335 past a 270 mm plate into the tool docks,
    // and homing_origin shifts G-code space off machine space.
    frame({{"toolhead",
            {{"axis_minimum", {0, 0, -6, 0}},
             {"axis_maximum", {271, 335, 275, 0}},
             {"homed_axes", "xyz"}}},
           {"gcode_move", {{"homing_origin", {-0.088928, -0.016043, 0.06, 0}}}}});
    BuildVolume vol;
    vol.x_max = 271;
    vol.y_max = 335;
    vol.plate_x_min = 3;
    vol.plate_x_max = 267;
    vol.plate_y_min = 3;
    vol.plate_y_max = 267;
    api.hardware().set_build_volume(vol);

    ctrl->prepare(kMaterials[0], false, nullptr, nullptr);
    drain();

    CHECK(sent("G1 Z254.9 F600"));
    CHECK(sent("G1 X135.1 Y257.0 F6000"));
}

TEST_CASE_METHOD(BedDryingFixture, "prepare homes first when an axis is unhomed",
                 "[bed_drying][1730]") {
    frame({{"toolhead", {{"homed_axes", "xy"}}}});
    ctrl->prepare(kMaterials[0], false, nullptr, nullptr);
    drain();
    const auto& h = client.gcode_script_history();
    auto home = std::find_if(h.begin(), h.end(),
                             [](const std::string& l) { return l.rfind("G28", 0) == 0; });
    auto move = std::find_if(h.begin(), h.end(), [](const std::string& l) {
        return l.find("G1 Z220.0") != std::string::npos;
    });
    REQUIRE(home != h.end());
    REQUIRE(move != h.end());
    CHECK(home < move);
}

TEST_CASE_METHOD(BedDryingFixture, "confirming the spools persists the latch, then heats",
                 "[bed_drying][1730]") {
    start_pla();

    const RunRecord r = SettingsManager::instance().get_bed_drying_record();
    CHECK(r.latched);
    CHECK(r.start_s == kStart);
    CHECK(r.end_s == kStart + kHours12);
    CHECK(r.bed_c == 70);
    CHECK(r.idle_restore_s == 300);
    CHECK(state.print_state().spool_latch_active());
    CHECK(ctrl->state() == BedDryingController::State::Running);
    CHECK(sent("heater_bed"));
    // Held to the planned end plus the 10 minute dead-man margin.
    CHECK(sent("SET_IDLE_TIMEOUT TIMEOUT=43800"));
}

TEST_CASE_METHOD(BedDryingFixture, "an app restart brings the latch back from settings",
                 "[bed_drying][1730]") {
    start_pla();
    ctrl.reset();
    state.print_state().set_spool_latch(false);

    now = kStart + 3600;
    ctrl = make_controller();
    ctrl->restore();

    CHECK(state.print_state().spool_latch_active());
    CHECK(ctrl->state() == BedDryingController::State::Running);
    CHECK(lv_subject_get_int(state.print_state().get_machine_motion_blocked_subject()) == 1);
}

TEST_CASE_METHOD(BedDryingFixture, "the flip reminder fires once, at the midpoint",
                 "[bed_drying][1730]") {
    start_pla();
    bed(70, 70);
    ctrl->tick(kStart + kHours12 / 2 - 1);
    CHECK(infos.empty());
    ctrl->tick(kStart + kHours12 / 2);
    CHECK(infos.size() == 1);
    ctrl->tick(kStart + kHours12 / 2 + 60);
    CHECK(infos.size() == 1);
    CHECK(SettingsManager::instance().get_bed_drying_record().flip_notified);
}

TEST_CASE_METHOD(BedDryingFixture, "a running cycle publishes minutes left and progress",
                 "[bed_drying][1730][progress]") {
    CHECK(lv_subject_get_int(ctrl->get_remaining_min_subject()) == -1);
    CHECK(lv_subject_get_int(ctrl->get_progress_pct_subject()) == -1);

    start_pla();
    bed(70, 70);
    ctrl->tick(kStart);
    CHECK(lv_subject_get_int(ctrl->get_remaining_min_subject()) == 12 * 60);
    CHECK(lv_subject_get_int(ctrl->get_progress_pct_subject()) == 0);

    now = kStart + kHours12 / 4;
    ctrl->tick(now);
    CHECK(lv_subject_get_int(ctrl->get_remaining_min_subject()) == 9 * 60);
    CHECK(lv_subject_get_int(ctrl->get_progress_pct_subject()) == 25);

    // A partial minute left still reads as a minute.
    now = kStart + kHours12 - 30;
    ctrl->tick(now);
    CHECK(lv_subject_get_int(ctrl->get_remaining_min_subject()) == 1);

    now = kStart + kHours12;
    ctrl->tick(now);
    drain();
    CHECK(lv_subject_get_int(ctrl->get_remaining_min_subject()) == -1);
    CHECK(lv_subject_get_int(ctrl->get_progress_pct_subject()) == -1);
}

TEST_CASE_METHOD(BedDryingFixture, "the run ends at its planned end and the latch stays",
                 "[bed_drying][1730]") {
    start_pla();
    bed(70, 70);
    client.clear_gcode_script_history();

    ctrl->tick(kStart + kHours12);
    drain();

    CHECK(ctrl->state() == BedDryingController::State::Cooling);
    // Spools still on the plate: the motors stay held, the timeout is not restored.
    CHECK(sent("SET_IDLE_TIMEOUT TIMEOUT=86400"));
    CHECK_FALSE(sent("SET_IDLE_TIMEOUT TIMEOUT=300"));
    CHECK(sent("heater_bed"));
    CHECK(state.print_state().spool_latch_active());
    CHECK(SettingsManager::instance().get_bed_drying_record().ended);
}

TEST_CASE_METHOD(BedDryingFixture, "the remove prompt waits for the bed to cool below 40 C",
                 "[bed_drying][1730]") {
    int prompts = 0;
    ctrl->set_on_ready_to_remove([&] { ++prompts; });
    start_pla();
    ctrl->stop();

    bed(0, 60);
    ctrl->tick(kStart + 60);
    CHECK(prompts == 0);
    CHECK(ctrl->state() == BedDryingController::State::Cooling);

    bed(0, 35);
    ctrl->tick(kStart + 120);
    ctrl->tick(kStart + 121);
    CHECK(prompts == 1);
    CHECK(ctrl->state() == BedDryingController::State::ReadyToRemove);

    client.clear_gcode_script_history();
    ctrl->confirm_removed();
    drain();
    // The configured timeout comes back only once the spools are off.
    CHECK(sent("SET_IDLE_TIMEOUT TIMEOUT=300"));
    CHECK_FALSE(state.print_state().spool_latch_active());
    CHECK_FALSE(SettingsManager::instance().get_bed_drying_record().latched);
    CHECK(ctrl->state() == BedDryingController::State::Idle);
}

TEST_CASE_METHOD(BedDryingFixture, "Klipper dropping the bed target ends the run",
                 "[bed_drying][1730]") {
    start_pla();
    bed(70, 50);
    ctrl->tick(kStart + 60);
    CHECK(ctrl->state() == BedDryingController::State::Running);
    bed(0, 50);
    ctrl->tick(kStart + 61);
    CHECK(ctrl->state() == BedDryingController::State::Cooling);
    CHECK(state.print_state().spool_latch_active());
}

TEST_CASE_METHOD(BedDryingFixture, "a restart after the planned end finishes the run at once",
                 "[bed_drying][1730]") {
    start_pla();
    ctrl.reset();
    state.print_state().set_spool_latch(false);
    client.clear_gcode_script_history();

    now = kStart + kHours12 + 3600; // power came back after the run should have ended
    ctrl = make_controller();
    ctrl->restore();
    drain();

    CHECK(state.print_state().spool_latch_active());
    CHECK(ctrl->state() != BedDryingController::State::Running);
    CHECK(sent("SET_IDLE_TIMEOUT TIMEOUT=86400"));
}

TEST_CASE_METHOD(BedDryingFixture, "nothing latches or heats when the drying state cannot be saved",
                 "[bed_drying][1730]") {
    bool& read_only = ConfigTestAccess::read_only_mode(*Config::get_instance());
    read_only = true;
    std::string error;
    bool ready = false;
    ctrl->prepare(
        kMaterials[0], false, [&] { ready = true; }, [&](const std::string& e) { error = e; });
    drain();
    read_only = false;

    CHECK_FALSE(ready);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(state.print_state().spool_latch_active());
    CHECK_FALSE(ctrl->confirm_placed());
    CHECK_FALSE(sent("heater_bed"));
}

TEST_CASE_METHOD(BedDryingFixture, "a restart while placing keeps the chosen material",
                 "[bed_drying][1730]") {
    ctrl->prepare(kMaterials[2], false, nullptr, nullptr); // PETG
    drain();
    REQUIRE(ctrl->state() == BedDryingController::State::Placing);
    ctrl.reset();
    state.print_state().set_spool_latch(false);

    ctrl = make_controller();
    ctrl->restore();
    CHECK(state.print_state().spool_latch_active());
    CHECK(ctrl->state() == BedDryingController::State::Placing);
    REQUIRE(ctrl->confirm_placed());
    drain();
    const RunRecord r = SettingsManager::instance().get_bed_drying_record();
    CHECK(r.bed_c == 85);
    CHECK(r.end_s == kStart + kHours12);
    CHECK(ctrl->state() == BedDryingController::State::Running);
}

TEST_CASE_METHOD(BedDryingFixture,
                 "removal restores Klipper's default when the timeout was never read",
                 "[bed_drying][1730]") {
    client.fail_configfile = true;
    start_pla();
    REQUIRE(SettingsManager::instance().get_bed_drying_record().idle_restore_s == 0);
    ctrl->stop();
    client.clear_gcode_script_history();
    ctrl->confirm_removed();
    drain();
    CHECK(sent("SET_IDLE_TIMEOUT TIMEOUT=600"));
}

// -----------------------------------------------------------------------------
// The unload and the plate move before the spools go on
// -----------------------------------------------------------------------------
//
// Both take a minute or more with nothing else on screen, so the banner shows
// each step, and stopping from it must stop the flow for good.

namespace {

void publish_ams_action(AmsAction action) {
    lv_subject_set_int(AmsState::instance().get_ams_action_subject(), static_cast<int>(action));
    drain();
}

/// AmsState's action subject is what an unload wait watches.
struct AmsSubjects {
    AmsSubjects() {
        AmsState::instance().init_subjects(true);
    }
    ~AmsSubjects() {
        AmsState::instance().deinit_subjects();
    }
    AmsSubjects(const AmsSubjects&) = delete;
    AmsSubjects& operator=(const AmsSubjects&) = delete;
};

std::string banner_text(BedDryingController& c) {
    return lv_subject_get_string(c.get_text_subject());
}

} // namespace

TEST_CASE_METHOD(BedDryingFixture, "the banner shows the unload, then the plate move",
                 "[bed_drying][1730]") {
    AmsSubjects ams;
    publish_ams_action(AmsAction::IDLE);
    bool ready = false;
    ctrl->await_unload([&] { ctrl->prepare(kMaterials[0], false, [&] { ready = true; }, nullptr); },
                       nullptr);

    CHECK(ctrl->state() == BedDryingController::State::Unloading);
    CHECK(lv_subject_get_int(ctrl->get_state_subject()) ==
          static_cast<int>(BedDryingController::State::Unloading));
    CHECK(banner_text(*ctrl) == "Unloading filament before drying...");

    publish_ams_action(AmsAction::UNLOADING);
    CHECK(ctrl->state() == BedDryingController::State::Unloading);
    publish_ams_action(AmsAction::IDLE);
    CHECK(ready);
    CHECK(ctrl->state() == BedDryingController::State::Placing);
}

TEST_CASE_METHOD(BedDryingFixture, "the banner stays up while the plate moves",
                 "[bed_drying][1730]") {
    bool ready = false;
    ctrl->prepare(kMaterials[0], false, [&] { ready = true; }, nullptr);

    CHECK(ctrl->state() == BedDryingController::State::Preparing);
    CHECK(banner_text(*ctrl) == "Homing and moving the plate...");
    drain();
    CHECK(ready);
    CHECK(ctrl->state() == BedDryingController::State::Placing);
}

TEST_CASE_METHOD(BedDryingFixture, "stopping during the unload never moves the plate",
                 "[bed_drying][1730]") {
    AmsSubjects ams;
    publish_ams_action(AmsAction::IDLE);
    bool moved_on = false;
    ctrl->await_unload([&] { moved_on = true; }, nullptr);
    publish_ams_action(AmsAction::UNLOADING);

    ctrl->cancel_preparation();
    CHECK(ctrl->state() == BedDryingController::State::Idle);
    publish_ams_action(AmsAction::IDLE);

    CHECK_FALSE(moved_on);
    CHECK(ctrl->state() == BedDryingController::State::Idle);
}

TEST_CASE_METHOD(BedDryingFixture, "stopping during the plate move never starts placement",
                 "[bed_drying][1730]") {
    bool ready = false;
    ctrl->prepare(kMaterials[0], false, [&] { ready = true; }, nullptr);
    ctrl->cancel_preparation();
    drain();

    CHECK_FALSE(ready);
    CHECK(ctrl->state() == BedDryingController::State::Idle);
    CHECK_FALSE(state.print_state().spool_latch_active());
    CHECK_FALSE(SettingsManager::instance().get_bed_drying_record().latched);
}

TEST_CASE_METHOD(BedDryingFixture, "a second start while the plate moves is refused",
                 "[bed_drying][1730]") {
    ctrl->prepare(kMaterials[0], false, nullptr, nullptr);
    std::string error;
    ctrl->prepare(kMaterials[0], false, nullptr, [&](const std::string& m) { error = m; });

    CHECK_FALSE(error.empty());
    const auto& h = client.gcode_script_history();
    CHECK(std::count_if(h.begin(), h.end(), [](const std::string& l) {
              return l.find("G1 Z220.0") != std::string::npos;
          }) == 1);
}

TEST_CASE_METHOD(BedDryingFixture,
                 "a plain chamber heater holds the air temperature, capped at its max_temp",
                 "[bed_drying][chamber_heater]") {
    discover_chamber_heater();
    SECTION("PLA's 50 C is under the cap") {
        start_with_chamber(kMaterials[0]);
        CHECK(sent("SET_HEATER_TEMPERATURE HEATER=chamber TARGET=50"));
        CHECK(SettingsManager::instance().get_bed_drying_record().chamber_c == 50);
    }
    SECTION("ABS's 80 C is held to the heater's 60") {
        start_with_chamber(kMaterials[4]);
        CHECK(sent("SET_HEATER_TEMPERATURE HEATER=chamber TARGET=60"));
        CHECK_FALSE(sent("HEATER=chamber TARGET=80"));
        CHECK(SettingsManager::instance().get_bed_drying_record().chamber_c == 60);
    }
    CHECK(sent("heater_bed"));
}

TEST_CASE_METHOD(BedDryingFixture, "the chamber heater stays off when the option is off",
                 "[bed_drying][chamber_heater]") {
    discover_chamber_heater();
    start_pla();
    CHECK_FALSE(sent("HEATER=chamber"));
    CHECK(SettingsManager::instance().get_bed_drying_record().chamber_c == 0);
}

TEST_CASE_METHOD(BedDryingFixture, "Stop turns the chamber heater off while it is still ours",
                 "[bed_drying][chamber_heater]") {
    discover_chamber_heater();
    start_with_chamber(kMaterials[0]);
    bed(70, 70);
    chamber_target(50);
    client.clear_gcode_script_history();

    ctrl->stop();
    drain();

    // Sent while the spool latch is still set, so the latch lets it through.
    REQUIRE(state.print_state().spool_latch_active());
    CHECK(sent("SET_HEATER_TEMPERATURE HEATER=chamber TARGET=0"));
}

TEST_CASE_METHOD(BedDryingFixture, "a chamber target changed by hand is left alone at the end",
                 "[bed_drying][chamber_heater]") {
    discover_chamber_heater();
    start_with_chamber(kMaterials[0]);
    bed(70, 70);
    chamber_target(45);
    client.clear_gcode_script_history();

    ctrl->stop();
    drain();

    CHECK_FALSE(sent("HEATER=chamber TARGET=0"));
}

TEST_CASE_METHOD(BedDryingFixture,
                 "a restart mid-run still turns the chamber heater off at the end",
                 "[bed_drying][chamber_heater]") {
    discover_chamber_heater();
    start_with_chamber(kMaterials[0]);
    ctrl.reset();
    state.print_state().set_spool_latch(false);

    ctrl = make_controller();
    ctrl->restore();
    bed(70, 70);
    chamber_target(50);
    client.clear_gcode_script_history();

    ctrl->tick(kStart + kHours12);
    drain();

    CHECK(ctrl->state() == BedDryingController::State::Cooling);
    CHECK(sent("SET_HEATER_TEMPERATURE HEATER=chamber TARGET=0"));
}

TEST_CASE_METHOD(BedDryingFixture, "a chamber dryer wins over the plain heater",
                 "[bed_drying][chamber_heater]") {
    discover_chamber_heater();
    tc.set_chamber_dryer(chamber::backend_by_id("panda_breath"), true);
    start_with_chamber(kMaterials[0]);
    CHECK(sent("PANDA_BREATH_DRY_START TEMP=50"));
    CHECK_FALSE(sent("HEATER=chamber"));
    CHECK(SettingsManager::instance().get_bed_drying_record().chamber_c == 0);
    tc.set_chamber_dryer(nullptr);
}

TEST_CASE_METHOD(BedDryingFixture, "a temperature_fan chamber is not driven as a heater",
                 "[bed_drying][chamber_heater]") {
    SettingsManager::instance().set_chamber_heater_assignment("auto");
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json{"temperature_fan chamber", "extruder", "heater_bed"});
    state.set_hardware(hw);
    REQUIRE(tc.resolved_name(HeaterType::Chamber) == "temperature_fan chamber");

    CHECK(ctrl->chamber_assist_available() == ChamberAssist::None);
    start_with_chamber(kMaterials[0]);
    CHECK_FALSE(sent("TEMPERATURE_FAN=chamber"));
    CHECK(SettingsManager::instance().get_bed_drying_record().chamber_c == 0);
}

TEST_CASE_METHOD(BedDryingFixture, "the start modal's chamber row names what heats and to what",
                 "[bed_drying][chamber_heater]") {
    lv_subject_t* assist = ctrl->get_chamber_assist_subject();
    lv_subject_t* text = ctrl->get_chamber_text_subject();

    ctrl->describe_chamber(kMaterials[4]);
    CHECK(lv_subject_get_int(assist) == static_cast<int>(ChamberAssist::None));

    discover_chamber_heater();
    ctrl->prepare(kMaterials[4], false, nullptr, nullptr); // reads the 60 C cap
    drain();
    ctrl->describe_chamber(kMaterials[4]);
    CHECK(lv_subject_get_int(assist) == static_cast<int>(ChamberAssist::Heater));
    CHECK(std::string(lv_subject_get_string(text)) == "Heat the chamber too (60°C)");
    ctrl->describe_chamber(kMaterials[0]);
    CHECK(std::string(lv_subject_get_string(text)) == "Heat the chamber too (50°C)");

    tc.set_chamber_dryer(chamber::backend_by_id("panda_breath"), true);
    ctrl->describe_chamber(kMaterials[0]);
    CHECK(lv_subject_get_int(assist) == static_cast<int>(ChamberAssist::Dryer));
    tc.set_chamber_dryer(nullptr);
}

TEST_CASE_METHOD(BedDryingFixture, "Stop while placing sends no chamber off",
                 "[bed_drying][chamber_heater]") {
    discover_chamber_heater();
    bool ready = false;
    ctrl->prepare(kMaterials[0], true, [&] { ready = true; }, nullptr);
    drain();
    REQUIRE(ready);
    REQUIRE(ctrl->state() == BedDryingController::State::Placing);
    client.clear_gcode_script_history();

    ctrl->stop();
    drain();

    CHECK_FALSE(sent("HEATER=chamber"));
}

TEST_CASE_METHOD(BedDryingFixture,
                 "a print taking the machine while spools are latched raises one alarm",
                 "[bed_drying][print_alarm]") {
    int alarms = 0;
    ctrl->set_on_print_while_latched([&] { ++alarms; });
    start_pla();
    REQUIRE(alarms == 0);

    // Started from another client: the latch cannot refuse it.
    frame({{"print_stats", {{"state", "printing"}, {"filename", "part.gcode"}}}});
    CHECK(alarms == 1);

    // Once per print, not once per status frame.
    frame({{"print_stats", {{"state", "printing"}, {"filename", "part.gcode"}}}});
    CHECK(alarms == 1);

    frame({{"print_stats", {{"state", "cancelled"}, {"filename", "part.gcode"}}}});
    frame({{"print_stats", {{"state", "printing"}, {"filename", "part.gcode"}}}});
    CHECK(alarms == 2);
}

TEST_CASE_METHOD(BedDryingFixture, "a print with no spools latched raises no alarm",
                 "[bed_drying][print_alarm]") {
    int alarms = 0;
    ctrl->set_on_print_while_latched([&] { ++alarms; });
    frame({{"print_stats", {{"state", "printing"}, {"filename", "part.gcode"}}}});
    CHECK(alarms == 0);
}

TEST_CASE_METHOD(BedDryingFixture,
                 "a restart into a running print with spools latched raises the alarm",
                 "[bed_drying][print_alarm]") {
    start_pla();
    ctrl.reset();
    state.print_state().set_spool_latch(false);
    frame({{"print_stats", {{"state", "printing"}, {"filename", "part.gcode"}}}});

    int alarms = 0;
    ctrl = make_controller();
    ctrl->set_on_print_while_latched([&] { ++alarms; });
    ctrl->restore();
    drain();
    CHECK(alarms == 1);
}
