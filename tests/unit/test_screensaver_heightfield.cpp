// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/screensaver_sim_kit.h"
#include "screensaver_heightfield.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::bilinear_sample;
using helix::ui::build_heightfield_slices;
using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::HeightfieldEdge;
using helix::ui::HeightfieldFog;
using helix::ui::HeightfieldRenderer;
using helix::ui::HeightfieldScene;
using helix::ui::HeightfieldSlice;
using helix::ui::HeightfieldView;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;
using helix::ui::upscale_frame;

using helix::test::near;
using helix::test::TestFrame;

namespace {

/// A view whose rows are easy to aim at: a scene asks for a screen row and gets the height
/// that projects there.
HeightfieldView test_view(int32_t w, int32_t h, uint16_t slices) {
    HeightfieldView v;
    v.w = w;
    v.h = h;
    v.horizon_row = 0.0f;
    v.focal_x = static_cast<float>(w) / 2.0f;
    v.focal_y = 100.0f;
    v.cam_h = 0.0f;
    v.z_near = 10.0f;
    v.z_far = 10.0f + static_cast<float>(slices);
    v.dz_grow = 1.0f;
    v.slices = slices;
    v.edge = HeightfieldEdge::FEATHERED_SILHOUETTE;
    v.span_gradient = true;
    v.first_slice_fills = true;
    return v;
}

/// Answers each (slice, column) with a row on screen and a colour.
class RowScene : public HeightfieldScene {
  public:
    using RowFn = std::function<float(size_t slice, int32_t x)>;
    using ColorFn = std::function<Rgb(size_t slice, int32_t x)>;

    RowScene(const HeightfieldView& view, RowFn row, ColorFn color)
        : view_(view), row_(std::move(row)), color_(std::move(color)) {}

    void sample_slice(size_t index, const HeightfieldSlice& slice, float* heights,
                      Rgb* colors) override {
        calls++;
        for (int32_t x = 0; x < view_.w; x++) {
            // row = horizon + (cam_h - h) * focal_y / z
            heights[x] =
                view_.cam_h - (row_(index, x) - view_.horizon_row) * slice.z / view_.focal_y;
            colors[x] = color_(index, x);
        }
    }

    int calls = 0;

  private:
    HeightfieldView view_;
    RowFn row_;
    ColorFn color_;
};

} // namespace

// ============================================================================
// Slices, fog, bilinear
// ============================================================================

TEST_CASE("slices run from near to far, each thicker by the grow factor",
          "[screensaver][heightfield]") {
    HeightfieldView water;
    water.z_near = 3.5f;
    water.z_far = 7.5f;
    water.dz_grow = 1.015f;
    water.slices = 110;
    water.w = 400;
    water.focal_x = 500.0f;
    std::vector<HeightfieldSlice> s;
    build_heightfield_slices(water, s);
    REQUIRE(s.size() == 110);
    CHECK(s[0].z == Catch::Approx(3.5f));
    CHECK(std::fabs(s[109].z + s[109].dz - 7.5f) < 1e-3f);
    CHECK(s[0].dz == Catch::Approx(0.01448f).margin(1e-4));
    for (size_t i = 0; i + 1 < s.size(); i++) {
        CHECK(s[i + 1].dz / s[i].dz == Catch::Approx(1.015f).margin(1e-5));
        CHECK(s[i + 1].z == Catch::Approx(s[i].z + s[i].dz).margin(1e-4));
    }
    for (const HeightfieldSlice& slice : s) {
        CHECK(slice.inv_z * slice.z == Catch::Approx(1.0f).margin(1e-6));
    }

    HeightfieldView clouds = water;
    clouds.z_near = 10.0f;
    clouds.z_far = 350.0f;
    clouds.dz_grow = 1.025f;
    clouds.slices = 117;
    build_heightfield_slices(clouds, s);
    CHECK(s[0].dz == Catch::Approx(0.501f).margin(0.002));
}

TEST_CASE("each slice knows the world x of its columns", "[screensaver][heightfield]") {
    HeightfieldView v = test_view(400, 240, 30);
    v.focal_x = 200.0f;
    std::vector<HeightfieldSlice> s;
    build_heightfield_slices(v, s);
    for (const HeightfieldSlice& slice : s) {
        CHECK(slice.x_step == Catch::Approx(slice.z / 200.0f));
        CHECK(slice.x_left == Catch::Approx((0.5f - 200.0f) * slice.z / 200.0f));
    }
}

