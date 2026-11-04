// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_manual_pull_prompt.h"

#include "../test_helpers/toolchanger_test_helper.h"
#include "../ui_test_utils.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "filament_op_slot_resolver.h"
#include "lane_source_store.h"
#include "printer_discovery.h"
#include "standard_macros.h"
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

/// The C5 with Z-Mod 1.7.4's afclite shim: the AFC objects it registers, with a
/// lane per head. `lanes` names the AFC_lane objects it publishes.
PrinterDiscovery zmod_c5_afclite_discovery(const std::vector<std::string>& lanes = {"E0", "E1",
                                                                                    "E2", "E3"}) {
    json objects = json::array({"zmod", "zmod_color", "save_variables", "zmod_afclite", "AFC",
                                "AFC_unit C5", "extruder", "extruder1", "extruder2", "extruder3",
                                "heater_bed", "toolhead", "gcode_move", "configfile"});
    for (const auto& lane : lanes) {
        objects.push_back("AFC_lane " + lane);
    }
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
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

/// wire_zmod() plus the feed this discovery offers.
void wire_zmod_with(ToolChangerHelper& tc, const PrinterDiscovery& hw) {
    tc.set_tool_commands(toolchanger_addon::resolve_tool_commands(hw));
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(hw));
    tc.set_tool_feed(toolchanger_addon::resolve_tool_feed(hw));
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
    // flight and a following park would be refused as busy.
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 2}}}});
    REQUIRE(tc.park_active_tool().success());
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
    // Registered, because lane_id() answers kInvalidLaneId for a backend
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

namespace {

std::vector<std::string> step_labels(const OperationStepModel& model) {
    std::vector<std::string> out;
    out.reserve(model.steps.size());
    for (const auto& s : model.steps) {
        out.push_back(s.label);
    }
    return out;
}

} // namespace

TEST_CASE("Z-Mod's swap narration classifies into phase words", "[toolchanger][zmod][steps]") {
    // Marker text is verbatim from field bundle 5WSPTZBL's klippy log, with the
    // respond_info framing the wire adds.
    auto grab = toolchanger_addon::parse_zmod_marker("// Grab attempt for T0: 1 of 5...");
    REQUIRE(grab.has_value());
    CHECK(grab->operation == "picking");
    CHECK(grab->phase_names_direction);

    auto unload = toolchanger_addon::parse_zmod_marker("// Unload attempt for T2: 3 of 5...");
    REQUIRE(unload.has_value());
    CHECK(unload->operation == "dropping");
    CHECK(unload->phase_names_direction);

    auto t_in = toolchanger_addon::parse_zmod_marker("X-Offset: 0.0000 _T_IN");
    REQUIRE(t_in.has_value());
    CHECK(t_in->operation == "picking");

    auto t_out = toolchanger_addon::parse_zmod_marker("Z-Offset: 0.0000 _T_OUT");
    REQUIRE(t_out.has_value());
    CHECK(t_out->operation == "dropping");

    // Console lines the swap macros never emit, including T_INFO's own tool
    // table: without a marker token there is no phase to report.
    CHECK_FALSE(toolchanger_addon::parse_zmod_marker("ok").has_value());
    CHECK_FALSE(toolchanger_addon::parse_zmod_marker("// T0: home").has_value());
    CHECK_FALSE(toolchanger_addon::parse_zmod_marker("// Slot 1 Config (PLA):").has_value());
    CHECK_FALSE(toolchanger_addon::parse_zmod_marker("// Door: Close").has_value());
}

