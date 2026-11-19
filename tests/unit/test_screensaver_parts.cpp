// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/refresh_period_hold_test_access.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "lv_draw_buf_guard.h"
#include "printer_image_manager.h"
#include "refresh_period_hold.h"
#include "screensaver_base.h"
#include "screensaver_buffer.h"
#include "screensaver_canvas.h"
#include "screensaver_clock.h"
#include "screensaver_clock_reading.h"
#include "screensaver_clock_saver.h"
#include "screensaver_fast_math.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_frame.h"
#include "screensaver_frame_timer.h"
#include "screensaver_image.h"
#include "screensaver_motion.h"
#include "screensaver_overlay.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_sim_saver.h"
#include "screensaver_vec.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::ClockPrintPhase;
using helix::ui::DirtyRect;
using helix::ui::kSaverMaxDirtyAreas;
using helix::ui::PixelWriter;
using helix::ui::SaverCanvas;
using helix::ui::SaverFrameTimer;
using helix::ui::SaverOverlay;

namespace {

bool box_covers(const std::vector<DirtyRect>& boxes, const DirtyRect& r) {
    return std::any_of(boxes.begin(), boxes.end(),
                       [&](const DirtyRect& b) { return helix::test::contains(b, r); });
}

bool areas_cover_point(const std::vector<lv_area_t>& areas, int32_t x, int32_t y) {
    return std::any_of(areas.begin(), areas.end(), [&](const lv_area_t& a) {
        return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
    });
}

lv_area_t coords_of(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area;
}

constexpr uint32_t kProbePeriodsMs[] = {helix::ui::kSaverFastPeriod, 45, 50};

/// A saver that records what the base asks of it.
class ProbeSaver : public helix::ui::SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::STARFIELD;
    }
    using SaverBase::apply_level;

    std::optional<lv_color_format_t> format = LV_COLOR_FORMAT_XRGB8888;
    bool refuse = false;
    bool defer_levels = false;
    bool canvas_at_start = false;
    size_t level_at_start = 99;
    uint32_t first_random = 0;
    std::vector<std::string> calls;
    std::vector<uint32_t> frame_dts;
    std::vector<DirtyRect> frame_dirty;
    std::vector<size_t> level_requests;

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return format;
    }
    bool on_start() override {
        calls.emplace_back("start");
        canvas_at_start = canvas().obj() != nullptr;
        level_at_start = level();
        first_random = rng()();
        return !refuse;
    }
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override {
        calls.emplace_back("frame");
        frame_dts.push_back(dt_ms);
        dirty = frame_dirty;
    }
    void on_stop() override {
        calls.emplace_back("stop");
    }
    size_t ladder_size() const override {
        return std::size(kProbePeriodsMs);
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return kProbePeriodsMs[level];
    }
    void on_level_request(size_t level) override {
        level_requests.push_back(level);
        if (!defer_levels) {
            apply_level(level);
        }
    }
};

} // namespace

// ============================================================================
// merge_dirty_areas
// ============================================================================

TEST_CASE("merged dirty areas number at most the limit and cover every input",
          "[screensaver][screensaver_parts]") {
    std::minstd_rand rng(17);
    std::vector<DirtyRect> input;
    for (int i = 0; i < 180; i++) {
        const auto x = static_cast<int32_t>(rng() % 780);
        const auto y = static_cast<int32_t>(rng() % 460);
        input.push_back(
            {x, y, x + static_cast<int32_t>(rng() % 20), y + static_cast<int32_t>(rng() % 20)});
    }
    REQUIRE(input.size() > kSaverMaxDirtyAreas);

    std::vector<DirtyRect> merged = input;
    helix::ui::merge_dirty_areas(merged, kSaverMaxDirtyAreas);

    CHECK(merged.size() == kSaverMaxDirtyAreas);
    for (const DirtyRect& r : input) {
        CHECK(box_covers(merged, r));
    }
}

TEST_CASE("merging within the limit keeps the areas in order and drops empty ones",
          "[screensaver][screensaver_parts]") {
    std::vector<DirtyRect> areas = {{0, 0, 9, 9}, {}, {100, 100, 120, 110}, {50, 5, 60, 6}};
    helix::ui::merge_dirty_areas(areas, kSaverMaxDirtyAreas);
    REQUIRE(areas.size() == 3);
    CHECK(areas[0] == DirtyRect{0, 0, 9, 9});
    CHECK(areas[1] == DirtyRect{100, 100, 120, 110});
    CHECK(areas[2] == DirtyRect{50, 5, 60, 6});
}

TEST_CASE("merging joins the pair that adds the fewest pixels",
          "[screensaver][screensaver_parts]") {
    std::vector<DirtyRect> areas = {{0, 0, 9, 9}, {10, 0, 19, 9}, {500, 400, 509, 409}};
    helix::ui::merge_dirty_areas(areas, 2);
    REQUIRE(areas.size() == 2);
    CHECK(areas[0] == DirtyRect{0, 0, 19, 9});
    CHECK(areas[1] == DirtyRect{500, 400, 509, 409});
}

// ============================================================================
// SaverOverlay
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver overlay is an opaque black touch-absorbing child of the top layer",
                 "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    lv_obj_t* obj = overlay.obj();
    REQUIRE(obj != nullptr);
    CHECK(lv_obj_get_parent(obj) == lv_layer_top());
    CHECK(lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), lv_color_black()));
    CHECK(lv_obj_is_clickable(obj));
    CHECK_FALSE(lv_obj_is_scrollable(obj));

    overlay.make_transparent();
    CHECK(lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == LV_OPA_TRANSP);

    overlay.destroy();
    CHECK(overlay.obj() == nullptr);
    // Deleted on a later timer pass, hidden until then.
    CHECK(lv_obj_is_hidden(obj));
}

// ============================================================================
// SaverCanvas
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver canvas allocates at LVGL's stride for its format and starts opaque black",
                 "[screensaver][screensaver_parts]") {
    const lv_color_format_t cf = GENERATE(as<lv_color_format_t>{}, LV_COLOR_FORMAT_XRGB8888,
                                          LV_COLOR_FORMAT_ARGB8888, LV_COLOR_FORMAT_RGB565);
    const int32_t w = GENERATE(630, 800);
    CAPTURE(static_cast<int>(cf), w);
    constexpr int32_t H = 120;

    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), w, H, cf));
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas.obj());
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == cf);
    CHECK(canvas.format() == cf);
    CHECK(canvas.width() == w);
    CHECK(canvas.height() == H);
    CHECK(canvas.stride() == lv_draw_buf_width_to_stride(static_cast<uint32_t>(w), cf));
    CHECK(canvas.stride() == buf->header.stride);
    CHECK(canvas.buffer_size() == static_cast<size_t>(canvas.stride()) * H);
    CHECK(canvas.buffer_size() >= buf->data_size);

    const uint32_t bytes = lv_color_format_get_size(cf);
    size_t not_black = 0;
    for (int32_t y = 0; y < H; y++) {
        const uint8_t* row = canvas.data() + static_cast<size_t>(y) * canvas.stride();
        for (int32_t x = 0; x < w; x++) {
            const uint8_t* px = row + static_cast<size_t>(x) * bytes;
            if (bytes == 2) {
                not_black += (px[0] | px[1]) != 0 ? 1 : 0;
            } else {
                not_black += ((px[0] | px[1] | px[2]) != 0 || px[3] != 0xFF) ? 1 : 0;
            }
        }
    }
    CHECK(not_black == 0);

    canvas.release();
    overlay.destroy();
}

