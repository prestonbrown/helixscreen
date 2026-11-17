// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_server_sink.cpp
 * @brief ScreenFrameStore: viewer gating, the encode cache and rate cap, resizes.
 *
 * Every case drives an injected clock, so idle and rate windows are exact.
 */

#include "jpeg_encode.h"
#include "remote_screen_server_sink.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace std::chrono_literals;

namespace {

struct FakeClock {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{} + 1h;
    ScreenFrameStore::Clock fn() {
        return [this] { return now; };
    }
};

/// A full-screen BGRA frame of one color, drawn with no secret on the display.
struct SolidFrame {
    std::vector<uint8_t> px;
    RemoteScreenFrame f;
    SolidFrame(int w, int h, uint8_t shade) : px(static_cast<size_t>(w) * h * 4, shade) {
        f.px_map = px.data();
        f.px_map_len = px.size();
        f.x2 = w - 1;
        f.y2 = h - 1;
        f.disp_w = w;
        f.disp_h = h;
        f.src_stride = static_cast<uint32_t>(w * 4);
        f.src_format = RemoteScreenPixelFormat::BGRA8888;
        f.secret = ScreenSecret::Absent;
    }
};

constexpr SnapshotRateCap kCap{5, 2};

std::vector<uint8_t> encode(const std::vector<uint8_t>& rgb, int w, int h) {
    std::vector<uint8_t> out;
    REQUIRE(encode_jpeg_rgb888(rgb.data(), w, h, kRemoteScreenJpegQuality, out));
    return out;
}

/// The JPEG the store serves for a SolidFrame of @p shade.
std::vector<uint8_t> solid_jpeg(int w, int h, uint8_t shade) {
    return encode(std::vector<uint8_t>(static_cast<size_t>(w) * h * 3, shade), w, h);
}

std::vector<uint8_t> placeholder_jpeg(int w, int h) {
    std::vector<uint8_t> rgb;
    draw_screen_placeholder_rgb888(w, h, rgb);
    return encode(rgb, w, h);
}

std::vector<uint8_t> served(ScreenFrameStore& store) {
    auto snap = store.snapshot(false, kCap);
    REQUIRE(snap.has_value());
    return *snap->jpeg;
}

} // namespace

TEST_CASE("store: wants_frames false before any request, true after, false after kViewerIdleMs",
          "[remote_screen][server_sink]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    CHECK_FALSE(store.wants_frames());
    store.note_request();
    CHECK(store.wants_frames());
    clock.now += std::chrono::milliseconds(kViewerIdleMs - 1);
    CHECK(store.wants_frames());
    clock.now += 1ms;
    CHECK_FALSE(store.wants_frames());
}

TEST_CASE("store: only a viewer page poll makes a page viewer; any request wants frames",
          "[remote_screen][server_sink][remote_viewer]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    CHECK_FALSE(store.page_viewer_present());
    store.note_request();
    REQUIRE(store.wants_frames());
    CHECK_FALSE(store.page_viewer_present());

    store.note_page_view();
    CHECK(store.page_viewer_present());
    clock.now += std::chrono::milliseconds(kViewerIdleMs - 1);
    CHECK(store.page_viewer_present());
    clock.now += 1ms;
    CHECK_FALSE(store.page_viewer_present());
}

TEST_CASE("store: release_if_idle frees buffers only after idle", "[remote_screen][server_sink]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    store.note_request();
    SolidFrame frame(40, 30, 0x80);
    store.on_frame(frame.f);
    REQUIRE(store.snapshot(false, kCap).has_value());
    REQUIRE(store.allocated_bytes() > 0);

    CHECK_FALSE(store.release_if_idle());
    CHECK(store.allocated_bytes() > 0);

    clock.now += std::chrono::milliseconds(kViewerIdleMs);
    CHECK(store.release_if_idle());
    CHECK(store.allocated_bytes() == 0);
    CHECK_FALSE(store.snapshot(false, kCap).has_value());
}

TEST_CASE("store: requests ask for FullRedraw until a frame lands, then Ready",
          "[remote_screen][server_sink]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    CHECK(store.note_request() == ScreenFrameStore::Need::FullRedraw);
    // Still nothing captured (a repaint that never came): ask again.
    CHECK(store.note_request() == ScreenFrameStore::Need::FullRedraw);
    SolidFrame frame(8, 8, 1);
    store.on_frame(frame.f);
    CHECK(store.note_request() == ScreenFrameStore::Need::Ready);
    CHECK(store.frame_size() == std::pair<int, int>{8, 8});

    store.release();
    CHECK(store.frame_size() == std::pair<int, int>{0, 0});
    CHECK(store.note_request() == ScreenFrameStore::Need::FullRedraw);
    store.on_frame(frame.f);
    CHECK(store.note_request() == ScreenFrameStore::Need::Ready);
}

