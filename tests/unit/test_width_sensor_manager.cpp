// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_width_sensor_manager.cpp
 * @brief WidthSensorManager on the SensorManager base: discovery, the
 *        FLOW_COMPENSATION default, status parsing, the diameter subject and
 *        the persisted settings shape.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/golden_json.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "config.h"
#include "hardware_object_table.h"
#include "printer_discovery.h"
#include "width_sensor_manager.h"
#include "width_sensor_types.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

namespace helix::sensors {
class WidthSensorManagerTestAccess {
  public:
    static void reset(WidthSensorManager& obj) {
        SensorManagerTestAccess::reconcile(obj, {});
        std::lock_guard<std::recursive_mutex> lock(obj.mutex());
        obj.readings_.clear();
        obj.set_sync_mode(true);
        if (obj.subjects_initialized()) {
            lv_subject_set_int(&obj.diameter_, -1);
            lv_subject_copy_string(&obj.diameter_text_, "--");
        }
    }
};
} // namespace helix::sensors

namespace {

constexpr const char* kTsl = "tsl1401cl_filament_width_sensor";
constexpr const char* kHall = "hall_filament_width_sensor";
constexpr uint8_t kNone = static_cast<uint8_t>(WidthSensorRole::NONE);
constexpr uint8_t kFlow = static_cast<uint8_t>(WidthSensorRole::FLOW_COMPENSATION);

PrinterDiscovery discovery_of(const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(json(objects));
    return hw;
}

class WidthSensorTestFixture : public LVGLTestFixture {
  public:
    WidthSensorTestFixture() {
        mgr().init_subjects();
        WidthSensorManagerTestAccess::reset(mgr());
    }

    ~WidthSensorTestFixture() override {
        WidthSensorManagerTestAccess::reset(mgr());
    }

  protected:
    WidthSensorManager& mgr() {
        return WidthSensorManager::instance();
    }

    void discover(const std::vector<std::string>& objects) {
        mgr().discover(discovery_of(objects));
    }

    void discover_test_sensors() {
        discover({kTsl, kHall});
    }

    void update_sensor_state(const std::string& klipper_name, float diameter, float raw_value) {
        json status;
        status[klipper_name]["Diameter"] = diameter;
        status[klipper_name]["Raw"] = raw_value;
        mgr().update_from_status(status);
    }

    uint8_t role_of(const std::string& klipper_name) {
        SensorEntry e;
        REQUIRE(mgr().find(klipper_name, e));
        return e.role;
    }

    int diameter() {
        return lv_subject_get_int(mgr().get_diameter_subject());
    }
};

} // namespace

// ============================================================================
// Type Helper Tests (width_sensor_types.h)
// ============================================================================

TEST_CASE("WidthSensorTypes - role string conversion", "[width][types]") {
    SECTION("width_role_to_string") {
        REQUIRE(width_role_to_string(WidthSensorRole::NONE) == "none");
        REQUIRE(width_role_to_string(WidthSensorRole::FLOW_COMPENSATION) == "flow_compensation");
    }

    SECTION("width_role_from_string") {
        REQUIRE(width_role_from_string("none") == WidthSensorRole::NONE);
        REQUIRE(width_role_from_string("flow_compensation") == WidthSensorRole::FLOW_COMPENSATION);
        REQUIRE(width_role_from_string("invalid") == WidthSensorRole::NONE);
        REQUIRE(width_role_from_string("") == WidthSensorRole::NONE);
    }

    SECTION("width_role_to_display_string") {
        REQUIRE(width_role_to_display_string(WidthSensorRole::NONE) == "Unassigned");
        REQUIRE(width_role_to_display_string(WidthSensorRole::FLOW_COMPENSATION) ==
                "Flow Compensation");
    }
}