TEST_CASE("sink lowers near heights smoothly to full height", "[screensaver][heightfield]") {
    HeightfieldView v = test_view(40, 30, 80);
    v.sink_depth = 40.0f;
    std::vector<HeightfieldSlice> s;
    build_heightfield_slices(v, s);
    REQUIRE(s[20].z == Catch::Approx(30.0f));
    CHECK(s[0].sink == Catch::Approx(0.0f));
    CHECK(s[20].sink == Catch::Approx(0.5f));
    CHECK(s[10].sink > 0.0f);
    CHECK(s[10].sink < s[20].sink);
    for (size_t i = 40; i < s.size(); i++) {
        CHECK(s[i].sink == Catch::Approx(1.0f));
    }
    v.sink_depth = 0.0f;
    build_heightfield_slices(v, s);
    for (const HeightfieldSlice& slice : s) {
        CHECK(slice.sink == 1.0f);
    }
}

TEST_CASE("fog rises or falls with depth", "[screensaver][heightfield]") {
    const HeightfieldFog rising{25.0f, 300.0f, 0.8f, false};
    CHECK(rising.at(25.0f) == Catch::Approx(0.0f));
    CHECK(rising.at(10.0f) == Catch::Approx(0.0f));
    CHECK(rising.at(300.0f) == Catch::Approx(1.0f));
    CHECK(rising.at(400.0f) == Catch::Approx(1.0f));
    CHECK(rising.at(162.5f) == Catch::Approx(std::pow(0.5f, 0.8f)));
    const HeightfieldFog falling{3.5f, 7.5f, 0.6f, true};
    CHECK(falling.at(3.5f) == Catch::Approx(1.0f));
    CHECK(falling.at(7.5f) == Catch::Approx(0.0f));
    CHECK(falling.at(5.5f) == Catch::Approx(std::pow(0.5f, 0.6f)));
}

TEST_CASE("bilinear sampling interpolates between cells and wraps or clamps",
          "[screensaver][heightfield]") {
    const float grid[] = {0.0f, 1.0f, 2.0f, 3.0f};
    CHECK(bilinear_sample(grid, 2, 2, 0.5f, 0.5f, false) == Catch::Approx(1.5f));
    CHECK(bilinear_sample(grid, 2, 2, 1.5f, 0.0f, true) == Catch::Approx(0.5f));
    CHECK(bilinear_sample(grid, 2, 2, 5.0f, 0.0f, false) == Catch::Approx(1.0f));
    CHECK(bilinear_sample(grid, 2, 2, -3.0f, 0.0f, false) == Catch::Approx(0.0f));
    CHECK(bilinear_sample(grid, 2, 2, 0.0f, 1.5f, true) == Catch::Approx(1.0f));
}

// ============================================================================
// render
// ============================================================================

TEST_CASE("samples cost columns times slices", "[screensaver][heightfield]") {
    const HeightfieldView v = test_view(40, 30, 20);
    HeightfieldRenderer r;
    r.init(v);
    RowScene scene(
        v, [](size_t, int32_t) { return 25.0f; }, [](size_t, int32_t) { return Rgb{9, 9, 9}; });
    r.render(scene);
    CHECK(r.samples() == 800);
    CHECK(scene.calls == 20);
}

TEST_CASE("a near tall surface hides a far low one", "[screensaver][heightfield]") {
    const HeightfieldView v = test_view(40, 60, 20);
    HeightfieldRenderer r;
    r.init(v);
    const Rgb near{200, 50, 50};
    const Rgb far{50, 50, 200};
    RowScene scene(
        v, [](size_t s, int32_t) { return s < 5 ? 10.0f : 20.0f; },
        [&](size_t s, int32_t) { return s < 5 ? near : far; });
    r.render(scene);
    for (int32_t x = 0; x < v.w; x++) {
        CHECK(r.top_at(x) == Catch::Approx(10.0f));
        for (int32_t y = 0; y < 10; y++) {
            CHECK(r.coverage_at(x, y) == 0);
        }
        for (int32_t y = 10; y < v.h; y++) {
            CHECK(r.coverage_at(x, y) == 255);
            CHECK(r.color_at(x, y).b < 100);
        }
    }
}

