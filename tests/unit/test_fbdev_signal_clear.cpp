// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The SIGTERM fast exit blanks the framebuffer with clear_framebuffer_signal_safe():
 * open/write/close over a path, length and depth the fbdev backend cached at
 * startup. 32bpp gets opaque black pixels, other depths zeros.
 * A regular file stands in for /dev/fb0 here; write(2) from offset 0 behaves the
 * same on both.
 */

#include "fbdev_signal_clear.h"
#include "framebuffer_fill.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

namespace {

struct FakeFramebuffer {
    std::string path;

    explicit FakeFramebuffer(size_t bytes) {
        path = (fs::temp_directory_path() / ("helix_fb_signal_clear_" + std::to_string(::getpid())))
                   .string();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::vector<char> ones(bytes, static_cast<char>(0xFF));
        out.write(ones.data(), static_cast<std::streamsize>(ones.size()));
    }
    ~FakeFramebuffer() {
        helix::cache_framebuffer_for_signal("", 0, 0);
        std::error_code ec;
        fs::remove(path, ec);
    }

    std::vector<unsigned char> contents() const {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

} // namespace

TEST_CASE("clear_framebuffer_signal_safe zeroes a 16bpp framebuffer", "[fbdev][signal_safe]") {
    // Not a multiple of the write chunk, so the short final write is exercised.
    constexpr size_t kBytes = 3 * 4096 + 123;
    FakeFramebuffer fb(kBytes);
    REQUIRE(helix::cache_framebuffer_for_signal(fb.path, kBytes, 16));

    helix::clear_framebuffer_signal_safe();

    const auto bytes = fb.contents();
    REQUIRE(bytes.size() == kBytes);
    CHECK(std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; }));
}

TEST_CASE("clear_framebuffer_signal_safe writes opaque black pixels at 32bpp",
          "[fbdev][signal_safe]") {
    constexpr size_t kBytes = 2 * 4096 + 64;
    FakeFramebuffer fb(kBytes);
    REQUIRE(helix::cache_framebuffer_for_signal(fb.path, kBytes, 32));

    helix::clear_framebuffer_signal_safe();

    const auto bytes = fb.contents();
    REQUIRE(bytes.size() == kBytes);
    bool all_black = true;
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        uint32_t word = 0;
        std::memcpy(&word, &bytes[i], sizeof(word));
        all_black = all_black && word == 0xFF000000u;
    }
    CHECK(all_black);
}

TEST_CASE("clear_framebuffer_signal_safe does nothing with no framebuffer registered",
          "[fbdev][signal_safe]") {
    constexpr size_t kBytes = 4096;
    FakeFramebuffer fb(kBytes);
    REQUIRE(helix::cache_framebuffer_for_signal(fb.path, kBytes, 16));
    CHECK_FALSE(helix::cache_framebuffer_for_signal("", kBytes, 16));

    helix::clear_framebuffer_signal_safe();

    const auto bytes = fb.contents();
    REQUIRE(bytes.size() == kBytes);
    CHECK(std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0xFF; }));
}

TEST_CASE("cache_framebuffer_for_signal rejects a zero length and an oversized path",
          "[fbdev][signal_safe]") {
    // A path that cannot exist: a case that aborts before deregistering must
    // never leave the dev box's real framebuffer as the target.
    const std::string missing = "/nonexistent/helix-test-fb";
    CHECK_FALSE(helix::cache_framebuffer_for_signal(missing, 0, 32));
    CHECK_FALSE(helix::cache_framebuffer_for_signal(std::string(300, 'x'), 4096, 32));
    CHECK(helix::cache_framebuffer_for_signal(missing, 4096, 32));
    helix::clear_framebuffer_signal_safe();
    helix::cache_framebuffer_for_signal("", 0, 0);
}

TEST_CASE("fill_framebuffer writes the depth's pixel", "[fbdev][display]") {
    std::vector<uint32_t> fb(4, 0xDEADBEEFu);

    SECTION("32bpp stores ARGB unchanged") {
        helix::fill_framebuffer(fb.data(), 16, 32, 0xFF102030u);
        CHECK(std::all_of(fb.begin(), fb.end(), [](uint32_t p) { return p == 0xFF102030u; }));
    }
    SECTION("16bpp converts to RGB565") {
        helix::fill_framebuffer(fb.data(), 16, 16, 0xFFFF8040u);
        const auto* px = reinterpret_cast<const uint16_t*>(fb.data());
        CHECK(std::all_of(px, px + 8, [](uint16_t p) { return p == 0xFC08u; }));
    }
    SECTION("any other depth is zeroed") {
        helix::fill_framebuffer(fb.data(), 16, 24, 0xFFFFFFFFu);
        CHECK(std::all_of(fb.begin(), fb.end(), [](uint32_t p) { return p == 0u; }));
    }
    SECTION("the depth comes from the stride, 32 without a width") {
        CHECK(helix::framebuffer_fill_bpp(800, 1600) == 16);
        CHECK(helix::framebuffer_fill_bpp(0, 1600) == 32);
    }
}
