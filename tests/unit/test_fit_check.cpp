// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_fit_check.cpp
 * @brief The file-fits-printer check: the shared table, the screen's own
 *        sources, the gate, and the detail view that shows the findings.
 *
 * Run with: ./build/bin/helix-tests "[plugin_fitcheck]"
 *
 * tests/fixtures/fit_check/cases.json is the table the plugin's checker
 * (moonraker-plugin/helix_fitcheck.py) is held to as well, so the two
 * cannot give different findings for one file.
 */

#include "ui_print_select_detail_view.h"
#include "ui_print_start_controller.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "app_globals.h"
#include "file_analysis.h"
#include "fit_check.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix_plugin_features.h"
#include "print_start_checks.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "printer_plugin_status_state.h"
#include "printer_state.h"

#include <algorithm>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

json shared_cases() {
    std::string src = __FILE__;
    const auto pos = src.rfind("/tests/unit/");
    const std::string root = pos != std::string::npos ? src.substr(0, pos) : ".";
    std::ifstream f(root + "/tests/fixtures/fit_check/cases.json");
    REQUIRE(f.is_open());
    return json::parse(f, nullptr, false);
}

FitAxes axes_of(const json& j) {
    FitAxes out;
    for (size_t i = 0; i < 3 && i < j.size(); ++i) {
        if (j[i].is_number()) {
            out[i] = j[i].get<double>();
        }
    }
    return out;
}

std::optional<double> opt_of(const json& j) {
    return j.is_number() ? std::optional<double>(j.get<double>()) : std::nullopt;
}

FitMachine machine_of(const json& j) {
    FitMachine m;
    m.kinematics = j["kinematics"].get<std::string>();
    m.axis_min = axes_of(j["axis_min"]);
    m.axis_max = axes_of(j["axis_max"]);
    m.print_radius = opt_of(j["print_radius"]);
    m.max_extruder_temp = opt_of(j["max_extruder_temp"]);
    m.max_bed_temp = opt_of(j["max_bed_temp"]);
    if (j["tool_count"].is_number()) {
        m.tool_count = j["tool_count"].get<int>();
    }
    m.tool_count_confirmed = j["tool_count_confirmed"].get<bool>();
    m.nozzle_diameters = j["nozzle_diameters"].get<std::vector<double>>();
    if (j.contains("nozzle_volume_types")) {
        m.nozzle_volume_types = j["nozzle_volume_types"].get<std::vector<std::string>>();
    }
    m.model = j["model"].get<std::string>();
    return m;
}

/// A table row's `live`: each extruder's status, by extruder number.
std::vector<FitLiveNozzle> live_of(const json& j) {
    std::vector<FitLiveNozzle> live;
    for (const auto& status : j) {
        FitLiveNozzle n;
        if (status.is_object()) {
            n.diameter = opt_of(status.value("nozzle_diameter", json()));
            if (status.value("nozzle_volume_type", json()).is_string()) {
                n.volume_type = status["nozzle_volume_type"].get<std::string>();
            }
        }
        live.push_back(n);
    }
    return live;
}

FitFile file_of(const json& j) {
    FitFile f;
    if (j["print"].is_object()) {
        f.print = fit_box_from_json(j["print"]);
    }
    if (j["travel"].is_object()) {
        f.travel = fit_box_from_json(j["travel"]);
    }
    f.max_hotend_temp = opt_of(j["max_hotend_temp"]);
    f.max_bed_temp = opt_of(j["max_bed_temp"]);
    for (const auto& t : j["tools"]) {
        f.tools.insert(t.get<int>());
    }
    f.nozzle_diameters = j["nozzle_diameters"].get<std::vector<double>>();
    if (j.contains("nozzle_volume_types")) {
        f.nozzle_volume_types = j["nozzle_volume_types"].get<std::vector<std::string>>();
    }
    if (j["printer_model"].is_string()) {
        f.printer_model = j["printer_model"].get<std::string>();
    }
    return f;
}

