// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_op_heater.cpp
 * @brief The nozzle heat around a filament op: FilamentOpHeater, and every
 *        execute_filament_*() surface going through it.
 *
 * Run with: ./build/bin/helix-tests "[filament][preheat][heater]"
 *
 * The home Filament tile's dialog dispatches through execute_filament_load/
 * unload/purge() and closes the moment the button is pressed, so these drive
 * that entry with a real TemperatureController and read the gcode Moonraker
 * receives: the heat command, then the filament macro, in that order.
 */

#include "ui_ams_sidebar.h"
#include "ui_filament_runout_handler.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/armed_timers.h"
#include "../test_helpers/filament_runout_handler_test_access.h"
#include "../test_helpers/filament_sensor_widget_test_access.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_op_heater.h"
#include "filament_op_router.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"
#include "temperature_controller.h"
#include "tool_state.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::FilamentSensorWidget;
using helix::FilamentSensorWidgetTestAccess;
using helix::ui::decide_preheat;
using helix::ui::FilamentOpHeater;
using helix::ui::PreheatDecision;
using helix::ui::PreheatInputs;

// ============================================================================
// decide_preheat(): the rule, with no printer
// ============================================================================

namespace {

PreheatInputs nozzle(int current, int target, std::optional<int> material = std::nullopt,
                     int held = 0) {
    PreheatInputs in;
    in.current_c = current;
    in.target_c = target;
    in.material_c = material;
    in.held_c = held;
    in.floor_c = 170;
    in.ceiling_c = 300;
    return in;
}

} // namespace

TEST_CASE("decide_preheat: a cold nozzle heats to the material", "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(25, 0, 240));
    CHECK(d.heat);
    CHECK(d.send_c == 240);
    CHECK(d.wait_c == 240);
}

TEST_CASE("decide_preheat: with no material named it heats to the default load temperature",
          "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(25, 0));
    CHECK(d.heat);
    CHECK(d.send_c == 220);
    CHECK(d.wait_c == 220);
}

TEST_CASE("decide_preheat: a reached live target counts only where the op honours it",
          "[filament][preheat][heater]") {
    // A paused print's reload, or a purge / extrude / retract: the op runs at the
    // temperature someone set, even when the material names a hotter one.
    PreheatInputs honoured = nozzle(210, 210, 240);
    honoured.honor_live_target = true;
    CHECK_FALSE(decide_preheat(honoured).heat);
    honoured.current_c = 206; // within the margin
    CHECK_FALSE(decide_preheat(honoured).heat);
    honoured.current_c = 150; // still heating toward it is not reached
    CHECK(decide_preheat(honoured).heat);

    // A load or unload while idle heats to the material: a nozzle left at the
    // last material's temperature is no reason to load PETG at PLA's.
    const PreheatDecision load = decide_preheat(nozzle(210, 210, 240));
    CHECK(load.heat);
    CHECK(load.wait_c == 240);
}

TEST_CASE("preheat_reached: within the margin, never below the extrusion floor",
          "[filament][preheat][heater]") {
    CHECK(helix::ui::preheat_reached(216, 220, 170));
    CHECK_FALSE(helix::ui::preheat_reached(214, 220, 170));
    // A wait at the floor reaches only at the floor: Klipper refuses below it.
    CHECK_FALSE(helix::ui::preheat_reached(166, 170, 170));
    CHECK(helix::ui::preheat_reached(170, 170, 170));
}

TEST_CASE("decide_preheat: a held target below the extrusion floor is not hot enough",
          "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(150, 150, 210));
    CHECK(d.heat);
    CHECK(d.send_c == 210);
}

TEST_CASE("decide_preheat: with the heater off, near the material temperature is hot enough",
          "[filament][preheat][heater]") {
    CHECK_FALSE(decide_preheat(nozzle(206, 0, 210)).heat);
    CHECK(decide_preheat(nozzle(204, 0, 210)).heat);
    // Unknown material: anything at or above the floor extrudes.
    CHECK_FALSE(decide_preheat(nozzle(170, 0)).heat);
    CHECK(decide_preheat(nozzle(169, 0)).heat);
}

TEST_CASE("decide_preheat: the swap latch raises both the bar and the wait",
          "[filament][preheat][heater]") {
    // The previous material sat at 250 and the nozzle cooled: it reheats to purge
    // that material out ahead of the new one.
    const PreheatDecision d = decide_preheat(nozzle(215, 0, 210, /*held=*/250));
    CHECK(d.heat);
    CHECK(d.send_c == 210); // the controller's keep_previous_hot holds it at 250
    CHECK(d.wait_c == 250);
}

TEST_CASE("decide_preheat: targets stay between the floor and the hotend's maximum",
          "[filament][preheat][heater]") {
    const PreheatDecision hot = decide_preheat(nozzle(25, 0, 350));
    CHECK(hot.send_c == 300);
    CHECK(hot.wait_c == 300);
    const PreheatDecision cool = decide_preheat(nozzle(25, 0, 150));
    CHECK(cool.send_c == 170);
    CHECK(cool.wait_c == 170);
}

// ============================================================================
// The surfaces, end to end
// ============================================================================

namespace {

constexpr const char* kHeatGcode = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=";

class HeaterFixture : public LVGLTestFixture {
  public:
    HeaterFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.temperature_state().init_extruders({"extruder"});
        state.set_klippy_state_sync(helix::KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(client, state);
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        controller =
            std::make_unique<helix_test::ScopedSharedResource<helix::TemperatureController>>(
                std::make_shared<helix::TemperatureController>(state, api.get()));
        heater = std::make_unique<helix_test::ScopedSharedResource<FilamentOpHeater>>(
            std::make_shared<FilamentOpHeater>(state, [this]() { return fake_now; }));

        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().clear_backends();
        helix::AmsState::instance().clear_external_spool_info();
        helix::MacroParamCache::instance().clear();
        configure_filament_macros();
        cold_extrude_was_ = helix::SafetySettingsManager::instance().get_allow_cold_extrude();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
        PostOpCooldownManager::instance().init();
        client.clear_gcode_script_history();
    }

