// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Creator 5's heads are rewritten in lane_data by other tools (a tag
// reader, Z-Mod's afclite): a changed head colour or material, or an afclite
// lane's new spool, reads the lane records again (prestonbrown/helixscreen#1632).

#include "ui_update_queue.h"

#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "lane_source_store.h"
#include "printer_discovery.h"
#include "settings_manager.h"
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

json zmod_color_frame(const char* head1_hex) {
    return json{
        {"zmod_color",
         {{"active_tool_id", -1},
          {"valid_types", json::array({"PLA", "PETG", "?"})},
          {"palette", json::array({"48358C", "8C8C89", "D584DE", "FFF245"})},
          {"slots", json::array({{{"ID", "1"}, {"Material", "PLA"}, {"HEX", "48358C"}},
                                 {{"ID", "2"}, {"Material", "PLA"}, {"HEX", head1_hex}},
                                 {{"ID", "3"}, {"Material", "PLA"}, {"HEX", "8C8C89"}},
                                 {{"ID", "4"}, {"Material", "PLA"}, {"HEX", "FFF245"}}})}}}};
}

/// A store-backed changer, so a scheduled refresh reaches the database.
class StoreBackedChanger : public AmsBackendToolChanger {
  public:
    StoreBackedChanger(IMoonrakerAPI* api, bool creator5) : AmsBackendToolChanger(api, nullptr) {
        set_discovered_tools({"T0", "T1", "T2", "T3"});
        running_ = true;
        if (creator5) {
            const auto hw = zmod_c5_afclite_discovery();
            set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
            set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
            set_material_source(toolchanger_addon::resolve_material_source(hw));
        }
    }
    ~StoreBackedChanger() override {
        ui::UpdateQueue::instance().drain();
    }
    AmsError execute_gcode(const std::string&) override {
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string&, helix::SharedCallback<void()>) override {
        return AmsErrorHelper::success();
    }
};

struct Rig {
    MockPrinter printer;
    test::RegisteredBackend<StoreBackedChanger> reg;
    explicit Rig(bool creator5) : reg(&printer.api, creator5) {
        ams::reset_lane_sources();
        ToolChangerTestAccess::call_on_started(*reg);
        settle();
    }
    static void settle() {
        for (int i = 0; i < 4; ++i) {
            ui::UpdateQueue::instance().drain();
        }
    }
    void feed(const json& status) {
        ToolChangerTestAccess::handle_status(
            *reg, json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
        settle();
    }
    [[nodiscard]] int reads() const {
        return printer.client.call_count("server.database.get_item");
    }
};

} // namespace

TEST_CASE("only a changed Creator 5 head colour reads the lane records again",
          "[toolchanger][zmod][refresh][1632]") {
    Rig rig(/*creator5=*/true);
    rig.feed(zmod_color_frame("8C8C89"));
    const int booted = rig.reads();
    rig.feed(zmod_color_frame("8C8C89"));
    CHECK(rig.reads() == booted);
    // The change reads the records; the record's copy of the new colour is a
    // write of ours, which reads them once more.
    rig.feed(zmod_color_frame("D584DE"));
    CHECK(rig.reads() > booted);
}

TEST_CASE("an afclite lane's new spool reads the lane records again, on the Creator 5 only",
          "[toolchanger][zmod][afclite][refresh][1632]") {
    const bool creator5 = GENERATE(true, false);
    Rig rig(creator5);
    rig.feed(json{{"AFC_lane E1", {{"spool_id", 24}}}});
    const int seen = rig.reads();
    rig.feed(json{{"AFC_lane E1", {{"spool_id", 24}}}});
    CHECK(rig.reads() == seen);
    rig.feed(json{{"AFC_lane E1", {{"spool_id", 21}}}});
    CHECK(rig.reads() == seen + (creator5 ? 1 : 0));
}

TEST_CASE("a lane record a tag reader rewrites shows on the Creator 5 head",
          "[toolchanger][zmod][refresh][1632]") {
    Rig rig(/*creator5=*/true);
    rig.feed(zmod_color_frame("8C8C89"));
    rig.printer.client.mock_db_set("lane_data", "T1",
                                   json{{"lane", "1"},
                                        {"color", "#E364CD"},
                                        {"material", "PLA"},
                                        {"name", "Sunlu PLA Magenta"},
                                        {"vendor_name", "Sunlu"},
                                        {"spool_id", 21}});
    rig.feed(zmod_color_frame("D584DE"));
    const SlotInfo after = rig.reg->get_slot_info(1);
    CHECK(after.spoolman_id == 21);
    CHECK(after.brand == "Sunlu");
    CHECK(rig.reg->get_slot_info(0).spoolman_id == 0);
}

