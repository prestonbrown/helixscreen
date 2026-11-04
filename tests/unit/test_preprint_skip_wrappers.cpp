// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preprint_skip_wrappers.cpp
 * @brief The helix_skips.cfg generator and the conflict check that reads it back.
 *
 * Settings are built by loading config text the way Klipper does (sections and
 * option names lowercased, continuation lines stripped, same-named sections
 * merged with the later file winning per option), so ours_intact() is checked
 * against what generate() actually produces once Klipper has parsed it.
 */

#include "bed_mesh_presence.h"
#include "core/text_io.h"
#include "preprint_skip_wrappers.h"
#include "printer_discovery.h"

#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;
namespace sw = helix::skip_wrappers;
using sw::Op;

namespace {

/// Merge config text into a configfile.settings-shaped object.
void load(json& settings, const std::string& text) {
    std::istringstream in(text);
    std::string raw;
    std::string sect;
    std::string opt;
    while (std::getline(in, raw)) {
        std::string line(helix::text_io::trim(raw));
        if (line.empty() || line[0] == '#') {
            continue;
        }
        bool continuation = raw[0] == ' ' || raw[0] == '\t';
        if (continuation && !opt.empty()) {
            std::string v = settings[sect][opt].get<std::string>();
            settings[sect][opt] = v + "\n" + line;
            continue;
        }
        if (line.front() == '[') {
            sect = helix::text_io::to_lower(line.substr(1, line.size() - 2));
            if (!settings.contains(sect)) {
                settings[sect] = json::object();
            }
            opt.clear();
            continue;
        }
        auto colon = line.find(':');
        opt = helix::text_io::to_lower(std::string(helix::text_io::trim(line.substr(0, colon))));
        settings[sect][opt] = std::string(helix::text_io::trim(line.substr(colon + 1)));
    }
}

json printer(std::initializer_list<const char*> sections) {
    json s = json::object();
    for (const char* name : sections) {
        s[name] = json::object();
    }
    return s;
}

/// The Voron 2.4 shape: bed mesh and QGL, no Z-tilt, PRINT_START a user macro.
json voron() {
    json s = printer({"bed_mesh", "quad_gantry_level", "probe", "stepper_z"});
    s["gcode_macro print_start"] = {{"gcode", "\nG28\nQUAD_GANTRY_LEVEL\nBED_MESH_CALIBRATE"}};
    return s;
}

json installed(json s) {
    load(s, sw::generate(sw::wrappable(s)));
    return s;
}

const std::string kKampStyle = "[gcode_macro BED_MESH_CALIBRATE]\n"
                               "rename_existing: _BED_MESH_CALIBRATE\n"
                               "gcode:\n"
                               "    _BED_MESH_CALIBRATE {rawparams}\n";

} // namespace

TEST_CASE("wrappable picks the leveling steps the printer has", "[skip_wrappers]") {
    SECTION("a Voron wraps bed mesh and QGL") {
        CHECK(sw::wrappable(voron()) == std::vector<Op>{Op::BedMesh, Op::Qgl});
    }
    SECTION("z_tilt and z_tilt_ng both wrap Z-tilt") {
        CHECK(sw::wrappable(printer({"z_tilt"})) == std::vector<Op>{Op::ZTilt});
        CHECK(sw::wrappable(printer({"z_tilt_ng"})) == std::vector<Op>{Op::ZTilt});
    }
    SECTION("no bed_mesh section, no bed mesh wrapper") {
        CHECK(sw::wrappable(printer({"quad_gantry_level"})) == std::vector<Op>{Op::Qgl});
    }
    SECTION("settings that are not an object wrap nothing") {
        CHECK(sw::wrappable(json()).empty());
        CHECK(sw::wrappable(json::array()).empty());
    }
}

