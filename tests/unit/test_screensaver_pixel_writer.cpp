// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_fast_math.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::FrameTarget;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

namespace {

constexpr uint8_t kUntouched = 0xAA;

/// A heap frame whose every byte starts as kUntouched, so a write outside its pixel shows.
struct TestFrame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    TestFrame(uint32_t w, uint32_t h, uint32_t stride, PixelFormat format)
        : bytes(static_cast<size_t>(stride) * h, kUntouched) {
        target = {bytes.data(), stride, w, h, format};
    }
    TestFrame(const TestFrame&) = delete;
    TestFrame& operator=(const TestFrame&) = delete;
};

} // namespace

TEST_CASE("PixelWriter writes XRGB8888 as B, G, R and an opaque X byte",
          "[screensaver][pixel_writer]") {
    TestFrame frame(4, 3, 4 * 4 + 8, PixelFormat::XRGB8888);
    PixelWriter writer(frame.target);

    writer.put(3, 2, Rgb{0x12, 0x34, 0x56});

    const size_t at = 2 * static_cast<size_t>(frame.target.stride) + 3 * 4;
    CHECK(frame.bytes[at] == 0x56);
    CHECK(frame.bytes[at + 1] == 0x34);
    CHECK(frame.bytes[at + 2] == 0x12);
    CHECK(frame.bytes[at + 3] == 0xFF);
    // Rows step by the stride, so the padding after the row's last pixel is left alone.
    CHECK(frame.bytes[at + 4] == kUntouched);
    CHECK(frame.bytes[at - 1] == kUntouched);
    CHECK(writer.get(3, 2) == Rgb{0x12, 0x34, 0x56});
}

TEST_CASE("PixelWriter fill covers every XRGB8888 pixel and no row padding",
          "[screensaver][pixel_writer]") {
    constexpr uint32_t W = 5;
    constexpr uint32_t H = 4;
    constexpr uint32_t kStride = W * 4 + 12;
    TestFrame frame(W, H, kStride, PixelFormat::XRGB8888);

    PixelWriter(frame.target).fill(Rgb{1, 2, 3});

    size_t wrong_pixels = 0;
    size_t touched_padding = 0;
    for (uint32_t y = 0; y < H; y++) {
        const uint8_t* row = frame.bytes.data() + static_cast<size_t>(y) * kStride;
        for (uint32_t x = 0; x < W; x++) {
            const uint8_t* px = row + x * 4;
            wrong_pixels += (px[0] != 3 || px[1] != 2 || px[2] != 1 || px[3] != 0xFF) ? 1 : 0;
        }
        for (uint32_t i = W * 4; i < kStride; i++) {
            touched_padding += row[i] != kUntouched ? 1 : 0;
        }
    }
    CHECK(wrong_pixels == 0);
    CHECK(touched_padding == 0);
}

TEST_CASE("PixelWriter knows which points lie inside the frame", "[screensaver][pixel_writer]") {
    TestFrame frame(4, 3, 16, PixelFormat::XRGB8888);
    const PixelWriter writer(frame.target);
    CHECK(writer.contains(0, 0));
    CHECK(writer.contains(3, 2));
    CHECK_FALSE(writer.contains(4, 0));
    CHECK_FALSE(writer.contains(0, 3));
    CHECK_FALSE(writer.contains(-1, 0));
    CHECK_FALSE(writer.contains(0, -1));
}

