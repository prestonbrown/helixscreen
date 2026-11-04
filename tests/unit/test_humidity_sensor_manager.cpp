// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_humidity_sensor_manager.cpp
 * @brief HumiditySensorManager on the SensorManager base: chip discovery, the
 *        name-driven CHAMBER and DRYER roles, status parsing and the chamber
 *        humidity subject. Humidity sensors have no persisted settings.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "hardware_object_table.h"
#include "humidity_sensor_manager.h"
#include "humidity_sensor_types.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

namespace helix::sensors {
class HumiditySensorManagerTestAccess {
  public:
    static void reset(HumiditySensorManager& obj) {
        SensorManagerTestAccess::reconcile(obj, {});
        std::lock_guard<std::recursive_mutex> lock(obj.mutex());
        obj.humidity_.clear();
        obj.set_sync_mode(true);
        if (obj.subjects_initialized()) {
            lv_subject_set_int(&obj.chamber_humidity_, -1);
        }
    }
};
} // namespace helix::sensors

namespace {

constexpr uint8_t kNone = static_cast<uint8_t>(HumiditySensorRole::NONE);
constexpr uint8_t kChamber = static_cast<uint8_t>(HumiditySensorRole::CHAMBER);
constexpr uint8_t kDryer = static_cast<uint8_t>(HumiditySensorRole::DRYER);

PrinterDiscovery discovery_of(const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(json(objects));
    return hw;
}

class HumiditySensorTestFixture : public LVGLTestFixture {
  public:
    HumiditySensorTestFixture() {
        mgr().init_subjects();
        HumiditySensorManagerTestAccess::reset(mgr());
    }

    ~HumiditySensorTestFixture() override {
        HumiditySensorManagerTestAccess::reset(mgr());
    }

  protected:
    HumiditySensorManager& mgr() {
        return HumiditySensorManager::instance();
    }

    void discover(const std::vector<std::string>& objects) {
        mgr().discover(discovery_of(objects));
    }

    void discover_test_sensors() {
        discover({"bme280 chamber", "htu21d dryer"});
    }

    void update_sensor_state(const std::string& klipper_name, float humidity, float temperature,
                             float pressure = 0.0f) {
        json status;
        status[klipper_name]["humidity"] = humidity;
        status[klipper_name]["temperature"] = temperature;
        if (pressure > 0.0f) {
            status[klipper_name]["pressure"] = pressure;
        }
        mgr().update_from_status(status);
    }

    SensorEntry entry(const std::string& klipper_name) {
        SensorEntry e;
        REQUIRE(mgr().find(klipper_name, e));
        return e;
    }

    int chamber() {
        return lv_subject_get_int(mgr().get_chamber_humidity_subject());
    }
};

} // namespace

// ============================================================================
// Type Helper Tests (humidity_sensor_types.h)
// ============================================================================

TEST_CASE("HumiditySensorTypes - role string conversion", "[humidity][types]") {
    SECTION("humidity_role_to_string") {
        REQUIRE(humidity_role_to_string(HumiditySensorRole::NONE) == "none");
        REQUIRE(humidity_role_to_string(HumiditySensorRole::CHAMBER) == "chamber");
        REQUIRE(humidity_role_to_string(HumiditySensorRole::DRYER) == "dryer");
    }

    SECTION("humidity_role_from_string") {
        REQUIRE(humidity_role_from_string("none") == HumiditySensorRole::NONE);
        REQUIRE(humidity_role_from_string("chamber") == HumiditySensorRole::CHAMBER);
        REQUIRE(humidity_role_from_string("dryer") == HumiditySensorRole::DRYER);
        REQUIRE(humidity_role_from_string("invalid") == HumiditySensorRole::NONE);
        REQUIRE(humidity_role_from_string("") == HumiditySensorRole::NONE);
    }

    SECTION("humidity_role_to_display_string") {
        REQUIRE(humidity_role_to_display_string(HumiditySensorRole::NONE) == "Unassigned");
        REQUIRE(humidity_role_to_display_string(HumiditySensorRole::CHAMBER) == "Chamber");
        REQUIRE(humidity_role_to_display_string(HumiditySensorRole::DRYER) == "Dryer");
    }
}

