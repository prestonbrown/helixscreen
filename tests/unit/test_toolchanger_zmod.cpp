// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/toolchanger_test_helper.h"
#include "lane_source_store.h"
#include "printer_discovery.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/toolchanger_test_access.h"
#include "toolchanger_addon.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::test::ToolChangerHelper;

namespace {

/// Objects a Creator 5 Pro on Z-Mod reports (ghzserg/z_c5pro 1.7: c5pro_generic.cfg,
/// zmod_color.py lookups, stock heaters).
PrinterDiscovery zmod_c5_discovery() {
    json objects = json::array({"zmod", "zmod_color", "save_variables", "extruder", "extruder1",
                                "extruder2", "extruder3", "heater_bed", "toolhead", "gcode_move",
                                "configfile", "gcode_macro OPEN_DOOR"});
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
    }
    for (int i = 0; i < 4; ++i) {
        objects.push_back("filament_switch_sensor fd_ex" + std::to_string(i));
        objects.push_back("filament_motion_sensor fm_ex" + std::to_string(i));
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// An AD5X on Z-Mod: zmod_color is there too, the carriage grab buttons are not.
PrinterDiscovery zmod_ad5x_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"zmod", "zmod_color", "zmod_ifs", "save_variables", "extruder",
                                  "heater_bed", "toolhead", "gcode_move", "configfile"}));
    return hw;
}

/// The backend holds a mutex, so the helper is built in place and wired here.
void wire_zmod(ToolChangerHelper& tc) {
    auto hw = zmod_c5_discovery();
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
}

} // namespace

TEST_CASE("A Creator 5 Pro on Z-Mod is claimed by the Z-Mod row", "[toolchanger][zmod]") {
    auto hw = zmod_c5_discovery();
    REQUIRE(toolchanger_addon::present(hw));
    CHECK(toolchanger_addon::machine_name(hw) == "Creator 5 Pro");
    CHECK(toolchanger_addon::required_status_objects(hw) == std::vector<std::string>{"zmod_color"});
    CHECK_FALSE(hw.has_tool_changer());
}

TEST_CASE("An AD5X on Z-Mod is not a tool changer", "[toolchanger][zmod]") {
    CHECK_FALSE(toolchanger_addon::present(zmod_ad5x_discovery()));
}

TEST_CASE("Z-Mod swaps with its own commands", "[toolchanger][zmod][commands]") {
    auto cmds = toolchanger_addon::resolve_tool_commands(zmod_c5_discovery());
    REQUIRE(cmds.present);
    CHECK(cmds.select_prefix == "_T_IN T=");
    CHECK(cmds.unselect == "_T_OUT");
}

TEST_CASE("Mounting on Z-Mod sends _T_IN", "[toolchanger][zmod][commands]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
    REQUIRE(tc.change_tool(2).success());
    CHECK(tc.sent().back() == "_T_IN T=2");
}

TEST_CASE("Parking on Z-Mod sends _T_OUT", "[toolchanger][zmod][commands]") {
    // Separate from mounting: the helper never acks gcode, so a mount stays in
    // flight and a following unmount would be refused as busy.
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 2}}}});
    REQUIRE(tc.unload_filament(2).success());
    CHECK(tc.sent().back() == "_T_OUT");
}

TEST_CASE("zmod_color.active_tool_id is the carriage reading", "[toolchanger][zmod]") {
    auto mounted = toolchanger_addon::read_tool(json{{"zmod_color", {{"active_tool_id", 2}}}});
    REQUIRE(mounted.has_value());
    CHECK(mounted->current_tool == 2);
    CHECK_FALSE(mounted->sensor_error);

    auto empty = toolchanger_addon::read_tool(json{{"zmod_color", {{"active_tool_id", -1}}}});
    REQUIRE(empty.has_value());
    CHECK(empty->current_tool == -1);
    CHECK_FALSE(empty->sensor_error);

    auto conflict = toolchanger_addon::read_tool(json{{"zmod_color", {{"active_tool_id", -2}}}});
    REQUIRE(conflict.has_value());
    CHECK(conflict->sensor_error);
}

