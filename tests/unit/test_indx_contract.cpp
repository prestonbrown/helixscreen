// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic fixtures built from the shapes in Bondtech's upstream macros
// (BondtechAB/INDX macros/indx.cfg, indx-tc-macros.cfg, indx-cal.cfg).

#include "ams_types.h" // ams_type_from_string, is_tool_changer
#include "display_numbering.h"
#include "indx_contract.h"
#include "macro_param_cache.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// The objects a stock INDX printer lists, with @p tools T<n> macros.
json indx_objects(int tools = 4) {
    json objects = json::array(
        {"indx", "save_variables", "extruder", "heater_bed", "toolhead", "gcode_move", "configfile",
         "load_cell_probe", "gcode_macro TOOL_POSITIONS", "gcode_macro _TOOLCHANGE_VARS",
         "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL", "gcode_macro LOAD_FILAMENT",
         "gcode_macro SET_TOOL_FILAMENT", "gcode_macro TOOL_STATUS"});
    for (int i = 0; i < tools; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
    }
    return objects;
}

json without(json objects, const std::string& name) {
    json out = json::array();
    for (const auto& o : objects) {
        if (o.get<std::string>() != name) {
            out.push_back(o);
        }
    }
    return out;
}

PrinterDiscovery discover(const json& objects, const json& config = json::object()) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    if (!config.empty()) {
        hw.parse_config_keys(config);
        MacroParamCache::instance().populate_from_configfile(config, hw.macros());
    }
    return hw;
}

/// MacroParamCache is process-wide: a case that fills it empties it again.
struct CacheReset {
    ~CacheReset() {
        MacroParamCache::instance().clear();
    }
};

} // namespace

// ============================================================================
// Detection
// ============================================================================

TEST_CASE("INDX: the indx object alone registers the backend", "[ams][indx][detect]") {
    auto hw = discover(indx_objects());
    CHECK(indx::present(hw));
    CHECK(hw.mmu_type() == AmsType::INDX);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    CHECK(hw.detected_ams_systems()[0].type == AmsType::INDX);
}

TEST_CASE("INDX: an [indx] config section registers the backend after objects found nothing",
          "[ams][indx][detect]") {
    // Kalico's [indx] may not list as an object; the configfile section is
    // the only signal then, and it arrives after parse_objects() registered.
    PrinterDiscovery hw;
    hw.parse_objects(without(indx_objects(), "indx"));
    REQUIRE(hw.mmu_type() == AmsType::NONE);
    hw.parse_config_keys(json{{"indx", {{"mcu", "indxmcu"}}}});
    CHECK(indx::present(hw));
    CHECK(hw.mmu_type() == AmsType::INDX);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    CHECK(hw.detected_ams_systems()[0].type == AmsType::INDX);
}

TEST_CASE("INDX: the tool count is read from the config section in any case",
          "[ams][indx][detect]") {
    CacheReset reset;
    CHECK(indx::config_tool_count() == 0);
    auto hw = discover(indx_objects(),
                       json{{"gcode_macro tool_positions", {{"variable_tool_count", "3"}}}});
    CHECK(indx::config_tool_count() == 3);
    discover(indx_objects(), json{{"gcode_macro TOOL_POSITIONS", {{"variable_tool_count", "40"}}}});
    CHECK(indx::config_tool_count() == indx::kMaxTools);
}

TEST_CASE("INDX: the macros without the [indx] module are someone else's", "[ams][indx][detect]") {
    auto hw = discover(without(indx_objects(), "indx"),
                       json{{"gcode_macro TOOL_POSITIONS", {{"variable_tool_count", "4"}}}});
    CHECK_FALSE(indx::present(hw));
    CHECK(hw.mmu_type() == AmsType::NONE);
    CHECK(hw.detected_ams_systems().empty());
}

TEST_CASE("INDX: rediscovery forgets an INDX that is gone", "[ams][indx][detect]") {
    PrinterDiscovery hw;
    hw.parse_objects(indx_objects());
    REQUIRE(indx::present(hw));
    REQUIRE(hw.has_save_variables());
    hw.parse_objects(json::array({"extruder", "heater_bed"}));
    CHECK_FALSE(indx::present(hw));
    CHECK_FALSE(hw.has_save_variables());
    CHECK(hw.mmu_type() == AmsType::NONE);
}

