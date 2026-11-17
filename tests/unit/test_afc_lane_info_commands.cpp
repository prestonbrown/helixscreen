// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_lane_info_commands.cpp
 * @brief A lane edit sends its temperatures (AFC_SET_SPOOL_TEMP) and its vendor and
 *        filament name (SET_FILAMENT_INFO) to AFC when AFC registers those commands.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "test_helpers/registered_backend.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {

class AfcLaneInfoHelper : public AmsBackendAfc {
  public:
    explicit AfcLaneInfoHelper(const std::vector<std::string>& names)
        : AmsBackendAfc(nullptr, nullptr) {
        set_discovered_lanes(names, {});
        nlohmann::json notification;
        notification["params"] =
            nlohmann::json::array({nlohmann::json{{"AFC", {{"units", {"Box_Turtle Turtle_1"}}}},
                                                  {"AFC_BoxTurtle Turtle_1",
                                                   {{"lanes", names},
                                                    {"extruders", {"extruder"}},
                                                    {"hubs", {"Turtle_1"}},
                                                    {"buffers", nlohmann::json::array()}}}},
                                   0.0});
        handle_status_update(notification);
        running_ = true;
    }

    void feed(const nlohmann::json& params) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    /// What HelixScreen's own lane record holds for @p slot.
    [[nodiscard]] const ams::FilamentSlotOverride& record(int slot) const {
        return overrides_.at(slot);
    }

    std::vector<std::string> gcodes;
    AmsError execute_gcode(const std::string& gcode) override {
        gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                           helix::SharedCallback<void(const MoonrakerError&)>,
                           bool = true) override {
        gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    void help_lists(std::initializer_list<const char*> commands) {
        nlohmann::json result = nlohmann::json::object();
        for (const char* c : commands) {
            result[c] = "desc";
        }
        REQUIRE(apply_gcode_help_response(nlohmann::json{{"result", result}}));
    }

    [[nodiscard]] bool sent_starting(const std::string& prefix) const {
        return std::any_of(gcodes.begin(), gcodes.end(),
                           [&](const std::string& g) { return g.rfind(prefix, 0) == 0; });
    }
};

} // namespace helix