TEST_CASE("A zmod_color frame without active_tool_id is no carriage news", "[toolchanger][zmod]") {
    CHECK_FALSE(
        toolchanger_addon::read_tool(json{{"zmod_color", {{"slots", json::array()}}}}).has_value());
}

TEST_CASE("The backend follows the Z-Mod carriage", "[toolchanger][zmod]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    CHECK(tc.get_current_tool() == 1);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
    CHECK(tc.get_current_tool() == -1);
    // At rest, disagreeing buttons are a fault the user has to see.
    tc.feed(json{{"zmod_color", {{"active_tool_id", -2}}}});
    CHECK(tc.get_system_info().action == AmsAction::ERROR);
}

TEST_CASE("A dock sensor fault clears when the sensors agree again", "[toolchanger][zmod]") {
    // Z-Mod publishes no phase word and no toolchanger status, so nothing else
    // would ever move the unit out of the error the fault raised.
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -2}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    CHECK(tc.get_system_info().action == AmsAction::IDLE);
    CHECK(tc.get_current_tool() == 1);
}

TEST_CASE("Z-Mod has no feeder and offers no feeder macros", "[toolchanger][zmod][feeder]") {
    auto hw = zmod_c5_discovery();
    CHECK_FALSE(toolchanger_addon::resolve_feeder(hw).present);
    CHECK(toolchanger_addon::feeder_macro_candidates(hw).empty());
}

namespace {

json full_zmod_color_frame() {
    json palette = json::array();
    for (const char* hex : {"FFFFFF", "FEF043", "0ACC38", "F72224", "161616"}) {
        palette.push_back(hex);
    }
    return json{
        {"zmod_color",
         {{"active_tool_id", -1},
          {"valid_types", json::array({"PLA", "PETG", "ABS", "?"})},
          {"palette", palette},
          {"slots", json::array({{{"ID", "1"}, {"Material", "PLA"}, {"HEX", "FFFFFF"}},
                                 {{"ID", "2"}, {"Material", "PETG"}, {"HEX", "0ACC38"}},
                                 {{"ID", "3"}, {"Material", "?"}, {"HEX", ""}},
                                 {{"ID", "4"}, {"Material", "ABS"}, {"HEX", "161616"}}})}}}};
}

void wire_material_source(ToolChangerHelper& tc) {
    tc.set_material_source(toolchanger_addon::resolve_material_source(zmod_c5_discovery()));
}

} // namespace

TEST_CASE("Z-Mod publishes each head's material and colour", "[toolchanger][zmod][material]") {
    helix::ams::reset_lane_sources();
    // Registered, because lane_id() answers INVALID_LANE_ID for a backend
    // AmsState does not know, and the lane funnels drop what that id names.
    helix::test::RegisteredBackend<ToolChangerHelper> tc_reg(4);
    ToolChangerHelper& tc = *tc_reg;
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    auto vc = helix::ams::lane_sources(tc.lane_id(1)).vendor_cache;
    REQUIRE(vc.has_value());
    CHECK(vc->material == std::optional<std::string>("PETG"));
    CHECK(vc->color_rgb == std::optional<uint32_t>(0x0ACC38));

    const auto info = tc.get_system_info();
    CHECK(info.units[0].slots[1].material == "PETG");
    CHECK(info.units[0].slots[1].color_rgb == 0x0ACC38);
    // "?" is the firmware saying nothing is set.
    auto unset = helix::ams::lane_sources(tc.lane_id(2)).vendor_cache;
    REQUIRE(unset.has_value());
    CHECK_FALSE(unset->material.has_value());
}

