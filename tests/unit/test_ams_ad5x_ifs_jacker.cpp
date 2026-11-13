// SPDX-License-Identifier: GPL-3.0-or-later
//
// IFS Jacker (prestonbrown/helixscreen#2039): an RS-485 splitter that makes two
// or more IFS units look like one IFS with 8, 12 or 16 channels. Z-Mod learns
// the count from the device and publishes it as `color_limit` on zmod_ifs and
// zmod_color; every lane command takes the port number past 4 unchanged. The
// AD5X IFS backend follows that count and groups the lanes in units of four,
// as Z-Mod's own AFC panel does.

#include "../lvgl_test_fixture.h"
#include "ad5x_status_parse.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_types.h"
#include "printer_discovery.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsBackendAd5xIfs;
using helix::AmsError;
using helix::AmsErrorHelper;

using json = nlohmann::json;

namespace {

const char* const kLaneHex[] = {"FF0000", "00FF00", "0000FF", "FFFFFF",
                                "112233", "445566", "778899", "AABBCC"};

/// A zmod_color frame for @p lanes ports, the shape zmod_color.get_status()
/// publishes: one slots[] entry per port up to color_limit.
json color_frame(int lanes) {
    json slots = json::array();
    for (int i = 1; i <= lanes; ++i) {
        slots.push_back({{"ID", std::to_string(i)},
                         {"Material", "PLA"},
                         {"HEX", kLaneHex[(i - 1) % 8]},
                         {"hasFilament", true}});
    }
    return json{
        {"zmod_color",
         {{"ifs", true}, {"channel", 0}, {"color_limit", lanes}, {"slots", std::move(slots)}}}};
}

/// A zmod_ifs frame: Ports carries one entry per port.
json ifs_frame(int lanes, int chan, const std::vector<int>& loaded_ports) {
    std::vector<bool> ports(static_cast<size_t>(lanes), false);
    for (int p : loaded_ports) {
        ports[static_cast<size_t>(p - 1)] = true;
    }
    return json{
        {"zmod_ifs", {{"color_limit", lanes}, {"Chan", chan}, {"State", 5}, {"Ports", ports}}}};
}

/// A started backend that records the gcode it sends. No GET_ZCOLOR follow-up
/// query, so a dispatch leaves no debounce task behind.
class CapturingAd5xIfs : public helix::test::StartedBackend<AmsBackendAd5xIfs> {
  public:
    CapturingAd5xIfs() {
        helix::Ad5xIfsTestAccess::set_zcolor_supported(*this, false);
    }

    std::vector<std::string> sent;

    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                           helix::SharedCallback<void(const MoonrakerError&)>, bool) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return true;
    }
    bool sent_line(const std::string& line) const {
        return std::find(sent.begin(), sent.end(), line) != sent.end();
    }
};

} // namespace

TEST_CASE("AD5X IFS takes its lane count from zmod_color.color_limit",
          "[ams][ad5x_ifs][ifs_jacker][2039]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    REQUIRE(backend.get_system_info().total_slots == 4);

    backend.fire_status(color_frame(8));

    const auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 8);
    // Lane 6 carries its own colour: the slots past the fourth are read, not
    // just counted.
    const auto* lane6 = info.get_slot_global(5);
    REQUIRE(lane6 != nullptr);
    CHECK(lane6->color_rgb == 0x445566u);
    CHECK(lane6->material == "PLA");
}

TEST_CASE("AD5X IFS takes its lane count from zmod_ifs alone",
          "[ams][ad5x_ifs][ifs_jacker][2039]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;

    backend.fire_status(ifs_frame(8, 0, {2, 6}));

    REQUIRE(backend.get_system_info().total_slots == 8);
    const auto info = backend.get_system_info();
    CHECK(info.get_slot_global(5)->status == helix::SlotStatus::AVAILABLE);
    CHECK(info.get_slot_global(6)->status == helix::SlotStatus::EMPTY);
    CHECK(info.get_slot_global(1)->status == helix::SlotStatus::AVAILABLE);
}

TEST_CASE("AD5X IFS groups jacker lanes into IFS units of four",
          "[ams][ad5x_ifs][ifs_jacker][2039]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;

    SECTION("eight lanes: two units") {
        backend.fire_status(color_frame(8));
        const auto info = backend.get_system_info();
        REQUIRE(info.units.size() == 2);
        CHECK(info.units[0].name == "IFS 1");
        CHECK(info.units[1].name == "IFS 2");
        CHECK(info.units[0].slot_count == 4);
        CHECK(info.units[1].slot_count == 4);
        CHECK(info.units[1].first_slot_global_index == 4);
    }
    SECTION("twelve lanes: three units") {
        backend.fire_status(color_frame(12));
        const auto info = backend.get_system_info();
        REQUIRE(info.units.size() == 3);
        CHECK(info.units[2].first_slot_global_index == 8);
        CHECK(info.total_slots == 12);
    }
    SECTION("back to four: one unit again, lane 1 keeps its colour") {
        backend.fire_status(color_frame(8));
        backend.fire_status(color_frame(4));
        const auto info = backend.get_system_info();
        REQUIRE(info.units.size() == 1);
        CHECK(info.units[0].name == "IFS");
        CHECK(info.total_slots == 4);
        CHECK(info.get_slot_global(0)->color_rgb == 0xFF0000u);
    }
}