TEST_CASE("wrappable leaves a user's own macro alone", "[skip_wrappers]") {
    json s = voron();
    SECTION("a user BED_MESH_CALIBRATE macro drops bed mesh, in any case") {
        load(s, "[gcode_macro Bed_Mesh_Calibrate]\ngcode:\n    G28\n");
        CHECK(sw::wrappable(s) == std::vector<Op>{Op::Qgl});
    }
    SECTION("a user BED_MESH_CLEAR macro drops bed mesh too") {
        load(s, "[gcode_macro BED_MESH_CLEAR]\nrename_existing: _BMC\ngcode:\n    _BMC\n");
        CHECK(sw::wrappable(s) == std::vector<Op>{Op::Qgl});
    }
    SECTION("our own loaded wrappers are not a conflict") {
        CHECK(sw::wrappable(installed(s)) == std::vector<Op>{Op::BedMesh, Op::Qgl});
    }
    SECTION("a user macro added after install, merged over ours, drops that step") {
        json merged = installed(s);
        load(merged, kKampStyle);
        CHECK(sw::wrappable(merged) == std::vector<Op>{Op::Qgl});
    }
}

TEST_CASE("ours_intact recognises exactly what generate wrote", "[skip_wrappers]") {
    json s = installed(voron());
    REQUIRE(sw::active(s) == std::vector<Op>{Op::BedMesh, Op::Qgl});
    CHECK(sw::ours_intact(s, Op::BedMesh));
    CHECK(sw::ours_intact(s, Op::Qgl));
    CHECK_FALSE(sw::ours_intact(s, Op::ZTilt));

    SECTION("a user macro that wins gcode and rename_existing is not ours") {
        load(s, kKampStyle);
        CHECK_FALSE(sw::ours_intact(s, Op::BedMesh));
    }
    SECTION("a merge that replaces only the gcode is not ours") {
        load(s, "[gcode_macro QUAD_GANTRY_LEVEL]\ngcode:\n    G28\n");
        CHECK_FALSE(sw::ours_intact(s, Op::Qgl));
    }
    SECTION("a merge that replaces only rename_existing is not ours") {
        load(s, "[gcode_macro QUAD_GANTRY_LEVEL]\nrename_existing: _QGL\n");
        CHECK_FALSE(sw::ours_intact(s, Op::Qgl));
    }
    SECTION("a missing BED_MESH_CLEAR wrapper breaks the bed mesh op") {
        s.erase("gcode_macro bed_mesh_clear");
        CHECK_FALSE(sw::ours_intact(s, Op::BedMesh));
    }
    SECTION("a merge that only adds an option keeps it ours") {
        load(s, "[gcode_macro BED_MESH_CALIBRATE]\nvariable_note: 1\n");
        CHECK(sw::ours_intact(s, Op::BedMesh));
    }
}

TEST_CASE("generate writes one-shot wrappers Klipper can parse", "[skip_wrappers]") {
    CHECK(sw::generate({}).empty());

    const std::string text = sw::generate({Op::BedMesh, Op::Qgl});

    SECTION("each wrapped command renames its built-in and forwards the params") {
        CHECK(text.find("[gcode_macro BED_MESH_CALIBRATE]\n"
                        "rename_existing: _HELIX_BASE_BED_MESH_CALIBRATE\n") != std::string::npos);
        CHECK(text.find("_HELIX_BASE_BED_MESH_CALIBRATE {rawparams}") != std::string::npos);
        CHECK(text.find("_HELIX_BASE_QUAD_GANTRY_LEVEL {rawparams}") != std::string::npos);
        CHECK(text.find("_HELIX_BASE_BED_MESH_CLEAR {rawparams}") != std::string::npos);
        CHECK(text.find("Z_TILT_ADJUST") == std::string::npos);
    }
    SECTION("no command shares a line: ';' would comment out the rest") {
        CHECK(text.find(';') == std::string::npos);
    }
    SECTION("_HELIX_PREP holds one flag per wrapped step and resets each to 1") {
        json s = json::object();
        load(s, text);
        const json& prep = s["gcode_macro _helix_prep"];
        CHECK(prep["variable_run_bed_mesh"] == "1");
        CHECK(prep["variable_run_qgl"] == "1");
        CHECK_FALSE(prep.contains("variable_run_z_tilt"));
        const std::string reset = prep["gcode"].get<std::string>();
        CHECK(reset.find("VARIABLE=run_bed_mesh VALUE=1") != std::string::npos);
        CHECK(reset.find("VARIABLE=run_qgl VALUE=1") != std::string::npos);
    }
    SECTION("a skipped step restores its flag; BED_MESH_CLEAR only guards") {
        json s = json::object();
        load(s, text);
        const std::string cal = s["gcode_macro bed_mesh_calibrate"]["gcode"];
        const std::string clear = s["gcode_macro bed_mesh_clear"]["gcode"];
        CHECK(cal.find("run_bed_mesh|int == 0") != std::string::npos);
        CHECK(cal.find("VARIABLE=run_bed_mesh VALUE=1") != std::string::npos);
        CHECK(clear.find("run_bed_mesh|int == 1") != std::string::npos);
        CHECK(clear.find("VALUE=") == std::string::npos);
    }
}