TEST_CASE("a span shades from this slice's colour at its top to the last slice's at its bottom",
          "[screensaver][heightfield]") {
    HeightfieldView v = test_view(20, 100, 2);
    const Rgb red{200, 0, 0};
    const Rgb blue{0, 0, 200};
    v.below_color = red;
    HeightfieldRenderer r;
    r.init(v);
    RowScene scene(
        v, [](size_t s, int32_t) { return s == 0 ? 60.0f : 20.0f; },
        [&](size_t s, int32_t) { return s == 0 ? red : blue; });
    r.render(scene);
    CHECK(near(r.color_at(10, 20), blue, 8));
    CHECK(near(r.color_at(10, 59), red, 8));
    CHECK(near(r.color_at(10, 40), Rgb{100, 0, 100}, 8));
}

TEST_CASE("no gradient where the previous slice did not draw the column",
          "[screensaver][heightfield]") {
    const HeightfieldView v = test_view(20, 100, 3);
    const Rgb colors[] = {{200, 0, 0}, {0, 200, 0}, {0, 0, 200}};
    HeightfieldRenderer r;
    r.init(v);
    // Slice 1 sits behind slice 0, slice 2 rises above both.
    RowScene scene(
        v,
        [](size_t s, int32_t) {
            const float rows[] = {50.0f, 60.0f, 30.0f};
            return rows[s];
        },
        [&](size_t s, int32_t) { return colors[s]; });
    r.render(scene);
    for (int32_t y = 31; y < 50; y++) {
        CAPTURE(y);
        CHECK(near(r.color_at(10, y), colors[2], 2));
    }
}

TEST_CASE("span_gradient off draws flat spans", "[screensaver][heightfield]") {
    HeightfieldView v = test_view(20, 100, 2);
    v.span_gradient = false;
    HeightfieldRenderer r;
    r.init(v);
    RowScene scene(
        v, [](size_t s, int32_t) { return s == 0 ? 60.0f : 20.0f; },
        [](size_t s, int32_t) { return s == 0 ? Rgb{200, 0, 0} : Rgb{0, 0, 200}; });
    r.render(scene);
    for (int32_t y = 21; y < 60; y++) {
        CHECK(near(r.color_at(10, y), Rgb{0, 0, 200}, 1));
    }
}

TEST_CASE("the feathered silhouette gives the top pixel fractional coverage and smooths it",
          "[screensaver][heightfield]") {
    const HeightfieldView v = test_view(12, 40, 1);
    HeightfieldRenderer r;
    r.init(v);
    RowScene flat(
        v, [](size_t, int32_t) { return 10.25f; },
        [](size_t, int32_t) { return Rgb{255, 255, 255}; });
    r.render(flat);
    CHECK(r.coverage_at(5, 10) == Catch::Approx(191).margin(3));
    CHECK(r.coverage_at(5, 9) == 0);
    CHECK(r.coverage_at(5, 11) == 255);
    CHECK(r.top_at(5) == Catch::Approx(10.25f));

    // Column 6 drops to row 20: the step column's three edge rows take [1 2 1] / 4 of their
    // neighbours.
    RowScene step(
        v, [](size_t, int32_t x) { return x >= 6 ? 20.0f : 10.25f; },
        [](size_t, int32_t) { return Rgb{255, 255, 255}; });
    r.render(step);
    CHECK(r.coverage_at(5, 9) == 0);
    CHECK(r.coverage_at(5, 10) == Catch::Approx((191 + 2 * 191 + 0) / 4).margin(3));
    CHECK(r.coverage_at(5, 11) == Catch::Approx((255 + 2 * 255 + 0) / 4).margin(3));
    CHECK(r.coverage_at(5, 12) == 255);
}

TEST_CASE("soft-under edges fade in over F rows and keep the far surface showing",
          "[screensaver][heightfield]") {
    HeightfieldView v = test_view(10, 100, 1);
    v.edge = HeightfieldEdge::SOFT_UNDER;
    v.feather_px = 10.0f;
    v.span_gradient = false;
    HeightfieldRenderer r;
    r.init(v);
    RowScene one(
        v, [](size_t, int32_t) { return 50.0f; }, [](size_t, int32_t) { return Rgb{255, 0, 0}; });
    r.render(one);
    CHECK(r.coverage_at(4, 49) == 0);
    CHECK(r.coverage_at(4, 50) < 30);
    for (int32_t y = 51; y < 60; y++) {
        CHECK(r.coverage_at(4, y) > r.coverage_at(4, y - 1));
    }
    CHECK(r.coverage_at(4, 59) >= 230);
    const uint8_t alone_57 = r.coverage_at(4, 57);

    v.slices = 2;
    v.z_far = v.z_near + 2.0f;
    r.set_view(v);
    RowScene two(
        v, [](size_t s, int32_t) { return s == 0 ? 50.0f : 55.0f; },
        [](size_t s, int32_t) { return s == 0 ? Rgb{255, 0, 0} : Rgb{0, 0, 255}; });
    r.render(two);
    for (int32_t y = 55; y < 60; y++) {
        CHECK(r.color_at(4, y).b > 0);
    }
    CHECK(r.coverage_at(4, 57) > alone_57);
    CHECK(r.color_at(4, 50).b == 0);
}

