// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: it tests tests/test_helpers/sample_checks.h, which is test code,
// not shipped code.

// The sample checks of tests/test_helpers/sample_checks.h, held to what a
// Catch2 assertion promises: the expression runs once, a failing one reports the
// values that evaluation produced, with the sample named, and ends the case or
// lets it continue as its namesake does. A failure fails whatever case holds
// it, so the failing ones run in the two hidden cases below, each in a fresh
// process whose output the cases here read.

#include "../test_helpers/fresh_process.h"
#include "../test_helpers/sample_checks.h"

#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Set only in the processes the cases below start.
constexpr const char* kProbeEnv = "HELIX_SAMPLE_CHECK_PROBE";

bool shows(const helix::test::ChildRun& run, const char* text) {
    return run.output.find(text) != std::string::npos;
}

} // namespace

TEST_CASE("a sample check evaluates its expression exactly once", "[test_helpers]") {
    helix::test::SampleChecks samples;
    int evaluations = 0;

    // Each passes on its first evaluation and is not looked at again.
    SAMPLE_REQUIRE(samples, ++evaluations == 1, 0);
    CHECK(evaluations == 1);
    SAMPLE_CHECK(samples, ++evaluations == 2, 1);
    CHECK(evaluations == 2);
    SAMPLE_CHECK_FALSE(samples, ++evaluations == 99, 2);
    CHECK(evaluations == 3);
    samples.done(3);
}

TEST_CASE("a failing sample REQUIRE reports its first evaluation and ends the case",
          "[test_helpers]") {
    const helix::test::ChildRun child =
        helix::test::run_case_in_fresh_process("sample check reporter probe: REQUIRE", kProbeEnv);
    INFO(child.output);

    // Catch2 exits with its number of failed assertions.
    REQUIRE(child.rc == 1);
    // The first evaluation failed, and that is the one reported: a second one
    // would have passed.
    CHECK(shows(child, "REQUIRE( ++evaluations == 2 )"));
    CHECK(shows(child, "1 == 2"));
    CHECK(shows(child, "sample 7"));
    CHECK(shows(child, "assertions: 1 | 1 failed"));
    // Nothing of the case ran past it.
    CHECK_FALSE(shows(child, "WENT ON"));
}

TEST_CASE("a failing sample CHECK reports both sides and lets its loop go on", "[test_helpers]") {
    const helix::test::ChildRun child =
        helix::test::run_case_in_fresh_process("sample check reporter probe: CHECK", kProbeEnv);
    INFO(child.output);

    // Two failing samples and the FAIL that ends the probe.
    REQUIRE(child.rc == 3);
    // The first pass fails with what its one evaluation produced.
    CHECK(shows(child, "CHECK( ++evaluations == 2 )"));
    CHECK(shows(child, "1 == 2"));
    CHECK(shows(child, "sample 0"));
    // The loop went on: the third pass fails too, under its own sample.
    CHECK(shows(child, "3 == 2"));
    CHECK(shows(child, "sample 22"));
    // Catch2 counts four assertions, not five: the passing sample check is none
    // of its business, so the one pass is the count done() makes.
    CHECK(shows(child, "assertions: 4 | 1 passed | 3 failed"));
    CHECK(shows(child, "WENT ON past 3 evaluations"));
}

TEST_CASE("sample check reporter probe: REQUIRE", "[.sample_check_probe]") {
    if (std::getenv(kProbeEnv) == nullptr) {
        SKIP("runs only in the process its [test_helpers] case starts");
    }
    helix::test::SampleChecks samples;
    int evaluations = 0;
    SAMPLE_REQUIRE(samples, ++evaluations == 2, 7);
    FAIL("WENT ON past a failing sample REQUIRE after " << evaluations << " evaluations");
}

TEST_CASE("sample check reporter probe: CHECK", "[.sample_check_probe]") {
    if (std::getenv(kProbeEnv) == nullptr) {
        SKIP("runs only in the process its [test_helpers] case starts");
    }
    helix::test::SampleChecks samples;
    int evaluations = 0;
    // Fails, passes, fails.
    for (int i = 0; i < 3; i++) {
        SAMPLE_CHECK(samples, ++evaluations == 2, i * 11);
    }
    samples.done(3);
    FAIL("WENT ON past " << evaluations << " evaluations");
}