TEST_CASE("Z-Mod's markers light the swap step bar", "[toolchanger][zmod][steps]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});

    // The direction capability is known from the provider row (the dialect's
    // markers are directional by construction), so the bar exists the moment a
    // swap is dispatched — before the first marker lands. Nothing to dock on a
    // fresh mount, nothing to pick up on an unmount.
    CHECK(step_labels(tc.get_operation_step_model(StepOperationType::LOAD_SWAP)) ==
          std::vector<std::string>{"Dock tool", "Pick up tool"});
    CHECK(step_labels(tc.get_operation_step_model(StepOperationType::LOAD_FRESH)) ==
          std::vector<std::string>{"Pick up tool"});
    CHECK(step_labels(tc.get_operation_step_model(StepOperationType::UNLOAD)) ==
          std::vector<std::string>{"Dock tool"});
    // But at rest there is no step to be on.
    CHECK(tc.get_system_info().operation_phase < 0);

    // The active step operation is process-wide; restore the default for later
    // cases in other files.
    struct RestoreStepOperation {
        ~RestoreStepOperation() {
            AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
        }
    } restore_step_operation;

    ToolChangerTestAccess::feed_zmod_marker(tc, "// Unload attempt for T1: 1 of 5...");
    CHECK(tc.get_system_info().action == AmsAction::UNLOADING);
    // Default active operation is LOAD_SWAP: docking is its first step.
    REQUIRE(tc.get_system_info().operation_phase == 0);

    ToolChangerTestAccess::feed_zmod_marker(tc, "// Grab attempt for T2: 1 of 5...");
    CHECK(tc.get_system_info().action == AmsAction::SELECTING);
    CHECK(tc.get_system_info().operation_phase == 1); // Pick up tool

    // The markers are a phase source, not a tool sensor: the carriage reading
    // is still zmod_color's alone.
    CHECK(tc.get_current_tool() == 1);
}

TEST_CASE("A Z-Mod marker with no operation running still names a real swap",
          "[toolchanger][zmod][steps]") {
    // An _T_IN typed into Mainsail is a genuine swap the screen did not start;
    // the marker reports it the same way.
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});

    struct RestoreStepOperation {
        ~RestoreStepOperation() {
            AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
        }
    } restore_step_operation;
    AmsState::instance().set_active_step_operation(StepOperationType::LOAD_FRESH);

    ToolChangerTestAccess::feed_zmod_marker(tc, "// Grab attempt for T0: 1 of 5...");
    CHECK(tc.get_system_info().action == AmsAction::SELECTING);
    // Fresh mount: picking is the only step.
    CHECK(tc.get_system_info().operation_phase == 0);
}

TEST_CASE("Z-Mod's endless spool is firmware-managed and always on", "[toolchanger][zmod]") {
    using namespace helix::printer;
    ToolChangerHelper zmod_tc(4);
    wire_zmod(zmod_tc);
    const auto caps = zmod_tc.get_endless_spool_capabilities();
    CHECK(caps.available());
    CHECK(caps.provider == "zmod");
    CHECK(caps.enabled == EndlessSpoolEnabled::On);
    CHECK(caps.editability == EndlessSpoolEditability::ReadOnly);
    CHECK(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);

    // Any other tool changer keeps the base default: nothing to switch to when
    // every tool is its own path and no firmware speaks for them.
    ToolChangerHelper plain(2);
    CHECK_FALSE(plain.get_endless_spool_capabilities().available());
}

TEST_CASE("T_INFO rows classify into dock and carriage facts", "[toolchanger][zmod][tinfo]") {
    bool ended = true;
    auto head = toolchanger_addon::parse_zmod_tinfo_line("// T1: HEAD", ended);
    REQUIRE(head.has_value());
    CHECK(head->current_tool == 1);
    CHECK_FALSE(ended);

    auto home = toolchanger_addon::parse_zmod_tinfo_line("// T0: home", ended);
    REQUIRE(home.has_value());
    REQUIRE(home->docks.size() == 1);
    CHECK(home->docks[0].has_value());
    CHECK(*home->docks[0]);

    auto both = toolchanger_addon::parse_zmod_tinfo_line("// T2: ERROR (both)", ended);
    REQUIRE(both.has_value());
    CHECK(both->sensor_error);

    // Undefined state says nothing for that tool; other console lines say
    // nothing at all.
    CHECK_FALSE(toolchanger_addon::parse_zmod_tinfo_line("// T3: ?", ended).has_value());
    CHECK_FALSE(toolchanger_addon::parse_zmod_tinfo_line("// Door: Close", ended).has_value());
    CHECK_FALSE(toolchanger_addon::parse_zmod_tinfo_line("ok", ended).has_value());

    // The Offset row closes the block.
    auto offset = toolchanger_addon::parse_zmod_tinfo_line(
        "// Offset: X=0.0000 Y=0.0000 Z=0.061 (0.000)", ended);
    CHECK_FALSE(offset.has_value());
    CHECK(ended);
}

