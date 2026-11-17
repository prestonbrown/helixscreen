// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_unlink_temps.cpp
 * @brief Unlinking an AFC lane from Spoolman puts its temperatures back with
 *        AFC_SET_SPOOL_TEMP, which AFC's empty SET_SPOOL_ID wiped.
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

class AfcUnlinkHelper : public AmsBackendAfc {
  public:
    AfcUnlinkHelper() : AmsBackendAfc(nullptr, nullptr) {
        set_discovered_lanes({"lane1"}, {});
        feed({{"AFC", {{"units", {"Box_Turtle Turtle_1"}}}},
              {"AFC_BoxTurtle Turtle_1",
               {{"lanes", {"lane1"}}, {"extruders", {"extruder"}}, {"hubs", {"Turtle_1"}}}}});
        running_ = true;
    }

    void feed(const nlohmann::json& params) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
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

    bool help_lists(std::initializer_list<const char*> commands) {
        nlohmann::json result = nlohmann::json::object();
        for (const char* c : commands) {
            result[c] = "desc";
        }
        return apply_gcode_help_response(nlohmann::json{{"result", result}});
    }

    /// Where @p prefix first went out, or gcodes.size() when it never did.
    [[nodiscard]] long sent_at(const std::string& prefix) const {
        return std::find_if(gcodes.begin(), gcodes.end(),
                            [&](const std::string& g) { return g.rfind(prefix, 0) == 0; }) -
               gcodes.begin();
    }
};

} // namespace helix

TEST_CASE_METHOD(LVGLTestFixture, "AFC unlink: the lane's temperatures go back after the unlink",
                 "[afc][afc_unlink]") {
    helix::test::RegisteredBackend<AfcUnlinkHelper> reg;
    AfcUnlinkHelper& afc = *reg;
    // AFC filled the lane from Spoolman spool 7.
    afc.feed({{"AFC_stepper lane1",
               {{"spool_id", 7},
                {"material", "PLA"},
                {"color", "#112233"},
                {"extruder_temp", 210},
                {"bed_temp", 70}}}});
    const SlotInfo linked = afc.get_slot_info(0);
    REQUIRE(linked.spoolman_id == 7);
    SlotInfo unlinked = linked;
    unlinked.unlink_as_different_filament(linked);

    SECTION("with AFC_SET_SPOOL_TEMP registered, they follow the empty SET_SPOOL_ID") {
        REQUIRE(afc.help_lists({"AFC_SET_SPOOL_TEMP"}));
        REQUIRE(afc.apply_user_edit(0, unlinked).success());
        const long sent = static_cast<long>(afc.gcodes.size());
        const long unlink = afc.sent_at("SET_SPOOL_ID LANE=lane1 SPOOL_ID=");
        const long temps =
            afc.sent_at("AFC_SET_SPOOL_TEMP LANE=lane1 EXTRUDER_TEMP=210 BED_TEMP=70");
        REQUIRE(unlink < sent);
        CHECK((temps > unlink && temps < sent));
    }
    SECTION("an AFC without the command is not sent it") {
        REQUIRE(afc.help_lists({"AFC_TOOLHEAD_CALIBRATION"}));
        REQUIRE(afc.apply_user_edit(0, unlinked).success());
        CHECK(afc.sent_at("AFC_SET_SPOOL_TEMP") == static_cast<long>(afc.gcodes.size()));
    }
}
