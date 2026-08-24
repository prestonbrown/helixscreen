// SPDX-License-Identifier: GPL-3.0-or-later
#include "detect_printer_cmd.h"
#include "printer_detector.h"
#include "printer_discovery.h"

#include <algorithm>

#include "catch_amalgamated.hpp"
#include "hv/json.hpp"

TEST_CASE("format_detect_verdict: confident match with runner-up", "[detect_cmd]") {
    PrinterDetectionResult r{"Qidi Q2", 90, "chamber"};
    r.preset = "qidi_q2";
    r.runner_up_type_name = "Qidi Q1 Pro";
    r.runner_up_confidence = 70;
    std::string json = helix::detect::format_detect_verdict(r, "qidi_q1_pro");
    REQUIRE(json.find("\"model\":\"Qidi Q2\"") != std::string::npos);
    REQUIRE(json.find("\"preset\":\"qidi_q2\"") != std::string::npos);
    REQUIRE(json.find("\"confidence\":90") != std::string::npos);
    REQUIRE(json.find("\"runner_up_preset\":\"qidi_q1_pro\"") != std::string::npos);
    REQUIRE(json.find("\"runner_up_confidence\":70") != std::string::npos);
}

TEST_CASE("format_detect_verdict: no preset and no runner-up emit null", "[detect_cmd]") {
    PrinterDetectionResult r{"Mystery Printer", 40, "corexy"};
    std::string json = helix::detect::format_detect_verdict(r, "");
    REQUIRE(json.find("\"preset\":null") != std::string::npos);
    REQUIRE(json.find("\"runner_up_preset\":null") != std::string::npos);
    REQUIRE(json.find("\"runner_up_confidence\":0") != std::string::npos);
}

// Regression: Voron 2.4 returns toolhead.kinematics=null — must not throw.
// Kinematics must be read from configfile.settings.printer.kinematics instead.
TEST_CASE("populate_discovery: null kinematics does not throw", "[detect_cmd]") {
    helix::PrinterDiscovery disc;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "quad_gantry_level"});
    nlohmann::json info = {{"hostname", "voronv2"}};
    // configfile status with kinematics in the static printer config section
    nlohmann::json cfg = {{"configfile",
                           {{"settings",
                             {{"printer", {{"kinematics", "corexy"}}},
                              {"stepper_x", {{"position_min", 0}, {"position_max", 350}}},
                              {"stepper_y", {{"position_min", 0}, {"position_max", 350}}},
                              {"stepper_z", {{"position_max", 340}}}}}}}};
    REQUIRE_NOTHROW(helix::detect::populate_discovery(disc, objects, info, cfg));
    REQUIRE(disc.hostname() == "voronv2");
    REQUIRE(disc.kinematics() == "corexy");
}

// Regression: null/missing fields must all be skipped without throwing.
TEST_CASE("populate_discovery: missing/null fields are skipped safely", "[detect_cmd]") {
    helix::PrinterDiscovery disc;
    nlohmann::json objects = nlohmann::json::array({"extruder"});
    nlohmann::json info = {{"hostname", nullptr}}; // null hostname
    nlohmann::json cfg = {{"configfile",
                           {{"settings",
                             {
                                 {"printer", {{"kinematics", nullptr}}} // null kinematics
                             }}}}};
    REQUIRE_NOTHROW(helix::detect::populate_discovery(disc, objects, info, cfg));
    // No throw is the primary regression guard; hostname/kinematics remain empty.
    REQUIRE(disc.hostname().empty());
    REQUIRE(disc.kinematics().empty());
}

// Regression: an info object that omits "hostname" entirely is distinct from one
// carrying a null hostname. The const json operator[] does not insert on a miss —
// it is only JSON_ASSERT-guarded, so a missing key aborts where asserts are live
// and is undefined behaviour where they are not. Reading via find() is the only
// safe form on a const reference.
TEST_CASE("populate_discovery: info object without a hostname key is safe", "[detect_cmd]") {
    helix::PrinterDiscovery disc;
    nlohmann::json objects = nlohmann::json::array({"extruder"});
    nlohmann::json info = {{"software_version", "v0.12.0"}}; // object, but no hostname
    nlohmann::json cfg = nlohmann::json::object();
    REQUIRE_NOTHROW(helix::detect::populate_discovery(disc, objects, info, cfg));
    REQUIRE(disc.hostname().empty());
}

