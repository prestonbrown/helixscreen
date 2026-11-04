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
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
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
#include "test_helpers/afc_test_access.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/cfs_test_access.h"

#include <filesystem>
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

// ---------------------------------------------------------------------------
// CFS external-spool lane_data publish (slicer sync)
// ---------------------------------------------------------------------------

namespace helix {

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
            slot.color_rgb = kAmsDefaultSlotColor;
            unit.slots.push_back(slot);
        }
        system_info_.units.push_back(unit);
        system_info_.total_slots = count;
        AfcTestAccess::slots(*this).initialize("Box Turtle 1", names);
        for (int i = 0; i < count; ++i) {
            auto* entry = AfcTestAccess::slots(*this).get_mut(i);
            if (entry) {
                entry->info.mapped_tool = i;
            }
        }
    }
};
} // namespace helix

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
    /// supports_bypass.
    void seed_stock_box() {
        const json box = json::parse(R"({
            "state": "connect", "filament": 0, "enable": 1, "filament_useup": 0,
            "map": {"T1A": "T1A"},
            "T1": {"state": "connect", "filament": "None",
                   "vender": ["none"], "remain_len": ["-1"],
                   "color_value": ["-1"], "material_type": ["-1"]}})");
        send(box);
    }

    void send(const json& box) {
        CfsTestAccess::handle_status(*backend,
                                     json{{"params", json::array({json{{"box", box}}, 0})}});
    }

    /// A fork box frame listing @p bays bays (0..bays-1) and the external
    /// holder at index @p bays, the way box.py numbers it.
    void send_fork_frame(int bays) {
        json slots = json::array();
        for (int i = 0; i < bays; ++i) {
            slots.push_back({{"index", i}, {"external", false}, {"present", false}});
        }
        slots.push_back({{"index", bays}, {"external", true}, {"present", true}});
        send(json{{"api_version", 1}, {"loaded_slot", -1}, {"slots", slots}});
    }

    /// A record at @p index that both the backend and the database hold, as a
    /// load would have left it.
    void seed_record(int index, const helix::ams::FilamentSlotOverride& ovr) {
        CfsTestAccess::seed_override(*backend, index, ovr);
        client.mock_db_set("lane_data", "lane" + std::to_string(index + 1),
                           helix::ams::to_lane_data_record(index, ovr));
    }

    static SlotInfo asa() {
        SlotInfo spool;
        spool.material = "ASA";
        spool.color_rgb = 0x1A2B3C;
        return spool;
    }
};
} // namespace

TEST_CASE("CFS external spool lane: stock publishes past the highest bay any chain has",
          "[ams][cfs][bypass-arming]") {
    CfsPublishFixture fx;
    fx.seed_stock_box();
    REQUIRE(fx.backend->get_system_info().supports_bypass);
    // Index 16, whatever the attached span: 4 boxes x 4 bays is 0..15.
    const std::string lane_key = "lane17";

    SlotInfo spool;
    spool.material = "ASA";
    spool.color_rgb = 0x1A2B3C;
    spool.brand = "Polymaker";
    spool.spoolman_id = 7;
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();

    auto rec = fx.client.mock_db_get("lane_data", lane_key);
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["helix_external"] == true);
    CHECK(rec["helix_material"] == "ASA");
    CHECK(rec["color"] == "#1A2B3C");
    CHECK(rec["vendor"] == "Polymaker");

    SECTION("clear removes the lane") {
        fx.backend->publish_external_spool_lane(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.client.mock_db_get("lane_data", lane_key).is_null());
    }

    SECTION("black is a real pick — publishes, unlike the gray default") {
        SlotInfo black = spool;
        black.color_rgb = 0x000000;
        black.material.clear();
        fx.backend->publish_external_spool_lane(&black);
        helix::ui::UpdateQueue::instance().drain();
        auto rec2 = fx.client.mock_db_get("lane_data", lane_key);
        REQUIRE_FALSE(rec2.is_null());
        CHECK(rec2["color"] == "#000000");
    }

    SECTION("identity-less record clears") {
        SlotInfo blank; // default gray, no material, no spoolman
        fx.backend->publish_external_spool_lane(&blank);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.client.mock_db_get("lane_data", lane_key).is_null());
    }
}

// OrcaSlicer sends the lane as the tool, so on Fork the external lane is the
// firmware's own external slot: T4 on a one-box chain (#1464).
TEST_CASE("CFS external spool lane: fork publishes at the firmware's external slot",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(4);
    REQUIRE(fx.backend->get_system_info().supports_bypass);

    const SlotInfo spool = CfsPublishFixture::asa();
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    const auto rec = fx.client.mock_db_get("lane_data", "lane5");
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["lane"] == "4");
    CHECK(rec["helix_external"] == true);
    CHECK(fx.client.mock_db_get("lane_data", "lane17").is_null());
}

