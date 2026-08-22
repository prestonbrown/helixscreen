// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_fb0_sink.cpp
 * @brief Fb0MailboxSink dirty-rect blit tests, backed by a temp file.
 *
 * The sink is pointed at a regular temp file sized like the U1's fb0
 * (480x320, stride 1920, 32bpp = 614400 bytes). The fbdev ioctls fail on a
 * regular file, so start() falls back to configure_geometry().
 *
 * CRITICAL contract: px_map is the DRAW-BUFFER ORIGIN (0,0), not the dirty-area
 * origin (LVGL direct/full render mode). A dirty rect's pixels live at the
 * rect's ABSOLUTE coordinates within px_map. So the source buffers here are
 * FULL-framebuffer sized, with content painted at the area's position — and a
 * dedicated test asserts a partial rect copies ITS content, not the buffer's
 * top-left corner (the #1031 ghosting regression).
 */

#include "remote_screen_fb0_sink.h"
#include "remote_screen_sink.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int FB_W = 480;
constexpr int FB_H = 320;
constexpr uint32_t FB_STRIDE = 1920;                              // 480 * 4, no row padding
constexpr size_t FB_SIZE = static_cast<size_t>(FB_STRIDE) * FB_H; // 614400

// 16bpp (RGB565) destination geometry — some U1 firmwares expose fb0 at 16bpp.
constexpr uint32_t FB_STRIDE16 = FB_W * 2;                            // 960, no row padding
constexpr size_t FB_SIZE16 = static_cast<size_t>(FB_STRIDE16) * FB_H; // 307200

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

std::string make_temp_fb_sized(size_t bytes) {
    char tmpl[] = "/tmp/helix_fb0_XXXXXX";
    int fd = ::mkstemp(tmpl);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, static_cast<off_t>(bytes)) == 0);
    ::close(fd);
    return std::string(tmpl);
}

std::string make_temp_fb() {
    return make_temp_fb_sized(FB_SIZE);
}

std::string make_temp_fb16() {
    return make_temp_fb_sized(FB_SIZE16);
}

// A frame whose px_map is a FULL-buffer-origin source (stride spans the whole
// display width). x1..y2 are absolute coords into that buffer.
RemoteScreenFrame make_frame(const uint8_t* px, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                             uint32_t src_stride,
                             RemoteScreenPixelFormat fmt = RemoteScreenPixelFormat::BGRA8888) {
    RemoteScreenFrame f;
    f.px_map = px;
    f.x1 = x1;
    f.y1 = y1;
    f.x2 = x2;
    f.y2 = y2;
    f.disp_w = FB_W;
    f.disp_h = FB_H;
    f.color_format = 18;
    f.src_stride = src_stride;
    f.src_format = fmt;
    return f;
}

// Fill a BGRA rect [x1,x2]x[y1,y2] within a full-size (FB_W x FB_H) buffer.
void fill_bgra_rect(std::vector<uint8_t>& buf, uint32_t stride, int x1, int y1, int x2, int y2,
                    uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    for (int y = y1; y <= y2; ++y) {
        for (int x = x1; x <= x2; ++x) {
            size_t o = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
            buf[o + 0] = b;
            buf[o + 1] = g;
            buf[o + 2] = r;
            buf[o + 3] = a;
        }
    }
}

} // namespace

