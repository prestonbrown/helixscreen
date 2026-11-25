// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_nonlinear_tuning.h"

#include <algorithm>

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

TEST_CASE("nonlinear tower: each face's speed changes sit where Kalico's tower path puts them",
          "[pa_nonlinear][pa_tower_layout]") {
    const FaceLayout left = tower_face_layout(TowerFace::Left);
    // The 10 mm slow middle of the 40 mm left face, as fractions across the face.
    REQUIRE(left.spots.size() == 3);
    CHECK(left.spots[0].kind == SpotKind::SlowDown);
    CHECK(left.spots[0].at == Catch::Approx(0.375f));
    CHECK(left.spots[1].kind == SpotKind::VerySlowSpot);
    CHECK(left.spots[1].at == Catch::Approx(0.5f));
    CHECK(left.spots[2].kind == SpotKind::SpeedUp);
    CHECK(left.spots[2].at == Catch::Approx(0.625f));
    CHECK(left.segments.front().speed == SpeedKind::Medium);

    const FaceLayout front = tower_face_layout(TowerFace::Front);
    REQUIRE(front.spots.size() == 4);
    // The pause at size_x / 3 left of center, the slow middle, the very slow
    // spot at size_x / 3 right of center.
    CHECK(front.spots[0].kind == SpotKind::Pause);
    CHECK(front.spots[0].at == Catch::Approx(1.0f / 6.0f));
    CHECK(front.spots[1].at == Catch::Approx(0.4f));
    CHECK(front.spots[2].at == Catch::Approx(0.6f));
    CHECK(front.spots[3].kind == SpotKind::VerySlowSpot);
    CHECK(front.spots[3].at == Catch::Approx(5.0f / 6.0f));
    CHECK(front.segments.front().speed == SpeedKind::Fast);

    // A wider tower keeps the slow middle 10 mm wide.
    const FaceLayout wide = tower_face_layout(TowerFace::Front, 100.0f, 40.0f);
    CHECK(wide.spots[1].at == Catch::Approx(0.45f));
    CHECK(wide.spots[2].at == Catch::Approx(0.55f));

    // Segments run from one edge of the face to the other without gaps.
    for (const FaceLayout* f : {&left, &front}) {
        CHECK(f->segments.front().from == Catch::Approx(0.0f));
        CHECK(f->segments.back().to == Catch::Approx(1.0f));
        for (size_t i = 1; i < f->segments.size(); ++i) {
            CHECK(f->segments[i].from == Catch::Approx(f->segments[i - 1].to));
        }
    }
}

TEST_CASE("nonlinear tower: the faces a pass judges", "[pa_nonlinear][pa_tower_layout]") {
    CHECK(judged_faces(TowerSide::Left) == std::vector<TowerFace>{TowerFace::Left});
    CHECK(judged_faces(TowerSide::Front) == std::vector<TowerFace>{TowerFace::Front});
    CHECK(judged_faces(TowerSide::LeftVsFront) ==
          std::vector<TowerFace>{TowerFace::Left, TowerFace::Front});
}

TEST_CASE("nonlinear tower: the time offset is judged at the front's outermost transitions",
          "[pa_nonlinear][pa_tower_layout]") {
    const FaceLayout front = tower_face_layout(TowerFace::Front);
    for (const ExtruderKind kind : {ExtruderKind::DirectDrive, ExtruderKind::Bowden}) {
        const NonlinearProcedure p(kind, ModelKind::Tanh);
        for (int i = 0; i < p.pass_count(); ++i) {
            const NonlinearPass& pass = p.pass_at(static_cast<size_t>(i));
            const auto spots = judged_spots(pass);
            const auto has = [&spots](SpotKind k) {
                return std::find(spots.begin(), spots.end(), k) != spots.end();
            };
            if (pass.sweep.param == NlParam::TimeOffset) {
                // The leftmost and rightmost changes on the front, and nothing between.
                CHECK(spots.size() == 2);
                CHECK(has(front.spots.front().kind));
                CHECK(has(front.spots.back().kind));
                CHECK_FALSE(has(SpotKind::SlowDown));
                CHECK_FALSE(has(SpotKind::SpeedUp));
            } else {
                CHECK(has(SpotKind::SlowDown));
                CHECK(has(SpotKind::SpeedUp));
            }
        }
    }
}