namespace {

SlotInfo edited_lane() {
    SlotInfo info;
    info.material = "PLA";
    info.color_rgb = 0x112233;
    info.nozzle_temp_min = 200;
    info.nozzle_temp_max = 220;
    info.bed_temp = 60;
    info.brand = "Overture's";
    info.spool_name = "Matte PLA";
    return info;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC lane edit: temps and vendor go to AFC when it has the commands",
                 "[afc][afc_lane_info]") {
    helix::test::RegisteredBackend<AfcLaneInfoHelper> reg{
        std::vector<std::string>{"lane1", "lane2"}};
    AfcLaneInfoHelper& afc = *reg;
    afc.help_lists({"AFC_SET_SPOOL_TEMP", "SET_FILAMENT_INFO"});

    REQUIRE(afc.apply_user_edit(0, edited_lane()).success());
    CHECK(std::count(afc.gcodes.begin(), afc.gcodes.end(),
                     "AFC_SET_SPOOL_TEMP LANE=lane1 EXTRUDER_TEMP=210 BED_TEMP=60") == 1);
    CHECK(std::count(afc.gcodes.begin(), afc.gcodes.end(),
                     "SET_FILAMENT_INFO LANE=lane1 VENDOR=\"Overture's\" NAME=\"Matte PLA\"") == 1);
    // AFC holds vendor and name now, so HelixScreen's record does not.
    const auto& record = afc.record(0);
    CHECK(record.brand.empty());
    CHECK(record.spool_name.empty());

    SECTION("an edit that changes neither sends neither again") {
        afc.gcodes.clear();
        REQUIRE(afc.apply_user_edit(0, edited_lane()).success());
        CHECK_FALSE(afc.sent_starting("AFC_SET_SPOOL_TEMP"));
        CHECK_FALSE(afc.sent_starting("SET_FILAMENT_INFO"));
    }
    SECTION("a Spoolman-linked lane takes all four from Spoolman") {
        afc.gcodes.clear();
        SlotInfo linked = edited_lane();
        linked.spoolman_id = 7;
        linked.bed_temp = 70;
        linked.brand = "Elegoo";
        REQUIRE(afc.apply_user_edit(1, linked).success());
        CHECK(afc.sent_starting("SET_SPOOL_ID LANE=lane2 SPOOL_ID=7"));
        CHECK_FALSE(afc.sent_starting("AFC_SET_SPOOL_TEMP"));
        CHECK_FALSE(afc.sent_starting("SET_FILAMENT_INFO"));
    }
    SECTION("an unlink sends again what AFC's empty SET_SPOOL_ID wipes") {
        // A lane AFC filled from Spoolman spool 7. Unlinking keeps the vendor and
        // temperatures in the editor, but AFC's clear_values() wipes them.
        afc.feed({{"AFC_stepper lane2",
                   {{"spool_id", 7},
                    {"spool_vendor", "Elegoo"},
                    {"material", "PLA"},
                    {"color", "#112233"},
                    {"extruder_temp", 210},
                    {"bed_temp", 70}}}});
        const SlotInfo linked = afc.get_slot_info(1);
        REQUIRE(linked.spoolman_id == 7);
        SlotInfo unlinked = linked;
        unlinked.unlink_as_different_filament(linked);
        afc.gcodes.clear();
        REQUIRE(afc.apply_user_edit(1, unlinked).success());
        const auto at = [&](const std::string& prefix) {
            return std::find_if(afc.gcodes.begin(), afc.gcodes.end(),
                                [&](const std::string& g) { return g.rfind(prefix, 0) == 0; }) -
                   afc.gcodes.begin();
        };
        const long sent = static_cast<long>(afc.gcodes.size());
        const auto unlink = at("SET_SPOOL_ID LANE=lane2 SPOOL_ID=");
        const auto temps = at("AFC_SET_SPOOL_TEMP LANE=lane2 EXTRUDER_TEMP=210 BED_TEMP=70");
        const auto vendor = at("SET_FILAMENT_INFO LANE=lane2 VENDOR=\"Elegoo\" NAME=");
        REQUIRE(unlink < sent);
        CHECK((temps > unlink && temps < sent));
        CHECK((vendor > unlink && vendor < sent));
    }
    SECTION("a vendor that cannot be sent stays in HelixScreen's record") {
        afc.gcodes.clear();
        SlotInfo info = edited_lane();
        info.brand = "Red #1";
        REQUIRE(afc.apply_user_edit(1, info).success());
        CHECK_FALSE(afc.sent_starting("SET_FILAMENT_INFO"));
        CHECK(afc.record(1).brand == "Red #1");
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC lane edit: an AFC without the commands is sent neither",
                 "[afc][afc_lane_info]") {
    helix::test::RegisteredBackend<AfcLaneInfoHelper> reg{std::vector<std::string>{"lane1"}};
    AfcLaneInfoHelper& afc = *reg;
    afc.help_lists({"AFC_TOOLHEAD_CALIBRATION"});

    REQUIRE(afc.apply_user_edit(0, edited_lane()).success());
    CHECK(afc.sent_starting("SET_MATERIAL LANE=lane1"));
    CHECK_FALSE(afc.sent_starting("AFC_SET_SPOOL_TEMP"));
    CHECK_FALSE(afc.sent_starting("SET_FILAMENT_INFO"));
    CHECK(afc.record(0).brand == "Overture's");
    CHECK(afc.record(0).spool_name == "Matte PLA");

    // Once AFC has the command, the record's vendor and name reach it on the next
    // save even when that save does not change them.
    afc.help_lists({"SET_FILAMENT_INFO"});
    afc.gcodes.clear();
    REQUIRE(afc.apply_user_edit(0, edited_lane()).success());
    CHECK(afc.sent_starting("SET_FILAMENT_INFO LANE=lane1 VENDOR=\"Overture's\""));
    CHECK(afc.record(0).brand.empty());
}