// With box 3 off the bus the fork's external slot is 8, box 3 bay A's key. That
// bay's record is not ours: publishing neither overwrites nor clears it (#1464).
TEST_CASE("CFS external spool lane: a bay record on the fork's external key is left alone",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(12);
    helix::ams::FilamentSlotOverride box3a;
    box3a.material = "PETG";
    box3a.spool_name = "Box 3 A spool";
    fx.seed_record(8, box3a);
    const json before = fx.client.mock_db_get("lane_data", "lane9");
    REQUIRE_FALSE(before.is_null());

    fx.send_fork_frame(8);
    const SlotInfo spool = CfsPublishFixture::asa();
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane9") == before);

    fx.backend->publish_external_spool_lane(nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane9") == before);
}

// A mirror written before the mark existed carries no mark. When it names the
// external spool saved in settings it is ours: adopted, marked and kept current,
// so an existing user's OrcaSlicer tray does not freeze (#1464).
TEST_CASE("CFS external spool lane: an old unmarked mirror naming the saved spool is adopted",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(4);
    const SlotInfo spool = CfsPublishFixture::asa();

    SECTION("the same filament is adopted, then kept current") {
        helix::ams::FilamentSlotOverride old_mirror;
        old_mirror.material = spool.material;
        old_mirror.color_rgb = spool.color_rgb;
        old_mirror.color_set = true;
        fx.seed_record(4, old_mirror);

        fx.backend->publish_external_spool_lane(&spool);
        helix::ui::UpdateQueue::instance().drain();
        auto rec = fx.client.mock_db_get("lane_data", "lane5");
        REQUIRE_FALSE(rec.is_null());
        CHECK(rec["helix_external"] == true);

        SlotInfo next;
        next.material = "PETG";
        next.color_rgb = 0x0A2989;
        fx.backend->publish_external_spool_lane(&next);
        helix::ui::UpdateQueue::instance().drain();
        rec = fx.client.mock_db_get("lane_data", "lane5");
        REQUIRE_FALSE(rec.is_null());
        CHECK(rec["helix_material"] == "PETG");
    }

    SECTION("a different filament is not ours: untouched by publish and clear") {
        helix::ams::FilamentSlotOverride other;
        other.material = "PLA";
        other.color_rgb = 0xFFFFFF;
        other.color_set = true;
        fx.seed_record(4, other);
        const json before = fx.client.mock_db_get("lane_data", "lane5");

        fx.backend->publish_external_spool_lane(&spool);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.client.mock_db_get("lane_data", "lane5") == before);
        fx.backend->publish_external_spool_lane(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.client.mock_db_get("lane_data", "lane5") == before);
    }
}

TEST_CASE("record_describes_spool: Spoolman id decides when either side has one",
          "[ams][bypass-arming][1464]") {
    helix::ams::FilamentSlotOverride rec;
    rec.material = "ASA";
    rec.color_rgb = 0x1A2B3C;
    rec.color_set = true;
    SlotInfo spool = CfsPublishFixture::asa();
    CHECK(helix::ams::record_describes_spool(rec, spool));

    spool.spoolman_id = 7;
    CHECK_FALSE(helix::ams::record_describes_spool(rec, spool));
    rec.spoolman_id = 7;
    CHECK(helix::ams::record_describes_spool(rec, spool));

    rec.spoolman_id = 0;
    spool.spoolman_id = 0;
    rec.brand = "Polymaker";
    spool.brand = "eSUN";
    CHECK_FALSE(helix::ams::record_describes_spool(rec, spool));
    spool.brand.clear();
    CHECK(helix::ams::record_describes_spool(rec, spool));
    rec.color_rgb = 0x000000;
    CHECK_FALSE(helix::ams::record_describes_spool(rec, spool));
}

