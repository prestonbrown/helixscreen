// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/ui_jog_pad.h"

#include "../catch_amalgamated.hpp"

using helix::jog_pad_zone_for_offset;
using helix::JogDirection;
using helix::JogPadZoneOptions;

namespace {
constexpr int kRadius = 100;
const JogPadZoneOptions kFullPad{};
const JogPadZoneOptions kCardinalNoHome{false, true};
} // namespace

TEST_CASE("A full jog pad keeps its home button and eight wedges", "[jog_pad][motion]") {
    CHECK(jog_pad_zone_for_offset(0, 0, kRadius, kFullPad).home);
    CHECK(jog_pad_zone_for_offset(0, 0, kRadius, kFullPad).on_pad);
    CHECK_FALSE(jog_pad_zone_for_offset(kRadius + 1, 0, kRadius, kFullPad).on_pad);

    CHECK(jog_pad_zone_for_offset(0, -50, kRadius, kFullPad).direction == JogDirection::N);
    CHECK(jog_pad_zone_for_offset(50, -50, kRadius, kFullPad).direction == JogDirection::NE);
    CHECK(jog_pad_zone_for_offset(-50, 50, kRadius, kFullPad).direction == JogDirection::SW);

    CHECK(jog_pad_zone_for_offset(0, -40, kRadius, kFullPad).inner);
    CHECK_FALSE(jog_pad_zone_for_offset(0, -80, kRadius, kFullPad).inner);
}

TEST_CASE("A cardinal-only jog pad has no home button and no diagonals", "[jog_pad][chute_jog]") {
    const auto hub = jog_pad_zone_for_offset(0, 0, kRadius, kCardinalNoHome);
    CHECK_FALSE(hub.on_pad);
    CHECK_FALSE(hub.home);

    // Each pair sits either side of a 45 degree boundary, where a full pad reads a diagonal.
    CHECK(jog_pad_zone_for_offset(30, -70, kRadius, kCardinalNoHome).direction == JogDirection::N);
    CHECK(jog_pad_zone_for_offset(70, -30, kRadius, kCardinalNoHome).direction == JogDirection::E);
    CHECK(jog_pad_zone_for_offset(70, 30, kRadius, kCardinalNoHome).direction == JogDirection::E);
    CHECK(jog_pad_zone_for_offset(30, 70, kRadius, kCardinalNoHome).direction == JogDirection::S);
    CHECK(jog_pad_zone_for_offset(-30, 70, kRadius, kCardinalNoHome).direction == JogDirection::S);
    CHECK(jog_pad_zone_for_offset(-70, 30, kRadius, kCardinalNoHome).direction == JogDirection::W);
    CHECK(jog_pad_zone_for_offset(-70, -30, kRadius, kCardinalNoHome).direction == JogDirection::W);
    CHECK(jog_pad_zone_for_offset(-30, -70, kRadius, kCardinalNoHome).direction == JogDirection::N);

    CHECK(jog_pad_zone_for_offset(0, -40, kRadius, kCardinalNoHome).inner);
    CHECK_FALSE(jog_pad_zone_for_offset(0, -80, kRadius, kCardinalNoHome).inner);
}