    ~HeaterFixture() override {
        heater.reset();
        controller.reset();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(cold_extrude_was_);
        PostOpCooldownManager::instance().cancel();
        set_moonraker_api(previous_api_);
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        helix::AmsState::instance().clear_backends();
        helix::AmsState::instance().clear_external_spool_info();
        helix::ui::UpdateQueue::instance().drain();
        client.disconnect();
        api.reset();
    }

    void configure_filament_macros() {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro LOAD_FILAMENT",
                                  "gcode_macro UNLOAD_FILAMENT", "gcode_macro PURGE"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).is_empty());
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::UnloadFilament).is_empty());
    }

    /// Klipper's report for the extruder, through the real status path.
    void feed_nozzle(double actual_c, double target_c) {
        state.update_from_status({{"extruder", {{"temperature", actual_c}, {"target", target_c}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The heater's handlers run deferred and queue the cooldown's own work, so
    /// drain until quiet.
    static void drain_all() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Assign an external spool of @p material, which a load with no lane heats for.
    void set_spool(const char* material) {
        helix::SlotInfo spool;
        spool.material = material;
        helix::AmsState::instance().set_external_spool_info_in_memory(spool);
    }

    void set_print_state(const char* wire) {
        state.update_from_status({{"print_stats", {{"state", wire}}}});
        drain_all();
    }

    /// Fire the heater's deadline check, as its LVGL timer does every second.
    void fire_deadline_timer() {
        for (lv_timer_t* t : helix::test::armed_timers_for(heater->ptr())) {
            t->timer_cb(t);
        }
        drain_all();
    }

    /// A ticket the heater heated for: a cold request, run once hot.
    helix::ui::FilamentOpTicket heated_ticket() {
        feed_nozzle(25, 0);
        helix::ui::FilamentOpTicket ticket = 0;
        helix::ui::PreheatRequest req;
        req.op = helix::ui::FilamentHeatOp::Extrude;
        req.run = [&ticket](helix::ui::FilamentOpTicket t) { ticket = t; };
        REQUIRE(heater->get().run_after_preheat(std::move(req)) ==
                FilamentOpHeater::Outcome::Heating);
        feed_nozzle(220, 220);
        REQUIRE(ticket != 0);
        return ticket;
    }

    /// Index of the first script containing @p needle, or -1.
    [[nodiscard]] int sent_at(const std::string& needle) const {
        const auto& hist = client.gcode_script_history();
        for (size_t i = 0; i < hist.size(); ++i) {
            if (hist[i].find(needle) != std::string::npos) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    /// Scripts that start with @p prefix: "LOAD_FILAMENT" without "UNLOAD_FILAMENT".
    [[nodiscard]] int count_starting(const std::string& prefix) const {
        int n = 0;
        for (const auto& script : client.gcode_script_history()) {
            if (script.rfind(prefix, 0) == 0) {
                ++n;
            }
        }
        return n;
    }
    [[nodiscard]] bool sent(const std::string& needle) const {
        return sent_at(needle) >= 0;
    }

    std::chrono::steady_clock::time_point fake_now = std::chrono::steady_clock::now();
    MoonrakerClientMock client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<helix_test::ScopedSharedResource<helix::TemperatureController>> controller;
    std::unique_ptr<helix_test::ScopedSharedResource<FilamentOpHeater>> heater;
    FilamentSensorWidget tile;

  private:
    helix::IMoonrakerAPI* previous_api_ = nullptr;
    bool cold_extrude_was_ = false;
};

} // namespace

TEST_CASE_METHOD(HeaterFixture, "The home tile's Load heats a cold nozzle before the load macro",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(25, 0);

    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater->get().pending());
    CHECK(sent(std::string(kHeatGcode) + "220"));
    CHECK_FALSE(sent("LOAD_FILAMENT"));

    feed_nozzle(150, 220);
    CHECK_FALSE(sent("LOAD_FILAMENT")); // still heating

    feed_nozzle(218, 220);
    CHECK_FALSE(heater->get().pending());
    REQUIRE(sent("LOAD_FILAMENT"));
    CHECK(sent_at(kHeatGcode) < sent_at("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "The home tile's Unload heats a cold nozzle before the macro",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(25, 0);

    FilamentSensorWidgetTestAccess::dispatch_unload(tile);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater->get().pending());
    CHECK(sent(kHeatGcode));
    CHECK_FALSE(sent("UNLOAD_FILAMENT"));

    feed_nozzle(220, 220);
    REQUIRE(sent("UNLOAD_FILAMENT"));
    CHECK(sent_at(kHeatGcode) < sent_at("UNLOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "The home tile's Purge heats a cold nozzle before the macro",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(25, 0);

    FilamentSensorWidgetTestAccess::dispatch_purge(tile);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater->get().pending());
    CHECK_FALSE(sent("PURGE"));

    feed_nozzle(220, 220);
    CHECK(sent("PURGE"));
}

TEST_CASE_METHOD(HeaterFixture, "A hot nozzle loads at once with no heat command",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(215, 215);
    client.clear_gcode_script_history();

    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent("LOAD_FILAMENT"));
    CHECK_FALSE(sent(kHeatGcode));
}

TEST_CASE_METHOD(HeaterFixture, "The allow-cold-load setting skips the preheat",
                 "[filament][preheat][heater][tile]") {
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(true);
    feed_nozzle(25, 0);

    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent("LOAD_FILAMENT"));
    CHECK_FALSE(sent(kHeatGcode));
}

TEST_CASE_METHOD(HeaterFixture, "A runout reload during a pause with a hot nozzle does not wait",
                 "[filament][preheat][heater][runout]") {
    // The paused print holds its own temperature; the reload runs at it, even
    // for a spool whose material names a hotter one.
    set_print_state("paused");
    set_spool("PETG");
    feed_nozzle(210, 210);
    client.clear_gcode_script_history();

    helix::ui::FilamentRunoutHandler handler(api.get());
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent("LOAD_FILAMENT"));
    CHECK_FALSE(sent(kHeatGcode));
}

TEST_CASE_METHOD(HeaterFixture, "The same reload while idle heats to the spool's material",
                 "[filament][preheat][heater][runout]") {
    // The contrast to the paused case: a nozzle left at the last material's
    // temperature is no reason to load PETG at PLA's.
    set_print_state("standby");
    set_spool("PETG");
    feed_nozzle(210, 210);
    client.clear_gcode_script_history();

    helix::ui::FilamentRunoutHandler handler(api.get());
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(heater->get().pending());
    CHECK_FALSE(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "The load runs after the surface that asked for it is gone",
                 "[filament][preheat][heater][tile]") {
    feed_nozzle(25, 0);
    {
        FilamentSensorWidget closing_tile;
        FilamentSensorWidgetTestAccess::dispatch_load(closing_tile);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(heater->get().pending());
    }
    feed_nozzle(220, 220);
    CHECK(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "A second tap while heating cancels and turns the heater back off",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent(std::string(kHeatGcode) + "0")); // it was off when the user asked

    // Heat arriving later for any other reason runs nothing.
    feed_nozzle(220, 220);
    CHECK_FALSE(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "Cancelling leaves alone a target someone else had set",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 200); // the user's own preheat, still climbing
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    REQUIRE(heater->get().cancel());
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(sent(std::string(kHeatGcode) + "0"));
}

TEST_CASE_METHOD(HeaterFixture, "A preheat whose target is taken away runs nothing",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(heater->get().pending());

    feed_nozzle(100, 220); // the target lands
    REQUIRE(heater->get().pending());
    feed_nozzle(100, 0); // and is cleared: the user, a heater fault, a shutdown
    CHECK_FALSE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK_FALSE(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "A preheat cancels a cooldown left from the previous op",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    cd.schedule();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(cd.has_pending_timer());

    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(heater->get().pending());
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "A finished op the heater heated for cools down only when idle",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");

    const auto ticket = heated_ticket();
    heater->get().op_succeeded(ticket);
    drain_all();
    CHECK(cd.has_pending_timer());
    // Named, so a tool change before it fires cannot redirect it.
    CHECK(cd.pending_heaters() == std::vector<std::string>{"extruder"});

    cd.cancel();
    drain_all();
    REQUIRE_FALSE(cd.has_pending_timer());

    // A paused print resumes at its own temperature.
    const auto paused_ticket = heated_ticket();
    set_print_state("paused");
    heater->get().op_succeeded(paused_ticket);
    drain_all();
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "An op the heater did not heat for is not cooled down",
                 "[filament][preheat][heater][cooldown]") {
    // A hand preheat, or a backend that heats for itself: not ours to turn off.
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    feed_nozzle(215, 215);

    helix::ui::FilamentOpTicket ticket = 0;
    helix::ui::PreheatRequest req;
    req.op = helix::ui::FilamentHeatOp::Extrude;
    req.run = [&ticket](helix::ui::FilamentOpTicket t) { ticket = t; };
    REQUIRE(heater->get().run_after_preheat(std::move(req)) == FilamentOpHeater::Outcome::Ran);
    REQUIRE(ticket != 0);

    heater->get().op_succeeded(ticket);
    drain_all();
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "A preheat waiting when an op finishes takes over its cooldown",
                 "[filament][preheat][heater][cooldown]") {
    // The finished op's heat is still on: the waiting preheat read it as the
    // prior target, and must turn the heater off if it is withdrawn.
    set_print_state("standby");
    const auto ticket = heated_ticket(); // leaves the nozzle at 220/220
    set_spool("PETG");                   // which the next load must climb past
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());

    heater->get().op_succeeded(ticket);
    drain_all();
    client.clear_gcode_script_history();
    REQUIRE(heater->get().cancel());
    drain_all();
    CHECK(sent(std::string(kHeatGcode) + "0"));
}

TEST_CASE_METHOD(HeaterFixture, "A replacement that runs on the replaced heat keeps it, then cools",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    set_spool("PETG");
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());
    // A target below the load's wait lands and is reached: not enough for the
    // load, enough for a purge, which runs at the temperature set.
    const auto petg = helix::ui::preheat_material_for_slot(-1);
    REQUIRE(petg.has_value());
    feed_nozzle(petg->temp_c - 10, petg->temp_c - 10);
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    helix::ui::FilamentRunoutHandler handler(api.get()); // another surface
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_purge(handler);
    drain_all();
    REQUIRE_FALSE(heater->get().pending());
    REQUIRE(count_starting("PURGE") == 1);
    CHECK_FALSE(sent(std::string(kHeatGcode) + "0")); // not taken away mid-purge

    CHECK(wait_until(
        [&cd]() {
            helix::ui::UpdateQueue::instance().drain_until_empty();
            return cd.has_pending_timer();
        },
        3000));
}

TEST_CASE_METHOD(HeaterFixture, "A tile load that heated cools down after its macro finishes",
                 "[filament][preheat][heater][cooldown][tile]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    feed_nozzle(220, 220);
    REQUIRE(sent("LOAD_FILAMENT"));

    REQUIRE(wait_until(
        [&cd]() {
            helix::ui::UpdateQueue::instance().drain_until_empty();
            return cd.has_pending_timer();
        },
        3000));
}

TEST_CASE_METHOD(HeaterFixture, "A backend op's end on the action feed schedules the cooldown",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    lv_subject_t* action = helix::AmsState::instance().get_ams_action_subject();
    lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));
    const auto ticket = heated_ticket();

    SECTION("busy, then IDLE") {
        heater->get().watch_backend_completion(ticket);
        drain_all();
        CHECK_FALSE(cd.has_pending_timer()); // IDLE at dispatch is not the end

        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));
        drain_all();
        CHECK(cd.has_pending_timer());
    }
    SECTION("busy, then ERROR leaves the heater for the recovery") {
        heater->get().watch_backend_completion(ticket);
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::ERROR));
        drain_all();
        CHECK_FALSE(cd.has_pending_timer());
    }
    SECTION("a new request does not drop the watch of an op in flight") {
        heater->get().watch_backend_completion(ticket);
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
        drain_all();

        helix::ui::PreheatRequest other;
        other.op = helix::ui::FilamentHeatOp::Extrude;
        other.run = [](helix::ui::FilamentOpTicket) {};
        REQUIRE(heater->get().run_after_preheat(std::move(other)) ==
                FilamentOpHeater::Outcome::Ran);

        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));
        drain_all();
        CHECK(cd.has_pending_timer());
    }
    SECTION("a watch past its deadline ignores a later, unrelated op") {
        heater->get().watch_backend_completion(ticket);
        drain_all();
        fake_now += FilamentOpHeater::kBackendWatchTimeout + std::chrono::seconds(1);
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));
        drain_all();
        CHECK_FALSE(cd.has_pending_timer());
    }
    SECTION("a ticket the heater did not heat for is not watched") {
        heater->get().watch_backend_completion(0);
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));
        drain_all();
        // `ticket` stays unwatched too: only the 0 above was armed.
        CHECK_FALSE(cd.has_pending_timer());
    }
}

