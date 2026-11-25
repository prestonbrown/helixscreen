// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Covers PrinterMotionState parsing of toolhead.axis_minimum / axis_maximum
// into the AxisBounds struct exposed via get_axis_bounds(). The bounds feed
// the jog-clamp soft-stop in MotionPanel::jog().

#include "../helix_test_fixture.h"
#include "../test_helpers/script_lines.h"
#include "belt_settle.h"
#include "printer_motion_state.h"

#include <array>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AxisBounds;
using helix::PrinterMotionState;
using nlohmann::json;

namespace {

// LVGL-free harness — PrinterMotionState's bounds parsing path doesn't touch
// any subjects, so we don't need init_subjects() to exercise it.
class BoundsFixture : public HelixTestFixture {
  public:
    PrinterMotionState state;
};

} // namespace

TEST_CASE_METHOD(BoundsFixture, "AxisBounds populates from toolhead.axis_min/max",
                 "[motion][bounds]") {
    state.init_subjects(false); // no XML registration in test

    json status = {
        {"toolhead",
         {{"axis_minimum", {0.0, 0.0, 0.0, 0.0}}, {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}};
    state.update_from_status(status);

    AxisBounds b = state.get_axis_bounds();
    REQUIRE(b.has_x);
    REQUIRE(b.has_y);
    REQUIRE(b.has_z);
    REQUIRE(b.x_min == Catch::Approx(0.0f));
    REQUIRE(b.x_max == Catch::Approx(235.0f));
    REQUIRE(b.y_min == Catch::Approx(0.0f));
    REQUIRE(b.y_max == Catch::Approx(235.0f));
    REQUIRE(b.z_min == Catch::Approx(0.0f));
    REQUIRE(b.z_max == Catch::Approx(250.0f));
}

TEST_CASE_METHOD(BoundsFixture, "AxisBounds ignores arrays shorter than 3 entries",
                 "[motion][bounds]") {
    state.init_subjects(false);
    json status = {{"toolhead", {{"axis_minimum", {0.0}}, {"axis_maximum", {235.0, 235.0}}}}};
    state.update_from_status(status);
    AxisBounds b = state.get_axis_bounds();
    REQUIRE_FALSE(b.has_x);
    REQUIRE_FALSE(b.has_y);
    REQUIRE_FALSE(b.has_z);
}

TEST_CASE_METHOD(BoundsFixture, "AxisBounds reset on deinit (reconnect path)", "[motion][bounds]") {
    state.init_subjects(false);
    state.update_from_status({{"toolhead",
                               {{"axis_minimum", {-10.0, -10.0, 0.0, 0.0}},
                                {"axis_maximum", {300.0, 300.0, 400.0, 0.0}}}}});
    REQUIRE(state.get_axis_bounds().has_x);

    state.deinit_subjects();
    AxisBounds b = state.get_axis_bounds();
    REQUIRE_FALSE(b.has_x);
    REQUIRE_FALSE(b.has_y);
    REQUIRE_FALSE(b.has_z);
    REQUIRE(b.x_max == Catch::Approx(0.0f)); // explicit zero-out, not stale
}

TEST_CASE_METHOD(BoundsFixture, "AxisBounds tolerates negative envelope (delta/coreXY origins)",
                 "[motion][bounds]") {
    state.init_subjects(false);
    state.update_from_status({{"toolhead",
                               {{"axis_minimum", {-150.0, -150.0, 0.0, 0.0}},
                                {"axis_maximum", {150.0, 150.0, 350.0, 0.0}}}}});
    AxisBounds b = state.get_axis_bounds();
    REQUIRE(b.x_min == Catch::Approx(-150.0f));
    REQUIRE(b.x_max == Catch::Approx(150.0f));
}

// ============================================================================
// G-code space: machine = gcode + homing_origin
// ============================================================================

TEST_CASE_METHOD(BoundsFixture, "to_gcode_space is the identity at zero origin",
                 "[motion][bounds]") {
    AxisBounds machine;
    machine.has_x = true;
    machine.x_min = 0.0f;
    machine.x_max = 350.0f;
    machine.has_y = true;
    machine.y_min = -150.0f;
    machine.y_max = 150.0f;
    machine.has_z = true;
    machine.z_min = 0.0f;
    machine.z_max = 275.0f;

    const AxisBounds g = helix::to_gcode_space(machine, 0.0, 0.0, 0.0);
    CHECK(g.x_min == Catch::Approx(machine.x_min));
    CHECK(g.x_max == Catch::Approx(machine.x_max));
    CHECK(g.y_min == Catch::Approx(machine.y_min));
    CHECK(g.y_max == Catch::Approx(machine.y_max));
    CHECK(g.z_min == Catch::Approx(machine.z_min));
    CHECK(g.z_max == Catch::Approx(machine.z_max));
    CHECK(g.has_x);
    CHECK(g.has_y);
    CHECK(g.has_z);
}

TEST_CASE_METHOD(BoundsFixture, "to_gcode_space shifts by minus the origin, per axis",
                 "[motion][bounds]") {
    AxisBounds machine;
    machine.has_x = true;
    machine.x_min = 0.0f;
    machine.x_max = 350.0f;
    machine.has_y = true;
    machine.y_min = -150.0f;
    machine.y_max = 150.0f;
    machine.has_z = true;
    machine.z_min = 0.0f;
    machine.z_max = 275.0f;

    // Positive origins (U1: homing_origin [-0.0889, -0.016, 0.06]) lower the
    // G-code ceiling to what gcode_position can actually command.
    const AxisBounds u1 = helix::to_gcode_space(machine, -0.0889, -0.016, 0.06);
    CHECK(u1.x_min == Catch::Approx(0.0889f));
    CHECK(u1.x_max == Catch::Approx(350.0889f));
    CHECK(u1.y_min == Catch::Approx(-149.984f));
    CHECK(u1.y_max == Catch::Approx(150.016f));
    CHECK(u1.z_min == Catch::Approx(-0.06f));
    CHECK(u1.z_max == Catch::Approx(274.94f));
    CHECK(u1.has_x);
    CHECK(u1.has_y);
    CHECK(u1.has_z);

    // Negative origins (a saved Z offset below the bed) raise the range.
    const AxisBounds raised = helix::to_gcode_space(machine, 0.0, 0.0, -0.25);
    CHECK(raised.z_min == Catch::Approx(0.25f));
    CHECK(raised.z_max == Catch::Approx(275.25f));
}

TEST_CASE_METHOD(BoundsFixture, "to_gcode_space leaves unknown axes unknown", "[motion][bounds]") {
    AxisBounds machine; // only Z known
    machine.has_z = true;
    machine.z_min = 0.0f;
    machine.z_max = 275.0f;

    const AxisBounds g = helix::to_gcode_space(machine, 1.0, 1.0, 1.0);
    CHECK_FALSE(g.has_x);
    CHECK_FALSE(g.has_y);
    CHECK(g.has_z);
    CHECK(g.z_max == Catch::Approx(274.0f));
}

TEST_CASE_METHOD(BoundsFixture, "gcode bounds shift the machine envelope by homing_origin",
                 "[motion][bounds]") {
    state.init_subjects(false);
    state.update_from_status(
        {{"toolhead",
          {{"axis_minimum", {0.0, 0.0, 0.0, 0.0}}, {"axis_maximum", {235.0, 235.0, 275.0, 0.0}}}}});
    state.update_from_status({{"gcode_move", {{"homing_origin", {-0.0889, -0.016, 0.06, 0.0}}}}});

    const AxisBounds g = state.get_gcode_axis_bounds();
    CHECK(g.has_x);
    CHECK(g.has_y);
    CHECK(g.has_z);
    CHECK(g.x_min == Catch::Approx(0.0889));
    CHECK(g.x_max == Catch::Approx(235.0889));
    CHECK(g.y_max == Catch::Approx(235.016));
    CHECK(g.z_min == Catch::Approx(-0.06));
    CHECK(g.z_max == Catch::Approx(274.94));

    // Machine envelope stays machine: other consumers (bed dimensions, belt
    // tension) compare against toolhead-space values.
    CHECK(state.get_axis_bounds().z_max == Catch::Approx(275.0f));

    // Reconnect resets the origin with the bounds.
    state.deinit_subjects();
    AxisBounds reset = state.get_gcode_axis_bounds();
    CHECK_FALSE(reset.has_z);
    CHECK(reset.x_min == Catch::Approx(0.0)); // origin back to zero
}

TEST_CASE("inset_bounds shrinks each known axis from both ends", "[motion][bounds]") {
    AxisBounds b;
    b.has_x = b.has_y = b.has_z = true;
    b.x_min = 0.0f;
    b.x_max = 235.0f;
    b.y_min = -5.0f;
    b.y_max = 235.0f;
    b.z_min = -0.06f;
    b.z_max = 274.94f;
    const AxisBounds r = helix::inset_bounds(b, 0.02);
    CHECK(r.x_min == Catch::Approx(0.02));
    CHECK(r.x_max == Catch::Approx(234.98));
    CHECK(r.y_min == Catch::Approx(-4.98));
    CHECK(r.z_min == Catch::Approx(-0.04));
    CHECK(r.z_max == Catch::Approx(274.92));
    CHECK(r.has_x);
    CHECK(r.has_z);
}

namespace {

/// A K2 Plus on community firmware just after a move out of its rear service
/// zone: Klipper still reports the widened Y envelope, 380, while the zone
/// guard's status says the print area ends at 352.
json k2_service_zone_frame() {
    return {{"toolhead",
             {{"axis_minimum", {-4.0, -6.2, -3.0, 0.0}},
              {"axis_maximum", {352.5, 380.0, 352.5, 0.0}},
              {"max_velocity", 800.0}}},
            {"extended_zone_transform", {{"standard_y_max", 352.0}, {"in_extended_zone", false}}},
            {"gcode_move", {{"homing_origin", {0.0, 0.0, 0.0, 0.0}}}}};
}

/// Every Y word of a G-code script.
std::vector<float> y_words(const std::string& gcode) {
    std::vector<float> ys;
    for (const auto& line : helix::test::script_lines(gcode)) {
        const auto at = line.find(" Y");
        if (at == std::string::npos) {
            continue;
        }
        const auto y = helix::text_io::parse_leading<float>(std::string_view(line).substr(at + 2));
        REQUIRE(y.has_value());
        ys.push_back(*y);
    }
    return ys;
}

} // namespace

TEST_CASE_METHOD(BoundsFixture, "a service-zone guard caps Y for whole-bed motion",
                 "[motion][bounds]") {
    state.init_subjects(false);
    state.update_from_status(k2_service_zone_frame());

    CHECK(state.get_axis_bounds().y_max == Catch::Approx(352.0f));
    CHECK(state.get_gcode_axis_bounds().y_max == Catch::Approx(352.0f));
    CHECK(state.get_axis_bounds().x_max == Catch::Approx(352.5f));
    // Moves meant for the zone still see the whole travel.
    CHECK(state.get_kinematic_axis_bounds().y_max == Catch::Approx(380.0f));

    // The guard's status is delta-only: later toolhead frames keep the cap.
    state.update_from_status({{"toolhead",
                               {{"axis_minimum", {-4.0, -6.2, -3.0, 0.0}},
                                {"axis_maximum", {352.5, 380.0, 352.5, 0.0}}}}});
    CHECK(state.get_axis_bounds().y_max == Catch::Approx(352.0f));

    // Reconnect forgets it with the bounds.
    state.clear_data();
    state.update_from_status(
        {{"toolhead",
          {{"axis_minimum", {0.0, 0.0, 0.0, 0.0}}, {"axis_maximum", {300.0, 380.0, 300.0, 0.0}}}}});
    CHECK(state.get_axis_bounds().y_max == Catch::Approx(380.0f));
}

TEST_CASE_METHOD(BoundsFixture, "a service-zone cap does not outlive deinit_subjects",
                 "[motion][bounds][2044]") {
    state.init_subjects(false);
    state.update_from_status(k2_service_zone_frame());
    REQUIRE(state.get_axis_bounds().y_max == Catch::Approx(352.0f));

    // The next printer's toolhead envelope arrives without a zone guard's
    // object: nothing of the previous printer's may cap it.
    state.deinit_subjects();
    state.init_subjects(false);
    state.update_from_status(
        {{"toolhead",
          {{"axis_minimum", {0.0, 0.0, 0.0, 0.0}}, {"axis_maximum", {300.0, 380.0, 300.0, 0.0}}}}});
    CHECK(state.get_axis_bounds().y_max == Catch::Approx(380.0f));
}

TEST_CASE_METHOD(BoundsFixture, "without a service-zone guard the Y envelope is Klipper's",
                 "[motion][bounds]") {
    state.init_subjects(false);
    state.update_from_status(
        {{"toolhead",
          {{"axis_minimum", {0.0, 0.0, 0.0, 0.0}}, {"axis_maximum", {350.0, 380.0, 340.0, 0.0}}}}});
    CHECK(state.get_axis_bounds().y_max == Catch::Approx(380.0f));
    CHECK(state.get_gcode_axis_bounds().y_max == Catch::Approx(380.0f));
}

TEST_CASE_METHOD(BoundsFixture,
                 "belt settle laps on a K2 with a service zone stay inside the print area",
                 "[motion][bounds][belt][settle]") {
    using namespace helix::calibration;
    state.init_subjects(false);
    state.update_from_status(k2_service_zone_frame());

    // The mapping BeltTensionPanel::settle_inputs() makes.
    const AxisBounds machine = state.get_axis_bounds();
    const AxisBounds gcode = state.get_gcode_axis_bounds();
    BeltSettleInputs in;
    in.x_min = machine.x_min;
    in.x_max = machine.x_max;
    in.y_min = machine.y_min;
    in.y_max = machine.y_max;
    in.has_xy_limits = machine.has_x && machine.has_y;
    in.probe_point = std::array<float, 3>{175.0f, 175.0f, 20.0f};
    in.origin_x = machine.x_min - gcode.x_min;
    in.origin_y = machine.y_min - gcode.y_min;
    in.origin_z = machine.z_min - gcode.z_min;
    in.max_velocity_mm_s = 800.0f;

    const auto plan = plan_belt_settle(in);
    REQUIRE(plan.refusal == BeltSettleRefusal::NONE);
    const auto ys = y_words(plan.gcode);
    REQUIRE_FALSE(ys.empty());
    for (const float y : ys) {
        CHECK(y <= 352.0f - plan.inset_y_mm + 0.05f);
    }
}