const json kSettings = {
    {"printer", {{"kinematics", "corexy"}}},
    {"stepper_x", {{"position_min", 0.0}, {"position_max", 220.0}}},
    {"stepper_y", {{"position_min", 0.0}, {"position_max", 220.0}}},
    {"stepper_z", {{"position_min", 0.0}, {"position_max", 250.0}}},
    {"extruder", {{"max_temp", 300.0}, {"nozzle_diameter", 0.4}}},
    {"heater_bed", {{"max_temp", 110.0}}},
};

FitBox box(double x0, double y0, double z0, double x1, double y1, double z1) {
    FitBox b;
    b.min = {x0, y0, z0};
    b.max = {x1, y1, z1};
    return b;
}

PrintStartContext ctx_for(const FitFile& file, bool with_machine = true) {
    PrintStartContext ctx;
    ctx.fit_file = file;
    if (with_machine) {
        ctx.fit_machine = fit_machine_from_settings(kSettings);
    }
    return ctx;
}

} // namespace

// ============================================================================
// The shared table
// ============================================================================

TEST_CASE("printer limits match the table the plugin reads too", "[plugin_fitcheck]") {
    const json cases = shared_cases();
    REQUIRE(cases["machines"].size() >= 8);
    for (const auto& c : cases["machines"]) {
        INFO(c["name"].get<std::string>());
        FitMachine m = fit_machine_from_settings(c["settings"]);
        if (c.contains("live")) {
            m = fit_machine_with_live_nozzles(m, live_of(c["live"]));
        }
        CHECK(fit_machine_json(m) == c["machine"]);
    }
}

TEST_CASE("findings match the table the plugin reads too", "[plugin_fitcheck]") {
    const json cases = shared_cases();
    REQUIRE(cases["checks"].size() >= 15);
    for (const auto& c : cases["checks"]) {
        INFO(c["name"].get<std::string>());
        json got = json::array();
        for (const FitFinding& f : check_fit(machine_of(c["machine"]), file_of(c["file"]))) {
            got.push_back(fit_finding_json(f));
        }
        CHECK(got == c["findings"]);
    }
}

TEST_CASE("limits of something that is not settings are unknown", "[plugin_fitcheck]") {
    const FitMachine m = fit_machine_from_settings(json::array({1, 2}));
    CHECK_FALSE(m.axis_max[0].has_value());
    CHECK_FALSE(m.tool_count.has_value());
}

// ============================================================================
// What this screen adds and reads
// ============================================================================

TEST_CASE("the screen names a known model and drops the tool count for a filament system",
          "[plugin_fitcheck]") {
    const FitMachine base = fit_machine_from_settings(kSettings);
    REQUIRE(base.tool_count == std::optional<int>(1));
    const FitMachine plain = fit_machine_for_screen(base, "Voron 0.2", false, 0.0);
    CHECK(plain.model == "Voron 0.2");
    CHECK(plain.tool_count == std::optional<int>(1));
    const FitMachine lanes = fit_machine_for_screen(base, "", true, 0.0);
    CHECK_FALSE(lanes.tool_count.has_value());

    FitFile two_tools;
    two_tools.tools = {0, 1};
    CHECK(check_fit(plain, two_tools).size() == 1);
    CHECK(check_fit(lanes, two_tools).empty());

    SECTION("a filament system's hardware can sit over the far edge of Y") {
        REQUIRE(base.axis_max[1] == std::optional<double>(220.0));
        const FitMachine shrunk = fit_machine_for_screen(base, "Creality K1C", true, 5.0);
        REQUIRE(shrunk.axis_max[1].has_value());
        CHECK(*shrunk.axis_max[1] == 215.0);

        FitFile edge;
        edge.travel = FitBox{};
        edge.travel->max[1] = 218.0;
        CHECK(check_fit(base, edge).empty());
        const auto findings = check_fit(shrunk, edge);
        REQUIRE(findings.size() == 1);
        CHECK(findings[0].axis == "y");

        // No Y limit known: the shrink has nothing to move.
        FitMachine no_y = base;
        no_y.axis_max[1].reset();
        CHECK_FALSE(fit_machine_for_screen(no_y, "", false, 5.0).axis_max[1].has_value());
    }
}

