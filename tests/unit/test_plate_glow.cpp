// SPDX-License-Identifier: GPL-3.0-or-later
#include "plate_glow.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

int alpha_at(const PlateGlowMask& m, int x, int y) {
    return m.alpha[size_t(y - m.y) * size_t(m.w) + size_t(x - m.x)];
}

} // namespace

TEST_CASE("bed_glow_level: by temperature, not target", "[plate_glow]") {
    CHECK(bed_glow_level(250) == 0);
    CHECK(bed_glow_level(500) == 0); // at the burn threshold, not past it
    CHECK(bed_glow_level(501) == 1);
    CHECK(bed_glow_level(699) == 1);
    CHECK(bed_glow_level(700) == 2);
    CHECK(bed_glow_level(900) == 3);
}

TEST_CASE("render_plate_glow: the plate fills, the haze fades upward and at its sides",
          "[plate_glow]") {
    // A plate 100 wide whose top edge is y 60, near edge y 80.
    const std::array<CalloutPoint, 4> quad{{{0, 80}, {100, 80}, {90, 60}, {10, 60}}};
    const PlateGlowMask m = render_plate_glow(quad, 40);
    REQUIRE(m.w == 100);
    CHECK(m.x == 0);
    CHECK(m.y == 20); // the haze's top
    CHECK(m.h == 61);
    REQUIRE(m.alpha.size() == size_t(m.w) * size_t(m.h));

    const int plate = alpha_at(m, 50, 70);
    const int low_haze = alpha_at(m, 50, 50);
    const int high_haze = alpha_at(m, 50, 25);
    CHECK(plate > low_haze);
    CHECK(low_haze > high_haze);
    CHECK(alpha_at(m, 50, 20) < 10); // faded out at the top
    // The haze's sides are softer than its middle.
    CHECK(alpha_at(m, 12, 50) < low_haze);
    // Over the plate's outermost corner the side fade leaves almost nothing.
    CHECK(alpha_at(m, 1, 62) < 5);

    CHECK(render_plate_glow({{{5, 5}, {5, 9}, {5, 9}, {5, 5}}}, 10).w == 0);
}

TEST_CASE("chamber_glow_level: by the air's temperature", "[plate_glow]") {
    CHECK(chamber_glow_level(250) == 0);
    CHECK(chamber_glow_level(300) == 0);
    CHECK(chamber_glow_level(301) == 1);
    CHECK(chamber_glow_level(449) == 1);
    CHECK(chamber_glow_level(450) == 2);
    CHECK(chamber_glow_level(599) == 2);
    CHECK(chamber_glow_level(600) == 3);
}

TEST_CASE("render_region_glow: light stays inside its regions", "[plate_glow]") {
    const GlowQuad front{{{10, 10}, {60, 10}, {60, 90}, {10, 90}}};
    const GlowQuad side{{{70, 20}, {100, 20}, {100, 80}, {70, 80}}};
    const PlateGlowMask m = render_region_glow({front, side}, {35, 50}, 80.f, 0.5f, 200.f);
    REQUIRE(m.step == 1);
    CHECK(m.x == 10);
    CHECK(m.y == 10);
    CHECK(m.w == 90);
    CHECK(m.h == 80);
    REQUIRE(m.alpha.size() == size_t(m.w) * size_t(m.h));

    CHECK(alpha_at(m, 35, 50) > 180);                 // inside the first, at the source
    CHECK(alpha_at(m, 85, 50) > 0);                   // inside the second
    CHECK(alpha_at(m, 65, 50) == 0);                  // the frame between them
    CHECK(alpha_at(m, 85, 12) == 0);                  // above the shorter region
    CHECK(alpha_at(m, 10, 50) < alpha_at(m, 14, 50)); // the edge feathers inward
    CHECK(alpha_at(m, 35, 50) > alpha_at(m, 55, 50)); // and it fades from the source
}