TEST_CASE("Fb0MailboxSink: full-frame magenta blit", "[remote_screen][fb0]") {
    std::string path = make_temp_fb();

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE, 32);
    REQUIRE(sink.start());
    REQUIRE(sink.wants_frames());

    std::vector<uint8_t> src(FB_SIZE);
    for (size_t i = 0; i < FB_SIZE; i += 4) {
        src[i + 0] = 0xFF;
        src[i + 1] = 0x00;
        src[i + 2] = 0xFF;
        src[i + 3] = 0xFF; // magenta BGRA
    }

    sink.on_frame(make_frame(src.data(), 0, 0, FB_W - 1, FB_H - 1, FB_STRIDE));
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE);
    const size_t center = static_cast<size_t>(160) * FB_STRIDE + static_cast<size_t>(240) * 4;
    REQUIRE(fb[center + 0] == 0xFF);
    REQUIRE(fb[center + 2] == 0xFF);
    REQUIRE(fb[0] == 0xFF);
    const size_t br = static_cast<size_t>(319) * FB_STRIDE + static_cast<size_t>(479) * 4;
    REQUIRE(fb[br + 0] == 0xFF);
    REQUIRE(fb[br + 2] == 0xFF);

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: partial rect copies ITS content, not top-left (ghost regression)",
          "[remote_screen][fb0]") {
    std::string path = make_temp_fb();

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE, 32);
    REQUIRE(sink.start());

    // Full-buffer source: RED across the whole buffer (incl. top-left), GREEN
    // only in the dirty rect (100,50)..(131,81). px_map is buffer origin.
    std::vector<uint8_t> src(FB_SIZE);
    fill_bgra_rect(src, FB_STRIDE, 0, 0, FB_W - 1, FB_H - 1, 0x00, 0x00, 0xFF,
                   0xFF);                                                     // red everywhere
    fill_bgra_rect(src, FB_STRIDE, 100, 50, 131, 81, 0x00, 0xFF, 0x00, 0xFF); // green rect

    // Flush only the green rect.
    sink.on_frame(make_frame(src.data(), 100, 50, 131, 81, FB_STRIDE));
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE);

    // fb0 at the rect must be GREEN (the rect's content) — NOT red (top-left).
    for (int r = 50; r <= 81; ++r) {
        const size_t off = static_cast<size_t>(r) * FB_STRIDE + static_cast<size_t>(100) * 4;
        INFO("row " << r);
        REQUIRE(fb[off + 0] == 0x00); // B
        REQUIRE(fb[off + 1] == 0xFF); // G  <-- green, would be 0x00 if ghosting top-left
        REQUIRE(fb[off + 2] == 0x00); // R  <-- would be 0xFF (red) under the old bug
    }
    // Last column of the rect (131) is green too.
    const size_t last = static_cast<size_t>(60) * FB_STRIDE + static_cast<size_t>(131) * 4;
    REQUIRE(fb[last + 1] == 0xFF);

    // Outside the flushed rect: fb0 was never written for this frame -> stays 0.
    REQUIRE(fb[0] == 0x00);
    const size_t left = static_cast<size_t>(60) * FB_STRIDE + static_cast<size_t>(99) * 4;
    REQUIRE(fb[left + 0] == 0x00);
    REQUIRE(fb[left + 1] == 0x00);

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: dirty rect past the edge is clamped", "[remote_screen][fb0]") {
    std::string path = make_temp_fb();

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE, 32);
    REQUIRE(sink.start());

    // Full buffer, distinct color in the in-bounds corner region.
    std::vector<uint8_t> src(FB_SIZE);
    fill_bgra_rect(src, FB_STRIDE, 460, 300, FB_W - 1, FB_H - 1, 0x11, 0x22, 0x33, 0x44);

    // Area (460,300)..(500,340) extends past the 480x320 fb — must clamp.
    sink.on_frame(make_frame(src.data(), 460, 300, 500, 340, FB_STRIDE));
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE); // no write past the mapping

    const size_t tl = static_cast<size_t>(300) * FB_STRIDE + static_cast<size_t>(460) * 4;
    REQUIRE(fb[tl + 0] == 0x11);
    REQUIRE(fb[tl + 2] == 0x33);
    const size_t last = static_cast<size_t>(319) * FB_STRIDE + static_cast<size_t>(479) * 4;
    REQUIRE(fb[last + 0] == 0x11);
    REQUIRE(fb[last + 2] == 0x33);

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: RGB565 source is converted to BGRA at the right position",
          "[remote_screen][fb0]") {
    std::string path = make_temp_fb();

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE, 32);
    REQUIRE(sink.start());

    // Full-size RGB565 buffer (stride 480*2 = 960). Pure red (0xF800) only in the
    // rect (10,10)..(25,25); rest zero.
    constexpr uint32_t RGB_STRIDE = FB_W * 2; // 960
    std::vector<uint8_t> src(static_cast<size_t>(RGB_STRIDE) * FB_H, 0);
    for (int y = 10; y <= 25; ++y) {
        for (int x = 10; x <= 25; ++x) {
            size_t o = static_cast<size_t>(y) * RGB_STRIDE + static_cast<size_t>(x) * 2;
            src[o + 0] = 0x00;
            src[o + 1] = 0xF8; // 0xF800 = pure red
        }
    }

    sink.on_frame(
        make_frame(src.data(), 10, 10, 25, 25, RGB_STRIDE, RemoteScreenPixelFormat::RGB565));
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE);

    // 0xF800 -> R8=0xFF, G=0, B=0. fb0 BGRA: [B=0, G=0, R=0xFF, A=0xFF] at (12,12).
    const size_t off = static_cast<size_t>(12) * FB_STRIDE + static_cast<size_t>(12) * 4;
    REQUIRE(fb[off + 0] == 0x00);
    REQUIRE(fb[off + 1] == 0x00);
    REQUIRE(fb[off + 2] == 0xFF);
    REQUIRE(fb[off + 3] == 0xFF);
    // Outside the rect stays zero.
    REQUIRE(fb[0] == 0x00);

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: unknown source format is skipped", "[remote_screen][fb0]") {
    std::string path = make_temp_fb();
    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE, 32);
    REQUIRE(sink.start());

    std::vector<uint8_t> src(FB_SIZE, 0xAB);
    sink.on_frame(make_frame(src.data(), 0, 0, FB_W - 1, FB_H - 1, FB_STRIDE,
                             RemoteScreenPixelFormat::Unknown));
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE);
    REQUIRE(fb[0] == 0x00);
    REQUIRE(fb[FB_SIZE / 2] == 0x00);

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: 16bpp dest, RGB565 source is copied verbatim", "[remote_screen][fb0]") {
    std::string path = make_temp_fb16();

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE16, 16);
    REQUIRE(sink.start());
    REQUIRE(sink.wants_frames());

    // Full-size RGB565 source (stride matches the 16bpp dest). Distinct value
    // 0x1234 in the rect (40,30)..(71,61); rest zero. RGB565 -> RGB565 is a
    // straight per-row memcpy, so fb0 must equal the source bytes verbatim.
    std::vector<uint8_t> src(FB_SIZE16, 0);
    const uint16_t val = 0x1234;
    for (int y = 30; y <= 61; ++y) {
        for (int x = 40; x <= 71; ++x) {
            size_t o = static_cast<size_t>(y) * FB_STRIDE16 + static_cast<size_t>(x) * 2;
            src[o + 0] = static_cast<uint8_t>(val & 0xFF);
            src[o + 1] = static_cast<uint8_t>(val >> 8);
        }
    }

    sink.on_frame(
        make_frame(src.data(), 40, 30, 71, 61, FB_STRIDE16, RemoteScreenPixelFormat::RGB565));
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE16);

    // fb0 at a pixel inside the rect equals the source bytes (little-endian 0x1234).
    const size_t off = static_cast<size_t>(45) * FB_STRIDE16 + static_cast<size_t>(50) * 2;
    REQUIRE(fb[off + 0] == 0x34);
    REQUIRE(fb[off + 1] == 0x12);
    // Last column of the rect (71) copied too.
    const size_t last = static_cast<size_t>(45) * FB_STRIDE16 + static_cast<size_t>(71) * 2;
    REQUIRE(fb[last + 0] == 0x34);
    REQUIRE(fb[last + 1] == 0x12);
    // Outside the flushed rect stays zero.
    REQUIRE(fb[0] == 0x00);

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: 16bpp dest, BGRA source packs to RGB565", "[remote_screen][fb0]") {
    std::string path = make_temp_fb16();

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE16, 16);
    REQUIRE(sink.start());

    // Full-size BGRA source (stride 1920). Four known colors in four adjacent
    // cells; each must pack to its canonical RGB565 value in the 16bpp dest.
    std::vector<uint8_t> src(FB_SIZE, 0);
    fill_bgra_rect(src, FB_STRIDE, 0, 0, 7, 7, 0x00, 0x00, 0xFF, 0xFF);   // red   -> 0xF800
    fill_bgra_rect(src, FB_STRIDE, 10, 0, 17, 7, 0x00, 0xFF, 0x00, 0xFF); // green -> 0x07E0
    fill_bgra_rect(src, FB_STRIDE, 20, 0, 27, 7, 0xFF, 0x00, 0x00, 0xFF); // blue  -> 0x001F
    fill_bgra_rect(src, FB_STRIDE, 30, 0, 37, 7, 0xFF, 0xFF, 0xFF, 0xFF); // white -> 0xFFFF

    sink.on_frame(
        make_frame(src.data(), 0, 0, 37, 7, FB_STRIDE, RemoteScreenPixelFormat::BGRA8888));
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE16);

    auto px16 = [&](int x, int y) -> uint16_t {
        size_t o = static_cast<size_t>(y) * FB_STRIDE16 + static_cast<size_t>(x) * 2;
        return static_cast<uint16_t>(fb[o] | (fb[o + 1] << 8));
    };

    REQUIRE(px16(3, 3) == 0xF800);  // red
    REQUIRE(px16(13, 3) == 0x07E0); // green
    REQUIRE(px16(23, 3) == 0x001F); // blue
    REQUIRE(px16(33, 3) == 0xFFFF); // white
    // Little-endian byte order in the buffer: red low byte 0x00, high byte 0xF8.
    const size_t red_off = static_cast<size_t>(3) * FB_STRIDE16 + static_cast<size_t>(3) * 2;
    REQUIRE(fb[red_off + 0] == 0x00);
    REQUIRE(fb[red_off + 1] == 0xF8);

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: unsupported bpp leaves an inactive no-op sink", "[remote_screen][fb0]") {
    std::string path = make_temp_fb_sized(static_cast<size_t>(FB_W) * FB_H * 3); // 24bpp-ish

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_W * 3, 24); // 24bpp is rejected
    REQUIRE_FALSE(sink.start());
    REQUIRE_FALSE(sink.wants_frames());

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: failed start leaves an inactive no-op sink", "[remote_screen][fb0]") {
    Fb0MailboxSink sink("/proc/nonexistent_helix_fb0");
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE, 32);

    REQUIRE_FALSE(sink.start());
    REQUIRE_FALSE(sink.wants_frames());

    std::vector<uint8_t> src(FB_SIZE, 0xFF);
    sink.on_frame(make_frame(src.data(), 0, 0, FB_W - 1, FB_H - 1, FB_STRIDE));

    sink.stop();
    REQUIRE_FALSE(sink.wants_frames());
}

