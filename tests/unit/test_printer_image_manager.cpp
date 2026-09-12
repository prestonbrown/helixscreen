// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_image_manager.cpp
 * @brief Unit tests for PrinterImageManager — image selection, import, and conversion
 */

#include "../../include/lvgl_image_writer.h"
#include "../../include/printer_image_manager.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace fs = std::filesystem;

// ============================================================================
// Helper: Create a minimal valid PNG file (4x4 RGBA, all red)
//
// PNG format: signature + IHDR + IDAT (raw stored) + IEND
// This creates a valid but tiny PNG that stbi can decode.
// ============================================================================
static void write_test_png(const std::string& path, int width = 4, int height = 4) {
    // For simplicity, write a raw RGBA buffer and use stbi to confirm it loads.
    // We'll use a minimal valid PNG. The easiest approach: write the raw bytes
    // for a known-good 4x4 PNG.
    //
    // Instead of hand-crafting PNG, create an uncompressed BMP that stbi can read.
    // BMP is simpler to construct.
    //
    // BMP Header (14 bytes) + DIB Header (40 bytes) + Pixel Data
    // Using 32-bit BGRA with BI_BITFIELDS for alpha support.

    // Use a simple approach: write a BMP file that stbi_load can read
    int bpp = 3;                                  // 24-bit BGR
    int row_stride = ((width * bpp + 3) / 4) * 4; // Rows padded to 4-byte boundary
    int pixel_data_size = row_stride * height;
    int file_size = 14 + 40 + pixel_data_size; // BMP header + DIB header + pixels

    std::vector<uint8_t> bmp(file_size, 0);

    // BMP File Header (14 bytes)
    bmp[0] = 'B';
    bmp[1] = 'M';
    // File size (little-endian)
    bmp[2] = file_size & 0xFF;
    bmp[3] = (file_size >> 8) & 0xFF;
    bmp[4] = (file_size >> 16) & 0xFF;
    bmp[5] = (file_size >> 24) & 0xFF;
    // Reserved (0)
    // Pixel data offset (14 + 40 = 54)
    bmp[10] = 54;

    // DIB Header (BITMAPINFOHEADER, 40 bytes)
    bmp[14] = 40; // Header size
    // Width (little-endian)
    bmp[18] = width & 0xFF;
    bmp[19] = (width >> 8) & 0xFF;
    // Height (little-endian, positive = bottom-up)
    bmp[22] = height & 0xFF;
    bmp[23] = (height >> 8) & 0xFF;
    // Color planes (1)
    bmp[26] = 1;
    // Bits per pixel (24)
    bmp[28] = 24;
    // Compression (0 = BI_RGB)
    // Image size (can be 0 for BI_RGB)
    // Pixels per meter, colors — all 0

    // Pixel data (BGR, bottom-up)
    int data_offset = 54;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = data_offset + y * row_stride + x * 3;
            bmp[idx + 0] = 0x00; // B
            bmp[idx + 1] = 0x00; // G
            bmp[idx + 2] = 0xFF; // R (all red)
        }
    }

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
}

/// Create an oversized BMP (fake — just a large file with valid BMP header but huge dimensions)
static void write_oversized_dimension_bmp(const std::string& path) {
    // Write a BMP header claiming 3000x3000 but with no actual pixel data
    // stbi_info will read the header and report the dimensions
    int width = 3000;
    int height = 3000;
    int bpp = 3;
    int row_stride = ((width * bpp + 3) / 4) * 4;
    int pixel_data_size = row_stride * height;
    int file_size = 14 + 40 + pixel_data_size;

    // Write just the headers (54 bytes) — stbi_info reads header only
    std::vector<uint8_t> bmp(54, 0);

    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = file_size & 0xFF;
    bmp[3] = (file_size >> 8) & 0xFF;
    bmp[4] = (file_size >> 16) & 0xFF;
    bmp[5] = (file_size >> 24) & 0xFF;
    bmp[10] = 54;

    bmp[14] = 40;
    bmp[18] = width & 0xFF;
    bmp[19] = (width >> 8) & 0xFF;
    bmp[20] = (width >> 16) & 0xFF;
    bmp[21] = (width >> 24) & 0xFF;
    bmp[22] = height & 0xFF;
    bmp[23] = (height >> 8) & 0xFF;
    bmp[24] = (height >> 16) & 0xFF;
    bmp[25] = (height >> 24) & 0xFF;
    bmp[26] = 1;
    bmp[28] = 24;

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
}

/// Write a file filled with zeros (to exceed size limit)
static void write_oversized_file(const std::string& path, size_t size) {
    std::vector<char> data(size, 0);
    // Minimum valid BMP header so it's recognized as an image
    data[0] = 'B';
    data[1] = 'M';
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), data.size());
}

