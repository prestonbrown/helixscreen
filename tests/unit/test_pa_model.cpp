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
