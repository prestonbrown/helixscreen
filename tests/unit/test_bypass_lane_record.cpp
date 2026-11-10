// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bypass (external) spool is a lane record, lane_name "bypass": the
// primary backend keeps it among its own lane records, and with no backend
// AmsState keeps it in lane_data itself. Either way it is read back like any
// other record, at connect, after each write and on the poll.

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/afc_test_access.h"
#include "../test_helpers/cfs_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "filament_slot_override_store.h"
#include "lane_source_store.h"
#include "test_helpers/registered_backend.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

/// Drain until quiet: a write's success queues a read, and the read's answer
/// queues its filing.
void settle() {
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

json bypass_record(int lane, const std::string& material, const std::string& color) {
    return {{"lane", std::to_string(lane)},
            {"lane_name", "bypass"},
            {"writer", "helixscreen"},
            {"filament_type", material},
            {"color", color}};
}

/// AmsState reading its own bypass store from @p printer, for one case.
struct OwnBypassStore {
    explicit OwnBypassStore(MockPrinter& printer) {
        AmsState::instance().clear_backends();
        AmsState::instance().set_moonraker_api(&printer.api);
        AmsState::instance().refresh_bypass_record();
        settle();
    }
    ~OwnBypassStore() {
        settle();
        AmsState::instance().set_moonraker_api(nullptr);
    }
};

class RecordBackedChanger : public AmsBackendToolChanger {
  public:
    explicit RecordBackedChanger(IMoonrakerAPI* api) : AmsBackendToolChanger(api, nullptr) {
        set_discovered_tools({"T0", "T1"});
        running_ = true;
    }
    ~RecordBackedChanger() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    AmsError execute_gcode(const std::string&) override {
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string&, SharedCallback<void()>) override {
        return AmsErrorHelper::success();
    }
};

class StartedAfc : public AmsBackendAfc {
  public:
    explicit StartedAfc(IMoonrakerAPI* api) : AmsBackendAfc(api, nullptr) {
        AfcTestAccess::initialize_slots(*this, std::vector<std::string>{"lane1", "lane2"});
    }
    void start_now() {
        on_started();
        settle();
    }
    ~StartedAfc() override {
        settle();
    }
    AmsError execute_gcode(const std::string&) override {
        return AmsErrorHelper::success();
    }
};

SlotInfo petg_spool() {
    SlotInfo spool;
    spool.material = "PETG";
    spool.color_rgb = 0x1E5AA8;
    spool.brand = "Polymaker";
    spool.spoolman_id = 7;
    return spool;
}

} // namespace

// --- No filament backend: AmsState keeps the record in lane_data ----------

