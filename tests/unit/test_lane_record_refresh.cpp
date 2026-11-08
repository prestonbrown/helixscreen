// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AmsSubscriptionBackend::refresh_lane_records(): a lane shows the record
// Moonraker holds, read at connect, after each write of ours, and on the poll.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/afc_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "ams_backend_afc.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "app_globals.h"
#include "connection_state.h"
#include "filament_slot_override_store.h"
#include "lane_apply.h"
#include "lane_source_store.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// A tool changer on a mock Moonraker. Its lane records are the only identity
/// it has, which keeps the firmware out of every case below.
class RecordBackedChanger : public helix::AmsBackendToolChanger {
  public:
    explicit RecordBackedChanger(helix::IMoonrakerAPI* api)
        : helix::AmsBackendToolChanger(api, nullptr) {
        set_discovered_tools({"T0", "T1"});
        running_ = true;
    }
    ~RecordBackedChanger() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    helix::AmsError execute_gcode(const std::string&) override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string&, helix::SharedCallback<void()>) override {
        return helix::AmsErrorHelper::success();
    }
};

nlohmann::json record(const std::string& material, const std::string& color) {
    return {{"lane", "1"}, {"material", material}, {"color", color}};
}

/// Drain until quiet: a write's success queues a read, and the read's answer
/// queues its filing.
void settle() {
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a lane shows the record Moonraker holds once it is read",
                 "[lane][refresh][toolchanger]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T1", record("PETG", "#1E5AA8"));
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    const auto slot = reg->get_slot_info(1);
    CHECK(slot.material == "PETG");
    CHECK(slot.color_rgb == 0x1E5AA8u);
    REQUIRE(helix::ams::lane_sources(reg.lane(1)).record.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "another client's change to a record shows on the next read",
                 "[lane][refresh][toolchanger]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T1", record("PETG", "#1E5AA8"));
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    REQUIRE(reg->get_slot_info(1).material == "PETG");

    printer.client.mock_db_set("lane_data", "T1", record("ABS", "#FF0000"));
    helix::ToolChangerTestAccess::refresh_lane_records(*reg);
    settle();
    CHECK(reg->get_slot_info(1).material == "ABS");
    CHECK(reg->get_slot_info(1).color_rgb == 0xFF0000u);

    // A record deleted elsewhere leaves the lane with no record to show.
    printer.client.mock_db_set("lane_data", "T1", nullptr);
    helix::ToolChangerTestAccess::refresh_lane_records(*reg);
    settle();
    CHECK_FALSE(helix::ams::lane_sources(reg.lane(1)).record.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an edit shows at once and its record's echo retires it",
                 "[lane][refresh][pending][toolchanger]") {
    MockPrinter printer;
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();

    const helix::SlotInfo original = reg->get_slot_info(1);
    helix::SlotInfo edited = original;
    edited.material = "PETG";
    edited.color_rgb = 0x1E5AA8;
    REQUIRE(reg->commit_user_edit(1, original, edited).success());
    CHECK(reg->get_slot_info(1).material == "PETG");

    const auto stored = printer.client.mock_db_get("lane_data", "T1");
    REQUIRE(stored.is_object());
    CHECK(stored["writer"] == "helixscreen");
    CHECK(stored["filament_type"] == "PETG");

    // The write's success starts a read; its record is the echo.
    settle();
    CHECK_FALSE(helix::ams::lane_sources(reg.lane(1)).pending.has_value());
    CHECK(reg->get_slot_info(1).material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "an edit whose record write fails is withdrawn",
                 "[lane][refresh][pending][toolchanger]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T1", record("PLA", "#FFFFFF"));
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    REQUIRE(reg->get_slot_info(1).material == "PLA");

    printer.client.fail_next("server.database.post_item");
    const helix::SlotInfo original = reg->get_slot_info(1);
    helix::SlotInfo edited = original;
    edited.material = "PETG";
    (void)reg->commit_user_edit(1, original, edited);
    settle();
    CHECK_FALSE(helix::ams::lane_sources(reg.lane(1)).pending.has_value());
    CHECK(helix::ams::resolved_lane(reg.lane(1)).material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "an edit never overwrites a record another producer writes",
                 "[lane][refresh][writer_rule][toolchanger]") {
    MockPrinter printer;
    nlohmann::json theirs = record("PLA", "#FFFFFF");
    theirs["writer"] = "spoolman-lane-sync";
    printer.client.mock_db_set("lane_data", "T1", theirs);
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();

    const helix::SlotInfo original = reg->get_slot_info(1);
    helix::SlotInfo edited = original;
    edited.material = "PETG";
    (void)reg->commit_user_edit(1, original, edited);
    settle();
    CHECK(printer.client.mock_db_get("lane_data", "T1") == theirs);
    CHECK_FALSE(helix::ams::lane_sources(reg.lane(1)).pending.has_value());
    CHECK(reg->get_slot_info(1).material == "PLA");
}

namespace {

/// An AFC backend on a mock Moonraker.
class StartedAfc : public helix::AmsBackendAfc {
  public:
    explicit StartedAfc(helix::IMoonrakerAPI* api) : helix::AmsBackendAfc(api, nullptr) {
        helix::AfcTestAccess::initialize_slots(*this, std::vector<std::string>{"lane1", "lane2"});
    }
    /// Start once registered, so the lane ids the read files onto exist.
    void start_now() {
        on_started();
        settle();
    }
    ~StartedAfc() override {
        settle();
    }
    helix::AmsError execute_gcode(const std::string&) override {
        return helix::AmsErrorHelper::success();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC's extras record supplies what AFC cannot hold, and nothing AFC does",
                 "[lane][refresh][extras][afc]") {
    MockPrinter printer;
    printer.client.mock_db_set("helix-screen-afc-overrides", "lane1",
                               {{"lane", "0"},
                                {"vendor_name", "Polymaker"},
                                {"name", "PolyTerra"},
                                {"spool_id", 99},
                                {"material", "ABS"},
                                {"color", "#00FF00"}});
    helix::test::RegisteredBackend<StartedAfc> reg(&printer.api);
    reg->start_now();
    const auto lane = helix::ams::resolved_lane(reg.lane(0));
    CHECK(lane.brand == std::string("Polymaker"));
    CHECK(lane.spool_name == std::string("PolyTerra"));
    // A spool id, material or colour in the extras record is not the lane's.
    CHECK_FALSE(lane.spoolman_id.has_value());
    CHECK_FALSE(lane.material.has_value());
    CHECK_FALSE(lane.color_rgb.has_value());
}

namespace {

/// AmsState's subjects for one case, torn down before the next.
struct ScopedAmsSubjects {
    ScopedAmsSubjects() {
        auto& ams = helix::AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);
    }
    ~ScopedAmsSubjects() {
        helix::ui::UpdateQueue::instance().drain();
        helix::AmsState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "lane editing waits for the first lane-record read",
                 "[lane][refresh][toolchanger][ready]") {
    ScopedAmsSubjects subjects;
    auto& ams = helix::AmsState::instance();
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T1", record("PETG", "#1E5AA8"));
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);

    // The first read fails: nothing has been read, so nothing may be edited.
    printer.client.fail_next("server.database.get_item");
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    REQUIRE_FALSE(helix::ams::lane_sources(reg.lane(1)).record.has_value());
    CHECK_FALSE(reg->lane_records_ready());
    ams.sync_from_backend();
    CHECK(lv_subject_get_int(ams.get_ams_lane_records_ready_subject()) == 0);
    {
        const helix::SlotInfo original = reg->get_slot_info(1);
        helix::SlotInfo edited = original;
        edited.material = "ABS";
        CHECK_FALSE(ams.commit_slot_edit(1, original, edited).success());
        CHECK(printer.client.mock_db_get("lane_data", "T1")["material"] == "PETG");
    }

    // The poll's retry lands, and editing opens.
    helix::ToolChangerTestAccess::refresh_lane_records(*reg);
    settle();
    REQUIRE(helix::ams::lane_sources(reg.lane(1)).record.has_value());
    CHECK(reg->lane_records_ready());
    ams.sync_from_backend();
    CHECK(lv_subject_get_int(ams.get_ams_lane_records_ready_subject()) == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "an AFC edit writes only AFC's extras, never lane_data",
                 "[lane][refresh][extras][writer_rule][afc]") {
    MockPrinter printer;
    helix::test::RegisteredBackend<StartedAfc> reg(&printer.api);
    reg->start_now();

    const helix::SlotInfo original = reg->get_slot_info(0);
    helix::SlotInfo edited = original;
    edited.brand = "Polymaker";
    edited.material = "PETG";
    edited.color_rgb = 0x1E5AA8;
    (void)reg->commit_user_edit(0, original, edited);
    settle();

    CHECK(printer.client.mock_db_get("lane_data", "lane1").is_null());
    const nlohmann::json extras = printer.client.mock_db_get("helix-screen-afc-overrides", "lane1");
    REQUIRE(extras.is_object());
    CHECK(extras["vendor_name"] == "Polymaker");
    // AFC holds these itself; the extras record never states them.
    CHECK(extras["material"].is_null());
    CHECK(extras["color"].is_null());
    CHECK(extras["spool_id"].is_null());
}

TEST_CASE_METHOD(LVGLTestFixture, "a read asked for while one is out runs once more, not twice",
                 "[lane][refresh][toolchanger]") {
    MockPrinter printer;
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    const int before = printer.client.call_count("server.database.get_item");

    printer.client.defer_next("server.database.get_item");
    helix::ToolChangerTestAccess::refresh_lane_records(*reg);
    helix::ToolChangerTestAccess::refresh_lane_records(*reg);
    helix::ToolChangerTestAccess::refresh_lane_records(*reg);
    CHECK(printer.client.call_count("server.database.get_item") == before + 1);
    printer.client.fire_deferred("server.database.get_item");
    settle();
    CHECK(printer.client.call_count("server.database.get_item") == before + 2);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the poll reads the lane records and shows another client's change",
                 "[lane][refresh][toolchanger]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T1", record("PETG", "#1E5AA8"));
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    REQUIRE(reg->get_slot_info(1).material == "PETG");

    printer.client.mock_db_set("lane_data", "T1", record("ABS", "#FF0000"));
    helix::ToolChangerTestAccess::poll_lane_records(*reg);
    settle();
    CHECK(reg->get_slot_info(1).material == "ABS");
}

// ============================================================================
// Review findings: writes before the first read, stale reads, refused clears,
// a reopened store, reconnecting
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a meter flush before the first read never replaces a record",
                 "[lane][refresh][toolchanger][review]") {
    MockPrinter printer;
    nlohmann::json held = record("PLA", "#FFFFFF");
    held["vendor_name"] = "Polymaker";
    printer.client.mock_db_set("lane_data", "T1", held);
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    printer.client.fail_next("server.database.get_item");
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    REQUIRE_FALSE(reg->lane_records_ready());

    reg->update_slot_weight(1, 300.0f, 1000.0f, /*persist=*/true);
    settle();
    const nlohmann::json stored = printer.client.mock_db_get("lane_data", "T1");
    CHECK(stored["material"] == "PLA");
    CHECK(stored["vendor_name"] == "Polymaker");
}

TEST_CASE_METHOD(LVGLTestFixture, "a read sent before our write lands is not adopted",
                 "[lane][refresh][toolchanger][review]") {
    MockPrinter printer;
    nlohmann::json old_record = record("PLA", "#FFFFFF");
    old_record["vendor_name"] = "Polymaker";
    printer.client.mock_db_set("lane_data", "T1", old_record);
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    REQUIRE(reg->get_slot_info(1).material == "PLA");

    // A poll's read goes out; the user's edit is written while it is.
    printer.client.defer_next("server.database.get_item");
    helix::ToolChangerTestAccess::refresh_lane_records(*reg);
    const helix::SlotInfo original = reg->get_slot_info(1);
    helix::SlotInfo edited = original;
    edited.material = "PETG";
    edited.brand = "Sunlu";
    REQUIRE(reg->commit_user_edit(1, original, edited).success());
    REQUIRE(printer.client.mock_db_get("lane_data", "T1")["filament_type"] == "PETG");

    // The read answers with what it saw before the write.
    const nlohmann::json written = printer.client.mock_db_get("lane_data", "T1");
    printer.client.mock_db_set("lane_data", "T1", old_record);
    printer.client.fire_deferred("server.database.get_item");
    printer.client.mock_db_set("lane_data", "T1", written);
    helix::ui::UpdateQueue::instance().drain();

    // A meter flush stages from what the backend holds now.
    reg->update_slot_weight(1, 300.0f, 1000.0f, /*persist=*/true);
    settle();
    const nlohmann::json stored = printer.client.mock_db_get("lane_data", "T1");
    CHECK(stored["filament_type"] == "PETG");
    CHECK(stored["vendor_name"] == "Sunlu");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a clear the writer rule refuses leaves the lane showing its record",
                 "[lane][refresh][toolchanger][writer_rule][review]") {
    MockPrinter printer;
    nlohmann::json theirs = record("PLA", "#FFFFFF");
    theirs["writer"] = "spoolman-lane-sync";
    printer.client.mock_db_set("lane_data", "T1", theirs);
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    REQUIRE(reg->get_slot_info(1).material == "PLA");

    reg->clear_slot_override(1);
    settle();
    CHECK(printer.client.mock_db_get("lane_data", "T1") == theirs);
    CHECK(reg->get_slot_info(1).material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "a store reopened while a read is out still gets read",
                 "[lane][refresh][toolchanger][review]") {
    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T1", record("PETG", "#1E5AA8"));
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    printer.client.defer_next("server.database.get_item");
    helix::ToolChangerTestAccess::call_on_started(*reg);
    // A reconnect reopens the store while the first read is still out.
    helix::ToolChangerTestAccess::call_on_started(*reg);
    printer.client.fire_deferred("server.database.get_item");
    settle();
    CHECK(reg->lane_records_ready());
    CHECK(reg->get_slot_info(1).material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "reconnecting reopens editing once the lanes are read",
                 "[lane][refresh][toolchanger][ready][review]") {
    ScopedAmsSubjects subjects;
    auto& ams = helix::AmsState::instance();
    MockPrinter printer;
    helix::test::RegisteredBackend<RecordBackedChanger> reg(&printer.api);
    helix::ToolChangerTestAccess::call_on_started(*reg);
    settle();
    const int connected = static_cast<int>(helix::ConnectionState::CONNECTED);
    const int lost = static_cast<int>(helix::ConnectionState::DISCONNECTED);
    lv_subject_set_int(get_printer_state().network_state().get_printer_connection_state_subject(),
                       connected);
    settle();
    ams.sync_from_backend();
    REQUIRE(lv_subject_get_int(ams.get_ams_lane_records_ready_subject()) == 1);

    lv_subject_set_int(get_printer_state().network_state().get_printer_connection_state_subject(),
                       lost);
    settle();
    REQUIRE(lv_subject_get_int(ams.get_ams_lane_records_ready_subject()) == 0);
    lv_subject_set_int(get_printer_state().network_state().get_printer_connection_state_subject(),
                       connected);
    settle();
    CHECK(lv_subject_get_int(ams.get_ams_lane_records_ready_subject()) == 1);
}
