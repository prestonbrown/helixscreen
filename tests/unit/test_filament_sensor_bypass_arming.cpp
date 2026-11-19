// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_sensor_bypass_arming.cpp
 * @brief Bypass⇄runout-sensor arming policy (FilamentSensorManager).
 *
 * Run with: ./build/bin/helix-tests "[bypass-arming]"
 *
 * When bypass engages, RUNOUT-role sensors the firmware holds disabled are
 * armed via SET_FILAMENT_SENSOR and restored on disengage. Policy lives
 * entirely in FilamentSensorManager (sensor abstraction layer); AmsState only
 * notifies the transition. The bypass spool's lane record is
 * test_bypass_lane_record.cpp.
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/log_capture.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace helix {

// Friend shim reaching the manager's private state — same idiom as
// RunoutScopeTestAccess in test_runout_empty_lane_scope.cpp (per-TU class to
// avoid an ODR clash).
class BypassArmingTestAccess {
  public:
    static void reset(FilamentSensorManager& mgr) {
        mgr.set_sync_mode(true);
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        mgr.entries_locked().clear();
        mgr.readings_.clear();
        mgr.lanes_.clear();
        mgr.known_.clear();
        mgr.bypass_armed_.clear();
        mgr.master_enabled_ = true;
        mgr.initial_status_received_ = false;
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }
};
} // namespace helix

namespace {
/// Fixture with the mock pair + API; the gcode wire is the client mock's
/// gcode_script_history().
class BypassArmingFixture : public HelixTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    FilamentSensorManager& mgr = FilamentSensorManager::instance();

    BypassArmingFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        BypassArmingTestAccess::reset(mgr);
        mgr.set_moonraker_api(api.get());
    }

    ~BypassArmingFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        BypassArmingTestAccess::reset(mgr);
        mgr.set_moonraker_api(nullptr);
    }

    /// Discover one switch sensor and give it a first status frame.
    void seed_toolhead_sensor(bool firmware_enabled) {
        discover_filament_sensors(mgr, {"filament_switch_sensor filament_sensor"});
        mgr.set_role("filament_switch_sensor filament_sensor",
                     static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        mgr.update_from_status(
            json{{"filament_switch_sensor filament_sensor",
                  {{"filament_detected", true}, {"enabled", firmware_enabled}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    std::vector<std::string> gcode_sent() {
        return client.gcode_script_history();
    }
};
} // namespace

TEST_CASE("bypass arming: engages firmware-disabled runout sensor with bare name",
          "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();

    auto sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    // SET_FILAMENT_SENSOR wants the bare name (post-section-prefix), which is
    // FilamentSensorConfig::sensor_name — the same form Creality's macros use.
    CHECK(sent[0] == "SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=1");
    CHECK(FilamentSensorManagerTestAccess::has_bypass_armed_sensors(fx.mgr));
}

TEST_CASE("bypass arming: firmware-enabled sensor is left alone", "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/true);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());
    CHECK_FALSE(FilamentSensorManagerTestAccess::has_bypass_armed_sensors(fx.mgr));
}

TEST_CASE("bypass arming: master-disabled monitoring refuses to arm", "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);
    fx.mgr.set_master_enabled(false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());
}

TEST_CASE("bypass arming: arm is idempotent, restore sends exactly one disable",
          "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(fx.gcode_sent().size() == 1);

    // Second engage notification (e.g. two backends transitioning): no re-send.
    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());

    // Disengage restores the pre-bypass firmware state exactly once.
    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(false);
    helix::ui::UpdateQueue::instance().drain();
    auto sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=0");
    CHECK_FALSE(FilamentSensorManagerTestAccess::has_bypass_armed_sensors(fx.mgr));

    // Disengage with nothing armed is silent.
    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(false);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());
}

TEST_CASE("bypass arming: re-arm after a real firmware disable echo", "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(fx.gcode_sent().size() == 1);

    // Someone else (a vendor macro) disabled the sensor again mid-bypass; the
    // next status frame reports it, and a subsequent engage edge re-arms.
    fx.mgr.update_from_status(json{{"filament_switch_sensor filament_sensor",
                                    {{"filament_detected", true}, {"enabled", false}}}});
    helix::ui::UpdateQueue::instance().drain();
    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(false); // restore path: nothing held armed
    helix::ui::UpdateQueue::instance().drain();

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    auto sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=1");
}

TEST_CASE("bypass arming: a wrapped SET_FILAMENT_SENSOR is bypassed for the renamed builtin",
          "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    // A [gcode_macro SET_FILAMENT_SENSOR] with rename_existing saves the state
    // as a user setting; the temporary arm and restore go to the builtin.
    REQUIRE(fx.api->hardware().parse_sensor_toggle_command(
        json{{"gcode_macro set_filament_sensor",
              {{"rename_existing", "_SET_FILAMENT_SENSOR"}, {"gcode", "..."}}}}));
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    auto sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "_SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=1");

    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(false);
    helix::ui::UpdateQueue::instance().drain();
    sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "_SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=0");
}

TEST_CASE_METHOD(BypassArmingFixture, "A sensor's first reading of empty is not logged as a runout",
                 "[filament_sensor][runout-log]") {
    const std::string key = "filament_switch_sensor filament_sensor";
    discover_filament_sensors(mgr, {key});
    mgr.set_role(key, static_cast<uint8_t>(FilamentSensorRole::RUNOUT));

    auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(8);
    sink->set_level(spdlog::level::warn);
    auto attached = std::make_unique<helix::ScopedLogSink>(sink);

    mgr.update_from_status(json{{key, {{"filament_detected", false}, {"enabled", true}}}});
    const bool first_reading_silent = sink->last_formatted().empty();

    mgr.update_from_status(json{{key, {{"filament_detected", true}, {"enabled", true}}}});
    mgr.update_from_status(json{{key, {{"filament_detected", false}, {"enabled", true}}}});
    const auto logged = sink->last_formatted();

    attached.reset();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(first_reading_silent);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].find("RUNOUT") != std::string::npos);
}
