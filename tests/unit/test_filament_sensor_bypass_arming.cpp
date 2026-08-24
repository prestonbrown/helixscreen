// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_sensor_bypass_arming.cpp
 * @brief Bypass⇄runout-sensor arming policy (FilamentSensorManager) and the
 *        CFS external-spool lane_data publish.
 *
 * Run with: ./build/bin/helix-tests "[bypass-arming]"
 *
 * Two halves of the same bypass story:
 *  1. Arming — when bypass engages, RUNOUT-role sensors the firmware holds
 *     disabled are armed via SET_FILAMENT_SENSOR and restored on disengage.
 *     Policy lives entirely in FilamentSensorManager (sensor abstraction
 *     layer); AmsState only notifies the transition.
 *  2. Slicer sync — the external spool is published as the lane one past the
 *     last CFS bay in the shared lane_data namespace so OrcaSlicer can select
 *     it. Capability dispatch via AmsBackend::publish_external_spool_lane;
 *     only CFS implements it today.
 */

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_types.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/cfs_test_access.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

// Friend shim reaching the manager's private state — same idiom as
// RunoutScopeTestAccess in test_runout_empty_lane_scope.cpp (per-TU class to
// avoid an ODR clash).
class BypassArmingTestAccess {
  public:
    static void reset(FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        mgr.sensors_.clear();
        mgr.states_.clear();
        mgr.bypass_armed_.clear();
        mgr.master_enabled_ = true;
        mgr.sync_mode_ = true;
        mgr.initial_status_received_ = false;
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }
};

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
        mgr.discover_sensors({"filament_switch_sensor filament_sensor"});
        mgr.set_sensor_role("filament_switch_sensor filament_sensor", FilamentSensorRole::RUNOUT);
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
    CHECK(fx.mgr.has_bypass_armed_sensors());
}

TEST_CASE("bypass arming: firmware-enabled sensor is left alone", "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/true);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());
    CHECK_FALSE(fx.mgr.has_bypass_armed_sensors());
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
    CHECK_FALSE(fx.mgr.has_bypass_armed_sensors());

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

// ---------------------------------------------------------------------------
// CFS external-spool lane_data publish (slicer sync)
// ---------------------------------------------------------------------------

// Friend shim for FilamentSlotOverrideStore — GLOBAL scope, matching the
// `friend class ::FilamentSlotOverrideStoreTestAccess` declaration (same
// idiom as test_ams_backend_cfs.cpp; per-TU class, no ODR clash).
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

// Friend shim for AmsBackendAfc (declared in ams_backend_afc.h) — seeds lanes
// without start(), same shape as test_ams_backend_afc.cpp's
// AmsBackendAfcTestHelper::initialize_test_lanes_with_slots. Global scope so
// the friend declaration matches.
class AfcBypassPublishTestAccess : public AmsBackendAfc {
  public:
    explicit AfcBypassPublishTestAccess(IMoonrakerAPI* api) : AmsBackendAfc(api, nullptr) {}

    void seed_lanes(int count) {
        system_info_.units.clear();
        std::vector<std::string> names;
        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Box Turtle 1";
        unit.slot_count = count;
        unit.first_slot_global_index = 0;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i + 1));
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            slot.status = SlotStatus::AVAILABLE;
            slot.mapped_tool = i;
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            unit.slots.push_back(slot);
        }
        system_info_.units.push_back(unit);
        system_info_.total_slots = count;
        slots_.initialize("Box Turtle 1", names);
        for (int i = 0; i < count; ++i) {
            auto* entry = slots_.get_mut(i);
            if (entry) {
                entry->info.mapped_tool = i;
            }
        }
    }
};

namespace {
struct CfsTmpCacheDir {
    std::filesystem::path path;
    explicit CfsTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("cfs_extlane_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~CfsTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

class CfsPublishFixture : public HelixTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<helix::printer::AmsBackendCfs> backend;
    CfsTmpCacheDir tmp{"pub"};

    CfsPublishFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        // Same shape as test_ams_backend_cfs.cpp's CfsRemapHelper: status
        // parsing (including the supports_bypass flip) is gated on running_,
        // which start() normally sets. Subclass exposes it.
        class RunningCfs : public helix::printer::AmsBackendCfs {
          public:
            RunningCfs(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
                : AmsBackendCfs(api, client) {}
            void mark_running() {
                running_ = true;
            }
        };
        auto running_backend = std::make_unique<RunningCfs>(api.get(), &client);
        running_backend->mark_running();
        backend = std::move(running_backend);
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(api.get(), "cfs");
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
        CfsTestAccess::inject_override_store(*backend, std::move(store));
    }

    ~CfsPublishFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Stock full box frame wrapped as a notify_status_update payload (same
    /// shape as make_cfs_notification in test_ams_backend_cfs.cpp): flips
    /// supports_bypass and sets total_slots (4 units x 4 lanes = 16 bays ->
    /// external lane index 16 -> "lane17").
    void seed_stock_box() {
        const json box = json::parse(R"({
            "state": "connect", "filament": 0, "enable": 1, "filament_useup": 0,
            "map": {"T1A": "T1A"},
            "T1": {"state": "connect", "filament": "None",
                   "vender": ["none"], "remain_len": ["-1"],
                   "color_value": ["-1"], "material_type": ["-1"]}})");
        CfsTestAccess::handle_status(*backend,
                                     json{{"params", json::array({json{{"box", box}}, 0})}});
    }
};
} // namespace

TEST_CASE("CFS external spool lane: publishes one past the last bay", "[ams][cfs][bypass-arming]") {
    CfsPublishFixture fx;
    fx.seed_stock_box();
    REQUIRE(fx.backend->get_system_info().supports_bypass);
    REQUIRE(fx.backend->get_system_info().total_slots > 0);
    const std::string lane_key =
        "lane" + std::to_string(fx.backend->get_system_info().total_slots + 1);

    SlotInfo spool;
    spool.material = "ASA";
    spool.color_rgb = 0x1A2B3C;
    spool.brand = "Polymaker";
    spool.spoolman_id = 7;
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();

    auto rec = fx.api->mock_get_db_value("lane_data", lane_key);
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["helix_material"] == "ASA");
    CHECK(rec["color"] == "#1A2B3C");
    CHECK(rec["vendor"] == "Polymaker");