TEST_CASE("A frame without slots leaves the firmware reading standing",
          "[toolchanger][zmod][material]") {
    helix::ams::reset_lane_sources();
    helix::test::RegisteredBackend<ToolChangerHelper> tc_reg(4);
    ToolChangerHelper& tc = *tc_reg;
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});

    auto vc = helix::ams::lane_sources(tc.lane_id(1)).vendor_cache;
    REQUIRE(vc.has_value());
    CHECK(vc->material == std::optional<std::string>("PETG"));
}

TEST_CASE("Z-Mod's valid types are the supported materials", "[toolchanger][zmod][material]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    CHECK_FALSE(tc.get_supported_materials().has_value());
    tc.feed(full_zmod_color_frame());
    auto types = tc.get_supported_materials();
    REQUIRE(types.has_value());
    CHECK(*types == std::vector<std::string>{"PLA", "PETG", "ABS"});
}

TEST_CASE("A changer without a material source files no firmware reading",
          "[toolchanger][zmod][material]") {
    helix::ams::reset_lane_sources();
    helix::test::RegisteredBackend<ToolChangerHelper> tc_reg(4); // no wire_material_source
    ToolChangerHelper& tc = *tc_reg;
    wire_zmod(tc);
    tc.feed(full_zmod_color_frame());
    CHECK_FALSE(helix::ams::lane_sources(tc.lane_id(1)).vendor_cache.has_value());
    CHECK_FALSE(tc.get_supported_materials().has_value());
}

namespace {

/// Slot as the editor hands it over: the current slot with the user's change.
SlotInfo edited(const ToolChangerHelper& tc, int slot, std::optional<uint32_t> rgb,
                std::optional<std::string> material) {
    SlotInfo info = tc.get_system_info().units[0].slots[static_cast<size_t>(slot)];
    if (rgb) {
        info.color_rgb = *rgb;
    }
    if (material) {
        info.material = *material;
    }
    return info;
}

size_t zcolor_sends(const ToolChangerHelper& tc) {
    size_t n = 0;
    for (const auto& g : tc.sent()) {
        n += g.rfind("CHANGE_ZCOLOR", 0) == 0 ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("A colour outside the palette is snapped", "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    REQUIRE(tc.commit_user_edit(0, before, edited(tc, 0, 0xE01010, std::nullopt)).success());
    REQUIRE_FALSE(tc.sent().empty());
    CHECK(tc.sent().back() == "CHANGE_ZCOLOR SLOT=1 HEX=F72224 TYPE=PLA SILENT=1");
}

TEST_CASE("A material outside the firmware's types is mapped to one of them",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    const SlotInfo before = tc.get_system_info().units[0].slots[1];
    REQUIRE(tc.commit_user_edit(1, before, edited(tc, 1, std::nullopt, "PETG-CF")).success());
    CHECK(tc.sent().back() == "CHANGE_ZCOLOR SLOT=2 HEX=0ACC38 TYPE=PETG SILENT=1");
}

TEST_CASE("The firmware owns colour and material: an edit declares neither",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    // Registered, because the lane funnels drop what an unstamped lane_id names.
    helix::test::RegisteredBackend<ToolChangerHelper> tc_reg(4);
    ToolChangerHelper& tc = *tc_reg;
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());
    REQUIRE(tc.firmware_stores_color_and_material(0));

    SlotInfo info = edited(tc, 0, 0xF72224, "ABS");
    info.brand = "Polymaker";
    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    REQUIRE(tc.commit_user_edit(0, before, info).success());

    auto user = helix::ams::lane_sources(tc.lane_id(0)).local_user;
    REQUIRE(user.has_value());
    CHECK_FALSE(user->color_rgb.has_value());
    CHECK_FALSE(user->material.has_value());
    CHECK(user->brand == std::optional<std::string>("Polymaker"));
}

TEST_CASE("An edit that changes neither colour nor material sends nothing",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    SlotInfo info = tc.get_system_info().units[0].slots[0];
    const SlotInfo before = info;
    info.brand = "Polymaker";
    REQUIRE(tc.commit_user_edit(0, before, info).success());
    CHECK(zcolor_sends(tc) == 0);
}

TEST_CASE("Before the firmware publishes slots an edit stays local",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    // Registered, because the lane funnels drop what an unstamped lane_id names.
    helix::test::RegisteredBackend<ToolChangerHelper> tc_reg(4);
    ToolChangerHelper& tc = *tc_reg;
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}}); // a Z-Mod without the export
    CHECK_FALSE(tc.firmware_stores_color_and_material(0));

    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    REQUIRE(tc.commit_user_edit(0, before, edited(tc, 0, 0xF72224, "ABS")).success());
    CHECK(zcolor_sends(tc) == 0);
    auto user = helix::ams::lane_sources(tc.lane_id(0)).local_user;
    REQUIRE(user.has_value());
    CHECK(user->color_rgb == std::optional<uint32_t>(0xF72224));
}

