// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_nonlinear_tuning.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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

TEST_CASE("nonlinear procedure: going back re-opens the last judgement with its tower's values",
          "[pa_nonlinear][pa_tuner][pa_nl_back]") {
    NonlinearProcedure p(ExtruderKind::DirectDrive, ModelKind::Tanh);
    CHECK_FALSE(p.can_go_back());
    CHECK_FALSE(p.go_back().has_value());

    p.record_height(12.0f);
    REQUIRE(p.pass_number() == 2);
    // A repeat of the two-sided pass is a judgement of its own.
    p.record_verdict(SideVerdict::LeftLower, 30.0f);
    REQUIRE(p.repeats() == 1);
    REQUIRE(p.values().nonlinear_offset == Catch::Approx(0.108f));

    REQUIRE(p.can_go_back());
    const auto h = p.go_back();
    REQUIRE(h.has_value());
    CHECK(*h == Catch::Approx(30.0f));
    CHECK(p.pass_number() == 2);
    CHECK(p.repeats() == 0);
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.12f));

    const auto first = p.go_back();
    REQUIRE(first.has_value());
    CHECK(*first == Catch::Approx(12.0f));
    CHECK(p.pass_number() == 1);
    // Pass 1's tower printed with no offset yet: that is what it is judged on again.
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.0f));
    CHECK_FALSE(p.can_go_back());

    // Judged again, it moves on as before.
    p.record_height(14.0f);
    CHECK(p.pass_number() == 2);
    CHECK(p.values().nonlinear_offset == Catch::Approx(0.14f));
}

TEST_CASE("nonlinear procedure: its saved form continues where it stopped",
          "[pa_nonlinear][pa_tuner][pa_nl_session]") {
    NonlinearProcedure p(ExtruderKind::Bowden, ModelKind::Recipr);
    p.record_height(20.0f);
    p.record_verdict(SideVerdict::LeftHigher, 10.0f);

    const auto r = NonlinearProcedure::from_json(p.to_json());
    REQUIRE(r.has_value());
    CHECK(r->kind() == ExtruderKind::Bowden);
    CHECK(r->values().model == ModelKind::Recipr);
    CHECK(r->pass_number() == p.pass_number());
    CHECK(r->repeats() == p.repeats());
    CHECK(r->values().linear_advance == Catch::Approx(p.values().linear_advance));
    CHECK(r->values().linearization_velocity == Catch::Approx(2.0f));
    auto back = *r;
    REQUIRE(back.go_back().has_value());
    REQUIRE(back.go_back().has_value());
    CHECK(back.pass_number() == 1);
    CHECK_FALSE(back.can_go_back());

    // A saved form for another pass order is not continued.
    auto other = p.to_json();
    other["passes"] = 3;
    CHECK_FALSE(NonlinearProcedure::from_json(other).has_value());
    CHECK_FALSE(NonlinearProcedure::from_json(nlohmann::json::object()).has_value());
}