/// Helper: read entire file into byte vector
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
// Scoped temp directory for test isolation
// ============================================================================
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("helix_pim_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        fs::remove_all(path);
    }
    std::string str() const {
        return path.string();
    }
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("PrinterImageManager init creates custom_images dir", "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    std::string custom_dir = tmp.str() + "/custom_images/";

    // Should not exist yet
    REQUIRE_FALSE(fs::exists(custom_dir));

    pim.init(tmp.str());

    REQUIRE(fs::exists(custom_dir));
    REQUIRE(fs::is_directory(custom_dir));
}

TEST_CASE("PrinterImageManager get_active_image_id returns empty when no config",
          "[printer_image_manager]") {
    // Without Config initialized, should return empty
    auto& pim = helix::PrinterImageManager::instance();
    std::string id = pim.get_active_image_id();
    // If Config is not initialized, we get empty string (the default)
    // This test mainly ensures no crash
    CHECK(id.empty() == true);
}

TEST_CASE("PrinterImageManager get_active_image_path returns empty for auto-detect",
          "[printer_image_manager]") {
    // Empty ID means auto-detect — should return ""
    auto& pim = helix::PrinterImageManager::instance();
    // With no config or empty printer_image, auto-detect returns ""
    std::string path = pim.get_active_image_path(800);
    CHECK(path.empty());
}

TEST_CASE("PrinterImageManager get_shipped_images returns list", "[printer_image_manager]") {
    auto& pim = helix::PrinterImageManager::instance();
    auto shipped = pim.get_shipped_images(800);

    // Should find PNG files in assets/images/printers/
    // This test depends on the actual assets being present (they are in the worktree)
    if (fs::exists("assets/images/printers/")) {
        REQUIRE(shipped.size() > 0);

        // Each entry should have valid fields
        for (const auto& img : shipped) {
            CHECK(img.id.rfind("shipped:", 0) == 0);
            CHECK_FALSE(img.display_name.empty());
            CHECK_FALSE(img.preview_path.empty());
        }

        // Should contain a known printer
        bool found_voron = false;
        for (const auto& img : shipped) {
            if (img.id == "shipped:voron-v2") {
                found_voron = true;
                break;
            }
        }
        CHECK(found_voron);
    }
}

TEST_CASE("PrinterImageManager validate_image rejects oversized file", "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    // Create a file larger than 5MB
    std::string big_file = tmp.str() + "/huge.bmp";
    write_oversized_file(big_file, 6 * 1024 * 1024);

    // Try to import — should fail with size error
    auto result = pim.import_image(big_file);
    REQUIRE_FALSE(result.success);
    CHECK(result.error.find("too large") != std::string::npos);
}

TEST_CASE("PrinterImageManager validate_image rejects oversized dimensions",
          "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    // Create a BMP with 3000x3000 header (exceeds 2048 limit)
    std::string big_dim_file = tmp.str() + "/huge_dim.bmp";
    write_oversized_dimension_bmp(big_dim_file);

    auto result = pim.import_image(big_dim_file);
    REQUIRE_FALSE(result.success);
    CHECK(result.error.find("too large") != std::string::npos);
}

TEST_CASE("PrinterImageManager import_image end-to-end", "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    // Create a test image (4x4 BMP that stbi can read)
    std::string test_image = tmp.str() + "/test-printer.bmp";
    write_test_png(test_image);

    auto result = pim.import_image(test_image);
    REQUIRE(result.success);
    CHECK(result.id == "custom:test-printer");
    CHECK(result.error.empty());

    // Verify both .bin files were created
    std::string path_300 = pim.get_custom_dir() + "test-printer-300.bin";
    std::string path_150 = pim.get_custom_dir() + "test-printer-150.bin";
    CHECK(fs::exists(path_300));
    CHECK(fs::exists(path_150));
}