TEST_CASE("nonlinear procedure: only Bowden's first advance keeps a share of its best value",
          "[pa_nonlinear]") {
    const NonlinearProcedure bowden(ExtruderKind::Bowden, ModelKind::Tanh);
    CHECK(bowden.kept_fraction(bowden.pass_at(0)) == Catch::Approx(0.8f));
    for (int i = 1; i < bowden.pass_count(); ++i) {
        CHECK(bowden.kept_fraction(bowden.pass_at(static_cast<size_t>(i))) == Catch::Approx(1.0f));
    }
    const NonlinearProcedure dd(ExtruderKind::DirectDrive, ModelKind::Tanh);
    for (int i = 0; i < dd.pass_count(); ++i) {
        CHECK(dd.kept_fraction(dd.pass_at(static_cast<size_t>(i))) == Catch::Approx(1.0f));
    }
}

TEST_CASE("nonlinear procedure: two heights decide the verdict, same within a millimeter",
          "[pa_nonlinear][pa_two_heights]") {
    CHECK(verdict_from_heights(24.0f, 27.5f) == SideVerdict::LeftLower);
    CHECK(verdict_from_heights(30.0f, 27.5f) == SideVerdict::LeftHigher);
    CHECK(verdict_from_heights(27.0f, 27.5f) == SideVerdict::Same);
    CHECK(verdict_from_heights(26.5f, 27.5f) == SideVerdict::Same);
    CHECK(verdict_from_heights(26.4f, 27.5f) == SideVerdict::LeftLower);
}

TEST_CASE("nonlinear procedure: the next tower's correction follows Kalico's rule",
          "[pa_nonlinear][pa_two_heights]") {
    SECTION("direct drive: left below the front lowers the offset") {
        NonlinearProcedure p(ExtruderKind::DirectDrive, ModelKind::Tanh);
        p.record_height(16.0f);
        REQUIRE(p.current().judge == TowerSide::LeftVsFront);
        auto c = p.correction(SideVerdict::LeftLower);
        REQUIRE(c.has_value());
        CHECK(c->param == NlParam::Offset);
        CHECK_FALSE(c->raise);
        c = p.correction(SideVerdict::LeftHigher);
        REQUIRE(c.has_value());
        CHECK(c->param == NlParam::Offset);
        CHECK(c->raise);
        CHECK_FALSE(p.correction(SideVerdict::Same).has_value());

        // The correction names the value it moves and where it goes: 10% down.
        const float before = p.values().nonlinear_offset;
        c = p.correction(SideVerdict::LeftLower);
        CHECK(c->from == Catch::Approx(before));
        CHECK(c->to == Catch::Approx(before * 0.9f));
        // The procedure applies what correction() says.
        p.record_verdict(SideVerdict::LeftLower, 24.0f);
        CHECK(p.values().nonlinear_offset == Catch::Approx(before * 0.9f));
    }
    SECTION("Bowden: left below the front raises the advance") {
        NonlinearProcedure p(ExtruderKind::Bowden, ModelKind::Tanh);
        p.record_height(20.0f);
        REQUIRE(p.current().judge == TowerSide::LeftVsFront);
        auto c = p.correction(SideVerdict::LeftLower);
        REQUIRE(c.has_value());
        CHECK(c->param == NlParam::Advance);
        CHECK(c->raise);
        const float before = p.values().linear_advance;
        p.record_verdict(SideVerdict::LeftLower, 24.0f);
        CHECK(p.values().linear_advance > before);
    }
    SECTION("a pass at its repeat limit takes the height: no correction") {
        NonlinearProcedure p(ExtruderKind::DirectDrive, ModelKind::Tanh);
        p.record_height(16.0f);
        for (int i = 0; i < NonlinearProcedure::kMaxRepeats; ++i) {
            p.record_verdict(SideVerdict::LeftLower, 24.0f);
        }
        REQUIRE(p.repeats() == NonlinearProcedure::kMaxRepeats);
        CHECK_FALSE(p.correction(SideVerdict::LeftLower).has_value());
    }
}

