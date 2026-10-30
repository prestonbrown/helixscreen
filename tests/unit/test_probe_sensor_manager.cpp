// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_probe_sensor_manager.cpp
 * @brief ProbeSensorManager on the SensorManager base: discovery from objects and
 *        config sections, the single-probe Z_PROBE default, status parsing, the
 *        per-type subscriptions, the z_offset seed and the persisted settings shape.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/golden_json.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "config.h"
#include "hardware_object_table.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "probe_sensor_manager.h"
#include "probe_sensor_types.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

namespace helix::sensors {
class ProbeSensorManagerTestAccess {
  public:
    static void reset(ProbeSensorManager& obj) {
        SensorManagerTestAccess::reconcile(obj, {});
        std::lock_guard<std::recursive_mutex> lock(obj.mutex());
        obj.readings_.clear();
        obj.type_override_ = ProbeSensorType::STANDARD;
        obj.set_sync_mode(true);
        if (obj.subjects_initialized()) {
            lv_subject_set_int(&obj.z_offset_, -1);
        }
    }

    /// Reading of the enabled, available Z_PROBE holder.
    static std::optional<ProbeSensorState> z_probe_reading(ProbeSensorManager& obj) {
        std::lock_guard<std::recursive_mutex> lock(obj.mutex());
        const auto* reading = obj.z_probe_reading_locked();
        return reading ? std::optional<ProbeSensorState>(*reading) : std::nullopt;
    }
};
} // namespace helix::sensors

namespace {

constexpr uint8_t kNone = static_cast<uint8_t>(ProbeSensorRole::NONE);
constexpr uint8_t kZProbe = static_cast<uint8_t>(ProbeSensorRole::Z_PROBE);

/// Objects as printer.objects.list reports them, config sections as configfile.config does.
PrinterDiscovery discovery_of(const std::vector<std::string>& objects,
                              const json& config = json::object()) {
    PrinterDiscovery hw;
    hw.parse_objects(json(objects));
    hw.parse_config_keys(config);
    return hw;
}

json& probe_slot() {
    Config* cfg = Config::get_instance();
    return cfg->get_json(cfg->df() + "probe_sensors");
}

class ProbeSensorTestFixture : public LVGLTestFixture {
  public:
    ProbeSensorTestFixture() : saved_slot_(probe_slot()) {
        probe_slot() = json::object();
        mgr().init_subjects();
        ProbeSensorManagerTestAccess::reset(mgr());
    }

    ~ProbeSensorTestFixture() override {
        ProbeSensorManagerTestAccess::reset(mgr());
        probe_slot() = saved_slot_;
    }

  protected:
    ProbeSensorManager& mgr() {
        return ProbeSensorManager::instance();
    }

    void discover(const std::vector<std::string>& objects, const json& config = json::object()) {
        mgr().discover(discovery_of(objects, config));
    }

    // Two specific (non-STANDARD) types, so no alias dedup applies and no single-probe
    // default is assigned.
    void discover_test_sensors() {
        discover({"bltouch", "smart_effector"});
    }

    void set_role(const std::string& klipper_name, ProbeSensorRole role) {
        mgr().set_role(klipper_name, static_cast<uint8_t>(role));
    }

    uint8_t role_of(const std::string& klipper_name) {
        SensorEntry e;
        REQUIRE(mgr().find(klipper_name, e));
        return e.role;
    }

    std::optional<ProbeSensorState> z_state() {
        return ProbeSensorManagerTestAccess::z_probe_reading(mgr());
    }

    int z_offset_subject() {
        return lv_subject_get_int(mgr().get_probe_z_offset_subject());
    }

    void update_sensor_state(const std::string& klipper_name, float last_z_result) {
        json status;
        status[klipper_name]["last_z_result"] = last_z_result;
        mgr().update_from_status(status);
    }

  private:
    json saved_slot_;
};

} // namespace

// ============================================================================
// Type Helper Tests (probe_sensor_types.h)
// ============================================================================

TEST_CASE("ProbeSensorTypes - role string conversion", "[probe][types]") {
    SECTION("probe_role_to_string") {
        REQUIRE(probe_role_to_string(ProbeSensorRole::NONE) == "none");
        REQUIRE(probe_role_to_string(ProbeSensorRole::Z_PROBE) == "z_probe");
    }

    SECTION("probe_role_from_string") {
        REQUIRE(probe_role_from_string("none") == ProbeSensorRole::NONE);
        REQUIRE(probe_role_from_string("z_probe") == ProbeSensorRole::Z_PROBE);
        REQUIRE(probe_role_from_string("invalid") == ProbeSensorRole::NONE);
        REQUIRE(probe_role_from_string("") == ProbeSensorRole::NONE);
    }

    SECTION("probe_role_to_display_string") {
        REQUIRE(probe_role_to_display_string(ProbeSensorRole::NONE) == "Unassigned");
        REQUIRE(probe_role_to_display_string(ProbeSensorRole::Z_PROBE) == "Z Probe");
    }
}

