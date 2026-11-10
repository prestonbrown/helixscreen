// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_zoffset.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/bed_mesh_panel_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/zoffset_calibration_test_access.h"
#include "app_globals.h"
#include "calibration_owned_heat.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "printer_state.h"
#include "subject_initializer.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::CalibrationOwnedHeat;
using helix::owned_heat_may_clear;
using json = nlohmann::json;

namespace {

class OwnedHeatFixture : public LVGLTestFixture {
  public:
    std::vector<std::string> offs;
    CalibrationOwnedHeat heat{"test", [this](const std::string& h) { offs.push_back(h); }};

    OwnedHeatFixture() {
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        set_link(true);
    }

    static void set_link(bool up) {
        lv_subject_set_int(
            get_printer_state().network_state().get_printer_connection_state_subject(),
            static_cast<int>(up ? helix::ConnectionState::CONNECTED
                                : helix::ConnectionState::DISCONNECTED));
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static void set_bed_target(double target) {
        get_printer_state().update_from_status(
            {{"heater_bed", {{"temperature", 24.0}, {"target", target}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static void set_job(const char* wire) {
        get_printer_state().update_from_status({{"print_stats", {{"state", wire}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

} // namespace

TEST_CASE("owned heat may be cleared only while it is still the run's", "[heater_hold]") {
    CHECK(owned_heat_may_clear(60, 60, false));
    CHECK(owned_heat_may_clear(60, 0, false));  // our send, unreported yet
    CHECK(owned_heat_may_clear(0, 200, false)); // firmware chose the target
    CHECK_FALSE(owned_heat_may_clear(60, 80, false));
    CHECK_FALSE(owned_heat_may_clear(60, 60, true));
    CHECK_FALSE(owned_heat_may_clear(0, 0, true));
}

TEST_CASE_METHOD(OwnedHeatFixture, "owned heat holds only what it will clear", "[heater_hold]") {
    REQUIRE_FALSE(heat.held());

    SECTION("owning takes the hold and turning off releases it") {
        heat.own("heater_bed", 60);
        CHECK(heat.held());
        CHECK(helix::CleanupHold::any_held_signal_safe());
        heat.turn_off();
        CHECK(offs == std::vector<std::string>{"heater_bed"});
        CHECK_FALSE(heat.held());
        CHECK_FALSE(heat.any());
    }

    SECTION("a heater that already had a target is cleared on the normal ending only") {
        heat.own("heater_bed", 60, /*hold=*/false);
        CHECK_FALSE(heat.held());
        heat.before_disconnect();
        CHECK(offs.empty());
    }

    SECTION("the same heater cleared twice sends once") {
        heat.own("heater_bed", 60);
        heat.turn_off();
        heat.turn_off();
        CHECK(offs.size() == 1);
    }

    SECTION("releasing one heater leaves the other owned") {
        heat.own("heater_bed", 60);
        heat.own("extruder", 200);
        heat.release("heater_bed");
        CHECK_FALSE(heat.owns("heater_bed"));
        CHECK(heat.owns("extruder"));
        CHECK(heat.held());
        heat.turn_off();
        CHECK(offs == std::vector<std::string>{"extruder"});
    }

    SECTION("adopting a heater takes the new run's hold") {
        heat.own("heater_bed", 60, /*hold=*/false);
        heat.own("heater_bed", 60);
        CHECK(heat.held());
        heat.before_disconnect();
        CHECK(offs == std::vector<std::string>{"heater_bed"});
    }

    SECTION("firmware takes over a target that is still the run's") {
        heat.own("heater_bed", 60);
        set_bed_target(60.0);
        heat.let_firmware_set("heater_bed");
        set_bed_target(75.0);
        heat.turn_off();
        CHECK(offs == std::vector<std::string>{"heater_bed"});
    }

    SECTION("firmware never takes over a target someone else set") {
        heat.own("heater_bed", 60);
        set_bed_target(80.0);
        heat.let_firmware_set("heater_bed");
        heat.turn_off();
        CHECK(offs.empty());
    }

    SECTION("forgetting drops the heat without a send") {
        heat.own("heater_bed", 60);
        heat.forget();
        CHECK(offs.empty());
        CHECK_FALSE(heat.held());
    }

    SECTION("a job that has started keeps its bed") {
        heat.own("heater_bed", 60);
        set_job("printing");
        heat.turn_off();
        CHECK(offs.empty());
        CHECK_FALSE(heat.held());
    }

    SECTION("disconnecting with the link already down sends nothing and lets go") {
        heat.own("heater_bed", 60);
        set_link(false);
        heat.before_disconnect();
        CHECK(offs.empty());
        CHECK_FALSE(heat.held());
    }
}

TEST_CASE_METHOD(OwnedHeatFixture, "an off the link was down for waits for the reconnect",
                 "[heater_hold]") {
    heat.own("heater_bed", 60);
    set_bed_target(60.0);
    set_link(false);
    heat.turn_off();
    REQUIRE(heat.owes_off("heater_bed"));
    REQUIRE(offs.empty());
    CHECK(heat.held());
    CHECK_FALSE(heat.settle_timer_armed());

    SECTION("nothing goes out while the link is still down") {
        heat.flush_owed();
        CHECK(offs.empty());
        CHECK(heat.owes_off("heater_bed"));
    }

    SECTION("it goes out after the reconnect settles") {
        set_link(true);
        REQUIRE(heat.settle_timer_armed());
        heat.flush_owed();
        CHECK(offs == std::vector<std::string>{"heater_bed"});
        CHECK_FALSE(heat.held());
        CHECK_FALSE(heat.settle_timer_armed());
    }

    SECTION("a target set while the link was down is left alone") {
        set_link(true);
        set_bed_target(90.0);
        heat.flush_owed();
        CHECK(offs.empty());
        CHECK_FALSE(heat.held());
    }

    SECTION("a new run adopts the heat and owes nothing") {
        heat.own("heater_bed", 60);
        CHECK_FALSE(heat.owes_off("heater_bed"));
        CHECK(heat.held());
    }

    SECTION("disconnecting after the reconnect sends the owed off") {
        set_link(true);
        heat.before_disconnect();
        CHECK(offs == std::vector<std::string>{"heater_bed"});
        CHECK_FALSE(heat.settle_timer_armed());
    }
}

TEST_CASE_METHOD(OwnedHeatFixture, "teardown gives both calibration panels their turn",
                 "[heater_hold]") {
    auto& mesh = helix::ui::BedMeshPanelTestAccess::heat(get_global_bed_mesh_panel());
    auto& zoffset = ZOffsetCalibrationTestAccess::heat(get_global_zoffset_cal_panel());
    mesh.own("heater_bed", 60);
    zoffset.own("heater_bed", 45);
    REQUIRE(mesh.held());
    REQUIRE(zoffset.held());

    SubjectInitializer initializer;
    initializer.before_disconnect();

    CHECK_FALSE(mesh.held());
    CHECK_FALSE(zoffset.held());
    CHECK_FALSE(helix::CleanupHold::any_held_signal_safe());
}
