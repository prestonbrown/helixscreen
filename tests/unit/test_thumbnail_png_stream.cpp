// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../lib/tinfl/tinfl_copy.h"
#include "lvgl.h"
#include "stb_image.h"
#include "thumbnail_downscale.h"
#include "thumbnail_png_stream.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::DecodedThumbnail;
using helix::PngHeader;
using helix::PngPalette;
using helix::PngRowDecoder;
using helix::ThumbnailDecodeFailure;
using helix::ThumbnailDims;

namespace {

/// The decode driven by miniz's tinfl, the inflater the firmware's ROM ships,
/// with an allocator that counts and can be told to fail.
struct TestInflate {
    using Decompressor = tinfl_copy::tinfl_decompressor;
    static constexpr size_t WINDOW = TINFL_LZ_DICT_SIZE;
    static inline int live = 0;
    static inline int fail_after = -1; ///< allocations left before one fails; -1 never

    static void init(Decompressor* d) {
        tinfl_init(d);
    }
    static int step(Decompressor* d, const uint8_t* in, size_t* in_size, uint8_t* window,
                    uint8_t* next, size_t* out_size, bool more_input) {
        return tinfl_copy::tinfl_decompress(
            d, in, in_size, window, next, out_size,
            tinfl_copy::TINFL_FLAG_PARSE_ZLIB_HEADER |
                (more_input ? tinfl_copy::TINFL_FLAG_HAS_MORE_INPUT : 0));
    }
    static void* alloc(size_t size) {
        if (fail_after == 0) {
            return nullptr;
        }
        if (fail_after > 0) {
            --fail_after;
        }
        ++live;
        return std::malloc(size);
    }
    static void free(void* p) {
        if (p) {
            --live;
        }
        std::free(p);
    }
    static inline size_t free_left = SIZE_MAX / 2;
    static size_t free_bytes() {
        return free_left;
    }
};

std::vector<uint8_t> fixture(const char* name) {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    dir = pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/" : "tests/fixtures/";
    std::ifstream in(dir + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> stb_rgba(const std::vector<uint8_t>& png, int& w, int& h) {
    int n = 0;
    uint8_t* px = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 4);
    REQUIRE(px != nullptr);
    std::vector<uint8_t> out(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return out;
}

/// What the streaming decode must produce: the whole image decoded by
/// stb_image, then downscaled in one go.
std::vector<uint8_t> expected(const std::vector<uint8_t>& png, int max_w, int max_h,
                              ThumbnailDims& dims) {
    int w = 0, h = 0;
    const auto rgba = stb_rgba(png, w, h);
    dims = helix::fit_thumbnail(w, h, max_w, max_h);
    std::vector<uint8_t> out(helix::rgb565a8_size(dims));
    helix::downscale_rgba_to_rgb565a8(rgba.data(), w, h, dims, out.data());
    return out;
}

DecodedThumbnail decode(const std::vector<uint8_t>& png, int max_w, int max_h) {
    return helix::decode_png_thumbnail<TestInflate>(png.data(), png.size(), max_w, max_h);
}

/// Rewrites a chunk's CRC after the test edits its bytes.
void fix_crc(std::vector<uint8_t>& png, size_t chunk_at) {
    const uint32_t len = (uint32_t{png[chunk_at]} << 24) | (uint32_t{png[chunk_at + 1]} << 16) |
                         (uint32_t{png[chunk_at + 2]} << 8) | png[chunk_at + 3];
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = chunk_at + 4; i < chunk_at + 8 + len; ++i) {
        c ^= png[i];
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
    }
    c ^= 0xFFFFFFFFu;
    for (int k = 0; k < 4; ++k) {
        png[chunk_at + 8 + len + static_cast<size_t>(k)] = static_cast<uint8_t>(c >> (24 - 8 * k));
    }
}

constexpr size_t IHDR_AT = 8;

} // namespace

TEST_CASE("the streaming decode matches a whole-image decode and downscale",
          "[thumbnail][png_stream]") {
    TestInflate::live = 0;
    // The 300x300 fixture inflates to 360KB, so the 32KB window wraps many times.
    // The small fixtures cycle their rows through all five PNG filter types and
    // cover palette+tRNS, an RGB colour key and grey+alpha.
    struct Case {
        const char* name;
        int box;
    };
    for (const Case& c :
         {Case{"thumbnail_300_rgba.png", 166}, Case{"thumb_filters_rgba.png", 37},
          Case{"thumb_filters_rgba.png", 16}, Case{"thumb_filters_rgb_key.png", 37},
          Case{"thumb_filters_grey_alpha.png", 20}, Case{"thumb_filters_palette.png", 37}}) {
        CAPTURE(c.name, c.box);
        const auto png = fixture(c.name);
        REQUIRE(!png.empty());
        ThumbnailDims dims;
        const auto want = expected(png, c.box, c.box, dims);
        const DecodedThumbnail got = decode(png, c.box, c.box);
        REQUIRE(got.failure == ThumbnailDecodeFailure::None);
        REQUIRE(got.pixels != nullptr);
        CHECK(got.dims.w == dims.w);
        CHECK(got.dims.h == dims.h);
        CHECK(std::memcmp(got.pixels, want.data(), want.size()) == 0);
        TestInflate::free(got.pixels);
    }
    CHECK(TestInflate::live == 0);
}

TEST_CASE("the row decoder takes its input in pieces of any size", "[thumbnail][png_stream]") {
    const auto png = fixture("thumb_filters_palette.png");
    PngHeader h;
    REQUIRE(helix::read_png_header(png.data(), png.size(), h));
    PngPalette palette;
    std::vector<uint8_t> zdata;
    REQUIRE(helix::for_each_png_idat(
        png.data(), png.size(), palette,
        [](const uint8_t* d, size_t n, void* user) {
            auto* z = static_cast<std::vector<uint8_t>*>(user);
            z->insert(z->end(), d, d + n);
            return true;
        },
        &zdata));
    int raw_len = 0;
    char* raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(zdata.data()),
                                        static_cast<int>(zdata.size()), &raw_len);
    REQUIRE(raw != nullptr);
    int w = 0, hh = 0;
    const auto want = stb_rgba(png, w, hh);
    for (size_t piece : {size_t{1}, size_t{7}, size_t{4096}}) {
        CAPTURE(piece);
        std::vector<uint8_t> got;
        PngRowDecoder rows(
            h, palette,
            [](const uint8_t* rgba, void* user) {
                auto* g = static_cast<std::vector<uint8_t>*>(user);
                g->insert(g->end(), rgba, rgba + 37 * 4);
            },
            &got);
        REQUIRE(rows.ok());
        for (int at = 0; at < raw_len; at += static_cast<int>(piece)) {
            const size_t n = std::min(piece, static_cast<size_t>(raw_len - at));
            REQUIRE(rows.feed(reinterpret_cast<const uint8_t*>(raw) + at, n));
        }
        CHECK(rows.complete());
        CHECK(got == want);
    }
    stbi_image_free(raw);
}