TEST_CASE("command and flag names", "[skip_wrappers]") {
    CHECK(std::string(sw::command_for(Op::BedMesh)) == "BED_MESH_CALIBRATE");
    CHECK(std::string(sw::command_for(Op::Qgl)) == "QUAD_GANTRY_LEVEL");
    CHECK(std::string(sw::command_for(Op::ZTilt)) == "Z_TILT_ADJUST");
    CHECK(std::string(sw::flag_for(Op::BedMesh)) == "run_bed_mesh");
    CHECK(std::string(sw::flag_for(Op::Qgl)) == "run_qgl");
    CHECK(std::string(sw::flag_for(Op::ZTilt)) == "run_z_tilt");
}

TEST_CASE("active needs the loaded file and an intact wrapper", "[skip_wrappers]") {
    SECTION("nothing is active before the file loads") {
        CHECK(sw::active(voron()).empty());
    }
    SECTION("a wrapper without _HELIX_PREP is not active") {
        json s = installed(voron());
        s.erase("gcode_macro _helix_prep");
        CHECK(sw::active(s).empty());
    }
    SECTION("a user macro merged over one wrapper leaves the other active") {
        json s = installed(voron());
        load(s, kKampStyle);
        CHECK(sw::active(s) == std::vector<Op>{Op::Qgl});
    }
}

TEST_CASE("LoadWatch judges the restart that loads helix_skips.cfg", "[skip_wrappers]") {
    sw::LoadWatch watch;
    using V = sw::LoadWatch::Verdict;

    SECTION("the READY the restart starts from is not an answer") {
        CHECK(watch.feed(true, false) == V::Waiting);
        CHECK(watch.feed(false, false) == V::Waiting);
        CHECK(watch.feed(true, false) == V::Loaded);
    }
    SECTION("a config error after the restart fails the load") {
        CHECK(watch.feed(true, false) == V::Waiting);
        CHECK(watch.feed(false, false) == V::Waiting);
        CHECK(watch.feed(false, true) == V::Failed);
    }
    SECTION("an error reported straight away fails it too") {
        CHECK(watch.feed(false, true) == V::Failed);
    }
}

TEST_CASE("update_gates folds full and delta status frames", "[skip_wrappers]") {
    sw::Gates g;

    SECTION("a probed mesh opens the bed mesh gate and an empty one closes it") {
        CHECK(sw::update_gates(g, {{"bed_mesh", {{"probed_matrix", {{0.1, 0.2}}}}}}));
        CHECK(g.mesh_loaded);
        CHECK(
            sw::update_gates(g, {{"bed_mesh", {{"probed_matrix", json::array({json::array()})}}}}));
        CHECK_FALSE(g.mesh_loaded);
    }
    SECTION("a frame without the field keeps the value") {
        sw::update_gates(g, {{"bed_mesh", {{"probed_matrix", {{0.1}}}}}});
        CHECK_FALSE(sw::update_gates(g, {{"bed_mesh", {{"profile_name", "default"}}}}));
        CHECK(g.mesh_loaded);
    }
    SECTION("applied comes from quad_gantry_level, z_tilt or z_tilt_ng") {
        CHECK(sw::update_gates(g, {{"quad_gantry_level", {{"applied", true}}}}));
        CHECK(g.qgl_applied);
        CHECK(sw::update_gates(g, {{"z_tilt_ng", {{"applied", true}}}}));
        CHECK(g.z_tilt_applied);
        CHECK(sw::update_gates(g, {{"z_tilt", {{"applied", false}}}}));
        CHECK_FALSE(g.z_tilt_applied);
    }
    SECTION("a run flag of 0 is a pending skip until it reads 1 again") {
        CHECK(sw::update_gates(g, {{"gcode_macro _HELIX_PREP", {{"run_qgl", 0}}}}));
        CHECK(sw::any_skip_pending(g));
        CHECK_FALSE(sw::update_gates(g, {{"gcode_macro _HELIX_PREP", {{"run_bed_mesh", 1}}}}));
        CHECK(sw::any_skip_pending(g));
        CHECK(sw::update_gates(g, {{"gcode_macro _HELIX_PREP", {{"run_qgl", 1}}}}));
        CHECK_FALSE(sw::any_skip_pending(g));
    }
    SECTION("a non-object frame changes nothing") {
        CHECK_FALSE(sw::update_gates(g, json::array()));
    }
}

