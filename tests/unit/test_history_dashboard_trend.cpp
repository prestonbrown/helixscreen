// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_history_data.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::count_trend_buckets;

// Moonraker leaves end_time null, parsed as 0, on interrupted and in_progress
// rows; the trend chart places those by start_time (prestonbrown/helixscreen#1713).

namespace {

constexpr double kNow = 1'800'000'000.0;
constexpr double kDay = 86400.0;

PrintHistoryJob job_at(double start_time, double end_time) {
    PrintHistoryJob job;
    job.start_time = start_time;
    job.end_time = end_time;
    return job;
}

} // namespace

TEST_CASE("count_trend_buckets: an end_time-0 job lands by its start_time",
          "[history][dashboard][trend]") {
    const auto counts = count_trend_buckets({job_at(kNow - 3600.0, 0.0)}, kNow, 7, kDay, false);
    REQUIRE(counts == std::vector<int>{0, 0, 0, 0, 0, 0, 1});
}

TEST_CASE("count_trend_buckets: a finished job lands by its end_time",
          "[history][dashboard][trend]") {
    const auto counts =
        count_trend_buckets({job_at(kNow - 2.5 * kDay, kNow - 1.5 * kDay)}, kNow, 7, kDay, false);
    REQUIRE(counts == std::vector<int>{0, 0, 0, 0, 0, 1, 0});
}

TEST_CASE("count_trend_buckets: a job with no timestamps is not counted",
          "[history][dashboard][trend]") {
    const auto counts = count_trend_buckets({job_at(0.0, 0.0)}, kNow, 7, kDay, true);
    REQUIRE(counts == std::vector<int>(7, 0));
}

TEST_CASE("count_trend_buckets: all time spans from the oldest job and keeps it",
          "[history][dashboard][trend]") {
    // The oldest job is an interrupted one, so only its start_time says how far
    // back the span reaches.
    const std::vector<PrintHistoryJob> jobs = {
        job_at(kNow - 120 * kDay, 0.0),
        job_at(kNow - 66 * kDay, kNow - 65 * kDay),
        job_at(kNow - 2 * kDay, kNow - kDay),
    };
    const auto counts = count_trend_buckets(jobs, kNow, 12, kDay, true);
    REQUIRE(counts == std::vector<int>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1});
}

TEST_CASE("count_trend_buckets: a fixed window drops a job older than its span",
          "[history][dashboard][trend]") {
    const auto counts =
        count_trend_buckets({job_at(kNow - 8 * kDay, kNow - 7.5 * kDay)}, kNow, 7, kDay, false);
    REQUIRE(counts == std::vector<int>(7, 0));
}
