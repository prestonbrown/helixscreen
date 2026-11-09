// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_load_cell_manager.cpp
 * @brief Unit tests for LoadCellManager
 *
 * Tests cover:
 * - Sensor discovery from Klipper object names (load_cell)
 * - Role assignment (SPOOL_WEIGHT)
 * - State updates from Moonraker status JSON
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "../ui_test_utils.h"
#include "ams_state.h"
#include "config.h"
#include "lane_source_store.h"
#include "load_cell_manager.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

// ============================================================================
// Test Access
// ============================================================================

namespace helix::sensors {
class LoadCellManagerTestAccess {
  public:
    static void reset(LoadCellManager& obj) {
        obj.set_sync_mode(true);
        SensorManagerTestAccess::reconcile(obj, {});
        std::lock_guard<std::recursive_mutex> lock(obj.mutex());
        obj.readings_.clear();
        obj.deinit_subjects();
    }
};
} // namespace helix::sensors

namespace {

PrinterDiscovery discovery_of(const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(json(objects));
    return hw;
}

} // namespace

// ============================================================================
// Test Fixture
// ============================================================================

class LoadCellTestFixture : public LVGLTestFixture {
  public:
    LoadCellTestFixture() {
        // Reset state for test isolation first
        LoadCellManagerTestAccess::reset(mgr());

        // Initialize subjects after reset (reset_for_testing deinits subjects)
        mgr().init_subjects();

        Config::get_instance()->reset_to_defaults();
        AmsState::instance().clear_external_spool_info();
    }

    ~LoadCellTestFixture() {
        // Reset after each test
        LoadCellManagerTestAccess::reset(mgr());
        AmsState::instance().clear_external_spool_info();
    }

  protected:
    LoadCellManager& mgr() {
        return LoadCellManager::instance();
    }

    // Helper to discover standard test sensors using config keys (how production works)
    void discover_test_sensors() {
        std::vector<std::string> klipper_objects = {"load_cell", "load_cell spool_weight"};
        discover(klipper_objects);
    }

    void discover(const std::vector<std::string>& objects) {
        mgr().discover(discovery_of(objects));
    }

    // Helper to simulate Moonraker status update
    void update_sensor_state(const std::string& klipper_name, float force_g) {
        json status;
        status[klipper_name]["force_g"] = force_g;
        mgr().update_from_status(status);
    }

    // The resolved record, which is what every view reads.
    SlotInfo ams_state_spool_info() {
        const auto info = AmsState::instance().get_external_spool_info();
        REQUIRE(info.has_value());
        return *info;
    }

    bool has_external_spool() {
        return AmsState::instance().get_external_spool_info().has_value();
    }

  private:
};

// Static members

// ============================================================================
// State Update Tests
// ============================================================================

TEST_CASE_METHOD(LoadCellTestFixture, "LoadCellManager - state updates", "[load_cell][state]") {
    discover_test_sensors();

    SECTION("Discovers unnamed and named sensors") {
        REQUIRE(mgr().sensor_count() == 2);
    }

    SECTION("Treats a single unnamed load cell as spool weight") {
        std::vector<std::string> klipper_objects = {"load_cell"};
        discover(klipper_objects);
        REQUIRE(mgr().sensor_count() == 1);

        update_sensor_state("load_cell", 300.0);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);
    }

    SECTION("Treats a single named load cell as spool weight, regardless of its name") {
        std::vector<std::string> klipper_objects = {"load_cell name_does_not_matter"};
        discover(klipper_objects);
        REQUIRE(mgr().sensor_count() == 1);

        update_sensor_state("load_cell name_does_not_matter", 300.0);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);
    }

    SECTION("Publishes spool weight update to AmsState") {
        update_sensor_state("load_cell spool_weight", 300.0);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);
    }

    SECTION("Does not publish spool weight update for load cells "
            "that don't have the SPOOL_WEIGHT role") {
        update_sensor_state("load_cell", 300.0);
        REQUIRE(!has_external_spool());
    }

    SECTION("Publishes update only if spool weight decreased by at least 0.5 g") {
        REQUIRE(!has_external_spool());

        update_sensor_state("load_cell spool_weight", 300.0);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);

        update_sensor_state("load_cell spool_weight", 299.6);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);

        update_sensor_state("load_cell spool_weight", 299.5);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 299.5);
    }

    SECTION("Publishes update only if spool weight increased by at least 1 g") {
        update_sensor_state("load_cell spool_weight", 300.0);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);

        update_sensor_state("load_cell spool_weight", 300.9);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);

        update_sensor_state("load_cell spool_weight", 301.0);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 301.0);
    }

    SECTION("Shows the reading over an earlier user edit of the weight") {
        SlotInfo spool;
        spool.material = "PLA";
        spool.remaining_weight_g = 750.0f;
        helix::AmsStateTestAccess::seed_external_spool(spool);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 750.0);

        update_sensor_state("load_cell spool_weight", 290.0);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 290.0);
    }

    SECTION("A frame without a usable force_g keeps the published weight") {
        update_sensor_state("load_cell spool_weight", 300.0);

        json missing;
        missing["load_cell spool_weight"] = json::object();
        mgr().update_from_status(missing);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);

        json null_value;
        null_value["load_cell spool_weight"]["force_g"] = nullptr;
        mgr().update_from_status(null_value);
        REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);
    }

    SECTION("Empty status update is handled") {
        json status = json::object();
        mgr().update_from_status(status);

        REQUIRE(mgr().sensor_count() == 2);
    }
}

