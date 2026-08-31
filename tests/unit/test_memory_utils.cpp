// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_memory_utils.cpp
 * @brief Unit tests for memory utility functions
 *
 * Tests the memory checking functions used to determine if G-code
 * rendering is safe given current system memory and file sizes.
 */

#include "memory_utils.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// is_gcode_2d_streaming_safe_impl Tests
// ============================================================================

TEST_CASE("2D streaming safe: small file with plenty of RAM", "[memory][streaming]") {
    // 1MB file, 64MB available, 800x480 display
    // Expected: layer_index ~48KB, cache 1MB, ghost ~1.5MB, margin 3MB = ~5.5MB needed
    size_t file_size = 1 * 1024 * 1024; // 1MB
    size_t available_kb = 64 * 1024;    // 64MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(
        is_gcode_2d_streaming_safe_impl(file_size, available_kb, display_width, display_height));
}

TEST_CASE("2D streaming safe: large file with plenty of RAM", "[memory][streaming]") {
    // 50MB file, 128MB available, 800x480 display
    // Expected: layer_index ~2.4MB, cache 1MB, ghost ~1.5MB, margin 3MB = ~8MB needed
    size_t file_size = 50 * 1024 * 1024; // 50MB
    size_t available_kb = 128 * 1024;    // 128MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(
        is_gcode_2d_streaming_safe_impl(file_size, available_kb, display_width, display_height));
}

TEST_CASE("2D streaming safe: AD5M typical scenario", "[memory][streaming]") {
    // 12.8MB file (real print), 38MB available, 800x480 display
    // Expected: layer_index ~614KB, cache 1MB, ghost ~1.5MB, margin 3MB = ~6.2MB needed
    // 38MB available > 6.2MB needed -> should pass
    size_t file_size = 12800 * 1024; // 12.8MB
    size_t available_kb = 38 * 1024; // 38MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(
        is_gcode_2d_streaming_safe_impl(file_size, available_kb, display_width, display_height));
}

TEST_CASE("2D streaming unsafe: insufficient RAM for requirements", "[memory][streaming]") {
    // 10MB file, only 4MB available, 800x480 display
    // Expected: layer_index ~480KB, cache 1MB, ghost ~1.5MB, margin 3MB = ~6MB needed
    // 4MB available < 6MB needed -> should fail
    size_t file_size = 10 * 1024 * 1024; // 10MB
    size_t available_kb = 4 * 1024;      // 4MB (very constrained)
    int display_width = 800;
    int display_height = 480;

    REQUIRE_FALSE(
        is_gcode_2d_streaming_safe_impl(file_size, available_kb, display_width, display_height));
}

TEST_CASE("2D streaming: larger display increases ghost buffer requirement",
          "[memory][streaming]") {
    // Same file, same RAM, but 1920x1080 display
    // Ghost buffer: 1920 * 1080 * 4 = ~8MB vs ~1.5MB for 800x480
    size_t file_size = 5 * 1024 * 1024; // 5MB
    size_t available_kb = 10 * 1024;    // 10MB
    int small_width = 800;
    int small_height = 480;
    int large_width = 1920;
    int large_height = 1080;

    // Small display should fit (ghost ~1.5MB, total ~6MB needed)
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, small_width, small_height));

    // Large display should NOT fit (ghost ~8MB alone exceeds available)
    REQUIRE_FALSE(
        is_gcode_2d_streaming_safe_impl(file_size, available_kb, large_width, large_height));
}

TEST_CASE("2D streaming: layer index scales with file size", "[memory][streaming]") {
    // Verify that larger files require more memory due to layer index
    size_t available_kb = 8 * 1024; // 8MB available
    int display_width = 800;
    int display_height = 480;

    // 1MB file: layer_index ~48KB, cache 1MB, ghost ~1.5MB, margin 3MB = ~5.5MB
    size_t small_file = 1 * 1024 * 1024;
    REQUIRE(
        is_gcode_2d_streaming_safe_impl(small_file, available_kb, display_width, display_height));

    // 100MB file: layer_index ~4.8MB, cache 1MB, ghost ~1.5MB, margin 3MB = ~10MB
    // Should fail with only 8MB available
    size_t large_file = 100 * 1024 * 1024;
    REQUIRE_FALSE(
        is_gcode_2d_streaming_safe_impl(large_file, available_kb, display_width, display_height));
}

TEST_CASE("2D streaming: exact boundary calculation", "[memory][streaming][edge]") {
    // Calculate exact memory needed and verify boundary behavior
    // Formula: (file_size / 500 * 24) / 1024 + 1024 + (w * h * 4) / 1024 + 3072

    size_t file_size = 10 * 1024 * 1024; // 10MB
    int display_width = 800;
    int display_height = 480;

    // Calculate expected requirement
    size_t estimated_layers = file_size / 500;
    size_t layer_index_kb = (estimated_layers * 24) / 1024;
    size_t lru_cache_kb = 1024;
    size_t ghost_buffer_kb = (800 * 480 * 4) / 1024;
    size_t safety_margin_kb = 3 * 1024;
    size_t total_needed_kb = layer_index_kb + lru_cache_kb + ghost_buffer_kb + safety_margin_kb;

    // Exactly at boundary should fail (we use > not >=)
    REQUIRE_FALSE(
        is_gcode_2d_streaming_safe_impl(file_size, total_needed_kb, display_width, display_height));

    // 1KB more should pass
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, total_needed_kb + 1, display_width,
                                            display_height));
}

