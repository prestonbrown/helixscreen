// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spool_remaining_format.h"

#include "../catch_amalgamated.hpp"

using helix::format_spool_grams;
using helix::format_spool_remaining;
using helix::format_spool_remaining_short;
using helix::spool_remaining_percent;

TEST_CASE("what is left on a spool reads in grams and percent", "[spool_remaining]") {
    CHECK(format_spool_remaining(850, 1000) == "850 g \xC2\xB7 85%");
    CHECK(format_spool_remaining_short(850, 1000) == "85%");
    // An unknown size leaves grams alone, in both forms.
    CHECK(format_spool_remaining(850, -1) == "850 g");
    CHECK(format_spool_remaining_short(850, 0) == "850 g");
    // An unknown weight says nothing.
    CHECK(format_spool_remaining(-1, 1000).empty());
    CHECK(format_spool_remaining_short(-1, 1000).empty());
    CHECK(format_spool_grams(-1).empty());
}

TEST_CASE("the percent left rounds and stays within the spool", "[spool_remaining]") {
    CHECK(spool_remaining_percent(333, 1000) == 33);
    CHECK(spool_remaining_percent(335, 1000) == 34);
    CHECK(spool_remaining_percent(0, 1000) == 0);
    CHECK(spool_remaining_percent(1200, 1000) == 100);
    CHECK(spool_remaining_percent(500, 0) == -1);
    CHECK(spool_remaining_percent(-1, 1000) == -1);
}