// ============================================================================
// Roles and the AmsState push
// ============================================================================

namespace {
constexpr uint8_t kSpool = static_cast<uint8_t>(LoadCellRole::SPOOL_WEIGHT);
constexpr uint8_t kNoRole = static_cast<uint8_t>(LoadCellRole::NONE);
} // namespace

TEST_CASE_METHOD(LoadCellTestFixture,
                 "LoadCellManager - SPOOL_WEIGHT is assigned by name or sole cell",
                 "[load_cell][roles]") {
    SECTION("a cell named for a spool holds the role, the other does not") {
        discover_test_sensors();
        SensorEntry spool;
        SensorEntry plain;
        REQUIRE(mgr().find("load_cell spool_weight", spool));
        REQUIRE(mgr().find("load_cell", plain));
        REQUIRE(spool.role == kSpool);
        REQUIRE(plain.role == kNoRole);
        REQUIRE(mgr().has_spool_weight_load_cell());
    }

    SECTION("two cells with no spool name leave the role unheld") {
        discover({"load_cell a", "load_cell b"});
        REQUIRE(mgr().sensor_count() == 2);
        REQUIRE_FALSE(mgr().has_spool_weight_load_cell());
    }

    SECTION("the role is exclusive and not offered") {
        discover({"load_cell spool_a", "load_cell spool_b"});
        SensorEntry a;
        SensorEntry b;
        REQUIRE(mgr().find("load_cell spool_a", a));
        REQUIRE(mgr().find("load_cell spool_b", b));
        REQUIRE(a.role == kSpool);
        REQUIRE(b.role == kNoRole);
        mgr().set_role("load_cell spool_b", kSpool);
        REQUIRE(mgr().find("load_cell spool_a", a));
        REQUIRE(mgr().find("load_cell spool_b", b));
        REQUIRE(a.role == kNoRole);
        REQUIRE(b.role == kSpool);
        REQUIRE(mgr().offered_roles().empty());
    }

    SECTION("get_sensors_sorted lists the spool cell first") {
        discover({"load_cell aaa", "load_cell spool_weight"});
        const auto sorted = mgr().get_sensors_sorted();
        REQUIRE(sorted.size() == 2);
        REQUIRE(sorted[0].klipper_name == "load_cell spool_weight");
    }
}

TEST_CASE_METHOD(LoadCellTestFixture,
                 "LoadCellManager - a new force_g reaches AmsState once, noise does not",
                 "[load_cell][ams]") {
    discover_test_sensors();

    // The bypass lane's Metered reading changes only when the manager pushes,
    // so planting a marker there shows whether a later frame pushed again.
    const auto raw_weight = [] {
        return AmsState::instance().get_external_spool_info().value().remaining_weight_g;
    };
    const auto plant_marker = [] {
        helix::ams::Observation marker(helix::ams::ObservationSource::Metered);
        marker.remaining_weight_g = 12345.0f;
        helix::ams::ingest(helix::ams::kBypassLaneId, marker);
    };

    update_sensor_state("load_cell spool_weight", 300.0);
    REQUIRE(raw_weight() == 300.0f);

    SECTION("an identical frame does not push") {
        // Marker 12345 would be overwritten by any push (delta is far past threshold).
        plant_marker();
        update_sensor_state("load_cell spool_weight", 300.0);
        REQUIRE(raw_weight() == 12345.0f);
    }

    SECTION("a changed frame pushes exactly the new reading") {
        plant_marker();
        update_sensor_state("load_cell spool_weight", 310.0);
        REQUIRE(raw_weight() == 310.0f);
    }

    SECTION("a sub-threshold change does not push") {
        update_sensor_state("load_cell spool_weight", 300.4);
        update_sensor_state("load_cell spool_weight", 300.9);
        REQUIRE(raw_weight() == 300.0f);
    }
}

// A reading belongs to the sensor discovery lists; when the cell vanishes its
// reading goes too, so a later printer's cell of the same name starts empty.
TEST_CASE_METHOD(LoadCellTestFixture,
                 "LoadCellManager - a reconnect without the load cell drops its reading",
                 "[load_cell][state]") {
    discover({"load_cell"});
    update_sensor_state("load_cell", 300.0);
    REQUIRE(ams_state_spool_info().remaining_weight_g == 300.0);

    discover({});
    AmsState::instance().clear_external_spool_info();
    discover({"load_cell"});
    CHECK_FALSE(has_external_spool());
}