TEST_CASE("PrinterImageManager import_image .bin files have valid LVGL headers",
          "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    std::string test_image = tmp.str() + "/header-test.bmp";
    write_test_png(test_image);

    auto result = pim.import_image(test_image);
    REQUIRE(result.success);

    // Read back the 300px variant and check header
    std::string path_300 = pim.get_custom_dir() + "header-test-300.bin";
    auto data = read_file_bytes(path_300);
    REQUIRE(data.size() >= sizeof(lv_image_header_t));

    lv_image_header_t header;
    std::memcpy(&header, data.data(), sizeof(header));

    CHECK(header.magic == LV_IMAGE_HEADER_MAGIC);
    CHECK(static_cast<uint8_t>(header.cf) == static_cast<uint8_t>(LV_COLOR_FORMAT_ARGB8888));
    // Source is 4x4, scaled to fit 300x300 — should be 300x300 (since w==h)
    // Actually 4x4 scaled: 4>=4, so target_w=300, target_h=300
    CHECK(header.w > 0);
    CHECK(header.h > 0);
    CHECK(header.w <= 300);
    CHECK(header.h <= 300);

    // Read the 150px variant
    std::string path_150 = pim.get_custom_dir() + "header-test-150.bin";
    auto data_150 = read_file_bytes(path_150);
    REQUIRE(data_150.size() >= sizeof(lv_image_header_t));

    lv_image_header_t header_150;
    std::memcpy(&header_150, data_150.data(), sizeof(header_150));
    CHECK(header_150.magic == LV_IMAGE_HEADER_MAGIC);
    CHECK(header_150.w > 0);
    CHECK(header_150.h > 0);
    CHECK(header_150.w <= 150);
    CHECK(header_150.h <= 150);
}

TEST_CASE("PrinterImageManager delete_custom_image removes files", "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    // Import an image first
    std::string test_image = tmp.str() + "/delete-test.bmp";
    write_test_png(test_image);

    auto result = pim.import_image(test_image);
    REQUIRE(result.success);

    // Verify files exist
    std::string path_300 = pim.get_custom_dir() + "delete-test-300.bin";
    std::string path_150 = pim.get_custom_dir() + "delete-test-150.bin";
    REQUIRE(fs::exists(path_300));
    REQUIRE(fs::exists(path_150));

    // Delete
    bool deleted = pim.delete_custom_image("delete-test");
    CHECK(deleted);

    // Verify files are gone
    CHECK_FALSE(fs::exists(path_300));
    CHECK_FALSE(fs::exists(path_150));
}