TEST_CASE("ProbeSensorTypes - type string conversion", "[probe][types]") {
    SECTION("probe_type_to_string") {
        REQUIRE(probe_type_to_string(ProbeSensorType::STANDARD) == "standard");
        REQUIRE(probe_type_to_string(ProbeSensorType::BLTOUCH) == "bltouch");
        REQUIRE(probe_type_to_string(ProbeSensorType::SMART_EFFECTOR) == "smart_effector");
        REQUIRE(probe_type_to_string(ProbeSensorType::EDDY_CURRENT) == "eddy_current");
    }

    SECTION("probe_type_from_string") {
        REQUIRE(probe_type_from_string("standard") == ProbeSensorType::STANDARD);
        REQUIRE(probe_type_from_string("bltouch") == ProbeSensorType::BLTOUCH);
        REQUIRE(probe_type_from_string("smart_effector") == ProbeSensorType::SMART_EFFECTOR);
        REQUIRE(probe_type_from_string("eddy_current") == ProbeSensorType::EDDY_CURRENT);
        REQUIRE(probe_type_from_string("invalid") == ProbeSensorType::STANDARD);
        REQUIRE(probe_type_from_string("") == ProbeSensorType::STANDARD);
    }
}

TEST_CASE("Probe type string conversions - new types", "[probe][types]") {
    SECTION("cartographer type to string") {
        REQUIRE(probe_type_to_string(ProbeSensorType::CARTOGRAPHER) == "cartographer");
    }
    SECTION("beacon type to string") {
        REQUIRE(probe_type_to_string(ProbeSensorType::BEACON) == "beacon");
    }
    SECTION("tap type to string") {
        REQUIRE(probe_type_to_string(ProbeSensorType::TAP) == "tap");
    }
    SECTION("klicky type to string") {
        REQUIRE(probe_type_to_string(ProbeSensorType::KLICKY) == "klicky");
    }
    SECTION("cartographer from string") {
        REQUIRE(probe_type_from_string("cartographer") == ProbeSensorType::CARTOGRAPHER);
    }
    SECTION("beacon from string") {
        REQUIRE(probe_type_from_string("beacon") == ProbeSensorType::BEACON);
    }
    SECTION("tap from string") {
        REQUIRE(probe_type_from_string("tap") == ProbeSensorType::TAP);
    }
    SECTION("klicky from string") {
        REQUIRE(probe_type_from_string("klicky") == ProbeSensorType::KLICKY);
    }
    SECTION("loadcell type to string") {
        REQUIRE(probe_type_to_string(ProbeSensorType::LOADCELL) == "loadcell");
    }
    SECTION("loadcell from string") {
        REQUIRE(probe_type_from_string("loadcell") == ProbeSensorType::LOADCELL);
    }
    SECTION("loadcell display string") {
        REQUIRE(probe_type_to_display_string(ProbeSensorType::LOADCELL) == "Load Cell");
    }
}