TEST_CASE("interlaced, 16-bit and oversized thumbnails keep the placeholder",
          "[thumbnail][png_stream]") {
    auto png = fixture("thumb_filters_rgba.png");
    auto with = [&](size_t field, uint8_t value) {
        auto edited = png;
        edited[IHDR_AT + 8 + field] = value;
        fix_crc(edited, IHDR_AT);
        return decode(edited, 37, 37);
    };
    CHECK(with(12, 1).failure == ThumbnailDecodeFailure::Unsupported); // interlace method
    CHECK(with(8, 16).failure == ThumbnailDecodeFailure::Unsupported); // bit depth
    CHECK(with(8, 4).failure == ThumbnailDecodeFailure::Unsupported);
    CHECK(with(9, 5).failure == ThumbnailDecodeFailure::Unsupported); // no such colour type
    CHECK(with(0, 1).failure == ThumbnailDecodeFailure::Unsupported); // width >= 16M
    CHECK(TestInflate::live == 0);
}

TEST_CASE("a truncated, corrupt or mislabelled PNG fails cleanly", "[thumbnail][png_stream]") {
    TestInflate::live = 0;
    const auto png = fixture("thumb_filters_rgba.png");

    // Cut short anywhere after the header, the way a capped fetch ends one.
    for (size_t keep = 33; keep < png.size(); keep += 97) {
        CAPTURE(keep);
        const std::vector<uint8_t> cut(png.begin(), png.begin() + static_cast<long>(keep));
        const DecodedThumbnail got = decode(cut, 37, 37);
        CHECK(got.pixels == nullptr);
        CHECK(got.failure == ThumbnailDecodeFailure::BadImage);
    }

    // A palette entry changed with its CRC left stale: only the CRC can tell.
    auto bad_plte = fixture("thumb_filters_palette.png");
    const size_t plte = 8 + 25;
    REQUIRE(std::memcmp(bad_plte.data() + plte + 4, "PLTE", 4) == 0);
    bad_plte[plte + 8] ^= 0xFF;
    CHECK(decode(bad_plte, 37, 37).failure == ThumbnailDecodeFailure::BadImage);

    // A flipped byte inside the first IDAT, with its CRC left stale.
    const size_t idat = 8 + 25; // after the signature and IHDR
    REQUIRE(std::memcmp(png.data() + idat + 4, "IDAT", 4) == 0);
    auto bad_crc = png;
    bad_crc[idat + 8 + 5] ^= 0xFF;
    CHECK(decode(bad_crc, 37, 37).failure == ThumbnailDecodeFailure::BadImage);

    // The same flip with a matching CRC reaches the inflater, which rejects it.
    auto bad_data = bad_crc;
    fix_crc(bad_data, idat);
    CHECK(decode(bad_data, 37, 37).pixels == nullptr);

    // A chunk length claiming more bytes than the file holds.
    auto bad_len = png;
    bad_len[idat] = 0x7F;
    CHECK(decode(bad_len, 37, 37).failure == ThumbnailDecodeFailure::BadImage);

    CHECK(decode({}, 37, 37).failure == ThumbnailDecodeFailure::BadImage);
    CHECK(TestInflate::live == 0);
}

