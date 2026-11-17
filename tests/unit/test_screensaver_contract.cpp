// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_aqua.h"
#include "screensaver_bounce.h"
#include "screensaver_canvas.h"
#include "screensaver_fireflies.h"
#include "screensaver_fireworks.h"
#include "screensaver_flip_clock.h"
#include "screensaver_highfly.h"
#include "screensaver_hyperspace.h"
#include "screensaver_maze.h"
#include "screensaver_nixie_clock.h"
#include "screensaver_pipes.h"
#include "screensaver_registry.h"
#include "screensaver_starfield.h"

#include <iterator>
#include <type_traits>
#include <utility>

#include "../catch_amalgamated.hpp"

// What every saver promises the manager and the gate, checked once over every saver type.
// A saver's own tests cover what is particular to it.

using helix::ui::AquaScreensaver;
using helix::ui::FirefliesScreensaver;
using helix::ui::FireworksScreensaver;
using helix::ui::FlipClockScreensaver;
using helix::ui::HighFlyScreensaver;
using helix::ui::HyperspaceScreensaver;
using helix::ui::kLvDrawCanvasFormat;
using helix::ui::kSaverBuildCanvasFormat;
using helix::ui::MazeScreensaver;
using helix::ui::NixieClockScreensaver;

namespace {

/// The canvas format a saver draws in, and whether a level request waits for a natural break.
template <typename Saver> struct CanvasSaverTraits;

template <> struct CanvasSaverTraits<StarfieldScreensaver> {
    static constexpr lv_color_format_t kFormat = kSaverBuildCanvasFormat;
    static constexpr bool kDefersLevel = false;
};
template <> struct CanvasSaverTraits<PipesScreensaver> {
    static constexpr lv_color_format_t kFormat = kSaverBuildCanvasFormat;
    static constexpr bool kDefersLevel = false;
};
template <> struct CanvasSaverTraits<FireworksScreensaver> {
    static constexpr lv_color_format_t kFormat = kSaverBuildCanvasFormat;
    static constexpr bool kDefersLevel = true; ///< at the next shell launch
};
template <> struct CanvasSaverTraits<MazeScreensaver> {
    static constexpr lv_color_format_t kFormat = kSaverBuildCanvasFormat;
    static constexpr bool kDefersLevel = false;
};
template <> struct CanvasSaverTraits<HyperspaceScreensaver> {
    static constexpr lv_color_format_t kFormat = kSaverBuildCanvasFormat;
    static constexpr bool kDefersLevel = false;
};
template <> struct CanvasSaverTraits<FirefliesScreensaver> {
    static constexpr lv_color_format_t kFormat = kSaverBuildCanvasFormat;
    static constexpr bool kDefersLevel = false;
};
// The clock savers draw text with lv_draw_label, which needs an alpha channel.
template <> struct CanvasSaverTraits<FlipClockScreensaver> {
    static constexpr lv_color_format_t kFormat = kLvDrawCanvasFormat;
    static constexpr bool kDefersLevel = false;
};
template <> struct CanvasSaverTraits<AquaScreensaver> {
    static constexpr lv_color_format_t kFormat = kLvDrawCanvasFormat;
    static constexpr bool kDefersLevel = false;
};
template <> struct CanvasSaverTraits<HighFlyScreensaver> {
    static constexpr lv_color_format_t kFormat = kLvDrawCanvasFormat;
    static constexpr bool kDefersLevel = false;
};
template <> struct CanvasSaverTraits<NixieClockScreensaver> {
    static constexpr lv_color_format_t kFormat = kLvDrawCanvasFormat;
    static constexpr bool kDefersLevel = false;
};

/// The sim a SimSaver runs.
template <typename Saver> using SimOf = std::decay_t<decltype(std::declval<const Saver&>().sim())>;

/// A width whose tight pitch differs from LVGL's stride at any alignment above 1 byte:
/// 630 * 4 = 2520 rounds up to 2528 at a 16-byte align (prestonbrown/helixscreen#1591).
constexpr int32_t kStrainedW = 630;
constexpr int32_t kStrainedH = 480;

/// LVGLTestFixture for a TEMPLATE_TEST_CASE_METHOD over saver types.
template <typename Saver> struct SaverFixture : LVGLTestFixture {};

} // namespace

#define HELIX_EVERY_SAVER                                                                          \
    FlyingToasterScreensaver, StarfieldScreensaver, PipesScreensaver,                              \
        helix::BouncingPrinterScreensaver, helix::BouncingLogoScreensaver, FireworksScreensaver,   \
        MazeScreensaver, HyperspaceScreensaver, FlipClockScreensaver, FirefliesScreensaver,        \
        AquaScreensaver, HighFlyScreensaver, NixieClockScreensaver

#define HELIX_CANVAS_SAVERS                                                                        \
    StarfieldScreensaver, PipesScreensaver, FireworksScreensaver, MazeScreensaver,                 \
        HyperspaceScreensaver, FlipClockScreensaver, FirefliesScreensaver, AquaScreensaver,        \
        HighFlyScreensaver, NixieClockScreensaver

// ============================================================================
// Every saver
// ============================================================================

TEMPLATE_TEST_CASE_METHOD(SaverFixture,
                          "a saver's level 0 runs at the configured period, else 16 ms, with the "
                          "refresh equal",
                          "[screensaver][screensaver_contract]", HELIX_EVERY_SAVER) {
    const uint32_t configured_ms = GENERATE(as<uint32_t>{}, 0, 20);
    INFO("configured period " << configured_ms << " ms");
    const LevelZeroPeriods periods = level_zero_periods<TestType>(configured_ms);
    CHECK(periods.timer_ms == (configured_ms != 0 ? configured_ms : helix::ui::kSaverFastPeriod));
    CHECK(periods.refresh_ms == periods.timer_ms);
}