TEST_CASE("PixelWriter writes RGB565 as the top 5, 6 and 5 bits, little-endian",
          "[screensaver][pixel_writer]") {
    TestFrame frame(4, 2, 4 * 2 + 2, PixelFormat::RGB565);
    PixelWriter writer(frame.target);
    const size_t stride = frame.target.stride;

    writer.put(0, 0, Rgb{0x12, 0x34, 0x56});
    writer.put(1, 1, Rgb{255, 0, 0});
    writer.put(2, 1, Rgb{0, 255, 0});
    writer.put(3, 1, Rgb{0, 0, 255});

    // 0x12 -> red 2, 0x34 -> green 13, 0x56 -> blue 10: (2 << 11) | (13 << 5) | 10 = 0x11AA
    CHECK(frame.bytes[0] == 0xAA);
    CHECK(frame.bytes[1] == 0x11);
    CHECK(frame.bytes[stride + 2] == 0x00);
    CHECK(frame.bytes[stride + 3] == 0xF8);
    CHECK(frame.bytes[stride + 4] == 0xE0);
    CHECK(frame.bytes[stride + 5] == 0x07);
    CHECK(frame.bytes[stride + 6] == 0x1F);
    CHECK(frame.bytes[stride + 7] == 0x00);
    CHECK(frame.bytes[stride + 8] == kUntouched); // row padding

    // Levels expand back to 8 bits by repeating their top bits.
    CHECK(writer.get(1, 1) == Rgb{255, 0, 0});
    CHECK(writer.get(2, 1) == Rgb{0, 255, 0});
    CHECK(writer.get(0, 0) == Rgb{16, 52, 82});
}

TEST_CASE("PixelWriter dithers RGB565 with a 4x4 ordered pattern and writes XRGB8888 exactly",
          "[screensaver][pixel_writer]") {
    TestFrame frame(4, 4, 8, PixelFormat::RGB565);
    PixelWriter writer(frame.target);
    for (int32_t y = 0; y < 4; y++) {
        for (int32_t x = 0; x < 4; x++) {
            writer.put_dithered(x, y, Rgb{70, 70, 70});
        }
    }

    // 70 is 8.51 red and blue levels of 31: half the cells round up, in a checkerboard.
    constexpr uint8_t kRedBlue[4][4] = {{8, 9, 8, 9}, {9, 8, 9, 8}, {8, 9, 8, 9}, {9, 8, 9, 8}};
    // 70 is 17.29 green levels of 63: the five cells with the highest thresholds round up.
    int green_up = 0;
    for (int32_t y = 0; y < 4; y++) {
        for (int32_t x = 0; x < 4; x++) {
            CAPTURE(x, y);
            const size_t at = static_cast<size_t>(y) * 8 + static_cast<size_t>(x) * 2;
            const uint16_t v = static_cast<uint16_t>(frame.bytes[at] | frame.bytes[at + 1] << 8);
            CHECK((v >> 11) == kRedBlue[y][x]);
            CHECK((v & 0x1F) == kRedBlue[y][x]);
            const int green = (v >> 5) & 0x3F;
            CHECK((green == 17 || green == 18));
            green_up += green == 18 ? 1 : 0;
        }
    }
    CHECK(green_up == 5);

    // The ends of the range never dither.
    for (uint8_t t = 0; t < 16; t++) {
        CAPTURE(static_cast<int>(t));
        CHECK(helix::ui::dither_channel(0, 31, t) == 0);
        CHECK(helix::ui::dither_channel(255, 31, t) == 31);
        CHECK(helix::ui::dither_channel(255, 63, t) == 63);
    }

    TestFrame exact(1, 1, 4, PixelFormat::XRGB8888);
    PixelWriter(exact.target).put_dithered(0, 0, Rgb{70, 71, 72});
    CHECK(PixelWriter(exact.target).get(0, 0) == Rgb{70, 71, 72});
}