TEST_CASE_METHOD(LVGLTestFixture, "releasing a saver canvas hides it and frees its buffer",
                 "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), 64, 48, LV_COLOR_FORMAT_XRGB8888));
    lv_obj_t* obj = canvas.obj();

    canvas.release();

    CHECK(lv_obj_is_hidden(obj));
    CHECK(canvas.obj() == nullptr);
    CHECK(canvas.data() == nullptr);
    CHECK(canvas.buffer_size() == 0);
    overlay.destroy();
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "a saver canvas invalidates at most SAVER_MAX_DIRTY_AREAS areas covering every dirty box",
    "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), 800, 480, LV_COLOR_FORMAT_XRGB8888));
    const lv_area_t coords = coords_of(canvas.obj());

    std::vector<DirtyRect> dirty;
    for (int32_t i = 0; i < 200; i++) {
        // A stride-coprime scatter over the canvas, so every box stays on it.
        const int32_t x = (i * 37) % 790;
        const int32_t y = (i * 53) % 470;
        dirty.push_back({x, y, x + 2, y + 2});
    }
    const std::vector<DirtyRect> input = dirty;
    helix::test::InvalidatedAreas invalidated(lv_obj_get_display(canvas.obj()));

    canvas.invalidate(dirty);

    REQUIRE_FALSE(invalidated.areas.empty());
    CHECK(invalidated.areas.size() <= kSaverMaxDirtyAreas);
    for (const DirtyRect& r : input) {
        CHECK(areas_cover_point(invalidated.areas, coords.x1 + r.x1, coords.y1 + r.y1));
        CHECK(areas_cover_point(invalidated.areas, coords.x1 + r.x2, coords.y1 + r.y2));
    }
    canvas.release();
    overlay.destroy();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver canvas layer session invalidates only the areas it marked",
                 "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), 200, 100, LV_COLOR_FORMAT_ARGB8888));
    const lv_area_t coords = coords_of(canvas.obj());
    helix::test::InvalidatedAreas invalidated(lv_obj_get_display(canvas.obj()));

    lv_layer_t layer;
    canvas.begin_layer(&layer);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_white();
    dsc.bg_opa = LV_OPA_COVER;
    const lv_area_t rect = {10, 20, 29, 39};
    lv_draw_rect(&layer, &dsc, &rect);
    canvas.mark_dirty(10, 20, 29, 39);
    canvas.finish_layer(&layer);

    REQUIRE(invalidated.areas.size() == 1);
    CHECK(invalidated.areas[0].x1 == coords.x1 + 10);
    CHECK(invalidated.areas[0].y1 == coords.y1 + 20);
    CHECK(invalidated.areas[0].x2 == coords.x1 + 29);
    CHECK(invalidated.areas[0].y2 == coords.y1 + 39);
    // The session drew the rect into the buffer.
    const uint8_t* px = canvas.data() + 25 * static_cast<size_t>(canvas.stride()) + 15 * 4;
    CHECK(px[0] == 0xFF);
    CHECK(px[1] == 0xFF);
    CHECK(px[2] == 0xFF);
    canvas.release();
    overlay.destroy();
}

// ============================================================================
// SaverFrameTimer
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver frame timer calls back with the time since its previous call",
                 "[screensaver][screensaver_parts]") {
    SaverFrameTimer timer;
    std::vector<uint32_t> dts;
    timer.start(20, [&](uint32_t dt_ms) { dts.push_back(dt_ms); });
    REQUIRE(timer.timer() != nullptr);
    CHECK(timer.timer()->period == 20);

    lv_tick_inc(25);
    fire_timer(timer.timer());
    lv_tick_inc(7);
    fire_timer(timer.timer());

    CHECK(dts == std::vector<uint32_t>{25, 7});
    timer.cancel();
}

TEST_CASE_METHOD(LVGLTestFixture, "changing a saver frame timer's period keeps motion continuous",
                 "[screensaver][screensaver_parts]") {
    SaverFrameTimer timer;
    uint32_t last_dt = 0;
    timer.start(16, [&](uint32_t dt_ms) { last_dt = dt_ms; });
    lv_tick_inc(10);
    timer.set_period(33);
    CHECK(timer.timer()->period == 33);
    lv_tick_inc(15);
    fire_timer(timer.timer());
    CHECK(last_dt == 25);
    timer.cancel();
}

TEST_CASE_METHOD(LVGLTestFixture, "cancelling a saver frame timer neuters it",
                 "[screensaver][screensaver_parts]") {
    SaverFrameTimer timer;
    int calls = 0;
    timer.start(16, [&](uint32_t) { calls++; });
    lv_timer_t* raw = timer.timer();
    REQUIRE(raw != nullptr);

    timer.cancel();

    CHECK(timer.timer() == nullptr);
    // lv_timer_cancel_safe() clears the callback; lv_timer_handler deletes the timer later.
    CHECK(raw->timer_cb == nullptr);
    CHECK(calls == 0);
}

// ============================================================================
// SaverBase
// ============================================================================