TEST_CASE("A T_INFO answer clears a stale carriage reading at boot", "[toolchanger][zmod][tinfo]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    // Firmware that only recomputes active_tool_id inside its own commands
    // boots with the dock and carriage buttons disagreeing.
    tc.feed(json{{"zmod_color", {{"active_tool_id", -2}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);

    for (int i = 0; i < 4; ++i) {
        ToolChangerTestAccess::feed_zmod_marker(tc, "// T" + std::to_string(i) + ": home");
    }
    ToolChangerTestAccess::feed_zmod_marker(tc, "// Offset: X=0.0000 Y=0.0000 Z=0.0000 (0.000)");
    CHECK(tc.get_current_tool() == -1);
    CHECK(tc.get_system_info().action == AmsAction::IDLE);

    // A stale POSITIVE carriage reading is the case only the block end can
    // clear: the agreeing home rows withdrew the fault above without touching
    // current_tool, and nothing else says the carriage is empty.
    tc.feed(json{{"zmod_color", {{"active_tool_id", 2}}}});
    REQUIRE(tc.get_current_tool() == 2);
    for (int i = 0; i < 4; ++i) {
        ToolChangerTestAccess::feed_zmod_marker(tc, "// T" + std::to_string(i) + ": home");
    }
    ToolChangerTestAccess::feed_zmod_marker(tc, "// Offset: X=0.0000 Y=0.0000 Z=0.0000 (0.000)");
    CHECK(tc.get_current_tool() == -1);

    // And a HEAD row names the carriage tool.
    ToolChangerTestAccess::feed_zmod_marker(tc, "// T2: HEAD");
    ToolChangerTestAccess::feed_zmod_marker(tc, "// Offset: X=0.0000 Y=0.0000 Z=0.0000 (0.000)");
    CHECK(tc.get_current_tool() == 2);
}

TEST_CASE("A T_INFO block that names a HEAD keeps its reading at block end",
          "[toolchanger][zmod][tinfo]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    REQUIRE(tc.get_current_tool() == 1);

    ToolChangerTestAccess::feed_zmod_marker(tc, "// T1: HEAD");
    ToolChangerTestAccess::feed_zmod_marker(tc, "// Offset: X=0.0000 Y=0.0000 Z=0.0000 (0.000)");
    CHECK(tc.get_current_tool() == 1);
}

TEST_CASE("The firmware's per-head filament presence stamps the slot",
          "[toolchanger][zmod][filament]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    wire_material_source(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
    REQUIRE(tc.get_system_info().units[0].slots[0].status == SlotStatus::AVAILABLE);

    json frame = full_zmod_color_frame();
    auto& slots = frame["zmod_color"]["slots"];
    slots[0]["hasFilament"] = false;
    slots[1]["hasFilament"] = true;
    tc.feed(frame);

    // By value: get_system_info() returns a copy, and a reference bound into
    // that temporary dangles.
    const auto stamped = tc.get_system_info().units[0].slots;
    CHECK(stamped[0].status == SlotStatus::EMPTY);
    CHECK(stamped[1].status == SlotStatus::AVAILABLE);
    // A head the frame said nothing about keeps its answer.
    CHECK(stamped[2].status == SlotStatus::AVAILABLE);
    CHECK(stamped[3].status == SlotStatus::AVAILABLE);
}

TEST_CASE("The afclite shim's fault flag surfaces and clears", "[toolchanger][zmod][afclite]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::IDLE);

    tc.feed(json{{"zmod_afclite",
                  {{"current_state", "error"}, {"error_state", true}, {"message", "Lane jam"}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::ERROR);
    auto fault = tc.current_error();
    REQUIRE(fault.has_value());
    CHECK(fault->detail == "Lane jam");

    tc.feed(json{{"zmod_afclite", {{"current_state", "idle"}, {"error_state", false}}}});
    CHECK(tc.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(tc.current_error().has_value());
}

TEST_CASE("An unknown afclite activity word changes nothing", "[toolchanger][zmod][afclite]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    REQUIRE(tc.get_system_info().action == AmsAction::IDLE);

    // "loading" is plausible vocabulary but unverified on this firmware: it is
    // logged, and the action stays whatever the verified sources said.
    tc.feed(json{{"zmod_afclite", {{"current_state", "loading"}, {"error_state", false}}}});
    CHECK(tc.get_system_info().action == AmsAction::IDLE);
    CHECK(tc.get_current_tool() == 1);
}

// ---------------------------------------------------------------------------
// No automatic unload: the user cuts the filament at the head and pulls it out.
// ---------------------------------------------------------------------------

namespace {

/// Collects the info toasts raised while it lives.
struct InfoToasts {
    std::vector<std::string> seen;
    InfoToasts() {
        helix::ui::set_test_notification_info_hook(
            [this](const std::string& msg) { seen.push_back(msg); });
    }
    ~InfoToasts() {
        helix::ui::set_test_notification_info_hook(nullptr);
    }
};

} // namespace

TEST_CASE("Z-Mod's Creator 5 row has a manual unload", "[toolchanger][zmod][manual_unload]") {
    ToolChangerHelper zmod(4);
    wire_zmod(zmod);
    CHECK(zmod.unload_is_manual());

    ToolChangerHelper plain(4);
    CHECK_FALSE(plain.unload_is_manual());
}

TEST_CASE("The manual-unload notice is raised once per tap and sends nothing",
          "[toolchanger][zmod][manual_unload]") {
    InfoToasts toasts;
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 2}}}});
    const size_t sent_before = tc.sent().size();

    CHECK(helix::ui::notify_if_filament_op_unavailable(&tc, -1, /*load=*/false));
    CHECK(helix::ui::notify_if_filament_op_unavailable(&tc, -1, /*load=*/false));

    REQUIRE(toasts.seen.size() == 2);
    CHECK(toasts.seen[0] == toasts.seen[1]); // identical text, which the toast stack folds
    CHECK(toasts.seen[0].find("no automatic unload") != std::string::npos);
    CHECK(tc.sent().size() == sent_before);
}

TEST_CASE("A tool changer with an automatic unload gets no notice",
          "[toolchanger][zmod][manual_unload]") {
    InfoToasts toasts;
    ToolChangerHelper tc(4);
    CHECK_FALSE(helix::ui::notify_if_filament_op_unavailable(&tc, -1, /*load=*/false));
    CHECK_FALSE(helix::ui::notify_if_filament_op_unavailable(nullptr, -1, /*load=*/false));
    CHECK(toasts.seen.empty());
}

TEST_CASE("Every filament-unload dispatch stops at the notice on a manual-unload backend",
          "[toolchanger][zmod][manual_unload]") {
    InfoToasts toasts;
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 2}}}});
    const size_t sent_before = tc.sent().size();

    helix::ui::execute_filament_unload(&tc, 2, /*target_is_loaded=*/true, "[Test]");

    CHECK(toasts.seen.size() == 1);
    CHECK(tc.sent().size() == sent_before);
}

