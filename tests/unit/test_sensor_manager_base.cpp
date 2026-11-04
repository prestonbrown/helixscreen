// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_sensor_manager_base.cpp
 * @brief SensorManager base against a small test manager: discovery, reconcile,
 *        role exclusivity, publish threading, settings persistence, row model,
 *        the registry and DynamicSubjectMap.
 *
 * Run with: ./build/bin/helix-tests "[sensor_manager_base]"
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/golden_json.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "accel_sensor_manager.h"
#include "config.h"
#include "dynamic_subject_map.h"
#include "filament_sensor_manager.h"
#include "input_sensor_manager.h"
#include "printer_discovery.h"
#include "probe_sensor_manager.h"
#include "sensor_manager.h"
#include "width_sensor_manager.h"

#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

namespace {

constexpr uint8_t kChamber = 1;
constexpr uint8_t kAux = 2;

class TestSensorManager : public SensorManager {
  public:
    using Options = SensorManager::Options;

    static SensorManager::Options plain_options() {
        SensorManager::Options o;
        o.name = "TestSensorManager";
        o.kinds = {hw::ObjectKind::TemperatureSensor};
        o.count_subject = "test_sensor_count";
        o.revision_subject = "test_sensor_revision";
        o.config_key = "test_sensors";
        o.roles = {{kChamber, "chamber", "Chamber", true, true},
                   {kAux, "aux", "Auxiliary", false, false}};
        o.types = {{0, "basic", "Basic"}};
        return o;
    }

    explicit TestSensorManager(SensorManager::Options o = plain_options())
        : SensorManager(std::move(o)) {}

    int refresh_count = 0;
    bool probe_mutex = false;
    bool mutex_was_free = false;
    int value = 0;

    void publish_now() {
        publish();
    }

  protected:
    void register_subjects(SubjectManager& /*subjects*/) override {}

    void refresh_subjects() override {
        ++refresh_count;
        if (probe_mutex) {
            std::thread other([this] {
                mutex_was_free = mutex().try_lock();
                if (mutex_was_free) {
                    mutex().unlock();
                }
            });
            other.join();
        }
    }

    bool parse_status(const json& status) override {
        bool changed = false;
        for (const auto& entry : entries_locked()) {
            const auto it = status.find(entry.klipper_name);
            if (it != status.end() && it->is_object()) {
                const auto t = it->find("temperature");
                if (t != it->end() && t->is_number_integer() && t->get<int>() != value) {
                    value = t->get<int>();
                    changed = true;
                }
            }
        }
        return changed;
    }
};

PrinterDiscovery discovery_of(const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(json(objects));
    return hw;
}

json load_fixture(const std::string& name) {
    std::ifstream in(helix::test::golden_path("sensor_settings", name));
    INFO("fixture " << name);
    REQUIRE(in.good());
    json j = json::parse(in, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    return j;
}

json& config_slot(const char* key) {
    Config* cfg = Config::get_instance();
    return cfg->get_json(cfg->df() + key);
}

class SensorManagerFixture : public LVGLTestFixture {
  public:
    SensorManagerFixture() {
        saved_slot_ = config_slot("test_sensors");
        mgr.set_sync_mode(true);
        mgr.init_subjects();
    }
    ~SensorManagerFixture() override {
        mgr.deinit_subjects();
        config_slot("test_sensors") = saved_slot_;
    }

  protected:
    void discover(const std::vector<std::string>& objects) {
        mgr.discover(discovery_of(objects));
    }
    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    uint8_t role_of_name(const std::string& name) {
        SensorEntry e;
        REQUIRE(mgr.find(name, e));
        return e.role;
    }

    TestSensorManager mgr;
    json saved_slot_;
};

const std::vector<std::string> kTwoSensors = {"extruder", "temperature_sensor chamber",
                                              "heater_bed", "temperature_sensor board"};

} // namespace

// ============================================================================
// Discovery and reconcile
// ============================================================================

TEST_CASE_METHOD(SensorManagerFixture, "SensorManager: discover adopts objects of its kinds",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    CHECK(lv_subject_get_int(mgr.count_subject()) == 2);
    CHECK(mgr.sensor_count() == 2);
    const auto entries = mgr.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].klipper_name == "temperature_sensor chamber");
    CHECK(entries[0].display_name == "chamber");
    CHECK(entries[0].available);
    CHECK(entries[1].klipper_name == "temperature_sensor board");
}

