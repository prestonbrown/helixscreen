// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thumbnail_cache.cpp
 * @brief Unit tests for ThumbnailCache directory selection and caching logic
 *
 * Tests the cache directory determination, path generation, and write verification.
 */

#include "../../include/thumbnail_cache.h"
#include "app_globals.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

// ============================================================================
// ThumbnailCache Directory Tests
// ============================================================================

TEST_CASE("ThumbnailCache cache directory initialization", "[assets][cache]") {
    // Get the singleton - this will initialize with the determined directory
    ThumbnailCache& cache = get_thumbnail_cache();

    SECTION("Cache directory is set and non-empty") {
        std::string cache_dir = cache.get_cache_dir();
        REQUIRE(!cache_dir.empty());
        INFO("Cache directory: " << cache_dir);
    }

    SECTION("Cache directory exists") {
        std::string cache_dir = cache.get_cache_dir();
        REQUIRE(std::filesystem::exists(cache_dir));
    }

    SECTION("Cache directory is writable") {
        std::string cache_dir = cache.get_cache_dir();
        std::string test_file = cache_dir + "/.write_test_" + std::to_string(rand());

        // Should be able to create a file
        std::ofstream ofs(test_file);
        REQUIRE(ofs.good());
        ofs << "test";
        ofs.close();

        // Cleanup
        std::filesystem::remove(test_file);
    }

    SECTION("Cache directory contains 'helix' in path") {
        // All cache paths should include 'helix' somewhere
        std::string cache_dir = cache.get_cache_dir();
        REQUIRE(cache_dir.find("helix") != std::string::npos);
    }
}

TEST_CASE("ThumbnailCache path generation", "[assets][cache]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    SECTION("get_cache_path returns path in cache directory") {
        std::string path = cache.get_cache_path("test/image.png");
        std::string cache_dir = cache.get_cache_dir();

        REQUIRE(path.find(cache_dir) == 0); // Should start with cache_dir
    }

    SECTION("get_cache_path generates .png extension") {
        std::string path = cache.get_cache_path("test/image.png");
        REQUIRE(path.substr(path.length() - 4) == ".png");
    }

    SECTION("Different paths generate different cache paths") {
        std::string path1 = cache.get_cache_path("file1.png");
        std::string path2 = cache.get_cache_path("file2.png");
        REQUIRE(path1 != path2);
    }

    SECTION("Same path generates same cache path") {
        std::string path1 = cache.get_cache_path("test/file.png");
        std::string path2 = cache.get_cache_path("test/file.png");
        REQUIRE(path1 == path2);
    }
}

TEST_CASE("ThumbnailCache LVGL path helpers", "[assets][cache]") {
    SECTION("is_lvgl_path detects A: prefix") {
        REQUIRE(ThumbnailCache::is_lvgl_path("A:/path/to/file.png"));
        REQUIRE(ThumbnailCache::is_lvgl_path("A:relative/path.bin"));
    }

    SECTION("is_lvgl_path rejects non-LVGL paths") {
        REQUIRE_FALSE(ThumbnailCache::is_lvgl_path("/path/to/file.png"));
        REQUIRE_FALSE(ThumbnailCache::is_lvgl_path("relative/path.png"));
        REQUIRE_FALSE(ThumbnailCache::is_lvgl_path("B:/wrong/prefix.png"));
        REQUIRE_FALSE(ThumbnailCache::is_lvgl_path(""));
        REQUIRE_FALSE(ThumbnailCache::is_lvgl_path("A")); // Too short
    }

    SECTION("to_lvgl_path adds A: prefix") {
        REQUIRE(ThumbnailCache::to_lvgl_path("/path/to/file.png") == "A:/path/to/file.png");
    }

    SECTION("to_lvgl_path doesn't double-prefix") {
        std::string already_lvgl = "A:/path/to/file.png";
        REQUIRE(ThumbnailCache::to_lvgl_path(already_lvgl) == already_lvgl);
    }
}

TEST_CASE("ThumbnailCache disk pressure monitoring", "[assets][cache]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    SECTION("get_available_disk_space returns non-zero on normal systems") {
        size_t space = cache.get_available_disk_space();
        // Should have at least some space available
        REQUIRE(space > 0);
    }

    SECTION("get_disk_pressure returns valid pressure level") {
        auto pressure = cache.get_disk_pressure();
        // Should be one of the valid enum values
        REQUIRE((pressure == ThumbnailCache::DiskPressure::Normal ||
                 pressure == ThumbnailCache::DiskPressure::Low ||
                 pressure == ThumbnailCache::DiskPressure::Critical));
    }

    SECTION("is_caching_allowed returns true on normal systems") {
        // On a development machine, we should have plenty of disk space
        REQUIRE(cache.is_caching_allowed());
    }
}