TEST_CASE("an allocation failure at any point fails the decode and frees what it took",
          "[thumbnail][png_stream]") {
    const auto png = fixture("thumb_filters_rgba.png");
    for (int n = 0; n < 3; ++n) { // the image, the inflater state, the window
        CAPTURE(n);
        TestInflate::live = 0;
        TestInflate::fail_after = n;
        const DecodedThumbnail got = decode(png, 37, 37);
        CHECK(got.pixels == nullptr);
        CHECK(got.failure == ThumbnailDecodeFailure::OutOfMemory);
        CHECK(TestInflate::live == 0);
    }
    TestInflate::fail_after = -1;
}

TEST_CASE("mutated PNGs never crash the decoder", "[thumbnail][png_stream]") {
    TestInflate::live = 0;
    const auto base = fixture("thumb_filters_rgba.png");
    std::mt19937 rng(1234);
    for (int i = 0; i < 3000; ++i) {
        auto png = base;
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits; ++e) {
            png[rng() % png.size()] = static_cast<uint8_t>(rng());
        }
        if (rng() % 3 == 0) {
            png.resize(rng() % png.size());
        }
        // Mostly re-sign the chunks so edits get past the CRC check to the
        // length, palette, filter and inflate paths.
        if (rng() % 4 != 0) {
            for (size_t at = 8; at + 12 <= png.size();) {
                const uint32_t len = (uint32_t{png[at]} << 24) | (uint32_t{png[at + 1]} << 16) |
                                     (uint32_t{png[at + 2]} << 8) | png[at + 3];
                if (len > png.size() - at - 12) {
                    break;
                }
                fix_crc(png, at);
                at += 12 + len;
            }
        }
        const DecodedThumbnail got = decode(png, 37, 37);
        TestInflate::free(got.pixels);
    }
    CHECK(TestInflate::live == 0);
}

TEST_CASE("a decode that would cut into the PSRAM floor is not started",
          "[thumbnail][png_stream][budget]") {
    TestInflate::live = 0;
    const auto png = fixture("thumbnail_300_rgba.png");
    const ThumbnailDims dims = helix::fit_thumbnail(300, 300, 166, 166);
    const size_t need =
        helix::rgb565a8_size(dims) + helix::thumbnail_decode_working_bytes(300, dims);

    TestInflate::free_left = helix::THUMBNAIL_PSRAM_FLOOR + need - 1;
    const DecodedThumbnail refused = decode(png, 166, 166);
    CHECK(refused.pixels == nullptr);
    CHECK(refused.failure == ThumbnailDecodeFailure::OutOfMemory);
    CHECK(TestInflate::live == 0); // nothing was even allocated

    TestInflate::free_left = helix::THUMBNAIL_PSRAM_FLOOR + need;
    const DecodedThumbnail ok = decode(png, 166, 166);
    CHECK(ok.failure == ThumbnailDecodeFailure::None);
    TestInflate::free(ok.pixels);
    TestInflate::free_left = SIZE_MAX / 2;
}

TEST_CASE("the floor and the card budget are hard edges", "[thumbnail][budget]") {
    using helix::card_thumbnail_fits_budget;
    using helix::thumbnail_decode_fits;
    CHECK(thumbnail_decode_fits(300 * 1024, 40 * 1024, 4 * 1024, 256 * 1024));
    CHECK_FALSE(thumbnail_decode_fits(300 * 1024, 40 * 1024, 4 * 1024 + 1, 256 * 1024));
    CHECK_FALSE(thumbnail_decode_fits(100 * 1024, 0, 0, 256 * 1024)); // already under the floor

    CHECK(card_thumbnail_fits_budget(0, 960 * 1024, 960 * 1024));
    CHECK(card_thumbnail_fits_budget(880 * 1024, 80 * 1024, 960 * 1024));
    CHECK_FALSE(card_thumbnail_fits_budget(880 * 1024, 80 * 1024 + 1, 960 * 1024));
    CHECK_FALSE(card_thumbnail_fits_budget(970 * 1024, 0, 960 * 1024)); // already over
}