TEST_CASE("a nozzle's flow type reads the same in every spelling", "[plugin_fitcheck]") {
    for (const char* s : {"standard", "Standard", " STANDARD "}) {
        CAPTURE(s);
        CHECK(fit_nozzle_volume_type(s) == "standard");
    }
    for (const char* s : {"high_flow", "High Flow", "HighFlow", "high-flow"}) {
        CAPTURE(s);
        CHECK(fit_nozzle_volume_type(s) == "high_flow");
    }
    for (const char* s : {"", "hardened", "Standard HF", "flow"}) {
        CAPTURE(s);
        CHECK(fit_nozzle_volume_type(s).empty());
    }
}

TEST_CASE("the nozzles the extruders report replace the config's", "[plugin_fitcheck]") {
    FitMachine base;
    base.nozzle_diameters = {0.4, 0.4};

    SECTION("a reported diameter and flow type") {
        FitLiveNozzle second;
        second.diameter = 0.6;
        second.volume_type = "High Flow";
        const FitMachine m = fit_machine_with_live_nozzles(base, {FitLiveNozzle{}, second});
        CHECK(m.nozzle_diameters == std::vector<double>{0.4, 0.6});
        CHECK(m.nozzle_volume_types == std::vector<std::string>{"", "high_flow"});
    }
    SECTION("nothing reported changes nothing") {
        const FitMachine m = fit_machine_with_live_nozzles(base, {});
        CHECK(m.nozzle_diameters == base.nozzle_diameters);
        CHECK(m.nozzle_volume_types.empty());
    }
    SECTION("an extruder the config does not list gets no diameter") {
        FitLiveNozzle third;
        third.diameter = 0.8;
        third.volume_type = "standard";
        const FitMachine m = fit_machine_with_live_nozzles(base, {{}, {}, third});
        CHECK(m.nozzle_diameters == std::vector<double>{0.4, 0.4});
        CHECK(m.nozzle_volume_types == std::vector<std::string>{"", "", "standard"});
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "the screen's limits take the nozzles the extruders report",
                 "[plugin_fitcheck]") {
    PrinterState& state = get_printer_state();
    state.init_subjects(false);
    PrinterDiscovery hw;
    hw.parse_fit_limits(json{{"extruder", {{"max_temp", 300.0}, {"nozzle_diameter", 0.4}}},
                             {"extruder1", {{"max_temp", 300.0}, {"nozzle_diameter", 0.4}}}});
    state.set_hardware(hw);
    auto& temperatures = state.temperature_state();
    temperatures.init_extruders({"extruder", "extruder1"});

    auto machine = current_fit_machine();
    REQUIRE(machine.has_value());
    CHECK(machine->nozzle_diameters == std::vector<double>{0.4, 0.4});
    CHECK(machine->nozzle_volume_types.empty());

    // Stock Klipper answers a field its extruder lacks with null.
    temperatures.update_from_status(
        {{"extruder", {{"nozzle_diameter", nullptr}, {"nozzle_volume_type", nullptr}}},
         {"extruder1", {{"nozzle_diameter", 0.6}, {"nozzle_volume_type", "high_flow"}}}});
    machine = current_fit_machine();
    REQUIRE(machine.has_value());
    CHECK(machine->nozzle_diameters == std::vector<double>{0.4, 0.6});
    CHECK(machine->nozzle_volume_types == std::vector<std::string>{"", "high_flow"});

    FitFile file;
    file.tools = {1};
    file.nozzle_diameters = {0.4, 0.6};
    file.nozzle_volume_types = {"standard", "standard"};
    const auto findings = check_fit(*machine, file);
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].kind == FitKind::NozzleFlowMismatch);
    CHECK(findings[0].severity == FitSeverity::Warn);
    CHECK(fit_finding_text(findings[0]) ==
          "Sliced for a standard nozzle on T1; the printer has High Flow.");

    temperatures.init_extruders({});
    state.set_hardware(PrinterDiscovery{});
}