TEST_CASE("An explicit park still unmounts on Z-Mod, and Unload never does",
          "[toolchanger][zmod][manual_unload]") {
    // Park calls the backend directly and is not swallowed by the
    // filament-unload notice; Unload is a filament op and sends nothing.
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    const size_t before = tc.sent().size();
    CHECK_FALSE(tc.unload_filament(1).success());
    CHECK(tc.sent().size() == before);
    REQUIRE(tc.park_active_tool().success());
    CHECK(tc.sent().back() == "_T_OUT");
}

// ---------------------------------------------------------------------------
// Load feeds through the afclite shim's CHANGE_TOOL.
// ---------------------------------------------------------------------------

TEST_CASE("The afclite shim's lanes give each head a feed command", "[toolchanger][zmod][feed]") {
    const auto feed = toolchanger_addon::resolve_tool_feed(zmod_c5_afclite_discovery());
    REQUIRE(feed.present);
    REQUIRE(feed.head_gcode.size() == 4);
    for (int head = 0; head < 4; ++head) {
        CAPTURE(head);
        CHECK(feed.head_gcode[static_cast<size_t>(head)] ==
              "CHANGE_TOOL LANE=E" + std::to_string(head));
    }
}

TEST_CASE("Without the afclite shim there is no feed", "[toolchanger][zmod][feed]") {
    CHECK_FALSE(toolchanger_addon::resolve_tool_feed(zmod_c5_discovery()).present);
    CHECK_FALSE(toolchanger_addon::resolve_tool_feed(zmod_ad5x_discovery()).present);
}