// ============================================================================
// Ending a preheat that never completes
// ============================================================================

TEST_CASE_METHOD(HeaterFixture, "A print started during an idle preheat abandons it",
                 "[filament][preheat][heater][print_guard]") {
    set_print_state("standby");
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());

    client.clear_gcode_script_history();
    set_print_state("printing");
    CHECK_FALSE(heater->get().pending());
    CHECK_FALSE(sent(std::string(kHeatGcode) + "0")); // the print owns the heater now

    feed_nozzle(220, 220); // PRINT_START heats the nozzle
    CHECK_FALSE(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "A print started during an idle purge preheat abandons it",
                 "[filament][preheat][heater][print_guard]") {
    set_print_state("standby");
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_purge(tile);
    drain_all();
    REQUIRE(heater->get().pending());

    set_print_state("printing");
    CHECK_FALSE(heater->get().pending());
    feed_nozzle(220, 220);
    CHECK_FALSE(sent("PURGE"));
}

TEST_CASE_METHOD(HeaterFixture, "A reload requested during a pause is not abandoned by the job",
                 "[filament][preheat][heater][print_guard][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0); // the paused print let the nozzle cool

    helix::ui::FilamentRunoutHandler handler(api.get());
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    drain_all();
    REQUIRE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "A target that never lands times the preheat out",
                 "[filament][preheat][heater][timeout]") {
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());

    fire_deadline_timer(); // within the window: still waiting
    REQUIRE(heater->get().pending());

    client.clear_gcode_script_history();
    fake_now += FilamentOpHeater::kTargetLandTimeout + std::chrono::seconds(1);
    fire_deadline_timer();
    CHECK_FALSE(heater->get().pending());
    // The heat it sent is taken back: the heater was off before the request.
    CHECK(sent(std::string(kHeatGcode) + "0"));

    feed_nozzle(220, 220);
    CHECK_FALSE(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "A nozzle that never gets hot times the preheat out",
                 "[filament][preheat][heater][timeout]") {
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    feed_nozzle(120, 220); // the target landed; the heater plateaus
    REQUIRE(heater->get().pending());

    fake_now += FilamentOpHeater::kTargetLandTimeout + std::chrono::seconds(1);
    fire_deadline_timer();
    REQUIRE(heater->get().pending()); // a seen target is not the short timeout

    client.clear_gcode_script_history();
    fake_now += FilamentOpHeater::kPreheatTimeout;
    fire_deadline_timer();
    CHECK_FALSE(heater->get().pending());
    CHECK(sent(std::string(kHeatGcode) + "0"));
}