// ============================================================================
// Sensor Discovery Tests
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - discovery", "[probe][discovery]") {
    SECTION("Discovers standard probe and makes it the Z probe") {
        discover({"probe"});

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs.size() == 1);
        REQUIRE(configs[0].klipper_name == "probe");
        REQUIRE(configs[0].sensor_name == "probe");
        REQUIRE(configs[0].type == ProbeSensorType::STANDARD);
        REQUIRE(configs[0].enabled == true);
        REQUIRE(configs[0].role == ProbeSensorRole::Z_PROBE);
    }

    SECTION("Discovers BLTouch probe") {
        discover({"bltouch"});

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "bltouch");
        REQUIRE(configs[0].sensor_name == "bltouch");
        REQUIRE(configs[0].type == ProbeSensorType::BLTOUCH);
    }

    SECTION("Discovers Smart Effector probe") {
        discover({"smart_effector"});

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "smart_effector");
        REQUIRE(configs[0].sensor_name == "smart_effector");
        REQUIRE(configs[0].type == ProbeSensorType::SMART_EFFECTOR);
    }

    SECTION("Discovers Eddy Current probe with name parameter") {
        discover({"probe_eddy_current btt"});

        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "probe_eddy_current btt");
        REQUIRE(configs[0].sensor_name == "btt");
        REQUIRE(configs[0].type == ProbeSensorType::EDDY_CURRENT);
    }

    SECTION("Discovers multiple probe types and assigns no default role") {
        discover({"bltouch", "smart_effector", "probe_eddy_current scanner"});

        REQUIRE(mgr().sensor_count() == 3);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].type == ProbeSensorType::BLTOUCH);
        REQUIRE(configs[1].type == ProbeSensorType::SMART_EFFECTOR);
        REQUIRE(configs[2].type == ProbeSensorType::EDDY_CURRENT);
        REQUIRE(configs[2].sensor_name == "scanner");
        for (const auto& c : configs) {
            REQUIRE(c.role == ProbeSensorRole::NONE);
        }
    }

    SECTION("Standard probe removed when specific type present") {
        discover({"probe", "bltouch"});

        REQUIRE(mgr().sensor_count() == 1);
        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].type == ProbeSensorType::BLTOUCH);
    }

    SECTION("Ignores unrelated objects") {
        discover(
            {"probe", "filament_switch_sensor runout", "temperature_sensor chamber", "extruder"});

        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].klipper_name == "probe");
    }

    SECTION("Empty sensor list clears previous sensors") {
        discover_test_sensors();
        REQUIRE(mgr().sensor_count() == 2);

        discover({});
        REQUIRE(mgr().sensor_count() == 0);
    }

    SECTION("Re-discovery replaces sensor list") {
        discover({"probe"});
        REQUIRE(mgr().get_sensors()[0].klipper_name == "probe");

        discover({"bltouch"});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].klipper_name == "bltouch");
    }

    SECTION("Sensor count subject is updated") {
        lv_subject_t* count_subject = mgr().count_subject();
        REQUIRE(lv_subject_get_int(count_subject) == 0);

        discover_test_sensors();
        REQUIRE(lv_subject_get_int(count_subject) == 2);

        discover({});
        REQUIRE(lv_subject_get_int(count_subject) == 0);
    }
}

// ============================================================================
// Role Assignment Tests
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - role assignment", "[probe][roles]") {
    discover_test_sensors();

    SECTION("Can set Z_PROBE role") {
        set_role("bltouch", ProbeSensorRole::Z_PROBE);
        REQUIRE(role_of("bltouch") == kZProbe);
    }

    SECTION("Role assignment is unique - assigning same role clears previous") {
        set_role("bltouch", ProbeSensorRole::Z_PROBE);
        set_role("smart_effector", ProbeSensorRole::Z_PROBE);

        REQUIRE(role_of("bltouch") == kNone);
        REQUIRE(role_of("smart_effector") == kZProbe);
    }

    SECTION("Can assign NONE without affecting other sensors") {
        set_role("bltouch", ProbeSensorRole::Z_PROBE);
        set_role("bltouch", ProbeSensorRole::NONE);

        REQUIRE(role_of("bltouch") == kNone);
        REQUIRE(role_of("smart_effector") == kNone);
    }

    SECTION("Assigning role to unknown sensor does nothing") {
        set_role("nonexistent_sensor", ProbeSensorRole::Z_PROBE);

        for (const auto& config : mgr().get_sensors()) {
            REQUIRE(config.role == ProbeSensorRole::NONE);
        }
    }
}