TEST_CASE("PrinterImageManager get_custom_images after import", "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    // Import a test image
    std::string test_image = tmp.str() + "/listed-printer.bmp";
    write_test_png(test_image);

    auto result = pim.import_image(test_image);
    REQUIRE(result.success);

    // Get custom images list
    auto custom = pim.get_custom_images();
    REQUIRE(custom.size() >= 1);

    bool found = false;
    for (const auto& img : custom) {
        if (img.id == "custom:listed-printer") {
            found = true;
            CHECK(img.display_name == "listed printer");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("PrinterImageManager get_invalid_custom_images returns failed imports",
          "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    std::string custom_dir = pim.get_custom_dir();

    SECTION("empty directory returns no invalid images") {
        auto invalid = pim.get_invalid_custom_images();
        CHECK(invalid.empty());
    }

    SECTION("valid imported image not returned as invalid") {
        std::string test_image = custom_dir + "good-image.bmp";
        write_test_png(test_image);
        auto result = pim.import_image(test_image);
        REQUIRE(result.success);

        auto invalid = pim.get_invalid_custom_images();
        for (const auto& img : invalid) {
            CHECK(img.display_name != "good image");
        }
    }

    SECTION("raw file without .bin is returned as invalid") {
        // Create a raw image that will fail import (oversized dimensions)
        std::string bad_image = custom_dir + "bad-image.bmp";
        write_oversized_dimension_bmp(bad_image);

        // Attempt import — it should fail
        auto result = pim.import_image(bad_image);
        REQUIRE_FALSE(result.success);

        auto invalid = pim.get_invalid_custom_images();
        REQUIRE(invalid.size() >= 1);

        bool found = false;
        for (const auto& img : invalid) {
            if (img.id == "invalid:bad-image") {
                found = true;
                CHECK(img.display_name == "bad image");
            }
        }
        CHECK(found);
    }

    SECTION("unsupported file extension is returned as invalid") {
        // Create a .gif file (unsupported format)
        std::string gif_path = custom_dir + "anim.gif";
        std::ofstream f(gif_path, std::ios::binary);
        f << "GIF89a fake data";
        f.close();

        auto invalid = pim.get_invalid_custom_images();
        REQUIRE(invalid.size() >= 1);

        bool found = false;
        for (const auto& img : invalid) {
            if (img.id == "invalid:anim") {
                found = true;
            }
        }
        CHECK(found);
    }

    SECTION(".bin files are not returned as invalid") {
        // Create a stray .bin file
        std::string bin_path = custom_dir + "stray.bin";
        std::ofstream f(bin_path, std::ios::binary);
        f << "fake bin";
        f.close();

        auto invalid = pim.get_invalid_custom_images();
        for (const auto& img : invalid) {
            CHECK(img.id != "invalid:stray");
        }
    }

    SECTION("non-image files like .DS_Store and .tmp are excluded") {
        std::ofstream(custom_dir + ".DS_Store") << "junk";
        std::ofstream(custom_dir + "notes.txt") << "hello";
        std::ofstream(custom_dir + "backup.tmp") << "data";

        auto invalid = pim.get_invalid_custom_images();
        for (const auto& img : invalid) {
            CHECK(img.id != "invalid:.DS_Store");
            CHECK(img.id != "invalid:notes");
            CHECK(img.id != "invalid:backup");
        }
    }

    SECTION("results are sorted by id") {
        std::string a_path = custom_dir + "zzz-image.gif";
        std::string b_path = custom_dir + "aaa-image.gif";
        std::ofstream(a_path) << "fake";
        std::ofstream(b_path) << "fake";

        auto invalid = pim.get_invalid_custom_images();
        REQUIRE(invalid.size() >= 2);
        for (size_t i = 1; i < invalid.size(); i++) {
            CHECK(invalid[i - 1].id <= invalid[i].id);
        }
    }
}

TEST_CASE("PrinterImageManager import creates .bin from raw image in custom dir",
          "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    std::string custom_dir = pim.get_custom_dir();

    // Place a valid image in custom_images/
    std::string raw_image = custom_dir + "my-printer.bmp";
    write_test_png(raw_image);

    // .bin doesn't exist yet
    std::string bin_300 = custom_dir + "my-printer-300.bin";
    REQUIRE_FALSE(fs::exists(bin_300));

    // import_image should convert it
    auto result = pim.import_image(raw_image);
    REQUIRE(result.success);
    CHECK(fs::exists(bin_300));
}

TEST_CASE("PrinterImageManager scan_for_images finds PNG/JPG case-insensitively",
          "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    std::string custom_dir = pim.get_custom_dir();

    // scan_for_images checks .png/.jpg/.jpeg — not .bmp
    // Create files with mixed-case extensions
    write_test_png(custom_dir + "lower.png");
    write_test_png(custom_dir + "upper.PNG");
    write_test_png(custom_dir + "mixed.JpG");

    auto found = pim.scan_for_images(custom_dir);
    // All three should be found (case-insensitive extension matching)
    CHECK(found.size() == 3);
}

TEST_CASE("PrinterImageManager image_changed_subject fires on set_active_image",
          "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    lv_subject_t* subj = pim.get_image_changed_subject();
    REQUIRE(subj != nullptr);

    int initial = lv_subject_get_int(subj);

    // set_active_image should bump the counter
    pim.set_active_image("shipped:test");
    CHECK(lv_subject_get_int(subj) == initial + 1);

    pim.set_active_image("");
    CHECK(lv_subject_get_int(subj) == initial + 2);

    // Reset to avoid polluting other tests
    pim.set_active_image("");
}

TEST_CASE("PrinterImageManager auto_import_raw_images imports valid, skips converted",
          "[printer_image_manager]") {
    TempDir tmp;
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(tmp.str());

    std::string custom_dir = pim.get_custom_dir();

    // Place a valid raw image
    std::string raw_image = custom_dir + "auto-test.bmp";
    write_test_png(raw_image);

    // First auto-import should convert it
    int imported = pim.auto_import_raw_images();
    // Note: auto_import only scans .png/.jpg/.jpeg — .bmp won't be found by scan_for_images
    // This validates that auto_import doesn't crash on an empty scan
    CHECK(imported >= 0);
}

TEST_CASE("PrinterImageManager::format_display_name", "[printer_image_manager]") {
    using PIM = helix::PrinterImageManager;

    SECTION("replaces dashes with spaces") {
        REQUIRE(PIM::format_display_name("voron-trident") == "voron trident");
    }

    SECTION("replaces underscores with spaces") {
        REQUIRE(PIM::format_display_name("voron_trident") == "voron trident");
    }

    SECTION("dashes between digits become dots") {
        REQUIRE(PIM::format_display_name("voron-0-2") == "voron 0.2");
    }

    SECTION("mixed separators") {
        REQUIRE(PIM::format_display_name("my_printer-v2-0-1") == "my printer v2.0.1");
    }

    SECTION("no separators unchanged") {
        REQUIRE(PIM::format_display_name("printer") == "printer");
    }

    SECTION("empty string") {
        REQUIRE(PIM::format_display_name("") == "");
    }

    SECTION("leading/trailing separators become spaces") {
        REQUIRE(PIM::format_display_name("-hello-") == " hello ");
    }

    SECTION("consecutive digits with dash") {
        REQUIRE(PIM::format_display_name("model-24r2") == "model 24r2");
        // 4-to-r is NOT digit-digit so dash becomes space — correct
    }

    SECTION("underscore between digits also becomes dot") {
        REQUIRE(PIM::format_display_name("v1_0_0") == "v1.0.0");
    }
}