TEST_CASE("ThumbnailCache size management", "[assets][cache]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    SECTION("get_max_size returns positive value") {
        REQUIRE(cache.get_max_size() >= ThumbnailCache::MIN_CACHE_SIZE);
    }

    SECTION("set_max_size updates the limit") {
        size_t original = cache.get_max_size();
        size_t new_size = 10 * 1024 * 1024; // 10 MB

        cache.set_max_size(new_size);
        REQUIRE(cache.get_max_size() == new_size);

        // Restore original
        cache.set_max_size(original);
    }

    SECTION("get_cache_size stays within the configured maximum") {
        // Was `size >= 0`, which is always true for size_t - it documented an
        // intent the compiler could not check and the test could not fail on.
        // The bound is the invariant actually worth pinning.
        REQUIRE(cache.get_cache_size() <= cache.get_max_size());
    }
}

// ============================================================================
// ThumbnailCache Thread Safety Tests
// ============================================================================

TEST_CASE("ThumbnailCache get_cache_dir is thread-safe", "[assets][cache][thread]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    SECTION("Multiple calls return consistent result") {
        std::string dir1 = cache.get_cache_dir();
        std::string dir2 = cache.get_cache_dir();
        std::string dir3 = cache.get_cache_dir();

        REQUIRE(dir1 == dir2);
        REQUIRE(dir2 == dir3);
    }
}

// ============================================================================
// Cache Path Edge Cases
// ============================================================================

TEST_CASE("ThumbnailCache path edge cases", "[assets][cache]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    SECTION("Paths with spaces are handled") {
        std::string path = cache.get_cache_path("My Model/thumb with spaces.png");
        REQUIRE(!path.empty());
        REQUIRE(std::filesystem::path(path).filename().extension() == ".png");
    }

    SECTION("Paths with unicode are handled") {
        std::string path = cache.get_cache_path("模型/缩略图.png");
        REQUIRE(!path.empty());
    }

    SECTION("Very long paths are handled") {
        std::string long_name(200, 'a');
        std::string path = cache.get_cache_path(long_name + ".png");
        REQUIRE(!path.empty());
    }
}

// ============================================================================
// Cache Age Validation Tests
// ============================================================================