// ============================================================================
// State Update Tests
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - state updates", "[probe][state]") {
    discover_test_sensors();
    set_role("bltouch", ProbeSensorRole::Z_PROBE);

    SECTION("Parses last_z_result and last_query from status JSON") {
        auto state = z_state();
        REQUIRE(state.has_value());
        REQUIRE(state->last_z_result == 0.0f);
        REQUIRE_FALSE(state->triggered);

        json status;
        status["bltouch"]["last_z_result"] = 0.125f;
        status["bltouch"]["last_query"] = true;
        mgr().update_from_status(status);

        state = z_state();
        REQUIRE(state->last_z_result == Catch::Approx(0.125f));
        REQUIRE(state->triggered);
    }

    SECTION("last_query accepts the integer form Cartographer publishes") {
        json status;
        status["bltouch"]["last_query"] = 1;
        mgr().update_from_status(status);
        REQUIRE(z_state()->triggered);

        status["bltouch"]["last_query"] = 0;
        mgr().update_from_status(status);
        REQUIRE_FALSE(z_state()->triggered);
    }

    SECTION("A null or absent last_query keeps the previous value") {
        json status;
        status["bltouch"]["last_query"] = true;
        mgr().update_from_status(status);

        json null_status;
        null_status["bltouch"]["last_query"] = nullptr;
        mgr().update_from_status(null_status);
        REQUIRE(z_state()->triggered);

        json absent;
        absent["bltouch"]["last_z_result"] = 0.2f;
        mgr().update_from_status(absent);
        REQUIRE(z_state()->triggered);
    }

    SECTION("A numeric z_offset updates the seed, a null one keeps it") {
        discover({"bltouch", "smart_effector"}, json{{"bltouch", {{"z_offset", "-1.850"}}}});
        set_role("bltouch", ProbeSensorRole::Z_PROBE);

        json null_status;
        null_status["bltouch"]["z_offset"] = nullptr;
        mgr().update_from_status(null_status);
        REQUIRE(z_state()->z_offset == Catch::Approx(-1.850f));

        // Creality K1's Z_OFFSET_APPLY_PROBE moves it live, without a restart.
        json status;
        status["bltouch"]["z_offset"] = -0.5f;
        mgr().update_from_status(status);
        REQUIRE(z_state()->z_offset == Catch::Approx(-0.5f));
        REQUIRE(z_offset_subject() == -500);
        REQUIRE(mgr().get_z_offset() == Catch::Approx(-0.5f));
    }

    SECTION("Status update for unknown sensor is ignored") {
        json status;
        status["unknown_sensor"]["last_z_result"] = 0.125f;
        mgr().update_from_status(status);

        REQUIRE(mgr().sensor_count() == 2);
        REQUIRE(z_state()->last_z_result == 0.0f);
    }

    SECTION("Empty status update is handled") {
        mgr().update_from_status(json::object());

        REQUIRE(mgr().sensor_count() == 2);
    }
}

// ============================================================================
// Subject Value Tests
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - z_offset subject",
                 "[probe][subjects]") {
    discover_test_sensors();

    SECTION("Shows -1 when no sensor holds Z_PROBE") {
        REQUIRE(z_offset_subject() == -1);
        REQUIRE(mgr().get_z_offset() == 0.0f);
    }

    SECTION("Follows the holder in microns") {
        set_role("bltouch", ProbeSensorRole::Z_PROBE);
        REQUIRE(z_offset_subject() == 0);

        json status;
        status["bltouch"]["z_offset"] = -0.125f;
        mgr().update_from_status(status);
        REQUIRE(z_offset_subject() == -125);
    }

    SECTION("Shows -1 when the holder is disabled") {
        set_role("bltouch", ProbeSensorRole::Z_PROBE);
        json status;
        status["bltouch"]["z_offset"] = -0.125f;
        mgr().update_from_status(status);

        mgr().set_enabled("bltouch", false);
        REQUIRE(z_offset_subject() == -1);
        REQUIRE_FALSE(z_state().has_value());
    }
}

// ============================================================================
// Config Persistence Tests
// ============================================================================

