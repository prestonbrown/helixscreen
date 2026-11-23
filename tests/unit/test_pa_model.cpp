// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_model.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::pa::ModelKind;
using helix::pa::PaModel;
using helix::pa::read_pa_model;
using nlohmann::json;

TEST_CASE("pa_model: classic firmware reports one K and nothing to select", "[pa_model]") {
    const PaModel m = read_pa_model({{"pressure_advance", 0.04}, {"smooth_time", 0.04}}, {});
    CHECK(m.kind == ModelKind::Classic);
    CHECK_FALSE(m.selectable());
    CHECK_FALSE(m.nonlinear());
    REQUIRE(m.primary_value());
    CHECK(*m.primary_value() == Catch::Approx(0.04f));
}

TEST_CASE("pa_model: a selectable firmware on the linear model still reads as K", "[pa_model]") {
    const PaModel m = read_pa_model(
        {{"pressure_advance_model", "linear"}, {"pressure_advance", 0.03}, {"time_offset", 0.0}},
        {});
    CHECK(m.kind == ModelKind::Linear);
    CHECK(m.selectable());
    CHECK_FALSE(m.nonlinear());
    REQUIRE(m.primary_value());
    CHECK(*m.primary_value() == Catch::Approx(0.03f));
}

TEST_CASE("pa_model: a nonlinear model reads linear_advance as its primary value", "[pa_model]") {
    const json status = {{"pressure_advance_model", GENERATE("tanh", "recipr")},
                         {"linear_advance", 0.017},
                         {"nonlinear_offset", 0.12},
                         {"linearization_velocity", 1.0},
                         {"time_offset", 0.0018}};
    const PaModel m = read_pa_model(status, {});
    CHECK(m.nonlinear());
    CHECK(m.selectable());
    REQUIRE(m.primary_value());
    CHECK(*m.primary_value() == Catch::Approx(0.017f));
    REQUIRE(m.nonlinear_offset);
    CHECK(*m.nonlinear_offset == Catch::Approx(0.12f));
    REQUIRE(m.linearization_velocity);
    CHECK(*m.linearization_velocity == Catch::Approx(1.0f));
    REQUIRE(m.time_offset);
    CHECK(*m.time_offset == Catch::Approx(0.0018f));
}

TEST_CASE("pa_model: recipr and tanh are told apart", "[pa_model]") {
    CHECK(read_pa_model({{"pressure_advance_model", "tanh"}}, {}).kind == ModelKind::Tanh);
    CHECK(read_pa_model({{"pressure_advance_model", "recipr"}}, {}).kind == ModelKind::Recipr);
}

TEST_CASE("pa_model: a delta frame keeps what it does not mention", "[pa_model]") {
    PaModel m = read_pa_model(
        {{"pressure_advance_model", "tanh"}, {"linear_advance", 0.02}, {"nonlinear_offset", 0.1}},
        {});
    m = read_pa_model({{"linear_advance", 0.025}}, m);
    CHECK(m.kind == ModelKind::Tanh);
    REQUIRE(m.linear_advance);
    CHECK(*m.linear_advance == Catch::Approx(0.025f));
    REQUIRE(m.nonlinear_offset);
    CHECK(*m.nonlinear_offset == Catch::Approx(0.1f));
}

TEST_CASE("pa_model: switching to nonlinear stops showing the old K", "[pa_model]") {
    PaModel m =
        read_pa_model({{"pressure_advance_model", "linear"}, {"pressure_advance", 0.04}}, {});
    REQUIRE(m.primary_value());

    // The frame that switches the model may not carry linear_advance yet: the
    // readout must go unknown, never keep showing the linear K under the new name.
    m = read_pa_model({{"pressure_advance_model", "tanh"}}, m);
    CHECK(m.nonlinear());
    CHECK_FALSE(m.primary_value());
    CHECK_FALSE(m.k);
}

TEST_CASE("pa_model: switching back to linear drops the nonlinear parameters", "[pa_model]") {
    PaModel m = read_pa_model({{"pressure_advance_model", "tanh"},
                               {"linear_advance", 0.02},
                               {"nonlinear_offset", 0.1},
                               {"linearization_velocity", 1.0}},
                              {});
    m = read_pa_model({{"pressure_advance_model", "linear"}, {"pressure_advance", 0.035}}, m);
    CHECK(m.kind == ModelKind::Linear);
    CHECK_FALSE(m.linear_advance);
    CHECK_FALSE(m.nonlinear_offset);
    CHECK_FALSE(m.linearization_velocity);
    REQUIRE(m.primary_value());
    CHECK(*m.primary_value() == Catch::Approx(0.035f));
}