TEST_CASE("An unsafe type is refused", "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    // Registered, because the refusal must also leave the lane model untouched.
    helix::test::RegisteredBackend<ToolChangerHelper> tc_reg(4);
    ToolChangerHelper& tc = *tc_reg;
    wire_zmod(tc);
    wire_material_source(tc);
    json frame = full_zmod_color_frame();
    frame["zmod_color"]["valid_types"] = json::array({"PLA;M112", "?"});
    tc.feed(frame);

    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    auto err = tc.commit_user_edit(0, before, edited(tc, 0, 0xF72224, "PLA;M112"));
    CHECK_FALSE(err.success());
    CHECK_FALSE(err.partially_applied);
    CHECK(zcolor_sends(tc) == 0);
    // The refusal changes nothing: the slot keeps the firmware reading and no
    // override is staged anywhere.
    const SlotInfo after = tc.get_system_info().units[0].slots[0];
    CHECK(after.material == before.material);
    CHECK(after.color_rgb == before.color_rgb);
    CHECK_FALSE(ToolChangerTestAccess::has_overrides(tc));
    CHECK_FALSE(helix::ams::lane_sources(tc.lane_id(0)).local_user.has_value());
}

TEST_CASE("A failed firmware write still declares the rest of the edit",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    helix::test::RegisteredBackend<ToolChangerHelper> tc_reg(4);
    ToolChangerHelper& tc = *tc_reg;
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());

    SlotInfo info = edited(tc, 0, 0xF72224, "ABS");
    info.brand = "Polymaker";
    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    tc.set_fail_gcode(true);
    auto err = tc.commit_user_edit(0, before, info);
    CHECK_FALSE(err.success());
    // The colour and material reached everything but the firmware; the brand
    // is still the user's statement and must be declared on the lane.
    CHECK(err.partially_applied);
    CHECK(zcolor_sends(tc) == 1);
    auto user = helix::ams::lane_sources(tc.lane_id(0)).local_user;
    REQUIRE(user.has_value());
    CHECK(user->brand == std::optional<std::string>("Polymaker"));
}

TEST_CASE("A slots-only frame keeps the latched palette and types",
          "[toolchanger][zmod][material][write]") {
    helix::ams::reset_lane_sources();
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(full_zmod_color_frame());
    json slots_only = full_zmod_color_frame();
    slots_only["zmod_color"].erase("palette");
    slots_only["zmod_color"].erase("valid_types");
    tc.feed(slots_only);

    const SlotInfo before = tc.get_system_info().units[0].slots[0];
    REQUIRE(tc.commit_user_edit(0, before, edited(tc, 0, 0xE01010, std::nullopt)).success());
    CHECK(tc.sent().back() == "CHANGE_ZCOLOR SLOT=1 HEX=F72224 TYPE=PLA SILENT=1");
    auto types = tc.get_supported_materials();
    REQUIRE(types.has_value());
    CHECK(*types == std::vector<std::string>{"PLA", "PETG", "ABS"});
}
