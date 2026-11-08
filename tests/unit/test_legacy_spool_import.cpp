// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen 1.0 kept per-tool spool links in tool_spools.json and the
// Moonraker key helix-screen/tool_spool_assignments, and the bypass spool in
// settings.json. The first connect after the upgrade writes them where a
// lane's identity now lives, and deletes the 1.0 copies.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/unique_temp_dir.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "config.h"
#include "helix_fs.h"
#include "lane_source_store.h"
#include "legacy_spool_import.h"
#include "test_helpers/registered_backend.h"
#include "text_io.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;
namespace legacy = helix::ams::legacy;

namespace {

/// A per-printer tool_spools.json as HelixScreen 1.0 wrote it.
json one_oh_tool_spools() {
    return json::parse(R"({
        "adopted_by": "default",
        "printers": {
            "default": {
                "0": {"spoolman_id": 12, "spool_name": "PolyTerra Charcoal",
                      "remaining_weight_g": 612.5, "total_weight_g": 1000.0},
                "1": {"spoolman_id": 34, "spool_name": "Sunlu PETG Blue"},
                "2": {"spoolman_id": 0, "spool_name": ""}
            },
            "voron": {
                "0": {"spoolman_id": 56, "spool_name": "Esun ABS+"}
            }
        }
    })");
}

void settle() {
    for (int i = 0; i < 4; ++i) {
        ui::UpdateQueue::instance().drain();
    }
}

/// Points AmsState at @p printer for one case, and lets go of it however the
/// case ends: a failed REQUIRE must not leave AmsState holding a dead API.
struct AmsOnPrinter {
    explicit AmsOnPrinter(MockPrinter& printer) {
        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();
        AmsState::instance().set_moonraker_api(&printer.api);
    }
    ~AmsOnPrinter() {
        settle();
        AmsState::instance().set_moonraker_api(nullptr);
        AmsState::instance().clear_backends();
    }
};

class RecordBackedChanger : public AmsBackendToolChanger {
  public:
    explicit RecordBackedChanger(IMoonrakerAPI* api) : AmsBackendToolChanger(api, nullptr) {
        set_discovered_tools({"T0", "T1"});
        running_ = true;
    }
    ~RecordBackedChanger() override {
        ui::UpdateQueue::instance().drain();
    }
    AmsError execute_gcode(const std::string&) override {
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string&, SharedCallback<void()>) override {
        return AmsErrorHelper::success();
    }
};

} // namespace

TEST_CASE("a 1.0 tool_spools.json yields the printer's own tool links",
          "[legacy_import][tool-state]") {
    const json file = one_oh_tool_spools();
    CHECK(legacy::tool_spools_from_file(file, "default") == std::map<int, int>{{0, 12}, {1, 34}});
    CHECK(legacy::tool_spools_from_file(file, "voron") == std::map<int, int>{{0, 56}});
    CHECK(legacy::tool_spools_from_file(file, "other").empty());

    // The single-printer layout: the whole file is the set.
    const json single = file["printers"]["default"];
    CHECK(legacy::tool_spools_from_file(single, "anyone") == std::map<int, int>{{0, 12}, {1, 34}});

    // Imported links leave; owed ones stay. A set naming no spool leaves the
    // file, and the last one takes the file with it.
    const auto rest = legacy::file_keeping_tools(file, "default", {});
    REQUIRE(rest.has_value());
    CHECK_FALSE((*rest)["printers"].contains("default"));
    CHECK((*rest)["printers"].contains("voron"));
    CHECK_FALSE(legacy::file_keeping_tools(*rest, "voron", {}).has_value());
    CHECK_FALSE(legacy::file_keeping_tools(single, "default", {}).has_value());

    const auto owed_one = legacy::file_keeping_tools(file, "default", {{1, 34}});
    REQUIRE(owed_one.has_value());
    CHECK(legacy::tool_spools_from_file(*owed_one, "default") == std::map<int, int>{{1, 34}});
    CHECK(legacy::tool_spools_from_file(*owed_one, "voron") == std::map<int, int>{{0, 56}});
    const auto single_owed = legacy::file_keeping_tools(single, "default", {{0, 12}});
    REQUIRE(single_owed.has_value());
    CHECK(legacy::tool_spools_from_file(*single_owed, "x") == std::map<int, int>{{0, 12}});
    CHECK(legacy::set_keeping_tools(single, {{1, 34}}) == json{{"1", single["1"]}});
}