TEST_CASE("INDX: a real MMU keeps its backend", "[ams][indx][detect]") {
    json objects = indx_objects();
    objects.push_back("mmu");
    auto hw = discover(objects);
    CHECK(hw.mmu_type() == AmsType::HAPPY_HARE);
}

TEST_CASE("INDX: one extruder and T<n> macros never also register a tool changer",
          "[ams][indx][detect]") {
    auto hw = discover(indx_objects(6));
    for (const auto& system : hw.detected_ams_systems()) {
        CHECK(system.type != AmsType::TOOL_CHANGER);
    }
}

// ============================================================================
// Contract
// ============================================================================

TEST_CASE("INDX: the stock macro set is recognized", "[ams][indx][contract]") {
    auto contract = indx::check_contract(discover(indx_objects()));
    CHECK(contract.recognized);
    CHECK(contract.missing.empty());
    CHECK(contract.has_load_filament);
}

TEST_CASE("INDX: each missing required object is named", "[ams][indx][contract]") {
    const std::vector<std::string> required = {"save_variables", "gcode_macro TOOL_POSITIONS",
                                               "gcode_macro _TOOLCHANGE_VARS",
                                               "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL"};
    for (const auto& name : required) {
        DYNAMIC_SECTION("missing " << name) {
            auto contract = indx::check_contract(discover(without(indx_objects(), name)));
            CHECK_FALSE(contract.recognized);
            REQUIRE(contract.missing.size() == 1);
            CHECK(contract.missing[0] == name);
        }
    }
}

TEST_CASE("INDX: the missing objects read as one line", "[ams][indx][contract]") {
    indx::Contract c;
    CHECK(indx::missing_text(c).empty());
    c.missing = {"save_variables", "gcode_macro PARK_TOOL"};
    CHECK(indx::missing_text(c) == "save_variables, gcode_macro PARK_TOOL");
}

TEST_CASE("INDX: LOAD_FILAMENT is optional", "[ams][indx][contract]") {
    auto contract =
        indx::check_contract(discover(without(indx_objects(), "gcode_macro LOAD_FILAMENT")));
    CHECK(contract.recognized);
    CHECK_FALSE(contract.has_load_filament);
}

TEST_CASE("INDX: a T<n> wrapper calling SET_ACTIVE_SPOOL manages the active spool",
          "[ams][indx][contract][spool]") {
    CacheReset reset;
    auto hw = discover(indx_objects(2));
    json config = {
        {"gcode_macro T0",
         {{"gcode", "SET_ACTIVE_SPOOL ID={printer['gcode_macro T0'].spool_id}\nCHANGE_TOOL TOOL=0"},
          {"variable_spool_id", "12"}}},
        {"gcode_macro T1", {{"gcode", "CHANGE_TOOL TOOL=1"}}},
    };
    MacroParamCache::instance().populate_from_configfile(config, hw.macros());
    CHECK(indx::macros_set_active_spool(hw));

    json stock = {{"gcode_macro T0", {{"gcode", "CHANGE_TOOL TOOL=0"}}},
                  {"gcode_macro T1", {{"gcode", "CHANGE_TOOL TOOL=1"}}}};
    MacroParamCache::instance().populate_from_configfile(stock, hw.macros());
    CHECK_FALSE(indx::macros_set_active_spool(hw));
    MacroParamCache::instance().clear();
}

TEST_CASE("INDX: subscriptions name the contract objects in their configured case",
          "[ams][indx][contract]") {
    json objects = without(indx_objects(2), "gcode_macro TOOL_POSITIONS");
    objects.push_back("gcode_macro Tool_Positions");
    auto subs = indx::required_status_objects(discover(objects));
    CHECK(subs["save_variables"] == json::array({"variables"}));
    REQUIRE(subs.contains("gcode_macro Tool_Positions"));
    CHECK(subs.contains("gcode_macro _TOOLCHANGE_VARS"));
    CHECK(subs.contains("gcode_macro LOAD_FILAMENT"));
    CHECK(subs.contains("gcode_macro T0"));
    CHECK(subs.contains("gcode_macro T1"));

    CHECK(indx::required_status_objects(discover(json::array({"extruder", "save_variables"})))
              .empty());
}