TEST_CASE_METHOD(
    LVGLTestFixture,
    "a saver starts with its overlay, canvas, seeded random sequence and a 16 ms timer",
    "[screensaver][screensaver_parts]") {
    // With no period configured, level 0 runs at kSaverFastPeriod whatever the display refresh.
    ScopedRefreshPeriod refresh(20);
    ScopedGlobalRefreshHold clean_hold;
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    SaverTestAccess::set_fixed_seed(saver, 7);

    saver.start();

    REQUIRE(saver.is_active());
    CHECK(saver.canvas_at_start);
    lv_obj_t* overlay = SaverTestAccess::overlay(saver);
    REQUIRE(overlay != nullptr);
    CHECK(lv_obj_get_parent(overlay) == lv_layer_top());
    CHECK(lv_obj_get_style_bg_opa(overlay, LV_PART_MAIN) == LV_OPA_TRANSP);
    lv_obj_t* canvas = SaverTestAccess::canvas(saver);
    REQUIRE(canvas != nullptr);
    CHECK(lv_obj_get_parent(canvas) == overlay);
    REQUIRE(SaverTestAccess::timer(saver) != nullptr);
    CHECK(SaverTestAccess::timer(saver)->period == helix::ui::kSaverFastPeriod);
    std::minstd_rand expected(7);
    CHECK(saver.first_random == expected());
    CHECK(saver.calls == std::vector<std::string>{"start"});
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver without a canvas keeps its overlay opaque and invalidates nothing",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    saver.format = std::nullopt;
    saver.frame_dirty = {{1, 1, 2, 2}};

    saver.start();

    REQUIRE(saver.is_active());
    CHECK_FALSE(saver.canvas_at_start);
    CHECK(SaverTestAccess::canvas(saver) == nullptr);
    CHECK(lv_obj_get_style_bg_opa(SaverTestAccess::overlay(saver), LV_PART_MAIN) == LV_OPA_COVER);
    helix::test::InvalidatedAreas invalidated(lv_display_get_default());
    advance_saver(saver, 16);
    CHECK(saver.frame_dts == std::vector<uint32_t>{16});
    CHECK(invalidated.areas.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a saver that refuses to start leaves nothing running",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    saver.refuse = true;

    saver.start();

    CHECK_FALSE(saver.is_active());
    CHECK(SaverTestAccess::timer(saver) == nullptr);
    CHECK(SaverTestAccess::overlay(saver) == nullptr);
    CHECK(SaverTestAccess::canvas(saver) == nullptr);
    CHECK(SaverTestAccess::draw_buf_size(saver) == 0);
    CHECK(saver.calls == std::vector<std::string>{"start"});
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver frame gets the elapsed time and its dirty areas are invalidated",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    saver.frame_dirty = {{5, 6, 7, 8}};
    saver.start();
    REQUIRE(saver.is_active());
    const lv_area_t coords = coords_of(SaverTestAccess::canvas(saver));
    helix::test::InvalidatedAreas invalidated(lv_display_get_default());

    advance_saver(saver, 30);

    CHECK(saver.frame_dts == std::vector<uint32_t>{30});
    REQUIRE(invalidated.areas.size() == 1);
    CHECK(invalidated.areas[0].x1 == coords.x1 + 5);
    CHECK(invalidated.areas[0].y1 == coords.y1 + 6);
    CHECK(invalidated.areas[0].x2 == coords.x1 + 7);
    CHECK(invalidated.areas[0].y2 == coords.y1 + 8);
}

TEST_CASE_METHOD(LVGLTestFixture, "a saver starts at its start level, clamped to its ladder",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};

    saver.set_start_level(2);
    saver.start();
    REQUIRE(saver.is_active());
    CHECK(saver.level() == 2);
    CHECK(saver.level_at_start == 2);
    CHECK(SaverTestAccess::timer(saver)->period == 50);
    CHECK(saver.level_count() == 3);
    saver.stop();

    saver.set_start_level(9);
    saver.start();
    REQUIRE(saver.is_active());
    CHECK(saver.level() == 2);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "a level request goes through the saver, which moves its timer and the held refresh",
    "[screensaver][screensaver_parts]") {
    helix::ScopedTimerPeriods restore;
    helix::ScopedTimerPeriods::set(40, 40);
    ScopedGlobalRefreshHold clean_hold;
    helix::RefreshPeriodHold& hold = helix::active_refresh_period_hold();
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};

    saver.request_level(1);
    CHECK(saver.level_requests.empty()); // not running

    hold.acquire();
    saver.start();
    REQUIRE(saver.is_active());
    CHECK(SaverTestAccess::timer(saver)->period == helix::ui::kSaverFastPeriod);
    CHECK(helix::default_refr_timer_period() == helix::ui::kSaverFastPeriod);

    saver.request_level(1);
    CHECK(saver.level_requests == std::vector<size_t>{1});
    CHECK(saver.level() == 1);
    CHECK(SaverTestAccess::timer(saver)->period == 45);
    CHECK(helix::default_refr_timer_period() == 45);

    saver.defer_levels = true;
    saver.request_level(2);
    CHECK(saver.level_requests == std::vector<size_t>{1, 2});
    CHECK(saver.level() == 1);
    CHECK(SaverTestAccess::timer(saver)->period == 45);
    saver.apply_level(2);
    CHECK(saver.level() == 2);
    CHECK(SaverTestAccess::timer(saver)->period == 50);
    CHECK(helix::default_refr_timer_period() == 50);
    CHECK(saver.level_period_ms(9) == 50);

    saver.request_level(9);
    CHECK(saver.level_requests.back() == 2);

    saver.stop();
    hold.release();
    CHECK(helix::default_refr_timer_period() == 40);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a configured refresh period replaces level 0's period and no other level's",
                 "[screensaver][screensaver_parts]") {
    helix::ScopedTimerPeriods restore;
    helix::ScopedTimerPeriods::set(40, 40);
    ScopedGlobalRefreshHold clean_hold;
    helix::RefreshPeriodHold& hold = helix::active_refresh_period_hold();
    hold.set_period(20);
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    CHECK(saver.level_period_ms(0) == 20);
    CHECK(saver.level_period_ms(1) == 45);
    CHECK(saver.level_period_ms(2) == 50);

    hold.acquire();
    saver.start();
    REQUIRE(saver.is_active());
    CHECK(SaverTestAccess::timer(saver)->period == 20);
    CHECK(helix::default_refr_timer_period() == 20);

    saver.request_level(1);
    CHECK(SaverTestAccess::timer(saver)->period == 45);
    CHECK(helix::default_refr_timer_period() == 45);

    saver.stop();
    hold.release();
    CHECK(helix::default_refr_timer_period() == 40);
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "stopping a saver cancels its timer, hides and frees its canvas, and queues its overlay",
    "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    saver.start();
    REQUIRE(saver.is_active());
    lv_obj_t* canvas = SaverTestAccess::canvas(saver);
    lv_obj_t* overlay = SaverTestAccess::overlay(saver);
    lv_timer_t* timer = SaverTestAccess::timer(saver);

    saver.stop();

    CHECK_FALSE(saver.is_active());
    CHECK(timer->timer_cb == nullptr);
    CHECK(SaverTestAccess::timer(saver) == nullptr);
    CHECK(lv_obj_is_hidden(canvas));
    CHECK(SaverTestAccess::draw_buf_size(saver) == 0);
    CHECK(SaverTestAccess::overlay(saver) == nullptr);
    CHECK(lv_obj_is_hidden(overlay));
    CHECK(saver.calls == std::vector<std::string>{"start", "stop"});
}

TEST_CASE_METHOD(LVGLTestFixture, "a restarted saver replays its fixed seed",
                 "[screensaver][screensaver_parts]") {
    ProbeSaver saver;
    ScreensaverStopOnExit<ProbeSaver> stop_on_exit{saver};
    SaverTestAccess::set_fixed_seed(saver, 1234);
    saver.start();
    const uint32_t first = saver.first_random;
    saver.stop();
    saver.start();
    CHECK(saver.first_random == first);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver canvas frame describes its buffer in the matching pixel format",
                 "[screensaver][screensaver_parts]") {
    for (const auto& [cf, format] :
         {std::pair<lv_color_format_t, helix::ui::PixelFormat>{LV_COLOR_FORMAT_XRGB8888,
                                                               helix::ui::PixelFormat::XRGB8888},
          std::pair<lv_color_format_t, helix::ui::PixelFormat>{LV_COLOR_FORMAT_RGB565,
                                                               helix::ui::PixelFormat::RGB565}}) {
        INFO("canvas format " << static_cast<int>(cf));
        SaverOverlay overlay;
        overlay.create();
        SaverCanvas canvas;
        REQUIRE(canvas.create(overlay.obj(), 96, 40, cf));
        const helix::ui::FrameTarget frame = canvas.frame();
        CHECK(frame.format == format);
        CHECK(frame.data == canvas.data());
        CHECK(frame.stride == canvas.stride());
        CHECK(frame.w == 96u);
        CHECK(frame.h == 40u);
        canvas.release();
        overlay.destroy();
    }
    CHECK(helix::ui::pixel_format_for(LV_COLOR_FORMAT_RGB565) == helix::ui::PixelFormat::RGB565);
    CHECK(helix::ui::pixel_format_for(LV_COLOR_FORMAT_XRGB8888) ==
          helix::ui::PixelFormat::XRGB8888);
}
// A saver whose dirty areas cover nearly the whole canvas is cheaper to invalidate whole: a
// double-buffered display syncs whatever the partial invalidation left out, strip by strip,
// on every frame. Coverage counts covered pixels: each area is rendered and flushed on its
// own, so scattered small areas cost their sum however far apart they lie.
TEST_CASE("a near-full dirty box invalidates the whole canvas",
          "[screensaver][screensaver_parts]") {
    using helix::ui::covers_whole_canvas;
    using helix::ui::DirtyRect;

    SECTION("the whole canvas counts") {
        CHECK(covers_whole_canvas(DirtyRect{0, 0, 799, 479}, 800, 480));
    }
    SECTION("a near-whole box counts") {
        // 790 x 470 of 800 x 480 is 96.7%.
        CHECK(covers_whole_canvas(DirtyRect{5, 5, 794, 474}, 800, 480));
    }
    SECTION("a half-screen box does not") {
        CHECK_FALSE(covers_whole_canvas(DirtyRect{0, 0, 799, 239}, 800, 480));
    }
    SECTION("an empty box does not") {
        CHECK_FALSE(covers_whole_canvas(DirtyRect{}, 800, 480));
    }
    SECTION("a canvas with no area never promotes") {
        CHECK_FALSE(covers_whole_canvas(DirtyRect{0, 0, 10, 10}, 0, 0));
    }
}