TEST_CASE("A lane the shim did not publish is never built", "[toolchanger][zmod][feed]") {
    const auto feed =
        toolchanger_addon::resolve_tool_feed(zmod_c5_afclite_discovery({"E0", "E1", "E3"}));
    REQUIRE(feed.present);
    REQUIRE(feed.head_gcode.size() == 3);
    CHECK(feed.head_gcode[0] == "CHANGE_TOOL LANE=E0");
    CHECK(feed.head_gcode[1] == "CHANGE_TOOL LANE=E1");
    // E3 is published but head 2 is not: nothing for head 2, and head 3 lies
    // past the table.
    CHECK(feed.head_gcode[2].empty());

    ToolChangerHelper tc(4);
    wire_zmod_with(tc, zmod_c5_afclite_discovery({"E0", "E1", "E3"}));
    const size_t sent_before = tc.sent().size();
    CHECK_FALSE(tc.load_filament(2).success());
    CHECK_FALSE(tc.load_filament(3).success());
    CHECK(tc.sent().size() == sent_before);
}

TEST_CASE("Load with the afclite shim sends CHANGE_TOOL for that head's lane",
          "[toolchanger][zmod][feed]") {
    for (int head = 0; head < 4; ++head) {
        CAPTURE(head);
        ToolChangerHelper tc(4);
        wire_zmod_with(tc, zmod_c5_afclite_discovery());
        tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
        REQUIRE(tc.load_filament(head).success());
        REQUIRE(tc.sent().size() == 1);
        CHECK(tc.sent().back() == "CHANGE_TOOL LANE=E" + std::to_string(head));
    }
}

TEST_CASE("Load on Z-Mod without the shim refuses and mounts nothing",
          "[toolchanger][zmod][feed]") {
    ToolChangerHelper tc(4);
    wire_zmod_with(tc, zmod_c5_discovery());
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
    const size_t sent_before = tc.sent().size();
    const AmsError err = tc.load_filament(2);
    CHECK_FALSE(err.success());
    CHECK_FALSE(err.user_msg.empty());
    CHECK(tc.sent().size() == sent_before);
    CHECK_FALSE(tc.load_feeds_filament());
    CHECK(tc.head_filament_ops(2).load == helix::FilamentOpSupport::Manual);
}

TEST_CASE("A feed is no select-slot action: mounting is the toolhead tap",
          "[toolchanger][zmod][feed]") {
    ToolChangerHelper tc(4);
    wire_zmod_with(tc, zmod_c5_afclite_discovery());
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
    CHECK_FALSE(tc.supports_gate_select());
    CHECK(tc.head_filament_ops(2).load == helix::FilamentOpSupport::Automatic);
    // The Creator 5 cuts to unload, with or without the shim.
    CHECK(tc.head_filament_ops(2).unload == helix::FilamentOpSupport::Manual);
}

TEST_CASE("Mounting with change_tool is untouched by the feed", "[toolchanger][zmod][feed]") {
    ToolChangerHelper tc(4);
    wire_zmod_with(tc, zmod_c5_afclite_discovery());
    tc.feed(json{{"zmod_color", {{"active_tool_id", -1}}}});
    REQUIRE(tc.change_tool(1).success());
    CHECK(tc.sent().back() == "_T_IN T=1");
}

TEST_CASE("A klipper-toolchanger machine with no feed never mounts on Load or Unload",
          "[toolchanger][zmod][feed]") {
    ToolChangerHelper tc(4);
    CHECK_FALSE(tc.load_feeds_filament());
    CHECK_FALSE(tc.supports_gate_select());
    CHECK_FALSE(tc.supports_auto_heat_on_load());
    const auto ops = tc.head_filament_ops(1);
    CHECK(ops.load == helix::FilamentOpSupport::Manual);
    CHECK(ops.unload == helix::FilamentOpSupport::Manual);
    CHECK(ops.load_reason == ops.unload_reason);
    const size_t sent_before = tc.sent().size();
    CHECK_FALSE(tc.load_filament(1).success());
    CHECK_FALSE(tc.unload_filament(1).success());
    CHECK(tc.sent().size() == sent_before);
}