TEST_CASE_METHOD(HeaterFixture, "Never runs below the extrusion floor",
                 "[filament][preheat][heater][floor]") {
    // A printer whose min_extrude_temp sits above the default load temperature:
    // the wait is the floor itself, and the margin must not undercut it.
    SafetyLimits limits = api->get_safety_limits();
    limits.min_extrude_temp_celsius = 230.0;
    api->set_safety_limits(limits);
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());

    feed_nozzle(227, 230);
    CHECK(heater->get().pending());
    CHECK_FALSE(sent("LOAD_FILAMENT"));

    feed_nozzle(230, 230);
    CHECK(sent("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "Another surface's different op replaces a pending preheat",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    helix::ui::FilamentRunoutHandler handler(api.get());
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_unload(handler);
    drain_all();
    REQUIRE(heater->get().pending());
    CHECK_FALSE(sent(std::string(kHeatGcode) + "0"));

    feed_nozzle(220, 220);
    REQUIRE(sent("UNLOAD_FILAMENT"));
    CHECK(count_starting("LOAD_FILAMENT") == 0); // the replaced load never runs
}

TEST_CASE_METHOD(HeaterFixture, "A surface waiting on the op hears when its preheat ends unrun",
                 "[filament][preheat][heater][cancel]") {
    // Bed drying moves on only once its unload finishes; it must learn that the
    // unload is not coming, or the flow stalls with no word.
    feed_nozzle(25, 0);
    int ended = 0;
    helix::ui::FilamentOpSurface surface;
    surface.log_tag = "[Test]";
    surface.on_preheat_ended = [&ended]() { ++ended; };

    SECTION("the user withdraws it") {
        helix::ui::execute_filament_unload(nullptr, -1, true, surface);
        drain_all();
        REQUIRE(heater->get().pending());
        REQUIRE(heater->get().cancel());
    }
    SECTION("another op replaces it") {
        helix::ui::execute_filament_unload(nullptr, -1, true, surface);
        drain_all();
        FilamentSensorWidgetTestAccess::dispatch_load(tile);
    }
    SECTION("it times out") {
        helix::ui::execute_filament_unload(nullptr, -1, true, surface);
        drain_all();
        fake_now += FilamentOpHeater::kTargetLandTimeout + std::chrono::seconds(1);
        fire_deadline_timer();
    }
    drain_all();
    CHECK(ended == 1);
}

TEST_CASE_METHOD(HeaterFixture, "A progress report that starts another op leaves the heater sane",
                 "[filament][preheat][heater]") {
    feed_nozzle(25, 0);
    int first_ran = 0;
    int second_ran = 0;
    helix::ui::PreheatRequest first;
    first.op = helix::ui::FilamentHeatOp::Load;
    first.run = [&first_ran](helix::ui::FilamentOpTicket) { ++first_ran; };
    first.on_progress = [this, &second_ran](const helix::ui::PreheatProgress& p) {
        if (p.phase != helix::ui::PreheatPhase::Reached) {
            return;
        }
        helix::ui::PreheatRequest second;
        second.op = helix::ui::FilamentHeatOp::Purge;
        second.run = [&second_ran](helix::ui::FilamentOpTicket) { ++second_ran; };
        heater->get().run_after_preheat(std::move(second));
    };
    REQUIRE(heater->get().run_after_preheat(std::move(first)) ==
            FilamentOpHeater::Outcome::Heating);

    feed_nozzle(220, 220);
    CHECK(first_ran == 1);
    CHECK(second_ran == 1);
    CHECK_FALSE(heater->get().pending());
}

// ============================================================================
// The deferred dispatch
// ============================================================================

namespace {

/// A filament system that records the loads it is handed.
class LoadRecordingBackend : public helix::AmsBackendMock {
  public:
    LoadRecordingBackend() : helix::AmsBackendMock(4) {}
    helix::AmsError load_filament(int slot_index) override {
        loads.push_back(slot_index);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError change_tool(int tool) override {
        loads.push_back(tool);
        return helix::AmsErrorHelper::success();
    }
    std::vector<int> loads;
};

} // namespace

TEST_CASE_METHOD(HeaterFixture, "A load that waited re-plans against the backend there is now",
                 "[filament][preheat][heater][replan]") {
    // A filament system when the load was asked for; gone by the time the nozzle
    // is hot. The load goes to the LOAD_FILAMENT macro instead of a freed backend.
    feed_nozzle(25, 0);
    auto owned = std::make_unique<LoadRecordingBackend>();
    LoadRecordingBackend* backend = owned.get();
    helix::AmsState::instance().set_backend(std::move(owned));

    helix::ui::execute_filament_load(backend, 1, "[Test]");
    drain_all();
    REQUIRE(heater->get().pending());
    REQUIRE(backend->loads.empty());

    helix::AmsState::instance().clear_backends();
    feed_nozzle(220, 220);
    drain_all();
    CHECK(sent("LOAD_FILAMENT"));
}

// ============================================================================
// A guarded surface closed mid-preheat
// ============================================================================

namespace {

/// Callbacks the sidebar's lifetime guard dropped since the last call.
uint64_t sidebar_skips() {
    uint64_t n = 0;
    for (const auto& e : helix::async_lifetime::take_snapshot().entries) {
        if (e.tag == "AmsOperationSidebar::load_macro") {
            n += e.count;
        }
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(
    HeaterFixture,
    "A sidebar closed mid-preheat: its macro-tier load still runs, with no hook into it",
    "[filament][preheat][heater][threading]") {
    // No filament system: the sidebar's load is the LOAD_FILAMENT macro, which
    // takes no parameters, so nothing prompts.
    nlohmann::json config;
    config["gcode_macro LOAD_FILAMENT"]["gcode"] = "G1 E50 F300";
    helix::MacroParamCache::instance().populate_from_configfile(config, {"LOAD_FILAMENT"});
    feed_nozzle(25, 0);
    sidebar_skips();

    auto sidebar = std::make_unique<helix::ui::AmsOperationSidebar>(state);
    sidebar->handle_load_with_preheat(0);
    drain_all();
    REQUIRE(heater->get().pending());

    SECTION("closed: the op runs, and only its hooks into the sidebar are dropped") {
        sidebar.reset();
        feed_nozzle(220, 220);
        drain_all();
        CHECK(sent("LOAD_FILAMENT"));
        // The macro succeeding reaches no hook: the sidebar sets none on this
        // tier, and the dispatch is not routed through its guard.
        CHECK(sidebar_skips() == 0);
    }
    SECTION("open: the same dispatch goes out") {
        feed_nozzle(220, 220);
        drain_all();
        CHECK(sidebar_skips() == 0);
        CHECK(sent("LOAD_FILAMENT"));
    }
}

TEST_CASE_METHOD(HeaterFixture, "A closed surface's deferred macro dispatch raises no prompt",
                 "[filament][preheat][heater][threading]") {
    // LOAD_FILAMENT takes parameters, and the sidebar asks for them. Once the
    // AMS panel is closed, a prompt would land on whatever screen is up.
    nlohmann::json config;
    // LENGTH is a parameter nothing prefills, so a live sidebar has to ask.
    config["gcode_macro LOAD_FILAMENT"]["gcode"] =
        "{% set t = params.TEMP|default(220)|int %}\n{% set l = params.LENGTH|default(50)|int "
        "%}\nM109 S{t}\nG1 E{l} F300";
    helix::MacroParamCache::instance().populate_from_configfile(config, {"LOAD_FILAMENT"});
    int prompts = 0;
    helix::ui::set_filament_param_prompter([&prompts](const std::string&,
                                                      const helix::CachedMacroInfo&,
                                                      const std::map<std::string, std::string>&,
                                                      helix::MacroExecuteCallback on_execute) {
        ++prompts;
        on_execute({});
    });
    feed_nozzle(25, 0);

    set_spool("PETG");
    auto sidebar = std::make_unique<helix::ui::AmsOperationSidebar>(state);
    sidebar->handle_load_with_preheat(0);
    drain_all();
    REQUIRE(heater->get().pending());

    SECTION("closed: the macro goes out with its prefill, unprompted") {
        sidebar.reset();
        feed_nozzle(280, 280);
        drain_all();
        CHECK(prompts == 0);
        REQUIRE(count_starting("LOAD_FILAMENT") == 1);
        // The spool's material, not the macro's own default of 220.
        for (const auto& script : client.gcode_script_history()) {
            if (script.rfind("LOAD_FILAMENT", 0) == 0) {
                CHECK(script.find("TEMP=") != std::string::npos);
                CHECK(script.find("TEMP=220") == std::string::npos);
            }
        }
    }
    SECTION("open: the user is asked, as on any tap") {
        feed_nozzle(280, 280);
        drain_all();
        CHECK(prompts == 1);
    }
    helix::ui::set_filament_param_prompter({});
}

// ============================================================================
// Which request is a re-tap
// ============================================================================

TEST_CASE_METHOD(HeaterFixture, "A load on another lane replaces the pending one",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 0);
    auto owned = std::make_unique<LoadRecordingBackend>();
    LoadRecordingBackend* backend = owned.get();
    helix::AmsState::instance().set_backend(std::move(owned));

    helix::ui::execute_filament_load(backend, 2, "[FilamentSensorWidget]");
    drain_all();
    REQUIRE(heater->get().pending());
    helix::ui::execute_filament_load(backend, 3, "[FilamentSensorWidget]");
    drain_all();
    REQUIRE(heater->get().pending()); // replaced, not withdrawn

    feed_nozzle(280, 280); // the mock's lanes name a hot material
    CHECK(backend->loads == std::vector<int>{3});
}

TEST_CASE_METHOD(HeaterFixture, "A runout reload replaces a tile load waiting during the pause",
                 "[filament][preheat][heater][cancel][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());

    helix::ui::FilamentRunoutHandler handler(api.get());
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    drain_all();
    REQUIRE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK(count_starting("LOAD_FILAMENT") == 1);
}

TEST_CASE_METHOD(HeaterFixture, "A second surface's unload proceeds, and the first hears it ended",
                 "[filament][preheat][heater][cancel]") {
    // Bed drying asking for an unload while another surface's waits.
    feed_nozzle(25, 0);
    int first_ended = 0;
    int second_ended = 0;
    helix::ui::FilamentOpSurface first;
    first.log_tag = "[First]";
    first.on_preheat_ended = [&first_ended]() { ++first_ended; };
    helix::ui::FilamentOpSurface second;
    second.log_tag = "[Second]";
    second.on_preheat_ended = [&second_ended]() { ++second_ended; };

    helix::ui::execute_filament_unload(nullptr, -1, true, first);
    drain_all();
    helix::ui::execute_filament_unload(nullptr, -1, true, second);
    drain_all();
    CHECK(first_ended == 1);
    REQUIRE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK(count_starting("UNLOAD_FILAMENT") == 1);
    CHECK(second_ended == 0);
}

TEST_CASE_METHOD(HeaterFixture, "A re-tap's own request hears that it was not run",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 0);
    int ended = 0;
    helix::ui::FilamentOpSurface surface;
    surface.log_tag = "[Test]";
    surface.on_preheat_ended = [&ended]() { ++ended; };

    helix::ui::execute_filament_unload(nullptr, -1, true, surface);
    drain_all();
    helix::ui::execute_filament_unload(nullptr, -1, true, surface);
    drain_all();

    CHECK_FALSE(heater->get().pending());
    CHECK(ended == 2); // the withdrawn request, and the tap that withdrew it
}

TEST_CASE_METHOD(HeaterFixture, "A request that cannot take the heater is told so",
                 "[filament][preheat][heater][cancel]") {
    // The replaced request's surface starts another op from its report; that
    // one holds the heater, and the newcomer is not run.
    feed_nozzle(25, 0);
    int third_ran = 0;
    helix::ui::PreheatRequest first;
    first.op = helix::ui::FilamentHeatOp::Load;
    first.owner = "first";
    first.run = [](helix::ui::FilamentOpTicket) {};
    first.on_progress = [this, &third_ran](const helix::ui::PreheatProgress& p) {
        if (p.phase != helix::ui::PreheatPhase::Cancelled) {
            return;
        }
        helix::ui::PreheatRequest third;
        third.op = helix::ui::FilamentHeatOp::Purge;
        third.run = [&third_ran](helix::ui::FilamentOpTicket) { ++third_ran; };
        heater->get().run_after_preheat(std::move(third));
    };
    REQUIRE(heater->get().run_after_preheat(std::move(first)) ==
            FilamentOpHeater::Outcome::Heating);

    bool second_ran = false;
    helix::ui::PreheatRequest second;
    second.op = helix::ui::FilamentHeatOp::Unload;
    second.owner = "second";
    second.run = [&second_ran](helix::ui::FilamentOpTicket) { second_ran = true; };
    CHECK(heater->get().run_after_preheat(std::move(second)) ==
          FilamentOpHeater::Outcome::Cancelled);
    REQUIRE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK(third_ran == 1);
    CHECK_FALSE(second_ran);
}

// ============================================================================
// Heat that a preheat ending unrun leaves behind
// ============================================================================

TEST_CASE_METHOD(HeaterFixture, "A replace that skips its preheat takes the heat over, then cools",
                 "[filament][preheat][heater][cancel]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    // The replacing op skips its preheat on the same nozzle: the load's heat is
    // its to cool down after, not to take away while it runs.
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(true);
    helix::ui::FilamentRunoutHandler handler(api.get()); // another surface
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_unload(handler);
    drain_all();

    CHECK_FALSE(heater->get().pending());
    REQUIRE(sent("UNLOAD_FILAMENT"));
    CHECK_FALSE(sent(std::string(kHeatGcode) + "0"));
    CHECK(wait_until(
        [&cd]() {
            helix::ui::UpdateQueue::instance().drain_until_empty();
            return cd.has_pending_timer();
        },
        3000));
}

// ============================================================================
// The cooldown and the backend watch
// ============================================================================

TEST_CASE_METHOD(HeaterFixture, "An op that runs at once cancels the previous op's cooldown",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    cd.schedule();
    drain_all();
    REQUIRE(cd.has_pending_timer());

    feed_nozzle(215, 215);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(sent("LOAD_FILAMENT"));
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "An op on a nozzle owed a cooldown takes the cooldown over",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    heater->get().op_succeeded(heated_ticket());
    drain_all();
    REQUIRE(cd.has_pending_timer());

    helix::ui::FilamentOpTicket ticket = 0;
    helix::ui::PreheatRequest req;
    req.op = helix::ui::FilamentHeatOp::Purge;
    req.run = [&ticket](helix::ui::FilamentOpTicket t) { ticket = t; };
    REQUIRE(heater->get().run_after_preheat(std::move(req)) == FilamentOpHeater::Outcome::Ran);
    drain_all();
    CHECK_FALSE(cd.has_pending_timer()); // not mid-op

    heater->get().op_succeeded(ticket);
    drain_all();
    CHECK(cd.has_pending_timer()); // and still owed after
}

TEST_CASE_METHOD(HeaterFixture, "A purge right after a heated load still cools down after",
                 "[filament][preheat][heater][cooldown]") {
    // Load, then purge inside the cooldown delay: the purge takes over the heat
    // the load's cooldown was owed for, and is cooled down after in its place.
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    const auto cooldown_pending = [this, &cd]() {
        return wait_until(
            [&cd]() {
                helix::ui::UpdateQueue::instance().drain_until_empty();
                return cd.has_pending_timer();
            },
            3000);
    };
    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    feed_nozzle(220, 220);
    REQUIRE(sent("LOAD_FILAMENT"));
    REQUIRE(cooldown_pending());

    FilamentSensorWidgetTestAccess::dispatch_purge(tile);
    drain_all();
    REQUIRE(count_starting("PURGE") == 1);
    REQUIRE_FALSE(heater->get().pending());
    // Cancelled so it cannot cool the nozzle mid-purge, then owed again after.
    CHECK(cooldown_pending());
}

TEST_CASE_METHOD(HeaterFixture, "An op finishing while a preheat waits schedules no cooldown",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    const auto ticket = heated_ticket();

    feed_nozzle(25, 0);
    FilamentSensorWidgetTestAccess::dispatch_load(tile);
    drain_all();
    REQUIRE(heater->get().pending());

    heater->get().op_succeeded(ticket);
    drain_all();
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "A backend watch does not take a busy from before its dispatch",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    lv_subject_t* action = helix::AmsState::instance().get_ams_action_subject();
    const auto ticket = heated_ticket();

    // The previous op is still busy when this one is dispatched.
    lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
    drain_all();
    heater->get().watch_backend_completion(ticket);
    drain_all();

    lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE)); // the previous op ends
    drain_all();
    CHECK_FALSE(cd.has_pending_timer());

    lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING)); // this one
    drain_all();
    lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));
    drain_all();
    CHECK(cd.has_pending_timer());
}

