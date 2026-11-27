// SPDX-License-Identifier: GPL-3.0-or-later
//
// helix::lane_writer: which firmware writes lane_data itself, and how its edit
// command takes a slot's identity.

#include "firmware_lane_writer.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
namespace lane_writer = helix::lane_writer;

namespace {
json plugins_frame(const json& version) {
    return json{{"ad5x_plugins", {{"api_version", version}, {"enabled", json::array()}}}};
}
} // namespace

TEST_CASE("lane_writer reads the firmware's answer from its plugin API version", "[lane_writer]") {
    CHECK(lane_writer::firmware_writes_lane_records(plugins_frame("1.12")) == true);
    CHECK(lane_writer::firmware_writes_lane_records(plugins_frame("1.13")) == true);
    CHECK(lane_writer::firmware_writes_lane_records(plugins_frame("2.0")) == true);
    // Minor versions compare as numbers: 1.9 is older than 1.12.
    CHECK(lane_writer::firmware_writes_lane_records(plugins_frame("1.9")) == false);
    CHECK(lane_writer::firmware_writes_lane_records(plugins_frame("1.11")) == false);

    // A frame that does not carry the version says nothing.
    CHECK_FALSE(lane_writer::firmware_writes_lane_records(json::object()).has_value());
    CHECK_FALSE(lane_writer::firmware_writes_lane_records(
                    json{{"ad5x_plugins", {{"enabled", json::array()}}}})
                    .has_value());
}

TEST_CASE("lane_writer subscribes only where the objects list names the firmware",
          "[lane_writer]") {
    helix::PrinterDiscovery hw;
    hw.set_printer_objects({"toolhead", "save_variables", "ifs", "ifs_materials"});
    CHECK(lane_writer::required_status_objects(hw).empty());
    CHECK_FALSE(lane_writer::firmware_may_write_lane_records(hw));

    hw.set_printer_objects({"toolhead", "save_variables", "ifs", "ifs_materials", "ad5x_plugins"});
    CHECK(lane_writer::required_status_objects(hw) == std::vector<std::string>{"ad5x_plugins"});
    CHECK(lane_writer::firmware_may_write_lane_records(hw));
}

TEST_CASE("lane_writer sends every identity field, quoting text", "[lane_writer]") {
    lane_writer::SlotIdentity id;
    id.spool_id = 42;
    id.vendor = "Polymaker";
    id.name = "Matte PLA, Black";
    id.filament_id = "GFL99";
    id.bed_temp = 60;
    CHECK(lane_writer::slot_identity_params(id) ==
          " SPOOL_ID=42 VENDOR=\"Polymaker\" NAME=\"Matte PLA, Black\" FILAMENT_ID=\"GFL99\""
          " BED_TEMP=60 TEMP=");

    SECTION("an unset field goes out empty, clearing the firmware's value") {
        CHECK(lane_writer::slot_identity_params({}) ==
              " SPOOL_ID= VENDOR= NAME= FILAMENT_ID= BED_TEMP= TEMP=");
    }
    SECTION("characters Klipper would cut at or unquote are dropped, the rest goes out") {
        id.name = "Blue #2; \"Deep\"";
        CHECK(lane_writer::slot_identity_params(id).find(" NAME=\"Blue 2 Deep\" ") !=
              std::string::npos);
        CHECK(lane_writer::sendable_text("  ###  ").empty());
    }
    SECTION("characters the firmware refuses in a value are dropped too") {
        id.vendor = "Bob's";
        id.name = "PLA-CF 15%";
        const std::string params = lane_writer::slot_identity_params(id);
        CHECK(params.find(" VENDOR=\"Bobs\" ") != std::string::npos);
        CHECK(params.find(" NAME=\"PLA-CF 15\" ") != std::string::npos);
    }
    SECTION("the nozzle temperature the user set goes out") {
        id.nozzle_temp = 215;
        CHECK(lane_writer::slot_identity_params(id).find(" BED_TEMP=60 TEMP=215") !=
              std::string::npos);
    }
    SECTION("a nozzle temperature the command would refuse goes out empty") {
        id.nozzle_temp = 120;
        const std::string low = lane_writer::slot_identity_params(id);
        id.nozzle_temp = 400;
        const std::string high = lane_writer::slot_identity_params(id);
        CHECK(low.substr(low.size() - 6) == " TEMP=");
        CHECK(high.substr(high.size() - 6) == " TEMP=");
    }
}

TEST_CASE("lane_writer reads each slot's identity from the firmware's status", "[lane_writer]") {
    const json status{{"ifs_materials",
                       {{"slots",
                         {{"1",
                           {{"type", "PLA"},
                            {"color", "#A03CF7"},
                            {"temp", 220.0},
                            {"spool_id", 7},
                            {"vendor", "Polymaker"},
                            {"name", "Galaxy"},
                            {"filament_id", "OFDSrzZ8"},
                            {"bed_temp", 60.0}}},
                          {"2",
                           {{"type", "PETG"},
                            {"color", "#F80"},
                            {"temp", 250.0},
                            {"spool_id", nullptr},
                            {"vendor", nullptr},
                            {"name", nullptr},
                            {"filament_id", nullptr},
                            {"bed_temp", nullptr}}},
                          {"3", {{"type", "ABS"}, {"color", "#FFFFFF"}, {"temp", 250.0}}}}}}}};
    const auto ids = lane_writer::slot_identities(status);

    REQUIRE(ids.count(0) == 1);
    CHECK(ids.at(0).spool_id == 7);
    CHECK(ids.at(0).vendor == "Polymaker");
    CHECK(ids.at(0).name == "Galaxy");
    CHECK(ids.at(0).filament_id == "OFDSrzZ8");
    CHECK(ids.at(0).bed_temp == 60);
    // Null is unset: the slot is stated, and empty.
    REQUIRE(ids.count(1) == 1);
    CHECK(ids.at(1).spool_id == 0);
    CHECK(ids.at(1).vendor.empty());
    CHECK(ids.at(1).bed_temp == 0);
    // A slot without the fields is a firmware that does not keep them.
    CHECK(ids.count(2) == 0);

    CHECK(lane_writer::slot_identities(json{{"ifs", json::object()}}).empty());
}