TEST_CASE("store: unchanged generation returns the cached jpeg pointer",
          "[remote_screen][server_sink]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    store.note_request();
    SolidFrame frame(16, 16, 0x40);
    store.on_frame(frame.f);
    auto a = store.snapshot(false, kCap);
    REQUIRE(a.has_value());
    clock.now += 10s; // far past any rate window
    store.note_request();
    auto b = store.snapshot(false, kCap);
    REQUIRE(b.has_value());
    CHECK(a->jpeg.get() == b->jpeg.get());
    CHECK(a->generation == b->generation);
}

TEST_CASE("store: a new generation inside min_encode_interval still returns the cache",
          "[remote_screen][server_sink]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    store.note_request();
    SolidFrame first(16, 16, 0x10);
    store.on_frame(first.f);
    auto a = store.snapshot(false, kCap);
    REQUIRE(a.has_value());

    SolidFrame second(16, 16, 0xF0);
    store.on_frame(second.f);
    clock.now += min_encode_interval(kCap, false) - 1ms;
    auto b = store.snapshot(false, kCap);
    REQUIRE(b.has_value());
    CHECK(b->jpeg.get() == a->jpeg.get());

    clock.now += 1ms;
    auto c = store.snapshot(false, kCap);
    REQUIRE(c.has_value());
    CHECK(c->jpeg.get() != a->jpeg.get());
    CHECK(c->generation > a->generation);
}

TEST_CASE("rate cap: printing selects printing_fps; constrained host gives 1",
          "[remote_screen][server_sink]") {
    CHECK(snapshot_rate_cap(false).idle_fps == 5);
    CHECK(snapshot_rate_cap(false).printing_fps == 2);
    CHECK(snapshot_rate_cap(true).idle_fps == 5);
    CHECK(snapshot_rate_cap(true).printing_fps == 1);
    CHECK(min_encode_interval({5, 1}, true) == 1000ms);
    CHECK(min_encode_interval({5, 2}, true) == 500ms);
    CHECK(min_encode_interval({5, 2}, false) == 200ms);
}

TEST_CASE("store: resolution change reallocates the shadow", "[remote_screen][server_sink]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    store.note_request();
    SolidFrame wide(100, 50, 0x20);
    store.on_frame(wide.f);
    auto a = store.snapshot(false, kCap);
    REQUIRE(a.has_value());
    CHECK(a->w == 100);
    CHECK(a->h == 50);

    SolidFrame tall(50, 100, 0x20);
    store.on_frame(tall.f);
    clock.now += 1s;
    auto b = store.snapshot(false, kCap);
    REQUIRE(b.has_value());
    CHECK(b->w == 50);
    CHECK(b->h == 100);
    // Partial rects alone cannot repaint a resized shadow, so the next viewer asks for one.
    CHECK(store.note_request() == ScreenFrameStore::Need::FullRedraw);
}

TEST_CASE("store: the full-repaint wait ends when a refresh after the invalidate lands",
          "[remote_screen][server_sink]") {
    ScreenFrameStore store;
    store.note_request();
    const uint64_t gen = store.full_generation();
    CHECK_FALSE(store.wait_for_full_refresh_after(gen, 20ms));

    SolidFrame frame(8, 8, 3);
    std::thread flusher([&] {
        std::this_thread::sleep_for(20ms);
        store.note_full_invalidate();
        store.on_frame(frame.f);
    });
    const auto start = std::chrono::steady_clock::now();
    CHECK(store.wait_for_full_refresh_after(gen, 2000ms));
    CHECK(std::chrono::steady_clock::now() - start < 1500ms);
    flusher.join();
}

TEST_CASE("store: a refresh that was not asked for does not answer the full-repaint wait",
          "[remote_screen][server_sink]") {
    // The repaint the viewer asked for is queued to the UI thread; a refresh already
    // under way (a navigation, a ticking clock) can finish first and covers only part
    // of the screen.
    ScreenFrameStore store;
    store.note_request();
    const uint64_t gen = store.full_generation();
    SolidFrame frame(8, 8, 3);
    store.on_frame(frame.f);
    CHECK(store.generation() > 0);
    CHECK(store.full_generation() == gen);
    CHECK_FALSE(store.wait_for_full_refresh_after(gen, 1ms));

    store.note_full_invalidate();
    store.on_frame(frame.f);
    CHECK(store.full_generation() == gen + 1);
    // Counted once: the next refresh is ordinary again.
    store.on_frame(frame.f);
    CHECK(store.full_generation() == gen + 1);
}

