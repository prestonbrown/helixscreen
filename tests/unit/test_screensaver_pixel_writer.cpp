// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
// Shared colour helpers: where each one rounds, clamps or truncates.
// ============================================================================

TEST_CASE("to_u8 rounds half up and clamps to a byte", "[screensaver][pixel_writer]") {
    CHECK(helix::ui::to_u8(0.49f) == 0);
    CHECK(helix::ui::to_u8(0.5f) == 1);
    CHECK(helix::ui::to_u8(127.49f) == 127);
    CHECK(helix::ui::to_u8(127.5f) == 128);
    CHECK(helix::ui::to_u8(254.5f) == 255);
    CHECK(helix::ui::to_u8(-3.0f) == 0);
    CHECK(helix::ui::to_u8(300.0f) == 255);
}

TEST_CASE("unit_to_u8 maps 0..1 onto a rounded byte and clamps outside it",
          "[screensaver][pixel_writer]") {
    CHECK(helix::ui::unit_to_u8(0.0f) == 0);
    CHECK(helix::ui::unit_to_u8(1.0f) == 255);
    CHECK(helix::ui::unit_to_u8(0.5f) == 128); // 127.5 rounds up
    CHECK(helix::ui::unit_to_u8(0.25f) == 64); // 63.75 rounds up
    CHECK(helix::ui::unit_to_u8(-0.5f) == 0);
    CHECK(helix::ui::unit_to_u8(2.0f) == 255);
}

TEST_CASE("blend_cov blends by integer coverage, rounding half up per channel",
          "[screensaver][pixel_writer]") {
    const Rgb black{0, 0, 0};
    const Rgb white{255, 255, 255};
    CHECK(helix::ui::blend_cov(black, white, 0) == black);
    CHECK(helix::ui::blend_cov(black, white, 255) == white);
    CHECK(helix::ui::blend_cov(black, white, 128) == Rgb{128, 128, 128});
    CHECK(helix::ui::blend_cov(white, black, 128) == Rgb{127, 127, 127});
    // One level apart: 255/255 rounds up to 1, 254/255 stays 0.
    CHECK(helix::ui::blend_cov(black, Rgb{1, 1, 1}, 128) == Rgb{1, 1, 1});
    CHECK(helix::ui::blend_cov(black, Rgb{1, 1, 1}, 127) == black);
    // Channels blend independently.
    CHECK(helix::ui::blend_cov(Rgb{0, 255, 0}, Rgb{255, 0, 255}, 255) == Rgb{255, 0, 255});
}

TEST_CASE("scale_rgb scales by amount/255 and truncates", "[screensaver][pixel_writer]") {
    const Rgb c{255, 128, 1};
    CHECK(helix::ui::scale_rgb(c, 255) == c);
    CHECK(helix::ui::scale_rgb(c, 0) == Rgb{0, 0, 0});
    CHECK(helix::ui::scale_rgb(c, 128) == Rgb{128, 64, 0});
    // 128 * 254 / 255 is 127.49: truncated, not rounded.
    CHECK(helix::ui::scale_rgb(c, 254) == Rgb{254, 127, 0});
}

TEST_CASE("scale_rgb_by rounds and clamps each channel", "[screensaver][pixel_writer]") {
    CHECK(helix::ui::scale_rgb_by({200, 100, 3}, 1.0f) == Rgb{200, 100, 3});
    CHECK(helix::ui::scale_rgb_by({200, 100, 3}, 0.0f) == Rgb{0, 0, 0});
    CHECK(helix::ui::scale_rgb_by({201, 101, 3}, 0.5f) == Rgb{101, 51, 2});
    CHECK(helix::ui::scale_rgb_by({200, 100, 3}, 2.0f) == Rgb{255, 200, 6});
}

TEST_CASE("mix_rgb interpolates, rounds half up and clamps past either end",
          "[screensaver][pixel_writer]") {
    const Rgb a{0, 10, 11};
    const Rgb b{255, 11, 10};
    CHECK(helix::ui::mix_rgb(a, b, 0.0f) == a);
    CHECK(helix::ui::mix_rgb(a, b, 1.0f) == b);
    // 127.5, 10.5 and 10.5 all round up.
    CHECK(helix::ui::mix_rgb(a, b, 0.5f) == Rgb{128, 11, 11});
    CHECK(helix::ui::mix_rgb(Rgb{0, 0, 0}, Rgb{1, 1, 1}, 0.49f) == Rgb{0, 0, 0});
    CHECK(helix::ui::mix_rgb(a, b, -0.25f).r == 0);
    CHECK(helix::ui::mix_rgb(a, b, 1.25f).r == 255);
}

TEST_CASE("edge_coverage is half on the edge and clamps a pixel either side",
          "[screensaver][pixel_writer]") {
    CHECK(helix::ui::edge_coverage(0.0f) == 0.5f);
    CHECK(helix::ui::edge_coverage(0.25f) == 0.75f);
    CHECK(helix::ui::edge_coverage(0.5f) == 1.0f);
    CHECK(helix::ui::edge_coverage(-0.5f) == 0.0f);
    CHECK(helix::ui::edge_coverage(-1.0f) == 0.0f);
    CHECK(helix::ui::edge_coverage(2.0f) == 1.0f);
}

TEST_CASE("unpack565 widens each field by repeating its top bits, as PixelWriter reads it",
          "[screensaver][pixel_writer]") {
    CHECK(PixelWriter::unpack565(0x0000) == Rgb{0, 0, 0});
    CHECK(PixelWriter::unpack565(0xFFFF) == Rgb{255, 255, 255});
    CHECK(PixelWriter::unpack565(0xF800) == Rgb{255, 0, 0});
    CHECK(PixelWriter::unpack565(0x07E0) == Rgb{0, 255, 0});
    CHECK(PixelWriter::unpack565(0x001F) == Rgb{0, 0, 255});
    // r5 = 0b10000 widens to 0b10000100, g6 = 0b100000 to 0b10000010, b5 like r5.
    CHECK(PixelWriter::unpack565(0x8000) == Rgb{132, 0, 0});
    CHECK(PixelWriter::unpack565(0x0400) == Rgb{0, 130, 0});
    CHECK(PixelWriter::unpack565(0x0010) == Rgb{0, 0, 132});

    TestFrame frame(1, 1, 2, PixelFormat::RGB565);
    PixelWriter writer(frame.target);
    frame.bytes[0] = 0x00;
    frame.bytes[1] = 0x84; // 0x8400, little-endian
    CHECK(writer.get(0, 0) == Rgb{132, 130, 0});
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