TEST_CASE("the RGB565 dither tables give dither_channel's level for every value and threshold",
          "[screensaver][pixel_writer]") {
    for (int t = 0; t < 16; t++) {
        for (int c = 0; c < 256; c++) {
            CAPTURE(t, c);
            const auto th = static_cast<uint8_t>(t);
            const auto v = static_cast<uint8_t>(c);
            const auto expect = static_cast<uint16_t>(helix::ui::dither_channel(v, 31, th) << 11 |
                                                      helix::ui::dither_channel(v, 63, th) << 5 |
                                                      helix::ui::dither_channel(v, 31, th));
            REQUIRE(helix::ui::dither_rgb565(Rgb{v, v, v}, th) == expect);
        }
    }
    // Each channel reads its own table column.
    CHECK(helix::ui::dither_rgb565(Rgb{255, 0, 0}, 0) == 0xF800);
    CHECK(helix::ui::dither_rgb565(Rgb{0, 255, 0}, 0) == 0x07E0);
    CHECK(helix::ui::dither_rgb565(Rgb{0, 0, 255}, 0) == 0x001F);
}

TEST_CASE("saver_floor and saver_ceil round like std::floor and std::ceil",
          "[screensaver][pixel_writer]") {
    const float values[] = {
        0.0f,        0.25f,      0.5f,        0.999f,     1.0f,        1.5f,     -0.25f,
        -0.5f,       -1.0f,      -1.5f,       -2.0001f,   123.75f,     -123.75f, 4194303.5f,
        -4194303.5f, 8388607.0f, -8388607.0f, 8388608.0f, -8388608.0f, 1.0e9f,   -1.0e9f};
    for (const float v : values) {
        CAPTURE(v);
        CHECK(helix::ui::saver_floor(v) == std::floor(v));
        CHECK(helix::ui::saver_ceil(v) == std::ceil(v));
    }
    CHECK(std::isnan(helix::ui::saver_floor(std::numeric_limits<float>::quiet_NaN())));
    CHECK(std::isnan(helix::ui::saver_ceil(std::numeric_limits<float>::quiet_NaN())));
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(helix::ui::saver_floor(-inf) == -inf);
    CHECK(helix::ui::saver_ceil(inf) == inf);
}

TEST_CASE("PixelWriter max blending brightens each channel and leaves a dimmer colour alone",
          "[screensaver][pixel_writer]") {
    SECTION("XRGB8888") {
        TestFrame frame(2, 1, 8, PixelFormat::XRGB8888);
        PixelWriter writer(frame.target);
        writer.put(0, 0, Rgb{10, 200, 30});
        writer.blend_max(0, 0, Rgb{100, 50, 40});
        CHECK(writer.get(0, 0) == Rgb{100, 200, 40});

        const std::vector<uint8_t> before = frame.bytes;
        writer.blend_max(0, 0, Rgb{5, 5, 5});
        CHECK(frame.bytes == before);
    }
    SECTION("RGB565") {
        TestFrame frame(2, 1, 4, PixelFormat::RGB565);
        PixelWriter writer(frame.target);
        writer.put(0, 0, Rgb{0, 0, 0});
        writer.blend_max(0, 0, Rgb{255, 255, 255});
        CHECK(writer.get(0, 0) == Rgb{255, 255, 255});

        writer.put(1, 0, Rgb{255, 255, 255});
        const std::vector<uint8_t> before = frame.bytes;
        writer.blend_max(1, 0, Rgb{0, 0, 0});
        CHECK(frame.bytes == before);
    }
}