TEMPLATE_TEST_CASE_METHOD(SaverFixture,
                          "a saver is registered by its type and name and draws at both depths",
                          "[screensaver][screensaver_contract]", HELIX_EVERY_SAVER) {
    TestType ss;
    const helix::ui::ScreensaverInfo* info = helix::ui::find_screensaver(ss.type());
    REQUIRE(info != nullptr);
    CAPTURE(info->name);
    const helix::ui::ScreensaverInfo* by_name = helix::ui::find_screensaver_by_name(info->name);
    REQUIRE(by_name == info);
    CHECK((info->depths & helix::ui::SAVER_DEPTH_16) != 0);
    CHECK((info->depths & helix::ui::SAVER_DEPTH_32) != 0);
}

// ============================================================================
// Every canvas saver
// ============================================================================

TEMPLATE_TEST_CASE_METHOD(SaverFixture,
                          "a canvas saver draws in its canvas format with its sim's ladder",
                          "[screensaver][screensaver_contract]", HELIX_CANVAS_SAVERS) {
    const auto& levels = SimOf<TestType>::kLevels;
    TestType ss;
    ScreensaverStopOnExit<TestType> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss));
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == CanvasSaverTraits<TestType>::kFormat);

    REQUIRE(ss.level_count() == std::size(levels));
    CHECK(levels[0].period_ms == helix::ui::kSaverFastPeriod);
    for (size_t i = 1; i < std::size(levels); i++) {
        CAPTURE(i);
        CHECK(ss.level_period_ms(i) == levels[i].period_ms);
        // Each rung down is at most as fast as the one above it.
        CHECK(levels[i].period_ms >= levels[i - 1].period_ms);
    }
    advance_saver(ss, 33);
    CHECK(ss.is_active());
}

TEMPLATE_TEST_CASE_METHOD(SaverFixture, "a canvas saver's level request reaches its frame period",
                          "[screensaver][screensaver_contract]", HELIX_CANVAS_SAVERS) {
    const auto& levels = SimOf<TestType>::kLevels;
    TestType ss;
    ScreensaverStopOnExit<TestType> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 4);
    ss.start();
    REQUIRE(ss.is_active());

    const size_t last = std::size(levels) - 1;
    ss.request_level(last);
    int frames = 0;
    while (ss.level() != last && frames < 400) {
        advance_saver(ss, 50);
        frames++;
    }
    REQUIRE(ss.level() == last);
    if (CanvasSaverTraits<TestType>::kDefersLevel) {
        CHECK(frames > 0);
    } else {
        CHECK(frames == 0);
    }
    CHECK(SaverTestAccess::timer(ss)->period == levels[last].period_ms);
    advance_saver(ss, levels[last].period_ms);
    CHECK(ss.is_active());
}

TEMPLATE_TEST_CASE_METHOD(SaverFixture, "a stopped canvas saver holds no sim buffers",
                          "[screensaver][screensaver_contract]", HELIX_CANVAS_SAVERS) {
    TestType ss;
    CHECK(ss.sim().heap_bytes() == 0);
    {
        ScreensaverStopOnExit<TestType> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        advance_saver(ss, 16);
        REQUIRE(ss.sim().heap_bytes() > 0);
    }
    CHECK_FALSE(ss.is_active());
    CHECK(ss.sim().heap_bytes() == 0);
}

TEMPLATE_TEST_CASE_METHOD(SaverFixture,
                          "a canvas saver's allocation covers LVGL's stride-derived extent",
                          "[screensaver][screensaver_contract][1591]", HELIX_CANVAS_SAVERS) {
    ScopedResolution res(lv_display_get_default(), kStrainedW, kStrainedH);
    TestType ss;
    ScreensaverStopOnExit<TestType> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss));
    REQUIRE(buf != nullptr);
    // The stride the saver walks and the one lv_canvas_set_buffer() recorded are one number,
    // and the allocation covers the extent LVGL writes.
    CHECK(SaverTestAccess::draw_buf_stride(ss) == buf->header.stride);
    CHECK(SaverTestAccess::draw_buf_size(ss) >= buf->data_size);
}

// An XRGB8888 canvas is copied byte for byte into an ARGB8888 display, so a pixel whose X byte
// is not 0xFF draws see-through (docs/devel/GPU_ACCELERATION.md, "The alpha trap"). The test
// display ignores that byte, so this reads it from the raw canvas buffer.
TEMPLATE_TEST_CASE_METHOD(SaverFixture, "every X byte of an XRGB8888 saver canvas stays 0xFF",
                          "[screensaver][screensaver_contract][screensaver_canvas]",
                          HELIX_CANVAS_SAVERS) {
    if (CanvasSaverTraits<TestType>::kFormat != LV_COLOR_FORMAT_XRGB8888) {
        SKIP("this saver's canvas in this build has no X byte");
    }
    TestType ss;
    ScreensaverStopOnExit<TestType> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 3);
    ss.start();
    REQUIRE(ss.is_active());
    const helix::ui::FrameTarget frame =
        helix::test::frame_of(lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss)));
    CHECK(helix::test::non_opaque_pixels(frame) == 0);
    for (int i = 0; i < 30; i++) {
        advance_saver(ss, 100);
    }
    REQUIRE(helix::test::lit_pixels(frame) > 0);
    CHECK(helix::test::non_opaque_pixels(frame) == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