TEST_CASE_METHOD(SensorManagerFixture,
                 "SensorManager: reconcile drops a vanished sensor and keeps survivors",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    mgr.set_role("temperature_sensor board", kChamber);
    REQUIRE(role_of_name("temperature_sensor board") == kChamber);

    discover({"extruder", "temperature_sensor board"});
    CHECK(mgr.sensor_count() == 1);
    CHECK(lv_subject_get_int(mgr.count_subject()) == 1);
    SensorEntry gone;
    CHECK_FALSE(mgr.find("temperature_sensor chamber", gone));
    CHECK(role_of_name("temperature_sensor board") == kChamber);
}

// ============================================================================
// Roles
// ============================================================================

TEST_CASE_METHOD(SensorManagerFixture,
                 "SensorManager: set_role takes an exclusive role from the other holder",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    mgr.set_role("temperature_sensor chamber", kChamber);
    REQUIRE(role_of_name("temperature_sensor chamber") == kChamber);

    mgr.set_role("temperature_sensor board", kChamber);
    CHECK(role_of_name("temperature_sensor board") == kChamber);
    CHECK(role_of_name("temperature_sensor chamber") == 0);
}

TEST_CASE_METHOD(SensorManagerFixture, "SensorManager: a shared role keeps both holders",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    mgr.set_role("temperature_sensor chamber", kAux);
    mgr.set_role("temperature_sensor board", kAux);
    CHECK(role_of_name("temperature_sensor chamber") == kAux);
    CHECK(role_of_name("temperature_sensor board") == kAux);
}

// ============================================================================
// Publishing and threading
// ============================================================================

TEST_CASE_METHOD(SensorManagerFixture, "SensorManager: sync mode refreshes inline",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    const int before = mgr.refresh_count;
    const int revision = lv_subject_get_int(mgr.revision_subject());
    mgr.update_from_status(json{{"temperature_sensor chamber", {{"temperature", 40}}}});
    CHECK(mgr.refresh_count == before + 1);
    CHECK(lv_subject_get_int(mgr.revision_subject()) == revision + 1);

    // No change: no publish.
    mgr.update_from_status(json{{"temperature_sensor chamber", {{"temperature", 40}}}});
    CHECK(mgr.refresh_count == before + 1);
}

TEST_CASE_METHOD(SensorManagerFixture, "SensorManager: deferred refresh runs on the queue",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    mgr.set_sync_mode(false);
    REQUIRE_FALSE(SensorManagerTestAccess::sync_mode(mgr));
    const int before = mgr.refresh_count;
    const int revision = lv_subject_get_int(mgr.revision_subject());

    mgr.update_from_status(json{{"temperature_sensor board", {{"temperature", 55}}}});
    CHECK(mgr.refresh_count == before);
    CHECK(lv_subject_get_int(mgr.revision_subject()) == revision);

    drain();
    CHECK(mgr.refresh_count == before + 1);
    CHECK(lv_subject_get_int(mgr.revision_subject()) == revision + 1);
}

TEST_CASE_METHOD(SensorManagerFixture, "SensorManager: publish after deinit is dropped",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    mgr.set_sync_mode(false);
    mgr.update_from_status(json{{"temperature_sensor board", {{"temperature", 60}}}});
    const int before = mgr.refresh_count;

    // The deferral queued above must not land on the subjects a later init creates:
    // only init's own publish runs.
    mgr.deinit_subjects();
    mgr.init_subjects();
    drain();
    CHECK(mgr.refresh_count == before + 1);
}