TEST_CASE("2D streaming: zero file size", "[memory][streaming][edge]") {
    // Zero-size file needs: cache (1MB) + ghost (~1.5MB) + margin (3MB) = ~5.5MB
    // Layer index is 0 for zero-size file
    size_t file_size = 0;
    size_t available_kb = 6 * 1024; // 6MB - enough for cache + ghost + margin
    int display_width = 800;
    int display_height = 480;

    REQUIRE(
        is_gcode_2d_streaming_safe_impl(file_size, available_kb, display_width, display_height));
}

// ============================================================================
// MemoryInfo Tests
// ============================================================================

TEST_CASE("MemoryInfo: is_constrained threshold", "[memory]") {
    MemoryInfo info;

    // Below 64MB available = low memory
    info.available_kb = 63 * 1024;
    REQUIRE(info.is_low_memory());

    // At 64MB available = not low memory
    info.available_kb = 64 * 1024;
    REQUIRE_FALSE(info.is_low_memory());

    // Above 64MB available = not low memory
    info.available_kb = 128 * 1024;
    REQUIRE_FALSE(info.is_low_memory());
}

TEST_CASE("MemoryInfo: available_mb conversion", "[memory]") {
    MemoryInfo info;

    info.available_kb = 64 * 1024; // 64MB
    REQUIRE(info.available_mb() == 64);

    info.available_kb = 38 * 1024; // 38MB (AD5M typical)
    REQUIRE(info.available_mb() == 38);

    info.available_kb = 1024; // 1MB
    REQUIRE(info.available_mb() == 1);
}

// ============================================================================
// oom_score_adj Tests
//
// helix-launcher.sh exports HELIX_OOM_SCORE_ADJ only when Klipper is co-hosted;
// helix-screen applies it to itself so that under memory pressure the kernel
// kills the UI (which helix-watchdog restarts) instead of Klipper (which
// cannot be restarted mid-print).
// ============================================================================

TEST_CASE("parse_oom_score_adj: absent or empty value is not applicable", "[memory][oom]") {
    int adj = -12345;
    REQUIRE_FALSE(parse_oom_score_adj(nullptr, adj));
    REQUIRE_FALSE(parse_oom_score_adj("", adj));
    REQUIRE(adj == -12345); // untouched
}

TEST_CASE("parse_oom_score_adj: zero is the documented disable spelling", "[memory][oom]") {
    int adj = -12345;
    REQUIRE_FALSE(parse_oom_score_adj("0", adj));
    REQUIRE(adj == -12345);
}

TEST_CASE("parse_oom_score_adj: accepts the launcher default", "[memory][oom]") {
    int adj = 0;
    REQUIRE(parse_oom_score_adj("300", adj));
    REQUIRE(adj == 300);
}

TEST_CASE("parse_oom_score_adj: accepts a negative adjustment", "[memory][oom]") {
    int adj = 0;
    REQUIRE(parse_oom_score_adj("-500", adj));
    REQUIRE(adj == -500);
}

TEST_CASE("parse_oom_score_adj: clamps to the kernel range", "[memory][oom]") {
    int adj = 0;
    REQUIRE(parse_oom_score_adj("2000", adj));
    REQUIRE(adj == 1000);

    REQUIRE(parse_oom_score_adj("-2000", adj));
    REQUIRE(adj == -1000);
}

TEST_CASE("parse_oom_score_adj: rejects non-numeric and trailing garbage", "[memory][oom]") {
    int adj = -12345;
    REQUIRE_FALSE(parse_oom_score_adj("abc", adj));
    REQUIRE_FALSE(parse_oom_score_adj("300x", adj));
    REQUIRE_FALSE(parse_oom_score_adj("+", adj));
    REQUIRE(adj == -12345);
}

TEST_CASE("write_oom_score_adj: writes the value verbatim", "[memory][oom]") {
    auto path = std::filesystem::temp_directory_path() /
                ("helix_oom_score_adj_" + std::to_string(::getpid()));

    REQUIRE(write_oom_score_adj(300, path.string().c_str()));

    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string contents;
    std::getline(in, contents);
    in.close();
    std::filesystem::remove(path);

    REQUIRE(contents == "300");
}

TEST_CASE("write_oom_score_adj: reports failure on an unwritable path", "[memory][oom]") {
    // A path under a file (not a directory) can never be opened for writing.
    auto base =
        std::filesystem::temp_directory_path() / ("helix_oom_notdir_" + std::to_string(::getpid()));
    std::ofstream(base) << "x";

    auto bogus = base / "oom_score_adj";
    REQUIRE_FALSE(write_oom_score_adj(300, bogus.string().c_str()));

    std::filesystem::remove(base);
}