TEST_CASE("a 1.0 settings external_spool block yields the bypass spool",
          "[legacy_import][bypass]") {
    const auto spool = legacy::external_spool_from_settings(json{{"assigned", true},
                                                                 {"color_rgb", 0x1E5AA8},
                                                                 {"material", "PETG"},
                                                                 {"brand", "Polymaker"},
                                                                 {"spoolman_id", 7},
                                                                 {"spool_name", "PolyLite"},
                                                                 {"remaining_weight_g", 640.0},
                                                                 {"nozzle_temp_min", 230}});
    REQUIRE(spool.has_value());
    CHECK(spool->material == "PETG");
    CHECK(spool->color_rgb == 0x1E5AA8u);
    CHECK(spool->brand == "Polymaker");
    CHECK(spool->spoolman_id == 7);
    CHECK(spool->remaining_weight_g == Catch::Approx(640.0f));
    CHECK(spool->nozzle_temp_min == 230);
    // The pre-"assigned" layout named a spool by its colour.
    CHECK(legacy::external_spool_from_settings(json{{"color_rgb", 0xFF0000}}).has_value());
    CHECK_FALSE(legacy::external_spool_from_settings(json{{"material", "PLA"}}).has_value());
    CHECK_FALSE(legacy::external_spool_from_settings(json::object()).has_value());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a tool changer's 1.0 tool links go to the lanes that feed them, once",
                 "[legacy_import][toolchanger][lane_truth]") {
    MockPrinter printer;
    printer.client.mock_db_set(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey,
                               one_oh_tool_spools()["printers"]["default"]);
    // T1's lane already links a spool: what Moonraker holds stands.
    printer.client.mock_db_set("lane_data", "T1", json{{"lane", "1"}, {"spool_id", 99}});
    const std::string dir = test::unique_temp_dir("helix_legacy_import");
    fs::create_directories(dir);
    const std::string file_path = fs::join_path(dir, legacy::kToolSpoolsFile);
    REQUIRE(text_io::write_file_atomic(file_path, one_oh_tool_spools().dump()));

    AmsOnPrinter scope(printer);
    auto& ams = AmsState::instance();
    test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    ToolChangerTestAccess::call_on_started(*reg);
    settle();

    ams.start_legacy_spool_import(file_path);
    settle();
    ams.sync_from_backend();
    settle();

    const json t0 = printer.client.mock_db_get("lane_data", "T0");
    REQUIRE(t0.is_object());
    CHECK(t0["spool_id"] == 12);
    CHECK(printer.client.mock_db_get("lane_data", "T1")["spool_id"] == 99);
    CHECK(reg->get_slot_info(0).spoolman_id == 12);
    CHECK(reg->get_slot_info(1).spoolman_id == 99);

    // The 1.0 copies are gone, another printer's set excepted.
    CHECK(printer.client.mock_db_get(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey)
              .is_null());
    const auto text = text_io::read_file(file_path);
    REQUIRE(text.has_value());
    const json left = json::parse(*text, nullptr, false);
    CHECK_FALSE(left["printers"].contains("default"));
    CHECK(left["printers"].contains("voron"));

    // A second connect has nothing left to import.
    printer.client.mock_db_set("lane_data", "T0", json{{"lane", "0"}});
    ams.start_legacy_spool_import(file_path);
    settle();
    ams.sync_from_backend();
    settle();
    CHECK(printer.client.mock_db_get("lane_data", "T0")["spool_id"].is_null());

    fs::remove(file_path);
    fs::remove(dir);
}

TEST_CASE_METHOD(LVGLTestFixture, "a 1.0 tool link whose write is refused stays in the 1.0 copies",
                 "[legacy_import][toolchanger][lane_truth]") {
    MockPrinter printer;
    printer.client.mock_db_set(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey,
                               one_oh_tool_spools()["printers"]["default"]);
    // Another writer owns T0's record, so HelixScreen may not write into it.
    printer.client.mock_db_set("lane_data", "T0", json{{"lane", "0"}, {"writer", "afclite"}});
    const std::string dir = test::unique_temp_dir("helix_legacy_import_refused");
    fs::create_directories(dir);
    const std::string file_path = fs::join_path(dir, legacy::kToolSpoolsFile);
    REQUIRE(text_io::write_file_atomic(file_path, one_oh_tool_spools().dump()));

    AmsOnPrinter scope(printer);
    auto& ams = AmsState::instance();
    test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    ToolChangerTestAccess::call_on_started(*reg);
    settle();

    ams.start_legacy_spool_import(file_path);
    settle();
    ams.sync_from_backend();
    settle();

    CHECK(printer.client.mock_db_get("lane_data", "T0")["spool_id"].is_null());
    CHECK(printer.client.mock_db_get("lane_data", "T1")["spool_id"] == 34);

    // T1's link is written and leaves; T0's is kept for the next connect.
    const json key =
        printer.client.mock_db_get(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey);
    CHECK(legacy::tool_spools_from_set(key) == std::map<int, int>{{0, 12}});
    const auto text = text_io::read_file(file_path);
    REQUIRE(text.has_value());
    const json left = json::parse(*text, nullptr, false);
    CHECK(legacy::tool_spools_from_file(left, "default") == std::map<int, int>{{0, 12}});
    CHECK(left["printers"].contains("voron"));

    fs::remove(file_path);
    fs::remove(dir);
}