TEST_CASE("High Flow G-code on a standard nozzle asks before the start, never stops it",
          "[plugin_fitcheck]") {
    FitFile file;
    file.tools = {0};
    file.nozzle_volume_types = {"high_flow"};
    PrintStartContext ctx = ctx_for(file);
    ctx.fit_machine->nozzle_volume_types = {"standard"};
    const CheckResult r = gate_fit_check(ctx);
    CHECK(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.body.find("High Flow") != std::string::npos);
    CHECK(r.body.find("T0") != std::string::npos);
}

TEST_CASE("a file's head gives its printer, nozzles and hottest targets", "[plugin_fitcheck]") {
    SECTION("PrusaSlicer and Orca config comments, M-codes and Klipper's heater command") {
        const FitHeadFacts facts = fit_head_facts("; generated by OrcaSlicer 2.3.1\n"
                                                  "; printer_settings_id = My Voron\n"
                                                  "; printer_model = Voron 0.2\n"
                                                  "; nozzle_diameter = 0.4,0.6\n"
                                                  "M140 S60 ; bed\n"
                                                  "M190 S65\n"
                                                  "m104 s215\n"
                                                  "M109 S230 ; wait\n"
                                                  "SET_HEATER_TEMPERATURE HEATER=extruder "
                                                  "TARGET=240\n"
                                                  "SET_HEATER_TEMPERATURE HEATER=heater_bed "
                                                  "TARGET=70\n"
                                                  "; M104 S999 is only a comment\n"
                                                  "G1 X10 S500\n");
        CHECK(facts.printer_model == "Voron 0.2");
        CHECK(facts.nozzle_diameters == std::vector<double>{0.4, 0.6});
        CHECK(facts.max_hotend_temp == std::optional<double>(240.0));
        CHECK(facts.max_bed_temp == std::optional<double>(70.0));
    }
    SECTION("Orca's nozzle flow types, by extruder") {
        const FitHeadFacts facts =
            fit_head_facts("; nozzle_diameter = 0.4,0.4,0.4,0.6\n"
                           "; nozzle_volume_type = Standard,High Flow,Standard,Standard\n");
        CHECK(facts.nozzle_volume_types ==
              std::vector<std::string>{"standard", "high_flow", "standard", "standard"});
        CHECK(fit_head_facts("; nozzle_volume_type = Turbo,Turbo\n").nozzle_volume_types.empty());
        CHECK(fit_head_facts("; nozzle_volume_type = ,High Flow\n").nozzle_volume_types ==
              std::vector<std::string>{"", "high_flow"});
    }
    SECTION("a preset name is never the printer model") {
        CHECK(fit_head_facts("; printer_settings_id = Prusa MK4\n").printer_model.empty());
    }
    SECTION("Cura's header") {
        const FitHeadFacts facts = fit_head_facts(";FLAVOR:Marlin\n"
                                                  ";TARGET_MACHINE.NAME:Creality Ender-3\n"
                                                  ";EXTRUDER_TRAIN.1.NOZZLE.DIAMETER:0.6\n"
                                                  ";EXTRUDER_TRAIN.0.NOZZLE.DIAMETER:0.4\n");
        CHECK(facts.printer_model == "Creality Ender-3");
        CHECK(facts.nozzle_diameters == std::vector<double>{0.4, 0.6});
        CHECK_FALSE(facts.max_hotend_temp.has_value());
    }
    SECTION("a head that declares nothing") {
        const FitHeadFacts facts = fit_head_facts("G28\nG1 X5 Y5\n");
        CHECK(facts.printer_model.empty());
        CHECK(facts.nozzle_diameters.empty());
        CHECK_FALSE(facts.max_bed_temp.has_value());
    }
}

