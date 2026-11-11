// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "print_health_log.h"

#include "../catch_amalgamated.hpp"

using helix::format_print_health;
using helix::PrintHealthLog;
using helix::PrintHealthSample;

TEST_CASE("format_print_health reports rates, and zeros over no interval", "[print_health]") {
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
    later.render_ns += 50 * 12'000'000ULL;
    later.slow_frames += 3;
    later.flushed_px += 10 * 384000;
    later.screen_px = 384000;
    later.rss_kb = 33 * 1024;
    later.peak_rss_kb = 41 * 1024;
    later.mem_available_kb = 393 * 1024;
    later.swap_free_kb = 2048;

    CHECK(format_print_health(earlier, later) ==
          "cpu=35.0% main=15.0% frames=2.5/s render=12.0ms slow=3 flushed=0.50 screens/s "
          "rss=33MB peak_rss=41MB mem_avail=393MB swap_free=2MB");

    const PrintHealthSample same; // no elapsed time and no screen
    CHECK(format_print_health(same, same) ==
          "cpu=0.0% main=0.0% frames=0.0/s render=0.0ms slow=0 flushed=0.00 screens/s rss=0MB "
          "peak_rss=0MB mem_avail=0MB swap_free=0MB");
}

TEST_CASE("FrameCostCounter times drawn frames and counts the slow ones", "[print_health]") {
    helix::FrameCostCounter c;
    c.render_ready(5'000'000); // no frame in progress: ignored
    c.render_start(10'000'000);
    c.render_ready(10'000'000 + helix::FrameCostCounter::kSlowRenderNs); // at the line: not slow
    c.render_start(100'000'000);
    c.render_ready(100'000'000 + helix::FrameCostCounter::kSlowRenderNs + 1);
    CHECK(c.frames() == 2);
    CHECK(c.slow_frames() == 1);
    CHECK(c.render_ns() == 2 * helix::FrameCostCounter::kSlowRenderNs + 1);
    c.render_ready(200'000'000); // a READY must pair with its own START
    CHECK(c.frames() == 2);
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

TEST_CASE_METHOD(LVGLTestFixture, "print health frames count renders, not refresh ticks",
                 "[print_health]") {
    lv_display_t* display = lv_display_get_default();
    REQUIRE(display != nullptr);
    helix::attach_print_health_counters(display);
    lv_refr_now(display); // settle anything already invalid
    const auto settled_sample = helix::read_print_health_sample(display);
    const uint64_t settled = settled_sample.frames;

    lv_refr_now(display); // a refresh with nothing to draw
    CHECK(helix::read_print_health_sample(display).frames == settled);

    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    const auto drawn = helix::read_print_health_sample(display);
    CHECK(drawn.frames == settled + 1);
    CHECK(drawn.render_ns > settled_sample.render_ns);

    // Flushed pixels are the refreshed area: the invalidated areas after LVGL joins them.
    lv_area_t box = {10, 20, 109, 69}; // 100 x 50
    lv_obj_invalidate_area(lv_screen_active(), &box);
    lv_refr_now(display);
    const auto one_box = helix::read_print_health_sample(display);
    CHECK(one_box.flushed_px - drawn.flushed_px == 100 * 50);
    lv_area_t overlap = {60, 20, 159, 69}; // the same size, shifted half its width
    lv_obj_invalidate_area(lv_screen_active(), &box);
    lv_obj_invalidate_area(lv_screen_active(), &overlap);
    lv_refr_now(display);
    CHECK(helix::read_print_health_sample(display).flushed_px - one_box.flushed_px == 150 * 50);
}