// ============================================================================
// Status
// ============================================================================

TEST_CASE("INDX: active_tool -1 and n", "[ams][indx][status]") {
    auto empty =
        indx::read_status(json{{"save_variables", {{"variables", {{"active_tool", -1}}}}}});
    REQUIRE(empty.has_value());
    CHECK(empty->active_tool == -1);

    auto mounted = indx::read_status(
        json{{"save_variables", {{"variables", {{"active_tool", 2}, {"toolchange_count", 41}}}}}});
    REQUIRE(mounted.has_value());
    CHECK(mounted->active_tool == 2);
    CHECK(mounted->toolchange_count == 41);
}

TEST_CASE("INDX: a frame without active_tool is no news", "[ams][indx][status]") {
    indx::Reading held;
    held.active_tool = 3;
    auto update =
        indx::read_status(json{{"save_variables", {{"variables", {{"global_z_offset", 0.12}}}}}});
    if (update) {
        CHECK_FALSE(update->active_tool.has_value());
        indx::merge(held, *update);
    }
    CHECK(held.active_tool == 3);
}

TEST_CASE("INDX: seat_suspect, latch and mode", "[ams][indx][status]") {
    auto r = indx::read_status(
        json{{"save_variables", {{"variables", {{"seat_suspect", 1}}}}},
             {"gcode_macro _TOOLCHANGE_VARS", {{"toolhead_state", 2}, {"mode", "sport"}}}});
    REQUIRE(r.has_value());
    CHECK(r->seat_suspect == 1);
    CHECK(r->toolhead_state == 2);
    CHECK(r->speed_mode == "sport");
}

TEST_CASE("INDX: TOOL_POSITIONS fields, in any key case", "[ams][indx][status]") {
    auto r = indx::read_status(json{{"gcode_macro tool_positions",
                                     {{"tool_count", 6},
                                      {"tools", json::array()},
                                      {"no_heat_tools", json::array({5})},
                                      {"docks_calibrated", false}}}});
    REQUIRE(r.has_value());
    CHECK(r->tool_count == 6);
    CHECK(r->no_heat_tools == std::set<int>{5});
    CHECK(r->docks_calibrated == false);
}

TEST_CASE("INDX: non-int values are ignored", "[ams][indx][status]") {
    auto r = indx::read_status(
        json{{"save_variables", {{"variables", {{"active_tool", "two"}, {"seat_suspect", 1.5}}}}},
             {"gcode_macro TOOL_POSITIONS", {{"tool_count", "many"}}}});
    if (r) {
        CHECK_FALSE(r->active_tool.has_value());
        CHECK_FALSE(r->seat_suspect.has_value());
        CHECK_FALSE(r->tool_count.has_value());
    }
}

TEST_CASE("INDX: filament presets keep the printer's own keys", "[ams][indx][status]") {
    auto r =
        indx::read_status(json{{"gcode_macro LOAD_FILAMENT",
                                {{"filament_presets",
                                  {{"PLA", {{"temp", 210}, {"density", 1.24}, {"hc", 1.8}}},
                                   {"petg", {{"temp", 240}, {"density", 1.27}, {"hc", 1.3}}}}}}}});
    REQUIRE(r.has_value());
    REQUIRE(r->presets.has_value());
    REQUIRE(r->presets->count("petg") == 1);
    CHECK(r->presets->at("petg").temp_c == Catch::Approx(240.0f));
    CHECK(r->presets->at("PLA").heat_capacity == Catch::Approx(1.8f));
}

TEST_CASE("INDX: a T<n> spool_id is read where declared", "[ams][indx][status][spool]") {
    auto r = indx::read_status(
        json{{"gcode_macro T3", {{"spool_id", 17}}}, {"gcode_macro T1", {{"active", true}}}});
    REQUIRE(r.has_value());
    CHECK(r->tool_spool_ids == std::map<int, int>{{3, 17}});
}

