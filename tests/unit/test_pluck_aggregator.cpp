// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pluck_aggregator.cpp
 * @brief Running median across plucks, with a commit threshold
 */

#include "../../include/pluck_aggregator.h"

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

TEST_CASE("aggregator starts empty and uncommitted", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    CHECK(agg.count() == 0);
    CHECK_FALSE(agg.committed());
    CHECK(agg.median() == 0.0f);
}

TEST_CASE("aggregator does not commit before the threshold", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    for (size_t i = 1; i < PluckAggregator::COMMIT_AFTER; ++i) {
        agg.add(86.0f);
        INFO("after " << i << " plucks");
        CHECK(agg.count() == i);
        CHECK_FALSE(agg.committed());
    }
    agg.add(86.0f);
    CHECK(agg.count() == PluckAggregator::COMMIT_AFTER);
    CHECK(agg.committed());
}

TEST_CASE("median ignores a single outlier", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    agg.add(86.0f);
    agg.add(86.0f);
    agg.add(56.0f); // the kind of miss a single HPS pass produces
    agg.add(86.0f);
    agg.add(86.0f);
    REQUIRE(agg.committed());
    CHECK(agg.median() == Catch::Approx(86.0f));
}

TEST_CASE("median of an even count averages the middle pair", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    agg.add(82.0f);
    agg.add(86.0f);
    CHECK(agg.median() == Catch::Approx(84.0f));
}

TEST_CASE("aggregator keeps accepting past the commit threshold", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    for (int i = 0; i < 12; ++i) {
        agg.add(86.0f);
    }
    CHECK(agg.count() == 12);
    CHECK(agg.committed());
    CHECK(agg.median() == Catch::Approx(86.0f));
}

TEST_CASE("reset clears everything", "[belt_tension][aggregate]") {
    PluckAggregator agg;
    for (int i = 0; i < 6; ++i) {
        agg.add(86.0f);
    }
    agg.reset();
    CHECK(agg.count() == 0);
    CHECK_FALSE(agg.committed());
    CHECK(agg.median() == 0.0f);
}

TEST_CASE("aggregator ignores non-positive frequencies", "[belt_tension][aggregate][edge_case]") {
    PluckAggregator agg;
    agg.add(0.0f);
    agg.add(-5.0f);
    CHECK(agg.count() == 0);
}