TEST_CASE("WidthSensorTypes - type string conversion", "[width][types]") {
    SECTION("width_type_to_string") {
        REQUIRE(width_type_to_string(WidthSensorType::TSL1401CL) == "tsl1401cl");
        REQUIRE(width_type_to_string(WidthSensorType::HALL) == "hall");
    }

    SECTION("width_type_from_string") {
        REQUIRE(width_type_from_string("tsl1401cl") == WidthSensorType::TSL1401CL);
        REQUIRE(width_type_from_string("hall") == WidthSensorType::HALL);
        REQUIRE(width_type_from_string("invalid") == WidthSensorType::TSL1401CL);
        REQUIRE(width_type_from_string("") == WidthSensorType::TSL1401CL);
    }
}

// ============================================================================
// Sensor Discovery Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - discovery", "[width][discovery]") {
    SECTION("Discovers TSL1401CL sensor") {
        discover({kTsl});

        REQUIRE(mgr().sensor_count() == 1);
        const auto entries = mgr().entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].klipper_name == kTsl);
        CHECK(entries[0].type == static_cast<uint8_t>(WidthSensorType::TSL1401CL));
        CHECK(entries[0].enabled);
        CHECK(entries[0].available);
        CHECK(entries[0].role == kFlow);
    }

    SECTION("Discovers Hall sensor") {
        discover({kHall});

        REQUIRE(mgr().sensor_count() == 1);
        const auto entries = mgr().entries();
        CHECK(entries[0].klipper_name == kHall);
        CHECK(entries[0].type == static_cast<uint8_t>(WidthSensorType::HALL));
        CHECK(entries[0].role == kFlow);
    }

    SECTION("Only the first of several sensors takes the default role") {
        discover_test_sensors();

        const auto entries = mgr().entries();
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].klipper_name == kTsl);
        CHECK(entries[0].role == kFlow);
        CHECK(entries[1].klipper_name == kHall);
        CHECK(entries[1].role == kNone);
    }

    SECTION("Ignores unrelated objects") {
        discover({kTsl, "filament_switch_sensor runout", "temperature_sensor chamber", "extruder"});
        CHECK(mgr().sensor_count() == 1);
    }

    SECTION("Empty sensor list clears previous sensors") {
        discover_test_sensors();
        REQUIRE(mgr().sensor_count() == 2);

        discover({});
        CHECK(mgr().sensor_count() == 0);
    }

    SECTION("Re-discovery drops vanished sensors") {
        discover({kTsl});
        discover({kHall});
        REQUIRE(mgr().sensor_count() == 1);
        CHECK(mgr().entries()[0].klipper_name == kHall);
    }

    SECTION("Sensor count subject is updated") {
        lv_subject_t* count_subject = mgr().count_subject();
        CHECK(lv_subject_get_int(count_subject) == 0);

        discover_test_sensors();
        CHECK(lv_subject_get_int(count_subject) == 2);

        discover({});
        CHECK(lv_subject_get_int(count_subject) == 0);
    }
}

TEST_CASE_METHOD(WidthSensorTestFixture,
                 "WidthSensorManager - rediscovery keeps the role a survivor holds",
                 "[width][discovery]") {
    discover_test_sensors();
    mgr().set_role(kHall, kFlow);
    REQUIRE(role_of(kTsl) == kNone);

    discover_test_sensors();
    CHECK(role_of(kHall) == kFlow);
    CHECK(role_of(kTsl) == kNone);
}

TEST_CASE_METHOD(WidthSensorTestFixture,
                 "WidthSensorManager - the default role goes to the first sensor only when no "
                 "sensor holds it",
                 "[width][discovery]") {
    discover({kHall});
    mgr().set_role(kHall, kNone);
    REQUIRE(role_of(kHall) == kNone);

    // Nothing holds the role after the survivor was cleared, so rediscovery assigns it.
    discover_test_sensors();
    CHECK(role_of(kTsl) == kFlow);
    CHECK(role_of(kHall) == kNone);

    // A holder that is not first keeps it.
    mgr().set_role(kHall, kFlow);
    discover_test_sensors();
    CHECK(role_of(kHall) == kFlow);
    CHECK(role_of(kTsl) == kNone);
}