TEST_CASE("Fb0MailboxSink: frame past px_map_len is skipped even when the inferred bound allows it",
          "[remote_screen][fb0]") {
    std::string path = make_temp_fb();

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE, 32);
    REQUIRE(sink.start());

    // The renderer only produced a 100-row buffer (e.g. a fallback backend after
    // DRM init failed), but the frame still reports the full 320-row display. The
    // inferred bound (src_stride * disp_h = 614400) therefore does NOT catch the
    // over-read; only the declared px_map_len does.
    constexpr int32_t REAL_ROWS = 100;
    const size_t real_len = static_cast<size_t>(FB_STRIDE) * REAL_ROWS; // 192000

    // Backing allocation is deliberately larger than the declared length so the
    // pre-fix behaviour reads valid-but-wrong memory instead of crashing the
    // test run. 0xAB marks anything that gets mirrored.
    std::vector<uint8_t> src(FB_SIZE, 0xAB);

    RemoteScreenFrame f = make_frame(src.data(), 0, 200, FB_W - 1, 219, FB_STRIDE);
    f.px_map_len = real_len;
    // Sanity: this frame passes the stride*disp_h inference but not the real length.
    const size_t last_src = static_cast<size_t>(219) * FB_STRIDE + static_cast<size_t>(FB_W) * 4;
    REQUIRE(last_src < static_cast<size_t>(FB_STRIDE) * FB_H);
    REQUIRE(last_src > real_len);

    sink.on_frame(f);
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE);
    // Nothing mirrored: the fb0 file is still all zeros.
    const size_t row200 = static_cast<size_t>(200) * FB_STRIDE;
    REQUIRE(fb[row200 + 0] == 0x00);
    REQUIRE(fb[row200 + 1] == 0x00);
    const size_t row219 = static_cast<size_t>(219) * FB_STRIDE + static_cast<size_t>(479) * 4;
    REQUIRE(fb[row219 + 0] == 0x00);
    REQUIRE(fb[0] == 0x00);

    ::unlink(path.c_str());
}