TEST_CASE("AD5X IFS on a plain four-port IFS is unchanged", "[ams][ad5x_ifs][ifs_jacker][2039]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;

    SECTION("color_limit 4") {
        backend.fire_status(color_frame(4));
    }
    SECTION("a frame that does not say") {
        backend.fire_status(json{{"zmod_ifs", {{"Chan", 1}}}});
    }
    SECTION("a limit below four is not a smaller IFS") {
        backend.fire_status(color_frame(2));
    }
    SECTION("a limit past the 16-tool table is clamped") {
        backend.fire_status(json{{"zmod_color", {{"color_limit", 40}}}});
        CHECK(backend.get_system_info().total_slots == 16);
        return;
    }
    const auto info = backend.get_system_info();
    CHECK(info.total_slots == 4);
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].name == "IFS");
}

TEST_CASE("AD5X IFS sends lane 5..8 commands with the port number",
          "[ams][ad5x_ifs][ifs_jacker][2039]") {
    CapturingAd5xIfs backend;
    backend.fire_status(color_frame(8));
    REQUIRE(backend.get_system_info().total_slots == 8);

    SECTION("load lane 6") {
        REQUIRE(backend.load_filament(5).success());
        CHECK(backend.sent == std::vector<std::string>{"INSERT_PRUTOK_IFS PRUTOK=6"});
    }
    SECTION("load lane 8, the last") {
        REQUIRE(backend.load_filament(7).success());
        CHECK(backend.sent_line("INSERT_PRUTOK_IFS PRUTOK=8"));
    }
    SECTION("past the last lane is refused") {
        CHECK_FALSE(backend.load_filament(8).success());
        CHECK(backend.sent.empty());
    }
    SECTION("cold eject of idle lane 7") {
        REQUIRE(backend.eject_lane(6).success());
        CHECK(backend.sent == std::vector<std::string>{"IFS_F24 PRUTOK=7",
                                                       "IFS_F11 PRUTOK=7 LEN=1000 SPEED=1200",
                                                       "IFS_F39 PRUTOK=7"});
    }
    SECTION("tool change to a tool aimed at lane 5") {
        REQUIRE(backend.set_tool_mapping(2, 4).success());
        backend.sent.clear();
        REQUIRE(backend.change_tool(2).success());
        CHECK(backend.sent == std::vector<std::string>{"A_CHANGE_FILAMENT CHANNEL=5"});
        CHECK(backend.get_system_info().tool_to_slot_map.at(2) == 4);
    }
}

TEST_CASE("AD5X IFS on a four-port IFS refuses lane 5", "[ams][ad5x_ifs][ifs_jacker][2039]") {
    CapturingAd5xIfs backend;
    CHECK_FALSE(backend.load_filament(4).success());
    CHECK_FALSE(backend.eject_lane(4).success());
    CHECK(backend.sent.empty());
}

TEST_CASE("AD5X IFS starts a jacker print with ports past four",
          "[ams][ad5x_ifs][ifs_jacker][print_start_params][2039]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array(
        {"zmod", "zmod_color", "zmod_ifs", "AFC", "AFC_IFS IFS_1", "AFC_lane lane1", "extruder"}));
    backend.set_discovery(hw);

    // Before the count arrives, lane 6 is past the IFS and left to the firmware.
    CHECK(backend.print_start_params({{0, 5}}).empty());

    backend.fire_status(color_frame(8));
    CHECK(backend.print_start_params({{0, 5}, {1, 7}}) ==
          helix::PrintStartParams{{"tools", "6,8"}});
    CHECK(backend.print_start_params({{0, 8}}).empty());
}

TEST_CASE("AD5X IFS parsers read ports past four", "[ams][ad5x_ifs][ifs_jacker][2039]") {
    using namespace helix::ad5x;

    SECTION("GET_ZCOLOR lines for lane 12, a two-digit lane") {
        const auto r =
            parse_zcolor_silent({"// Extruder: 12: PETG/112233 | IFS: True", "// 12: PETG/112233"});
        REQUIRE(r.slots[11].has_value());
        CHECK(r.slots[11]->hex == "112233");
        CHECK(r.extruder_slot == 11);
    }
    SECTION("Adventurer5M.json ffmColor7") {
        const auto file = parse_adventurer_json(
            R"({"FFMInfo":{"channel":7,"ffmColor7":"#AABBCC","ffmType7":"ABS"}})");
        REQUIRE(file.has_value());
        REQUIRE(file->ports[6].has_value());
        CHECK(file->ports[6]->hex == "AABBCC");
        CHECK(file->ports[6]->type == "ABS");
    }
    SECTION("zmod_ifs Ports of eight entries, with its color_limit") {
        ZColorSilentResult r;
        read_ifs_status_object(
            json{{"color_limit", 8},
                 {"Ports", json::array({false, false, false, false, false, false, true, false})}},
            r);
        REQUIRE(r.ifs_ports.has_value());
        REQUIRE(r.ifs_ports->size() == 8);
        CHECK((*r.ifs_ports)[6]);
        CHECK(r.port_count == 8);
    }
    SECTION("the lessWaste tools table keeps 5 as its unmapped marker") {
        const auto vars = parse_save_variables(json{{"less_waste_tools", json::array({2, 5, 1})},
                                                    {"less_waste_colors", json::array()}},
                                               "");
        REQUIRE(vars.tools[0].has_value());
        CHECK(*vars.tools[0] == 2);
        CHECK(vars.tools[1] == kUnmappedPort);
        CHECK(*vars.tools[2] == 1);
    }
}
