// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 69 to 100 of test_ams_backend_cfs.cpp before it was split, in its order. Mostly
// [flat][filament_slot_override], [filament_slot_override][firmware_writeback],
// [presence][filament_slot_override], [filament_slot_override][1390], [filament_slot_override], and
// more

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/cfs_test_fixtures.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/force_bypass_controls_guard.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toast_log.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_mapper.h"
#include "filament_op_dispatch.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_resolver.h"
#include "lane_translation.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "system/telemetry_manager.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/cfs_box_fixtures.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;
using helix::printer::AmsBackendCfs;
using helix::printer::CfsMaterialDb;
using helix::test::make_cfs_status_json;
using helix::test::make_unit_box_explicit;

using helix::test::CfsAutoRefillHelper;
using helix::test::CfsCalibHelper;
using helix::test::CfsInsertRuleRig;
using helix::test::CfsK1RemapHelper;
using helix::test::CfsK2CalibHelper;
using helix::test::CfsLifecycleHelper;
using helix::test::CfsLiveAckBackend;
using helix::test::CfsOverrideRig;
using helix::test::CfsRemapHelper;
using helix::test::CfsTmpCacheDir;
using helix::test::check_not_single_spool;
using helix::test::configfile_queries;
using helix::test::drain_calib_queue;
using helix::test::feed_toolhead_filament;
using helix::test::find_action;
using helix::test::FlatFailDispatchCfs;
using helix::test::HeldAckCfs;
using helix::test::json;
using helix::test::k2_plus_startup_box;
using helix::test::k2_plus_t1_connected;
using helix::test::kEditedRemainingG;
using helix::test::link_lane_four_to_spool_137;
using helix::test::MacroCacheScope;
using helix::test::make_boot_box_all_units_down;
using helix::test::make_cfs_notification;
using helix::test::make_empty_bus_box;
using helix::test::make_flat_box;
using helix::test::make_flat_fork_box;
using helix::test::make_fork_box_with_boxes;
using helix::test::make_gapped_fork_box;
using helix::test::make_multi_unit_box;
using helix::test::make_runout_box;
using helix::test::make_runout_removed_box;
using helix::test::make_runout_seated_box;
using helix::test::make_single_unit_box;
using helix::test::make_unbranded_bay_box;
using helix::test::make_unit_up_delta;
using helix::test::no_unit_settings;
using helix::test::paused_ctx;
using helix::test::per_slot_box;
using helix::test::per_slot_unit;
using helix::test::registered_unit_settings;
using helix::test::script_has_command;
using helix::test::SyncCfs;
using helix::test::toolhead_sensor_frame;
using helix::test::UnansweredClientCfs;
using helix::test::UnhomedCfsBackend;
using json = nlohmann::json;

