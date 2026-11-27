// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// TEST_MIRROR_OK: it tests tests/test_helpers/sample_checks.h, which is test code,
// not shipped code.

// The sample checks of tests/test_helpers/sample_checks.h, held to what a
// Catch2 assertion promises: the expression runs once, a failing one is
// reported and ends the case as its namesake does, a passing one writes
// nothing.

#include "../test_helpers/sample_checks.h"

#include "../catch_amalgamated.hpp"

namespace {

/// A sample REQUIRE on an expression that counts how often it is evaluated.
/// It passes on its second evaluation and fails on its first.
void counted_sample_require(helix::test::SampleChecks& samples, int& evaluations) {
    SAMPLE_REQUIRE(samples, ++evaluations == 2, 7);
}

} // namespace

TEST_CASE("a sample check evaluates its expression exactly once", "[test_helpers]") {
    helix::test::SampleChecks samples;
    int evaluations = 0;

    // The first evaluation fails, and that is the one reported: a second one
    // would have passed.
    CHECK_THROWS_AS(counted_sample_require(samples, evaluations), Catch::TestFailureException);
    CHECK(evaluations == 1);

    // One that passes on its first evaluation is not looked at again.
    evaluations = 0;
    SAMPLE_REQUIRE(samples, ++evaluations == 1, 0);
    CHECK(evaluations == 1);
    SAMPLE_CHECK(samples, ++evaluations == 2, 1);
    CHECK(evaluations == 2);
    SAMPLE_CHECK_FALSE(samples, ++evaluations == 99, 2);
    CHECK(evaluations == 3);
    // The failed one was counted too: a loop's count is its passes.
    samples.done(4);
}