TEST_CASE("the plugin's record wins field by field; the screen's reads fill the rest",
          "[plugin_fitcheck]") {
    FitHeadFacts head;
    head.printer_model = "From Head";
    head.nozzle_diameters = {0.6};
    head.max_hotend_temp = 250.0;
    head.max_bed_temp = 60.0;
    FileMetadata md;
    md.first_layer_extr_temp = 260.0;
    md.first_layer_bed_temp = 55.0;
    md.nozzle_diameter = 0.8;
    const FitBox preview = box(1, 2, 0.2, 100, 110, 30);

    SECTION("no record: head, metadata and preview") {
        const FitFile f = fit_file_from_sources(std::nullopt, &head, md, preview, {0, 2});
        REQUIRE(f.print.has_value());
        CHECK(f.print->max[1] == std::optional<double>(110.0));
        CHECK_FALSE(f.travel.has_value());
        CHECK(f.max_hotend_temp == std::optional<double>(260.0)); // the hotter source
        CHECK(f.max_bed_temp == std::optional<double>(60.0));
        CHECK(f.printer_model == "From Head");
        CHECK(f.nozzle_diameters == std::vector<double>{0.6});
        CHECK(f.tools == std::set<int>{0, 2});
    }
    SECTION("metadata's nozzle when the head names none") {
        head.nozzle_diameters.clear();
        const FitFile f = fit_file_from_sources(std::nullopt, &head, md, std::nullopt, {});
        CHECK(f.nozzle_diameters == std::vector<double>{0.8});
        CHECK_FALSE(f.print.has_value());
    }
    SECTION("metadata's per-extruder nozzles beat its single one, and give the flow types") {
        head.nozzle_diameters.clear();
        md.nozzle_diameter_list = {0.4, 0.6};
        md.filament_volume_types = {"standard", "high_flow"};
        const FitFile f = fit_file_from_sources(std::nullopt, &head, md, std::nullopt, {});
        CHECK(f.nozzle_diameters == std::vector<double>{0.4, 0.6});
        CHECK(f.nozzle_volume_types == std::vector<std::string>{"standard", "high_flow"});

        INFO("the head's flow types answer first; a record's that name none known do not");
        head.nozzle_volume_types = {"high_flow", "high_flow"};
        CHECK(
            fit_file_from_sources(std::nullopt, &head, md, std::nullopt, {}).nozzle_volume_types ==
            head.nozzle_volume_types);
        FitFile record;
        record.nozzle_volume_types = {"", ""};
        CHECK(fit_file_from_sources(record, &head, md, std::nullopt, {}).nozzle_volume_types ==
              head.nozzle_volume_types);
    }
    SECTION("a record answers before every other source") {
        FitFile record;
        record.travel = box(-5, 0, 0, 230, 200, 40);
        record.max_hotend_temp = 210.0;
        record.max_bed_temp = 50.0;
        record.printer_model = "From Record";
        record.nozzle_diameters = {0.4};
        record.tools = {0};
        const FitFile f = fit_file_from_sources(record, &head, md, preview, {});
        CHECK_FALSE(f.print.has_value()); // the preview's box does not stand beside travel
        REQUIRE(f.travel.has_value());
        CHECK(f.travel->min[0] == std::optional<double>(-5.0));
        CHECK(f.max_hotend_temp == std::optional<double>(210.0));
        CHECK(f.max_bed_temp == std::optional<double>(50.0));
        CHECK(f.printer_model == "From Record");
        CHECK(f.nozzle_diameters == std::vector<double>{0.4});
        CHECK(f.tools == std::set<int>{0});
    }
}

