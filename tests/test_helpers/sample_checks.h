// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A check made once per sample of a loop. Catch2 flushes a --success report
// after every insertion, so each assertion of the suite costs about 230
// write() calls: a property test that asserts per sample writes more report
// than the rest of its file, and on a filesystem where a small write is dear it
// spends its run writing it.

#include <cstddef>

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// Counts the checks the SAMPLE_ macros made, so a case can end by proving its
/// loop ran: one assertion for the loop instead of one per sample.
class SampleChecks {
  public:
    SampleChecks& operator++() {
        ++count_;
        return *this;
    }

    /// Fails unless exactly @p checks were made: a loop that never ran, or that
    /// stopped early, is a red case rather than a green one with no evidence.
    void done(std::size_t checks) const {
        CHECK(count_ == checks);
    }

    std::size_t count() const {
        return count_;
    }

  private:
    std::size_t count_ = 0;
};

} // namespace helix::test

/// REQUIRE, REQUIRE_FALSE, CHECK and CHECK_FALSE for one sample of a loop. A
/// sample check fails the case exactly as its Catch2 namesake does, with the
/// sample named, and it writes nothing to the report while the sample passes.
///
///   SampleChecks samples;
///   for (int i = 0; i < 20000; i++) {
///       SAMPLE_REQUIRE(samples, same(a + b, want), i);
///   }
///   samples.done(20000);
///
/// @p expr is evaluated twice when it fails, once for the verdict and once for
/// the expansion the namesake prints, so a failing sample may move a random
/// generator twice.
#define HELIX_SAMPLE_TEST(name, disposition, samples, expr, sample)                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            INFO("sample " << (sample));                                                           \
            INTERNAL_CATCH_TEST(name, disposition, expr);                                          \
        }                                                                                          \
        ++(samples);                                                                               \
    } while (false)

#define SAMPLE_REQUIRE(samples, expr, sample)                                                      \
    HELIX_SAMPLE_TEST("REQUIRE", Catch::ResultDisposition::Normal, samples, expr, sample)
#define SAMPLE_CHECK(samples, expr, sample)                                                        \
    HELIX_SAMPLE_TEST("CHECK", Catch::ResultDisposition::ContinueOnFailure, samples, expr, sample)

#define HELIX_SAMPLE_TEST_FALSE(name, disposition, samples, expr, sample)                          \
    do {                                                                                           \
        if (expr) {                                                                                \
            INFO("sample " << (sample));                                                           \
            INTERNAL_CATCH_TEST(name, disposition, expr);                                          \
        }                                                                                          \
        ++(samples);                                                                               \
    } while (false)

#define SAMPLE_REQUIRE_FALSE(samples, expr, sample)                                                \
    HELIX_SAMPLE_TEST_FALSE(                                                                       \
        "REQUIRE_FALSE", Catch::ResultDisposition::Normal | Catch::ResultDisposition::FalseTest,   \
        samples, expr, sample)
#define SAMPLE_CHECK_FALSE(samples, expr, sample)                                                  \
    HELIX_SAMPLE_TEST_FALSE("CHECK_FALSE",                                                         \
                            Catch::ResultDisposition::ContinueOnFailure |                          \
                                Catch::ResultDisposition::FalseTest,                               \
                            samples, expr, sample)

#ifdef CATCH_CONFIG_DISABLE
#undef HELIX_SAMPLE_TEST
#undef SAMPLE_REQUIRE
#undef SAMPLE_CHECK
#undef HELIX_SAMPLE_TEST_FALSE
#undef SAMPLE_REQUIRE_FALSE
#undef SAMPLE_CHECK_FALSE
#define HELIX_SAMPLE_TEST(name, disposition, samples, expr, sample) (void)(0)
#define SAMPLE_REQUIRE(samples, expr, sample) (void)(0)
#define SAMPLE_CHECK(samples, expr, sample) (void)(0)
#define HELIX_SAMPLE_TEST_FALSE(name, disposition, samples, expr, sample) (void)(0)
#define SAMPLE_REQUIRE_FALSE(samples, expr, sample) (void)(0)
#define SAMPLE_CHECK_FALSE(samples, expr, sample) (void)(0)
#endif