TEST_CASE_METHOD(SensorManagerFixture, "SensorManager: refresh runs without the mutex held",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    mgr.probe_mutex = true;
    mgr.mutex_was_free = false;
    mgr.update_from_status(json{{"temperature_sensor board", {{"temperature", 70}}}});
    mgr.probe_mutex = false;
    CHECK(mgr.mutex_was_free);
}

TEST_CASE_METHOD(SensorManagerFixture,
                 "SensorManager: an observer may call back into the manager during refresh",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    struct Ctx {
        TestSensorManager* mgr;
        int start;
        bool fired = false;
    } ctx{&mgr, lv_subject_get_int(mgr.revision_subject())};

    lv_observer_t* obs = lv_subject_add_observer(
        mgr.revision_subject(),
        [](lv_observer_t* o, lv_subject_t* s) {
            auto* c = static_cast<Ctx*>(lv_observer_get_user_data(o));
            if (c->fired || lv_subject_get_int(s) == c->start) {
                return;
            }
            c->fired = true;
            c->mgr->set_role("temperature_sensor board", kChamber);
        },
        &ctx);

    const int before = mgr.refresh_count;
    mgr.update_from_status(json{{"temperature_sensor chamber", {{"temperature", 33}}}});
    lv_observer_remove(obs);

    CHECK(ctx.fired);
    CHECK(role_of_name("temperature_sensor board") == kChamber);
    CHECK(mgr.refresh_count == before + 2);
}

// ============================================================================
// Settings persistence
// ============================================================================

// Each fixture is a settings shape users have on disk today. It goes through the
// real manager: discovery of its sensors, load_settings(), then save_settings().
TEST_CASE("SensorManager: settings round trip through every persisted manager is byte-identical",
          "[sensor_manager_base]") {
    LVGLTestFixture lvgl;
    struct Case {
        const char* fixture;
        const char* config_key;
        SensorManager* manager;
    };
    const std::vector<Case> cases = {
        {"filament", "filament_sensors", &FilamentSensorManager::instance()},
        {"probe", "probe_sensors", &ProbeSensorManager::instance()},
        {"width", "width_sensors", &WidthSensorManager::instance()},
        {"input", "input_sensors", &InputSensorManager::instance()},
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.fixture) {
            const json fixture = load_fixture(c.fixture);
            REQUIRE(fixture["sensors"].size() > 0);
            std::vector<std::string> objects;
            for (const auto& sensor : fixture["sensors"]) {
                objects.push_back(sensor["klipper_name"].get<std::string>());
            }

            SensorManager& m = *c.manager;
            const bool sync = SensorManagerTestAccess::sync_mode(m);
            json& slot = config_slot(c.config_key);
            const json saved = slot;
            m.set_sync_mode(true);
            m.discover(discovery_of(objects));
            slot = fixture;
            m.load_settings();
            slot = json::object();
            m.save_settings();
            const std::string written = config_slot(c.config_key).dump();

            config_slot(c.config_key) = saved;
            m.discover(PrinterDiscovery{});
            m.load_settings();
            m.set_sync_mode(sync);

            INFO("expected " << fixture.dump());
            INFO("written  " << written);
            CHECK(written == fixture.dump());
        }
    }
}

// A role that is present but not a string reads as none, as an unknown role
// string does; a wrong-typed enabled is skipped.
TEST_CASE_METHOD(SensorManagerFixture,
                 "SensorManager: a wrong-typed saved role reads as none, enabled is skipped",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    config_slot("test_sensors") = json{
        {"sensors",
         json::array(
             {json{{"klipper_name", "temperature_sensor chamber"}, {"enabled", "yes"}, {"role", 7}},
              json{{"klipper_name", "temperature_sensor board"},
                   {"enabled", false},
                   {"role", "chamber"}}})}};
    mgr.set_role("temperature_sensor chamber", kAux);
    mgr.load_settings();

    SensorEntry chamber;
    REQUIRE(mgr.find("temperature_sensor chamber", chamber));
    CHECK(chamber.enabled);
    CHECK(chamber.role == 0);
    SensorEntry board;
    REQUIRE(mgr.find("temperature_sensor board", board));
    CHECK_FALSE(board.enabled);
    CHECK(board.role == kChamber);
}

