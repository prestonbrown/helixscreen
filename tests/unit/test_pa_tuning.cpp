// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/pa_tuning.h"

#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::pa;

namespace {

int count_occurrences(const std::string& haystack, const std::string& needle) {
    int n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE("clamp_k bounds K and rejects non-finite input", "[pa_tuning]") {
    CHECK(clamp_k(-0.01f) == kMinK);
    CHECK(clamp_k(0.042f) == Catch::Approx(0.042f));
    CHECK(clamp_k(5.0f) == kMaxK);
    CHECK(clamp_k(std::numeric_limits<float>::quiet_NaN()) == kMinK);
}

TEST_CASE("set_pa_gcode names the extruder and formats four decimals", "[pa_tuning]") {
    CHECK(set_pa_gcode("extruder", 0.042f) ==
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0420");
    CHECK(set_pa_gcode("extruder1", 0.5f) ==
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder1 ADVANCE=0.5000");
    // A runaway value is clamped before it reaches the printer.
    CHECK(set_pa_gcode("extruder", -1.0f) ==
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0000");
}

TEST_CASE("default ramps differ by an order of magnitude between extruder kinds", "[pa_tuning]") {
    const Ramp dd = default_ramp(ExtruderKind::DirectDrive);
    const Ramp bw = default_ramp(ExtruderKind::Bowden);
    CHECK(dd.start_k == 0.0f);
    CHECK(dd.end_k == Catch::Approx(0.1f));
    CHECK(bw.end_k == Catch::Approx(1.0f));
    CHECK(dd.bands == 10);
}

TEST_CASE("tower geometry maps layers to Z and bands", "[pa_tuning]") {
    TowerGeometry g; // 0.25 first, 0.2 layers, 1mm base, 3mm bands
    // 1 first layer + 5 base layers + 10 bands of 15 layers
    CHECK(layer_count(g, 10) == 156);
    CHECK(layer_z(g, 1) == Catch::Approx(0.25f));
    CHECK(layer_z(g, 2) == Catch::Approx(0.45f));
    CHECK(layer_z(g, 156) == Catch::Approx(31.25f));

    CHECK(band_for_layer(g, 10, 1) == -1);
    CHECK(band_for_layer(g, 10, 6) == -1);
    CHECK(band_for_layer(g, 10, 7) == 0);
    CHECK(band_for_layer(g, 10, 21) == 0);
    CHECK(band_for_layer(g, 10, 22) == 1);
    CHECK(band_for_layer(g, 10, 156) == 9);
    // Past the end stays in the last band.
    CHECK(band_for_layer(g, 10, 500) == 9);
}

TEST_CASE("ramp K steps linearly from start to end across the bands", "[pa_tuning]") {
    TowerGeometry g;
    Ramp r{0.02f, 0.11f, 10};
    CHECK(ramp_k_for_layer(g, r, 1) == Catch::Approx(0.02f));
    CHECK(ramp_k_for_layer(g, r, 7) == Catch::Approx(0.02f));
    CHECK(ramp_k_for_layer(g, r, 22) == Catch::Approx(0.03f));
    CHECK(ramp_k_for_layer(g, r, 156) == Catch::Approx(0.11f));

    SECTION("a single band holds start_k") {
        Ramp one{0.05f, 0.5f, 1};
        CHECK(ramp_k_for_layer(g, one, 100) == Catch::Approx(0.05f));
    }
}

TEST_CASE("generated gcode reports layers, heats, and never sets K", "[pa_tuning]") {
    TestPrintParams p;
    p.nozzle_temp = 215;
    p.bed_temp = 65;
    p.material = "PETG";
    const std::string gcode = generate_test_gcode(p);
    const int layers = layer_count(p.geometry, p.bands);

    CHECK(gcode.find("SET_PRESSURE_ADVANCE") == std::string::npos);
    CHECK(gcode.find("SET_PRINT_STATS_INFO TOTAL_LAYER=156") != std::string::npos);
    // Every layer, plus the one past the last that marks the end gcode.
    CHECK(count_occurrences(gcode, "SET_PRINT_STATS_INFO CURRENT_LAYER=") == layers + 1);
    CHECK(gcode.find("SET_PRINT_STATS_INFO CURRENT_LAYER=157\n") != std::string::npos);
    CHECK(gcode.find("SET_PRINT_STATS_INFO CURRENT_LAYER=1\n") != std::string::npos);
    CHECK(gcode.find("SET_PRINT_STATS_INFO CURRENT_LAYER=156\n") != std::string::npos);
    CHECK(gcode.find("M190 S65") != std::string::npos);
    CHECK(gcode.find("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215") != std::string::npos);
    CHECK(gcode.find("TEMPERATURE_WAIT SENSOR=extruder MINIMUM=213") != std::string::npos);
    // No T parameter on a heater command: on lanes it is an error.
    CHECK(gcode.find("M104") == std::string::npos);
    CHECK(gcode.find("M109") == std::string::npos);
    CHECK(gcode.find("G28") != std::string::npos);
    CHECK(gcode.find("; total layer number: 156") != std::string::npos);
    CHECK(gcode.find("; filament_type = PETG") != std::string::npos);
    // Heat goes off at the end.
    CHECK(gcode.rfind("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0") >
          gcode.rfind("CURRENT_LAYER="));
}

TEST_CASE("generated gcode heats the chosen extruder by name before selecting its tool",
          "[pa_tuning]") {
    TestPrintParams p;
    p.tool = 3;
    p.heater = "extruder2";
    const std::string gcode = generate_test_gcode(p);
    const size_t wait = gcode.find("TEMPERATURE_WAIT SENSOR=extruder2");
    const size_t select = gcode.find("\nT3\n");
    REQUIRE(wait != std::string::npos);
    REQUIRE(select != std::string::npos);
    CHECK(wait < select);
    CHECK(gcode.find("SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0") != std::string::npos);

    p.tool = -1;
    const std::string none = generate_test_gcode(p);
    CHECK(none.find("\nT0") == std::string::npos);
    CHECK(none.find("\nT3") == std::string::npos);
}

TEST_CASE("the tower is centred on the bed and stays inside it", "[pa_tuning]") {
    TestPrintParams p;
    p.bed_x_min = 0;
    p.bed_x_max = 300;
    p.bed_y_min = 0;
    p.bed_y_max = 200;
    const std::string gcode = generate_test_gcode(p);
    // Outer wall centreline: 20 - 0.225 from centre (150, 100).
    CHECK(gcode.find("X129.775 Y79.775") != std::string::npos);
    CHECK(gcode.find("X170.225 Y120.225") != std::string::npos);
}

TEST_CASE("the estimate grows with wall speed falling", "[pa_tuning]") {
    TestPrintParams fast;
    TestPrintParams slow;
    slow.wall_speed_mm_s = 50.0f;
    CHECK(estimate_print_seconds(slow) > estimate_print_seconds(fast));
    CHECK(estimate_print_seconds(fast) > 60);
}

TEST_CASE("BandLog merges consecutive layers with one K", "[pa_tuning]") {
    BandLog log;
    log.record(1, 0.0f, 0.25f, 0.0f);
    log.record(2, 0.25f, 0.45f, 0.0f);
    log.record(3, 0.45f, 0.65f, 0.01f);
    log.record(4, 0.65f, 0.85f, 0.01f);
    log.record(5, 0.85f, 1.05f, 0.02f);

    const auto bands = log.bands();
    REQUIRE(bands.size() == 3);
    CHECK(bands[0].first_layer == 1);
    CHECK(bands[0].last_layer == 2);
    CHECK(bands[0].z_low == Catch::Approx(0.0f));
    CHECK(bands[0].z_high == Catch::Approx(0.45f));
    CHECK(bands[1].k == Catch::Approx(0.01f));
    CHECK(bands[1].z_low == Catch::Approx(0.45f));
    CHECK(bands[1].z_high == Catch::Approx(0.85f));
    CHECK(bands[2].first_layer == 5);
}

TEST_CASE("BandLog keeps the latest report per layer and orders by layer", "[pa_tuning]") {
    BandLog log;
    log.record(3, 0.45f, 0.65f, 0.03f);
    log.record(1, 0.0f, 0.25f, 0.01f);
    log.record(3, 0.45f, 0.65f, 0.05f);
    const auto bands = log.bands();
    REQUIRE(bands.size() == 2);
    CHECK(bands[0].first_layer == 1);
    CHECK(bands[1].k == Catch::Approx(0.05f));
    log.record(0, 0.0f, 0.0f, 0.9f);
    CHECK(log.bands().size() == 2);
    log.clear();
    CHECK(log.empty());
}

namespace {

struct SessionRig {
    std::vector<float> sent;
    TuningSession session{[this](float k) { sent.push_back(k); }};
    TowerGeometry g;
    Ramp r{0.0f, 0.09f, 10};

    /// Feed a layer change plus the printer echoing back the last command.
    void layer_and_echo(int layer) {
        session.on_layer(layer);
        if (!sent.empty()) {
            session.on_k_reported(sent.back());
        }
    }
};

} // namespace

TEST_CASE("the ramp sends K only when a layer enters a new band", "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    CHECK(rig.session.state() == TuningSession::State::Starting);
    CHECK(rig.sent.empty());

    rig.session.on_print_started();
    CHECK(rig.session.state() == TuningSession::State::Printing);
    REQUIRE(rig.sent.size() == 1);
    CHECK(rig.sent[0] == Catch::Approx(0.0f));

    for (int layer = 1; layer <= 21; ++layer) {
        rig.layer_and_echo(layer);
    }
    CHECK(rig.sent.size() == 1); // still band 0
    rig.layer_and_echo(22);
    REQUIRE(rig.sent.size() == 2);
    CHECK(rig.sent[1] == Catch::Approx(0.01f));
}

TEST_CASE("a first layer report starts a print that never reported printing",
          "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_layer(1);
    CHECK(rig.session.state() == TuningSession::State::Printing);
    CHECK(rig.session.layer() == 1);
}

TEST_CASE("a nudge takes over from the ramp for the rest of the print", "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    for (int layer = 1; layer <= 25; ++layer) {
        rig.layer_and_echo(layer);
    }
    const size_t before = rig.sent.size();
    rig.session.nudge(0.005f);
    CHECK(rig.session.mode() == TuningSession::Mode::Manual);
    REQUIRE(rig.sent.size() == before + 1);
    CHECK(rig.sent.back() == Catch::Approx(0.015f));

    // Two taps before the printer reports accumulate.
    rig.session.nudge(0.005f);
    CHECK(rig.sent.back() == Catch::Approx(0.02f));
    rig.session.on_k_reported(0.02f);

    // Later bands no longer move K.
    const size_t after = rig.sent.size();
    for (int layer = 26; layer <= 80; ++layer) {
        rig.session.on_layer(layer);
    }
    CHECK(rig.sent.size() == after);

    // A nudge below zero clamps.
    rig.session.nudge(-1.0f);
    CHECK(rig.sent.back() == 0.0f);
}

TEST_CASE("the band log records the K the printer reported, not the plan",
          "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    rig.session.on_k_reported(0.0f);
    for (int layer = 1; layer <= 22; ++layer) {
        rig.session.on_layer(layer);
    }
    // The printer applied something other than what was planned for band 1.
    rig.session.on_k_reported(0.0123f);
    rig.session.on_print_ended();
    REQUIRE(rig.session.state() == TuningSession::State::Picking);
    const auto bands = rig.session.bands();
    REQUIRE(bands.size() == 2);
    CHECK(bands[0].k == Catch::Approx(0.0f));
    CHECK(bands[0].last_layer == 21);
    CHECK(bands[1].k == Catch::Approx(0.0123f));
    CHECK(bands[1].first_layer == 22);
}

TEST_CASE("picking a band applies its K as the result", "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    for (int layer = 1; layer <= 40; ++layer) {
        rig.layer_and_echo(layer);
    }
    rig.session.on_print_ended();
    REQUIRE(rig.session.state() == TuningSession::State::Picking);
    CHECK_FALSE(rig.session.pick_band(7));
    CHECK_FALSE(rig.session.pick_band(-1));
    REQUIRE(rig.session.pick_band(1));
    CHECK(rig.session.state() == TuningSession::State::Done);
    REQUIRE(rig.session.result());
    CHECK(*rig.session.result() == Catch::Approx(0.01f));
    CHECK(rig.sent.back() == Catch::Approx(0.01f));

    // A result is not undone by abandoning afterwards.
    const size_t n = rig.sent.size();
    rig.session.abandon();
    CHECK(rig.sent.size() == n);
}

TEST_CASE("keep current re-sends the K the tower ended on", "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    rig.layer_and_echo(1);
    rig.session.nudge(0.033f);
    rig.session.on_k_reported(0.033f);
    rig.session.on_print_ended();
    // The end gcode put the original back; the printer reports it, but the
    // layer past the last means it is not logged.
    rig.session.on_layer(layer_count(rig.g, rig.r.bands) + 1);
    rig.session.on_k_reported(0.04f);
    const size_t n = rig.sent.size();
    rig.session.keep_current();
    CHECK(rig.session.state() == TuningSession::State::Done);
    CHECK(*rig.session.result() == Catch::Approx(0.033f));
    REQUIRE(rig.sent.size() == n + 1);
    CHECK(rig.sent.back() == Catch::Approx(0.033f));
}

TEST_CASE("abandoning a run without a result restores the original K", "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    rig.layer_and_echo(1);
    rig.session.abandon();
    CHECK(rig.session.state() == TuningSession::State::Idle);
    CHECK(rig.sent.back() == Catch::Approx(0.04f));

    SECTION("an original the printer never reported is not invented") {
        SessionRig unknown;
        unknown.session.start(unknown.g, unknown.r, std::nullopt);
        unknown.session.on_print_started();
        const size_t n = unknown.sent.size();
        unknown.session.abandon();
        CHECK(unknown.sent.size() == n);
    }
    SECTION("a run that never sent anything restores nothing") {
        SessionRig quiet;
        quiet.session.start(quiet.g, quiet.r, 0.04f);
        quiet.session.abandon();
        CHECK(quiet.sent.empty());
    }
}

TEST_CASE("a print that ends before any layer is void", "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    rig.session.on_print_ended();
    CHECK(rig.session.state() == TuningSession::State::Idle);
    // The start K it sent is taken back.
    CHECK(rig.sent.back() == Catch::Approx(0.04f));
}

TEST_CASE("nudges and picks outside their state do nothing", "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.nudge(0.01f);
    CHECK(rig.sent.empty());
    CHECK_FALSE(rig.session.pick_band(0));
    rig.session.keep_current();
    CHECK_FALSE(rig.session.result());
    rig.session.on_layer(5);
    CHECK(rig.session.layer() == 0);
}

TEST_CASE("a saved K applies on spool activation only while idle", "[pa_tuning]") {
    CHECK(should_auto_apply_on_activation(false, 0.04f));
    CHECK_FALSE(should_auto_apply_on_activation(true, 0.04f));
    CHECK_FALSE(should_auto_apply_on_activation(false, std::nullopt));
    CHECK_FALSE(should_auto_apply_on_activation(true, std::nullopt));
}

TEST_CASE("the Moonraker record keeps every filament and extruder it has seen", "[pa_tuning]") {
    SavedK first;
    first.k = 0.04213f;
    first.extruder = "extruder";
    first.material = "PLA";
    first.filament_id = 7;
    first.updated = "2026-10-16T20:00:00Z";
    auto rec = merge_moonraker_record(nullptr, first);
    CHECK(rec["version"] == 1);
    CHECK(rec["last"]["k"].get<double>() == Catch::Approx(0.0421));
    CHECK(rec["filaments"]["7"]["material"] == "PLA");
    CHECK(rec["extruders"]["extruder"]["k"].get<double>() == Catch::Approx(0.0421));

    SavedK second;
    second.k = 0.5f;
    second.extruder = "extruder1";
    rec = merge_moonraker_record(rec, second);
    CHECK(rec["last"]["extruder"] == "extruder1");
    // No filament: nothing new under filaments, and the old entry survives.
    CHECK(rec["filaments"].size() == 1);
    CHECK(rec["filaments"]["7"]["k"].get<double>() == Catch::Approx(0.0421));
    CHECK(rec["extruders"].size() == 2);

    // Garbage in the key is replaced, not merged into.
    CHECK(merge_moonraker_record("junk", second)["last"]["k"].get<double>() == Catch::Approx(0.5));
}

namespace {

MacroParamsLookup lookup(std::map<std::string, std::vector<std::string>> macros) {
    return [macros](const std::string& name) -> std::optional<std::vector<std::string>> {
        auto it = macros.find(name);
        if (it == macros.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

const std::vector<std::string> kStart = {"PRINT_START", "START_PRINT"};
const std::vector<std::string> kEnd = {"PRINT_END", "END_PRINT"};

} // namespace

TEST_CASE("the printer's own start and end macros are picked with their temperature params",
          "[pa_tuning][pa_macros]") {
    auto m = pick_machine_macros(
        kStart, kEnd, lookup({{"START_PRINT", {"BED", "EXTRUDER", "CHAMBER"}}, {"END_PRINT", {}}}));
    CHECK(m.start == "START_PRINT");
    CHECK(m.start_bed_param == "BED");
    CHECK(m.start_nozzle_param == "EXTRUDER");
    CHECK(m.end == "END_PRINT");
    CHECK(start_macro_call(m, 60, 215) == "START_PRINT BED=60 EXTRUDER=215");

    SECTION("the explicit spelling wins when a macro takes both") {
        auto both = pick_machine_macros(
            kStart, kEnd, lookup({{"PRINT_START", {"BED", "BED_TEMP", "EXTRUDER_TEMP"}}}));
        CHECK(both.start_bed_param == "BED_TEMP");
        CHECK(both.end.empty());
    }
    SECTION("a macro with no recognisable temperatures is called bare") {
        auto bare = pick_machine_macros(kStart, kEnd, lookup({{"PRINT_START", {"MESH"}}}));
        CHECK(start_macro_call(bare, 60, 215) == "PRINT_START");
    }
    SECTION("no macro at all, and no lookup, mean no call") {
        CHECK(pick_machine_macros(kStart, kEnd, lookup({})).start.empty());
        CHECK(pick_machine_macros(kStart, kEnd, nullptr).start.empty());
        CHECK(start_macro_call(MachineMacros{}, 60, 215).empty());
    }
}

TEST_CASE("the tower starts through the start macro and ends through the end macro",
          "[pa_tuning][pa_macros]") {
    TestPrintParams p;
    p.bed_temp = 60;
    p.nozzle_temp = 215;
    p.macros = MachineMacros{"PRINT_START", "BED_TEMP", "EXTRUDER_TEMP", "PRINT_END"};
    const std::string gcode = generate_test_gcode(p);
    CHECK(gcode.find("\nPRINT_START BED_TEMP=60 EXTRUDER_TEMP=215\n") != std::string::npos);
    CHECK(gcode.find("\nG28\n") == std::string::npos);
    // The macro heats in its own order: nothing heats before it.
    CHECK(gcode.find("SET_HEATER_TEMPERATURE") > gcode.find("PRINT_START"));
    CHECK(gcode.find("M140 S60") > gcode.find("PRINT_START"));
    CHECK(gcode.find("\nPRINT_END\n") != std::string::npos);
    // The cooldown still follows the end macro.
    CHECK(gcode.rfind("TARGET=0") > gcode.find("\nPRINT_END\n"));

    SECTION("a macro that takes no temperatures gets the heaters started first") {
        p.macros = MachineMacros{"PRINT_START", "", "", ""};
        const std::string bare = generate_test_gcode(p);
        CHECK(bare.find("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215") <
              bare.find("\nPRINT_START\n"));
    }
    SECTION("no start macro homes and heats itself") {
        p.macros = MachineMacros{};
        const std::string plain = generate_test_gcode(p);
        CHECK(plain.find("\nG28\n") != std::string::npos);
        CHECK(plain.find("M140 S60") < plain.find("\nG28\n"));
    }
}

TEST_CASE("the end gcode restores the original K before the end macro", "[pa_tuning]") {
    TestPrintParams p;
    p.heater = "extruder1";
    p.restore_k = 0.045f;
    p.macros.end = "PRINT_END";
    const std::string gcode = generate_test_gcode(p);
    const size_t restore = gcode.find("SET_PRESSURE_ADVANCE EXTRUDER=extruder1 ADVANCE=0.0450");
    REQUIRE(restore != std::string::npos);
    CHECK(restore > gcode.find("CURRENT_LAYER=157"));
    CHECK(restore < gcode.find("\nPRINT_END\n"));

    p.restore_k.reset();
    CHECK(generate_test_gcode(p).find("SET_PRESSURE_ADVANCE") == std::string::npos);
}

TEST_CASE("the printable area keeps the tower on the plate", "[pa_tuning][pa_area]") {
    SECTION("a negative position_min reaching a purge bucket is cut to the plate") {
        AreaSources src;
        src.has_axes = true;
        src.axis_x_min = 0;
        src.axis_x_max = 300;
        src.axis_y_min = -15;
        src.axis_y_max = 305;
        src.has_plate = true;
        src.plate_x_min = 5;
        src.plate_x_max = 295;
        src.plate_y_min = 5;
        src.plate_y_max = 295;
        const auto a = printable_area(src);
        REQUIRE(a);
        CHECK(a->y_min == Catch::Approx(5.0f));
        CHECK(a->x_max == Catch::Approx(295.0f));
    }
    SECTION("a delta uses the square inside its round bed") {
        AreaSources src;
        src.has_axes = true;
        src.axis_x_min = -100;
        src.axis_x_max = 100;
        src.axis_y_min = -100;
        src.axis_y_max = 100;
        src.round_bed = true;
        const auto a = printable_area(src);
        REQUIRE(a);
        CHECK(a->x_max == Catch::Approx(70.71f).epsilon(0.001));
        CHECK(a->y_min == Catch::Approx(-70.71f).epsilon(0.001));
        // The prime line lands inside the circle too.
        TestPrintParams p;
        p.bed_x_min = a->x_min;
        p.bed_x_max = a->x_max;
        p.bed_y_min = a->y_min;
        p.bed_y_max = a->y_max;
        CHECK(generate_test_gcode(p).find("Y-65.711") != std::string::npos);
    }
    SECTION("no axes, no area") {
        CHECK_FALSE(printable_area(AreaSources{}));
    }
}

TEST_CASE("fit_bands merges the closest neighbours and keeps late nudges", "[pa_tuning]") {
    std::vector<Band> bands;
    for (int i = 0; i < 6; ++i) {
        bands.push_back(Band{i * 10 + 1, i * 10 + 10, i * 2.0f, i * 2.0f + 2.0f, 0.01f * i});
    }
    bands.push_back(Band{61, 62, 12.0f, 12.4f, 0.051f}); // a late nudge, close to its neighbour
    bands.push_back(Band{63, 70, 12.4f, 14.0f, 0.090f});
    const auto fitted = fit_bands(bands, 7);
    REQUIRE(fitted.size() == 7);
    // 0.050 and 0.051 merged, taking the taller band's K; the last stays.
    CHECK(fitted[5].k == Catch::Approx(0.05f));
    CHECK(fitted[5].last_layer == 62);
    CHECK(fitted.back().k == Catch::Approx(0.09f));
    CHECK(fit_bands(bands, 100).size() == bands.size());
    CHECK(fit_bands(bands, 0).empty());
}

TEST_CASE("a newly active spool targets the extruder its tool holds", "[pa_tuning]") {
    const std::vector<SpoolOnExtruder> mounted = {{7, "extruder"}, {9, "extruder2"}};
    CHECK(extruder_for_spool(9, mounted, "extruder") == "extruder2");
    CHECK(extruder_for_spool(4, mounted, "extruder1") == "extruder1");
    CHECK(extruder_for_spool(0, {{0, "extruder3"}}, "extruder") == "extruder");
}

TEST_CASE("nothing logs or nudges once the tower is done", "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    rig.layer_and_echo(156);
    rig.session.on_layer(157);
    rig.session.on_k_reported(0.04f);
    const size_t n = rig.sent.size();
    rig.session.nudge(0.01f);
    CHECK(rig.sent.size() == n);
    rig.session.on_print_ended();
    CHECK(rig.session.bands().back().k == Catch::Approx(0.09f));
}

TEST_CASE("the restore arriving ahead of the end marker does not overwrite the top band",
          "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    for (int layer = 1; layer <= 156; ++layer) {
        rig.layer_and_echo(layer);
    }
    // One status frame carries the end gcode's restore and its layer marker,
    // and the extruder is parsed before print_stats: K first, then the layer.
    rig.session.on_k_reported(0.04f);
    rig.session.on_layer(157);
    rig.session.on_print_ended();
    REQUIRE(rig.session.state() == TuningSession::State::Picking);
    CHECK(rig.session.bands().back().k == Catch::Approx(0.09f));
    CHECK(rig.session.ending_k() == Catch::Approx(0.09f));

    SECTION("a nudge back to the original on the top layer is still logged") {
        SessionRig n;
        n.session.start(n.g, n.r, 0.04f);
        n.session.on_print_started();
        n.layer_and_echo(156);
        REQUIRE(n.session.nudge(0.04f - 0.09f));
        n.session.on_k_reported(0.04f);
        CHECK(n.session.bands().back().k == Catch::Approx(0.04f));
    }
}

TEST_CASE("a nudge after the tower is done reports that nothing was sent",
          "[pa_tuning][pa_session]") {
    SessionRig rig;
    rig.session.start(rig.g, rig.r, 0.04f);
    rig.session.on_print_started();
    rig.layer_and_echo(1);
    CHECK(rig.session.nudge(0.001f));
    rig.session.on_layer(157);
    CHECK_FALSE(rig.session.nudge(0.001f));
}
