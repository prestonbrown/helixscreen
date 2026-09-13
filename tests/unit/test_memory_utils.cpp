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
//
// The gate prices what a streaming render allocates: the layer index (one
// 40-byte gcode::StreamingLayerEntry per layer, one layer per 500 bytes of
// G-code), the layer cache at this RAM tier's budget, an ARGB8888 ghost buffer
// the size of the display, a 3MB margin, and — only when the G-code cache
// directory is RAM-backed — the downloaded file itself.
//
// Expected totals below are computed by hand rather than from the gate's own
// constants: a test that recomputes the formula agrees with any formula.
// ============================================================================

namespace {

/// 128MB board (Elegoo Centauri Carbon class): constrained tier, 2MB cache.
constexpr size_t CONSTRAINED_TOTAL_KB = 128 * 1024;
/// 384MB board: normal tier, 16MB cache.
constexpr size_t NORMAL_TOTAL_KB = 384 * 1024;
/// 1GB board: good tier, 32MB cache.
constexpr size_t GOOD_TOTAL_KB = 1024 * 1024;

/// Whether the resolved G-code cache directory is tmpfs/ramfs.
constexpr bool ON_FLASH = false;
constexpr bool ON_TMPFS = true;

} // namespace

TEST_CASE("2D streaming safe: small file with plenty of RAM", "[memory][streaming]") {
    // 1MB file, 64MB available, 800x480 display, 1GB board.
    // index 81 + cache 32768 + ghost 1500 + margin 3072 = 37421KB needed.
    size_t file_size = 1 * 1024 * 1024; // 1MB
    size_t available_kb = 64 * 1024;    // 64MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, GOOD_TOTAL_KB, display_width,
                                            display_height, ON_FLASH));
}

TEST_CASE("2D streaming safe: large file with plenty of RAM", "[memory][streaming]") {
    // 50MB file, 128MB available, 800x480 display, 384MB board.
    // index 4095 + cache 16384 + ghost 1500 + margin 3072 = 25051KB needed.
    size_t file_size = 50 * 1024 * 1024; // 50MB
    size_t available_kb = 128 * 1024;    // 128MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, NORMAL_TOTAL_KB, display_width,
                                            display_height, ON_FLASH));
}

TEST_CASE("2D streaming safe: AD5M typical scenario", "[memory][streaming]") {
    // 12.8MB file (real print), 38MB available, 800x480, 47MB board caching to
    // /data (real storage).
    // index 1023 + cache 2048 + ghost 1500 + margin 3072 = 7643KB needed.
    size_t file_size = 12800 * 1024; // 12.8MB
    size_t available_kb = 38 * 1024; // 38MB
    size_t total_kb = 47 * 1024;     // 47MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, total_kb, display_width,
                                            display_height, ON_FLASH));
}

TEST_CASE("2D streaming unsafe: insufficient RAM for requirements", "[memory][streaming]") {
    // 10MB file, only 4MB available, 800x480 display.
    // index 819 + cache 2048 + ghost 1500 + margin 3072 = 7439KB needed.
    size_t file_size = 10 * 1024 * 1024; // 10MB
    size_t available_kb = 4 * 1024;      // 4MB (very constrained)
    int display_width = 800;
    int display_height = 480;

    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_FLASH));
}

TEST_CASE("2D streaming: larger display increases ghost buffer requirement",
          "[memory][streaming]") {
    // Same file, same RAM, but 1920x1080 display.
    // Ghost buffer: 1920 * 1080 * 4 = 8100KB vs 1500KB for 800x480.
    size_t file_size = 5 * 1024 * 1024; // 5MB
    size_t available_kb = 10 * 1024;    // 10MB
    int small_width = 800;
    int small_height = 480;
    int large_width = 1920;
    int large_height = 1080;

    // Small display fits: index 409 + cache 2048 + ghost 1500 + margin 3072 = 7029KB.
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, CONSTRAINED_TOTAL_KB,
                                            small_width, small_height, ON_FLASH));

    // Large display does not: same terms with ghost 8100 = 13629KB.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, CONSTRAINED_TOTAL_KB,
                                                  large_width, large_height, ON_FLASH));
}

TEST_CASE("2D streaming: layer index scales with file size", "[memory][streaming]") {
    // Verify that larger files require more memory due to layer index
    size_t available_kb = 8 * 1024; // 8MB available
    int display_width = 800;
    int display_height = 480;

    // 1MB file: index 81 + cache 2048 + ghost 1500 + margin 3072 = 6701KB.
    size_t small_file = 1 * 1024 * 1024;
    REQUIRE(is_gcode_2d_streaming_safe_impl(small_file, available_kb, CONSTRAINED_TOTAL_KB,
                                            display_width, display_height, ON_FLASH));

    // 100MB file: index 8191 + cache 2048 + ghost 1500 + margin 3072 = 14811KB.
    size_t large_file = 100 * 1024 * 1024;
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(large_file, available_kb, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_FLASH));
}