TEST_CASE("INDX: a frame with nothing INDX in it reads as nothing", "[ams][indx][status]") {
    CHECK_FALSE(indx::read_status(json{{"extruder", {{"temperature", 200.0}}}}).has_value());
    CHECK_FALSE(indx::read_status(json::array()).has_value());
}

// ============================================================================
// Commands
// ============================================================================

TEST_CASE("INDX: change sends T<n> when the macro exists, else CHANGE_TOOL",
          "[ams][indx][commands]") {
    auto with_t = discover(indx_objects(4));
    CHECK(indx::change_tool_gcode(with_t, 2) == "T2");

    auto no_t = discover(without(indx_objects(4), "gcode_macro T2"));
    CHECK(indx::change_tool_gcode(no_t, 2) == "CHANGE_TOOL TOOL=2");
    CHECK(indx::change_tool_gcode(no_t, 1) == "T1");
    CHECK(indx::park_gcode() == "PARK_TOOL");
}

TEST_CASE("INDX: load names a preset, else a temperature", "[ams][indx][commands]") {
    std::map<std::string, indx::FilamentPreset> presets = {{"PLA", {210, 1.24f, 1.8f}},
                                                           {"PETG", {240, 1.27f, 1.3f}}};
    CHECK(indx::load_gcode(1, "petg", 230, presets) == "LOAD_FILAMENT TOOL=1 TYPE=PETG");
    CHECK(indx::load_gcode(0, "Xenomat-9", 215, presets) == "LOAD_FILAMENT TOOL=0 TEMP=215");
    CHECK(indx::load_gcode(0, "", 220, presets) == "LOAD_FILAMENT TOOL=0 TEMP=220");
    // Unsafe on a gcode line: never sent as a TYPE, loaded by temperature.
    CHECK(indx::load_gcode(0, "PLA\nM112", 220, presets) == "LOAD_FILAMENT TOOL=0 TEMP=220");
    // The preset is sent as the printer spells it.
    std::map<std::string, indx::FilamentPreset> mixed = {{"Petg", {240, 1.27f, 1.3f}}};
    CHECK(indx::load_gcode(2, "PETG", 230, mixed) == "LOAD_FILAMENT TOOL=2 TYPE=Petg");
    // A family member loads as its family's preset.
    CHECK(indx::load_gcode(0, "PLA+", 215, presets) == "LOAD_FILAMENT TOOL=0 TYPE=PLA");
}

TEST_CASE("INDX: motion refused on an unrecognized contract, seat fault, uncalibrated docks",
          "[ams][indx][commands]") {
    indx::Contract ok;
    ok.recognized = true;
    indx::Reading state;
    CHECK(indx::motion_blocked_reason(ok, state).empty());

    indx::Contract bad;
    bad.missing = {"gcode_macro CHANGE_TOOL"};
    CHECK_FALSE(indx::motion_blocked_reason(bad, state).empty());

    state.seat_suspect = 2;
    CHECK_FALSE(indx::motion_blocked_reason(ok, state).empty());
    state.seat_suspect = -1;
    CHECK(indx::motion_blocked_reason(ok, state).empty());

    state.docks_calibrated = false;
    CHECK_FALSE(indx::motion_blocked_reason(ok, state).empty());
}

// ============================================================================
// Heat
// ============================================================================

TEST_CASE("INDX: heating needs a mounted tool with a hotend", "[ams][indx][heat]") {
    CHECK_FALSE(indx::heat_blocked_reason(-1, {}).empty());
    CHECK_FALSE(indx::heat_blocked_reason(3, {3}).empty());
    CHECK(indx::heat_blocked_reason(2, {3}).empty());
    CHECK(indx::heat_blocked_reason(0, {}).empty());
}

TEST_CASE("INDX: the type parses from its names and is a tool changer", "[ams][indx][detect]") {
    CHECK(ams_type_from_string("indx") == AmsType::INDX);
    CHECK(ams_type_from_string("INDX") == AmsType::INDX);
    CHECK(is_tool_changer(AmsType::INDX));
}