// ============================================================================
// Role Assignment Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - role assignment", "[width][roles]") {
    discover_test_sensors();

    SECTION("FLOW_COMPENSATION is exclusive: assigning it clears the previous holder") {
        mgr().set_role(kHall, kFlow);
        CHECK(role_of(kTsl) == kNone);
        CHECK(role_of(kHall) == kFlow);
    }

    SECTION("Can assign NONE without affecting other sensors") {
        mgr().set_role(kTsl, kNone);
        CHECK(role_of(kTsl) == kNone);
        CHECK(role_of(kHall) == kNone);
    }

    SECTION("Assigning role to unknown sensor does nothing") {
        mgr().set_role("nonexistent_sensor", kFlow);
        CHECK(role_of(kTsl) == kFlow);
        CHECK(role_of(kHall) == kNone);
    }

    SECTION("None and the role are offered in the Settings dropdown") {
        CHECK(mgr().offered_roles() == std::vector<uint8_t>{kNone, kFlow});
    }
}

// ============================================================================
// State Update Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - state updates", "[width][state]") {
    discover_test_sensors();

    SECTION("Parses diameter from status JSON") {
        CHECK(diameter() == 0);
        update_sensor_state(kTsl, 1.75f, 12345.0f);
        CHECK(diameter() == 1750);
    }

    SECTION("A null field from a field-restricted subscription is ignored") {
        update_sensor_state(kTsl, 1.75f, 12345.0f);
        json status;
        status[kTsl]["Diameter"] = nullptr;
        status[kTsl]["Raw"] = 99.0f;
        mgr().update_from_status(status);
        CHECK(diameter() == 1750);
    }

    SECTION("Status update for unknown sensor is ignored") {
        json status;
        status["unknown_sensor"]["Diameter"] = 1.75f;
        mgr().update_from_status(status);
        CHECK(mgr().sensor_count() == 2);
        CHECK(diameter() == 0);
    }

    SECTION("A reading from a sensor without the role does not reach the subject") {
        update_sensor_state(kHall, 1.90f, 1.0f);
        CHECK(diameter() == 0);
    }

    SECTION("Empty status update is handled") {
        mgr().update_from_status(json::object());
        CHECK(mgr().sensor_count() == 2);
    }
}

// ============================================================================
// Subject Value Tests
// ============================================================================

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - subject values",
                 "[width][subjects]") {
    discover_test_sensors();

    SECTION("Diameter subject is 0 after discovery (first sensor holds the role)") {
        CHECK(diameter() == 0);
    }

    SECTION("Diameter subject updates correctly (diameter x 1000)") {
        update_sensor_state(kTsl, 1.75f, 12345.0f);
        CHECK(diameter() == 1750);
        update_sensor_state(kTsl, 1.82f, 54321.0f);
        CHECK(diameter() == 1820);
    }

    SECTION("Diameter subject is -1 when the sensor is disabled") {
        update_sensor_state(kTsl, 1.75f, 12345.0f);
        mgr().set_enabled(kTsl, false);
        CHECK(diameter() == -1);
    }

    SECTION("Diameter subject follows the role to another sensor") {
        update_sensor_state(kTsl, 1.75f, 1.0f);
        update_sensor_state(kHall, 1.60f, 1.0f);
        mgr().set_role(kHall, kFlow);
        CHECK(diameter() == 1600);
    }

    SECTION("Diameter subject is -1 when no sensor holds the role") {
        mgr().set_role(kTsl, kNone);
        CHECK(diameter() == -1);
    }
}

// ============================================================================
// Config Persistence Tests
// ============================================================================

