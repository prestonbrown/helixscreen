// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_nonlinear_tuning.h"

#include "../catch_amalgamated.hpp"

using namespace helix::pa;

TEST_CASE("nonlinear procedure, direct drive: offset, then advance, then time offset",
          "[pa_nonlinear]") {
    NonlinearProcedure p(ExtruderKind::DirectDrive, ModelKind::Tanh);
    REQUIRE(p.pass_count() == 3);
    CHECK(p.values().linearization_velocity == Catch::Approx(1.0f));

    REQUIRE(p.current().sweep.param == NlParam::Offset);
    CHECK(p.current().judge == TowerSide::Left);
    CHECK(p.current().sweep.factor == Catch::Approx(0.01f));
    p.record_height(12.0f);
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.12f));

    REQUIRE(p.current().sweep.param == NlParam::Advance);
    CHECK(p.current().judge == TowerSide::LeftVsFront);
    // Left best below the front: too much offset, so it drops 10% and the
    // advance tower prints again.
    p.record_verdict(SideVerdict::LeftLower, 30.0f);
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.108f));
    CHECK(p.pass_number() == 2);
    CHECK(p.repeats() == 1);
    p.record_verdict(SideVerdict::LeftHigher, 30.0f);
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.1188f));
    p.record_verdict(SideVerdict::Same, 17.0f);
    CHECK(p.values().linear_advance == Catch::Approx(0.017f));

    REQUIRE(p.current().sweep.param == NlParam::TimeOffset);
    CHECK(p.current().judge == TowerSide::Front);
    CHECK(p.repeats() == 0);
    p.record_height(18.0f);
    CHECK(p.values().time_offset == Catch::Approx(0.0018f));
    CHECK(p.done());
}

TEST_CASE("nonlinear procedure, Bowden: advance kept at 80%, offset checked twice",
          "[pa_nonlinear]") {
    NonlinearProcedure p(ExtruderKind::Bowden, ModelKind::Recipr);
    REQUIRE(p.pass_count() == 4);
    CHECK(p.values().model == ModelKind::Recipr);
    CHECK(p.values().linearization_velocity == Catch::Approx(2.0f));

    REQUIRE(p.current().sweep.param == NlParam::Advance);
    CHECK(p.current().judge == TowerSide::Front);
    CHECK(p.current().sweep.factor == Catch::Approx(0.01f));
    p.record_height(10.0f);
    CHECK(p.values().linear_advance == Catch::Approx(0.08f));

    REQUIRE(p.current().sweep.param == NlParam::Offset);
    CHECK(p.current().sweep.factor == Catch::Approx(0.02f));
    // Bowden corrects the advance instead, and a left side best lower wants more.
    p.record_verdict(SideVerdict::LeftLower, 20.0f);
    CHECK(p.values().linear_advance == Catch::Approx(0.088f));
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.0f));
    p.record_verdict(SideVerdict::Same, 5.0f);
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.1f));

    REQUIRE(p.current().sweep.param == NlParam::TimeOffset);
    p.record_height(10.0f);
    CHECK(p.values().time_offset == Catch::Approx(0.001f));

    REQUIRE(p.current().sweep.param == NlParam::Offset);
    p.record_verdict(SideVerdict::LeftHigher, 20.0f);
    CHECK(p.values().linear_advance == Catch::Approx(0.0792f));
    p.record_verdict(SideVerdict::Same, 6.0f);
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.12f));
    CHECK(p.done());
}

TEST_CASE("nonlinear procedure: a pass that never agrees ends on the user's height",
          "[pa_nonlinear]") {
    NonlinearProcedure p(ExtruderKind::DirectDrive, ModelKind::Tanh);
    p.record_height(10.0f);
    REQUIRE(p.current().sweep.param == NlParam::Advance);
    for (int i = 0; i < NonlinearProcedure::kMaxRepeats; ++i) {
        p.record_verdict(SideVerdict::LeftHigher, 25.0f);
        REQUIRE(p.current().sweep.param == NlParam::Advance);
    }
    p.record_verdict(SideVerdict::LeftHigher, 25.0f);
    CHECK(p.current().sweep.param == NlParam::TimeOffset);
    CHECK(p.values().linear_advance == Catch::Approx(0.025f));
}