// ============================================================================
// Phase-3 device actions
// ============================================================================

namespace {

json with_phase3_macros(json objects) {
    for (const char* m : {"MODE_STEALTH", "MODE_NORMAL", "MODE_SPORT", "TC_CYCLE_ALL",
                          "MANUAL_TOOL_REMOVE", "MANUAL_TOOL_SEAT"}) {
        objects.push_back(std::string("gcode_macro ") + m);
    }
    return objects;
}

} // namespace

TEST_CASE("INDX contract: optional phase-3 macros are found", "[ams][indx][contract]") {
    auto all = indx::check_contract(discover(with_phase3_macros(indx_objects())));
    CHECK(all.recognized);
    CHECK(all.has_speed_modes);
    CHECK(all.has_cycle_all);
    CHECK(all.has_manual_tool_recovery);
    CHECK(all.has_set_tool_filament);

    auto no_sport = indx::check_contract(
        discover(without(with_phase3_macros(indx_objects()), "gcode_macro MODE_SPORT")));
    CHECK(no_sport.recognized);
    CHECK_FALSE(no_sport.has_speed_modes);
    CHECK(no_sport.has_cycle_all);

    auto no_seat = indx::check_contract(
        discover(without(with_phase3_macros(indx_objects()), "gcode_macro MANUAL_TOOL_SEAT")));
    CHECK(no_seat.recognized);
    CHECK_FALSE(no_seat.has_manual_tool_recovery);

    auto stock = indx::check_contract(discover(indx_objects()));
    CHECK_FALSE(stock.has_speed_modes);
    CHECK_FALSE(stock.has_cycle_all);
    CHECK_FALSE(stock.has_manual_tool_recovery);
    CHECK(stock.has_set_tool_filament);
    auto no_stf =
        indx::check_contract(discover(without(indx_objects(), "gcode_macro SET_TOOL_FILAMENT")));
    CHECK_FALSE(no_stf.has_set_tool_filament);
}

TEST_CASE("INDX contract: speed modes round-trip", "[ams][indx][contract]") {
    CHECK(indx::parse_speed_mode("normal") == indx::SpeedMode::Normal);
    CHECK(indx::parse_speed_mode("SPORT") == indx::SpeedMode::Sport);
    CHECK(indx::parse_speed_mode("Stealth") == indx::SpeedMode::Stealth);
    CHECK_FALSE(indx::parse_speed_mode("ludicrous").has_value());
    CHECK_FALSE(indx::parse_speed_mode("").has_value());
    for (auto mode : indx::kSpeedModes) {
        CHECK(indx::parse_speed_mode(indx::speed_mode_option(mode)) == mode);
    }
    CHECK(std::string(indx::speed_mode_option(indx::SpeedMode::Stealth)) == "Stealth");
    CHECK(indx::speed_mode_gcode(indx::SpeedMode::Sport) == "MODE_SPORT");
    CHECK(indx::speed_mode_gcode(indx::SpeedMode::Normal) == "MODE_NORMAL");
    CHECK(indx::speed_mode_gcode(indx::SpeedMode::Stealth) == "MODE_STEALTH");
}

TEST_CASE("INDX contract: recovery and calibration commands", "[ams][indx][contract]") {
    CHECK(indx::manual_seat_gcode(3) == "MANUAL_TOOL_SEAT TOOL=3");
    CHECK(indx::manual_remove_gcode() == "MANUAL_TOOL_REMOVE");
    CHECK(indx::cycle_all_gcode() == "TC_CYCLE_ALL");
    CHECK(indx::calibration_gcode(indx::HeaterCalibration::Heater) == "INDX_CALIBRATE");
    CHECK(indx::calibration_gcode(indx::HeaterCalibration::Fan) == "INDX_FAN_CALIBRATE");
}