TEST_CASE("a record's bounds and header reach the fit check", "[plugin_fitcheck]") {
    const json result = {
        {"status", "ready"},
        {"analysis",
         {{"schema", 1},
          {"filename", "part.gcode"},
          {"tools", json::array({json{{"tool", 1}}, json{{"tool", 0}}})},
          {"bounds",
           {{"print", {{"min", {10.0, 10.0, 0.2}}, {"max", {200.0, 190.0, 20.0}}}},
            {"travel", {{"min", {0.0, -2.0, nullptr}}, {"max", {221.0, 200.0, nullptr}}}},
            {"max_hotend_temp", 245.0},
            {"max_bed_temp", nullptr}}},
          {"header",
           {{"printer_model", "Voron 0.2"},
            {"nozzle_diameters", {0.4, "x"}},
            {"nozzle_volume_types", {"High Flow", 3}},
            {"bed", nullptr},
            {"filament_types", json::array()}}}}}};
    const FileAnalysisReply reply = parse_file_analysis_reply(result);
    REQUIRE(reply.record.has_value());
    const FitFile& fit = reply.record->fit;
    REQUIRE(fit.travel.has_value());
    CHECK(fit.travel->min[1] == std::optional<double>(-2.0));
    CHECK_FALSE(fit.travel->max[2].has_value());
    REQUIRE(fit.print.has_value());
    CHECK(fit.print->max[0] == std::optional<double>(200.0));
    CHECK(fit.max_hotend_temp == std::optional<double>(245.0));
    CHECK_FALSE(fit.max_bed_temp.has_value());
    CHECK(fit.printer_model == "Voron 0.2");
    CHECK(fit.nozzle_diameters == std::vector<double>{0.4});
    CHECK(fit.nozzle_volume_types == std::vector<std::string>{"high_flow", ""});
    CHECK(fit.tools == std::set<int>{0, 1});

    FileStartFacts facts;
    apply_file_analysis(facts, *reply.record);
    REQUIRE(facts.fit_record.has_value());
    PrintStartContext ctx;
    fill_file_context(ctx, facts);
    REQUIRE(ctx.fit_file.travel.has_value());
    CHECK(ctx.fit_file.travel->max[0] == std::optional<double>(221.0));
    const auto findings = check_fit(fit_machine_from_settings(kSettings), ctx.fit_file);
    REQUIRE(findings.size() == 3);
    CHECK((findings[0].axis + findings[0].side) == "xmax");
    CHECK((findings[1].axis + findings[1].side) == "ymin");
    CHECK(findings[2].kind == FitKind::TooManyTools); // tools 0 and 1 on one extruder
}

// ============================================================================
// The gate
// ============================================================================

TEST_CASE("the fit gate follows the printer-stopping check", "[plugin_fitcheck]") {
    const auto& gates = default_print_start_gates();
    const auto at = [&](const char* name) {
        return std::find_if(gates.begin(), gates.end(),
                            [&](const PrintStartGate& g) { return g.name == name; });
    };
    REQUIRE(at("fit_check") != gates.end());
    CHECK(at("fit_check") == at("printer_stopping_command") + 1);
}

TEST_CASE("a blocking finding stops the start and names only what blocks", "[plugin_fitcheck]") {
    FitFile file;
    file.travel = box(0, 0, 0, 230, 100, 20);
    file.nozzle_diameters = {0.6};
    const CheckResult r = gate_fit_check(ctx_for(file));
    CHECK(r.verdict == CheckResult::Verdict::Block);
    CHECK(r.severity == GateSeverity::Error);
    CHECK(r.proceed_label.empty());
    CHECK(r.body.find("230") != std::string::npos);
    CHECK(r.body.find("nozzle") == std::string::npos);
}

TEST_CASE("warnings alone ask before starting", "[plugin_fitcheck]") {
    FitFile file;
    file.nozzle_diameters = {0.6};
    const CheckResult r = gate_fit_check(ctx_for(file));
    CHECK(r.verdict == CheckResult::Verdict::Warn);
    CHECK_FALSE(r.proceed_label.empty());
    CHECK(r.body.find("0.6") != std::string::npos);
}

TEST_CASE("another printer model is shown, never asked about at Print", "[plugin_fitcheck]") {
    PrintStartContext ctx = ctx_for(FitFile{});
    ctx.fit_machine->model = "Voron 0.2";
    ctx.fit_file.printer_model = "Prusa MK4";
    const auto findings = check_fit(*ctx.fit_machine, ctx.fit_file);
    REQUIRE(findings.size() == 1);
    CHECK(findings[0].severity == FitSeverity::Note);
    CHECK(gate_fit_check(ctx).verdict == CheckResult::Verdict::Pass);
    CHECK_FALSE(fit_findings_text(findings, FitSeverity::Note).empty());
    CHECK(fit_worst(findings) == std::optional<FitSeverity>(FitSeverity::Note));

    INFO("beside a warning, the dialog names only the warning");
    ctx.fit_file.nozzle_diameters = {0.6};
    const CheckResult r = gate_fit_check(ctx);
    CHECK(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.body.find("Prusa MK4") == std::string::npos);
    CHECK(fit_worst(check_fit(*ctx.fit_machine, ctx.fit_file)) ==
          std::optional<FitSeverity>(FitSeverity::Warn));
    CHECK_FALSE(fit_worst({}).has_value());
    FitFinding note;
    note.severity = FitSeverity::Note;
    FitFinding block;
    block.severity = FitSeverity::Block;
    CHECK(fit_worst({note, block}) == std::optional<FitSeverity>(FitSeverity::Block));
}