TEST_CASE("first_slice_fills false leaves the rows below the near edge uncovered",
          "[screensaver][heightfield]") {
    HeightfieldView v = test_view(10, 80, 2);
    v.first_slice_fills = false;
    HeightfieldRenderer r;
    r.init(v);
    RowScene scene(
        v, [](size_t s, int32_t) { return s == 0 ? 40.0f : 20.0f; },
        [](size_t, int32_t) { return Rgb{100, 100, 100}; });
    r.render(scene);
    CHECK(r.front_top_at(3) == Catch::Approx(40.0f));
    CHECK(r.front_color_at(3) == Rgb{100, 100, 100});
    CHECK(r.coverage_at(3, 30) == 255);
    for (int32_t y = 41; y < v.h; y++) {
        CHECK(r.coverage_at(3, y) == 0);
    }

    v.first_slice_fills = true;
    r.set_view(v);
    r.render(scene);
    CHECK(r.coverage_at(3, 60) == 255);
}

TEST_CASE("depth records the slice where coverage first reached half",
          "[screensaver][heightfield]") {
    const HeightfieldView v = test_view(10, 80, 3);
    HeightfieldRenderer r;
    r.init(v);
    RowScene scene(
        v,
        [](size_t s, int32_t) {
            const float rows[] = {60.0f, 40.0f, 20.0f};
            return rows[s];
        },
        [](size_t, int32_t) { return Rgb{100, 100, 100}; });
    r.render(scene);
    CHECK(r.depth_slice_at(2, 70) == 0);
    CHECK(r.depth_slice_at(2, 50) == 1);
    CHECK(r.depth_slice_at(2, 30) == 2);
    CHECK(r.depth_slice_at(2, 10) == HeightfieldRenderer::kNoDepth);

    HeightfieldView soft = v;
    soft.edge = HeightfieldEdge::SOFT_UNDER;
    soft.feather_px = 10.0f;
    r.set_view(soft);
    r.render(scene);
    // Half coverage comes five rows into the feather.
    CHECK(r.depth_slice_at(2, 21) == HeightfieldRenderer::kNoDepth);
    CHECK(r.depth_slice_at(2, 26) == 2);
}

TEST_CASE("render allocates nothing and heap stays constant", "[screensaver][heightfield]") {
    HeightfieldView v = test_view(64, 48, 30);
    HeightfieldRenderer r;
    r.init(v);
    RowScene scene(
        v, [](size_t s, int32_t x) { return 40.0f - 0.5f * s + std::sin(x * 0.3f); },
        [](size_t s, int32_t) { return Rgb{static_cast<uint8_t>(s * 5), 30, 60}; });
    r.render(scene);
    const size_t heap = r.heap_bytes();
    REQUIRE(heap > 0);
    for (int i = 0; i < 10; i++) {
        r.render(scene);
    }
    CHECK(r.heap_bytes() == heap);
    v.slices = 20;
    r.set_view(v);
    r.render(scene);
    CHECK(r.heap_bytes() == heap);
    r.clear();
    CHECK(r.heap_bytes() == 0);
}

// ============================================================================
// resolve and upscale
// ============================================================================

TEST_CASE("resolve composes under coverage over the background", "[screensaver][heightfield]") {
    HeightfieldView v = test_view(8, 20, 1);
    v.span_gradient = false;
    HeightfieldRenderer r;
    r.init(v);
    RowScene scene(
        v, [](size_t, int32_t) { return 10.5f; }, [](size_t, int32_t) { return Rgb{200, 0, 0}; });
    r.render(scene);
    REQUIRE(r.coverage_at(3, 10) == Catch::Approx(128).margin(1));
    std::vector<Rgb> frame(static_cast<size_t>(v.w) * v.h);
    r.resolve(nullptr, Rgb{0, 0, 200}, 0, v.h, frame);
    const Rgb px = frame[10 * v.w + 3];
    CHECK(px.r == Catch::Approx(100).margin(2));
    CHECK(px.g == 0);
    CHECK(px.b == Catch::Approx(100).margin(2));
    CHECK(frame[2 * v.w + 3] == Rgb{0, 0, 200});
    CHECK(frame[15 * v.w + 3] == Rgb{200, 0, 0});

    std::vector<Rgb> bg(frame.size(), Rgb{0, 90, 0});
    std::vector<Rgb> out(frame.size(), Rgb{1, 2, 3});
    r.resolve(bg.data(), Rgb{}, 5, 8, out);
    CHECK(out[6 * v.w + 1] == Rgb{0, 90, 0});
    CHECK(out[4 * v.w + 1] == Rgb{1, 2, 3});
    CHECK(out[8 * v.w + 1] == Rgb{1, 2, 3});
}