TEST_CASE("offerable opens each toggle on its own gate", "[skip_wrappers]") {
    const std::vector<Op> all = {Op::BedMesh, Op::Qgl, Op::ZTilt};
    sw::Gates g;
    CHECK(sw::offerable(all, g).empty());
    g.mesh_loaded = true;
    CHECK(sw::offerable(all, g) == std::vector<Op>{Op::BedMesh});
    g.qgl_applied = true;
    g.z_tilt_applied = true;
    CHECK(sw::offerable(all, g) == all);
    CHECK(sw::offerable({Op::Qgl}, g) == std::vector<Op>{Op::Qgl});
}

TEST_CASE("status_fields subscribes what the gates read", "[skip_wrappers]") {
    CHECK(sw::status_fields({}, false).empty());

    const json f = sw::status_fields({Op::BedMesh, Op::Qgl, Op::ZTilt}, false);
    CHECK(f["quad_gantry_level"] == json::array({"applied"}));
    CHECK(f["z_tilt_ng"] == json::array({"applied"}));
    CHECK_FALSE(f.contains("z_tilt"));
    CHECK(f["gcode_macro _HELIX_PREP"] == json::array({"run_bed_mesh", "run_qgl", "run_z_tilt"}));

    CHECK(sw::status_fields({Op::ZTilt}, true).contains("z_tilt"));
}

TEST_CASE("option_for renders the flag line for both toggle states", "[skip_wrappers]") {
    const PrePrintOption opt = sw::option_for(Op::Qgl);
    CHECK(opt.id == "qgl");
    CHECK(opt.default_enabled);
    CHECK(sw::is_wrapper_option(opt));
    CHECK(render_pre_start_gcode(opt, false) ==
          "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_qgl VALUE=0");
    CHECK(render_pre_start_gcode(opt, true) ==
          "SET_GCODE_VARIABLE MACRO=_HELIX_PREP VARIABLE=run_qgl VALUE=1");
    CHECK(sw::option_for(Op::BedMesh).id == "bed_mesh");
    CHECK(sw::option_for(Op::ZTilt).id == "z_tilt");

    PrePrintOption plain;
    plain.id = "qgl";
    CHECK_FALSE(sw::is_wrapper_option(plain));
}

TEST_CASE("probed_matrix_has_mesh reads Klipper's empty mesh as absent",
          "[bed_mesh][skip_wrappers]") {
    CHECK_FALSE(helix::probed_matrix_has_mesh(json::array({json::array()}))); // Klipper, no mesh
    CHECK_FALSE(helix::probed_matrix_has_mesh(json::array()));
    CHECK_FALSE(helix::probed_matrix_has_mesh(json()));
    CHECK(helix::probed_matrix_has_mesh(json::array({json::array({0.1, 0.2})})));
}

TEST_CASE("a cleared discovery forgets the skip wrappers", "[skip_wrappers]") {
    helix::PrinterDiscovery hw;
    hw.set_skip_wrappers({Op::Qgl}, {Op::Qgl});
    hw.clear();
    CHECK(hw.skip_wrappable().empty());
    CHECK(hw.skip_active().empty());
}