// Klipper's /printer/info is fetched over the network, so the result is not
// guaranteed to be an object at all. A scalar must be skipped, not indexed:
// the const operator[] throws type_error.305 on a non-object.
TEST_CASE("populate_discovery: non-object info is skipped", "[detect_cmd]") {
    helix::PrinterDiscovery disc;
    nlohmann::json objects = nlohmann::json::array({"extruder"});
    nlohmann::json cfg = nlohmann::json::object();
    REQUIRE_NOTHROW(helix::detect::populate_discovery(disc, objects, nlohmann::json("oops"), cfg));
    REQUIRE(disc.hostname().empty());
}

// ============================================================================
// The object list has to survive parse_objects()
// ============================================================================
//
// parse_objects() classifies the Klipper object list into heaters, sensors,
// fans, macros and so on, but for a long time it kept no copy of the raw list.
// The only writer of printer_objects() was the separate set_printer_objects(),
// which the app's discovery sequence calls and this CLI path does not. Detection
// reads printer_objects() for its object_exists, macro_match and macro_exclude
// heuristics - 73 of the 94 database entries use at least one of those - so
// `--detect-printer` scored blind against the strongest signals in the database,
// and scripts/install.sh seeds a preset from exactly this path.

TEST_CASE("populate_discovery: the raw object list reaches detection", "[detect_cmd][objects]") {
    nlohmann::json info = {{"hostname", "voron"}};
    nlohmann::json cfg = {
        {"configfile", {{"settings", {{"printer", {{"kinematics", "corexy"}}}}}}}};

    helix::PrinterDiscovery with_qgl;
    helix::detect::populate_discovery(
        with_qgl, nlohmann::json::array({"extruder", "heater_bed", "quad_gantry_level"}), info,
        cfg);

    // The list itself must be retained - this is the whole defect.
    REQUIRE_FALSE(with_qgl.printer_objects().empty());
    REQUIRE(std::find(with_qgl.printer_objects().begin(), with_qgl.printer_objects().end(),
                      "quad_gantry_level") != with_qgl.printer_objects().end());

    // And it must actually be scored: quad_gantry_level is a 95-point signal, so
    // the same rig without it cannot score as high. Asserted comparatively so a
    // future database retune does not turn this into a maintenance chore.
    helix::PrinterDiscovery without_qgl;
    helix::detect::populate_discovery(without_qgl,
                                      nlohmann::json::array({"extruder", "heater_bed"}), info, cfg);

    auto hit = PrinterDetector::auto_detect(with_qgl);
    auto miss = PrinterDetector::auto_detect(without_qgl);
    INFO("with QGL: " << hit.type_name << " @" << hit.confidence << " / without: " << miss.type_name
                      << " @" << miss.confidence);
    REQUIRE(hit.confidence > miss.confidence);
}

TEST_CASE("populate_discovery: a macro_exclude heuristic actually excludes",
          "[detect_cmd][objects]") {
    // flashforge_ad5m_pro carries `macro_exclude SUPPORT_FORGE_X`; the ForgeX
    // sibling carries `macro_match SUPPORT_FORGE_X` at 99. A rig running ForgeX
    // must therefore land on the ForgeX entry, and the stock entry must be ruled
    // out entirely. With the object list dropped, neither heuristic can fire and
    // the stock entry wins by default - which is what our own AD5M did.
    nlohmann::json info = {{"hostname", "ad5m-pro"}};
    nlohmann::json cfg = {
        {"configfile", {{"settings", {{"printer", {{"kinematics", "corexy"}}}}}}}};
    nlohmann::json objects = nlohmann::json::array(
        {"extruder", "heater_bed", "mod_params", "gcode_macro SUPPORT_FORGE_X"});

    helix::PrinterDiscovery disc;
    helix::detect::populate_discovery(disc, objects, info, cfg);

    auto result = PrinterDetector::auto_detect(disc);
    INFO("detected " << result.type_name << " @" << result.confidence << " runner-up "
                     << result.runner_up_type_name);
    REQUIRE(result.type_name == "FlashForge Adventurer 5M Pro (ForgeX)");
    REQUIRE(result.type_name != "FlashForge Adventurer 5M Pro");
}