TEST_CASE("a 2x upscale interpolates and a 4x one replicates, in either format",
          "[screensaver][heightfield]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const std::vector<Rgb> two{Rgb{0, 0, 0}, Rgb{255, 255, 255}};
    {
        TestFrame c(4, 2, format);
        upscale_frame(two, 2, 1, 2, 0, 1, c.target);
        const PixelWriter w(c.target);
        CHECK(w.get(0, 0).g < 8);
        CHECK(w.get(3, 0).g > 247);
        for (int32_t x = 1; x < 4; x++) {
            CHECK(w.get(x, 0).g >= w.get(x - 1, 0).g);
        }
        CHECK(w.get(1, 0).g > 20);
        CHECK(w.get(2, 0).g < 235);
    }
    {
        // Full and empty channels, which RGB565 stores exactly whatever the dither.
        const std::vector<Rgb> four{Rgb{0, 255, 0}, Rgb{255, 0, 255}};
        TestFrame c(8, 4, format);
        upscale_frame(four, 2, 1, 4, 0, 1, c.target);
        const PixelWriter w(c.target);
        for (int32_t y = 0; y < 4; y++) {
            for (int32_t x = 0; x < 4; x++) {
                CHECK(w.get(x, y) == w.get(0, 0));
                CHECK(w.get(x + 4, y) == w.get(4, 0));
            }
        }
        CHECK(!(w.get(0, 0) == w.get(4, 0)));
    }
    if (format == PixelFormat::RGB565) {
        // Mid grey dithers to the right mean over a 4 x 4 tile.
        const std::vector<Rgb> grey(4, Rgb{128, 128, 128});
        TestFrame c(4, 4, format);
        upscale_frame(grey, 2, 2, 2, 0, 2, c.target);
        const PixelWriter w(c.target);
        int sum = 0;
        for (int32_t y = 0; y < 4; y++) {
            for (int32_t x = 0; x < 4; x++) {
                sum += w.get(x, y).r;
            }
        }
        CHECK(sum / 16.0 == Catch::Approx(128.0).margin(2.0));
    }
}

TEST_CASE("a 2x upscale gives each channel its own exact 3:1 bilinear mix",
          "[screensaver][heightfield]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    // A band inside the frame, so the vertical neighbour clamps at both of its edges.
    const int32_t y_from = GENERATE(0, 2);
    CAPTURE(static_cast<int>(format), y_from);
    constexpr int32_t kW = 13;
    constexpr int32_t kH = 9;
    const int32_t y_to = y_from == 0 ? kH : 7;
    std::vector<Rgb> frame(static_cast<size_t>(kW) * kH);
    uint32_t seed = 0x2156u;
    for (Rgb& c : frame) {
        seed = seed * 1664525u + 1013904223u;
        c = Rgb{static_cast<uint8_t>(seed >> 24), static_cast<uint8_t>(seed >> 16),
                static_cast<uint8_t>(seed >> 8)};
    }
    // Pure red, green and blue cells, so a channel landing in another's place shows.
    frame[0] = Rgb{255, 0, 0};
    frame[static_cast<size_t>(kW) + 5] = Rgb{0, 255, 0};
    frame[static_cast<size_t>(kW) * 3 + 12] = Rgb{0, 0, 255};

    TestFrame got(kW * 2, kH * 2, format);
    TestFrame want(kW * 2, kH * 2, format);
    upscale_frame(frame, kW, kH, 2, y_from, y_to, got.target);

    const auto at = [&](int32_t x, int32_t y) { return frame[static_cast<size_t>(y) * kW + x]; };
    PixelWriter expect(want.target);
    for (int32_t oy = y_from * 2; oy < y_to * 2; oy++) {
        const int32_t sy = oy / 2;
        const int32_t sy2 = std::clamp((oy & 1) == 0 ? sy - 1 : sy + 1, y_from, y_to - 1);
        for (int32_t ox = 0; ox < kW * 2; ox++) {
            const int32_t sx = ox / 2;
            const int32_t side = (ox & 1) == 0 ? std::max(0, sx - 1) : std::min(kW - 1, sx + 1);
            const auto mix = [&](uint8_t Rgb::*ch) {
                const auto col = [&](int32_t x) { return 3 * (at(x, sy).*ch) + (at(x, sy2).*ch); };
                return static_cast<uint8_t>((3 * col(sx) + col(side) + 8) >> 4);
            };
            expect.put_dithered(ox, oy, Rgb{mix(&Rgb::r), mix(&Rgb::g), mix(&Rgb::b)});
        }
    }
    const PixelWriter read(got.target);
    const PixelWriter ref(want.target);
    for (int32_t oy = y_from * 2; oy < y_to * 2; oy++) {
        for (int32_t ox = 0; ox < kW * 2; ox++) {
            CAPTURE(ox, oy);
            REQUIRE(read.get(ox, oy) == ref.get(ox, oy));
        }
    }
}