TEST_CASE("a file that fits, or a printer whose config is unread, passes", "[plugin_fitcheck]") {
    FitFile fits;
    fits.travel = box(0, 0, 0, 220, 220, 250);
    fits.max_hotend_temp = 300.0;
    CHECK(gate_fit_check(ctx_for(fits)).verdict == CheckResult::Verdict::Pass);

    FitFile over;
    over.max_hotend_temp = 400.0;
    CHECK(gate_fit_check(ctx_for(over, /*with_machine=*/false)).verdict ==
          CheckResult::Verdict::Pass);
}

// ============================================================================
// The switch
// ============================================================================

TEST_CASE("the hold is a plugin-only switch that acts, homed in Safety & Alerts",
          "[plugin_fitcheck]") {
    const auto it = std::find_if(
        kPluginSwitchFeatures.begin(), kPluginSwitchFeatures.end(),
        [](const PluginSwitchFeature& f) { return std::string(f.name) == kFitCheckFeature; });
    REQUIRE(it != kPluginSwitchFeatures.end());
    CHECK_FALSE(it->has_fallback);
    CHECK(it->acts);
    CHECK(it->min_version == kFitCheckMinVersion);
    CHECK(std::string(it->on_subject) == "plugin_feature_on_fit_check_blocks_autostart");

    CHECK(plugin_switch_mode(false, {}, kFitCheckFeature, kFitCheckMinVersion, false, true) ==
          PluginSwitchMode::NeedsPlugin);
    CHECK_FALSE(plugin_feature_opted_in(json::object(), kFitCheckFeature, it->acts));

    PluginFeatureEntry entry;
    entry.version = 1;
    entry.available = true;
    const auto lines = plugin_feature_lines({{kFitCheckFeature, entry}});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].home == PluginFeatureHome::Safety);
    CHECK(lines[0].label != lines[0].name);
}

// ============================================================================
// The detail view
// ============================================================================

namespace {

/// A real panel over the mock API, the printer's config read, with a file
/// whose start G-code heats past this printer's nozzle limit.
class FitDetailFixture : private PrintSelectGlobalStateReset, public PrintSelectPanelFixture {
  public:
    FitDetailFixture()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Unregistered,
                                  PrintSelectVisit::Immediate, PrintSelectApi::Mock) {
        set_moonraker_api(api_.get());
        PrinterDiscovery hw;
        hw.parse_objects(json{"extruder"});
        hw.parse_fit_limits(kSettings);
        get_printer_state().set_hardware(hw);
        panel_->refresh_files(/*force=*/true);
        drain();
        REQUIRE(panel_->select_file_by_name(file_.name()));
        drain();
        detail = PrintSelectPanelTestAccess::detail_view(*panel_);
        REQUIRE(detail != nullptr);
    }

    ~FitDetailFixture() override {
        get_printer_state().set_hardware(PrinterDiscovery{});
        set_moonraker_api(nullptr);
        drain();
    }

    static int severity() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "detail_fit_check_severity"));
    }

    static std::string text() {
        return lv_subject_get_string(lv_xml_get_subject(nullptr, "detail_fit_check_text"));
    }

    lv_obj_t* find(const char* name) const {
        lv_obj_t* obj = lv_obj_find_by_name(detail->get_widget(), name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    bool shown(const char* name) const {
        return !lv_obj_is_hidden(find(name));
    }

    PlantedGcode file_{"fit_hot.gcode", "",
                       "; printer_model = Voron 0.2\nM104 S320\nM140 S60\nG28\n"};
    ui::PrintSelectDetailView* detail = nullptr;
};

} // namespace