TEST_CASE("render_region_glow: the floor keeps far corners lit", "[plate_glow]") {
    const GlowQuad q{{{0, 0}, {100, 0}, {100, 20}, {0, 20}}};
    const PlateGlowMask lit = render_region_glow({q}, {10, 10}, 30.f, 0.5f, 200.f);
    CHECK(alpha_at(lit, 90, 10) == 100);
    const PlateGlowMask dark = render_region_glow({q}, {10, 10}, 30.f, 0.f, 200.f);
    CHECK(alpha_at(dark, 90, 10) == 0);
}

TEST_CASE("render_region_glow: winding does not matter", "[plate_glow]") {
    const GlowQuad cw{{{10, 10}, {60, 10}, {60, 60}, {10, 60}}};
    const GlowQuad ccw{{{10, 10}, {10, 60}, {60, 60}, {60, 10}}};
    const auto a = render_region_glow({cw}, {30, 30}, 50.f, 0.5f, 200.f);
    const auto b = render_region_glow({ccw}, {30, 30}, 50.f, 0.5f, 200.f);
    CHECK(alpha_at(a, 30, 30) > 0);
    CHECK(a.alpha == b.alpha);
}

TEST_CASE("render_region_glow: a huge region renders coarser, not bigger", "[plate_glow]") {
    const GlowQuad q{{{0, 0}, {1600, 0}, {1600, 1200}, {0, 1200}}};
    const PlateGlowMask m = render_region_glow({q}, {800, 600}, 1000.f, 0.5f, 200.f);
    CHECK(m.step > 1);
    CHECK(m.alpha.size() <= kMaxGlowMaskBytes);
    CHECK(m.alpha.size() == size_t(m.w) * size_t(m.h));
    // The mask still spans the whole region once drawn at its step.
    CHECK(m.w * m.step >= 1600);
    CHECK(m.h * m.step >= 1200);
    CHECK(m.w * m.step < 1600 + m.step);
}

TEST_CASE("render_region_glow: nothing to draw", "[plate_glow]") {
    CHECK(render_region_glow({}, {0, 0}, 10.f, 0.5f, 200.f).w == 0);
    const GlowQuad flat{{{5, 5}, {5, 9}, {5, 9}, {5, 5}}};
    CHECK(render_region_glow({flat}, {0, 0}, 10.f, 0.5f, 200.f).w == 0);
}

TEST_CASE("render_disc_glow: full at the center, gone at the rim", "[plate_glow]") {
    const PlateGlowMask m = render_disc_glow({50, 40}, 20);
    CHECK(m.x == 30);
    CHECK(m.y == 20);
    REQUIRE(m.w == 40);
    REQUIRE(m.h == 40);
    CHECK(alpha_at(m, 50, 40) > 240);
    CHECK(alpha_at(m, 60, 40) < alpha_at(m, 50, 40));
    CHECK(alpha_at(m, 60, 40) > 0);
    CHECK(alpha_at(m, 30, 40) == 0);
    CHECK(render_disc_glow({5, 5}, 0).w == 0);
}

TEST_CASE("render_region_glow: a cutout takes the glow away where it sits", "[plate_glow]") {
    const GlowQuad window{{{0, 0}, {100, 0}, {100, 100}, {0, 100}}};
    const GlowQuad screen{{{20, 20}, {50, 20}, {50, 50}, {20, 50}}};
    const PlateGlowMask plain = render_region_glow({window}, {50, 50}, 120.f, 0.5f, 200.f);
    const PlateGlowMask cut = render_region_glow({window}, {50, 50}, 120.f, 0.5f, 200.f, {screen});
    REQUIRE(cut.w == plain.w);
    REQUIRE(cut.h == plain.h);
    CHECK(alpha_at(plain, 35, 35) > 0);
    CHECK(alpha_at(cut, 35, 35) == 0);                       // inside the cutout
    CHECK(alpha_at(cut, 80, 80) == alpha_at(plain, 80, 80)); // the window around it
    CHECK(alpha_at(cut, 49, 35) < alpha_at(plain, 49, 35));  // its edge feathers too
    CHECK(alpha_at(cut, 51, 35) == alpha_at(plain, 51, 35));
}