TEST_CASE("store: abort_waits wakes a waiter early", "[remote_screen][server_sink]") {
    ScreenFrameStore store;
    const uint64_t gen = store.full_generation();
    std::thread aborter([&] {
        std::this_thread::sleep_for(20ms);
        store.abort_waits();
    });
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(store.wait_for_full_refresh_after(gen, 5000ms));
    CHECK(std::chrono::steady_clock::now() - start < 2000ms);
    aborter.join();
}

TEST_CASE("store: snapshot before any frame is nullopt", "[remote_screen][server_sink]") {
    ScreenFrameStore store;
    store.note_request();
    CHECK_FALSE(store.snapshot(false, kCap).has_value());
}

TEST_CASE("server sink forwards gating and frames to its store", "[remote_screen][server_sink]") {
    FakeClock clock;
    auto store = std::make_shared<ScreenFrameStore>(clock.fn());
    ScreenServerSink sink(store);
    REQUIRE(sink.start());
    CHECK_FALSE(sink.wants_frames());
    store->note_request();
    CHECK(sink.wants_frames());
    const uint64_t gen = store->generation();
    SolidFrame frame(8, 8, 9);
    sink.on_frame(frame.f);
    CHECK(store->generation() == gen + 1);
    sink.stop();
    CHECK(store->allocated_bytes() == 0);
}

TEST_CASE("store: a refresh in stripes advances the generation only on its last stripe",
          "[remote_screen][server_sink]") {
    ScreenFrameStore store;
    store.note_request();
    store.note_full_invalidate();
    const uint64_t gen = store.generation();
    const uint64_t full = store.full_generation();
    std::vector<uint8_t> stripe(16 * 4 * 4, 0x40);
    for (int i = 0; i < 4; ++i) {
        RemoteScreenFrame f;
        f.px_map = stripe.data();
        f.px_map_len = stripe.size();
        f.x1 = 0;
        f.y1 = i * 4;
        f.x2 = 15;
        f.y2 = i * 4 + 3;
        f.px_map_x = 0;
        f.px_map_y = i * 4;
        f.disp_w = 16;
        f.disp_h = 16;
        f.src_stride = 64;
        f.src_format = RemoteScreenPixelFormat::BGRA8888;
        f.last_in_refresh = (i == 3);
        store.on_frame(f);
        if (i < 3) {
            // A viewer waiting for the repaint must not be answered with one stripe of it.
            CHECK(store.generation() == gen);
            CHECK_FALSE(store.wait_for_full_refresh_after(full, 1ms));
        }
    }
    CHECK(store.generation() == gen + 1);
    CHECK(store.wait_for_full_refresh_after(full, 1ms));
}

TEST_CASE("store: a rejected last stripe still completes a refresh whose other stripes landed",
          "[remote_screen][server_sink]") {
    ScreenFrameStore store;
    store.note_request();
    store.note_full_invalidate();
    const uint64_t gen = store.generation();
    std::vector<uint8_t> stripe(16 * 8 * 4, 0x40);
    RemoteScreenFrame f;
    f.px_map = stripe.data();
    f.px_map_len = stripe.size();
    f.x2 = 15;
    f.y2 = 7;
    f.disp_w = 16;
    f.disp_h = 16;
    f.src_stride = 64;
    f.src_format = RemoteScreenPixelFormat::BGRA8888;
    f.last_in_refresh = false;
    store.on_frame(f);
    // The last rect starts before its own source origin: rejected.
    RemoteScreenFrame bad = f;
    bad.y1 = 8;
    bad.y2 = 15;
    bad.px_map_y = 12;
    bad.last_in_refresh = true;
    store.on_frame(bad);
    CHECK(store.generation() == gen + 1);
    CHECK(store.full_generation() == 1);
}

// ---------------------------------------------------------------------------
// The pairing code is never served: a frame drawn while it was on the display is
// replaced by the placeholder until a whole repaint without it has landed.
// ---------------------------------------------------------------------------