TEST_CASE("Load plans as a feed on a head that is already mounted, only with the shim",
          "[toolchanger][zmod][feed]") {
    ToolChangerHelper with_shim(4);
    wire_zmod_with(with_shim, zmod_c5_afclite_discovery());
    with_shim.feed(json{{"zmod_color", {{"active_tool_id", 2}}}});

    AmsSystemInfo info;
    auto caps = helix::ui::read_backend_caps(&with_shim, info, 2);
    REQUIRE(info.current_slot == 2);
    auto plan = helix::ui::plan_load(info, caps, 2, false, false);
    CHECK(plan.tier == helix::ui::FilamentTier::AmsBackend);
    CHECK(plan.ams_call == helix::ui::AmsCall::Load);
    CHECK(plan.ams_arg == 2);

    // A different head is fed too, not swapped in.
    caps = helix::ui::read_backend_caps(&with_shim, info, 1);
    plan = helix::ui::plan_load(info, caps, 1, false, false);
    CHECK(plan.ams_call == helix::ui::AmsCall::Load);
}

TEST_CASE("Load stays enabled on a loaded head only when Load is a feed",
          "[toolchanger][zmod][feed]") {
    helix::ui::OpButtonState state;
    state.slot_is_loaded = true;
    CHECK(helix::ui::compute_op_button_gating(state).load_disabled);

    state.load_feeds_filament = true;
    CHECK_FALSE(helix::ui::compute_op_button_gating(state).load_disabled);

    // Busy and printing refusals hold either way.
    state.system_busy = true;
    CHECK(helix::ui::compute_op_button_gating(state).load_disabled);
    state.system_busy = false;
    state.print_blocks_op = true;
    CHECK(helix::ui::compute_op_button_gating(state).load_disabled);
}

TEST_CASE("The UI does not preheat for a Z-Mod feed", "[toolchanger][zmod][feed]") {
    ToolChangerHelper with_shim(4);
    wire_zmod_with(with_shim, zmod_c5_afclite_discovery());
    ToolChangerHelper plain(4);

    helix::ui::FilamentOpPlan load;
    load.tier = helix::ui::FilamentTier::AmsBackend;
    load.ams_call = helix::ui::AmsCall::Load;
    load.ams_arg = 1;

    CHECK(helix::ui::preheat_skip_reason(load, StandardMacroSlot::LoadFilament, &with_shim) ==
          helix::ui::PreheatSkip::BackendSelfHeats);
    CHECK(helix::ui::preheat_skip_reason(load, StandardMacroSlot::LoadFilament, &plain) ==
          helix::ui::PreheatSkip::None);
}

TEST_CASE("Unload stays manual when Load is a feed", "[toolchanger][zmod][feed][manual_unload]") {
    ToolChangerHelper tc(4);
    wire_zmod_with(tc, zmod_c5_afclite_discovery());
    CHECK(tc.unload_is_manual());
}

TEST_CASE("A rejected Z-Mod swap ends: its markers never say idle",
          "[toolchanger][zmod][dispatch]") {
    ToolChangerHelper tc(4);
    wire_zmod(tc);
    tc.feed(json{{"zmod_color", {{"active_tool_id", 1}}}});
    struct RestoreStepOperation {
        ~RestoreStepOperation() {
            AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
        }
    } restore_step_operation;

    REQUIRE(tc.change_tool(2).success());
    ToolChangerTestAccess::feed_zmod_marker(tc, "// Unload attempt for T1: 1 of 5...");
    ToolChangerTestAccess::feed_zmod_marker(tc, "// Grab attempt for T2: 1 of 5...");
    REQUIRE(tc.get_current_action() == AmsAction::SELECTING);
    tc.reject_last_send(MoonrakerErrorType::JSON_RPC_ERROR, "Grab failed");
    CHECK(tc.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(tc.get_system_info().is_busy());
}
