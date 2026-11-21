// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_classify.h"
#include "error_wording.h"
#include "printer_discovery.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::error_wording::classify;

namespace {

PrinterDiscovery with_objects(std::initializer_list<const char*> names) {
    nlohmann::json objects = nlohmann::json::array();
    for (const char* n : names) {
        objects.push_back(n);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

struct Case {
    const char* line;
    const char* id;
    std::vector<std::string> captures; ///< from capture 1 on
};

void check_cases(const PrinterDiscovery& hw, const std::vector<Case>& cases) {
    for (const auto& c : cases) {
        CAPTURE(c.line);
        const auto got = classify(c.line, &hw);
        REQUIRE(got.has_value());
        CHECK(got->id == c.id);
        CHECK(got->worded);
        for (size_t i = 0; i < c.captures.size(); ++i) {
            REQUIRE(got->captures.size() > i + 1);
            CHECK(got->captures[i + 1] == c.captures[i]);
        }
    }
}

} // namespace

TEST_CASE("ZMOD's messages classify on a ZMOD printer, English and Russian",
          "[error-wording][flashforge]") {
    const PrinterDiscovery hw = with_objects({"zmod_ifs", "zmod_color", "extruder"});
    // Each line is the exact text zmod_ifs.py, zmod_tenz.py or a ZMOD macro raises.
    check_cases(hw,
                {
                    {"Filament jam detected (IFS).", "FF-I15", {}},
                    {"Failed to extract filament from extruder", "FF-I1", {}},
                    {"Не удалось извлечь пруток из экструдера", "FF-I1", {}},
                    {"No filament 3 in IFS", "FF-I2", {"3"}},
                    {"Нет прутка 3 в IFS", "FF-I2", {"3"}},
                    {"Filament 2 stalled in IFS", "FF-I3", {"2"}},
                    {"IFS driver failure", "FF-I6", {}},
                    {"Timeout waiting for response from command F10#12", "FF-I8", {}},
                    {"IFS: Timeout waiting for status 11|5, received 3", "FF-I9", {}},
                    {"Filament runout detected (Head)", "FF-I17", {}},
                    {"Filament runout detected T2", "FF-Z7", {"2"}},
                    {"Filament jam detected T1", "FF-Z8", {"1"}},
                    {"Filament runout or jam detected", "FF-Z5", {}},
                    {"Filament runout detected", "FF-Z6", {}},
                    {"!! Nozzle hit bed or part detachment. Weight 812>600. Z=1.200. PAUSE. "
                     "https://wiki.zmod.link/Global/#nozzle_control",
                     "FF-Z1",
                     {"812", "600", "1.200"}},
                    {"Nozzle hit bed or part detachment. Weight 900>600. Z=0.400. "
                     "FIRMWARE_RESTART. https://wiki.zmod.link/Global/#nozzle_control",
                     "FF-Z2",
                     {"900", "600", "0.400"}},
                    {"Cell Tare: Error. Weight: 120->95 https://wiki.zmod.link/FAQ/", "FF-Z3", {}},
                    {"Resumption interrupted !!! \"extruder\" is not hot enough (1), please "
                     "heat it up and press RESUME again",
                     "FF-Z9",
                     {}},
                    {"Resumption interrupted !!! \"filament_sensor\" has detected that the "
                     "filament has run out, please load it and press RESUME",
                     "FF-Z10",
                     {}},
                    {"File cube.gcode MD5 mismatch. File deleted. Print canceled", "FF-Z13", {}},
                    {"WARNING: Last probe EXCEEDS saved value by 0.210 mm! Use "
                     "SAVE_ZMOD_DATA MESH_TEST=1",
                     "FF-Z15",
                     {}},
                });
}

TEST_CASE("Forge-X and OpenCreator messages classify on their own firmware",
          "[error-wording][flashforge]") {
    check_cases(with_objects({"mod_params", "extruder"}),
                {
                    {"Bed pressure detected: 1530 g.", "FF-F1", {"1530"}},
                    {"Unable to start print. Filament runout detected!", "FF-F6", {}},
                    {"Resume aborted !!! \"extruder\" not hot enough, please heat up again and "
                     "press RESUME",
                     "FF-F8",
                     {}},
                });
    check_cases(with_objects({"creator5_toolchanger", "extruder"}),
                {
                    {"E0163: Paused. T2 clog detected: filament is present but the wheel "
                     "stopped moving. Clear the filament path before resuming.",
                     "FF-C1",
                     {"2"}},
                    {"Conflicting dock/grab sensors; motion blocked", "FF-C3", {}},
                    {"T1 stayed in its dock after 3 grabs; heaters disabled", "FF-C4", {"1", "3"}},
                    {"T0 is not seated in its dock (dock sensor 0, mount sensor 1 at X=10 Y=20 "
                     "after 2 approaches); not releasing it (heaters off). XY may have lost "
                     "steps: re-home before docking",
                     "FF-C5",
                     {"0"}},
                    {"Motion guard: X skipped steps: 4 electrical cycles (16 full steps) so far",
                     "FF-C7",
                     {"X"}},
                });
}

TEST_CASE("a firmware's rows never word another printer's messages",
          "[error-wording][flashforge]") {
    const PrinterDiscovery plain = with_objects({"extruder", "heater_bed"});
    CHECK_FALSE(classify("Failed to extract filament from extruder", &plain).has_value());
    CHECK_FALSE(classify("Bed pressure detected: 1530 g.", &plain).has_value());
    CHECK_FALSE(classify("Failed to extract filament from extruder").has_value());
    // A Forge-X printer is not a ZMOD one.
    const PrinterDiscovery forge_x = with_objects({"mod_params"});
    CHECK_FALSE(classify("Filament jam detected (IFS).", &forge_x).has_value());
}

TEST_CASE("a FlashForge coded error is worded from the prose it carries",
          "[error-wording][flashforge][classify]") {
    ClassifyContext ctx;
    const auto e = helix::error_classify::classify(
        "!! {\"module\": \"estop\", \"status\": \"fail\", \"coded\": \"0201-0000-0003\", \"msg\": "
        "\"No trigger on probe after full movement\"}",
        ctx);
    REQUIRE(e.has_value());
    CHECK(e->code == "0201-0000-0003");
    CHECK_FALSE(e->title.empty());
    CHECK(e->detail.find("probe") != std::string::npos);
}

TEST_CASE("a ZMOD console error reads with its wording", "[error-wording][flashforge][classify]") {
    const PrinterDiscovery hw = with_objects({"zmod_ifs"});
    ClassifyContext ctx;
    ctx.is_printing = true;
    ctx.hw = &hw;
    const auto e = helix::error_classify::classify("!! Filament jam detected (IFS).", ctx);
    REQUIRE(e.has_value());
    CHECK_FALSE(e->title.empty());
    CHECK(e->severity == ErrorSeverity::CRITICAL);
    CHECK(e->raw_detail == "Filament jam detected (IFS).");
}