TEST_CASE("secret: a frame drawn with the code shown serves the placeholder, not the screen",
          "[remote_screen][server_sink][secret]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    store.note_request();
    SolidFrame live(32, 24, 0x55);
    store.on_frame(live.f);
    REQUIRE(served(store) == solid_jpeg(32, 24, 0x55));
    const uint64_t live_gen = store.snapshot(false, kCap)->generation;

    SolidFrame code(32, 24, 0x99);
    code.f.secret = ScreenSecret::Shown;
    store.on_frame(code.f);
    const auto snap = store.snapshot(false, kCap);
    REQUIRE(snap.has_value());
    CHECK(*snap->jpeg == placeholder_jpeg(32, 24));
    CHECK(*snap->jpeg != solid_jpeg(32, 24, 0x99));
    // A new generation, so an open stream sends the placeholder too.
    CHECK(snap->generation > live_gen);
    CHECK_FALSE(store.screen_clean());

    // Neither the rate window nor more refreshes with the code shown reveal the screen.
    clock.now += 10s;
    store.on_frame(code.f);
    CHECK(served(store) == placeholder_jpeg(32, 24));
}

TEST_CASE("secret: a frame drawn with the state unknown serves the placeholder",
          "[remote_screen][server_sink][secret]") {
    ScreenFrameStore store;
    store.note_request();
    SolidFrame live(16, 16, 0x30);
    store.on_frame(live.f);
    REQUIRE(served(store) == solid_jpeg(16, 16, 0x30));

    SolidFrame unknown(16, 16, 0x30);
    unknown.f.secret = ScreenSecret::Unknown;
    store.on_frame(unknown.f);
    CHECK(served(store) == placeholder_jpeg(16, 16));
}

TEST_CASE("secret: the screen comes back only after a full repaint drawn without the code",
          "[remote_screen][server_sink][secret]") {
    FakeClock clock;
    ScreenFrameStore store(clock.fn());
    store.note_request();
    SolidFrame code(32, 24, 0x99);
    code.f.secret = ScreenSecret::Shown;
    store.on_frame(code.f);
    REQUIRE(served(store) == placeholder_jpeg(32, 24));

    // A partial refresh with the code gone may leave its pixels elsewhere in the shadow.
    SolidFrame gone(32, 24, 0x44);
    store.on_frame(gone.f);
    CHECK(served(store) == placeholder_jpeg(32, 24));
    CHECK(store.note_request() == ScreenFrameStore::Need::FullRedraw);

    // A whole repaint that has the code on it again does not count.
    store.note_full_invalidate();
    store.on_frame(code.f);
    store.on_frame(gone.f);
    CHECK(served(store) == placeholder_jpeg(32, 24));

    store.note_full_invalidate();
    store.on_frame(gone.f);
    CHECK(store.screen_clean());
    CHECK(served(store) == solid_jpeg(32, 24, 0x44));
}

TEST_CASE("secret: a repaint in stripes reveals the screen only on its last stripe",
          "[remote_screen][server_sink][secret]") {
    ScreenFrameStore store;
    store.note_request();
    SolidFrame code(8, 8, 0x99);
    code.f.secret = ScreenSecret::Shown;
    store.on_frame(code.f);

    store.note_full_invalidate();
    SolidFrame stripe(8, 8, 0x22);
    stripe.f.last_in_refresh = false;
    store.on_frame(stripe.f);
    CHECK_FALSE(store.screen_clean());
    CHECK(served(store) == placeholder_jpeg(8, 8));
    stripe.f.last_in_refresh = true;
    store.on_frame(stripe.f);
    CHECK(store.screen_clean());
}

TEST_CASE("secret: the placeholder fills the screen size with a centered line of text",
          "[remote_screen][server_sink][secret]") {
    std::vector<uint8_t> rgb;
    draw_screen_placeholder_rgb888(800, 480, rgb);
    REQUIRE(rgb.size() == 800u * 480u * 3u);
    const auto lit = std::count_if(rgb.begin(), rgb.end(), [&](uint8_t v) { return v != rgb[0]; });
    CHECK(lit > 0);
    // Nothing lit in the top and bottom quarters: the line sits in the middle.
    const size_t quarter = 800u * 120u * 3u;
    CHECK(std::all_of(rgb.begin(), rgb.begin() + quarter, [&](uint8_t v) { return v == rgb[0]; }));
    CHECK(std::all_of(rgb.end() - quarter, rgb.end(), [&](uint8_t v) { return v == rgb[0]; }));

    // Too small to hold the line: a plain fill.
    draw_screen_placeholder_rgb888(10, 10, rgb);
    REQUIRE(rgb.size() == 300u);
    CHECK(std::all_of(rgb.begin(), rgb.end(), [&](uint8_t v) { return v == rgb[0]; }));
}
