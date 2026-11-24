// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/app_globals.h"
#include "../../include/core/update_queue.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/tune_controller.h"
#include "../lvgl_test_fixture.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class TuneFixture : public LVGLTestFixture {
  public:
    TuneFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
    }

    ~TuneFixture() override {
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    const std::string& last_sent() const {
        return mock_client.last_send_script();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
};

} // namespace

TEST_CASE("clamp_speed_percent bounds to the shipped range", "[tune_controller]") {
    REQUIRE(helix::tune::clamp_speed_percent(10) == 50);
    REQUIRE(helix::tune::clamp_speed_percent(100) == 100);
    REQUIRE(helix::tune::clamp_speed_percent(500) == 200);
}

TEST_CASE("clamp_flow_percent bounds to the shipped range", "[tune_controller]") {
    REQUIRE(helix::tune::clamp_flow_percent(10) == 75);
    REQUIRE(helix::tune::clamp_flow_percent(100) == 100);
    REQUIRE(helix::tune::clamp_flow_percent(500) == 125);
}

TEST_CASE_METHOD(TuneFixture, "set_speed_percent sends a clamped M220", "[tune_controller][mock]") {
    helix::tune::set_speed_percent(api.get(), 500);
    REQUIRE(last_sent() == "M220 S200");
}

TEST_CASE_METHOD(TuneFixture, "set_flow_percent sends a clamped M221", "[tune_controller][mock]") {
    helix::tune::set_flow_percent(api.get(), 10);
    REQUIRE(last_sent() == "M221 S75");
}

TEST_CASE_METHOD(TuneFixture, "an in-range value is sent unmodified", "[tune_controller][mock]") {
    // Guards against a clamp that rewrites everything, which would make the
    // two clamping tests above pass for the wrong reason.
    helix::tune::set_speed_percent(api.get(), 120);
    REQUIRE(last_sent() == "M220 S120");
}