TEST_CASE("PixelWriter lines visit both ends and step at most one pixel per axis",
          "[screensaver][pixel_writer]") {
    using Points = std::vector<std::pair<int32_t, int32_t>>;
    const auto trace = [](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        Points points;
        PixelWriter::line(x0, y0, x1, y1, [&](int32_t x, int32_t y) { points.emplace_back(x, y); });
        return points;
    };

    CHECK(trace(0, 0, 4, 2) == Points{{0, 0}, {1, 1}, {2, 1}, {3, 2}, {4, 2}});
    CHECK(trace(0, 0, 1, 3) == Points{{0, 0}, {0, 1}, {1, 2}, {1, 3}});
    CHECK(trace(2, 5, 2, 2) == Points{{2, 5}, {2, 4}, {2, 3}, {2, 2}});
    CHECK(trace(7, 7, 7, 7) == Points{{7, 7}});

    const std::vector<std::pair<Points::value_type, Points::value_type>> segments = {
        {{-3, 9}, {12, -4}}, {{5, 5}, {-6, 1}}, {{0, 0}, {-2, -9}}};
    for (const auto& [a, b] : segments) {
        const Points points = trace(a.first, a.second, b.first, b.second);
        INFO("from " << a.first << "," << a.second << " to " << b.first << "," << b.second);
        REQUIRE_FALSE(points.empty());
        CHECK(points.front() == a);
        CHECK(points.back() == b);
        const size_t expected = static_cast<size_t>(
            std::max(std::abs(b.first - a.first), std::abs(b.second - a.second)) + 1);
        CHECK(points.size() == expected);
        for (size_t i = 1; i < points.size(); i++) {
            CHECK(std::abs(points[i].first - points[i - 1].first) <= 1);
            CHECK(std::abs(points[i].second - points[i - 1].second) <= 1);
        }
    }
}

TEST_CASE("hue_color walks the wheel in 256 steps", "[screensaver][pixel_writer]") {
    using helix::ui::hue_color;
    CHECK(hue_color(0) == Rgb{255, 0, 0});
    CHECK(hue_color(128) == Rgb{0, 255, 255});
    CHECK(hue_color(256) == hue_color(0));
    CHECK(hue_color(43) == Rgb{253, 255, 0});
}

TEST_CASE("add_light saturates each XRGB8888 channel and keeps the X byte",
          "[screensaver][pixel_writer]") {
    uint32_t px = 0xFF0A1420u;
    helix::ui::add_light(&px, 250, 1, 2);
    CHECK(px == 0xFFFF1522u);
}

TEST_CASE("an RGB565 add finer than one level shows as an ordered pattern",
          "[screensaver][pixel_writer]") {
    using helix::ui::kBayer4x4;
    uint16_t tile[4][4] = {};
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            helix::ui::add_light(&tile[y][x], 4, 4, 4, kBayer4x4[y][x]);
        }
    }
    int r = 0;
    int g = 0;
    int b = 0;
    for (const auto& row : tile) {
        for (const uint16_t v : row) {
            r += v >> 11;
            g += v >> 5 & 0x3F;
            b += v & 0x1F;
        }
    }
    CHECK(r == 8);
    CHECK(g == 16);
    CHECK(b == 8);

    // Undithered, light under one level of red or blue truncates to nothing.
    uint16_t plain = 0;
    for (int i = 0; i < 16; i++) {
        helix::ui::add_light(&plain, 4, 0, 4);
    }
    CHECK(plain == 0);

    uint16_t white = 0xFFFF;
    helix::ui::add_light(&white, 255, 255, 255, 15);
    CHECK(white == 0xFFFF);
}

// ============================================================================
// Shared colour and pixel helpers, each proven bit-for-bit equal to the spellings of the same
// rule the savers write by hand. The oracles are those spellings, verbatim.
// ============================================================================