TEST_CASE("INDX contract: calibration gates", "[ams][indx][contract]") {
    using indx::HeaterCalibration;
    indx::Contract ok;
    ok.recognized = true;
    indx::Reading state;
    state.active_tool = 2;
    state.no_heat_tools = std::set<int>{5};

    CHECK(indx::calibration_blocked_reason(ok, state, HeaterCalibration::Heater, 0.0).empty());
    CHECK_THAT(indx::calibration_blocked_reason(ok, state, HeaterCalibration::Heater, 210.0),
               Catch::Matchers::ContainsSubstring("heater off"));
    CHECK(indx::calibration_blocked_reason(ok, state, HeaterCalibration::Fan, 210.0).empty());

    indx::Reading empty = state;
    empty.active_tool = -1;
    CHECK_THAT(indx::calibration_blocked_reason(ok, empty, HeaterCalibration::Fan, 0.0),
               Catch::Matchers::ContainsSubstring("No tool on the head"));
    indx::Reading unknown = state;
    unknown.active_tool.reset();
    CHECK_THAT(indx::calibration_blocked_reason(ok, unknown, HeaterCalibration::Heater, 0.0),
               Catch::Matchers::ContainsSubstring("No tool on the head"));

    indx::Reading no_heat = state;
    no_heat.active_tool = 5;
    CHECK_THAT(indx::calibration_blocked_reason(ok, no_heat, HeaterCalibration::Heater, 0.0),
               Catch::Matchers::ContainsSubstring("has no hotend"));

    indx::Reading half = state;
    half.seat_suspect = 1;
    CHECK_THAT(indx::calibration_blocked_reason(ok, half, HeaterCalibration::Fan, 0.0),
               Catch::Matchers::ContainsSubstring("half seated"));

    indx::Reading uncalibrated = state;
    uncalibrated.docks_calibrated = false;
    CHECK(
        indx::calibration_blocked_reason(ok, uncalibrated, HeaterCalibration::Heater, 0.0).empty());

    indx::Contract bad;
    CHECK_THAT(indx::calibration_blocked_reason(bad, state, HeaterCalibration::Heater, 0.0),
               Catch::Matchers::ContainsSubstring("not recognized"));
}

TEST_CASE("INDX contract: tools are named the app's way", "[ams][indx][contract]") {
    CHECK(indx::tool_text(2) == "Tool 3 (T2)");
    CHECK(indx::tool_text(2) == helix::ui::tool_label_in_form(helix::ui::LaneNoun::Tool,
                                                              helix::ui::tool_label(2), 2,
                                                              helix::ui::ToolLabelForm::Full));
    CHECK_THAT(indx::seat_confirm_message(2), Catch::Matchers::ContainsSubstring("Tool 3 (T2)"));
    CHECK_THAT(indx::remove_confirm_message(2), Catch::Matchers::ContainsSubstring("Tool 3 (T2)"));

    json seat = indx_objects();
    seat.push_back("gcode_macro MANUAL_TOOL_SEAT");
    CHECK(indx::supports_seat_by_hand(discover(seat)));
    CHECK_FALSE(indx::supports_seat_by_hand(discover(indx_objects())));

    json toolchanger = json::array({"toolchanger", "tool T0", "tool T1", "extruder", "extruder1",
                                    "save_variables", "gcode_macro MANUAL_TOOL_SEAT"});
    CHECK_FALSE(indx::supports_seat_by_hand(discover(toolchanger)));
}

TEST_CASE("INDX contract: filament model command", "[ams][indx][contract]") {
    CHECK(indx::set_tool_filament_gcode(2, {240, 1.27f, 1.30f}) ==
          "SET_TOOL_FILAMENT TOOL=2 DENSITY=1.270 HEAT_CAPACITY=1.300");
    std::map<std::string, indx::FilamentPreset> presets = {{"PLA", {210, 1.24f, 1.8f}},
                                                           {"PETG", {240, 1.27f, 1.3f}}};
    CHECK(indx::preset_for_material("petg", presets) == std::optional<std::string>("PETG"));
    CHECK(indx::preset_for_material("PLA+", presets) == std::optional<std::string>("PLA"));
    CHECK_FALSE(indx::preset_for_material("Nylon; G28", presets).has_value());
    CHECK_FALSE(indx::preset_for_material("", presets).has_value());
    CHECK_FALSE(indx::preset_for_material("Xenomat-9", presets).has_value());
}
