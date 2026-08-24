// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_lvgl_image_writer.cpp
 * @brief Unit tests for the shared LVGL binary image writer
 *
 * Tests write_lvgl_bin(): header correctness, atomic write semantics,
 * failure handling, and round-trip verification.
 */

#include "../../include/lvgl_image_writer.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Helper: read entire file into a byte vector
// ============================================================================
static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

// ============================================================================
// LVGL Image Writer Tests
// ============================================================================

TEST_CASE("write_lvgl_bin produces valid LVGL header", "[lvgl_image_writer]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "helix_test_lvgl_writer";
    std::filesystem::create_directories(tmp_dir);
    std::string out_path = (tmp_dir / "header_test.bin").string();

    constexpr int width = 64;
    constexpr int height = 48;
    constexpr uint8_t cf = 0x10; // LV_COLOR_FORMAT_ARGB8888
    constexpr size_t pixel_bytes = width * height * 4;
    std::vector<uint8_t> pixels(pixel_bytes, 0xAB);

    REQUIRE(write_lvgl_bin(out_path, width, height, cf, pixels.data(), pixels.size()));

    auto data = read_file_bytes(out_path);
    REQUIRE(data.size() >= sizeof(lv_image_header_t));

    lv_image_header_t header;
    std::memcpy(&header, data.data(), sizeof(header));

    SECTION("Magic bytes match LV_IMAGE_HEADER_MAGIC") {
        REQUIRE(header.magic == LV_IMAGE_HEADER_MAGIC);
    }

    SECTION("Dimensions are correct") {
        REQUIRE(header.w == width);
        REQUIRE(header.h == height);
    }

    SECTION("Color format is ARGB8888") {
        REQUIRE(static_cast<uint8_t>(header.cf) == cf);
    }

    SECTION("Stride is width * 4 (ARGB8888)") {
        REQUIRE(header.stride == width * 4);
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("write_lvgl_bin uses atomic write (temp file + rename)", "[lvgl_image_writer]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "helix_test_lvgl_atomic";
    std::filesystem::create_directories(tmp_dir);
    std::string out_path = (tmp_dir / "atomic_test.bin").string();
    std::string temp_path = out_path + ".tmp";

    constexpr int width = 8;
    constexpr int height = 8;
    constexpr uint8_t cf = 0x10;
    std::vector<uint8_t> pixels(width * height * 4, 0xFF);

    REQUIRE(write_lvgl_bin(out_path, width, height, cf, pixels.data(), pixels.size()));

    SECTION("Final file exists after successful write") {
        REQUIRE(std::filesystem::exists(out_path));
    }

    SECTION("Temp file is cleaned up after successful write") {
        REQUIRE_FALSE(std::filesystem::exists(temp_path));
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("write_lvgl_bin returns false on write failure (bad path)", "[lvgl_image_writer]") {
    // Path to a non-existent directory - opening the file should fail
    std::string bad_path = "/no_such_directory_abc123/test.bin";

    constexpr int width = 4;
    constexpr int height = 4;
    constexpr uint8_t cf = 0x10;
    std::vector<uint8_t> pixels(width * height * 4, 0x00);

    REQUIRE_FALSE(write_lvgl_bin(bad_path, width, height, cf, pixels.data(), pixels.size()));
}

TEST_CASE("write_lvgl_bin output file size = header + pixel data", "[lvgl_image_writer]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "helix_test_lvgl_size";
    std::filesystem::create_directories(tmp_dir);
    std::string out_path = (tmp_dir / "size_test.bin").string();

    constexpr int width = 32;
    constexpr int height = 16;
    constexpr uint8_t cf = 0x10;
    constexpr size_t pixel_bytes = width * height * 4;
    std::vector<uint8_t> pixels(pixel_bytes, 0x42);

    REQUIRE(write_lvgl_bin(out_path, width, height, cf, pixels.data(), pixels.size()));

    auto file_size = std::filesystem::file_size(out_path);
    REQUIRE(file_size == sizeof(lv_image_header_t) + pixel_bytes);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("write_lvgl_bin round-trip: write then read back and verify header",
          "[lvgl_image_writer]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "helix_test_lvgl_roundtrip";
    std::filesystem::create_directories(tmp_dir);
    std::string out_path = (tmp_dir / "roundtrip_test.bin").string();

    constexpr int width = 100;
    constexpr int height = 75;
    constexpr uint8_t cf = 0x10;
    constexpr size_t pixel_bytes = width * height * 4;

    // Fill with recognizable pattern
    std::vector<uint8_t> pixels(pixel_bytes);
    for (size_t i = 0; i < pixel_bytes; ++i) {
        pixels[i] = static_cast<uint8_t>(i & 0xFF);
    }

    REQUIRE(write_lvgl_bin(out_path, width, height, cf, pixels.data(), pixels.size()));

    // Read back
    auto data = read_file_bytes(out_path);
    REQUIRE(data.size() == sizeof(lv_image_header_t) + pixel_bytes);

    // Verify header fields
    lv_image_header_t header;
    std::memcpy(&header, data.data(), sizeof(header));
    REQUIRE(header.magic == LV_IMAGE_HEADER_MAGIC);
    REQUIRE(header.w == width);
    REQUIRE(header.h == height);
    REQUIRE(static_cast<uint8_t>(header.cf) == cf);
    REQUIRE(header.stride == width * 4);

    // Verify pixel data matches what we wrote
    const uint8_t* read_pixels = data.data() + sizeof(lv_image_header_t);
    REQUIRE(std::memcmp(read_pixels, pixels.data(), pixel_bytes) == 0);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Concurrent writers to one path do not interleave into a shared temp file",
          "[assets][image_writer][threading][960]") {
    // The staging name must be unique per WRITER, not per destination. The
    // thumbnail pool runs two workers over one cache directory, and two
    // requests for the same source at the same target size resolve to the same
    // output path — with a fixed "<path>.tmp" both wrote into one file before
    // both renamed it. Identical payloads made that benign in production, so
    // the writers here deliberately carry DIFFERENT bytes: the final file must
    // be exactly one of them, never a blend.
    //
    // Detection is PROBABILISTIC, as any race test is. Measured against the
    // known-bad shared-".tmp" build it caught the tear in roughly 1 run in 3,
    // and only once the payload was large enough (1 MB) to span many write(2)
    // calls — at 64x64 it never reproduced at all, which is why the dimensions
    // below look excessive. It is reliable in the direction CI cares about:
    // with per-writer staging it passed 5/5, because each writer owns its temp
    // file and rename(2) is atomic. A regression net, not a proof.
    const auto tmp_dir = std::filesystem::temp_directory_path() /
                         ("helix_writer_race_" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::create_directories(tmp_dir);
    const std::string out_path = (tmp_dir / "contended.bin").string();

    constexpr int WIDTH = 512;
    constexpr int HEIGHT = 512;
    constexpr size_t PIXEL_BYTES = WIDTH * HEIGHT * 4;
    constexpr int WRITERS = 8;

    std::vector<std::thread> writers;
    writers.reserve(WRITERS);
    for (int w = 0; w < WRITERS; ++w) {
        writers.emplace_back([&, w] {
            // Each writer fills with its own distinct byte.
            std::vector<uint8_t> pixels(PIXEL_BYTES, static_cast<uint8_t>(0x10 + w));
            for (int rep = 0; rep < 30; ++rep) {
                write_lvgl_bin(out_path, WIDTH, HEIGHT,
                               static_cast<uint8_t>(LV_COLOR_FORMAT_ARGB8888), pixels.data(),
                               pixels.size());
            }
        });
    }
    for (auto& t : writers) {
        t.join();
    }

    auto data = read_file_bytes(out_path);
    REQUIRE(data.size() == sizeof(lv_image_header_t) + PIXEL_BYTES);

    // Every pixel byte must come from ONE writer. A torn file mixes fills.
    const uint8_t* px = data.data() + sizeof(lv_image_header_t);
    const uint8_t first = px[0];
    bool uniform = true;
    for (size_t i = 0; i < PIXEL_BYTES; ++i) {
        if (px[i] != first) {
            uniform = false;
            break;
        }
    }
    CHECK(uniform);
    CHECK(first >= 0x10);
    CHECK(first < static_cast<uint8_t>(0x10 + WRITERS));

    // No staging files left behind.
    int leftovers = 0;
    for (const auto& e : std::filesystem::directory_iterator(tmp_dir)) {
        if (e.path().string().find(".tmp") != std::string::npos) {
            ++leftovers;
        }
    }
    CHECK(leftovers == 0);

    std::filesystem::remove_all(tmp_dir);
}