namespace {
json& width_slot() {
    Config* cfg = Config::get_instance();
    return cfg->get_json(cfg->df() + "width_sensors");
}

json load_width_fixture() {
    std::ifstream in(helix::test::golden_path("sensor_settings", "width"));
    REQUIRE(in.good());
    json j = json::parse(in, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    return j;
}
} // namespace

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - saved config reads back",
                 "[width][config]") {
    const json fixture = load_width_fixture();
    const json saved_slot = width_slot();

    // The fixture gives the role to the first sensor; the opposite assignment
    // proves load_settings() overrides the discovery default.
    json swapped = fixture;
    swapped["sensors"][0]["role"] = "none";
    swapped["sensors"][1]["role"] = "flow_compensation";

    discover_test_sensors();
    width_slot() = swapped;
    mgr().load_settings();
    CHECK(role_of(kTsl) == kNone);
    CHECK(role_of(kHall) == kFlow);

    width_slot() = fixture;
    mgr().load_settings();
    CHECK(role_of(kTsl) == kFlow);
    CHECK(role_of(kHall) == kNone);

    width_slot() = json::object();
    mgr().save_settings();
    const std::string written = width_slot().dump();
    width_slot() = saved_slot;
    INFO("expected " << fixture.dump());
    CHECK(written == fixture.dump());
}

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - disabled state persists",
                 "[width][config]") {
    const json saved_slot = width_slot();
    discover_test_sensors();
    mgr().set_enabled(kHall, false);
    mgr().save_settings();
    const json written = width_slot();
    width_slot() = saved_slot;

    REQUIRE(written["sensors"].size() == 2);
    CHECK(written["sensors"][1]["klipper_name"] == kHall);
    CHECK(written["sensors"][1]["enabled"] == false);
}

TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - unknown saved sensor is ignored",
                 "[width][config]") {
    const json saved_slot = width_slot();
    discover_test_sensors();
    width_slot() = json{{"sensors", json::array({json{{"klipper_name", "unknown_sensor"},
                                                      {"role", "flow_compensation"}}})}};
    mgr().load_settings();
    width_slot() = saved_slot;

    CHECK(role_of(kTsl) == kFlow);
    CHECK(role_of(kHall) == kNone);
}

// ============================================================================
// Lifetime
// ============================================================================

// Outside sync mode a status update reaches the subject only through the queue,
// and a deferral queued before deinit_subjects() is dropped.
TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - status refresh is deferred",
                 "[width][lifetime]") {
    discover_test_sensors();
    mgr().set_sync_mode(false);

    update_sensor_state(kTsl, 1.75f, 100.0f);
    CHECK(diameter() == 0);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(diameter() == 1750);

    update_sensor_state(kTsl, 1.80f, 100.0f);
    mgr().deinit_subjects();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    mgr().init_subjects();
    mgr().set_sync_mode(true);
    CHECK(diameter() == -1);
}

TEST_CASE("WidthSensorManager - is in the sensor registry", "[width][sensor_manager_base]") {
    const auto& managers = sensor_managers();
    CHECK(std::find(managers.begin(), managers.end(), &WidthSensorManager::instance()) !=
          managers.end());
}

TEST_CASE("hw table: width sensors carry their type as the chip", "[width][hw_object_table]") {
    CHECK(hw::classify_object(kTsl).chip == static_cast<uint8_t>(WidthSensorType::TSL1401CL));
    CHECK(hw::classify_object(kHall).chip == static_cast<uint8_t>(WidthSensorType::HALL));
    CHECK(hw::chip_name(hw::ObjectKind::WidthSensor, hw::classify_object(kHall).chip) == "Hall");
}

// The object name is long and carries no instance, so the row is named by its
// type and the object name moves underneath.
TEST_CASE_METHOD(WidthSensorTestFixture, "WidthSensorManager - rows are named by type",
                 "[width][sensors]") {
    discover({"hall_filament_width_sensor", "tsl1401cl_filament_width_sensor"});
    CHECK(mgr().row(0).name == "Hall width sensor");
    CHECK(mgr().row(0).detail == "Hall · hall_filament_width_sensor");
    CHECK(mgr().row(1).name == "TSL1401CL width sensor");
    CHECK(mgr().row(1).detail == "TSL1401CL · tsl1401cl_filament_width_sensor");
}