TEST_CASE_METHOD(SensorManagerFixture, "SensorManager: an unknown saved role reads as none",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    mgr.set_role("temperature_sensor chamber", kChamber);
    config_slot("test_sensors") = json{
        {"sensors",
         json::array({json{{"klipper_name", "temperature_sensor chamber"}, {"role", "z_probe"}}})}};
    mgr.load_settings();
    CHECK(role_of_name("temperature_sensor chamber") == 0);
    CHECK(SensorManagerTestAccess::settings_json(mgr)["sensors"][0]["role"] == "none");
}

// ============================================================================
// Settings rows
// ============================================================================

TEST_CASE_METHOD(SensorManagerFixture,
                 "SensorManager: offered_roles lists offered roles in RoleSpec order",
                 "[sensor_manager_base]") {
    CHECK(mgr.offered_roles() == std::vector<uint8_t>{kChamber});

    TestSensorManager::Options o = TestSensorManager::plain_options();
    o.name = "TestSensorManagerOffered";
    o.roles = {{0, "none", "Unassigned", false, true},
               {3, "third", "Third", true, true},
               {2, "hidden", "Hidden", false, false},
               {1, "first", "First", true, true}};
    TestSensorManager other(std::move(o));
    CHECK(other.offered_roles() == std::vector<uint8_t>{0, 3, 1});
}

TEST_CASE_METHOD(SensorManagerFixture,
                 "SensorManager: row shows a held role that is not offered as held_role_label",
                 "[sensor_manager_base]") {
    discover(kTwoSensors);
    mgr.set_role("temperature_sensor board", kAux);
    mgr.set_role("temperature_sensor chamber", kChamber);
    mgr.set_enabled("temperature_sensor chamber", false);

    const SensorRowView board = mgr.row(1);
    CHECK(board.name == "board");
    CHECK(board.detail == "Basic");
    CHECK(board.role_count == 1);
    CHECK(board.selected_role == -1);
    CHECK(board.held_role_label == "Auxiliary");

    const SensorRowView chamber = mgr.row(0);
    CHECK(chamber.selected_role == 0);
    CHECK(chamber.held_role_label.empty());
    CHECK(chamber.toggle == RowToggle::Enabled);
    CHECK_FALSE(chamber.toggle_value);

    CHECK(mgr.row(5).name.empty());
}

// ============================================================================
// Registry
// ============================================================================

TEST_CASE_METHOD(SensorManagerFixture, "SensorManager: the registry walks registered managers",
                 "[sensor_manager_base]") {
    const size_t before = sensor_managers().size();
    SensorManagerTestAccess::add_to_registry(&mgr);
    REQUIRE(sensor_managers().size() == before + 1);
    CHECK(sensor_managers().back() == &mgr);

    const auto hw = discovery_of(kTwoSensors);
    for (auto* m : sensor_managers()) {
        if (m == &mgr) {
            m->discover(hw);
        }
    }
    SensorManagerTestAccess::remove_from_registry(&mgr);
    CHECK(sensor_managers().size() == before);
    CHECK(mgr.sensor_count() == 2);
}

// ============================================================================
// DynamicSubjectMap
// ============================================================================

TEST_CASE("DynamicSubjectMap removes subjects for vanished keys and expires their lifetime",
          "[sensor_manager_base]") {
    LVGLTestFixture lvgl;
    DynamicSubjectMap map;

    SubjectLifetime a_life;
    SubjectLifetime b_life;
    lv_subject_t* a = map.get_or_create("a", 5, a_life);
    lv_subject_t* b = map.get_or_create("b", 7, b_life);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(lv_subject_get_int(a) == 5);
    CHECK(lv_subject_get_int(b) == 7);
    REQUIRE(a_life);
    REQUIRE(*a_life);
    REQUIRE(*b_life);

    // Same key: same subject, value untouched by the new initial.
    SubjectLifetime again;
    CHECK(map.get_or_create("a", 99, again) == a);
    CHECK(lv_subject_get_int(a) == 5);
    CHECK(again == a_life);

    map.remove_missing({"a"});
    CHECK(map.size() == 1);
    CHECK_FALSE(*b_life);
    CHECK(*a_life);
    SubjectLifetime gone;
    CHECK(map.find("b", gone) == nullptr);
    CHECK_FALSE(gone);
    SubjectLifetime found;
    CHECK(map.find("a", found) == a);
    CHECK(found == a_life);

    map.clear();
    CHECK(map.size() == 0);
    CHECK_FALSE(*a_life);
}