// Two small boxes at opposite corners span the whole canvas between them while covering a
// handful of pixels, so deciding the whole-canvas promotion on their bounding box would
// turn almost any scattered frame into a full redraw.
// The coverage rule answers at compile time, so a caller can size a buffer or assert a
// budget from it without running anything.
static_assert(helix::ui::covers_whole_canvas(helix::ui::DirtyRect{0, 0, 799, 479}, 800, 480),
              "a full-canvas box covers the canvas");
static_assert(!helix::ui::covers_whole_canvas(helix::ui::DirtyRect{0, 0, 4, 4}, 800, 480),
              "a 5x5 box does not cover the canvas");

TEST_CASE("scattered dirty boxes do not promote a whole-canvas invalidate",
          "[screensaver][screensaver_parts]") {
    using helix::ui::covers_whole_canvas;
    using helix::ui::DirtyRect;

    const DirtyRect corners[] = {{0, 0, 4, 4}, {795, 475, 799, 479}};
    int64_t covered = 0;
    DirtyRect span;
    for (const DirtyRect& r : corners) {
        covered += r.area();
        span.add(r);
    }
    // The pair spans essentially the whole canvas but covers 50 of its 384000 pixels.
    REQUIRE(span.area() * 100 >= 800 * 480 * 90);
    REQUIRE(covered * 100 < 800 * 480 * 90);
    CHECK_FALSE(covers_whole_canvas(covered, 800, 480));
    CHECK(covers_whole_canvas(covered * 8000, 800, 480));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a saver canvas keeps scattered dirty boxes as separate small areas",
                 "[screensaver][screensaver_parts]") {
    SaverOverlay overlay;
    overlay.create();
    SaverCanvas canvas;
    REQUIRE(canvas.create(overlay.obj(), 800, 480, LV_COLOR_FORMAT_XRGB8888));
    // Invalidations are clipped to the object's coords, which a layout pass computes.
    lv_obj_update_layout(canvas.obj());
    helix::test::InvalidatedAreas invalidated(lv_obj_get_display(canvas.obj()));

    // The two boxes span the whole canvas between them.
    std::vector<DirtyRect> dirty = {{0, 0, 4, 4}, {795, 475, 799, 479}};
    canvas.invalidate(dirty);

    REQUIRE(invalidated.areas.size() == 2);
    for (const lv_area_t& a : invalidated.areas) {
        CHECK(lv_area_get_size(&a) == 25);
    }
    canvas.release();
    overlay.destroy();
}

// ============================================================================
// Shared frame, buffer, vector and constant helpers, each proven equal to the spellings the
// savers write by hand
// ============================================================================

