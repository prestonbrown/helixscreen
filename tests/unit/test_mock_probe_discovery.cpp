// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_probe_discovery.cpp
 * @brief Mock/production parity for probe discovery from configfile
 *
 * MoonrakerClientMock::discover_printer() is a shortcut past the real
 * MoonrakerDiscoverySequence. The real sequence queries
 * `configfile.config` and hands it to ProbeSensorManager::discover_from_config()
 * so a probe whose runtime status reports a null z_offset (flashforge_loadcell
 * is the motivating case) still gets its persisted offset. Nothing exercised
 * that under --test, which is exactly where it would be verified.
 *
 * Two halves, tested separately so a failure says which one broke:
 *  1. the mock's configfile payload actually carries a probe section
 *  2. discover_printer() feeds that section to ProbeSensorManager
 */

#include "ui_update_queue.h"

#include "../../src/api/moonraker_client_mock_internal.h"
#include "../lvgl_test_fixture.h"
#include "moonraker_client_mock.h"
#include "probe_sensor_manager.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

namespace {

/// Scoped HELIX_MOCK_PROBE_TYPE override. The mock keys its objects list, its
/// status payload and (now) its configfile payload off this one variable, so a
/// leak here changes probe behaviour for every later test in the process.
class ScopedProbeType {
  public:
    explicit ScopedProbeType(const char* value) {
        if (const char* prev = std::getenv("HELIX_MOCK_PROBE_TYPE")) {
            had_prev_ = true;
            prev_ = prev;
        }
        setenv("HELIX_MOCK_PROBE_TYPE", value, 1);
    }
    ~ScopedProbeType() {
        if (had_prev_) {
            setenv("HELIX_MOCK_PROBE_TYPE", prev_.c_str(), 1);
        } else {
            unsetenv("HELIX_MOCK_PROBE_TYPE");
        }
    }

  private:
    bool had_prev_ = false;
    std::string prev_;
};

class MockProbeDiscoveryFixture : public LVGLTestFixture {
  public:
    MockProbeDiscoveryFixture() {
        ProbeSensorManager::instance().init_subjects();
    }

    ~MockProbeDiscoveryFixture() override {
        // Leave no discovered probes behind for the next test in this process.
        ProbeSensorManager::instance().discover({});
        helix::ui::UpdateQueue::instance().drain();
    }

  protected:
    /// What PrinterSession::setup_discovery_callbacks() does for probes: on
    /// hardware discovery, queue ProbeSensorManager::discover() onto the main
    /// thread. Registering it makes the ordering under test real — the seeding
    /// callback must land AFTER this one or there are no sensors to seed.
    static void wire_app_probe_discovery(MoonrakerClientMock& client) {
        client.set_on_hardware_discovered([](const helix::PrinterDiscovery& hardware) {
            auto objects = hardware.printer_objects();
            helix::ui::queue_update([objects]() {
                auto& psm = ProbeSensorManager::instance();
                psm.discover(objects);
                psm.load_config_from_file();
            });
        });
    }
};

} // namespace

// ============================================================================
// 1. The mock's configfile payload carries a probe section
// ============================================================================

TEST_CASE_METHOD(MockProbeDiscoveryFixture,
                 "Mock configfile.config carries a probe section with z_offset",
                 "[mock][probe][config]") {
    ScopedProbeType probe_type("bltouch");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);

    json response;
    client.send_jsonrpc("printer.objects.query",
                        {{"objects", json::object({{"configfile", json::array({"config"})}})}},
                        [&response](const json& r) { response = r; });

    REQUIRE(response.contains("result"));
    const json& cfg = response["result"]["status"]["configfile"]["config"];

    // Klipper reports configfile.config values as strings — ProbeSensorManager
    // parses them with std::stof, so a raw number here would silently drop the
    // section on the real path and pass on the mock path.
    REQUIRE(cfg.contains("bltouch"));
    REQUIRE(cfg["bltouch"].contains("z_offset"));
    REQUIRE(cfg["bltouch"]["z_offset"].is_string());
    REQUIRE(std::stof(cfg["bltouch"]["z_offset"].get<std::string>()) == Catch::Approx(-1.850f));
}

TEST_CASE_METHOD(MockProbeDiscoveryFixture,
                 "Mock configfile probe section follows HELIX_MOCK_PROBE_TYPE",
                 "[mock][probe][config]") {
    SECTION("loadcell reports its offset in config only") {
        ScopedProbeType probe_type("loadcell");
        MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);

        json response;
        client.send_jsonrpc("printer.objects.query",
                            {{"objects", json::object({{"configfile", json::array({"config"})}})}},
                            [&response](const json& r) { response = r; });

        const json& cfg = response["result"]["status"]["configfile"]["config"];
        REQUIRE(cfg.contains("probe"));
        REQUIRE(std::stof(cfg["probe"]["z_offset"].get<std::string>()) == Catch::Approx(-0.185f));
    }

    SECTION("none emits no probe section at all") {
        ScopedProbeType probe_type("none");
        MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);

        json response;
        client.send_jsonrpc("printer.objects.query",
                            {{"objects", json::object({{"configfile", json::array({"config"})}})}},
                            [&response](const json& r) { response = r; });

        const json& cfg = response["result"]["status"]["configfile"]["config"];
        REQUIRE_FALSE(cfg.contains("probe"));
        REQUIRE_FALSE(cfg.contains("bltouch"));
        REQUIRE_FALSE(cfg.contains("cartographer"));
        REQUIRE_FALSE(cfg.contains("beacon"));
    }
}