TEST_CASE_METHOD(LVGLTestFixture, "the bypass spool is the bypass record Moonraker holds",
                 "[bypass][lane][refresh]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "lane1", bypass_record(0, "PETG", "#1E5AA8"));
    OwnBypassStore store(printer);

    const auto spool = AmsState::instance().get_external_spool_info();
    REQUIRE(spool.has_value());
    CHECK(spool->material == "PETG");
    CHECK(spool->color_rgb == 0x1E5AA8u);
    CHECK(spool->slot_index == helix::ams::kBypassSlotIndex);
    CHECK(helix::ams::lane_sources(helix::ams::kBypassLaneId).record.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "another client's change to the bypass record shows on the next read",
                 "[bypass][lane][refresh]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "lane1", bypass_record(0, "PETG", "#1E5AA8"));
    OwnBypassStore store(printer);
    REQUIRE(AmsState::instance().get_external_spool_info()->material == "PETG");

    printer.client.mock_db_set("lane_data", "lane1", bypass_record(0, "ABS", "#FF0000"));
    AmsState::instance().refresh_bypass_record();
    settle();
    CHECK(AmsState::instance().get_external_spool_info()->material == "ABS");
    CHECK(AmsState::instance().get_external_spool_info()->color_rgb == 0xFF0000u);

    // Deleted elsewhere: no spool recorded.
    printer.client.mock_db_set("lane_data", "lane1", nullptr);
    AmsState::instance().refresh_bypass_record();
    settle();
    CHECK_FALSE(AmsState::instance().get_external_spool_info().has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a bypass read still out when the backends clear is not filed",
                 "[bypass][lane][refresh][multi-printer]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "lane1", bypass_record(0, "PETG", "#1E5AA8"));
    OwnBypassStore store(printer);
    REQUIRE(AmsState::instance().get_external_spool_info().has_value());

    // A read is answered but not yet filed when the printer goes away; the
    // next printer holds no bypass record.
    AmsState::instance().refresh_bypass_record();
    printer.client.mock_db_set("lane_data", "lane1", nullptr);
    AmsState::instance().clear_backends();
    CHECK_FALSE(AmsState::instance().get_external_spool_info().has_value());

    // The first pass files what is answered: the departed read must not be.
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(AmsState::instance().get_external_spool_info().has_value());
    CHECK_FALSE(helix::ams::lane_sources(helix::ams::kBypassLaneId).record.has_value());
    settle();
    CHECK_FALSE(AmsState::instance().get_external_spool_info().has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a bypass edit writes an RFC record and shows until it echoes",
                 "[bypass][lane][refresh][pending]") {
    MockPrinter printer;
    OwnBypassStore store(printer);

    AmsState::instance().set_external_spool_info(petg_spool());
    // Shown at once, as the pending edit.
    REQUIRE(AmsState::instance().get_external_spool_info().has_value());
    CHECK(AmsState::instance().get_external_spool_info()->material == "PETG");

    const json written = printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(written.is_object());
    CHECK(written["writer"] == "helixscreen");
    CHECK(written["lane_name"] == "bypass");
    CHECK(written["lane"] == "0");
    CHECK(written["lss_version"] == "0.1");
    CHECK(written["filament_type"] == "PETG");
    CHECK(written["color"] == "#1E5AA8");
    CHECK(written["vendor_name"] == "Polymaker");
    CHECK(written["spool_id"] == 7);
    CHECK_FALSE(written.contains("helix_external"));

    settle();
    CHECK_FALSE(helix::ams::lane_sources(helix::ams::kBypassLaneId).pending.has_value());
    CHECK(AmsState::instance().get_external_spool_info()->material == "PETG");
    CHECK(AmsState::instance().get_external_spool_info()->spoolman_id == 7);
}

TEST_CASE_METHOD(LVGLTestFixture, "a bypass record carries the spool's Orca filament id",
                 "[bypass][lane][refresh][orca]") {
    MockPrinter printer;
    OwnBypassStore store(printer);
    SlotInfo spool = petg_spool();
    spool.spoolman_filament_id = 55;
    spool.spoolman_orca_filament_id = "OGFL99";

    CHECK(helix::ams::external_lane_record(spool).spoolman_orca_filament_id == "OGFL99");

    AmsState::instance().set_external_spool_info(spool);
    const json written = printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(written.is_object());
    CHECK(written["filament_id"] == "OGFL99");
    CHECK(written["helix_filament_id_source"] == "spoolman");
    settle();
}

TEST_CASE_METHOD(LVGLTestFixture, "a bypass edit whose write fails is withdrawn",
                 "[bypass][lane][refresh][pending]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "lane1", bypass_record(0, "PLA", "#FFFFFF"));
    OwnBypassStore store(printer);

    printer.client.fail_next("server.database.post_item");
    AmsState::instance().set_external_spool_info(petg_spool());
    settle();
    CHECK_FALSE(helix::ams::lane_sources(helix::ams::kBypassLaneId).pending.has_value());
    CHECK(AmsState::instance().get_external_spool_info()->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "clearing the bypass spool deletes its record",
                 "[bypass][lane][refresh]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "lane1", bypass_record(0, "PLA", "#FFFFFF"));
    OwnBypassStore store(printer);
    REQUIRE(AmsState::instance().get_external_spool_info().has_value());

    AmsState::instance().clear_external_spool_info();
    CHECK_FALSE(AmsState::instance().get_external_spool_info().has_value());
    settle();
    CHECK(printer.client.mock_db_get("lane_data", "lane1").is_null());
    CHECK_FALSE(AmsState::instance().get_external_spool_info().has_value());
}

// --- A backend holds it: one past its lanes, read by its own poll ----------

TEST_CASE_METHOD(LVGLTestFixture,
                 "a tool changer holds the bypass record one past its tools, not on a tool",
                 "[bypass][lane][refresh][toolchanger]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T2", bypass_record(2, "PETG", "#1E5AA8"));
    test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    ToolChangerTestAccess::call_on_started(*reg);
    settle();

    const auto spool = AmsState::instance().get_external_spool_info();
    REQUIRE(spool.has_value());
    CHECK(spool->material == "PETG");
    CHECK_FALSE(helix::ams::lane_sources(reg.lane(2)).record.has_value());

    SECTION("another client's change shows on the next poll") {
        printer.client.mock_db_set("lane_data", "T2", bypass_record(2, "ABS", "#FF0000"));
        ToolChangerTestAccess::poll_lane_records(*reg);
        settle();
        CHECK(AmsState::instance().get_external_spool_info()->material == "ABS");
    }

    SECTION("an edit is written at the same key, in the backend's key style") {
        AmsState::instance().set_external_spool_info(petg_spool());
        const json written = printer.client.mock_db_get("lane_data", "T2");
        REQUIRE(written.is_object());
        CHECK(written["lane_name"] == "bypass");
        CHECK(written["lane"] == "2");
        CHECK(written["spool_id"] == 7);
        CHECK(printer.client.mock_db_get("lane_data", "lane1").is_null());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "on AFC the bypass record goes in the extras, never lane_data",
                 "[bypass][lane][refresh][extras][writer_rule][afc]") {
    MockPrinter printer;
    test::RegisteredBackend<StartedAfc> reg(&printer.api);
    reg->start_now();

    AmsState::instance().set_external_spool_info(petg_spool());
    settle();

    CHECK(printer.client.mock_db_get("lane_data", "lane3").is_null());
    const json extras = printer.client.mock_db_get("helix-screen-afc-overrides", "lane3");
    REQUIRE(extras.is_object());
    CHECK(extras["lane_name"] == "bypass");
    // No firmware holds the bypass spool, so its extras record keeps it whole.
    CHECK(extras["filament_type"] == "PETG");
    CHECK(extras["color"] == "#1E5AA8");
    CHECK(extras["spool_id"] == 7);
    const auto spool = AmsState::instance().get_external_spool_info();
    REQUIRE(spool.has_value());
    CHECK(spool->material == "PETG");
    CHECK(spool->spoolman_id == 7);
    CHECK_FALSE(helix::ams::lane_sources(helix::ams::kBypassLaneId).pending.has_value());
}

// A bypass record written before records named their writer sits at the
// bypass index unmarked; it is taken over, not treated as a slot's.
TEST_CASE_METHOD(LVGLTestFixture, "an unmarked writerless record past the lanes is taken over",
                 "[bypass][lane][refresh][toolchanger]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T2", {{"lane", "2"}, {"material", "PLA"}});
    test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    ToolChangerTestAccess::call_on_started(*reg);
    settle();

    AmsState::instance().set_external_spool_info(petg_spool());
    settle();
    const json written = printer.client.mock_db_get("lane_data", "T2");
    CHECK(written["lane_name"] == "bypass");
    CHECK(written["filament_type"] == "PETG");
}

// --- CFS: the index is the firmware's external slot or 16 -------------------

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

// Status parsing (including the supports_bypass flip) is gated on running_,
// which start() normally sets; this exposes it and the frame entry point
// without a subscription.
class RunningCfs : public helix::printer::AmsBackendCfs {
  public:
    RunningCfs(IMoonrakerAPI* api, helix::IMoonrakerClient* client) : AmsBackendCfs(api, client) {}
    void mark_running() {
        running_ = true;
    }
    using AmsBackendCfs::handle_status_update;
};

class CfsPublishFixture : public HelixTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<RunningCfs> backend;
    CfsTmpCacheDir tmp{"pub"};

    CfsPublishFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        backend = std::make_unique<RunningCfs>(api.get(), &client);
        backend->mark_running();
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(api.get(), "cfs");
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
        backend->handle_status_update(json{{"params", json::array({json{{"box", box}}, 0})}});
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

    /// A record at @p index in the database, read back the way the backend's
    /// lane-record refresh reads it.
    void seed_record(int index, const helix::ams::FilamentSlotOverride& ovr) {
        client.mock_db_set("lane_data", "lane" + std::to_string(index + 1),
                           helix::ams::to_lane_data_record(index, ovr));
        backend->refresh_lane_records();
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    static SlotInfo asa() {
        SlotInfo spool;
        spool.material = "ASA";
        spool.color_rgb = 0x1A2B3C;
        return spool;
    }
};
} // namespace

TEST_CASE("CFS bypass record: stock writes past the highest bay any chain has",
          "[bypass][ams][cfs]") {
    CfsPublishFixture fx;
    fx.seed_stock_box();
    const auto record = helix::ams::external_lane_record(CfsPublishFixture::asa());
    fx.backend->publish_external_spool_lane(&record);
    helix::ui::UpdateQueue::instance().drain();
    const json rec = fx.client.mock_db_get("lane_data", "lane17");
    REQUIRE(rec.is_object());
    CHECK(rec["lane"] == "16");
    CHECK(rec["lane_name"] == "bypass");

    fx.backend->publish_external_spool_lane(nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane17").is_null());
}

TEST_CASE("CFS bypass record: fork writes at the firmware's external slot",
          "[bypass][ams][cfs][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(4);
    const auto record = helix::ams::external_lane_record(CfsPublishFixture::asa());
    fx.backend->publish_external_spool_lane(&record);
    helix::ui::UpdateQueue::instance().drain();
    const json rec = fx.client.mock_db_get("lane_data", "lane5");
    REQUIRE(rec.is_object());
    CHECK(rec["lane"] == "4");
    CHECK(fx.client.mock_db_get("lane_data", "lane17").is_null());
}

// With box 3 off the bus the fork's external slot is 8, box 3 bay A's key. That
// bay's record is not the bypass record: neither overwritten nor deleted.
TEST_CASE("CFS bypass record: a bay record on the fork's external key is left alone",
          "[bypass][ams][cfs][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(12);
    helix::ams::FilamentSlotOverride box3a;
    box3a.material = "PETG";
    box3a.spool_name = "Box 3 A spool";
    fx.seed_record(8, box3a);
    const json before = fx.client.mock_db_get("lane_data", "lane9");
    REQUIRE_FALSE(before.is_null());

    fx.send_fork_frame(8);
    const auto record = helix::ams::external_lane_record(CfsPublishFixture::asa());
    fx.backend->publish_external_spool_lane(&record);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane9") == before);

    fx.backend->publish_external_spool_lane(nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane9") == before);
}

// The fork's external slot follows the chain; the record left at the old key
// would read as a second bypass spool.
TEST_CASE("CFS bypass record: a moved external slot deletes the record at the old key",
          "[bypass][ams][cfs][1464]") {
    CfsPublishFixture fx;
    fx.send_fork_frame(4);
    const auto record = helix::ams::external_lane_record(CfsPublishFixture::asa());
    fx.seed_record(4, record);

    fx.send_fork_frame(8);
    fx.backend->publish_external_spool_lane(&record);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.client.mock_db_get("lane_data", "lane5").is_null());
    CHECK(fx.client.mock_db_get("lane_data", "lane9")["lane_name"] == "bypass");
}