TEST_CASE("HumiditySensorTypes - type string conversion", "[humidity][types]") {
    SECTION("humidity_type_to_string") {
        REQUIRE(humidity_type_to_string(HumiditySensorType::BME280) == "bme280");
        REQUIRE(humidity_type_to_string(HumiditySensorType::HTU21D) == "htu21d");
        REQUIRE(humidity_type_to_string(HumiditySensorType::SHT3X) == "sht3x");
        REQUIRE(humidity_type_to_string(HumiditySensorType::AHT10) == "aht10");
        REQUIRE(humidity_type_to_string(HumiditySensorType::AHT20) == "aht20");
        REQUIRE(humidity_type_to_string(HumiditySensorType::AHT20_F) == "aht20_f");
    }

    SECTION("humidity_type_from_string") {
        REQUIRE(humidity_type_from_string("bme280") == HumiditySensorType::BME280);
        REQUIRE(humidity_type_from_string("htu21d") == HumiditySensorType::HTU21D);
        REQUIRE(humidity_type_from_string("sht3x") == HumiditySensorType::SHT3X);
        REQUIRE(humidity_type_from_string("aht10") == HumiditySensorType::AHT10);
        REQUIRE(humidity_type_from_string("aht20") == HumiditySensorType::AHT20);
        REQUIRE(humidity_type_from_string("aht20_f") == HumiditySensorType::AHT20_F);
        REQUIRE(humidity_type_from_string("invalid") == HumiditySensorType::BME280);
        REQUIRE(humidity_type_from_string("") == HumiditySensorType::BME280);
    }

    SECTION("humidity_type_to_string / from_string round-trip") {
        for (auto t :
             {HumiditySensorType::BME280, HumiditySensorType::HTU21D, HumiditySensorType::SHT3X,
              HumiditySensorType::AHT10, HumiditySensorType::AHT20, HumiditySensorType::AHT20_F}) {
            REQUIRE(humidity_type_from_string(humidity_type_to_string(t)) == t);
        }
    }

    SECTION("humidity_type_to_display_string") {
        REQUIRE(humidity_type_to_display_string(HumiditySensorType::BME280) == "BME280");
        REQUIRE(humidity_type_to_display_string(HumiditySensorType::HTU21D) == "HTU21D");
        REQUIRE(humidity_type_to_display_string(HumiditySensorType::SHT3X) == "SHT3X");
        REQUIRE(humidity_type_to_display_string(HumiditySensorType::AHT10) == "AHT10");
        REQUIRE(humidity_type_to_display_string(HumiditySensorType::AHT20) == "AHT20");
        REQUIRE(humidity_type_to_display_string(HumiditySensorType::AHT20_F) == "AHT20-F");
    }
}

// ============================================================================
// Sensor Discovery Tests
// ============================================================================

// ============================================================================
// Discovery
// ============================================================================

TEST_CASE_METHOD(HumiditySensorTestFixture, "HumiditySensorManager - discovery",
                 "[humidity][discovery]") {
    struct Case {
        const char* object;
        const char* short_name;
        HumiditySensorType type;
    };
    const Case cases[] = {
        {"bme280 chamber", "chamber", HumiditySensorType::BME280},
        {"htu21d dryer", "dryer", HumiditySensorType::HTU21D},
        {"sht3x box", "box", HumiditySensorType::SHT3X},
        {"aht10 box", "box", HumiditySensorType::AHT10},
        {"aht20 box", "box", HumiditySensorType::AHT20},
        {"aht20_f heater_box1", "heater_box1", HumiditySensorType::AHT20_F},
        {"bme280 my_custom_name", "my_custom_name", HumiditySensorType::BME280},
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION("Discovers " << c.object) {
            discover({c.object});
            REQUIRE(mgr().sensor_count() == 1);
            const auto e = entry(c.object);
            CHECK(e.display_name == c.short_name);
            CHECK(e.type == static_cast<uint8_t>(c.type));
            CHECK(e.enabled);
        }
    }

    SECTION("Ignores unrelated objects") {
        discover({"bme280 chamber", "temperature_sensor foo", "extruder"});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().entries()[0].display_name == "chamber");
    }

    SECTION("Empty sensor list clears previous sensors") {
        discover_test_sensors();
        REQUIRE(mgr().sensor_count() == 2);
        discover({});
        REQUIRE(mgr().sensor_count() == 0);
    }

    SECTION("Re-discovery replaces sensor list") {
        discover({"bme280 chamber"});
        REQUIRE(mgr().entries()[0].display_name == "chamber");
        discover({"htu21d dryer"});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().entries()[0].display_name == "dryer");
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
// Roles
// ============================================================================

