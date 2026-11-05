// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_health_log.h"

#include "../catch_amalgamated.hpp"

using helix::format_print_health;
using helix::PrintHealthLog;
using helix::PrintHealthSample;

TEST_CASE("format_print_health reports rates over the interval", "[print_health]") {
    PrintHealthSample earlier;
    earlier.process_cpu_ns = 1'000'000'000;
    earlier.main_thread_cpu_ns = 500'000'000;
    earlier.wall_ns = 5'000'000'000;
    earlier.frames = 100;
    earlier.flushed_px = 1000;

    PrintHealthSample later = earlier;
    later.process_cpu_ns += 7'000'000'000;
    later.main_thread_cpu_ns += 3'000'000'000;
    later.wall_ns += 20'000'000'000;
    later.frames += 50;
    later.flushed_px += 10 * 384000;
    later.screen_px = 384000;
    later.rss_kb = 33 * 1024;
    later.mem_available_kb = 393 * 1024;
    later.swap_free_kb = 2048;

    CHECK(format_print_health(earlier, later) ==
          "cpu=35.0% main=15.0% frames=2.5/s flushed=0.50 screens/s rss=33MB mem_avail=393MB "
          "swap_free=2MB");
}

TEST_CASE("format_print_health survives no elapsed time and no screen", "[print_health]") {
    const PrintHealthSample same;
    CHECK(format_print_health(same, same) ==
          "cpu=0.0% main=0.0% frames=0.0/s flushed=0.00 screens/s rss=0MB mem_avail=0MB "
          "swap_free=0MB");
}

TEST_CASE("PrintHealthLog logs once per interval, and only during a print", "[print_health]") {
    int samples = 0;
    PrintHealthLog log([&samples] {
        ++samples;
        PrintHealthSample s;
        s.wall_ns = static_cast<uint64_t>(samples) * 60'000'000'000ULL;
        s.process_cpu_ns = static_cast<uint64_t>(samples) * 6'000'000'000ULL;
        return s;
    });
    const uint32_t interval = PrintHealthLog::kIntervalMs;

    SECTION("nothing is sampled or logged while idle") {
        CHECK_FALSE(log.tick(0, false));
        CHECK_FALSE(log.tick(interval * 2, false));
        CHECK(samples == 0);
    }

    SECTION("the first active tick is a baseline, and each interval gives one line") {
        CHECK_FALSE(log.tick(1000, true));
        CHECK(samples == 1);
        CHECK_FALSE(log.tick(1000 + interval - 1, true));
        CHECK(samples == 1); // not due, so nothing is read
        const auto line = log.tick(1000 + interval, true);
        REQUIRE(line);
        CHECK(line->rfind("cpu=10.0%", 0) == 0);
        CHECK_FALSE(log.tick(1000 + interval + 1, true));
    }

    SECTION("time between prints is never measured") {
        CHECK_FALSE(log.tick(0, true));
        CHECK_FALSE(log.tick(1000, false));
        CHECK_FALSE(log.tick(interval * 5, true)); // a new baseline, not a line
        CHECK(log.tick(interval * 6, true));
    }

    SECTION("the tick counter wrapping does not stop the log") {
        const uint32_t near_wrap = UINT32_MAX - 1000;
        CHECK_FALSE(log.tick(near_wrap, true));
        CHECK(log.tick(near_wrap + interval, true));
    }
}