namespace {
json load_probe_fixture() {
    std::ifstream in(helix::test::golden_path("sensor_settings", "probe"));
    REQUIRE(in.good());
    json j = json::parse(in, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    return j;
}
} // namespace

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - saved config reads back",
                 "[probe][config]") {
    const json fixture = load_probe_fixture();

    discover({"probe_eddy_current btt_eddy", "bltouch"});
    probe_slot() = fixture;
    mgr().load_settings();
    CHECK(role_of("probe_eddy_current btt_eddy") == kZProbe);
    CHECK(role_of("bltouch") == kNone);
    SensorEntry bltouch;
    REQUIRE(mgr().find("bltouch", bltouch));
    CHECK_FALSE(bltouch.enabled);

    probe_slot() = json::object();
    mgr().save_settings();
    INFO("expected " << fixture.dump());
    CHECK(probe_slot().dump() == fixture.dump());
}

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - unknown saved sensor is ignored",
                 "[probe][config]") {
    discover_test_sensors();
    probe_slot() = json{
        {"sensors", json::array({json{{"klipper_name", "unknown_sensor"}, {"role", "z_probe"}}})}};
    mgr().load_settings();

    for (const auto& sensor : mgr().get_sensors()) {
        REQUIRE(sensor.role == ProbeSensorRole::NONE);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - edge cases", "[probe][edge]") {
    SECTION("No state for an unassigned role") {
        discover_test_sensors();
        REQUIRE_FALSE(z_state().has_value());
    }

    SECTION("The holder is read only while enabled") {
        discover_test_sensors();
        set_role("bltouch", ProbeSensorRole::Z_PROBE);
        REQUIRE(z_state().has_value());

        mgr().set_enabled("bltouch", false);
        REQUIRE_FALSE(z_state().has_value());
    }

    SECTION("Eddy current probe without name parameter is ignored") {
        discover({"probe_eddy_current"});
        REQUIRE(mgr().sensor_count() == 0);
    }
}

// ============================================================================
// New Probe Type Discovery Tests
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - discovery of new probe types",
                 "[probe][discovery]") {
    SECTION("Discovers cartographer object") {
        discover({"cartographer"});
        REQUIRE(mgr().sensor_count() == 1);
        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].type == ProbeSensorType::CARTOGRAPHER);
        REQUIRE(configs[0].sensor_name == "cartographer");
        REQUIRE(configs[0].klipper_name == "cartographer");
    }

    SECTION("Discovers beacon object") {
        discover({"beacon"});
        REQUIRE(mgr().sensor_count() == 1);
        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].type == ProbeSensorType::BEACON);
        REQUIRE(configs[0].sensor_name == "beacon");
        REQUIRE(configs[0].klipper_name == "beacon");
    }

    SECTION("Eddy object beside a cartographer collapses to the cartographer") {
        discover({"probe_eddy_current carto", "cartographer"});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::CARTOGRAPHER);
    }

    SECTION("Eddy object beside a beacon collapses to the beacon") {
        discover({"probe_eddy_current beacon_probe", "beacon"});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::BEACON);
    }

    SECTION("Plain eddy current without companion stays EDDY_CURRENT") {
        discover({"probe_eddy_current btt"});
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::EDDY_CURRENT);
    }

    SECTION("Cartographer with eddy current deduplicates to single sensor") {
        discover({"probe_eddy_current carto", "cartographer", "probe"});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].klipper_name == "cartographer");
    }

    SECTION("Beacon with eddy current deduplicates to single sensor") {
        discover({"beacon", "probe_eddy_current beacon_probe", "probe"});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].klipper_name == "beacon");
    }
}

// ============================================================================
// Config-only probe sections
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - config-only probe sections",
                 "[probe][discovery][config]") {
    SECTION("load_cell_probe beside the probe object types it and feeds its z_offset") {
        discover({"probe"}, json{{"load_cell_probe", {{"z_offset", "-0.185"}}}});
        REQUIRE(mgr().sensor_count() == 1);
        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "probe");
        REQUIRE(configs[0].type == ProbeSensorType::LOADCELL);
        REQUIRE(configs[0].role == ProbeSensorRole::Z_PROBE);
        REQUIRE(z_state()->z_offset == Catch::Approx(-0.185f));
    }

    SECTION("dockable_probe beside the probe object is not a second sensor") {
        discover({"probe"}, json{{"dockable_probe", {{"z_offset", "1.0"}}}});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::STANDARD);
        REQUIRE(z_state()->z_offset == Catch::Approx(1.0f));
    }

    SECTION("a probe section of its own wins over an alias section") {
        discover({"probe"}, json{{"probe", {{"z_offset", "-2.0"}}},
                                 {"dockable_probe", {{"z_offset", "1.0"}}}});
        REQUIRE(z_state()->z_offset == Catch::Approx(-2.0f));
    }

    SECTION("with no probe object the section is the sensor") {
        discover({}, json{{"load_cell_probe", {{"z_offset", "0.5"}}}});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].klipper_name == "load_cell_probe");
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::LOADCELL);
    }
}

// ============================================================================
// Macro-based Probe Detection Tests
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - macro-based probe detection",
                 "[probe][discovery][macros]") {
    SECTION("Detects Klicky from ATTACH_PROBE/DOCK_PROBE macros") {
        discover({"probe", "gcode_macro ATTACH_PROBE", "gcode_macro DOCK_PROBE"});
        auto configs = mgr().get_sensors();
        REQUIRE(configs.size() == 1);
        REQUIRE(configs[0].type == ProbeSensorType::KLICKY);
    }

    SECTION("Detects Klicky from alternate macro names") {
        discover({"probe", "gcode_macro _Probe_Deploy", "gcode_macro _Probe_Stow"});
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::KLICKY);
    }

    SECTION("Standard probe without Klicky macros stays STANDARD") {
        discover({"probe"});
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::STANDARD);
    }

    SECTION("Standard probe with unrelated macros stays STANDARD") {
        discover({"probe", "gcode_macro START_PRINT", "gcode_macro END_PRINT"});
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::STANDARD);
    }
}

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - probe type override",
                 "[probe][discovery]") {
    discover({"probe"});
    mgr().set_probe_type_override(ProbeSensorType::PRTOUCH_V2);
    REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::PRTOUCH_V2);

    // A typed probe is not overridden again.
    mgr().set_probe_type_override(ProbeSensorType::TAP);
    REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::PRTOUCH_V2);
}