TEST_CASE("Fb0MailboxSink: frame inside px_map_len still mirrors", "[remote_screen][fb0]") {
    std::string path = make_temp_fb();

    Fb0MailboxSink sink(path);
    sink.configure_geometry(FB_W, FB_H, FB_STRIDE, 32);
    REQUIRE(sink.start());

    // Same short 100-row buffer, but the dirty rect lives inside it — the guard
    // must not reject a frame the declared length covers.
    constexpr int32_t REAL_ROWS = 100;
    std::vector<uint8_t> src(FB_SIZE, 0);
    fill_bgra_rect(src, FB_STRIDE, 0, 10, FB_W - 1, 41, 0x00, 0xFF, 0x00, 0xFF); // green

    RemoteScreenFrame f = make_frame(src.data(), 0, 10, FB_W - 1, 41, FB_STRIDE);
    f.px_map_len = static_cast<size_t>(FB_STRIDE) * REAL_ROWS;
    sink.on_frame(f);
    sink.stop();

    std::vector<uint8_t> fb = read_file(path);
    REQUIRE(fb.size() == FB_SIZE);
    const size_t off = static_cast<size_t>(20) * FB_STRIDE + static_cast<size_t>(240) * 4;
    REQUIRE(fb[off + 0] == 0x00);
    REQUIRE(fb[off + 1] == 0xFF);
    REQUIRE(fb[off + 2] == 0x00);

    ::unlink(path.c_str());
}
