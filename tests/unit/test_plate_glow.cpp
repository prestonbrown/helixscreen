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
