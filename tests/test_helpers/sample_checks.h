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

  private:
    std::size_t count_ = 0;
};

namespace detail {

/// What Catch2 concluded when it formed the decomposed expression. The
/// expression's parts were evaluated once to form it; nothing runs again.
template <typename LhsT> bool verdict_of(const Catch::ExprLhs<LhsT>& expr) {
    return expr.makeUnaryExpr().getResult();
}

inline bool verdict_of(const Catch::ITransientExpression& expr) {
    return expr.getResult();
}

/// One sample check on an expression that is evaluated and decomposed already.
/// A passing one is counted, and that is all it costs. A failing one is
/// reported as its Catch2 namesake reports (the values of both sides, then the
/// sample), and ends the case or lets it continue as the namesake does.
template <typename Expr, typename Sample>
void sample_check(SampleChecks& samples, Catch::StringRef macro_name,
                  const Catch::SourceLineInfo& line, Catch::ResultDisposition::Flags disposition,
                  Catch::StringRef expression, const Expr& expr, const Sample& sample) {
    ++samples;
    if (verdict_of(expr) != Catch::isFalseTest(disposition)) {
        return;
    }
    INFO("sample " << sample);
    Catch::AssertionHandler handler(macro_name, line, expression, disposition);
    INTERNAL_CATCH_TRY {
        handler.handleExpr(expr);
    }
    INTERNAL_CATCH_CATCH(handler)
    INTERNAL_CATCH_REACT(handler)
}

} // namespace detail
} // namespace helix::test

/// REQUIRE, CHECK and CHECK_FALSE for one sample of a loop. Each evaluates its
/// expression exactly once, as its namesake does, so the expression may advance
/// state or draw from a generator. A passing sample writes nothing to the
/// report.
///
///   SampleChecks samples;
///   for (int i = 0; i < 20000; i++) {
///       SAMPLE_REQUIRE(samples, same(a + b, want), i);
///   }
///   samples.done(20000);
///
/// A failing one reports the values the one evaluation produced, with the
/// sample named.
#define HELIX_SAMPLE_TEST(name, disposition, samples, expr, sample)                                \
    do {                                                                                           \
        helix::test::detail::sample_check(samples, name##_catch_sr, CATCH_INTERNAL_LINEINFO,       \
                                          disposition, #expr, Catch::Decomposer() <= expr,         \
                                          sample);                                                 \
    } while (false)

#define SAMPLE_REQUIRE(samples, expr, sample)                                                      \
    HELIX_SAMPLE_TEST("REQUIRE", Catch::ResultDisposition::Normal, samples, expr, sample)
#define SAMPLE_CHECK(samples, expr, sample)                                                        \
    HELIX_SAMPLE_TEST("CHECK", Catch::ResultDisposition::ContinueOnFailure, samples, expr, sample)
#define SAMPLE_CHECK_FALSE(samples, expr, sample)                                                  \
    HELIX_SAMPLE_TEST("CHECK_FALSE",                                                               \
                      Catch::ResultDisposition::ContinueOnFailure |                                \
                          Catch::ResultDisposition::FalseTest,                                     \
                      samples, expr, sample)