TEST_CASE("pa_model: an unknown model name leaves the model as it was", "[pa_model]") {
    PaModel m =
        read_pa_model({{"pressure_advance_model", "linear"}, {"pressure_advance", 0.04}}, {});
    m = read_pa_model({{"pressure_advance_model", "quadratic"}}, m);
    CHECK(m.kind == ModelKind::Linear);
    REQUIRE(m.primary_value());
    CHECK(*m.primary_value() == Catch::Approx(0.04f));
}

TEST_CASE("pa_model: switching models at runtime names every parameter", "[pa_model]") {
    helix::pa::NonlinearValues v;
    v.model = ModelKind::Tanh;
    v.linear_advance = 0.017f;
    v.nonlinear_offset = 0.12f;
    v.linearization_velocity = 1.0f;
    v.time_offset = 0.0018f;
    CHECK(helix::pa::set_nonlinear_gcode("extruder", v) ==
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=tanh ADVANCE=0.017000 OFFSET=0.120000 "
          "VELOCITY=1.000000 TIME_OFFSET=0.001800 SMOOTH_TIME=0.020000");
    // A classic kind is never sent as a model name.
    v.model = ModelKind::Linear;
    CHECK(helix::pa::set_nonlinear_gcode("extruder1", v).find("MODEL=tanh") != std::string::npos);
    CHECK(helix::pa::set_linear_gcode("extruder", 0.04f) ==
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=linear ADVANCE=0.040000");
    CHECK(helix::pa::set_linear_gcode("extruder", std::nullopt) ==
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=linear ADVANCE=0.000000");
}

TEST_CASE("pa_model: the values a running nonlinear model reports", "[pa_model]") {
    CHECK_FALSE(helix::pa::nonlinear_values(read_pa_model({{"pressure_advance", 0.04}}, {})));
    // Not every parameter reported yet: nothing to resume from.
    CHECK_FALSE(helix::pa::nonlinear_values(
        read_pa_model({{"pressure_advance_model", "tanh"}, {"linear_advance", 0.02}}, {})));
    const auto v = helix::pa::nonlinear_values(read_pa_model({{"pressure_advance_model", "recipr"},
                                                              {"linear_advance", 0.02},
                                                              {"nonlinear_offset", 0.1},
                                                              {"linearization_velocity", 2.0}},
                                                             {}));
    REQUIRE(v);
    CHECK(v->model == ModelKind::Recipr);
    CHECK(v->linearization_velocity == Catch::Approx(2.0f));
    CHECK(v->time_offset == Catch::Approx(0.0f));
}

TEST_CASE("pa_model: the tower sweep, its command and the value at a height", "[pa_model]") {
    const helix::pa::TowerSweep s{helix::pa::NlParam::Offset, 0.0f, 0.01f};
    CHECK(helix::pa::tuning_tower_gcode(s) ==
          "TUNING_TOWER COMMAND=SET_PRESSURE_ADVANCE PARAMETER=OFFSET START=0.000000 "
          "FACTOR=0.010000");
    CHECK(helix::pa::value_at(s, 27.0f) == Catch::Approx(0.27f));
    CHECK(helix::pa::value_at(s, -3.0f) == Catch::Approx(0.0f));
    CHECK(helix::pa::print_tower_gcode(0.4f, 210, 0) ==
          "PRINT_PA_TOWER NOZZLE=0.40 TARGET_TEMP=210");
    CHECK(helix::pa::print_tower_gcode(0.6f, 245, 150) ==
          "PRINT_PA_TOWER NOZZLE=0.60 TARGET_TEMP=245 FAST_VELOCITY=150");
    CHECK(helix::pa::has_tower_module({"extruder", "pa_test"}));
    CHECK_FALSE(helix::pa::has_tower_module({"extruder", "pa_test_x"}));
}

TEST_CASE("pa_model: saving a nonlinear model comments out pressure_advance", "[pa_model]") {
    helix::pa::NonlinearValues v;
    v.linear_advance = 0.017f;
    v.nonlinear_offset = 0.12f;
    const auto keys = helix::pa::nonlinear_config_keys(v);
    REQUIRE(keys.size() == 7);
    CHECK(keys[0].key == "pressure_advance");
    CHECK(keys[0].comment_out);
    CHECK(keys[1].key == "pressure_advance_model");
    CHECK(keys[1].value == "tanh");
    CHECK(keys[2].value == "0.017000");
    CHECK(keys[6].key == "pressure_advance_smooth_time");
    CHECK(keys[6].value == "0.020000");
    for (size_t i = 1; i < keys.size(); ++i) {
        CHECK_FALSE(keys[i].comment_out);
    }
}