// PrinterState sets the override once per printer type, so it must outlive the
// configfile pass's second discover(), which re-derives types from object names.
TEST_CASE_METHOD(ProbeSensorTestFixture,
                 "ProbeSensorManager - probe type override survives rediscovery",
                 "[probe][discovery]") {
    discover({"probe"});
    mgr().set_probe_type_override(ProbeSensorType::PRTOUCH_V2);
    REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::PRTOUCH_V2);

    rediscover_from_config(discovery_of({"probe"}, json{{"probe", {{"z_offset", "0.1"}}}}));
    CHECK(mgr().get_sensors()[0].type == ProbeSensorType::PRTOUCH_V2);

    // A printer type without one clears it for the next discovery.
    mgr().set_probe_type_override(ProbeSensorType::STANDARD);
    discover({"probe"});
    CHECK(mgr().get_sensors()[0].type == ProbeSensorType::STANDARD);
}

// PrinterState::set_printer_type_internal's override, from a database entry.
TEST_CASE_METHOD(ProbeSensorTestFixture,
                 "ProbeSensorManager - a k2-improvements K2 keeps the probe it reports",
                 "[probe][discovery][k2improvements]") {
    for (const char* printer :
         {"Creality K2 Plus (k2-improvements)", "Creality K2 Pro (k2-improvements)"}) {
        CAPTURE(printer);
        const std::string db_type = PrinterDetector::get_probe_type(printer);
        REQUIRE_FALSE(db_type.empty());
        auto apply_database_override = [&] {
            mgr().set_probe_type_override(probe_type_from_string(db_type));
        };

        // The mod run without Cartographer leaves PRTouch behind a plain probe,
        // typed as the stock K2 entries type it.
        discover({"probe", "prtouch_v3"});
        apply_database_override();
        REQUIRE(mgr().get_sensors().size() == 1);
        CHECK(mgr().get_sensors()[0].type == ProbeSensorType::STANDARD);

        ProbeSensorManagerTestAccess::reset(mgr());
        discover({"cartographer"});
        apply_database_override();
        REQUIRE(mgr().get_sensors().size() == 1);
        CHECK(mgr().get_sensors()[0].type == ProbeSensorType::CARTOGRAPHER);
        ProbeSensorManagerTestAccess::reset(mgr());
    }
}

// ============================================================================
// Probe Type Display String Tests
// ============================================================================

TEST_CASE("Probe type display strings", "[probe][types]") {
    REQUIRE(probe_type_to_display_string(ProbeSensorType::STANDARD) == "Probe");
    REQUIRE(probe_type_to_display_string(ProbeSensorType::BLTOUCH) == "BLTouch");
    REQUIRE(probe_type_to_display_string(ProbeSensorType::SMART_EFFECTOR) == "Smart Effector");
    REQUIRE(probe_type_to_display_string(ProbeSensorType::EDDY_CURRENT) == "Eddy Current");
    REQUIRE(probe_type_to_display_string(ProbeSensorType::CARTOGRAPHER) == "Cartographer");
    REQUIRE(probe_type_to_display_string(ProbeSensorType::BEACON) == "Beacon");
    REQUIRE(probe_type_to_display_string(ProbeSensorType::TAP) == "Voron Tap");
    REQUIRE(probe_type_to_display_string(ProbeSensorType::KLICKY) == "Klicky");
    REQUIRE(probe_type_to_display_string(ProbeSensorType::LOADCELL) == "Load Cell");
}

// ============================================================================
// Null Field Handling Tests
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - null fields in status",
                 "[probe][state][loadcell]") {
    discover({"probe"}, json{{"probe", {{"z_offset", "-0.250"}}}});

    SECTION("Null z_offset does not overwrite the config-seeded value") {
        json null_status;
        null_status["probe"]["last_z_result"] = 0.1f;
        null_status["probe"]["z_offset"] = nullptr;
        mgr().update_from_status(null_status);

        auto state = z_state();
        REQUIRE(state->z_offset == Catch::Approx(-0.25f));
        REQUIRE(state->last_z_result == Catch::Approx(0.1f));
        REQUIRE(z_offset_subject() == -250);
    }

    SECTION("Null last_z_result does not overwrite existing value") {
        json status;
        status["probe"]["last_z_result"] = 0.5f;
        mgr().update_from_status(status);

        json null_status;
        null_status["probe"]["last_z_result"] = nullptr;
        mgr().update_from_status(null_status);

        REQUIRE(z_state()->last_z_result == Catch::Approx(0.5f));
    }
}