namespace oracle {

uint8_t heightfield_to_u8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

uint8_t fireworks_mix_channel(uint8_t a, uint8_t b, float t) {
    const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
}

size_t aqua_lut_index(float v) {
    return static_cast<size_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

uint8_t pipes_unit_byte(float v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

uint8_t flip_blend_channel(uint8_t from, uint8_t to, uint32_t cov) {
    return static_cast<uint8_t>((from * (255u - cov) + to * cov + 127u) / 255u);
}

uint8_t heightfield_premultiply_channel(uint8_t c, uint32_t cov) {
    return static_cast<uint8_t>((c * cov + 127u) / 255u);
}

uint8_t fireworks_dimmed_channel(uint8_t c, uint32_t amount) {
    return static_cast<uint8_t>(c * amount / 255);
}

uint8_t hyperspace_scale8(uint32_t v, uint32_t amount) {
    return static_cast<uint8_t>(std::min<uint32_t>(255, v * amount / 255));
}

float flip_corner_coverage(float r, float dist) {
    return std::clamp(r - dist + 0.5f, 0.0f, 1.0f);
}

Rgb pixel_writer_get_565(uint16_t v) {
    const auto r5 = static_cast<uint8_t>(v >> 11 & 0x1F);
    const auto g6 = static_cast<uint8_t>(v >> 5 & 0x3F);
    const auto b5 = static_cast<uint8_t>(v & 0x1F);
    return {static_cast<uint8_t>(r5 << 3 | r5 >> 2), static_cast<uint8_t>(g6 << 2 | g6 >> 4),
            static_cast<uint8_t>(b5 << 3 | b5 >> 2)};
}

} // namespace oracle

namespace {

/// Every float from `lo` up to `hi`, both included, in order of their bit patterns (lo >= 0).
template <typename F> void each_float(float lo, float hi, F&& f) {
    for (float v = lo; v <= hi; v = std::nextafter(v, 2.0f * hi + 1.0f)) {
        f(v);
    }
}

/// `n` evenly spaced values over [lo, hi], plus every integer and half in it.
std::vector<float> dense_sweep(float lo, float hi, int n) {
    std::vector<float> out;
    for (int i = 0; i <= n; i++) {
        out.push_back(lo + (hi - lo) * static_cast<float>(i) / static_cast<float>(n));
    }
    for (float v = std::ceil(lo * 2.0f) / 2.0f; v <= hi; v += 0.5f) {
        out.push_back(v);
        out.push_back(std::nextafter(v, lo - 1.0f));
        out.push_back(std::nextafter(v, hi + 1.0f));
    }
    return out;
}

} // namespace

TEST_CASE("to_u8 rounds and clamps a float to a byte exactly as the heightfield does",
          "[screensaver][pixel_writer]") {
    size_t mismatches = 0;
    for (const float v : dense_sweep(-20.0f, 280.0f, 600000)) {
        mismatches += helix::ui::to_u8(v) != oracle::heightfield_to_u8(v);
    }
    CHECK(mismatches == 0);
    CHECK(helix::ui::to_u8(254.5f) == 255);
    CHECK(helix::ui::to_u8(-3.0f) == 0);
    CHECK(helix::ui::to_u8(300.0f) == 255);
    CHECK(helix::ui::to_u8(127.49f) == 127);
}

TEST_CASE("unit_to_u8 is aqua's table index everywhere and pipes' rounded byte wherever they "
          "agree",
          "[screensaver][pixel_writer]") {
    size_t aqua_mismatches = 0;
    for (const float v : dense_sweep(-1.0f, 2.0f, 3000000)) {
        aqua_mismatches += helix::ui::unit_to_u8(v) != oracle::aqua_lut_index(v);
    }
    CHECK(aqua_mismatches == 0);

    // v * 255 + 0.5f rounds up past lround only where v * 255 lands just under one half, in
    // [0.25, 0.5): the one binade where adding 0.5 crosses into a coarser one. Every float v
    // there is checked; above it the two are equal by construction, which the sweep samples.
    std::vector<float> disagree;
    each_float(0.25f / 255.0f, 0.5f / 255.0f, [&](float v) {
        if (helix::ui::unit_to_u8(v) != oracle::pipes_unit_byte(v)) {
            disagree.push_back(v);
        }
    });
    size_t sampled = 0;
    for (const float v : dense_sweep(0.0f, 1.0f, 3000000)) {
        if (v * 255.0f >= 0.5f) {
            sampled += helix::ui::unit_to_u8(v) != oracle::pipes_unit_byte(v);
        }
    }
    CHECK(sampled == 0);
    INFO("first disagreeing v: " << (disagree.empty() ? 0.0f : disagree.front()));
    CHECK(disagree.size() <= 1);
    for (const float v : disagree) {
        // The only difference: v * 255 is the float just under 0.5.
        CHECK(v * 255.0f == std::nextafter(0.5f, 0.0f));
        CHECK(helix::ui::unit_to_u8(v) == 1);
        CHECK(oracle::pipes_unit_byte(v) == 0);
    }
}

TEST_CASE("blend_cov is the flip clock's integer blend and the heightfield's premultiply",
          "[screensaver][pixel_writer]") {
    size_t blend_mismatches = 0;
    size_t premultiply_mismatches = 0;
    for (uint32_t from = 0; from < 256; from++) {
        for (uint32_t to = 0; to < 256; to++) {
            for (uint32_t cov = 0; cov < 256; cov++) {
                const auto f = static_cast<uint8_t>(from);
                const auto t = static_cast<uint8_t>(to);
                const Rgb got = helix::ui::blend_cov({f, t, f}, {t, f, t}, cov);
                blend_mismatches += got.r != oracle::flip_blend_channel(f, t, cov) ||
                                    got.g != oracle::flip_blend_channel(t, f, cov) ||
                                    got.b != oracle::flip_blend_channel(f, t, cov);
            }
            const Rgb pre = helix::ui::blend_cov({}, {static_cast<uint8_t>(from), 0, 0}, to);
            premultiply_mismatches +=
                pre.r != oracle::heightfield_premultiply_channel(static_cast<uint8_t>(from), to);
        }
    }
    CHECK(blend_mismatches == 0);
    CHECK(premultiply_mismatches == 0);
}

TEST_CASE("scale_rgb is fireworks' dimmed, the flip clock's shade and hyperspace's scale8",
          "[screensaver][pixel_writer]") {
    size_t mismatches = 0;
    for (uint32_t c = 0; c < 256; c++) {
        for (uint32_t amount = 0; amount < 256; amount++) {
            const auto ch = static_cast<uint8_t>(c);
            const Rgb got = helix::ui::scale_rgb({ch, ch, ch}, amount);
            mismatches += got.r != oracle::fireworks_dimmed_channel(ch, amount);
            mismatches += got.g != oracle::hyperspace_scale8(c, amount);
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("scale_rgb_by rounds and clamps each channel", "[screensaver][pixel_writer]") {
    CHECK(helix::ui::scale_rgb_by({200, 100, 3}, 1.0f) == Rgb{200, 100, 3});
    CHECK(helix::ui::scale_rgb_by({200, 100, 3}, 0.0f) == Rgb{0, 0, 0});
    CHECK(helix::ui::scale_rgb_by({201, 101, 3}, 0.5f) == Rgb{101, 51, 2});
    CHECK(helix::ui::scale_rgb_by({200, 100, 3}, 2.0f) == Rgb{255, 200, 6});
}

TEST_CASE("mix_rgb is fireworks' channel mix at every pair of bytes",
          "[screensaver][pixel_writer]") {
    std::vector<float> ts;
    for (int i = -16; i <= 80; i++) {
        ts.push_back(static_cast<float>(i) / 64.0f);
    }
    ts.push_back(0.3f);
    ts.push_back(0.7f);
    ts.push_back(1.0f / 3.0f);
    size_t mismatches = 0;
    for (uint32_t a = 0; a < 256; a++) {
        for (uint32_t b = 0; b < 256; b++) {
            const auto ca = static_cast<uint8_t>(a);
            const auto cb = static_cast<uint8_t>(b);
            for (const float t : ts) {
                mismatches += helix::ui::mix_rgb({ca, 0, 0}, {cb, 0, 0}, t).r !=
                              oracle::fireworks_mix_channel(ca, cb, t);
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("edge_coverage is the flip clock's corner coverage", "[screensaver][pixel_writer]") {
    size_t mismatches = 0;
    for (const float r : {0.0f, 1.0f, 3.5f, 7.0f, 12.25f}) {
        for (const float dist : dense_sweep(-2.0f, 16.0f, 20000)) {
            mismatches +=
                helix::ui::edge_coverage(r - dist) != oracle::flip_corner_coverage(r, dist);
        }
    }
    CHECK(mismatches == 0);
    CHECK(helix::ui::edge_coverage(0.0f) == 0.5f);
    CHECK(helix::ui::edge_coverage(-0.5f) == 0.0f);
    CHECK(helix::ui::edge_coverage(0.5f) == 1.0f);
}

TEST_CASE("unpack565 expands every RGB565 word as PixelWriter reads it",
          "[screensaver][pixel_writer]") {
    TestFrame frame(1, 1, 2, PixelFormat::RGB565);
    PixelWriter writer(frame.target);
    size_t mismatches = 0;
    for (uint32_t v = 0; v < 65536; v++) {
        const Rgb expect = oracle::pixel_writer_get_565(static_cast<uint16_t>(v));
        frame.bytes[0] = static_cast<uint8_t>(v & 0xFF);
        frame.bytes[1] = static_cast<uint8_t>(v >> 8);
        mismatches += !(PixelWriter::unpack565(static_cast<uint16_t>(v)) == expect);
        mismatches += !(writer.get(0, 0) == expect);
    }
    CHECK(mismatches == 0);
    CHECK(PixelWriter::unpack565(0xFFFF) == Rgb{255, 255, 255});
}

TEST_CASE("blend_over writes the dithered mix of the pixel and the colour",
          "[screensaver][pixel_writer]") {
    for (const PixelFormat format : {PixelFormat::XRGB8888, PixelFormat::RGB565}) {
        TestFrame a(8, 8, 8 * 4, format);
        TestFrame b(8, 8, 8 * 4, format);
        PixelWriter wa(a.target);
        PixelWriter wb(b.target);
        for (int32_t y = 0; y < 8; y++) {
            for (int32_t x = 0; x < 8; x++) {
                const Rgb base{static_cast<uint8_t>(x * 30), static_cast<uint8_t>(y * 30), 90};
                wa.put(x, y, base);
                wb.put(x, y, base);
                const Rgb c{250, 40, static_cast<uint8_t>(x * y)};
                const float alpha = static_cast<float>(x + y) / 14.0f;
                wa.blend_over(x, y, c, alpha);
                wb.put_dithered(x, y, helix::ui::mix_rgb(wb.get(x, y), c, alpha));
            }
        }
        CHECK(a.bytes == b.bytes);
    }
}

TEST_CASE("fill_rect fills the part of a box inside the frame and nothing else",
          "[screensaver][pixel_writer]") {
    using helix::ui::DirtyRect;
    for (const PixelFormat format : {PixelFormat::XRGB8888, PixelFormat::RGB565}) {
        for (const DirtyRect box :
             {DirtyRect{2, 1, 5, 3}, DirtyRect{-3, -2, 1, 9}, DirtyRect{6, 5, 12, 12},
              DirtyRect{9, 9, 12, 12}, DirtyRect{}}) {
            TestFrame got(8, 7, 8 * 4 + 4, format);
            TestFrame hand(8, 7, 8 * 4 + 4, format);
            TestFrame dithered(8, 7, 8 * 4 + 4, format);
            TestFrame hand_dithered(8, 7, 8 * 4 + 4, format);
            const Rgb c{123, 45, 210};
            PixelWriter(got.target).fill_rect(box, c);
            PixelWriter(dithered.target).fill_rect_dithered(box, c);
            PixelWriter wh(hand.target);
            PixelWriter whd(hand_dithered.target);
            for (int32_t y = box.y1; y <= box.y2; y++) {
                for (int32_t x = box.x1; x <= box.x2; x++) {
                    if (wh.contains(x, y)) {
                        wh.put(x, y, c);
                        whd.put_dithered(x, y, c);
                    }
                }
            }
            CHECK(got.bytes == hand.bytes);
            CHECK(dithered.bytes == hand_dithered.bytes);
        }
    }
}