TEST_CASE("CFS a linked lane's moved material waits on the box and reports a rejection",
          "[ams][cfs][filament_slot_override][firmware_writeback][box_wins]") {
    helix::test::RegisteredWired<CfsRemapHelper> reg;
    auto& b = *reg;
    b.fire_status(make_cfs_notification(
        make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"})));

    SlotInfo original = b.get_slot_info(0);
    original.spoolman_id = 149;
    SlotInfo edit = original;
    edit.material = "ASA";
    const std::uint64_t since = helix::ams::pending_filings(reg.lane(0));

    SECTION("in flight, the lane holds the edit as pending") {
        (void)b.commit_user_edit(0, original, edit);
        const auto pending = helix::ams::lane_sources(reg.lane(0)).pending;
        REQUIRE(pending.has_value());
        CHECK(pending->material == "ASA");
    }
    SECTION("a rejected write settles as failed") {
        b.reject = true;
        (void)b.commit_user_edit(0, original, edit);
        std::optional<helix::ams::PendingOutcome> heard;
        helix::ams::await_pending(
            reg.lane(0), since,
            [&heard](helix::ams::PendingOutcome outcome, const std::string&) { heard = outcome; });
        REQUIRE(heard.has_value());
        CHECK(*heard == helix::ams::PendingOutcome::Failed);
    }
}

TEST_CASE("CFS refuses a box write while a print holds the printer",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    struct PrintingCfs : CfsRemapHelper {
        using CfsRemapHelper::CfsRemapHelper;
        [[nodiscard]] bool print_holds_printer() const override {
            return true;
        }
    };
    helix::test::RegisteredWired<PrintingCfs> reg;
    auto& b = *reg;
    b.fire_status(make_cfs_notification(
        make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"})));

    SlotInfo edit = b.get_slot_info(0);
    edit.color_rgb = 0x00FF00;
    const auto err = b.commit_user_edit(0, b.get_slot_info(0), edit);
    CHECK_FALSE(err.success());
    CHECK_FALSE(err.user_msg.empty());
    CHECK(b.captured.empty());
}

TEST_CASE("CFS Clear Spool writes the box's clear once",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    helix::test::RegisteredWired<CfsRemapHelper> reg;
    auto& b = *reg;
    const auto clear_as_the_menu_does = [&b] {
        SlotInfo cleared = b.get_slot_info(0);
        cleared.material.clear();
        cleared.color_rgb = kAmsDefaultSlotColor;
        cleared.brand.clear();
        REQUIRE(b.commit_user_edit(0, b.get_slot_info(0), cleared).success());
        static_cast<helix::AmsBackend&>(b).clear_slot_override(0);
    };
    const auto count = [&b](const std::string& needle) {
        return std::count_if(b.captured.begin(), b.captured.end(), [&needle](const auto& g) {
            return g.find(needle) != std::string::npos;
        });
    };

    SECTION("stock, with a tag seated: material, colour and remaining length") {
        b.fire_status(make_cfs_notification(
            make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"})));
        clear_as_the_menu_does();
        CHECK(count("PART=material_type DATA=-1") == 1);
        CHECK(count("PART=color_value DATA=-1") == 1);
        CHECK(count("PART=remain_len DATA=-1") == 1);
    }
    SECTION("Fork") {
        b.fire_status(make_cfs_notification(make_flat_fork_box()));
        clear_as_the_menu_does();
        CHECK(count("_BOX_SLOT_CLEAR SLOT=0") == 1);
    }
    SECTION("a Clear while an edit's box write is out erases what Klipper's OK would save") {
        b.fire_status(make_cfs_notification(
            make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"})));
        b.hold = true;
        SlotInfo edit = b.get_slot_info(0);
        edit.brand = "Sunlu";
        REQUIRE(b.commit_user_edit(0, b.get_slot_info(0), edit).success());
        static_cast<helix::AmsBackend&>(b).clear_slot_override(0);
        b.held_ack();
        CHECK(CfsTestAccess::get_override(b, 0)
                  .value_or(helix::ams::FilamentSlotOverride{})
                  .brand.empty());
    }
}

TEST_CASE("CFS Fork: an edit naming no colour keeps the colour the box holds",
          "[ams][cfs][fork][firmware_writeback]") {
    helix::test::RegisteredWired<CfsRemapHelper> reg;
    auto& b = *reg;
    b.fire_status(make_cfs_notification(make_flat_box("PLA", "Polymaker", "PolyLite", "#1A1A1A")));

    SlotInfo edit = b.get_slot_info(0);
    edit.material = "PETG";
    edit.color_rgb = kAmsDefaultSlotColor;
    REQUIRE(b.commit_user_edit(0, b.get_slot_info(0), edit).success());
    REQUIRE(b.captured.size() == 1);
    CHECK(b.captured[0].find("COLOR=\"#1A1A1A\"") != std::string::npos);
    CHECK(b.captured[0].find("808080") == std::string::npos);
}

TEST_CASE("CFS a material the box has no code for shows from the lane record",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    helix::test::RegisteredWired<CfsRemapHelper> reg;
    auto& b = *reg;
    const json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                          {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    b.fire_status(make_cfs_notification(box));

    SlotInfo edit = b.get_slot_info(0);
    edit.material = "Unobtanium";
    edit.brand = "ACME";
    REQUIRE(b.commit_user_edit(0, b.get_slot_info(0), edit).success());
    for (const auto& gcode : b.captured) {
        CHECK(gcode.find("PART=material_type") == std::string::npos);
    }

    // The record lands, and the box still carries the previous label's code.
    const auto record = CfsTestAccess::get_override(b, 0);
    REQUIRE(record.has_value());
    helix::test::file_override_as_lane_records(b, 0, *record);
    b.fire_status(make_cfs_notification(box));
    CHECK(b.get_slot_info(0).material == "Unobtanium");
    CHECK(b.get_slot_info(0).brand == "ACME");
}

TEST_CASE("CFS a generic code leaves the lane record's brand standing",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    helix::test::RegisteredWired<CfsRemapHelper> reg;
    auto& b = *reg;
    b.fire_status(make_cfs_notification(
        make_single_unit_box({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"})));

    SlotInfo edit = b.get_slot_info(0);
    edit.material = "PLA";
    edit.brand = "Sunlu";
    edit.color_rgb = 0x00FF00;
    REQUIRE(b.commit_user_edit(0, b.get_slot_info(0), edit).success());
    REQUIRE(b.captured[0].find("PART=material_type DATA=000001") != std::string::npos);

    const auto record = CfsTestAccess::get_override(b, 0);
    REQUIRE(record.has_value());
    helix::test::file_override_as_lane_records(b, 0, *record);
    // The box restates the generic code it was given, which names no brand.
    b.fire_status(make_cfs_notification(
        make_single_unit_box({"000001", "-1", "-1", "-1"}, {"000FF00", "-1", "-1", "-1"})));
    CHECK(b.get_slot_info(0).material == "PLA");
    CHECK(b.get_slot_info(0).brand == "Sunlu");
    CHECK_FALSE(helix::ams::lane_sources(reg.lane(0)).pending.has_value());
}

TEST_CASE("CFS a user's brand outranks a guess from an unreadable tag",
          "[ams][cfs][filament_slot_override][firmware_writeback][2048]") {
    // A third-party spool's tag the box cannot decode: no material code, and a
    // vender entry that names no vendor. The brand the parse guesses from it is
    // no reading of the spool, so the user's own brand must stand over it.
    std::string vender;
    SECTION("vender reads unknown") {
        vender = "unknown";
    }
    SECTION("vender reads a raw tag UID") {
        vender = "04A1B2C3";
    }

    helix::test::RegisteredWired<CfsRemapHelper> reg;
    auto& b = *reg;
    json box = make_single_unit_box({"-1", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"});
    box["T1"]["vender"][0] = vender;
    b.fire_status(make_cfs_notification(box));
    REQUIRE(b.get_slot_info(0).status != SlotStatus::EMPTY);

    SlotInfo edit = b.get_slot_info(0);
    edit.material = "";
    edit.brand = "Anycubic";
    edit.color_rgb = 0xFF0000;
    REQUIRE(b.commit_user_edit(0, b.get_slot_info(0), edit).success());

    const auto record = CfsTestAccess::get_override(b, 0);
    REQUIRE(record.has_value());
    helix::test::file_override_as_lane_records(b, 0, *record);
    // The box restates the colour it was given and still has no code.
    box["T1"]["color_value"][0] = "0FF0000";
    b.fire_status(make_cfs_notification(box));
    CHECK(b.get_slot_info(0).brand == "Anycubic");
    CHECK(b.get_slot_info(0).color_rgb == 0xFF0000u);
}

TEST_CASE("CFS bypass declaration lives in Moonraker beside the lane records",
          "[ams][cfs][bypass][filament_slot_override]") {
    CfsOverrideRig rig("cfs_bypass_declared_key");
    CfsTestAccess::mark_running(*rig.backend);
    rig.poll(make_single_unit_box({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"}));
    rig.backend->refresh_lane_records();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(rig.backend->is_bypass_active());

    SECTION("enable writes the key and disable deletes it") {
        REQUIRE(rig.backend->enable_bypass().success());
        CHECK(rig.client.mock_db_get("helix-screen", "cfs_bypass_declared") == true);
        // Nothing of the declaration's lands among the lane records.
        CHECK(rig.client.mock_db_get("lane_data", "helix_bypass_declared").is_null());
        REQUIRE(rig.backend->disable_bypass().success());
        CHECK(rig.client.mock_db_get("helix-screen", "cfs_bypass_declared").is_null());
    }

    SECTION("a loaded bay or a lane load withdraws the stored declaration") {
        REQUIRE(rig.backend->enable_bypass().success());
        REQUIRE(rig.client.mock_db_get("helix-screen", "cfs_bypass_declared") == true);
        SECTION("a bay loaded") {
            json box =
                make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
            box["T1"]["filament"] = "A";
            rig.poll(box);
        }
        SECTION("a lane load") {
            REQUIRE(rig.backend->load_filament(0).success());
        }
        CHECK(rig.client.mock_db_get("helix-screen", "cfs_bypass_declared").is_null());
    }

    SECTION("another client's declaration is picked up by the poll") {
        rig.client.mock_db_set("helix-screen", "cfs_bypass_declared", true);
        rig.backend->refresh_lane_records();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(rig.backend->is_bypass_active());

        // And its withdrawal.
        rig.client.mock_db()["helix-screen"].erase("cfs_bypass_declared");
        rig.backend->refresh_lane_records();
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(rig.backend->is_bypass_active());
    }

    SECTION("a read older than our own write does not flip the toggle back") {
        REQUIRE(rig.backend->enable_bypass().success());
        CHECK_FALSE(CfsTestAccess::adopt_bypass_declaration(*rig.backend, false));
        CHECK(rig.backend->is_bypass_active());

        // The read that returns the write confirms it; after that a read
        // without the key is another client's withdrawal.
        CfsTestAccess::adopt_bypass_declaration(*rig.backend, true);
        CfsTestAccess::adopt_bypass_declaration(*rig.backend, false);
        CHECK_FALSE(rig.backend->is_bypass_active());
    }

    SECTION("a write that fails leaves the next read in charge") {
        rig.client.fail_next("server.database.post_item");
        REQUIRE(rig.backend->enable_bypass().success());
        helix::ui::UpdateQueue::instance().drain();
        CfsTestAccess::adopt_bypass_declaration(*rig.backend, false);
        CHECK_FALSE(rig.backend->is_bypass_active());
    }
}

TEST_CASE("CFS two color edits in one poll window both survive their echoes",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    CfsOverrideRig rig("cfs_double_color_edit");

    json box_before = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                           {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    rig.poll(box_before);
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "101001|0FF5500");

    // The user retunes the colour twice before firmware has echoed either
    // write. Both pushes are in flight at once, so both echoes must be
    // expected simultaneously: the first echo landing after the second push
    // is still OUR write coming home, not a swap.
    SlotInfo edit1;
    edit1.material = "ASA-CF";
    edit1.color_rgb = 0x1A1A1A;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit1);

    SlotInfo edit2;
    edit2.material = "ASA-CF";
    edit2.color_rgb = 0x2B2B2B;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit2);

    auto staged = CfsTestAccess::get_override(*rig.backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->color_rgb == 0x2B2B2Bu);

    // Polls before either echo: firmware still reports the OLD colour.
    rig.poll(box_before);
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

    // First write lands: the generic ASA-CF code and colour 01A1A1A. The
    // second is still in flight, then its record lands.
    rig.poll(make_single_unit_box({"000033", "101001", "101001", "101001"},
                                  {"01A1A1A", "0FFFFFF", "00A2989", "0C12E1F"}));
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    rig.record_lands();

    // Second write lands: colour 02B2B2B, the value the user last chose.
    rig.poll(make_single_unit_box({"000033", "101001", "101001", "101001"},
                                  {"02B2B2B", "0FFFFFF", "00A2989", "0C12E1F"}));
    auto settled = CfsTestAccess::get_override(*rig.backend, 0);
    REQUIRE(settled.has_value());
    CHECK(settled->color_rgb == 0x2B2B2Bu);
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "000033|02B2B2B");

    // With the edit settled, a genuine swap is detected normally.
    rig.poll(make_single_unit_box({"102001", "101001", "101001", "101001"},
                                  {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"}));
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

TEST_CASE("CFS a first edit's echo after the second edit's record is no swap",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    CfsOverrideRig rig("cfs_quick_edits");
    json box_before = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                           {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    rig.poll(box_before);

    SlotInfo edit1 = rig.backend->get_slot_info(0);
    edit1.color_rgb = 0x1A1A1A;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit1);
    SlotInfo edit2 = rig.backend->get_slot_info(0);
    edit2.color_rgb = 0x2B2B2B;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit2);

    // The second edit's record lands first and ends its Pending; then the box
    // restates the first edit, then the second.
    rig.record_lands();
    REQUIRE_FALSE(helix::ams::lane_sources(rig.backend->lane_id(0)).pending.has_value());
    rig.poll(make_single_unit_box({"101001", "101001", "101001", "101001"},
                                  {"01A1A1A", "0FFFFFF", "00A2989", "0C12E1F"}));
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    rig.poll(make_single_unit_box({"101001", "101001", "101001", "101001"},
                                  {"02B2B2B", "0FFFFFF", "00A2989", "0C12E1F"}));
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    CHECK(rig.backend->get_slot_info(0).color_rgb == 0x2B2B2Bu);

    // The box restated the last write, so a change from here is a swap.
    rig.poll(make_single_unit_box({"102001", "101001", "101001", "101001"},
                                  {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"}));
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

TEST_CASE("CFS flat-schema fingerprint change clears override (hardware swap detected)",
          "[ams][cfs][flat][filament_slot_override]") {
    CfsOverrideRig rig("cfs_flat_swap_clears");

    // Seed override AND the corresponding DB record so the swap's clear deletes
    // something observable, matching the stock-schema test above.
    rig.client.mock_db_set(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(*rig.backend, 0, ovr);

    // First flat frame establishes the baseline from bay 0's identity. No clear.
    json box1 = make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500");
    rig.poll(box1);

    // Pins what the flat feed builds a fingerprint from: material and
    // colour, the fields the insert rule compares.
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|FF5500");
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    REQUIRE(!rig.client.mock_db_get("lane_data", "lane1").is_null());

    // Same identity again: the same spool re-observed, the override stands.
    rig.poll(box1);
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

    // A different identity is a different physical spool.
    json box2 = make_flat_box("PETG", "Bambu", "Basic Green", "#00FF00");
    rig.poll(box2);

    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PETG|00FF00");
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    CHECK(rig.client.mock_db_get("lane_data", "lane1").is_null());
}

TEST_CASE("CFS flat-schema bay with no identity is not a fingerprint signal",
          "[ams][cfs][flat][filament_slot_override]") {
    CfsOverrideRig rig("cfs_flat_identity_less_noop");

    rig.client.mock_db_set("lane_data", "lane1", json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    CfsTestAccess::seed_override(*rig.backend, 0, ovr);

    json box1 = make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500");
    rig.poll(box1);
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|FF5500");

    // Bay 0 occupied but carrying no identity ("None" is this module's spelling
    // of absent): an empty fingerprint, which must not touch the baseline or
    // clear: the contract the stock schema's sentinels hold. A frame that
    // could not read the tag must not mask the swap the next good read reports.
    json unreadable = make_flat_box("None", "None", "None", "None");
    rig.poll(unreadable);
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|FF5500");
    CHECK(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    CHECK(!rig.client.mock_db_get("lane_data", "lane1").is_null());

    // The next good read matches the baseline: no clear, no corrupted state.
    rig.poll(box1);
    CHECK(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

TEST_CASE("CFS flat-schema identity writeback echo does not self-wipe the override",
          "[ams][cfs][flat][filament_slot_override][firmware_writeback]") {
    CfsOverrideRig rig("cfs_flat_echo_survives");

    // Fork dialect baseline from the tag's own identity.
    json box_before = make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500");
    rig.poll(box_before);
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|FF5500");

    // User assigns a new identity. The fork branch pushes _BOX_SLOT_SET, whose
    // values box.py echoes back in a later status frame.
    SlotInfo edit;
    edit.material = "asa-cf";
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite ASA";
    edit.color_rgb = 0x1A1A1A;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit);

    // The record carries the material as the module stores it, uppercased.
    auto staged = CfsTestAccess::get_override(*rig.backend, 0);
    REQUIRE(staged.has_value());
    REQUIRE(staged->material == "ASA-CF");
    rig.record_lands();

    // Polls before the echo lands read the OLD identity: unchanged, no clear.
    rig.poll(box_before);
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

    // The echo reports exactly what we pushed: material uppercased by
    // slot_set_gcode, color in the #RRGGBB form it wrote.
    json box_echo = make_flat_box("ASA-CF", "Polymaker", "PolyLite ASA", "#1A1A1A");
    rig.poll(box_echo);

    auto after_echo = CfsTestAccess::get_override(*rig.backend, 0);
    REQUIRE(after_echo.has_value());
    CHECK(after_echo->material == "ASA-CF");
    CHECK(rig.backend->get_slot_info(0).material == "ASA-CF");
    CHECK(after_echo->color_rgb == 0x1A1A1Au);
    // The baseline advanced to the echoed fingerprint, so a later genuine swap
    // is still measured against firmware truth.
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "ASA-CF|1A1A1A");

    // A genuine swap after the echo still clears the override.
    rig.poll(make_flat_box("PETG", "Bambu", "Basic Green", "#00FF00"));
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

TEST_CASE("CFS flat-schema an edit the box never received is refused and saves nothing",
          "[ams][cfs][flat][filament_slot_override]") {
    CfsOverrideRig<FlatFailDispatchCfs> rig("cfs_flat_dispatch_fails");

    json box_before = make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500");
    rig.poll(box_before);

    SlotInfo edit;
    edit.material = "asa-cf";
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite ASA";
    edit.color_rgb = 0x1A1A1A;
    const auto err = rig.backend->commit_user_edit(0, rig.backend->get_slot_info(0), edit);

    CHECK_FALSE(err.success());
    // The record, if the box's own reading was mirrored into one, still
    // carries the box's identity.
    const auto record = CfsTestAccess::get_override(*rig.backend, 0);
    CHECK((!record.has_value() || record->material == "PLA"));
    CHECK_FALSE(helix::ams::lane_sources(rig.backend->lane_id(0)).pending.has_value());
    CHECK(rig.backend->get_slot_info(0).material == "PLA");
}

TEST_CASE("CFS flat-schema two edits in one poll window both survive their echoes",
          "[ams][cfs][flat][filament_slot_override][firmware_writeback]") {
    CfsOverrideRig rig("cfs_flat_double_edit");

    json box_before = make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500");
    rig.poll(box_before);
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|FF5500");

    // Two identity writes dispatched before firmware has echoed either: the
    // first write's echo is still OUR value coming home when it lands after
    // the second push, and must not read as a swap that wipes the edit.
    SlotInfo edit1;
    edit1.material = "asa-cf";
    edit1.brand = "Polymaker";
    edit1.spool_name = "PolyLite ASA";
    edit1.color_rgb = 0x1A1A1A;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit1);

    SlotInfo edit2;
    edit2.material = "petg";
    edit2.brand = "Bambu";
    edit2.spool_name = "Basic Green";
    edit2.color_rgb = 0x2B2B2B;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit2);

    auto staged = CfsTestAccess::get_override(*rig.backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->material == "PETG");

    // Firmware still reporting the pre-edit identity: Unchanged.
    rig.poll(box_before);
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

    // First write's echo (material uppercased by _BOX_SLOT_SET).
    rig.poll(make_flat_box("ASA-CF", "Polymaker", "PolyLite ASA", "#1A1A1A"));
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    rig.record_lands();

    // Second write's echo: the identity the user last chose.
    rig.poll(make_flat_box("PETG", "Bambu", "Basic Green", "#2B2B2B"));
    auto settled = CfsTestAccess::get_override(*rig.backend, 0);
    REQUIRE(settled.has_value());
    CHECK(settled->material == "PETG");
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PETG|2B2B2B");

    // The edit settled: a genuine swap clears normally.
    rig.poll(make_flat_box("ABS", "Prusa", "Prusa Orange", "#FF8800"));
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

TEST_CASE("CFS writes no lane record before the first read of them lands",
          "[ams][cfs][filament_slot_override]") {
    MockPrinter mock_printer;
    helix::ams::FilamentSlotOverride saved;
    saved.brand = "Polymaker";
    saved.spoolman_id = 42;
    saved.material = "PLA";
    saved.color_rgb = 0xFF5500;
    saved.color_set = true;
    mock_printer.client.mock_db_set("lane_data", "lane1",
                                    helix::ams::to_lane_data_record(0, saved));

    // Delivered straight to the handler, so the frame runs with the read's
    // adoption still queued.
    struct FrameFirstCfs : AmsBackendCfs {
        using AmsBackendCfs::AmsBackendCfs;
        using AmsBackendCfs::handle_status_update;
    };
    helix::test::RegisteredBackend<helix::test::WiredBackend<FrameFirstCfs>> backend(
        &mock_printer.api);
    REQUIRE(backend->start().success());
    // A box frame beats the read: the bay is loaded and states a colour.
    backend->handle_status_update(make_cfs_notification(
        make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"})));

    const auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored.value("spool_id", 0) == 42);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("CFS restart compares against the fingerprint the record carried",
          "[ams][cfs][flat][filament_slot_override]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    json box_f1 = make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500");
    json box_f2 = make_flat_box("PETG", "Bambu", "Basic Green", "#00FF00");

    // A record as a session running older code left it: a user override with
    // no fingerprint. Session 1 boots on it, observes the bay, and the
    // observation must persist into the record: two backends over one mock
    // DB stand in for two app lifetimes.
    helix::ams::FilamentSlotOverride saved;
    saved.brand = "Polymaker";
    saved.spoolman_id = 42;
    saved.material = "PLA";
    saved.color_rgb = 0xFF5500;
    saved.color_set = true;
    mock_printer.client.mock_db_set("lane_data", "lane1",
                                    helix::ams::to_lane_data_record(0, saved));
    {
        helix::test::RegisteredWired<AmsBackendCfs> session1(&api);
        // The record on the wire loaded as this session's override.
        REQUIRE(CfsTestAccess::get_override(*session1, 0).has_value());
        session1->fire_status(make_cfs_notification(box_f1));
        // The write it made re-reads the records through the update queue.
        helix::ui::UpdateQueue::instance().drain_until_empty();
        auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
        REQUIRE(!stored.is_null());
        REQUIRE(stored.contains("helix_fingerprint"));
        CHECK(stored.at("helix_fingerprint") == "PLA|FF5500");
    }

    SECTION("a swap made while the app was down clears the override") {
        helix::test::RegisteredWired<AmsBackendCfs> session2(&api);
        // The loaded record's fingerprint is the baseline, so the first frame
        // after restart is a comparison, not a first observation.
        REQUIRE(CfsTestAccess::get_override(*session2, 0).has_value());
        session2->fire_status(make_cfs_notification(box_f2));
        // The write it made re-reads the records through the update queue.
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(CfsTestAccess::last_rfid_uid(*session2, 0) == "PETG|00FF00");
        CHECK_FALSE(CfsTestAccess::get_override(*session2, 0).has_value());
        CHECK(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
    }

    SECTION("the same spool across a restart keeps the override") {
        helix::test::RegisteredWired<AmsBackendCfs> session2(&api);
        session2->fire_status(make_cfs_notification(box_f1));
        // The write it made re-reads the records through the update queue.
        helix::ui::UpdateQueue::instance().drain_until_empty();
        auto ovr = CfsTestAccess::get_override(*session2, 0);
        REQUIRE(ovr.has_value());
        CHECK(ovr->brand == "Polymaker");
        CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
    }

    SECTION("a record without a stored fingerprint stays a first-observation baseline") {
        // The shape every record written before the field existed has.
        mock_printer.client.mock_db_set("lane_data", "lane1",
                                        helix::ams::to_lane_data_record(0, saved));
        helix::test::RegisteredWired<AmsBackendCfs> session2(&api);
        session2->fire_status(make_cfs_notification(box_f2));
        // The write it made re-reads the records through the update queue.
        helix::ui::UpdateQueue::instance().drain_until_empty();
        // Baseline, not Changed: the identity differs from the record's data,
        // but with no stored fingerprint there is nothing to compare against.
        auto ovr = CfsTestAccess::get_override(*session2, 0);
        REQUIRE(ovr.has_value());
        CHECK(ovr->brand == "Polymaker");
        // The baseline observation itself heals the fingerprint in, so the
        // NEXT restart compares against this spool.
        auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
        REQUIRE(!stored.is_null());
        REQUIRE(stored.contains("helix_fingerprint"));
        CHECK(stored.at("helix_fingerprint") == "PETG|00FF00");
    }
    // The last session's poll reads (records and the bypass key) answer
    // through the queue after its backend is gone; settle them here.
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

TEST_CASE("CFS fingerprint shape names its schema only when unambiguous",
          "[ams][cfs][filament_slot_override]") {
    using helix::printer::CfsSchema;
    // Stock colour_value is "0RRGGBB"; a stock material_type is a numeric code.
    CHECK(AmsBackendCfs::fingerprint_schema("101001|0FF5500") == CfsSchema::Stock);
    CHECK(AmsBackendCfs::fingerprint_schema("unknown|01A1A1A") == CfsSchema::Stock);
    CHECK(AmsBackendCfs::fingerprint_schema("|0FF5500") == CfsSchema::Stock);
    CHECK(AmsBackendCfs::fingerprint_schema("101001|") == CfsSchema::Stock);
    // Six hex digits of colour are flat's spelling whatever the material.
    CHECK(AmsBackendCfs::fingerprint_schema("101001|FF0000") == CfsSchema::Flat);
    CHECK(AmsBackendCfs::fingerprint_schema("123|FF0000") == CfsSchema::Flat);
    // The flat composite carries six hex digits of colour.
    CHECK(AmsBackendCfs::fingerprint_schema("PLA|FF5500") == CfsSchema::Flat);
    CHECK(AmsBackendCfs::fingerprint_schema("|FF5500") == CfsSchema::Flat);
    // Readings both schemas can produce name neither.
    CHECK_FALSE(AmsBackendCfs::fingerprint_schema("PLA|").has_value());
    CHECK_FALSE(AmsBackendCfs::fingerprint_schema("unknown|").has_value());
    CHECK_FALSE(AmsBackendCfs::fingerprint_schema("").has_value());
    CHECK_FALSE(AmsBackendCfs::fingerprint_schema("PLA").has_value());
    CHECK_FALSE(AmsBackendCfs::fingerprint_schema("PLA|eSUN|Black PLA|111111").has_value());
}

TEST_CASE("CFS stock to flat schema switch keeps overrides and still detects swaps",
          "[ams][cfs][flat][filament_slot_override]") {
    CfsOverrideRig rig("cfs_schema_switch_stock_to_flat");

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(*rig.backend, 0, ovr);

    rig.poll(make_single_unit_box({"101001", "101001", "101001", "101001"},
                                  {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"}));
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "101001|0FF5500");
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

    // The box comes back speaking the flat schema about the same spool. A
    // stock baseline says nothing about a flat reading, so this is a first
    // observation, not a swap.
    json same_spool = make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500");
    rig.poll(same_spool);
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|FF5500");
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    CHECK(CfsTestAccess::get_override(*rig.backend, 0)->brand == "Polymaker");

    // A real material change under the new schema still clears it.
    rig.poll(make_flat_box("PETG", "Bambu", "Basic Green", "#00FF00"));
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PETG|00FF00");
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

TEST_CASE("CFS flat to stock schema switch keeps overrides and still detects swaps",
          "[ams][cfs][flat][filament_slot_override]") {
    CfsOverrideRig rig("cfs_schema_switch_flat_to_stock");

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(*rig.backend, 0, ovr);

    rig.poll(make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500"));
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|FF5500");
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

    // A T<n> frame returns the box to the stock schema.
    rig.poll(make_single_unit_box({"101001", "101001", "101001", "101001"},
                                  {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"}));
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "101001|0FF5500");
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    CHECK(CfsTestAccess::get_override(*rig.backend, 0)->brand == "Polymaker");

    rig.poll(make_single_unit_box({"102001", "101001", "101001", "101001"},
                                  {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"}));
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "102001|000FF00");
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

TEST_CASE("CFS colour change across a schema switch is a swap",
          "[ams][cfs][flat][filament_slot_override]") {
    CfsOverrideRig rig("cfs_schema_switch_colour_swap");

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(*rig.backend, 0, ovr);

    SECTION("stock to flat") {
        rig.poll(make_single_unit_box({"101001", "101001", "101001", "101001"},
                                      {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"}));
        REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

        // The spool was swapped while the firmware changed: the flat reading
        // is a different colour, which no schema change explains.
        rig.poll(make_flat_box("PLA", "Polymaker", "PolyLite Green", "#00FF00"));
        CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|00FF00");
        CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    }

    SECTION("flat to stock") {
        rig.poll(make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500"));
        REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

        rig.poll(make_single_unit_box({"101001", "101001", "101001", "101001"},
                                      {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"}));
        CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "101001|000FF00");
        CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    }
}

TEST_CASE("CFS reading either schema could spell compares against a cross-schema baseline",
          "[ams][cfs][flat][filament_slot_override]") {
    CfsOverrideRig rig("cfs_schema_switch_ambiguous_reading");

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    CfsTestAccess::seed_override(*rig.backend, 0, ovr);

    rig.poll(make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500"));
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "PLA|FF5500");
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

    // A stock frame reading "unknown|" for the bay: no colour to compare, and
    // a shape that does not name the stock schema, so it is not grounds to
    // drop the baseline. It compares as a different spool.
    rig.poll(make_single_unit_box({"unknown", "101001", "101001", "101001"},
                                  {"-1", "0FFFFFF", "00A2989", "0C12E1F"}));
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "unknown|");
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

TEST_CASE("CFS persisted fingerprint from the other schema is no baseline after restart",
          "[ams][cfs][flat][filament_slot_override]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    // A record a stock session persisted, loaded by a session whose box now
    // speaks the flat schema.
    helix::ams::FilamentSlotOverride saved;
    saved.brand = "Polymaker";
    saved.spoolman_id = 42;
    saved.material = "PLA";
    saved.color_rgb = 0xFF5500;
    saved.color_set = true;
    saved.fingerprint = "101001|0FF5500";
    mock_printer.client.mock_db_set("lane_data", "lane1",
                                    helix::ams::to_lane_data_record(0, saved));

    helix::test::RegisteredWired<AmsBackendCfs> session(&api);
    REQUIRE(CfsTestAccess::get_override(*session, 0).has_value());
    REQUIRE(CfsTestAccess::last_rfid_uid(*session, 0) == "101001|0FF5500");

    session->fire_status(
        make_cfs_notification(make_flat_box("PLA", "Polymaker", "PolyLite Orange", "#FF5500")));
    // The write it made re-reads the records through the update queue.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(CfsTestAccess::get_override(*session, 0).has_value());
    CHECK(CfsTestAccess::get_override(*session, 0)->brand == "Polymaker");

    // The record now carries the flat fingerprint, so the next restart
    // compares like with like.
    auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    REQUIRE(stored.contains("helix_fingerprint"));
    CHECK(stored.at("helix_fingerprint") == "PLA|FF5500");

    session->fire_status(
        make_cfs_notification(make_flat_box("PETG", "Bambu", "Basic Green", "#00FF00")));
    // The write it made re-reads the records through the update queue.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(CfsTestAccess::get_override(*session, 0).has_value());
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
    // The last session's poll reads (records and the bypass key) answer
    // through the queue after its backend is gone; settle them here.
    helix::ui::UpdateQueue::instance().drain_until_empty();
}
// Presence: `remain_len` LATCHES after a tagged spool is pulled, so it cannot
// be an unconditional presence signal. It exists only to cover UNTAGGED spools,
// which report no RFID payload at all — see parse_box_status.

TEST_CASE("CFS clears the stale lane_data record when a tagged spool is removed",
          "[ams][cfs][presence][filament_slot_override]") {
    CfsOverrideRig rig("cfs_removal_clears");

    // Slot 3 seated with a tagged spool. The auto-mirror publishes firmware
    // truth to lane4 so OrcaSlicer can see it.
    json box_seated = make_unit_box_explicit(
        {"unknown", "unknown", "101001", "101001"}, {"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        {"unknown", "unknown", "unknown", "unknown"}, {"100", "0", "46", "50"});
    rig.poll(box_seated);

    REQUIRE(rig.backend->get_slot_info(3).status == SlotStatus::AVAILABLE);
    auto seated_record = rig.client.mock_db_get("lane_data", "lane4");
    REQUIRE(!seated_record.is_null());

    // Spool pulled. `vender` drops to the sentinel; everything else latches.
    // The RFID fingerprint is therefore UNCHANGED — check_hardware_event_clear
    // can never fire here, which is exactly why removal needs its own path.
    json box_removed = make_unit_box_explicit(
        {"unknown", "unknown", "101001", "101001"}, {"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        {"unknown", "unknown", "unknown", "none"}, {"100", "0", "46", "50"});
    rig.poll(box_removed);

    SECTION("slot reads EMPTY and is not promoted back by the stale override") {
        CHECK(rig.backend->get_slot_info(3).status == SlotStatus::EMPTY);
    }
    SECTION("the auto-mirrored override is erased") {
        CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 3).has_value());
    }
    SECTION("the lane_data record is deleted, and stays deleted across polls") {
        CHECK(rig.client.mock_db_get("lane_data", "lane4").is_null());
        for (int i = 0; i < 3; ++i) {
            rig.poll(box_removed);
            CHECK(rig.client.mock_db_get("lane_data", "lane4").is_null());
            CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 3).has_value());
        }
    }
    SECTION("neighbouring seated slots keep their records") {
        CHECK_FALSE(rig.client.mock_db_get("lane_data", "lane3").is_null());
    }
}

TEST_CASE("CFS repoll re-lands a linked spool's filament definition id on the lane",
          "[ams][cfs][filament_slot_override][1632]") {
    CfsOverrideRig rig("cfs_filament_id_repaint");

    rig.poll(make_runout_seated_box(/*useup=*/0, /*active=*/"D"));
    link_lane_four_to_spool_137(*rig.backend);
    REQUIRE(rig.backend->get_slot_info(3).spoolman_filament_id == 99);

    // Every poll replaces the parsed units wholesale, wiping every
    // firmware-sourced field off the live slot. The convergence repaint must
    // put the linked spool's filament definition id back beside its spool id.
    for (int i = 0; i < 2; ++i) {
        rig.poll(make_runout_seated_box(/*useup=*/0, /*active=*/"D"));
        const auto info = rig.backend->get_slot_info(3);
        CHECK(info.spoolman_id == 137);
        CHECK(info.spoolman_filament_id == 99);
    }
}

TEST_CASE("CFS empty bay without a runout edge keeps the Spoolman link",
          "[ams][cfs][filament_slot_override][1390]") {
    CfsOverrideRig rig("cfs_no_edge_keeps_link");

    // The transient-unreadable-tag protection from clear_stale_override_on_
    // removal_locked must keep working: an EMPTY poll alone, with no captured
    // runout episode, never strips a link.
    SECTION("no runout observed at all (latch 0 throughout)") {
        rig.poll(make_runout_seated_box(/*useup=*/0, /*active=*/"D"));
        link_lane_four_to_spool_137(*rig.backend);

        for (int i = 0; i < 2; ++i) {
            rig.poll(make_runout_removed_box(/*useup=*/0, /*active=*/"None"));
            CHECK(CfsTestAccess::get_override(*rig.backend, 3)->spoolman_id == 137);
        }
    }

    SECTION("sticky latch with no observed edge (first sighting already 1)") {
        // App started while the latch was already tripped, or the rise was
        // never observed: there is no 0 -> 1 transition, so no episode.
        rig.poll(make_runout_seated_box(/*useup=*/1, /*active=*/"D"));
        link_lane_four_to_spool_137(*rig.backend);
        rig.poll(make_runout_removed_box(/*useup=*/1, /*active=*/"None"));

        CHECK(CfsTestAccess::get_override(*rig.backend, 3)->spoolman_id == 137);
    }
}

TEST_CASE("CFS runout on one lane does not strip another lane's link",
          "[ams][cfs][filament_slot_override][1390]") {
    CfsOverrideRig rig("cfs_wrong_lane_keeps_link");

    // Lane 2 (bay B) is the feeding lane this print.
    rig.poll(make_runout_seated_box(/*useup=*/0, /*active=*/"B"));

    // Lane 4 (bay D) idles with its own link.
    link_lane_four_to_spool_137(*rig.backend);

    // Lane 2 runs out; the edge captures slot 1 (bay B), not slot 3 (bay D).
    rig.poll(make_runout_seated_box(/*useup=*/1, /*active=*/"B"));

    // The user empties lane 4 for an unrelated reason while lane 2's episode
    // is armed: lane 4's link must survive.
    rig.poll(make_runout_removed_box(/*useup=*/1, /*active=*/"B"));

    auto ovr = CfsTestAccess::get_override(*rig.backend, 3);
    REQUIRE(ovr.has_value());
    CHECK(ovr->spoolman_id == 137);
    CHECK(ovr->spoolman_vendor_id == 21);
    CHECK(rig.backend->get_slot_info(3).spoolman_id == 137);
    CHECK(rig.client.mock_db_get("lane_data", "lane4").value("spool_id", 0) == 137);
}

TEST_CASE("CFS runout strip fires once per episode", "[ams][cfs][filament_slot_override][1390]") {
    CfsOverrideRig rig("cfs_runout_strip_one_shot");

    // Full episode: edge while lane 4 feeds, bay D then reads EMPTY.
    rig.poll(make_runout_seated_box(/*useup=*/0, /*active=*/"D"));
    link_lane_four_to_spool_137(*rig.backend);
    rig.poll(make_runout_seated_box(/*useup=*/1, /*active=*/"D"));
    rig.poll(make_runout_removed_box(/*useup=*/1, /*active=*/"None"));
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 3)->spoolman_id == 0);

    // The user links the fresh spool by hand while the sticky latch is still
    // tripped. A later transient EMPTY read of the same bay must not strip
    // the new link: the episode was consumed by the first strip.
    link_lane_four_to_spool_137(*rig.backend);
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 3)->spoolman_id == 137);

    for (int i = 0; i < 2; ++i) {
        rig.poll(make_runout_removed_box(/*useup=*/1, /*active=*/"None"));
        CHECK(CfsTestAccess::get_override(*rig.backend, 3)->spoolman_id == 137);
    }

    // The latch finally clears (fresh spool feeding) and the bay goes empty
    // again - a deliberate unload, not a runout: still no strip.
    rig.poll(make_runout_seated_box(/*useup=*/0, /*active=*/"D"));
    rig.poll(make_runout_removed_box(/*useup=*/0, /*active=*/"None"));
    CHECK(CfsTestAccess::get_override(*rig.backend, 3)->spoolman_id == 137);
}

TEST_CASE("CFS: a labeled untagged spool stays AVAILABLE while it is seated",
          "[ams][cfs][presence][filament_slot_override]") {
    // The other half of the ghost rule. A user-labeled spool must render solid
    // while it is physically in the bay and ghost only once pulled, so presence
    // has to come from a live signal rather than from the assignment.
    //
    // `vender` is that signal: on CFS 1.1.3 it reports occupancy for ANY seated
    // spool, tagged or not. Verified on a K2 Plus carrying only third-party
    // filament — both occupied bays read vender "unknown" with no Creality RFID
    // in the machine, while both empty bays read "none".
    //
    // material_type here is the code our own identity push wrote (#968), not one
    // read off a tag. That suppresses the untagged remain_len fallback, which is
    // exactly the case an override->AVAILABLE promotion would otherwise cover.
    CfsOverrideRig rig("cfs_labeled_untagged_seated");

    json box_seated =
        make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                               {"unknown", "none", "none", "none"}, {"100", "-1", "-1", "-1"});
    rig.poll(box_seated);

    SlotInfo edit;
    edit.material = "ASA-GF";
    edit.brand = "Ambrosia";
    edit.spool_name = "Black ASA-GF";
    edit.color_rgb = 0x000000;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit);
    rig.poll(box_seated);

    SECTION("seated + labeled renders solid, not ghosted") {
        CHECK(rig.backend->get_slot_info(0).status == SlotStatus::AVAILABLE);
        CHECK(rig.backend->get_slot_info(0).material == "ASA-GF");
    }

    SECTION("pulling it ghosts the lane: EMPTY status, identity intact") {
        json box_pulled =
            make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                                   {"none", "none", "none", "none"}, {"100", "-1", "-1", "-1"});
        rig.poll(box_pulled);
        CHECK(rig.backend->get_slot_info(0).status == SlotStatus::EMPTY);
        auto ovr = CfsTestAccess::get_override(*rig.backend, 0);
        REQUIRE(ovr.has_value());
        CHECK(ovr->material == "ASA-GF");
    }
}

TEST_CASE("CFS: labeling an untagged bay does not blank it on vender-sentinel firmware",
          "[ams][cfs][presence][filament_slot_override]") {
    // The sibling of "a labeled untagged spool stays AVAILABLE while it is
    // seated", for the firmware that case does NOT cover.
    //
    // There, `vender` reports occupancy for any seated spool, so presence never
    // consults the untagged remain_len fallback. On firmware where `vender`
    // stays sentinel for an untagged spool — the #1077 population the fallback
    // exists for — that fallback is the ONLY thing keeping the bay visible, and
    // labeling must not switch it off: the label is written to `material_type`
    // (#968), firmware echoes it back, and a non-sentinel material_type reads as
    // tag payload. Naming a spool must not make it vanish.
    CfsOverrideRig rig("cfs_label_untagged_vender_sentinel");

    // Untagged 3rd-party spool in bay A: no vendor, no RFID payload, real
    // length off the measuring wheel. `same_material` supplies a code the
    // firmware itself reported, which is the only vocabulary the identity push
    // is allowed to write back.
    auto box_with_group = [](const std::vector<std::string>& materials) {
        json box =
            make_unit_box_explicit(materials, {"-1", "-1", "-1", "-1"},
                                   {"none", "none", "none", "none"}, {"46", "-1", "-1", "-1"});
        box["same_material"] =
            json::array({json::array({"101001", "0FF0000", json::array({"T1A"}), "ASA-GF"})});
        return box;
    };

    const json box_before = box_with_group({"-1", "-1", "-1", "-1"});
    rig.poll(box_before);

    // Precondition: the fallback is what makes this bay visible at all.
    REQUIRE(rig.backend->get_slot_info(0).status == SlotStatus::AVAILABLE);

    SlotInfo edit;
    edit.material = "ASA-GF";
    edit.spool_name = "Black ASA-GF";
    edit.color_rgb = 0x000000;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit);

    // Firmware now echoes our own code back on every frame, byte-identical to
    // a tag read. Nothing about the physical bay changed.
    const json box_after = box_with_group({"101001", "-1", "-1", "-1"});
    rig.poll(box_after);

    SECTION("the seated spool is still present after being named") {
        CHECK(rig.backend->get_slot_info(0).status == SlotStatus::AVAILABLE);
    }

    SECTION("it stays present across repeated polls of the echo") {
        for (int i = 0; i < 3; ++i) {
            rig.poll(box_after);
            CHECK(rig.backend->get_slot_info(0).status == SlotStatus::AVAILABLE);
        }
    }

    SECTION("the bay still empties when the spool is actually pulled") {
        // remain_len is the only live signal on this firmware, so it is what
        // has to fall. The discount must not pin the bay AVAILABLE.
        json box_pulled = box_with_group({"101001", "-1", "-1", "-1"});
        box_pulled["T1"]["remain_len"] = std::vector<std::string>{"-1", "-1", "-1", "-1"};
        rig.poll(box_pulled);
        CHECK(rig.backend->get_slot_info(0).status == SlotStatus::EMPTY);
    }

    SECTION("neighbouring untouched bays are unaffected") {
        CHECK(rig.backend->get_slot_info(1).status == SlotStatus::EMPTY);
        CHECK(rig.backend->get_slot_info(2).status == SlotStatus::EMPTY);
    }
}

TEST_CASE("CFS: relabeling a TAGGED bay still suppresses the untagged fallback",
          "[ams][cfs][presence][filament_slot_override]") {
    // The guard that keeps the discount above from re-opening the ghost bug.
    //
    // A bay already reporting a real material code is genuinely tagged. Its
    // remain_len LATCHES after the spool is pulled, so the untagged fallback
    // must stay suppressed there — otherwise a removed spool reads AVAILABLE
    // forever. The push therefore records a code as "ours" only when the bay's
    // PRE-push reading was a sentinel, which a tagged bay's never is.
    CfsOverrideRig rig("cfs_relabel_tagged_bay");

    const json box_seated =
        make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                               {"unknown", "none", "none", "none"}, {"100", "-1", "-1", "-1"});
    rig.poll(box_seated);
    REQUIRE(rig.backend->get_slot_info(0).status == SlotStatus::AVAILABLE);

    SlotInfo edit;
    edit.material = "ASA-CF";
    edit.color_name = "Dark Gray";
    edit.color_rgb = 0x1A1A1A;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit);
    rig.poll(box_seated);

    SECTION("still AVAILABLE while seated") {
        CHECK(rig.backend->get_slot_info(0).status == SlotStatus::AVAILABLE);
    }

    SECTION("pulling it reads EMPTY, not a latched-length ghost") {
        json box_pulled =
            make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                                   {"none", "none", "none", "none"}, {"100", "-1", "-1", "-1"});
        rig.poll(box_pulled);
        CHECK(rig.backend->get_slot_info(0).status == SlotStatus::EMPTY);
    }
}

TEST_CASE("CFS probes RFID on a bay insert, without feeding filament",
          "[ams][cfs][presence][probe_on_insert]") {
    // Firmware reports occupancy the instant a spool goes in but does not read
    // its tag. Verified on a K2 Plus: a genuine Creality RFID spool inserted
    // into an empty bay sat at vender "unknown" / material_type "unknown" /
    // remain_len -1 indefinitely, so a stale override kept painting the lane
    // until BOX_GET_RFID was sent by hand.
    helix::test::StartedBackend<CfsRemapHelper> backend;
    // An insert the reader cannot name queues the same-spool notice; drain
    // each poll so the queue empties with the test.
    auto poll = [&backend](const json& box) {
        backend.fire_status(make_cfs_notification(box));
        helix::ui::UpdateQueue::instance().drain();
    };

    // Bay A seated, B/C/D empty.
    const json empty_bcd =
        make_unit_box_explicit({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"},
                               {"unknown", "none", "none", "none"}, {"100", "-1", "-1", "-1"});

    SECTION("first sighting seeds occupancy without probing") {
        poll(empty_bcd);
        CHECK(backend.captured.empty());
    }

    SECTION("steady state does not probe") {
        poll(empty_bcd);
        poll(empty_bcd);
        CHECK(backend.captured.empty());
    }

    SECTION("empty -> occupied with no tag probes that bay only") {
        poll(empty_bcd);
        backend.captured.clear();
        // Spool into bay B: occupancy appears, tag does not.
        const json b_in = make_unit_box_explicit(
            {"101001", "unknown", "-1", "-1"}, {"0FFFFFF", "0000000", "-1", "-1"},
            {"unknown", "unknown", "none", "none"}, {"100", "-1", "-1", "-1"});
        poll(b_in);
        REQUIRE(backend.captured.size() == 1);
        CHECK(backend.captured[0] == "BOX_INFO_REFRESH ADDR=1 NUM=2");
    }

    SECTION("sends the full refresh — every step of it is load bearing") {
        poll(empty_bcd);
        backend.captured.clear();
        const json b_in = make_unit_box_explicit(
            {"101001", "unknown", "-1", "-1"}, {"0FFFFFF", "0000000", "-1", "-1"},
            {"unknown", "unknown", "none", "none"}, {"100", "-1", "-1", "-1"});
        poll(b_in);
        REQUIRE(backend.captured.size() == 1);
        // BOX_GET_RFID on its own is inert: measured on a K2 Plus, the tag only
        // resolves once BOX_SET_PRE_LOADING has run, and remain_len reports the
        // "never measured" default of 100 until the same motion happens.
        CHECK(backend.captured[0].find("BOX_INFO_REFRESH") != std::string::npos);
    }

    SECTION("a latched material_type does NOT suppress the probe") {
        // Regression: bay C on the K2 rig reported material_type 101001 latched
        // from a spool pulled days earlier. Treating a non-sentinel code as
        // "firmware already knows this bay" skipped the probe on exactly the
        // bays carrying stale identity — the case this feature exists for.
        poll(empty_bcd);
        backend.captured.clear();
        const json c_in_latched = make_unit_box_explicit(
            {"101001", "-1", "101001", "-1"}, {"0FFFFFF", "-1", "0000000", "-1"},
            {"unknown", "none", "unknown", "none"}, {"100", "-1", "-1", "-1"});
        poll(c_in_latched);
        REQUIRE(backend.captured.size() == 1);
        CHECK(backend.captured[0] == "BOX_INFO_REFRESH ADDR=1 NUM=4");
    }

    SECTION("two bays inserted together share one command pair") {
        poll(empty_bcd);
        backend.captured.clear();
        const json bc_in = make_unit_box_explicit(
            {"101001", "unknown", "unknown", "-1"}, {"0FFFFFF", "0000000", "0000000", "-1"},
            {"unknown", "unknown", "unknown", "none"}, {"100", "-1", "-1", "-1"});
        poll(bc_in);
        REQUIRE(backend.captured.size() == 1);
        // B = 0b0010, C = 0b0100 -> NUM=6
        CHECK(backend.captured[0] == "BOX_INFO_REFRESH ADDR=1 NUM=6");
    }

    SECTION("removal re-arms the probe for the next insert") {
        poll(empty_bcd);
        const json b_in = make_unit_box_explicit(
            {"101001", "unknown", "-1", "-1"}, {"0FFFFFF", "0000000", "-1", "-1"},
            {"unknown", "unknown", "none", "none"}, {"100", "-1", "-1", "-1"});
        poll(b_in);
        poll(empty_bcd); // pulled back out
        backend.captured.clear();
        poll(b_in); // and back in
        REQUIRE(backend.captured.size() == 1);
        CHECK(backend.captured[0] == "BOX_INFO_REFRESH ADDR=1 NUM=2");
    }
}