TEST_CASE("the Creator 5 subscribes and reads each afclite lane's spool",
          "[toolchanger][zmod][afclite]") {
    const auto objects = toolchanger_addon::required_status_objects(zmod_c5_afclite_discovery());
    for (const char* lane : {"AFC_lane E0", "AFC_lane E1", "AFC_lane E2", "AFC_lane E3"}) {
        CHECK(std::find(objects.begin(), objects.end(), lane) != objects.end());
    }
    const auto spools =
        toolchanger_addon::read_afc_lane_spools(json{{"AFC_lane E0", {{"spool_id", 19}}},
                                                     {"AFC_lane E3", {{"spool_id", nullptr}}},
                                                     {"AFC_lane E1", {{"map", "T1"}}},
                                                     {"zmod_color", {{"active_tool_id", 0}}}});
    REQUIRE(spools.has_value());
    REQUIRE(spools->size() == 2);
    for (const auto& spool : *spools) {
        CHECK(spool.spool_id == (spool.lane == "E0" ? 19 : 0));
    }
    CHECK_FALSE(toolchanger_addon::read_afc_lane_spools(json{{"AFC_lane E1", {{"map", "T1"}}}})
                    .has_value());
}

TEST_CASE("Z-Mod lane truth: the record copies colour and material from the firmware",
          "[toolchanger][zmod][lane_truth]") {
    Rig rig(/*creator5=*/true);
    rig.feed(zmod_color_frame("8C8C89"));
    rig.reg->refresh_lane_records();
    Rig::settle();
    json rec = rig.printer.client.mock_db_get("lane_data", "T1");
    REQUIRE(rec.is_object());
    CHECK(rec["color"] == "#8C8C89");
    CHECK(rec["material"] == "PLA");

    // The edit's colour goes to the firmware, snapped to its palette; the
    // record takes the brand and keeps the firmware's colour until the echo.
    SlotInfo edit = rig.reg->get_slot_info(1);
    edit.brand = "Polymaker";
    edit.color_rgb = 0xD080E0;
    REQUIRE(rig.reg->commit_user_edit(1, rig.reg->get_slot_info(1), edit).success());
    Rig::settle();
    rec = rig.printer.client.mock_db_get("lane_data", "T1");
    CHECK(rec["vendor_name"] == "Polymaker");
    CHECK(rec["color"] == "#8C8C89");
    const auto pending = ams::lane_sources(rig.reg.lane(1)).pending;
    REQUIRE(pending.has_value());
    CHECK(pending->color_rgb == std::optional<uint32_t>(0xD584DEu));

    rig.feed(zmod_color_frame("D584DE"));
    rec = rig.printer.client.mock_db_get("lane_data", "T1");
    CHECK(rec["color"] == "#D584DE");
    CHECK(rec["vendor_name"] == "Polymaker");
    CHECK_FALSE(ams::lane_sources(rig.reg.lane(1)).pending.has_value());
    CHECK(rig.reg->get_slot_info(1).color_rgb == 0xD584DEu);
}

namespace {

/// A C5 frame whose head 0 reports @p has_filament.
json head0_filament_frame(bool has_filament) {
    json frame = zmod_color_frame("8C8C89");
    frame["zmod_color"]["slots"][0]["hasFilament"] = has_filament;
    return frame;
}

} // namespace

TEST_CASE("an emptied tool keeps or clears its record as Keep Spool Info on Eject says",
          "[toolchanger][zmod][keep_spool][lane_truth]") {
    const bool keep = GENERATE(true, false);
    auto& settings = SettingsManager::instance();
    const bool previous = settings.get_ams_keep_spool_info_on_eject();
    settings.set_ams_keep_spool_info_on_eject(keep);

    MockPrinter printer;
    printer.client.mock_db_set("lane_data", "T0",
                               json{{"lane", "0"},
                                    {"material", "PLA"},
                                    {"color", "#48358C"},
                                    {"vendor_name", "Polymaker"},
                                    {"spool_id", 42},
                                    {"loaded", true}});
    test::RegisteredBackend<StoreBackedChanger> reg(&printer.api, /*creator5=*/true);
    ams::reset_lane_sources();
    ToolChangerTestAccess::call_on_started(*reg);
    Rig::settle();
    auto feed = [&](const json& status) {
        ToolChangerTestAccess::handle_status(
            *reg, json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
        Rig::settle();
    };
    feed(head0_filament_frame(true));
    AmsState::instance().sync_from_backend();
    Rig::settle();
    REQUIRE(reg->get_slot_info(0).spoolman_id == 42);

    feed(head0_filament_frame(false));
    AmsState::instance().sync_from_backend();
    Rig::settle();

    const json rec = printer.client.mock_db_get("lane_data", "T0");
    REQUIRE(rec.is_object());
    CHECK(rec["loaded"] == false);
    if (keep) {
        CHECK(rec["spool_id"] == 42);
        CHECK(rec["vendor_name"] == "Polymaker");
        CHECK(reg->get_slot_info(0).spoolman_id == 42);
    } else {
        CHECK(rec["spool_id"].is_null());
        CHECK(rec["vendor_name"].is_null());
        CHECK(reg->get_slot_info(0).spoolman_id == 0);
    }
    settings.set_ams_keep_spool_info_on_eject(previous);
}