namespace {

/// The dirty reduction aqua and high fly spell out after a step.
void aqua_reduce(std::vector<DirtyRect>& dirty, int32_t w, int32_t h) {
    const DirtyRect whole{0, 0, w - 1, h - 1};
    helix::ui::merge_dirty_areas(dirty, kSaverMaxDirtyAreas);
    int64_t covered = 0;
    for (const DirtyRect& b : dirty) {
        covered += b.area();
    }
    if (helix::ui::covers_whole_canvas(covered, w, h)) {
        dirty.assign(1, whole);
    }
}

struct HandVec {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

HandVec pipes_cross(HandVec a, HandVec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float pipes_length(HandVec v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

HandVec pipes_normalized(HandVec v) {
    const float len = pipes_length(v);
    if (len <= 0.0f) {
        return {};
    }
    return {v.x / len, v.y / len, v.z / len};
}

HandVec pipes_add_scaled(HandVec a, HandVec d, float s) {
    return {a.x + d.x * s, a.y + d.y * s, a.z + d.z * s};
}

float fireflies_length_sq(HandVec v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

HandVec fireflies_clamp_length(HandVec v, float max_len) {
    const float sq = fireflies_length_sq(v);
    if (sq <= max_len * max_len || sq == 0.0f) {
        return v;
    }
    const float k = max_len / std::sqrt(sq);
    return {v.x * k, v.y * k, v.z * k};
}

bool same(helix::ui::SaverVec3 a, HandVec b) {
    return screensaver_same_bits(a.x, b.x) && screensaver_same_bits(a.y, b.y) &&
           screensaver_same_bits(a.z, b.z);
}

/// `a + b * s` rounds twice through the operators, while the hand form is one
/// expression a compiler may contract into a fused multiply-add (clang does on
/// arm64), which rounds once. They differ by at most the product's rounding,
/// one epsilon of the terms, however much the sum cancels.
bool within_one_rounding(float sum, float addend, float product, float hand) {
    if (screensaver_same_bits(sum, hand))
        return true;
    const float bound =
        std::numeric_limits<float>::epsilon() * (std::fabs(addend) + std::fabs(product));
    return std::fabs(sum - hand) <= bound;
}
bool add_scaled_matches(helix::ui::SaverVec3 a, helix::ui::SaverVec3 b, float s, HandVec hand) {
    const helix::ui::SaverVec3 sum = a + b * s;
    return within_one_rounding(sum.x, a.x, b.x * s, hand.x) &&
           within_one_rounding(sum.y, a.y, b.y * s, hand.y) &&
           within_one_rounding(sum.z, a.z, b.z * s, hand.z);
}

} // namespace

TEST_CASE("a whole rect spans the frame and clipping keeps only what lies inside",
          "[screensaver][screensaver_parts]") {
    CHECK(DirtyRect::whole(800, 480) == DirtyRect{0, 0, 799, 479});
    CHECK(DirtyRect::whole(800, 480).area() == 800 * 480);
    CHECK(DirtyRect{-5, -3, 10, 4}.clipped(8, 6) == DirtyRect{0, 0, 7, 4});
    CHECK(DirtyRect{2, 1, 3, 2}.clipped(8, 6) == DirtyRect{2, 1, 3, 2});
    CHECK(DirtyRect{9, 0, 12, 3}.clipped(8, 6).empty());
    CHECK(DirtyRect{0, -9, 3, -1}.clipped(8, 6).empty());
    CHECK(DirtyRect{}.clipped(8, 6).empty());
}

TEST_CASE("reduce_dirty reaches aqua's and high fly's reduction on any dirty list",
          "[screensaver][screensaver_parts]") {
    std::minstd_rand rng(5);
    for (int trial = 0; trial < 400; trial++) {
        const int32_t w = 64 + static_cast<int32_t>(rng() % 200);
        const int32_t h = 48 + static_cast<int32_t>(rng() % 120);
        std::vector<DirtyRect> areas;
        const int n = static_cast<int>(rng() % 220);
        const int32_t big = trial % 3 == 0 ? w : 12;
        for (int i = 0; i < n; i++) {
            const auto x = static_cast<int32_t>(rng() % w);
            const auto y = static_cast<int32_t>(rng() % h);
            areas.push_back({x, y, x + static_cast<int32_t>(rng() % big),
                             y + static_cast<int32_t>(rng() % big)});
        }
        if (trial % 7 == 0) {
            areas.push_back({});
        }
        std::vector<DirtyRect> expect = areas;
        aqua_reduce(expect, w, h);
        helix::ui::reduce_dirty(areas, kSaverMaxDirtyAreas, w, h);
        REQUIRE(areas == expect);
    }
    std::vector<DirtyRect> nearly_all{{0, 0, 99, 94}};
    helix::ui::reduce_dirty(nearly_all, kSaverMaxDirtyAreas, 100, 100);
    CHECK(nearly_all == std::vector<DirtyRect>{DirtyRect::whole(100, 100)});
    std::vector<DirtyRect> little{{0, 0, 9, 9}};
    helix::ui::reduce_dirty(little, kSaverMaxDirtyAreas, 100, 100);
    CHECK(little == std::vector<DirtyRect>{{0, 0, 9, 9}});
}

TEST_CASE("scaled_extent rounds an extent up to whole blocks", "[screensaver][screensaver_parts]") {
    for (int32_t px = 0; px <= 1024; px++) {
        for (int32_t s = 1; s <= 8; s++) {
            REQUIRE(helix::ui::scaled_extent(px, s) == (px + s - 1) / s);
            REQUIRE(helix::ui::scaled_extent(px, s) * s >= px);
        }
    }
}

TEST_CASE("round_px rounds halves away from zero", "[screensaver][screensaver_parts]") {
    CHECK(helix::ui::round_px(2.5f) == 3);
    CHECK(helix::ui::round_px(-2.5f) == -3);
    CHECK(helix::ui::round_px(2.49f) == 2);
    for (int i = -20000; i <= 20000; i++) {
        const float v = static_cast<float>(i) * 0.013f;
        REQUIRE(helix::ui::round_px(v) == static_cast<int32_t>(std::lround(v)));
    }
}

TEST_CASE("frame_row and with_pixel_type address a frame's rows in its own pixel type",
          "[screensaver][screensaver_parts]") {
    std::vector<uint8_t> bytes(4 * 40, 0);
    for (const auto format : {helix::ui::PixelFormat::RGB565, helix::ui::PixelFormat::XRGB8888}) {
        helix::ui::FrameTarget frame{bytes.data(), 40, 8, 4, format};
        const size_t size = helix::ui::with_pixel_type(format, [&](auto px) {
            using Px = decltype(px);
            Px* row = helix::ui::frame_row<Px>(frame, 2);
            CHECK(reinterpret_cast<uint8_t*>(row) == bytes.data() + 80);
            return sizeof(Px);
        });
        CHECK(size == (format == helix::ui::PixelFormat::RGB565 ? 2u : 4u));
    }
}

TEST_CASE("buffer_bytes counts and release_buffers frees every vector given",
          "[screensaver][screensaver_parts]") {
    std::vector<uint32_t> a(10);
    std::vector<uint8_t> b(7);
    std::vector<DirtyRect> c;
    c.reserve(3);
    CHECK(helix::ui::buffer_bytes(a) == a.capacity() * 4);
    CHECK(helix::ui::buffer_bytes(a, b, c) ==
          a.capacity() * 4 + b.capacity() + c.capacity() * sizeof(DirtyRect));
    CHECK(helix::ui::buffer_bytes() == 0);
    helix::ui::release_buffers(a, b, c);
    CHECK(a.capacity() == 0);
    CHECK(b.capacity() == 0);
    CHECK(c.capacity() == 0);
    CHECK(helix::ui::buffer_bytes(a, b, c) == 0);
}

TEST_CASE("SaverVec3 computes what the pipes and fireflies vector code computes",
          "[screensaver][screensaver_parts]") {
    using helix::ui::SaverVec3;
    std::minstd_rand rng(17);
    auto any = [&rng] { return helix::ui::screensaver::random_between(rng, -50.0f, 50.0f); };
    for (int i = 0; i < 20000; i++) {
        const HandVec ha{any(), any(), any()};
        const HandVec hb{any(), any(), any()};
        const SaverVec3 a{ha.x, ha.y, ha.z};
        const SaverVec3 b{hb.x, hb.y, hb.z};
        const float s = any();
        REQUIRE(same(a + b, {ha.x + hb.x, ha.y + hb.y, ha.z + hb.z}));
        REQUIRE(same(a - b, {ha.x - hb.x, ha.y - hb.y, ha.z - hb.z}));
        REQUIRE(same(a * s, {ha.x * s, ha.y * s, ha.z * s}));
        REQUIRE(add_scaled_matches(a, b, s, pipes_add_scaled(ha, hb, s)));
        REQUIRE(same(helix::ui::cross(a, b), pipes_cross(ha, hb)));
        REQUIRE(
            screensaver_same_bits(helix::ui::dot(a, b), ha.x * hb.x + ha.y * hb.y + ha.z * hb.z));
        REQUIRE(screensaver_same_bits(helix::ui::length(a), pipes_length(ha)));
        REQUIRE(screensaver_same_bits(helix::ui::length_sq(a), fireflies_length_sq(ha)));
        REQUIRE(same(helix::ui::normalized_or(a, {}), pipes_normalized(ha)));
        const float cap = std::fabs(s);
        REQUIRE(same(helix::ui::clamp_length(a, cap), fireflies_clamp_length(ha, cap)));
    }
    CHECK(same(helix::ui::normalized_or({}, {0, 0, 1}), {0, 0, 1}));
    CHECK(same(helix::ui::clamp_length({}, 3.0f), {}));
}

TEST_CASE("SAVER_PI and SAVER_TWO_PI are every saver's float pi and 2 pi",
          "[screensaver][screensaver_parts]") {
    CHECK(screensaver_same_bits(helix::ui::kSaverPi, 3.14159265f));
    CHECK(screensaver_same_bits(helix::ui::kSaverPi, 3.14159265358979f));
    CHECK(screensaver_same_bits(helix::ui::kSaverPi, static_cast<float>(M_PI)));
    CHECK(screensaver_same_bits(helix::ui::kSaverTwoPi, 6.28318530717958f));
    CHECK(screensaver_same_bits(helix::ui::kSaverTwoPi, 2.0f * 3.14159265f));
}

// ============================================================================
// SimSaver, ClockSaver and the clock text decisions
// ============================================================================

namespace {

struct ProbeLevel {
    uint32_t period_ms;
};
constexpr ProbeLevel kProbeSimLevels[] = {{helix::ui::kSaverFastPeriod}, {33}, {50}};

/// What the probe sims were asked to do, in order.
std::vector<std::string>& sim_calls() {
    static std::vector<std::string> calls;
    return calls;
}

/// A sim on the plain contract that records each call and holds a buffer while it runs.
struct ProbeSim {
    static constexpr const auto& kLevels = kProbeSimLevels;

    void init(helix::ui::FrameTarget& frame, std::minstd_rand& rng, size_t level) {
        sim_calls().push_back("init " + std::to_string(level));
        width = frame.w;
        first_random = rng();
        held.resize(256);
    }
    void step(uint32_t dt_ms, helix::ui::FrameTarget& frame, std::minstd_rand& /*rng*/,
              std::vector<DirtyRect>& dirty) {
        sim_calls().push_back("step " + std::to_string(dt_ms));
        helix::ui::PixelWriter(frame).put(2, 3, {200, 10, 10});
        dirty.push_back({2, 3, 2, 3});
    }
    void request_level(size_t level) {
        sim_calls().push_back("level " + std::to_string(level));
    }
    void clear() {
        sim_calls().push_back("clear");
        helix::ui::release_buffer(held);
    }
    size_t heap_bytes() const {
        return helix::ui::buffer_bytes(held);
    }

    uint32_t width = 0;
    uint32_t first_random = 0;
    std::vector<uint8_t> held;
};

class ProbeSimSaver : public helix::ui::SimSaver<ProbeSim> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::HYPERSPACE;
    }
    helix::ui::ClockInput input() const {
        return read_input();
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::kSaverBuildCanvasFormat;
    }
};

/// A clock sim: draws from the frame, the rng, the level and a clock reading.
struct ProbeClockSim {
    static constexpr const auto& kLevels = kProbeSimLevels;

    void init(helix::ui::FrameTarget& /*frame*/, std::minstd_rand& /*rng*/, size_t level,
              const helix::ui::ClockReading& reading) {
        sim_calls().push_back("init " + std::to_string(level) + " at " +
                              std::to_string(reading.time.minute));
    }
    void step(uint32_t dt_ms, helix::ui::FrameTarget& /*frame*/, std::minstd_rand& /*rng*/,
              const helix::ui::ClockReading& reading, std::vector<DirtyRect>& dirty) {
        sim_calls().push_back("step " + std::to_string(dt_ms) + " at " +
                              std::to_string(reading.time.minute));
        dirty.push_back({0, 0, 9, 9});
        dirty.push_back({20, 20, 29, 29});
    }
    void request_level(size_t /*level*/) {}
    void clear() {
        sim_calls().push_back("clear");
    }
};

class ProbeClockSaver : public helix::ui::ClockSaver<ProbeClockSim> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::AQUA;
    }
    std::vector<std::vector<DirtyRect>> unders;
    int resets = 0;
    std::string first_line;

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::kLvDrawCanvasFormat;
    }
    void draw_clock_text(const std::vector<DirtyRect>& under,
                         std::vector<DirtyRect>& dirty) override {
        unders.push_back(under);
        first_line = clock_lines_now().first;
        dirty.push_back({40, 40, 41, 41});
    }
    void reset_clock_text() override {
        resets++;
    }
};