TEST_CASE("nonlinear procedure: a two-sided judgement keeps both heights through a restart",
          "[pa_nonlinear][pa_two_heights]") {
    NonlinearProcedure p(ExtruderKind::DirectDrive, ModelKind::Tanh);
    p.record_height(16.0f);
    p.record_heights(18.0f, 30.0f);
    REQUIRE(p.repeats() == 1);
    auto restored = NonlinearProcedure::from_json(p.to_json());
    REQUIRE(restored.has_value());
    const auto j = restored->go_back_judgement();
    REQUIRE(j.has_value());
    REQUIRE(j->left_mm.has_value());
    REQUIRE(j->front_mm.has_value());
    CHECK(*j->left_mm == Catch::Approx(18.0f));
    CHECK(*j->front_mm == Catch::Approx(30.0f));
    CHECK(restored->pass_number() == 2);
    CHECK(restored->repeats() == 0);
}

TEST_CASE("tower layer progress reads the layer and fraction from the nozzle height",
          "[pa_nonlinear]") {
    auto p = tower_layer_progress(0.2f);
    CHECK(p.layer == 1);
    CHECK(p.layers == 250);
    CHECK(p.fraction == Catch::Approx(0.004f));
    CHECK(tower_layer_progress(1.0f).layer == 5);
    CHECK(tower_layer_progress(0.0f).layer == 1);
    CHECK(tower_layer_progress(50.0f).layer == 250);
    auto over = tower_layer_progress(55.0f);
    CHECK(over.layer == 250);
    CHECK(over.fraction == Catch::Approx(1.0f));
}

TEST_CASE("tower time left: expected time until the climb shows a rate, then the rate",
          "[pa_nonlinear]") {
    TowerTimeLeft eta(1740);
    CHECK(eta.seconds_left() == 1740);
    eta.sample(0.2f, 0.0);
    CHECK(eta.seconds_left() == 1740);
    // The brim layer: still the expected time, less what has printed.
    eta.sample(0.2f, 120.0);
    CHECK(eta.seconds_left() == 1620);
    // The second layer starts the rate; under 1 mm of climb is too little to use.
    eta.sample(0.4f, 150.0);
    eta.sample(1.0f, 200.0);
    CHECK(eta.seconds_left() == 1540);
    // 1 mm in 100 s, 48.6 mm to go.
    eta.sample(1.4f, 250.0);
    CHECK(eta.seconds_left() == 4860);
    // A travel move lower down is no step back.
    eta.sample(0.6f, 260.0);
    CHECK(eta.seconds_left() == 5346);
    for (float z = 1.6f; z < 50.05f; z += 0.2f) {
        eta.sample(z, 5000.0);
    }
    CHECK(eta.seconds_left() == 0);
}

TEST_CASE("tower time left never goes below zero on a slow first layer", "[pa_nonlinear]") {
    TowerTimeLeft eta(60);
    eta.sample(0.2f, 0.0);
    eta.sample(0.2f, 300.0);
    CHECK(eta.seconds_left() == 0);
}

TEST_CASE("tower reached height climbs a layer at a time and ignores lifts and travel",
          "[pa_nonlinear]") {
    CHECK(tower_reached_mm(0.0f, 0.2f) == Catch::Approx(0.2f));
    CHECK(tower_reached_mm(28.8f, 29.0f) == Catch::Approx(29.0f));
    // The lift that ends a cancelled print.
    CHECK(tower_reached_mm(29.0f, 58.9f) == Catch::Approx(29.0f));
    // A travel move lower down.
    CHECK(tower_reached_mm(29.0f, 3.0f) == Catch::Approx(29.0f));
}