TEST_CASE("ThumbnailCache age validation", "[assets][cache][invalidation]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    // Create a unique test file to avoid conflicts
    std::string test_path = "test_age_validation_" + std::to_string(rand()) + ".png";
    std::string cache_path = cache.get_cache_path(test_path);

    // Create a cached file
    {
        std::ofstream ofs(cache_path, std::ios::binary);
        REQUIRE(ofs.good());
        // Write minimal valid PNG header
        const unsigned char png_header[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        ofs.write(reinterpret_cast<const char*>(png_header), sizeof(png_header));
        ofs.close();
    }

    SECTION("get_if_cached without source_modified returns cached file") {
        std::string result = cache.get_if_cached(test_path);
        REQUIRE(!result.empty());
        REQUIRE(ThumbnailCache::is_lvgl_path(result));
    }

    SECTION("get_if_cached with source_modified=0 skips validation") {
        std::string result = cache.get_if_cached(test_path, 0);
        REQUIRE(!result.empty());
    }

    SECTION("get_if_cached with old source_modified returns cached file") {
        // Source file is older than cache - cache is valid
        time_t old_time = std::time(nullptr) - 3600; // 1 hour ago
        std::string result = cache.get_if_cached(test_path, old_time);
        REQUIRE(!result.empty());
    }

    SECTION("get_if_cached with future source_modified invalidates and returns empty") {
        // Source file is newer than cache - cache is stale
        time_t future_time = std::time(nullptr) + 3600; // 1 hour in future
        std::string result = cache.get_if_cached(test_path, future_time);
        REQUIRE(result.empty());

        // File should be removed
        REQUIRE_FALSE(std::filesystem::exists(cache_path));
    }

    // Cleanup
    if (std::filesystem::exists(cache_path)) {
        std::filesystem::remove(cache_path);
    }
}

// ============================================================================
// save_raw_png Tests (for USB thumbnail extraction fallback)
// ============================================================================

TEST_CASE("ThumbnailCache save_raw_png saves valid PNG data", "[assets][cache][save_raw_png]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    // Valid minimal 1x1 PNG (decoded from base64)
    // This is a real PNG with proper magic bytes and structure
    std::vector<uint8_t> valid_png = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,       // PNG signature
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,       // IHDR chunk
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,       // 1x1 pixels
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, // RGB, etc
        0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54,       // IDAT chunk
        0x08, 0xD7, 0x63, 0xF8, 0xFF, 0xFF, 0x3F, 0x00,       // Compressed data
        0x05, 0xFE, 0x02, 0xFE, 0xA3, 0x56, 0x4A, 0x25,       // CRC
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,       // IEND chunk
        0xAE, 0x42, 0x60, 0x82                                // IEND CRC
    };

    std::string source_id = "test_save_raw_png_" + std::to_string(rand());

    SECTION("Returns LVGL path for valid PNG data") {
        std::string result = cache.save_raw_png(source_id, valid_png);

        REQUIRE(!result.empty());
        REQUIRE(ThumbnailCache::is_lvgl_path(result));

        // Cleanup
        cache.invalidate(source_id);
    }

    SECTION("Saved file exists and contains correct data") {
        std::string result = cache.save_raw_png(source_id, valid_png);
        REQUIRE(!result.empty());

        // Strip A: prefix and check file exists
        std::string local_path = result.substr(2);
        REQUIRE(std::filesystem::exists(local_path));

        // Verify file size matches
        REQUIRE(std::filesystem::file_size(local_path) == valid_png.size());

        // Read back and verify content
        std::ifstream ifs(local_path, std::ios::binary);
        std::vector<uint8_t> read_back(valid_png.size());
        ifs.read(reinterpret_cast<char*>(read_back.data()), valid_png.size());
        ifs.close();

        REQUIRE(read_back == valid_png);

        // Cleanup
        cache.invalidate(source_id);
    }

    SECTION("Different source_ids create different cache files") {
        std::string id1 = "test_save_raw_1_" + std::to_string(rand());
        std::string id2 = "test_save_raw_2_" + std::to_string(rand());

        std::string path1 = cache.save_raw_png(id1, valid_png);
        std::string path2 = cache.save_raw_png(id2, valid_png);

        REQUIRE(!path1.empty());
        REQUIRE(!path2.empty());
        REQUIRE(path1 != path2);

        // Cleanup
        cache.invalidate(id1);
        cache.invalidate(id2);
    }
}

TEST_CASE("ThumbnailCache save_raw_png validates PNG data", "[assets][cache][save_raw_png]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    SECTION("Rejects empty data") {
        std::vector<uint8_t> empty_data;
        std::string result = cache.save_raw_png("test_empty", empty_data);
        REQUIRE(result.empty());
    }

    SECTION("Rejects data smaller than PNG header") {
        std::vector<uint8_t> too_small = {0x89, 'P', 'N', 'G'}; // Only 4 bytes
        std::string result = cache.save_raw_png("test_small", too_small);
        REQUIRE(result.empty());
    }

    SECTION("Rejects invalid PNG magic bytes") {
        std::vector<uint8_t> invalid_magic = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        std::string result = cache.save_raw_png("test_invalid_magic", invalid_magic);
        REQUIRE(result.empty());
    }

    SECTION("Rejects JPEG data (wrong magic)") {
        std::vector<uint8_t> jpeg_data = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46};
        std::string result = cache.save_raw_png("test_jpeg", jpeg_data);
        REQUIRE(result.empty());
    }

    SECTION("Rejects empty source identifier") {
        std::vector<uint8_t> valid_png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::string result = cache.save_raw_png("", valid_png);
        REQUIRE(result.empty());
    }
}

