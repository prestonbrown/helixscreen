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
          " BED_TEMP=60");

    SECTION("an unset field goes out empty, clearing the firmware's value") {
        CHECK(lane_writer::slot_identity_params({}) ==
              " SPOOL_ID= VENDOR= NAME= FILAMENT_ID= BED_TEMP=");
    }
    SECTION("text Klipper would cut at goes out empty") {
        id.name = "Blue #2";
        CHECK(lane_writer::slot_identity_params(id).find(" NAME= ") != std::string::npos);
    }
}
