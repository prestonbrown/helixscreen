// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_image_cache_oversize.cpp
 * @brief An image too large for the image cache still decodes and draws
 *
 * LVGL's LRU reports a reserve larger than the whole cache as TOO_LARGE before it
 * evicts anything, so an oversized image can never be stored. The decoder must hand
 * back the bitmap it decoded rather than discard it, or the image renders as nothing.
 * Covered by patches/lvgl-image-cache-oversize-uncached.patch.
 */

#include "../../include/lvgl_image_writer.h"
#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/image/lv_image_decoder_private.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Writes an ARGB8888 .bin whose decoded size is `w*h*4` bytes.
std::string make_bin(const std::string& name, int w, int h) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0xA0);
    REQUIRE(write_lvgl_bin(path, w, h, LV_COLOR_FORMAT_ARGB8888, px.data(), px.size()));
    return path;
}

/// Opens `path` through LVGL and reports whether a bitmap came back, and whether it
/// was cached. `dsc.cache_entry` stays null when the image bypassed the cache.
struct OpenResult {
    lv_result_t res;
    bool has_bitmap;
    bool cached;
};

OpenResult open_via_lvgl(const std::string& path) {
    const std::string lvgl_path = "A:" + path;
    lv_image_decoder_dsc_t dsc;
    const lv_result_t res = lv_image_decoder_open(&dsc, lvgl_path.c_str(), nullptr);
    OpenResult out{res, res == LV_RESULT_OK && dsc.decoded != nullptr,
                   res == LV_RESULT_OK && dsc.cache_entry != nullptr};
    if (res == LV_RESULT_OK) {
        lv_image_decoder_close(&dsc);
    }
    return out;
}

/// Sizes the image cache for one test and puts it back on the way out. The shipping
/// default is 0, and a leaked non-zero size changes every later test in the shard.
struct ScopedImageCache {
    explicit ScopedImageCache(uint32_t bytes) {
        lv_image_cache_resize(bytes, false);
    }
    ~ScopedImageCache() {
        lv_image_cache_resize(0, true);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Image larger than the cache still decodes", "[image][cache]") {
    // 256x256 ARGB8888 = 256 KB decoded, against a 64 KB cache.
    const std::string big = make_bin("helix_oversize_big.bin", 256, 256);
    ScopedImageCache cache(64 * 1024);

    const OpenResult r = open_via_lvgl(big);

    CHECK(r.res == LV_RESULT_OK);
    CHECK(r.has_bitmap);
    // It cannot be stored, so it must have drawn from the uncached path.
    CHECK_FALSE(r.cached);

    std::filesystem::remove(big);
}

TEST_CASE_METHOD(LVGLTestFixture, "Image that fits the cache is cached", "[image][cache]") {
    // 64x64 ARGB8888 = 16 KB decoded, well inside the same 64 KB cache.
    const std::string small = make_bin("helix_oversize_small.bin", 64, 64);
    ScopedImageCache cache(64 * 1024);

    const OpenResult r = open_via_lvgl(small);

    CHECK(r.res == LV_RESULT_OK);
    CHECK(r.has_bitmap);
    CHECK(r.cached);

    std::filesystem::remove(small);
}