TEST_CASE_METHOD(LVGLTestFixture, "a 1.0 bypass spool whose write fails stays parked",
                 "[legacy_import][bypass][lane_truth]") {
    MockPrinter printer;
    Config* config = Config::get_instance();
    REQUIRE(config != nullptr);
    const std::string parked = config->df() + legacy::kLegacyExternalSpoolPath;
    config->get_json(parked) =
        json{{"assigned", true}, {"material", "PETG"}, {"color_rgb", 0x1E5AA8}, {"spoolman_id", 7}};

    AmsOnPrinter scope(printer);
    auto& ams = AmsState::instance();
    ams.refresh_bypass_record();
    settle();
    printer.client.fail_next("server.database.post_item");
    ams.start_legacy_spool_import("/nonexistent/tool_spools.json");
    settle();

    CHECK_FALSE(printer.client.mock_db_get("lane_data", "lane1").is_object());
    REQUIRE(config->try_get_json(parked) != nullptr);

    // The next connect writes it, and only then lets the parked block go.
    ams.start_legacy_spool_import("/nonexistent/tool_spools.json");
    settle();
    CHECK(printer.client.mock_db_get("lane_data", "lane1")["spool_id"] == 7);
    CHECK(config->try_get_json(parked) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a 1.0 bypass spool goes into an empty bypass record, and only there",
                 "[legacy_import][bypass][lane_truth]") {
    const bool record_exists = GENERATE(false, true);
    MockPrinter printer;
    if (record_exists) {
        printer.client.mock_db_set("lane_data", "lane1",
                                   json{{"lane", "0"},
                                        {"lane_name", "bypass"},
                                        {"writer", "helixscreen"},
                                        {"filament_type", "ABS"}});
    }
    Config* config = Config::get_instance();
    REQUIRE(config != nullptr);
    config->get_json(config->df() + legacy::kLegacyExternalSpoolPath) =
        json{{"assigned", true}, {"material", "PETG"}, {"color_rgb", 0x1E5AA8}, {"spoolman_id", 7}};

    AmsOnPrinter scope(printer);
    auto& ams = AmsState::instance();
    ams.refresh_bypass_record();
    settle();
    ams.start_legacy_spool_import("/nonexistent/tool_spools.json");
    settle();

    const json rec = printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(rec.is_object());
    if (record_exists) {
        CHECK(rec["filament_type"] == "ABS");
    } else {
        CHECK(rec["lane_name"] == "bypass");
        CHECK(rec["spool_id"] == 7);
        CHECK(rec["filament_type"] == "PETG");
    }
    CHECK(config->try_get_json(config->df() + legacy::kLegacyExternalSpoolPath) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "1.0 tool links on a lane-owning system are dropped, never copied back",
                 "[legacy_import][lane_truth]") {
    // There the 1.0 ToolState only mirrored the lanes, so its copy may name a
    // spool the firmware has since cleared.
    MockPrinter printer;
    printer.client.mock_db_set(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey,
                               json{{"0", {{"spoolman_id", 12}}}});
    AmsOnPrinter scope(printer);
    auto& ams = AmsState::instance();
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_operation_delay(0);
    REQUIRE_FALSE(mock->supports_per_tool_spool_assignment());
    SlotInfo empty = mock->get_slot_info(0);
    empty.spoolman_id = 0;
    REQUIRE(helix::test::apply_edit(*mock, 0, empty).success());
    ams.set_backend(std::move(mock));

    ams.start_legacy_spool_import("/nonexistent/tool_spools.json");
    settle();
    ams.sync_from_backend();
    settle();

    CHECK(ams.get_backend()->get_slot_info(0).spoolman_id == 0);
    CHECK(printer.client.mock_db_get(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey)
              .is_null());
}