TEST_CASE("ThumbnailCache save_raw_png integrates with cache eviction",
          "[assets][cache][save_raw_png]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    // Valid minimal PNG
    std::vector<uint8_t> valid_png = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
        0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08,
        0xD7, 0x63, 0xF8, 0xFF, 0xFF, 0x3F, 0x00, 0x05, 0xFE, 0x02, 0xFE, 0xA3, 0x56, 0x4A,
        0x25, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

    SECTION("Saved file can be found via get_if_cached") {
        std::string source_id = "test_cache_integration_" + std::to_string(rand());

        // Save the PNG
        std::string saved_path = cache.save_raw_png(source_id, valid_png);
        REQUIRE(!saved_path.empty());

        // Should now be found via get_if_cached
        std::string cached_path = cache.get_if_cached(source_id);
        REQUIRE(!cached_path.empty());
        REQUIRE(cached_path == saved_path);

        // Cleanup
        cache.invalidate(source_id);
    }

    SECTION("Saved file can be invalidated") {
        std::string source_id = "test_invalidate_" + std::to_string(rand());

        std::string saved_path = cache.save_raw_png(source_id, valid_png);
        REQUIRE(!saved_path.empty());

        // File exists
        std::string local_path = saved_path.substr(2);
        REQUIRE(std::filesystem::exists(local_path));

        // Invalidate
        size_t removed = cache.invalidate(source_id);
        REQUIRE(removed >= 1);

        // File should be gone
        REQUIRE_FALSE(std::filesystem::exists(local_path));
    }
}

TEST_CASE("ThumbnailCache invalidation removes all variants", "[assets][cache][invalidation]") {
    ThumbnailCache& cache = get_thumbnail_cache();

    // Create a unique test path
    std::string test_path = "test_invalidate_variants_" + std::to_string(rand()) + ".png";
    std::string cache_path = cache.get_cache_path(test_path);
    std::string cache_dir = cache.get_cache_dir();

    // Extract hash from the cache path
    std::filesystem::path p(cache_path);
    std::string hash_name = p.stem().string(); // e.g., "abc123"

    // Create the main PNG and some .bin variants
    std::vector<std::string> created_files;
    {
        // Main PNG
        std::ofstream ofs(cache_path, std::ios::binary);
        ofs << "test";
        ofs.close();
        created_files.push_back(cache_path);

        // .bin variants (like the optimized thumbnails)
        for (const auto& suffix : {"_120x120_RGB565.bin", "_160x160_RGB565.bin"}) {
            std::string bin_path = cache_dir + "/" + hash_name + suffix;
            std::ofstream bin_ofs(bin_path, std::ios::binary);
            bin_ofs << "test";
            bin_ofs.close();
            created_files.push_back(bin_path);
        }
    }

    SECTION("invalidate removes PNG and all .bin variants") {
        // Verify files exist
        for (const auto& file : created_files) {
            REQUIRE(std::filesystem::exists(file));
        }

        // Invalidate
        size_t removed = cache.invalidate(test_path);
        REQUIRE(removed >= 1); // At least the PNG

        // Verify PNG is gone
        REQUIRE_FALSE(std::filesystem::exists(cache_path));

        // Verify .bin variants are also gone
        for (size_t i = 1; i < created_files.size(); ++i) {
            REQUIRE_FALSE(std::filesystem::exists(created_files[i]));
        }
    }

    // Cleanup any remaining files
    for (const auto& file : created_files) {
        if (std::filesystem::exists(file)) {
            std::filesystem::remove(file);
        }
    }
}

// ============================================================================
// ThumbnailCache + HELIX_CACHE_DIR Integration
// ============================================================================

TEST_CASE("ThumbnailCache respects HELIX_CACHE_DIR via get_helix_cache_dir", "[assets][cache]") {
    // Save and restore env var
    const char* orig = std::getenv("HELIX_CACHE_DIR");
    std::string orig_val = orig ? orig : "";
    bool had_orig = (orig != nullptr);

    std::string tmpdir = std::string("/tmp/helix_test_thumb_env_") +
                         std::to_string(static_cast<unsigned long>(time(nullptr)));
    std::filesystem::create_directories(tmpdir);

    setenv("HELIX_CACHE_DIR", tmpdir.c_str(), 1);

    // get_helix_cache_dir should resolve under our tmpdir
    std::string result = get_helix_cache_dir("helix_thumbs");
    REQUIRE(result.find(tmpdir) == 0);
    REQUIRE(result.find("helix_thumbs") != std::string::npos);

    // Restore env
    if (had_orig) {
        setenv("HELIX_CACHE_DIR", orig_val.c_str(), 1);
    } else {
        unsetenv("HELIX_CACHE_DIR");
    }

    std::error_code ec;
    std::filesystem::remove_all(tmpdir, ec);
}
