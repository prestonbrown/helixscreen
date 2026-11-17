// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_unit_card_fit.cpp
 * @brief A unit card on the Multi-Filament Overview gives its width to the lane
 *        bars first; the environment readout takes one line under them only when
 *        it fits, and is dropped otherwise.
 */

#include "ui_panel_ams_overview.h"

#include "../catch_amalgamated.hpp"

using helix::ui::CardEnvironment;
using helix::ui::decide_unit_card_fit;

namespace {
// The overview's bar floor and gap at the 800x480 breakpoint.
constexpr int32_t kMinBar = 6;
constexpr int32_t kGap = 3;
} // namespace

TEST_CASE("Unit card fit: every lane gets a bar while they fit at the floor width",
          "[ams_overview][card_fit]") {
    // 89 px holds (89 + 3) / 9 = 10 bars of 6 px with 3 px gaps.
    CHECK(decide_unit_card_fit(89, 4, kMinBar, kGap, 0).bars_shown == 4);
    CHECK(decide_unit_card_fit(89, 10, kMinBar, kGap, 0).bars_shown == 10);
    CHECK(decide_unit_card_fit(89, 11, kMinBar, kGap, 0).bars_shown == 10);
    // Exactly enough room for the last bar, without a trailing gap.
    CHECK(decide_unit_card_fit(87, 10, kMinBar, kGap, 0).bars_shown == 10);
    CHECK(decide_unit_card_fit(86, 10, kMinBar, kGap, 0).bars_shown == 9);
}

TEST_CASE("Unit card fit: the environment takes the line when it fits the card",
          "[ams_overview][card_fit]") {
    const auto fit = decide_unit_card_fit(89, 4, kMinBar, kGap, 74);
    CHECK(fit.bars_shown == 4);
    CHECK(fit.environment == CardEnvironment::Line);
    // A line exactly as wide as the card still fits.
    CHECK(decide_unit_card_fit(89, 4, kMinBar, kGap, 89).environment == CardEnvironment::Line);
}

TEST_CASE("Unit card fit: a line wider than the card is dropped, never the bars",
          "[ams_overview][card_fit]") {
    const auto fit = decide_unit_card_fit(70, 4, kMinBar, kGap, 71);
    CHECK(fit.bars_shown == 4);
    CHECK(fit.environment == CardEnvironment::Dropped);
}

TEST_CASE("Unit card fit: a unit with more lanes than fit keeps its lane count",
          "[ams_overview][card_fit]") {
    // The line under the bars says how many lanes there are; the environment
    // gives it up even though it would fit.
    const auto fit = decide_unit_card_fit(89, 12, kMinBar, kGap, 40);
    CHECK(fit.bars_shown == 10);
    CHECK(fit.environment == CardEnvironment::Dropped);
}

TEST_CASE("Unit card fit: a unit without environment data is only bars",
          "[ams_overview][card_fit]") {
    const auto fit = decide_unit_card_fit(89, 4, kMinBar, kGap, 0);
    CHECK(fit.bars_shown == 4);
    CHECK(fit.environment == CardEnvironment::None);
    CHECK(decide_unit_card_fit(89, 12, kMinBar, kGap, 0).environment == CardEnvironment::None);
}

TEST_CASE("Unit card fit: no lanes or no room draws no bars", "[ams_overview][card_fit]") {
    CHECK(decide_unit_card_fit(89, 0, kMinBar, kGap, 0).bars_shown == 0);
    CHECK(decide_unit_card_fit(89, 0, kMinBar, kGap, 40).environment == CardEnvironment::Line);
    CHECK(decide_unit_card_fit(5, 4, kMinBar, kGap, 0).bars_shown == 0);
    CHECK(decide_unit_card_fit(0, 4, kMinBar, kGap, 0).bars_shown == 0);
}