TEST_CASE("2D streaming: the layer index is priced at the real entry size",
          "[memory][streaming][edge]") {
    // 100MB of G-code is 209715 layers by the 1-per-500-bytes estimate, so the
    // index alone is 209715 * 40 / 1024 = 8191KB. Total on a constrained board:
    // 8191 + cache 2048 + ghost 1500 + margin 3072 = 14811KB.
    size_t file_size = 100 * 1024 * 1024;
    int display_width = 800;
    int display_height = 480;

    // Exactly at the boundary is rejected (the gate uses > not >=).
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 14811, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_FLASH));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 14812, CONSTRAINED_TOTAL_KB, display_width,
                                            display_height, ON_FLASH));

    // A per-entry price below sizeof(StreamingLayerEntry) authorizes this file
    // at 12000KB available; the real index does not fit there.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 12000, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_FLASH));
}

TEST_CASE("2D streaming: the layer cache is priced at this device's budget tier",
          "[memory][streaming][edge]") {
    // Zero-size file: no index, so the cache budget is the only term that moves
    // between tiers. Ghost 1500 + margin 3072 = 4572KB is common to all three.
    size_t file_size = 0;
    int display_width = 800;
    int display_height = 480;

    // Constrained (2MB cache): 4572 + 2048 = 6620KB.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 6620, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_FLASH));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 6621, CONSTRAINED_TOTAL_KB, display_width,
                                            display_height, ON_FLASH));

    // Normal (16MB cache): 4572 + 16384 = 20956KB.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 20956, NORMAL_TOTAL_KB, display_width,
                                                  display_height, ON_FLASH));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 20957, NORMAL_TOTAL_KB, display_width,
                                            display_height, ON_FLASH));

    // Good (32MB cache): 4572 + 32768 = 37340KB.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 37340, GOOD_TOTAL_KB, display_width,
                                                  display_height, ON_FLASH));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 37341, GOOD_TOTAL_KB, display_width,
                                            display_height, ON_FLASH));
}

TEST_CASE("2D streaming: a RAM-backed cache directory charges the whole file",
          "[memory][streaming][edge]") {
    // 10MB file on a constrained board, 800x480:
    // index 819 + cache 2048 + ghost 1500 + margin 3072 = 7439KB on storage.
    // On tmpfs the 10240KB file is memory too: 17679KB.
    size_t file_size = 10 * 1024 * 1024;
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 7440, CONSTRAINED_TOTAL_KB, display_width,
                                            display_height, ON_FLASH));
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 7440, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_TMPFS));

    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 17679, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_TMPFS));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 17680, CONSTRAINED_TOTAL_KB, display_width,
                                            display_height, ON_TMPFS));
}

TEST_CASE("2D streaming: 6MB file on a 128MB board with a tmpfs cache directory",
          "[memory][streaming][edge]") {
    // 480x272 panel, 11976KB available, cache directory on tmpfs:
    // index 487 + cache 2048 + ghost 510 + margin 3072 + spill 6095 = 12212KB,
    // which does not fit. The same file cached on real storage costs 6117KB and
    // streams fine — flash devices must keep the preview.
    size_t file_size = 6241553;
    size_t available_kb = 11976;
    int display_width = 480;
    int display_height = 272;

    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_TMPFS));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, CONSTRAINED_TOTAL_KB,
                                            display_width, display_height, ON_FLASH));

    // Boundaries either way, so every term is pinned.
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 12212, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_TMPFS));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 12213, CONSTRAINED_TOTAL_KB, display_width,
                                            display_height, ON_TMPFS));
    REQUIRE_FALSE(is_gcode_2d_streaming_safe_impl(file_size, 6117, CONSTRAINED_TOTAL_KB,
                                                  display_width, display_height, ON_FLASH));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, 6118, CONSTRAINED_TOTAL_KB, display_width,
                                            display_height, ON_FLASH));
}

TEST_CASE("2D streaming: zero file size", "[memory][streaming][edge]") {
    // No layer index and no spill for an empty file: cache 2048 + ghost 1500 +
    // margin 3072 = 6620KB, tmpfs or not.
    size_t file_size = 0;
    size_t available_kb = 7 * 1024; // 7MB
    int display_width = 800;
    int display_height = 480;

    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, CONSTRAINED_TOTAL_KB,
                                            display_width, display_height, ON_FLASH));
    REQUIRE(is_gcode_2d_streaming_safe_impl(file_size, available_kb, CONSTRAINED_TOTAL_KB,
                                            display_width, display_height, ON_TMPFS));
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