TEST_CASE_METHOD(TuneFixture, "a nonlinear extruder gets no bare ADVANCE=",
                 "[tune_controller][pa_model][mock]") {
    auto& global = get_printer_state();
    global.init_subjects(false);
    global.temperature_state().init_extruders({"extruder"});
    helix::tune::set_pressure_advance(api.get(), "extruder", 0.04f);
    CHECK(last_sent() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0400");

    // Under tanh, ADVANCE= would replace only linear_advance and keep the offset.
    global.update_from_status(
        {{"extruder", {{"pressure_advance_model", "tanh"}, {"linear_advance", 0.02}}}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(helix::tune::pressure_advance_is_nonlinear("extruder"));
    helix::tune::set_pressure_advance(api.get(), "extruder", 0.05f);
    CHECK(last_sent() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0400");
}

TEST_CASE("a null api sends nothing and does not crash", "[tune_controller]") {
    helix::tune::set_speed_percent(nullptr, 120);
    helix::tune::set_flow_percent(nullptr, 120);
    SUCCEED();
}

TEST_CASE("volumetric_flow_mm3_s multiplies extruder velocity by the filament cross-section",
          "[tune_controller][tune_units]") {
    // 5 mm/s of 1.75 mm filament, 2.405 mm^2
    CHECK(helix::tune::volumetric_flow_mm3_s(500, 1.75) == Catch::Approx(12.026).epsilon(1e-3));
    CHECK(helix::tune::volumetric_flow_mm3_s(0, 1.75) == Catch::Approx(0.0));
    // 5 mm/s of 2.85 mm filament, 6.379 mm^2
    CHECK(helix::tune::volumetric_flow_mm3_s(500, 2.85) == Catch::Approx(31.897).epsilon(1e-3));
}

TEST_CASE("status_speed_flow_text shows override percentages or physical units",
          "[tune_controller][tune_units]") {
    SECTION("percent mode ignores the physical inputs") {
        auto text = helix::tune::status_speed_flow_text(false, 120, 95, 150, 500, 1.75);
        CHECK(text.speed == "120%");
        CHECK(text.flow == "95%");
    }
    SECTION("physical mode shows the measured speed as-is and the volumetric flow") {
        // The measured speed already carries the override; scaling it by
        // speed_pct again would read 216 here.
        auto text = helix::tune::status_speed_flow_text(true, 120, 95, 180, 500, 1.75);
        CHECK(text.speed == "180 mm/s");
        CHECK(text.flow == "12.0 mm\xC2\xB3/s");
        CHECK(helix::tune::status_speed_flow_text(true, 120, 95, 180, 500, 2.85).flow ==
              "31.9 mm\xC2\xB3/s");
    }
    SECTION("physical mode with the toolhead still reads zero") {
        auto text = helix::tune::status_speed_flow_text(true, 100, 100, 0, 0, 1.75);
        CHECK(text.speed == "0 mm/s");
        CHECK(text.flow == "0.0 mm\xC2\xB3/s");
    }
}

TEST_CASE("Every live speed readout shares one format", "[tune_controller][tune_units]") {
    CHECK(helix::tune::live_speed_text(187) == "187 mm/s");
    CHECK(helix::tune::status_speed_flow_text(true, 100, 100, 187, 0, 1.75).speed ==
          helix::tune::live_speed_text(187));
}

TEST_CASE("Every live flow readout shares one format", "[tune_controller][tune_units]") {
    CHECK(helix::tune::live_flow_text(12.026) == "12.0 mm\xC2\xB3/s");
    CHECK(helix::tune::live_flow_text(0.0) == "0.0 mm\xC2\xB3/s");
    CHECK(helix::tune::live_flow_text(31.97) == "32.0 mm\xC2\xB3/s");
    CHECK(helix::tune::status_speed_flow_text(true, 100, 100, 187, 500, 1.75).flow ==
          helix::tune::live_flow_text(helix::tune::volumetric_flow_mm3_s(500, 1.75)));
}

TEST_CASE("A retract reads as no flow and never a negative one", "[tune_controller][tune_units]") {
    // 40 mm/s backwards through 1.75 mm filament, and a tiny reverse sample.
    CHECK(helix::tune::volumetric_flow_mm3_s(-4000, 1.75) == 0.0);
    CHECK_FALSE(std::signbit(helix::tune::volumetric_flow_mm3_s(-4000, 1.75)));
    CHECK_FALSE(std::signbit(helix::tune::volumetric_flow_mm3_s(-1, 1.75)));
    CHECK(helix::tune::live_flow_text(helix::tune::volumetric_flow_mm3_s(-1, 2.85)) ==
          "0.0 mm\xC2\xB3/s");
    CHECK(helix::tune::status_speed_flow_text(true, 100, 100, 120, -4000, 2.85).flow ==
          "0.0 mm\xC2\xB3/s");
}

TEST_CASE("The live rates read speed over flow", "[tune_controller][tune_units]") {
    CHECK(helix::tune::live_rates_text(60, 12.026) == "60 mm/s\n12.0 mm\xC2\xB3/s");
    CHECK(helix::tune::live_rates_text(0, 0.0) == "0 mm/s\n0.0 mm\xC2\xB3/s");
    CHECK(helix::tune::live_rates_text(187, 31.9) ==
          helix::tune::live_speed_text(187) + "\n" + helix::tune::live_flow_text(31.9));
}

TEST_CASE_METHOD(TuneFixture, "set_pressure_advance sends a clamped SET_PRESSURE_ADVANCE",
                 "[tune_controller][pa_tune][mock]") {
    helix::tune::set_pressure_advance(api.get(), "extruder1", 0.0425f);
    REQUIRE(last_sent() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder1 ADVANCE=0.0425");
    helix::tune::set_pressure_advance(api.get(), "extruder", 9.0f);
    REQUIRE(last_sent() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=2.0000");
}

TEST_CASE_METHOD(TuneFixture, "the mock applies SET_PRESSURE_ADVANCE per extruder",
                 "[tune_controller][pa_tune][mock]") {
    helix::tune::set_pressure_advance(api.get(), "extruder", 0.031f);
    CHECK(mock_client.pressure_advance_for("extruder").advance == Catch::Approx(0.031));
    // Other extruders keep their own value.
    CHECK(mock_client.pressure_advance_for("extruder1").advance == Catch::Approx(0.04));
}

TEST_CASE("pressure advance readback text comes from the subject", "[tune_controller][pa_tune]") {
    CHECK(helix::tune::pressure_advance_text(-1) == "--");
    CHECK(helix::tune::pressure_advance_text(420) == "0.0420");
    CHECK(helix::tune::pressure_advance_text(0) == "0.0000");
    CHECK_FALSE(helix::tune::pressure_advance_from_subject(-1));
    CHECK(*helix::tune::pressure_advance_from_subject(1234) == Catch::Approx(0.1234f));
}

TEST_CASE_METHOD(TuneFixture, "an unnamed extruder sends no pressure advance",
                 "[tune_controller][pa_tune][mock]") {
    helix::tune::set_speed_percent(api.get(), 120);
    helix::tune::set_pressure_advance(api.get(), "", 0.04f);
    CHECK(last_sent() == "M220 S120");
    helix::tune::set_pressure_advance(nullptr, "extruder", 0.04f);
    CHECK(last_sent() == "M220 S120");
}