/// A clock sim with no rng that draws a flip-clock face.
struct ProbeFaceSim {
    static constexpr const auto& kLevels = kProbeSimLevels;

    void init(helix::ui::FrameTarget& /*frame*/, size_t /*level*/,
              const helix::ui::FlipFace& face) {
        shown = face;
    }
    void step(uint32_t /*dt_ms*/, helix::ui::FrameTarget& /*frame*/,
              const helix::ui::FlipFace& face, std::vector<DirtyRect>& /*dirty*/) {
        shown = face;
    }
    void request_level(size_t /*level*/) {}
    void clear() {}

    helix::ui::FlipFace shown;
};

class ProbeFaceSaver : public helix::ui::ClockSaver<ProbeFaceSim, helix::ui::FlipClockState> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::FLIP_CLOCK;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::kLvDrawCanvasFormat;
    }
    void draw_clock_text(const std::vector<DirtyRect>& /*under*/,
                         std::vector<DirtyRect>& /*dirty*/) override {}
    void reset_clock_text() override {}
};

/// A sim without an rng that reads one extra value each call.
struct ProbeNoRngSim {
    void init(helix::ui::FrameTarget& /*frame*/, size_t level, int extra) {
        got = static_cast<int>(level) * 100 + extra;
    }
    void step(uint32_t dt_ms, helix::ui::FrameTarget& /*frame*/, int extra,
              std::vector<DirtyRect>& dirty) {
        got = static_cast<int>(dt_ms) + extra;
        dirty.push_back({0, 0, 0, 0});
    }
    int got = 0;
};

/// A scene sim that reads the print: records the mode each call saw.
struct ProbeReadingSim {
    static constexpr const auto& kLevels = kProbeSimLevels;

    void init(helix::ui::FrameTarget& /*frame*/, std::minstd_rand& /*rng*/, size_t /*level*/,
              const helix::ui::ClockReading& reading) {
        modes.push_back(reading.mode);
    }
    void step(uint32_t /*dt_ms*/, helix::ui::FrameTarget& /*frame*/, std::minstd_rand& /*rng*/,
              const helix::ui::ClockReading& reading, std::vector<DirtyRect>& /*dirty*/) {
        modes.push_back(reading.mode);
    }
    void request_level(size_t /*level*/) {}
    void clear() {
        modes.clear();
    }

    std::vector<helix::ui::ClockMode> modes;
};

class ProbeReadingSaver : public helix::ui::SimSaver<ProbeReadingSim> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::HYPERSPACE;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::kSaverBuildCanvasFormat;
    }
};

helix::ui::ClockInput input_at(int32_t seconds_of_day) {
    helix::ui::ClockInput in;
    in.seconds_of_day = seconds_of_day;
    in.date = {2026, 10, 16, 5};
    return in;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a sim saver drives its sim through start, frames, levels and stop",
                 "[screensaver][screensaver_parts]") {
    ProbeSimSaver ss;
    ScreensaverStopOnExit<ProbeSimSaver> stop_on_exit{ss};
    CHECK(ss.level_count() == std::size(kProbeSimLevels));
    sim_calls().clear();
    SaverTestAccess::set_fixed_seed(ss, 9);
    ss.set_start_level(1);
    ss.start();
    REQUIRE(ss.is_active());
    // Cleared before init, so a sim starts from nothing however the last run ended.
    CHECK(sim_calls() == std::vector<std::string>{"clear", "init 1"});
    CHECK(ss.sim().width ==
          static_cast<uint32_t>(lv_display_get_horizontal_resolution(lv_display_get_default())));
    std::minstd_rand seeded(9);
    CHECK(ss.sim().first_random == seeded());
    CHECK(SaverTestAccess::timer(ss)->period == 33);
    CHECK(ss.sim().heap_bytes() > 0);

    sim_calls().clear();
    advance_saver(ss, 33);
    CHECK(sim_calls() == std::vector<std::string>{"step 33"});
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss));
    CHECK(PixelWriter(helix::test::frame_of(buf)).get(2, 3) == helix::ui::Rgb{200, 10, 10});

    sim_calls().clear();
    ss.request_level(7);
    CHECK(ss.level() == 2);
    CHECK(sim_calls() == std::vector<std::string>{"level 2"});
    CHECK(SaverTestAccess::timer(ss)->period == 50);

    sim_calls().clear();
    ss.stop();
    CHECK(sim_calls() == std::vector<std::string>{"clear"});
    CHECK(ss.sim().heap_bytes() == 0);
    CHECK(&SimSaverTestAccess::sim(ss) == &ss.sim());
}

TEST_CASE_METHOD(LVGLTestFixture, "a scene saver reads the print a frame before the card would",
                 "[screensaver][screensaver_parts]") {
    using helix::ui::ClockMode;
    ProbeReadingSaver ss;
    ScreensaverStopOnExit<ProbeReadingSaver> stop_on_exit{ss};
    helix::ui::ClockInput reported;
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    advance_saver(ss, 33);
    CHECK(ss.sim().modes == std::vector<ClockMode>{ClockMode::CLOCK, ClockMode::CLOCK});
    CHECK_FALSE(ss.print_card_shown());

    // The frame the source turns PRINTING, the sim draws it, and the card agrees.
    reported.print = {ClockPrintPhase::PRINTING, -1, 12};
    advance_saver(ss, 33);
    REQUIRE(ss.sim().modes.size() == 3);
    CHECK(ss.sim().modes.back() == ClockMode::PROGRESS);
    CHECK(ss.print_card_shown());
    CHECK(ss.print_card_face().mode == ClockMode::PROGRESS);

    // A saver started mid-print opens on the print.
    ss.stop();
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(ss.sim().modes == std::vector<ClockMode>{ClockMode::PROGRESS});
}

