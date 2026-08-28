// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_machine_limits_char.cpp
 * @brief MachineLimits validity and equality
 *
 * The Machine Limits overlay gates its Apply on is_valid() and decides
 * whether a reset changed anything with operator==, so both predicates are
 * behaviour, not accessors. Display formatting goes through
 * helix::format::format_speed_mm_s / format_accel_mm_s2, covered in
 * test_format_utils.cpp.
 *
 * @see include/calibration_types.h - MachineLimits
 * @see src/ui/ui_settings_machine_limits.cpp - MachineLimitsOverlay
 */

#include "calibration_types.h"

#include "../catch_amalgamated.hpp"

namespace {

MachineLimits typical_limits() {
    MachineLimits limits;
    limits.max_velocity = 300.0;
    limits.max_accel = 3000.0;
    limits.max_accel_to_decel = 1500.0;
    limits.square_corner_velocity = 5.0;
    limits.max_z_velocity = 15.0;
    limits.max_z_accel = 100.0;
    return limits;
}

} // namespace

TEST_CASE("MachineLimits is_valid requires velocity and accel", "[calibration][machine_limits]") {
    SECTION("Default-constructed limits are not valid") {
        REQUIRE_FALSE(MachineLimits{}.is_valid());
    }

    SECTION("Both positive is valid") {
        REQUIRE(typical_limits().is_valid());
    }

    SECTION("Zero max_velocity invalidates") {
        MachineLimits limits = typical_limits();
        limits.max_velocity = 0;
        REQUIRE_FALSE(limits.is_valid());
    }

    SECTION("Zero max_accel invalidates") {
        MachineLimits limits = typical_limits();
        limits.max_accel = 0;
        REQUIRE_FALSE(limits.is_valid());
    }

    SECTION("Negative values invalidate") {
        MachineLimits limits = typical_limits();
        limits.max_velocity = -1.0;
        REQUIRE_FALSE(limits.is_valid());
    }

    SECTION("The other four fields do not affect validity") {
        MachineLimits limits;
        limits.max_velocity = 300.0;
        limits.max_accel = 3000.0;
        // accel_to_decel, square_corner_velocity and both Z limits left at 0
        REQUIRE(limits.is_valid());
    }
}

TEST_CASE("MachineLimits equality compares every field", "[calibration][machine_limits]") {
    SECTION("Identical limits compare equal") {
        REQUIRE(typical_limits() == typical_limits());
        REQUIRE_FALSE(typical_limits() != typical_limits());
    }

    SECTION("Each field alone breaks equality") {
        const MachineLimits base = typical_limits();

        auto differs_in = [&base](void (*mutate)(MachineLimits&)) {
            MachineLimits other = base;
            mutate(other);
            return base != other && !(base == other);
        };

        REQUIRE(differs_in([](MachineLimits& l) { l.max_velocity += 1.0; }));
        REQUIRE(differs_in([](MachineLimits& l) { l.max_accel += 1.0; }));
        REQUIRE(differs_in([](MachineLimits& l) { l.max_accel_to_decel += 1.0; }));
        REQUIRE(differs_in([](MachineLimits& l) { l.square_corner_velocity += 1.0; }));
        REQUIRE(differs_in([](MachineLimits& l) { l.max_z_velocity += 1.0; }));
        REQUIRE(differs_in([](MachineLimits& l) { l.max_z_accel += 1.0; }));
    }
}