TEST_CASE("decoding into a caller's buffer allocates only the working memory",
          "[thumbnail][png_stream][slots]") {
    TestInflate::live = 0;
    const auto png = fixture("thumbnail_300_rgba.png");
    ThumbnailDims dims;
    const auto want = expected(png, 166, 166, dims);
    std::vector<uint8_t> slot(helix::rgb565a8_size({166, 166}), 0xAA);

    const DecodedThumbnail got = helix::decode_png_thumbnail<TestInflate>(
        png.data(), png.size(), 166, 166, slot.data(), slot.size());
    REQUIRE(got.failure == ThumbnailDecodeFailure::None);
    CHECK(got.pixels == slot.data());
    CHECK(std::memcmp(slot.data(), want.data(), want.size()) == 0);
    CHECK(TestInflate::live == 0); // nothing of the decode's is left over

    // The floor counts only the working memory when the image has a home.
    TestInflate::free_left =
        helix::THUMBNAIL_PSRAM_FLOOR + helix::thumbnail_decode_working_bytes(300, dims);
    CHECK(helix::decode_png_thumbnail<TestInflate>(png.data(), png.size(), 166, 166, slot.data(),
                                                   slot.size())
              .failure == ThumbnailDecodeFailure::None);
    TestInflate::free_left = SIZE_MAX / 2;

    // A buffer smaller than the box's image is refused, not overrun.
    CHECK(helix::decode_png_thumbnail<TestInflate>(png.data(), png.size(), 166, 166, slot.data(),
                                                   slot.size() - 1)
              .failure == ThumbnailDecodeFailure::Unsupported);
    CHECK(TestInflate::live == 0);
}

TEST_CASE("a decode onto a backdrop fills the whole box, the image centred and blended",
          "[thumbnail][png_stream][slots]") {
    TestInflate::live = 0;
    const auto png = fixture("thumbnail_300_rgba.png");
    const int box_w = 200, box_h = 166; // wider than the square image: margins left and right
    ThumbnailDims dims;
    const auto a8 = expected(png, box_w, box_h, dims);
    REQUIRE(dims.w < box_w);

    std::vector<uint16_t> back(static_cast<size_t>(box_w) * box_h);
    for (size_t i = 0; i < back.size(); ++i) {
        back[i] = static_cast<uint16_t>(i * 2654435761u >> 7);
    }
    // LVGL centres an image in a larger widget the same way.
    const int ox = box_w / 2 - dims.w / 2, oy = box_h / 2 - dims.h / 2;
    std::vector<uint16_t> want = back;
    for (int y = 0; y < dims.h; ++y) {
        for (int x = 0; x < dims.w; ++x) {
            const size_t i = static_cast<size_t>(y) * dims.w + x;
            uint16_t c;
            std::memcpy(&c, a8.data() + i * 2, 2);
            uint16_t& px = want[static_cast<size_t>(oy + y) * box_w + ox + x];
            px = lv_color_16_16_mix(c, px, a8[static_cast<size_t>(dims.w) * dims.h * 2 + i]);
        }
    }

    std::vector<uint16_t> slot(back.size(), 0xAAAA);
    const DecodedThumbnail got = helix::decode_png_thumbnail<TestInflate>(
        png.data(), png.size(), box_w, box_h, reinterpret_cast<uint8_t*>(slot.data()),
        slot.size() * 2, back.data());
    REQUIRE(got.failure == ThumbnailDecodeFailure::None);
    CHECK(got.opaque);
    CHECK(got.dims.w == box_w);
    CHECK(got.dims.h == box_h);
    CHECK(slot == want);
    CHECK(TestInflate::live == 0);

    // The opaque image is the box at two bytes a pixel; a smaller slot is refused.
    CHECK(helix::decode_png_thumbnail<TestInflate>(png.data(), png.size(), box_w, box_h,
                                                   reinterpret_cast<uint8_t*>(slot.data()),
                                                   slot.size() * 2 - 1, back.data())
              .failure == ThumbnailDecodeFailure::Unsupported);

    // Without a slot the decode allocates the box itself.
    const DecodedThumbnail owned = helix::decode_png_thumbnail<TestInflate>(
        png.data(), png.size(), box_w, box_h, nullptr, 0, back.data());
    REQUIRE(owned.pixels);
    CHECK(std::memcmp(owned.pixels, want.data(), want.size() * 2) == 0);
    TestInflate::free(owned.pixels);
    CHECK(TestInflate::live == 0);
}