TEST_CASE_METHOD(HumiditySensorTestFixture, "HumiditySensorManager - roles", "[humidity][roles]") {
    SECTION("Sensors named chamber and dryer take CHAMBER and DRYER") {
        discover_test_sensors();
        CHECK(entry("bme280 chamber").role == kChamber);
        CHECK(entry("htu21d dryer").role == kDryer);
    }

    SECTION("A sensor with neither name holds no role") {
        discover({"aht10 box"});
        CHECK(entry("aht10 box").role == kNone);
    }

    SECTION("The first sensor with the name takes the role") {
        discover({"bme280 chamber_a", "htu21d chamber_b"});
        CHECK(entry("bme280 chamber_a").role == kChamber);
        CHECK(entry("htu21d chamber_b").role == kNone);
    }

    SECTION("Roles are not offered in Settings") {
        CHECK(mgr().offered_roles().empty());
    }

    SECTION("Roles are exclusive") {
        discover_test_sensors();
        mgr().set_role("htu21d dryer", kChamber);
        CHECK(entry("htu21d dryer").role == kChamber);
        CHECK(entry("bme280 chamber").role == kNone);
    }

    SECTION("Assigning a role to an unknown sensor changes nothing") {
        discover_test_sensors();
        mgr().set_role("nonexistent_sensor", kChamber);
        CHECK(mgr().sensor_count() == 2);
        CHECK(entry("bme280 chamber").role == kChamber);
    }

    SECTION("Humidity sensors are not persisted") {
        discover_test_sensors();
        mgr().save_settings();
        mgr().load_settings();
        CHECK(entry("bme280 chamber").role == kChamber);
    }
}

// ============================================================================
// Status and subject
// ============================================================================

TEST_CASE_METHOD(HumiditySensorTestFixture, "HumiditySensorManager - chamber humidity subject",
                 "[humidity][state]") {
    SECTION("0 when the CHAMBER sensor has reported nothing yet") {
        discover_test_sensors();
        CHECK(chamber() == 0);
    }

    SECTION("-1 with no sensors") {
        CHECK(chamber() == -1);
    }

    SECTION("Follows the chamber sensor (humidity x 10)") {
        discover_test_sensors();
        update_sensor_state("bme280 chamber", 45.5f, 25.0f, 1013.25f);
        CHECK(chamber() == 455);
        update_sensor_state("bme280 chamber", 52.3f, 25.0f);
        CHECK(chamber() == 523);
    }

    SECTION("The dryer sensor does not move the chamber subject") {
        discover_test_sensors();
        update_sensor_state("htu21d dryer", 20.1f, 55.0f);
        CHECK(chamber() == 0);
    }

    SECTION("-1 when the chamber sensor is disabled") {
        discover_test_sensors();
        update_sensor_state("bme280 chamber", 45.5f, 25.0f);
        mgr().set_enabled("bme280 chamber", false);
        CHECK(chamber() == -1);
        mgr().set_enabled("bme280 chamber", true);
        CHECK(chamber() == 455);
    }

    SECTION("Null fields in a field-restricted update are ignored") {
        discover_test_sensors();
        update_sensor_state("bme280 chamber", 45.5f, 25.0f);
        json status;
        status["bme280 chamber"]["humidity"] = nullptr;
        status["bme280 chamber"]["temperature"] = 26.0f;
        mgr().update_from_status(status);
        CHECK(chamber() == 455);
    }

    SECTION("Status for an unknown sensor or an empty status is ignored") {
        discover_test_sensors();
        update_sensor_state("bme280 chamber", 45.5f, 25.0f);
        update_sensor_state("bme280 other", 99.0f, 25.0f);
        mgr().update_from_status(json::object());
        CHECK(chamber() == 455);
        CHECK(mgr().sensor_count() == 2);
    }

    SECTION("Re-discovery with the chamber sensor gone resets the subject") {
        discover_test_sensors();
        update_sensor_state("bme280 chamber", 45.5f, 25.0f);
        discover({"htu21d dryer"});
        CHECK(chamber() == -1);
    }

    SECTION("A chamber sensor that comes back starts without its old reading") {
        discover_test_sensors();
        update_sensor_state("bme280 chamber", 45.5f, 25.0f);
        discover({"htu21d dryer"});
        discover_test_sensors();
        CHECK(chamber() == 0);
    }
}

// Outside sync mode a status update reaches the subject only through the queue,
// and a deferral queued before deinit_subjects() is dropped.
TEST_CASE_METHOD(HumiditySensorTestFixture, "HumiditySensorManager - status refresh is deferred",
                 "[humidity][lifetime]") {
    discover_test_sensors();
    mgr().set_sync_mode(false);

    update_sensor_state("bme280 chamber", 45.0f, 25.0f);
    CHECK(chamber() == 0);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(chamber() == 450);

    update_sensor_state("bme280 chamber", 50.0f, 25.0f);
    mgr().deinit_subjects();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    mgr().init_subjects();
    mgr().set_sync_mode(true);
    CHECK(chamber() == -1);
}

TEST_CASE("HumiditySensorManager - is in the sensor registry", "[humidity][sensor_manager_base]") {
    const auto& managers = sensor_managers();
    CHECK(std::find(managers.begin(), managers.end(), &HumiditySensorManager::instance()) !=
          managers.end());
}