TEST_CASE("upscale writes only the rows asked for and returns that box",
          "[screensaver][heightfield]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    std::vector<Rgb> frame(10 * 6, Rgb{200, 200, 200});
    TestFrame c(20, 12, format, 0, 0x11, 0x11);
    const DirtyRect box = upscale_frame(frame, 10, 6, 2, 2, 4, c.target);
    CHECK(box == DirtyRect{0, 4, 19, 7});
    const uint32_t bpp = helix::ui::pixel_bytes(format);
    for (uint32_t y = 0; y < 12; y++) {
        const bool inside = y >= 4 && y < 8;
        size_t touched = 0;
        for (uint32_t i = 0; i < 20 * bpp; i++) {
            touched += c.bytes[y * c.target.stride + i] != 0x11 ? 1 : 0;
        }
        CAPTURE(y);
        CHECK((touched > 0) == inside);
    }
}

TEST_CASE("upscale writes only the columns asked for and returns that box",
          "[screensaver][heightfield]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    const int32_t scale = GENERATE(2, 4);
    CAPTURE(static_cast<int>(format), scale);
    std::vector<Rgb> frame(10 * 6, Rgb{200, 200, 200});
    TestFrame c(10 * scale, 6 * scale, format, 0, 0x11, 0x11);
    const DirtyRect box = upscale_frame(frame, 10, 6, scale, 1, 4, c.target, 3, 7);
    CHECK(box == DirtyRect{3 * scale, scale, 7 * scale - 1, 4 * scale - 1});
    const PixelWriter w(c.target);
    for (int32_t y = 0; y < 6 * scale; y++) {
        for (int32_t x = 0; x < 10 * scale; x++) {
            const bool in = x >= box.x1 && x <= box.x2 && y >= box.y1 && y <= box.y2;
            const uint32_t bpp = helix::ui::pixel_bytes(format);
            const bool touched =
                c.bytes[static_cast<size_t>(y) * c.target.stride + x * bpp] != 0x11;
            CAPTURE(x, y);
            CHECK(touched == in);
        }
    }
}

TEST_CASE("an odd-sized canvas never writes past a row", "[screensaver][heightfield]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    const int32_t scale = GENERATE(2, 4);
    CAPTURE(static_cast<int>(format), scale);
    constexpr uint32_t OW = 321;
    constexpr uint32_t kOh = 203;
    const int32_t w = (OW + scale - 1) / scale;
    const int32_t h = (kOh + scale - 1) / scale;
    std::vector<Rgb> frame(static_cast<size_t>(w) * h, Rgb{90, 140, 200});
    TestFrame c(OW, kOh, format, 7);
    const DirtyRect box = upscale_frame(frame, w, h, scale, 0, h, c.target);
    CHECK(box == DirtyRect{0, 0, static_cast<int32_t>(OW) - 1, static_cast<int32_t>(kOh) - 1});
    size_t unwritten = 0;
    const PixelWriter wr(c.target);
    for (uint32_t y = 0; y < kOh; y++) {
        for (uint32_t x = 0; x < OW; x++) {
            unwritten += wr.get(x, y).b < 150 ? 1 : 0;
        }
    }
    CHECK(c.padding_intact());
    CHECK(unwritten == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