TEST_CASE("CFS external spool lane: our marked mirror is updated, an unmarked record is not",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    fx.seed_stock_box();
    const SlotInfo spool = CfsPublishFixture::asa();

    SECTION("a marked mirror is ours to update") {
        helix::ams::FilamentSlotOverride old_mirror;
        old_mirror.material = "PLA";
        old_mirror.external_mirror = true;
        fx.seed_record(16, old_mirror);
        fx.backend->publish_external_spool_lane(&spool);
        helix::ui::UpdateQueue::instance().drain();
        const auto rec = fx.client.mock_db_get("lane_data", "lane17");
        REQUIRE_FALSE(rec.is_null());
        CHECK(rec["helix_material"] == "ASA");
        CHECK(rec["helix_external"] == true);
    }

    SECTION("an unmarked record at the key is untouched") {
        helix::ams::FilamentSlotOverride other;
        other.material = "PLA";
        fx.seed_record(16, other);
        const json before = fx.client.mock_db_get("lane_data", "lane17");
        fx.backend->publish_external_spool_lane(&spool);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.client.mock_db_get("lane_data", "lane17") == before);
        fx.backend->publish_external_spool_lane(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.client.mock_db_get("lane_data", "lane17") == before);
    }
}

// A marked mirror is no bay's record where no bay is, but where its key is a
// bay the box reports, it is that bay's record: an adopted record may have been
// a real bay's before it was marked (#1464).
TEST_CASE("External spool mirror: kept as a bay's record only where the key is a present bay",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    helix::ams::FilamentSlotOverride mirror;
    mirror.material = "ASA";
    mirror.spool_name = "Box 2 A spool";
    mirror.external_mirror = true;
    CfsTestAccess::seed_override(*fx.backend, 4, mirror);

    SECTION("box 2 present: bay 4 keeps it") {
        fx.send_fork_frame(8);
        CHECK(CfsTestAccess::get_override(*fx.backend, 4).has_value());
    }
    SECTION("one box: index 4 is the external slot, not a bay") {
        fx.send_fork_frame(4);
        CHECK_FALSE(CfsTestAccess::get_override(*fx.backend, 4).has_value());
    }
}

// A mirror kept as a bay's record becomes that bay's record outright: unmarked
// and persisted so, or the external publish would go on treating the bay's
// record as its own (#1464).
TEST_CASE("External spool mirror: one kept as a bay's record stops being ours",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    helix::ams::FilamentSlotOverride kept;
    kept.material = "PETG";
    kept.spool_name = "Box 3 A spool";
    kept.external_mirror = true;
    fx.seed_record(8, kept);

    fx.send_fork_frame(12);
    REQUIRE(CfsTestAccess::get_override(*fx.backend, 8).has_value());
    CHECK_FALSE(CfsTestAccess::get_override(*fx.backend, 8)->external_mirror);
    helix::ui::UpdateQueue::instance().drain();
    const json persisted = fx.client.mock_db_get("lane_data", "lane9");
    REQUIRE_FALSE(persisted.is_null());
    CHECK_FALSE(persisted.contains("helix_external"));

    // Box 3 off the bus: the external slot is 8, box 3 bay A's key.
    fx.send_fork_frame(8);
    const SlotInfo spool = CfsPublishFixture::asa();
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane9") == persisted);

    // Box 3 back: the external slot moves to 12 and 8 is box 3 bay A again.
    fx.send_fork_frame(12);
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane9") == persisted);
}

// Our mirror's key turning into a reported bay in any frame makes the record
// that bay's: unmarked there and then, so a later save of the bay is not
// written marked and the next publish does not clear it (#1464).
TEST_CASE("CFS external spool lane: our mirror on a returning box's bay becomes the bay's",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(8);
    // A named spool: the record carries identity, so the returning bay reading
    // empty keeps it as the bay's record rather than clearing it.
    SlotInfo spool = CfsPublishFixture::asa();
    spool.spool_name = "External ASA";
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(fx.client.mock_db_get("lane_data", "lane9")["helix_external"] == true);

    // Box 3 returns within the session: 8 is its bay A.
    fx.send_fork_frame(12);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(CfsTestAccess::get_override(*fx.backend, 8).has_value());
    CHECK_FALSE(CfsTestAccess::get_override(*fx.backend, 8)->external_mirror);
    CHECK_FALSE(fx.client.mock_db_get("lane_data", "lane9").contains("helix_external"));

    SlotInfo edit = fx.backend->get_slot_info(8);
    edit.spool_name = "Box 3 A spool";
    REQUIRE(helix::test::apply_edit(*fx.backend, 8, edit).success());
    helix::ui::UpdateQueue::instance().drain();

    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    const json rec = fx.client.mock_db_get("lane_data", "lane9");
    REQUIRE_FALSE(rec.is_null());
    CHECK_FALSE(rec.contains("helix_external"));
    CHECK_FALSE(fx.client.mock_db_get("lane_data", "lane13").is_null());
}