// ============================================================================
// Cartographer Status Source Tests
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture,
                 "ProbeSensorManager - Cartographer reads the probe object it registers",
                 "[probe][state][cartographer]") {
    discover({"probe", "cartographer"});
    REQUIRE(mgr().sensor_count() == 1);
    REQUIRE(role_of("cartographer") == kZProbe);

    // The cartographer object nests last_z_result per mode; the flat keys live
    // on the probe object.
    json status;
    status["cartographer"] = {{"scan", {{"last_z_result", 9.0}}}, {"touch", nullptr}};
    status["probe"] = {{"name", "cartographer"}, {"last_query", 1}, {"last_z_result", -0.425}};
    mgr().update_from_status(status);

    auto state = z_state();
    REQUIRE(state->last_z_result == Catch::Approx(-0.425f));
    REQUIRE(state->triggered);
}

// ============================================================================
// Subscriptions
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - extra_subscriptions per type",
                 "[probe][subscription]") {
    const auto subs_of = [&](const std::vector<std::string>& objects) {
        return mgr().extra_subscriptions(discovery_of(objects));
    };
    const json full = json::array({"last_query", "last_z_result", "z_offset"});
    const json no_query = json::array({"last_z_result", "z_offset"});

    CHECK(subs_of({"bltouch"}) == json{{"bltouch", full}});
    CHECK(subs_of({"beacon"}) == json{{"beacon", no_query}});
    CHECK(subs_of({"probe_eddy_current btt"}) == json{{"probe_eddy_current btt", no_query}});
    CHECK(subs_of({"probe", "cartographer"}) == json{{"probe", full}});
    CHECK(subs_of({"bltouch", "probe"}) == json{{"bltouch", full}});
    CHECK(subs_of({"probe_eddy_current carto", "probe", "cartographer"}) == json{{"probe", full}});
    CHECK(subs_of({"extruder", "gcode_macro X"}) == json::object());

    // Config-only sections are not Klipper objects: nothing to subscribe.
    CHECK(mgr().extra_subscriptions(discovery_of({}, json{{"dockable_probe", json::object()}})) ==
          json::object());
}

// ============================================================================
// z_offset seeding from the configfile
// ============================================================================

TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - discovery seeds z_offset",
                 "[probe][discovery][config]") {
    SECTION("Seeds z_offset from config string value") {
        discover({"probe"}, json{{"probe", {{"z_offset", "-0.250"}}}});

        REQUIRE(z_state()->z_offset == Catch::Approx(-0.25f));
        REQUIRE(z_offset_subject() == -250);
    }

    SECTION("Seeds z_offset from config numeric value") {
        discover({"probe"}, json{{"probe", {{"z_offset", -1.5f}}}});
        REQUIRE(z_state()->z_offset == Catch::Approx(-1.5f));
    }

    SECTION("A non-numeric z_offset seeds nothing") {
        discover({"probe"}, json{{"probe", {{"z_offset", "oops"}}}});
        REQUIRE(z_state()->z_offset == 0.0f);
    }

    SECTION("Ignores config for another sensor") {
        discover({"probe"}, json{{"unknown_sensor", {{"z_offset", "-0.5"}}}});
        REQUIRE(z_state()->z_offset == 0.0f);
    }

    SECTION("A probe whose status reports a null z_offset still shows the configured offset") {
        discover({"probe"}, json{{"probe", {{"z_offset", "-0.250"}}}});

        json status;
        status["probe"]["last_z_result"] = 0.0f;
        status["probe"]["z_offset"] = nullptr;
        mgr().update_from_status(status);

        REQUIRE(z_state()->z_offset == Catch::Approx(-0.25f));
        REQUIRE(z_offset_subject() == -250);
        REQUIRE(mgr().get_z_offset() == Catch::Approx(-0.25f));
    }
}