// ============================================================================
// A lane mapped to another extruder
// ============================================================================

namespace {

/// Lane 1 feeds tool 1.
class MappedLaneBackend : public LoadRecordingBackend {
  public:
    helix::SlotInfo get_slot_info(int slot_index) const override {
        helix::SlotInfo info = LoadRecordingBackend::get_slot_info(slot_index);
        if (slot_index == 1) {
            info.mapped_tool = 1;
        }
        return info;
    }
};

} // namespace

TEST_CASE_METHOD(HeaterFixture, "A lane on extruder1 heats and cools extruder1, not the active one",
                 "[filament][preheat][heater][multi_extruder]") {
    state.temperature_state().init_extruders({"extruder", "extruder1"});
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder", "extruder1"});
    helix::ToolState::instance().init_tools(hardware);
    REQUIRE(helix::ToolState::instance().extruder_name_for_tool(1) == "extruder1");
    set_print_state("standby");
    state.update_from_status({{"extruder", {{"temperature", 200.0}, {"target", 200.0}}},
                              {"extruder1", {{"temperature", 25.0}, {"target", 0.0}}}});
    drain_all();

    auto owned = std::make_unique<MappedLaneBackend>();
    MappedLaneBackend* backend = owned.get();
    helix::AmsState::instance().set_backend(std::move(owned));
    client.clear_gcode_script_history();

    helix::ui::execute_filament_load(backend, 1, "[Test]");
    drain_all();
    REQUIRE(heater->get().pending());
    CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET="));
    CHECK_FALSE(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET="));

    SECTION("it loads once extruder1 is hot, and cools extruder1 after") {
        state.update_from_status({{"extruder1", {{"temperature", 220.0}, {"target", 220.0}}}});
        drain_all();
        REQUIRE(backend->loads == std::vector<int>{1});

        lv_subject_t* action = helix::AmsState::instance().get_ams_action_subject();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(helix::AmsAction::IDLE));
        drain_all();
        auto& cd = PostOpCooldownManager::instance();
        REQUIRE(cd.has_pending_timer());
        CHECK(cd.pending_heaters() == std::vector<std::string>{"extruder1"});

        // Let it fire: extruder1 goes off, extruder keeps the user's target.
        // The cooldown reads the app's printer state, so it sees the targets there.
        helix::PrinterState& app = get_printer_state();
        app.temperature_state().init_extruders({"extruder", "extruder1"});
        app.update_from_status(
            {{"extruder", {{"target", 200.0}}}, {"extruder1", {{"target", 220.0}}}});
        client.clear_gcode_script_history();
        auto* cfg = helix::Config::get_instance();
        const std::string delay_key = cfg->df() + "filament/cooldown_delay_seconds";
        const int prev_delay = cfg->get<int>(delay_key, 120);
        cfg->set<int>(delay_key, 1);
        cd.schedule("extruder1");
        drain_all();
        process_lvgl(1200);
        cfg->set<int>(delay_key, prev_delay);
        app.temperature_state().init_extruders({"extruder"});
        CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0"));
        CHECK_FALSE(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"));
    }
    SECTION("a request on the active extruder puts extruder1 back") {
        client.clear_gcode_script_history();
        FilamentSensorWidgetTestAccess::dispatch_purge(tile); // runs on extruder at 200/200
        drain_all();
        CHECK(sent("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=0"));
        CHECK_FALSE(sent("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"));
    }
    SECTION("extruder1 going away mid-preheat abandons it") {
        state.temperature_state().init_extruders({"extruder"});
        fire_deadline_timer();
        CHECK_FALSE(heater->get().pending());
    }

    helix::PrinterDiscovery single;
    single.parse_objects(nlohmann::json{"extruder"});
    helix::ToolState::instance().init_tools(single);
}

// ============================================================================
// A CFS bypass unload
// ============================================================================

namespace {

/// CFS, running, with the action script captured instead of sent.
class CfsCapture : public helix::printer::AmsBackendCfs {
  public:
    CfsCapture() : AmsBackendCfs(nullptr, nullptr) {
        running_ = true;
    }
    bool toolhead_homed() const override {
        return true;
    }
    std::vector<std::string> dispatched;

  private:
    helix::AmsError dispatch_action_script(std::string gcode) override {
        dispatched.push_back(std::move(gcode));
        return helix::AmsErrorHelper::success();
    }
};

} // namespace

TEST_CASE_METHOD(HeaterFixture, "A CFS bypass unload on a cold nozzle preheats first",
                 "[filament][preheat][heater][cfs]") {
    // The bypass unload ends in a raw extruder retract, which Klipper refuses on
    // a cold nozzle: nothing may skip the preheat in front of it.
    feed_nozzle(25, 0);
    auto owned = std::make_unique<CfsCapture>();
    CfsCapture* cfs = owned.get();
    helix::AmsState::instance().set_backend(std::move(owned));

    helix::ui::execute_filament_unload(cfs, helix::ui::kExternalSpoolSlot,
                                       /*target_is_loaded=*/true, "[Test]");
    drain_all();

    CHECK(heater->get().pending());
    CHECK(sent(kHeatGcode));
    CHECK(cfs->dispatched.empty());
}

// ============================================================================
// One surface, one op at a time
// ============================================================================

TEST_CASE_METHOD(HeaterFixture, "A second op from the same dialog waits its turn: it is refused",
                 "[filament][preheat][heater][cancel][runout]") {
    // The runout dialog stays open after Load so the user can purge next. A
    // Purge tapped while the Load heats must not drop the Load.
    set_print_state("paused");
    feed_nozzle(25, 0);
    helix::ui::FilamentRunoutHandler handler(api.get());
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    drain_all();
    REQUIRE(heater->get().pending());

    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_purge(handler);
    drain_all();
    REQUIRE(heater->get().pending()); // the load still waits

    feed_nozzle(220, 220);
    CHECK(count_starting("LOAD_FILAMENT") == 1);
    CHECK_FALSE(sent("PURGE"));
}

TEST_CASE_METHOD(HeaterFixture, "A refused request hears that it was not run",
                 "[filament][preheat][heater][cancel]") {
    feed_nozzle(25, 0);
    int ended = 0;
    helix::ui::FilamentOpSurface surface;
    surface.log_tag = "[Test]";
    surface.on_preheat_ended = [&ended]() { ++ended; };
    helix::ui::execute_filament_load(nullptr, -1, surface);
    drain_all();
    REQUIRE(heater->get().pending());

    helix::ui::execute_filament_unload(nullptr, -1, true, surface);
    drain_all();
    CHECK(heater->get().pending());
    CHECK(ended == 1); // the unload; the load is still coming

    feed_nozzle(220, 220);
    CHECK(count_starting("LOAD_FILAMENT") == 1);
    CHECK(count_starting("UNLOAD_FILAMENT") == 0);
}

// ============================================================================
// A paused print resuming under a pending preheat
// ============================================================================

TEST_CASE_METHOD(HeaterFixture,
                 "A reload requested during a pause is abandoned when the print resumes",
                 "[filament][preheat][heater][print_guard][runout]") {
    // The dialog stays open after Load, with Resume beside it. A load queued
    // into a resumed print would run mid-print.
    set_print_state("paused");
    feed_nozzle(25, 0);
    helix::ui::FilamentRunoutHandler handler(api.get());

    SECTION("load") {
        helix::ui::FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    }
    SECTION("unload") {
        helix::ui::FilamentRunoutHandlerTestAccess::dispatch_unload(handler);
    }
    SECTION("purge") {
        helix::ui::FilamentRunoutHandlerTestAccess::dispatch_purge(handler);
    }
    drain_all();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    set_print_state("printing");
    CHECK_FALSE(heater->get().pending());
    // The resumed print owns the heater: no TARGET=0 into a running print.
    CHECK_FALSE(sent(std::string(kHeatGcode) + "0"));

    feed_nozzle(220, 220);
    CHECK(count_starting("LOAD_FILAMENT") == 0);
    CHECK(count_starting("UNLOAD_FILAMENT") == 0);
    CHECK_FALSE(sent("PURGE"));
}

TEST_CASE_METHOD(HeaterFixture,
                 "A reload requested during a pause is abandoned when the print ends",
                 "[filament][preheat][heater][print_guard][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    helix::ui::FilamentRunoutHandler handler(api.get());
    helix::ui::FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    drain_all();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    set_print_state("cancelled");
    CHECK_FALSE(heater->get().pending());
    // The print no longer owns the heater, and it was off before the request.
    CHECK(sent(std::string(kHeatGcode) + "0"));

    feed_nozzle(220, 220);
    CHECK(count_starting("LOAD_FILAMENT") == 0);
}

TEST_CASE_METHOD(HeaterFixture, "A request made while printing is not dropped by the pause rule",
                 "[filament][preheat][heater][print_guard]") {
    // Only a pause ending drops a request; one made while the job already runs
    // (or is starting) belongs to that job, which never "leaves" a pause.
    const char* state = GENERATE("printing", "standby");
    CAPTURE(state);
    set_print_state("printing");
    if (std::string(state) == "standby") {
        // Preparing: the host-side start block, with print_stats still standby.
        set_print_state("standby");
        this->state.print_state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
        drain_all();
    }
    feed_nozzle(25, 0);
    bool ran = false;
    helix::ui::PreheatRequest req;
    req.op = helix::ui::FilamentHeatOp::Extrude;
    req.owner = "printing-test";
    req.run = [&ran](helix::ui::FilamentOpTicket) { ran = true; };
    REQUIRE(heater->get().run_after_preheat(std::move(req)) == FilamentOpHeater::Outcome::Heating);

    feed_nozzle(220, 220);
    CHECK(ran);
    this->state.print_state().set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
}