// ============================================================================
// The configfile pass
// ============================================================================

TEST_CASE("Only managers that read config sections rediscover on the configfile pass",
          "[sensor_manager_base]") {
    for (auto* m : sensor_managers()) {
        const std::string name = m->name();
        const bool reads_sections = name == "AccelSensorManager" || name == "ProbeSensorManager" ||
                                    name == "InputSensorManager";
        INFO(name);
        CHECK(m->rediscovers_on_config() == reads_sections);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "The configfile pass hands accel, probe and input their config sections",
                 "[sensor_manager_base]") {
    auto& accel = AccelSensorManager::instance();
    auto& probe = ProbeSensorManager::instance();
    auto& input = InputSensorManager::instance();
    for (SensorManager* m : std::vector<SensorManager*>{&accel, &probe, &input}) {
        m->init_subjects();
        m->set_sync_mode(true);
        m->discover(PrinterDiscovery{});
    }

    PrinterDiscovery hw;
    hw.parse_objects(json::array({"probe", "gcode_button frontDoor"}));
    for (SensorManager* m : std::vector<SensorManager*>{&accel, &probe, &input}) {
        m->discover(hw);
    }
    REQUIRE(accel.sensor_count() == 0);
    REQUIRE(probe.get_z_offset() == 0.0f);
    REQUIRE_FALSE(input.firmware_reacts_to_opening("gcode_button frontDoor"));

    PrinterDiscovery configured = hw;
    configured.parse_config_keys(
        json{{"adxl345", json::object()},
             {"probe", {{"z_offset", "1.5"}}},
             {"gcode_button frontDoor", {{"release_gcode", "\nRESPOND MSG=open"}}}});
    rediscover_from_config(configured);

    CHECK(accel.sensor_count() == 1);
    CHECK(probe.get_z_offset() == Catch::Approx(1.5f));
    CHECK(input.firmware_reacts_to_opening("gcode_button frontDoor"));

    for (SensorManager* m : std::vector<SensorManager*>{&accel, &probe, &input}) {
        m->discover(PrinterDiscovery{});
    }
}

TEST_CASE_METHOD(SensorManagerFixture,
                 "SensorManager: saved_sensor_names lists saved sensors discovery does not",
                 "[sensor_manager_base]") {
    discover({"extruder", "temperature_sensor chamber"});
    config_slot("test_sensors") =
        json{{"sensors", json::array({json{{"klipper_name", "temperature_sensor chamber"}},
                                      json{{"klipper_name", "temperature_sensor gone"}},
                                      json{{"klipper_name", 7}}, json("not an object"),
                                      json{{"role", "chamber"}}})}};
    const std::vector<std::string> expected = {"temperature_sensor chamber",
                                               "temperature_sensor gone"};
    CHECK(mgr.saved_sensor_names() == expected);
    SensorEntry gone;
    CHECK_FALSE(mgr.find("temperature_sensor gone", gone));
}

TEST_CASE_METHOD(SensorManagerFixture,
                 "SensorManager: saved_sensor_names is empty with nothing saved",
                 "[sensor_manager_base]") {
    config_slot("test_sensors") = json::object();
    CHECK(mgr.saved_sensor_names().empty());

    TestSensorManager::Options o = TestSensorManager::plain_options();
    o.config_key = nullptr;
    TestSensorManager unpersisted(std::move(o));
    config_slot("test_sensors") =
        json{{"sensors", json::array({json{{"klipper_name", "temperature_sensor chamber"}}})}};
    CHECK(unpersisted.saved_sensor_names().empty());
}