// The old key is cleared only where we hold our own marked mirror; a key we
// hold nothing for is no record of ours (#1464).
TEST_CASE("CFS external spool lane: an old key we hold no record for is not cleared",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(8);
    const SlotInfo spool = CfsPublishFixture::asa();
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();

    // Someone else's record lands on the old key without passing through us.
    CfsTestAccess::erase_override(*fx.backend, 8);
    const json foreign = json{{"lane", "8"}, {"material", "PLA"}};
    fx.client.mock_db_set("lane_data", "lane9", foreign);

    fx.send_fork_frame(12);
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane9") == foreign);
}

// When the fork's external slot moves (the top box returns), the mirror at the
// old key would stay behind as a phantom tray. It is cleared there, but only if
// the record is still ours (#1464).
TEST_CASE("CFS external spool lane: a moved external slot clears our old mirror only",
          "[ams][cfs][bypass-arming][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(8);
    const SlotInfo spool = CfsPublishFixture::asa();
    fx.backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(fx.client.mock_db_get("lane_data", "lane9").is_null());

    SECTION("our mirror at the old key is cleared") {
        fx.send_fork_frame(12);
        fx.backend->publish_external_spool_lane(&spool);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.client.mock_db_get("lane_data", "lane9").is_null());
        CHECK_FALSE(fx.client.mock_db_get("lane_data", "lane13").is_null());
    }

    SECTION("a record that is not ours at the old key is untouched") {
        helix::ams::FilamentSlotOverride box3a;
        box3a.material = "PETG";
        box3a.spool_name = "Box 3 A spool";
        fx.seed_record(8, box3a);
        const json before = fx.client.mock_db_get("lane_data", "lane9");
        fx.send_fork_frame(12);
        fx.backend->publish_external_spool_lane(&spool);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(fx.client.mock_db_get("lane_data", "lane9") == before);
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
    CHECK(fx.client.mock_db_get("lane_data", "lane1").is_null());
    CHECK(fx.client.mock_db_get("lane_data", "lane17").is_null());
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
    spool.spoolman_id = 31;
    spool.spoolman_filament_id = 55;
    const int lane_index = 4; // e.g. T0-T3 lanes -> extern is 4
    CHECK(helix::ams::publish_external_lane(&store, lane_index, &spool, "test"));
    helix::ui::UpdateQueue::instance().drain();

    auto rec = client.mock_db_get("lane_data", expect_outer);
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["lane"] == "4"); // inner field authoritative, 0-based string
    CHECK(rec["helix_material"] == "ASA");
    CHECK(rec.value("helix_spoolman_filament_id", 0) == 55);
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

TEST_CASE("external lane publish carries the spool's Orca filament id",
          "[ams][bypass-arming][orca]") {
    HelixTestFixture fx;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api{client, state};

    CfsTmpCacheDir tmp{"orca"};
    helix::ams::FilamentSlotOverrideStore store(&api, "t", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    SlotInfo spool;
    spool.material = "ABS";
    spool.spoolman_id = 31;
    spool.spoolman_filament_id = 55;
    spool.spoolman_orca_filament_id = "OGFL99";

    CHECK(helix::ams::external_lane_record(spool).spoolman_orca_filament_id == "OGFL99");

    REQUIRE(helix::ams::publish_external_lane(&store, 4, &spool, "test"));
    helix::ui::UpdateQueue::instance().drain();
    const auto rec = client.mock_db_get("lane_data", "T4");
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec.value("filament_id", "") == "OGFL99");
    CHECK(rec.value("helix_filament_id_source", "") == "spoolman");
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
    auto rec = client.mock_db_get("lane_data", "T4");
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["lane"] == "4");
    CHECK(rec["helix_material"] == "ASA");

    SECTION("null spool clears the lane") {
        backend.publish_external_spool_lane(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(client.mock_db_get("lane_data", "T4").is_null());
    }
}

TEST_CASE("IFS external spool lane: no bypass fitted, so nothing publishes",
          "[ams][ifs][bypass-arming]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api{client, state};

    auto backend = std::make_unique<AmsBackendAd5xIfs>(&api, &client);
    CfsTmpCacheDir tmp{"ifs"};
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(*backend, std::move(store));
    // The AD5X has no bypass: every path into the hub runs through an IFS
    // lane, and there is no external direct-feed spool entry.
    REQUIRE_FALSE(backend->get_system_info().supports_bypass);
    REQUIRE(backend->get_system_info().total_slots == 4);

    SlotInfo spool;
    spool.material = "PETG";
    spool.color_rgb = 0x00FF00;
    backend->publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();

    // The publish path is gated on the capability, so no lane record appears.
    CHECK(client.mock_db_get("lane_data", "lane5").is_null());
    CHECK(client.mock_db_get("lane_data", "T4").is_null());
}