TEST_CASE_METHOD(LVGLTestFixture, "a sim without a reading still starts and steps",
                 "[screensaver][screensaver_parts]") {
    static_assert(!helix::ui::sim_inits_with_v<ProbeSim, const helix::ui::ClockReading&>);
    static_assert(helix::ui::sim_inits_with_v<ProbeReadingSim, const helix::ui::ClockReading&>);
    static_assert(!helix::ui::sim_inits_with_v<ProbeReadingSim>);
    ProbeSimSaver ss;
    ScreensaverStopOnExit<ProbeSimSaver> stop_on_exit{ss};
    // A running print changes nothing for a sim that does not read it.
    ss.set_input_source([] {
        helix::ui::ClockInput in;
        in.print = {ClockPrintPhase::PRINTING, 600, 40};
        return in;
    });
    sim_calls().clear();
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(sim_calls() == std::vector<std::string>{"clear", "init 0"});
    advance_saver(ss, 20);
    CHECK(sim_calls() == std::vector<std::string>{"clear", "init 0", "step 20"});
}

TEST_CASE("ladder_period reads a table's rows and clamps to its last rung",
          "[screensaver][screensaver_parts]") {
    CHECK(helix::ui::ladder_period(kProbeSimLevels, 0) == helix::ui::kSaverFastPeriod);
    CHECK(helix::ui::ladder_period(kProbeSimLevels, 1) == 33);
    CHECK(helix::ui::ladder_period(kProbeSimLevels, 2) == 50);
    CHECK(helix::ui::ladder_period(kProbeSimLevels, 99) == 50);
}