    SECTION("clear removes the lane") {
        fx.backend->publish_external_spool_lane(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.api->mock_get_db_value("lane_data", lane_key).is_null());
    }

    SECTION("black is a real pick — publishes, unlike the gray default") {
        SlotInfo black = spool;
        black.color_rgb = 0x000000;
        black.material.clear();
        fx.backend->publish_external_spool_lane(&black);
        helix::ui::UpdateQueue::instance().drain();
        auto rec2 = fx.api->mock_get_db_value("lane_data", lane_key);
        REQUIRE_FALSE(rec2.is_null());
        CHECK(rec2["color"] == "#000000");
    }

    SECTION("identity-less record clears") {
        SlotInfo blank; // default gray, no material, no spoolman
        fx.backend->publish_external_spool_lane(&blank);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.api->mock_get_db_value("lane_data", lane_key).is_null());
    }
}

TEST_CASE("CFS external spool lane: never publishes without bypass support",
          "[ams][cfs][bypass-arming]") {
    CfsPublishFixture fx;
    // No box frame yet: supports_bypass still false, total_slots 0.
    REQUIRE_FALSE(fx.backend->get_system_info().supports_bypass);

    SlotInfo spool;
    spool.material = "ASA";
    spool.color_rgb = 0x1A2B3C;
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.api->mock_get_db_value("lane_data", "lane1").is_null());
    CHECK(fx.api->mock_get_db_value("lane_data", "lane17").is_null());
}

// ---------------------------------------------------------------------------
// AFC + IFS external-spool lane publish (same capability, shared helper)
// ---------------------------------------------------------------------------

namespace {
/// Raw-store test for the helper's key-style contract: the outer key follows
/// the store's style while the inner 0-based `lane` field — what Orca reads —
/// is always the slot index.
void seed_store_and_publish(helix::ams::LaneKeyStyle style, const char* expect_outer) {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api{client, state};

    CfsTmpCacheDir tmp{"style"};
    helix::ams::FilamentSlotOverrideStore store(&api, "t", style);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    SlotInfo spool;
    spool.material = "ASA";
    spool.color_rgb = 0x1A2B3C;
    const int lane_index = 4; // e.g. T0-T3 lanes -> extern is 4
    CHECK(helix::ams::publish_external_lane(&store, lane_index, &spool, "test"));
    helix::ui::UpdateQueue::instance().drain();

    auto rec = api.mock_get_db_value("lane_data", expect_outer);
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["lane"] == "4"); // inner field authoritative, 0-based string
    CHECK(rec["helix_material"] == "ASA");
}
} // namespace

TEST_CASE("external lane helper: outer key follows store style, inner lane is index",
          "[ams][bypass-arming]") {
    HelixTestFixture fx;
    // Tool style (AFC publish store, tool changers): "T4".
    seed_store_and_publish(helix::ams::LaneKeyStyle::Tool, "T4");
    // Lane style (HelixScreen filament systems): "lane5" (1-based outer).
    seed_store_and_publish(helix::ams::LaneKeyStyle::Lane, "lane5");
}

TEST_CASE("AFC external spool lane: publishes T{N} one past the last lane",
          "[ams][afc][bypass-arming]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api{client, state};

    AfcBypassPublishTestAccess backend(&api);
    backend.seed_lanes(4);
    REQUIRE(backend.get_system_info().total_slots == 4);
    REQUIRE(backend.get_system_info().supports_bypass);

    SlotInfo spool;
    spool.material = "ASA";
    spool.color_rgb = 0x1A2B3C;
    backend.publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();

    // AFC's own lane_data convention is T<n> since its virtual-tools
    // firmware — the extern entry rides the same style at T4.
    auto rec = api.mock_get_db_value("lane_data", "T4");
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["lane"] == "4");
    CHECK(rec["helix_material"] == "ASA");

    SECTION("null spool clears the lane") {
        backend.publish_external_spool_lane(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(api.mock_get_db_value("lane_data", "T4").is_null());
    }
}

TEST_CASE("IFS external spool lane: publishes one past NUM_PORTS", "[ams][ifs][bypass-arming]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api{client, state};

    auto backend = std::make_unique<AmsBackendAd5xIfs>(&api, &client);
    CfsTmpCacheDir tmp{"ifs"};
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(*backend, std::move(store));
    // IFS claims bypass statically; total_slots is NUM_PORTS (4).
    REQUIRE(backend->get_system_info().supports_bypass);
    REQUIRE(backend->get_system_info().total_slots == 4);

    SlotInfo spool;
    spool.material = "PETG";
    spool.color_rgb = 0x00FF00;
    backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();

    // IFS store uses the Lane style: slot 4 -> outer "lane5", inner "4".
    auto rec = api.mock_get_db_value("lane_data", "lane5");
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["lane"] == "4");
    CHECK(rec["helix_material"] == "PETG");
}