// Outside sync mode a status update reaches the subject only through the queue,
// and a deferral queued before deinit_subjects() is dropped.
TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - status refresh is deferred",
                 "[probe][lifetime]") {
    discover_test_sensors();
    set_role("bltouch", ProbeSensorRole::Z_PROBE);
    mgr().set_sync_mode(false);

    const auto offset_status = [](float z_offset) {
        json status;
        status["bltouch"]["z_offset"] = z_offset;
        return status;
    };
    mgr().update_from_status(offset_status(0.25f));
    REQUIRE(z_offset_subject() == 0);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(z_offset_subject() == 250);

    mgr().update_from_status(offset_status(0.5f));
    mgr().deinit_subjects();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    mgr().init_subjects();
    mgr().set_sync_mode(true);
    REQUIRE(z_offset_subject() == -1);
}

// ============================================================================
// Single probe is the Z probe
// ============================================================================

// Klipper's bltouch/smart_effector/probe_eddy_current, Beacon and Cartographer all
// register the generic probe object beside their own. The pair is one probe, so a fresh
// install gets Z_PROBE without anyone picking it.
TEST_CASE_METHOD(ProbeSensorTestFixture,
                 "ProbeSensorManager - alias objects still auto-assign Z_PROBE", "[probe][roles]") {
    SECTION("bltouch plus its probe alias") {
        discover({"bltouch", "probe"});
        mgr().load_settings();
        REQUIRE(mgr().get_sensors()[0].klipper_name == "bltouch");
        update_sensor_state("bltouch", 0.1f);
        REQUIRE(z_state()->last_z_result == Catch::Approx(0.1f));
    }

    SECTION("cartographer plus an eddy object and its probe object") {
        discover({"probe", "probe_eddy_current carto", "cartographer"});
        mgr().load_settings();
        update_sensor_state("probe", -0.4f);
        REQUIRE(z_state()->last_z_result == Catch::Approx(-0.4f));
    }

    SECTION("plain probe alone, with no settings saved") {
        discover({"probe"});
        mgr().load_settings();
        REQUIRE(role_of("probe") == kZProbe);
        update_sensor_state("probe", 0.25f);
        REQUIRE(z_state()->last_z_result == Catch::Approx(0.25f));
    }

    SECTION("a saved explicit role still wins") {
        probe_slot() = json{
            {"sensors", json::array({{{"klipper_name", "smart_effector"}, {"role", "z_probe"}}})}};
        discover({"bltouch", "smart_effector", "probe"});
        mgr().load_settings();
        auto configs = mgr().get_sensors();
        REQUIRE(configs.size() == 2);
        for (const auto& c : configs) {
            CAPTURE(c.klipper_name);
            REQUIRE((c.role == ProbeSensorRole::Z_PROBE) == (c.klipper_name == "smart_effector"));
        }
    }

    SECTION("a saved role of none on a single probe stays none") {
        probe_slot() = json{{"sensors", json::array({{{"klipper_name", "probe"},
                                                      {"role", "none"},
                                                      {"enabled", true},
                                                      {"type", "standard"}}})}};
        discover({"probe"});
        REQUIRE(role_of("probe") == kZProbe);
        mgr().load_settings();
        REQUIRE(role_of("probe") == kNone);
        REQUIRE_FALSE(z_state().has_value());
    }

    SECTION("a saved role of none survives the configfile pass and the next save") {
        probe_slot() = json{{"sensors", json::array({{{"enabled", true},
                                                      {"klipper_name", "probe"},
                                                      {"role", "none"},
                                                      {"type", "standard"}}})}};
        const std::string saved = probe_slot().dump();
        // Connect: discover, then saved settings.
        discover({"probe"});
        mgr().load_settings();
        REQUIRE(role_of("probe") == kNone);

        // configfile.config arrives and the config-reading managers discover again.
        rediscover_from_config(discovery_of({"probe"}, json{{"probe", {{"z_offset", "-0.250"}}}}));
        CHECK(role_of("probe") == kNone);
        CHECK(z_offset_subject() == -1);

        // Any Settings edit saves every manager's settings.
        mgr().save_settings();
        CHECK(probe_slot().dump() == saved);
    }
}

// An object with no instance in its name reads as its type, its Klipper name below.
TEST_CASE_METHOD(ProbeSensorTestFixture, "ProbeSensorManager - rows are named by type",
                 "[probe][sensors]") {
    discover({"bltouch"});
    const auto view = mgr().row(0);
    CHECK(view.name == "BLTouch");
    CHECK(view.detail == "bltouch");
    // The role is text only: no dropdown, the held role shown as its label.
    CHECK(view.role_count == 0);
    CHECK(view.held_role_label == "Z Probe");
}