TEST_CASE("a sim that draws no random numbers is driven without an rng",
          "[screensaver][screensaver_parts]") {
    static_assert(helix::ui::sim_inits_with_v<ProbeNoRngSim, int>);
    static_assert(helix::ui::sim_steps_with_v<ProbeNoRngSim, int>);
    static_assert(!helix::ui::sim_inits_with_v<ProbeNoRngSim>);
    static_assert(helix::ui::sim_inits_with_v<ProbeSim>);
    static_assert(helix::ui::sim_has_heap_bytes_v<ProbeSim>);
    static_assert(!helix::ui::sim_has_heap_bytes_v<ProbeNoRngSim>);
    helix::test::SimRun<ProbeNoRngSim> run(1, 8, 8);
    run.init(2, 7);
    CHECK(run.sim.got == 207);
    run.step(16, 5);
    CHECK(run.sim.got == 21);
    CHECK(run.dirty.size() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a clock saver steps its clock from its source and redraws text over what changed",
                 "[screensaver][screensaver_parts]") {
    ProbeClockSaver ss;
    ScreensaverStopOnExit<ProbeClockSaver> stop_on_exit{ss};
    Screensaver& as_saver = ss;
    CHECK(as_saver.clock_input_sink() == static_cast<helix::ui::ClockInputSink*>(&ss));
    int32_t now = helix::test::hms(10, 8, 59);
    ss.set_input_source([&now] { return input_at(now); });
    sim_calls().clear();
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(sim_calls() == std::vector<std::string>{"clear", "init 0 at 8"});
    CHECK(ss.resets == 1);
    REQUIRE(ss.unders.size() == 1);
    CHECK(ss.unders[0].empty());
    CHECK_FALSE(ss.first_line.empty());
    CHECK(ss.clock().reading().time.minute == 8);

    now = helix::test::hms(10, 9, 0);
    sim_calls().clear();
    advance_saver(ss, 400);
    CHECK(sim_calls() == std::vector<std::string>{"step 400 at 9"});
    REQUIRE(ss.unders.size() == 2);
    // The text sees exactly what the sim repainted, not its own boxes.
    CHECK(ss.unders[1] == std::vector<DirtyRect>{{0, 0, 9, 9}, {20, 20, 29, 29}});

    sim_calls().clear();
    ss.stop();
    CHECK(sim_calls() == std::vector<std::string>{"clear"});
    CHECK(ss.resets == 2);
}

TEST_CASE("every saver reads the print from its source", "[screensaver][screensaver_parts]") {
    ProbeSimSaver ss;
    Screensaver& as_saver = ss;
    helix::ui::ClockInputSink* sink = as_saver.clock_input_sink();
    REQUIRE(sink != nullptr);
    CHECK(ss.input().print.phase == ClockPrintPhase::NONE);
    sink->set_input_source([] {
        helix::ui::ClockInput in;
        in.print = {ClockPrintPhase::PAUSED, 600, 42};
        return in;
    });
    CHECK(ss.input().print.phase == ClockPrintPhase::PAUSED);
    CHECK(ss.input().print.progress_pct == 42);
}

TEST_CASE_METHOD(LVGLTestFixture, "a clock saver hands a face-drawing sim its state's face",
                 "[screensaver][screensaver_parts]") {
    ProbeFaceSaver ss;
    ScreensaverStopOnExit<ProbeFaceSaver> stop_on_exit{ss};
    ss.set_input_source([] { return input_at(helix::test::hms(14, 25)); });
    ss.start();
    REQUIRE(ss.is_active());
    advance_saver(ss, 16);
    CHECK(SimSaverTestAccess::sim(ss).shown.cards == ss.clock().face().cards);
}

TEST_CASE("clock drift moves as clock_drift_at says and reports when it moved",
          "[screensaver][screensaver_parts]") {
    const DirtyRect bounds{200, 150, 599, 329};
    helix::ui::ClockDrift drift;
    drift.set_range(bounds, 800, 480);
    int32_t max_dx = 0;
    int32_t max_dy = 0;
    helix::ui::clock_drift_range(bounds, 800, 480, max_dx, max_dy);
    CHECK(drift.max_dx == max_dx);
    CHECK(drift.max_dy == max_dy);
    REQUIRE(max_dx > 0);

    // The aqua and high fly blocks, run beside it.
    uint32_t elapsed = 0;
    int32_t drift_x = 0;
    int32_t drift_y = 0;
    int moves = 0;
    for (int frame = 0; frame < 4000; frame++) {
        const uint32_t dt = helix::test::kUnevenFrameMs[frame % 5] * 300;
        elapsed += dt;
        int32_t dx = 0;
        int32_t dy = 0;
        helix::ui::clock_drift_at(elapsed, max_dx, max_dy, dx, dy);
        const bool moved = dx != drift_x || dy != drift_y;
        drift_x = dx;
        drift_y = dy;
        REQUIRE(drift.step(dt) == moved);
        REQUIRE(drift.dx == drift_x);
        REQUIRE(drift.dy == drift_y);
        REQUIRE(drift.elapsed_ms == elapsed);
        moves += moved;
    }
    CHECK(moves > 10);
    drift.reset();
    CHECK(drift.elapsed_ms == 0);
    CHECK(drift.dx == 0);
    CHECK(drift.max_dx == 0);
}

TEST_CASE("clock faces are the same when time and format match, and progress when shown",
          "[screensaver][screensaver_parts]") {
    using helix::ui::ClockMode;
    using helix::ui::ClockReading;
    const ClockReading a = helix::test::reading_at(helix::test::hms(9, 41));
    ClockReading b = a;
    CHECK(helix::ui::clock_face_same(a, b, true));
    b.progress_pct = 40;
    CHECK(helix::ui::clock_face_same(a, b, false));
    CHECK_FALSE(helix::ui::clock_face_same(a, b, true));
    b = a;
    b.mode = ClockMode::COUNTDOWN;
    CHECK(helix::ui::clock_face_same(a, b, false));
    CHECK_FALSE(helix::ui::clock_face_same(a, b, true));
    b = helix::test::reading_at(helix::test::hms(9, 42));
    CHECK_FALSE(helix::ui::clock_face_same(a, b, false));
    b = helix::test::reading_at(helix::test::hms(9, 41), true);
    CHECK_FALSE(helix::ui::clock_face_same(a, b, false));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "clock lines read the print while one is on the readout and the date otherwise",
                 "[screensaver][screensaver_parts]") {
    using helix::ui::ClockMode;
    using helix::ui::ClockReading;
    ClockReading clock = helix::test::reading_at(helix::test::hms(21, 41));
    clock.date = {2019, 10, 21, 1};
    const helix::ui::ClockLines day = helix::ui::clock_lines(clock);
    const helix::ui::ClockDateText date = helix::ui::clock_date_text(clock.date);
    CHECK(day.first == date.date);
    CHECK(day.second == date.weekday);
    CHECK(day.meridiem == helix::ui::clock_meridiem_text(clock.time.meridiem));
    CHECK_FALSE(day.meridiem.empty());

    ClockReading print = clock;
    print.mode = ClockMode::COUNTDOWN;
    print.countdown_s = helix::test::hms(1, 12);
    print.eta_minute_of_day = 22 * 60 + 53;
    const helix::ui::ClockLines eta = helix::ui::clock_lines(print);
    const helix::ui::ClockPrintText text = helix::ui::clock_print_text(print);
    CHECK(eta.first == text.eta);
    CHECK(eta.second == text.left);

    helix::ui::ClockTextCache cache;
    const helix::ui::ClockLines* first = &cache.get(clock);
    CHECK(first->first == day.first);
    // A second later the text is the same, so nothing is rebuilt.
    ClockReading later = clock;
    later.time.second = 7;
    CHECK(cache.get(later).first == day.first);
    CHECK(cache.get(print).first == text.eta);
    cache.reset();
    CHECK(cache.get(clock).second == day.second);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a label needs drawing when its text or box changes or a repaint crosses it",
                 "[screensaver][screensaver_parts]") {
    SaverCanvas canvas;
    REQUIRE(canvas.create(lv_screen_active(), 200, 80, helix::ui::kLvDrawCanvasFormat));
    helix::ui::SaverLabel label;
    const DirtyRect box{20, 20, 119, 49};
    CHECK(label.needs_draw("12:34", box, {}));
    std::vector<DirtyRect> dirty;
    REQUIRE(label.draw(canvas, "12:34", box, {255, 255, 255}, LV_OPA_COVER, LV_TEXT_ALIGN_LEFT, {},
                       dirty));
    CHECK_FALSE(label.needs_draw("12:34", box, {}));
    CHECK_FALSE(label.needs_draw("12:34", box, {{150, 0, 160, 10}}));
    CHECK(label.needs_draw("12:34", box, {{100, 40, 130, 60}}));
    CHECK(label.needs_draw("12:35", box, {}));
    CHECK(label.needs_draw("12:34", {21, 20, 120, 49}, {}));
    // A halo reaches past the box, so a repaint beside it counts.
    label.set_halo({{0, 0, 0}, LV_OPA_50, 4});
    CHECK(label.needs_draw("12:34", box, {{122, 30, 124, 32}}));
    CHECK_FALSE(label.needs_draw("12:34", box, {{125, 30, 127, 32}}));
    dirty.clear();
    CHECK_FALSE(label.draw(canvas, "12:34", box, {255, 255, 255}, LV_OPA_COVER, LV_TEXT_ALIGN_LEFT,
                           {{150, 0, 160, 10}}, dirty));
    CHECK(dirty.empty());
    canvas.release();
}

// ============================================================================
// The sim test kit
// ============================================================================

TEST_CASE("a test frame's padding check sees one stray byte", "[screensaver][screensaver_parts]") {
    for (const auto format : {helix::ui::PixelFormat::XRGB8888, helix::ui::PixelFormat::RGB565}) {
        helix::test::TestFrame frame(9, 5, format);
        CHECK(frame.padding_intact());
        helix::ui::PixelWriter(frame.target).fill({255, 255, 255});
        CHECK(frame.padding_intact());
        const uint32_t row = 9 * helix::ui::pixel_bytes(format);
        frame.bytes[static_cast<size_t>(2) * frame.target.stride + row + 3] ^= 1;
        CHECK_FALSE(frame.padding_intact());
        frame.bytes[static_cast<size_t>(2) * frame.target.stride + row + 3] ^= 1;
        REQUIRE(frame.padding_intact());
        frame.bytes[static_cast<size_t>(5) * frame.target.stride] ^= 1;
        CHECK_FALSE(frame.padding_intact());
    }
}

TEST_CASE("a frame diff finds a changed pixel outside the reported boxes",
          "[screensaver][screensaver_parts]") {
    helix::test::TestFrame frame(16, 8, helix::ui::PixelFormat::RGB565);
    const std::vector<uint8_t> before = frame.snapshot();
    helix::ui::PixelWriter writer(frame.target);
    writer.put(3, 3, {255, 0, 0});
    writer.put(10, 6, {0, 255, 0});
    const helix::test::FrameDiff missed = helix::test::diff_against(before, frame, {{0, 0, 4, 4}});
    CHECK(missed.changed == 2);
    CHECK(missed.uncovered == 1);
    CHECK(missed.changed_box == DirtyRect{3, 3, 10, 6});
    const helix::test::FrameDiff covered =
        helix::test::diff_against(before, frame, {{0, 0, 4, 4}, {10, 6, 10, 6}});
    CHECK(covered.uncovered == 0);
    CHECK(helix::test::lit_pixels(frame.target) == 2);
    CHECK(helix::test::count_pixels(frame.target, [](helix::ui::Rgb c) { return c.g > 0; }) == 1);
}

TEST_CASE("a frame hash covers every pixel and no padding, and opacity counts the X byte",
          "[screensaver][screensaver_parts]") {
    helix::test::TestFrame frame(6, 4, helix::ui::PixelFormat::XRGB8888);
    CHECK(helix::test::non_opaque_pixels(frame.target) == 24);
    helix::ui::PixelWriter(frame.target).fill({1, 2, 3});
    CHECK(helix::test::non_opaque_pixels(frame.target) == 0);
    const uint64_t hash = helix::test::frame_fnv1a(frame.target);
    frame.bytes[6 * 4 + 1] ^= 0xFF;
    CHECK(helix::test::frame_fnv1a(frame.target) == hash);
    frame.bytes[frame.target.stride + 5] ^= 0xFF;
    CHECK(helix::test::frame_fnv1a(frame.target) != hash);
    // FNV-1a's published offset and prime: "a" hashes to 0xaf63dc4c8601ec8c.
    const uint8_t a = 'a';
    CHECK(helix::test::fnv1a(helix::test::kFnv1aOffset, &a, 1) == 0xaf63dc4c8601ec8cULL);
}

// ============================================================================
// Saver images
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a saver decodes an image once into a buffer it owns",
                 "[screensaver][screensaver_parts]") {
    constexpr const char* LOGO = "A:assets/images/helix-icon-128.png";
    lv_image_header_t header{};
    REQUIRE(helix::ui::saver_image_info(LOGO, header));
    CHECK(header.w == 128);
    lv_draw_buf_t* logo = helix::ui::saver_decode_image(LOGO, "logo");
    REQUIRE(logo != nullptr);
    CHECK(logo->header.w == 128);
    helix::safe_draw_buf_destroy(logo, "test");
    CHECK(logo == nullptr);

    CHECK_FALSE(helix::ui::saver_image_info("A:assets/images/no-such-image.png", header));
    CHECK(helix::ui::saver_decode_image("A:assets/images/no-such-image.png", "missing",
                                        spdlog::level::debug) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the saver printer picture is the displayed printer image, never empty",
                 "[screensaver][screensaver_parts]") {
    for (const int32_t w : {480, 800, 1280}) {
        const std::string path = helix::ui::saver_printer_image_path(w);
        CHECK_FALSE(path.empty());
        CHECK(path == helix::PrinterImageManager::instance().get_displayed_image_path(w));
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
