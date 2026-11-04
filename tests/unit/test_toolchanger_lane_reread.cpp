// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_lane_reread.cpp
 * @brief The Creator 5's lane_data re-read (prestonbrown/helixscreen#1632).
 *
 * Pins what schedules a re-read (a changed zmod_color head, an afclite lane's
 * new spool_id, on the C5 row only), which records it files (only those
 * another tool changed since the last read, never one carrying HelixScreen's
 * own keys), and what the head's override keeps afterwards (a rebinding
 * record starts it afresh, any other record amends it).
 */

#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/seeded_override.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_toolchanger.h"
#include "core/update_queue.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "test_helpers/registered_backend.h"
#include "toolchanger_addon.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// The C5 with Z-Mod's afclite shim and a lane per head.
PrinterDiscovery zmod_c5_afclite_discovery() {
    json objects = json::array({"zmod", "zmod_color", "save_variables", "zmod_afclite", "AFC",
                                "AFC_unit C5", "extruder", "extruder1", "extruder2", "extruder3",
                                "heater_bed", "toolhead", "gcode_move", "configfile"});
    for (int i = 0; i < 4; ++i) {
        objects.push_back("AFC_lane E" + std::to_string(i));
    }
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// A zmod_color frame whose second head holds @p head1_hex and third head
/// @p head2_hex.
json zmod_color_frame(const char* head1_hex, const char* head2_hex = "8C8C89") {
    return json{
        {"zmod_color",
         {{"active_tool_id", -1},
          {"valid_types", json::array({"PLA", "PETG", "?"})},
          {"palette", json::array({"48358C", "8C8C89", "D584DE", "FFF245"})},
          {"slots", json::array({{{"ID", "1"}, {"Material", "PLA"}, {"HEX", "48358C"}},
                                 {{"ID", "2"}, {"Material", "PLA"}, {"HEX", head1_hex}},
                                 {{"ID", "3"}, {"Material", "PLA"}, {"HEX", head2_hex}},
                                 {{"ID", "4"}, {"Material", "PLA"}, {"HEX", "FFF245"}}})}}}};
}

void feed(AmsBackendToolChanger& b, const json& status) {
    ToolChangerTestAccess::handle_status(
        b, json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
}

/// What a tag reader leaves in lane_data for a head: the namespace's shared
/// spellings only, stamped with when the spool was set.
json outside_tool_record(int head, const std::string& color, const std::string& name,
                         const std::string& vendor, int spool_id, const std::string& scan_time) {
    return json{{"lane", std::to_string(head)},
                {"color", color},
                {"material", "PLA"},
                {"name", name},
                {"vendor_name", vendor},
                {"spool_id", spool_id},
                {"scan_time", scan_time}};
}

/// Store-backed backend, so on_started() builds the lane_data store and the
/// re-read has something to read.
class StoreBackedC5 : public AmsBackendToolChanger {
  public:
    StoreBackedC5(IMoonrakerAPI* api, int tool_count) : AmsBackendToolChanger(api, nullptr) {
        std::vector<std::string> names;
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
        running_ = true;
        const auto hw = zmod_c5_afclite_discovery();
        set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
        set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
        set_material_source(toolchanger_addon::resolve_material_source(hw));
    }

    ~StoreBackedC5() override {
        ui::UpdateQueue::instance().drain();
    }

    AmsError execute_gcode(const std::string&) override {
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string&, helix::SharedCallback<void()>) override {
        return AmsErrorHelper::success();
    }
};

} // namespace

TEST_CASE("A Creator 5 head a tag reader rebinds follows the reader's lane_data record",
          "[toolchanger][zmod][lane_data][1632]") {
    ams::reset_lane_sources();
    ConfigDirGuard tmp("toolchanger_reread_rebind");
    MockPrinter mock_printer;
    mock_printer.client.mock_db_set("lane_data", "T1",
                                    outside_tool_record(1, "#8C9099", "Panchroma PLA Grey",
                                                        "Panchroma", 24, "2026-10-20T23:00:00Z"));

    test::RegisteredBackend<StoreBackedC5> reg(&mock_printer.api, 4);
    StoreBackedC5& c5 = *reg;
    ToolChangerTestAccess::call_on_started(c5);
    feed(c5, zmod_color_frame("8C8C89"));
    REQUIRE(c5.get_slot_info(1).spoolman_id == 24);

    // The reader writes the head's record, then the firmware's colour changes.
    mock_printer.client.mock_db_set("lane_data", "T1",
                                    outside_tool_record(1, "#E364CD", "Sunlu PLA Magenta", "Sunlu",
                                                        21, "2026-10-20T23:38:21Z"));
    feed(c5, zmod_color_frame("D584DE"));
    REQUIRE(ToolChangerTestAccess::lane_reread_pending(c5));

    ToolChangerTestAccess::fire_lane_record_reread(c5);
    ui::UpdateQueue::instance().drain();

    const SlotInfo after = c5.get_slot_info(1);
    CHECK(after.spoolman_id == 21);
    CHECK(after.spool_name == "Sunlu PLA Magenta");
    CHECK(after.brand == "Sunlu");
    // The other heads are untouched.
    CHECK(c5.get_slot_info(0).spoolman_id == 0);
}

TEST_CASE("A Creator 5 record carrying HelixScreen's own keys is not taken as an outside edit",
          "[toolchanger][zmod][lane_data][1632]") {
    ams::reset_lane_sources();
    ConfigDirGuard tmp("toolchanger_reread_own_keys");
    MockPrinter mock_printer;
    mock_printer.client.mock_db_set("lane_data", "T1",
                                    outside_tool_record(1, "#8C9099", "Panchroma PLA Grey",
                                                        "Panchroma", 24, "2026-10-20T23:00:00Z"));

    test::RegisteredBackend<StoreBackedC5> reg(&mock_printer.api, 4);
    StoreBackedC5& c5 = *reg;
    ToolChangerTestAccess::call_on_started(c5);
    feed(c5, zmod_color_frame("8C8C89"));

    // "vendor" is HelixScreen's legacy spelling: a record carrying it is ours.
    auto ours =
        outside_tool_record(1, "#E364CD", "Sunlu PLA Magenta", "Sunlu", 21, "2026-10-20T23:38:21Z");
    ours["vendor"] = "Sunlu";
    mock_printer.client.mock_db_set("lane_data", "T1", ours);
    feed(c5, zmod_color_frame("D584DE"));
    ToolChangerTestAccess::fire_lane_record_reread(c5);
    ui::UpdateQueue::instance().drain();

    CHECK(c5.get_slot_info(1).spoolman_id == 24);
}

TEST_CASE("A Creator 5 re-read leaves a head whose record it already read alone",
          "[toolchanger][zmod][lane_data][1632]") {
    // Head 1's record is the one it booted from. Spoolman has since described
    // the spool that record links; a tag scanned into head 2 must not file
    // head 1's old record again over what Spoolman said.
    ams::reset_lane_sources();
    ConfigDirGuard tmp("toolchanger_reread_unchanged");
    MockPrinter mock_printer;
    mock_printer.client.mock_db_set("lane_data", "T1",
                                    outside_tool_record(1, "#8C9099", "Panchroma PLA Grey",
                                                        "Panchroma", 24, "2026-10-20T23:00:00Z"));

    test::RegisteredBackend<StoreBackedC5> reg(&mock_printer.api, 4);
    StoreBackedC5& c5 = *reg;
    ToolChangerTestAccess::call_on_started(c5);
    feed(c5, zmod_color_frame("8C8C89"));

    SpoolInfo spool;
    spool.id = 24;
    spool.vendor = "Panchroma";
    spool.material = "PLA";
    spool.filament_name = "Panchroma Matte Charcoal";
    spool.color_hex = "#333333";
    test::spool_states(c5, 1, spool);

    mock_printer.client.mock_db_set("lane_data", "T2",
                                    outside_tool_record(2, "#E364CD", "Sunlu PLA Magenta", "Sunlu",
                                                        21, "2026-10-20T23:38:21Z"));
    feed(c5, zmod_color_frame("8C8C89", "D584DE"));
    ToolChangerTestAccess::fire_lane_record_reread(c5);
    ui::UpdateQueue::instance().drain();

    REQUIRE(c5.get_slot_info(2).spoolman_id == 21);
    const SlotInfo head1 = c5.get_slot_info(1);
    CHECK(head1.color_rgb == 0x333333u);
    CHECK(head1.spool_name == "Panchroma Matte Charcoal");
}

TEST_CASE("A Creator 5 re-read that rebinds a head starts the override afresh",
          "[toolchanger][zmod][lane_data][1632]") {
    // The old spool's record named its colour; the new spool's does not. A
    // rebinding record replaces the override, so the old spool's colour name
    // is not saved back over the new spool's.
    ams::reset_lane_sources();
    ConfigDirGuard tmp("toolchanger_reread_rebind_override");
    MockPrinter mock_printer;
    auto old_spool = outside_tool_record(1, "#8C9099", "Panchroma PLA Grey", "Panchroma", 24,
                                         "2026-10-20T23:00:00Z");
    old_spool["color_name"] = "Ash Grey";
    mock_printer.client.mock_db_set("lane_data", "T1", old_spool);

    test::RegisteredBackend<StoreBackedC5> reg(&mock_printer.api, 4);
    StoreBackedC5& c5 = *reg;
    ToolChangerTestAccess::call_on_started(c5);
    feed(c5, zmod_color_frame("8C8C89"));
    REQUIRE(ToolChangerTestAccess::get_override(c5, 1).has_value());
    REQUIRE(ToolChangerTestAccess::get_override(c5, 1)->color_name == "Ash Grey");

    mock_printer.client.mock_db_set("lane_data", "T1",
                                    outside_tool_record(1, "#E364CD", "Sunlu PLA Magenta", "Sunlu",
                                                        21, "2026-10-20T23:38:21Z"));
    feed(c5, zmod_color_frame("D584DE"));
    ToolChangerTestAccess::fire_lane_record_reread(c5);
    ui::UpdateQueue::instance().drain();

    const auto ovr = ToolChangerTestAccess::get_override(c5, 1);
    REQUIRE(ovr.has_value());
    CHECK(ovr->spoolman_id == 21);
    CHECK(ovr->spool_name == "Sunlu PLA Magenta");
    CHECK(ovr->color_name.empty());
}

TEST_CASE("A Creator 5 re-read that keeps a head's spool amends the override",
          "[toolchanger][zmod][lane_data][1632]") {
    // Same spool, renamed: what the new record leaves out still describes the
    // spool, so the override keeps it.
    ams::reset_lane_sources();
    ConfigDirGuard tmp("toolchanger_reread_amend_override");
    MockPrinter mock_printer;
    auto first = outside_tool_record(1, "#8C9099", "Panchroma PLA Grey", "Panchroma", 24,
                                     "2026-10-20T23:00:00Z");
    first["color_name"] = "Ash Grey";
    mock_printer.client.mock_db_set("lane_data", "T1", first);

    test::RegisteredBackend<StoreBackedC5> reg(&mock_printer.api, 4);
    StoreBackedC5& c5 = *reg;
    ToolChangerTestAccess::call_on_started(c5);
    feed(c5, zmod_color_frame("8C8C89"));
    REQUIRE(ToolChangerTestAccess::get_override(c5, 1)->color_name == "Ash Grey");

    mock_printer.client.mock_db_set("lane_data", "T1",
                                    outside_tool_record(1, "#E364CD", "Panchroma PLA Grey Matte",
                                                        "Panchroma", 24, "2026-10-20T23:38:21Z"));
    feed(c5, zmod_color_frame("D584DE"));
    ToolChangerTestAccess::fire_lane_record_reread(c5);
    ui::UpdateQueue::instance().drain();

    const auto ovr = ToolChangerTestAccess::get_override(c5, 1);
    REQUIRE(ovr.has_value());
    CHECK(ovr->spoolman_id == 24);
    CHECK(ovr->spool_name == "Panchroma PLA Grey Matte");
    CHECK(ovr->color_name == "Ash Grey");
}

TEST_CASE("An afclite lane's new spool schedules a lane_data re-read",
          "[toolchanger][zmod][afclite][lane_data]") {
    // A new spool that snaps to the old one's palette colour restates nothing
    // zmod_color shows; the lane's spool_id is what changes.
    test::ToolChangerHelper tc(4);
    tc.set_material_source(toolchanger_addon::resolve_material_source(zmod_c5_afclite_discovery()));
    feed(tc, json{{"AFC_lane E1", {{"spool_id", 24}}}});
    CHECK_FALSE(ToolChangerTestAccess::lane_reread_pending(tc));

    feed(tc, json{{"AFC_lane E1", {{"spool_id", 24}}}});
    CHECK_FALSE(ToolChangerTestAccess::lane_reread_pending(tc));

    feed(tc, json{{"AFC_lane E1", {{"spool_id", 21}}}});
    CHECK(ToolChangerTestAccess::lane_reread_pending(tc));
}

TEST_CASE("A tool changer off the Creator 5 row re-reads nothing for an AFC lane's new spool",
          "[toolchanger][afclite][lane_data]") {
    // Real AFC on a tool changer publishes AFC_lane objects too; only the C5
    // row's heads have lane_data records an outside tool rewrites.
    test::ToolChangerHelper tc(4);
    feed(tc, json{{"AFC_lane E1", {{"spool_id", 24}}}});
    feed(tc, json{{"AFC_lane E1", {{"spool_id", 21}}}});
    CHECK_FALSE(ToolChangerTestAccess::lane_reread_pending(tc));
}

TEST_CASE("Only a changed head colour or material schedules a lane_data re-read",
          "[toolchanger][zmod][material][lane_data]") {
    ams::reset_lane_sources();
    test::RegisteredBackend<test::ToolChangerHelper> reg(4);
    test::ToolChangerHelper& tc = *reg;
    tc.set_material_source(toolchanger_addon::resolve_material_source(zmod_c5_afclite_discovery()));

    // The first frame is the boot state, and a frame restating it is no change.
    tc.feed(zmod_color_frame("8C8C89"));
    CHECK_FALSE(ToolChangerTestAccess::lane_reread_pending(tc));
    tc.feed(zmod_color_frame("8C8C89"));
    CHECK_FALSE(ToolChangerTestAccess::lane_reread_pending(tc));

    tc.feed(zmod_color_frame("D584DE"));
    CHECK(ToolChangerTestAccess::lane_reread_pending(tc));
}

TEST_CASE("The Creator 5 subscribes each afclite lane", "[toolchanger][zmod][afclite]") {
    const auto objects = toolchanger_addon::required_status_objects(zmod_c5_afclite_discovery());
    for (const char* lane : {"AFC_lane E0", "AFC_lane E1", "AFC_lane E2", "AFC_lane E3"}) {
        CHECK(std::find(objects.begin(), objects.end(), lane) != objects.end());
    }
}

TEST_CASE("afclite lane spools are read from their AFC_lane objects",
          "[toolchanger][zmod][afclite]") {
    const auto spools =
        toolchanger_addon::read_afc_lane_spools(json{{"AFC_lane E0", {{"spool_id", 19}}},
                                                     {"AFC_lane E3", {{"spool_id", nullptr}}},
                                                     {"AFC_lane E1", {{"map", "T1"}}},
                                                     {"zmod_color", {{"active_tool_id", 0}}}});
    REQUIRE(spools.has_value());
    REQUIRE(spools->size() == 2);
    for (const auto& spool : *spools) {
        if (spool.lane == "E0") {
            CHECK(spool.spool_id == 19);
        } else {
            CHECK(spool.lane == "E3");
            CHECK(spool.spool_id == 0); // null: the lane has no spool
        }
    }
    // A frame naming no lane's spool_id is not a clearing.
    CHECK_FALSE(toolchanger_addon::read_afc_lane_spools(json{{"AFC_lane E1", {{"map", "T1"}}}})
                    .has_value());
}