// ============================================================================
// 2. discover_printer() feeds that section to ProbeSensorManager
// ============================================================================

TEST_CASE_METHOD(MockProbeDiscoveryFixture,
                 "MoonrakerClientMock::discover_printer seeds probe z_offset from configfile",
                 "[mock][probe][discovery]") {
    ScopedProbeType probe_type("bltouch");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    wire_app_probe_discovery(client);

    bool discovered = false;
    client.discover_printer([&discovered]() { discovered = true; }, nullptr);
    REQUIRE(discovered);

    helix::ui::UpdateQueue::instance().drain();

    auto& psm = ProbeSensorManager::instance();
    REQUIRE(psm.has_sensors());
    psm.set_sensor_role("bltouch", ProbeSensorRole::Z_PROBE);

    // No status update has arrived (discover_printer does not dispatch one), so
    // the only possible source of a non-zero offset is the configfile seed.
    REQUIRE(psm.get_z_offset() == Catch::Approx(-1.850f));
}

TEST_CASE_METHOD(MockProbeDiscoveryFixture,
                 "Mock probe seeding survives a status update that reports null z_offset",
                 "[mock][probe][discovery]") {
    // The flashforge_loadcell shape the real seeding exists for: Klipper's
    // runtime status has z_offset: null, and the configfile is the only place
    // the persisted value lives.
    ScopedProbeType probe_type("loadcell");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    wire_app_probe_discovery(client);

    bool discovered = false;
    client.discover_printer([&discovered]() { discovered = true; }, nullptr);
    REQUIRE(discovered);
    helix::ui::UpdateQueue::instance().drain();

    auto& psm = ProbeSensorManager::instance();
    REQUIRE(psm.has_sensors());
    psm.set_sensor_role("probe", ProbeSensorRole::Z_PROBE);
    REQUIRE(psm.get_z_offset() == Catch::Approx(-0.185f));

    json status;
    status["probe"]["last_z_result"] = 0.0;
    status["probe"]["z_offset"] = nullptr;
    psm.update_from_status(status);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(psm.get_z_offset() == Catch::Approx(-0.185f));
}

// ============================================================================
// 3. The mock's probe status matches what each upstream module publishes
// ============================================================================

namespace {

std::set<std::string> keys_of(const json& obj) {
    std::set<std::string> keys;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        keys.insert(it.key());
    }
    return keys;
}

} // namespace

TEST_CASE("Mock probe status carries the keys real firmware publishes",
          "[mock][probe][subscription]") {
    using Keys = std::set<std::string>;
    const Keys helper_keys = {"name", "last_query", "last_probe_position", "last_z_result"};

    SECTION("bltouch registers bltouch and the probe alias, same ProbeCommandHelper status") {
        ScopedProbeType probe_type("bltouch");
        const json st = helix::sim::mock_probe_status();
        REQUIRE(keys_of(st) == Keys{"bltouch", "probe"});
        REQUIRE(keys_of(st["bltouch"]) == helper_keys);
        REQUIRE(st["bltouch"] == st["probe"]);
        REQUIRE(st["bltouch"]["last_query"].is_boolean());
    }

    SECTION("generic probe publishes the ProbeCommandHelper status") {
        ScopedProbeType probe_type("tap");
        const json st = helix::sim::mock_probe_status();
        REQUIRE(keys_of(st) == Keys{"probe"});
        REQUIRE(keys_of(st["probe"]) == helper_keys);
    }

    SECTION("beacon: flat status on beacon, a name-only probe alias") {
        ScopedProbeType probe_type("beacon");
        const json st = helix::sim::mock_probe_status();
        REQUIRE(keys_of(st) == Keys{"beacon", "probe"});
        REQUIRE(keys_of(st["beacon"]) ==
                Keys{"last_sample", "last_received_sample", "last_z_result", "last_probe_position",
                     "last_probe_result", "last_offset_result", "last_poke_result", "model"});
        REQUIRE(st["probe"] == json{{"name", "beacon"}});
    }

    SECTION("cartographer: per-mode status on cartographer, flat keys on probe") {
        ScopedProbeType probe_type("cartographer");
        const json st = helix::sim::mock_probe_status();
        REQUIRE(keys_of(st) == Keys{"cartographer", "probe"});
        REQUIRE(keys_of(st["cartographer"]) == Keys{"scan", "touch", "mcu"});
        REQUIRE(keys_of(st["cartographer"]["scan"]) ==
                Keys{"current_model", "models", "last_z_result"});
        REQUIRE(keys_of(st["probe"]) == helper_keys);
        REQUIRE(st["probe"]["last_query"].is_number_integer());
    }

    SECTION("none publishes no probe object") {
        ScopedProbeType probe_type("none");
        REQUIRE(helix::sim::mock_probe_status().empty());
    }
}

TEST_CASE_METHOD(MockProbeDiscoveryFixture, "Mock objects list carries each probe object once",
                 "[mock][probe][discovery]") {
    const auto probe_count = [](const char* type) {
        ScopedProbeType probe_type(type);
        MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
        std::vector<std::string> objects;
        client.set_on_hardware_discovered(
            [&objects](const helix::PrinterDiscovery& hw) { objects = hw.printer_objects(); });
        client.discover_printer([] {}, nullptr);
        return std::count(objects.begin(), objects.end(), "probe");
    };

    SECTION("a generic probe is listed once") {
        REQUIRE(probe_count("tap") == 1);
    }
    SECTION("bltouch lists its probe alias once") {
        REQUIRE(probe_count("bltouch") == 1);
    }
    SECTION("none lists no probe object") {
        REQUIRE(probe_count("none") == 0);
    }
}