TEST_CASE_METHOD(FitDetailFixture, "the detail view shows a blocking finding and the gate stops it",
                 "[plugin_fitcheck][print_select]") {
    CHECK(severity() == 3);
    CHECK(text().find("320") != std::string::npos);
    // The card shows, with the blocking icon and not the others.
    CHECK(shown("fit_check_card"));
    CHECK(shown("fit_check_block_icon"));
    CHECK_FALSE(shown("fit_check_warn_icon"));
    CHECK_FALSE(shown("fit_check_note_icon"));
    CHECK(std::string(lv_label_get_text(find("fit_check_label"))) == text());
    CHECK(detail->get_fit_file().max_hotend_temp == std::optional<double>(320.0));

    auto* controller = PrintSelectPanelTestAccess::print_controller(*panel_);
    REQUIRE(controller != nullptr);
    controller->set_file(file_.name(), "", {}, "");
    const PrintStartContext ctx = PrintStartControllerTestAccess::gather(*controller);
    REQUIRE(ctx.fit_machine.has_value());
    CHECK(gate_fit_check(ctx).verdict == CheckResult::Verdict::Block);
}

TEST_CASE_METHOD(FitDetailFixture, "without the printer's config the detail view shows nothing",
                 "[plugin_fitcheck][print_select]") {
    get_printer_state().set_hardware(PrinterDiscovery{});
    detail->refresh_fit_check();
    CHECK(severity() == 0);
    CHECK(text().empty());
    CHECK_FALSE(shown("fit_check_card"));
}

namespace {

/// Two files that fit by their own G-code, and the plugin's file analysis
/// answering a record only for the first: one heating past the nozzle limit.
class FitRecordFixture : private PrintSelectGlobalStateReset, public PrintSelectPanelFixture {
  public:
    FitRecordFixture()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Unregistered,
                                  PrintSelectVisit::Immediate, PrintSelectApi::Mock) {
        set_moonraker_api(api_.get());
        PrinterDiscovery hw;
        hw.parse_objects(json{"extruder"});
        hw.parse_fit_limits(kSettings);
        get_printer_state().set_hardware(hw);
        PluginFeatureEntry entry;
        entry.version = kFileAnalysisMinVersion;
        entry.available = true;
        entry.enabled = true;
        auto& plugin = get_printer_state().plugin_status_state();
        plugin.set_installed(true);
        plugin.set_features({{kFileAnalysisFeature, entry}});
        MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, kFileAnalysisRpc,
            [](MoonrakerClientMock*, const json& params,
               helix::SharedCallback<void(const json&)> success_cb,
               helix::SharedCallback<void(const MoonrakerError&)>) -> bool {
                const std::string name = params.value("filename", "");
                json result = {{"status", "pending"}};
                if (name == "fit_rec_hot.gcode") {
                    result = {{"status", "ready"},
                              {"analysis",
                               {{"schema", 1},
                                {"filename", name},
                                {"bounds", {{"max_hotend_temp", 320.0}}}}}};
                }
                success_cb(json{{"result", result}});
                return true;
            });
        panel_->refresh_files(/*force=*/true);
        drain();
        detail = PrintSelectPanelTestAccess::detail_view(*panel_);
        REQUIRE(detail != nullptr);
    }

    ~FitRecordFixture() override {
        auto& plugin = get_printer_state().plugin_status_state();
        plugin.set_features({});
        plugin.set_installed(false);
        get_printer_state().set_hardware(PrinterDiscovery{});
        set_moonraker_api(nullptr);
        drain();
    }

    static int severity() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "detail_fit_check_severity"));
    }

    PlantedGcode hot_{"fit_rec_hot.gcode", "", "G28\n"};
    PlantedGcode cool_{"fit_rec_cool.gcode", "", "G28\n"};
    ui::PrintSelectDetailView* detail = nullptr;
};

} // namespace

TEST_CASE_METHOD(FitRecordFixture,
                 "the detail view reads the plugin's record, and drops it for the next file",
                 "[plugin_fitcheck][print_select]") {
    REQUIRE(panel_->select_file_by_name(hot_.name()));
    drain();
    // Only the record knows this file heats past the limit.
    CHECK(severity() == 3);

    REQUIRE(panel_->select_file_by_name(cool_.name()));
    drain();
    CHECK(severity() == 0);
}
