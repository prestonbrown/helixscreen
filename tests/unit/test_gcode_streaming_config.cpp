// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_streaming_config.cpp
 * @brief Unit tests for G-code streaming configuration and low-RAM force-streaming
 */

#include "gcode_streaming_config.h"
#include "memory_utils.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Helper to create MemoryInfo with specific total RAM (in KB)
static MemoryInfo make_mem(size_t total_kb, size_t available_kb = 0) {
    MemoryInfo mem;
    mem.total_kb = total_kb;
    mem.available_kb = available_kb;
    mem.free_kb = available_kb;
    return mem;
}

static constexpr size_t KB = 1024;
static constexpr size_t MB = 1024 * 1024;
static constexpr size_t GB_KB = 1024ULL * 1024; // 1GB in KB

// ============================================================================
// MemoryInfo::should_force_streaming() tests
// ============================================================================

TEST_CASE("should_force_streaming returns true for 1GB device", "[gcode]") {
    auto mem = make_mem(1 * GB_KB);
    REQUIRE(mem.should_force_streaming());
}

TEST_CASE("should_force_streaming returns true for 2GB device", "[gcode]") {
    auto mem = make_mem(2 * GB_KB);
    REQUIRE(mem.should_force_streaming());
}

TEST_CASE("should_force_streaming returns false for 4GB device", "[gcode]") {
    auto mem = make_mem(4 * GB_KB);
    REQUIRE_FALSE(mem.should_force_streaming());
}

TEST_CASE("should_force_streaming returns false for just above 2GB", "[gcode]") {
    auto mem = make_mem(2 * GB_KB + 1);
    REQUIRE_FALSE(mem.should_force_streaming());
}

TEST_CASE("should_force_streaming returns false for 8GB device", "[gcode]") {
    auto mem = make_mem(8 * GB_KB);
    REQUIRE_FALSE(mem.should_force_streaming());
}

TEST_CASE("should_force_streaming returns false for 16GB device", "[gcode]") {
    auto mem = make_mem(16 * GB_KB);
    REQUIRE_FALSE(mem.should_force_streaming());
}

TEST_CASE("should_force_streaming returns false when total_kb is 0 (unknown)", "[gcode]") {
    auto mem = make_mem(0);
    REQUIRE_FALSE(mem.should_force_streaming());
}

// ============================================================================
// should_use_gcode_streaming(file_size, mem) testable overload
// ============================================================================

TEST_CASE("Testable overload returns true for small file on 2GB device", "[gcode]") {
    // Even a tiny file should stream on a low-RAM device
    auto mem = make_mem(2 * GB_KB, 512 * KB); // 2GB total, 512MB available
    size_t small_file = 100 * KB;             // 100KB file
    REQUIRE(should_use_gcode_streaming(small_file, mem));
}

TEST_CASE("Testable overload uses threshold logic for 8GB device", "[gcode]") {
    // 8GB device with 4GB available, default 40% threshold:
    // threshold = (4GB * 0.40) / 15 expansion = ~109MB
    // A 1MB file should NOT trigger streaming
    auto mem = make_mem(8 * GB_KB, 4 * GB_KB); // 8GB total, 4GB available
    size_t small_file = 1 * MB;                // 1MB file
    REQUIRE_FALSE(should_use_gcode_streaming(small_file, mem));
}

TEST_CASE("Testable overload streams large file on 8GB device", "[gcode]") {
    // 8GB device with 4GB available, default 40% threshold:
    // threshold = (4GB * 0.40) / 15 = ~109MB
    // A 200MB file SHOULD trigger streaming
    auto mem = make_mem(8 * GB_KB, 4 * GB_KB); // 8GB total, 4GB available
    size_t large_file = 200 * MB;              // 200MB file
    REQUIRE(should_use_gcode_streaming(large_file, mem));
}

TEST_CASE("Testable overload falls back for unknown available memory on 8GB device", "[gcode]") {
    // 8GB total but available_kb=0 (unknown) - should fall back to 2MB heuristic
    auto mem = make_mem(8 * GB_KB, 0);
    size_t small_file = 1 * MB; // 1MB < 2MB threshold
    REQUIRE_FALSE(should_use_gcode_streaming(small_file, mem));

    size_t large_file = 3 * MB; // 3MB > 2MB threshold
    REQUIRE(should_use_gcode_streaming(large_file, mem));
}

// ============================================================================
// A screen's streaming opt-out only counts when 3D exists to fall back on
// ============================================================================

/**
 * PrintSelectDetailView calls ui_gcode_viewer_disable_streaming() so a
 * 3D-preferred screen gets the full-load path. That opt-out was unconditional,
 * and on a build with no 3D renderer it forced the same full-load path with
 * nothing to render into and no budget of its own.
 *
 * Measured on a K2 Plus (488 MB, ENABLE_GLES_3D=no): the detail view opened a
 * 130 MB gcode logging "streaming mode: OFF", helix-screen reached 387 MB RSS
 * and the kernel OOM-killed it. The identical file on the identical device
 * logged "streaming mode: ON" from another screen minutes earlier and rendered.
 *
 * These run in a test binary that IS compiled with 3D enabled, which is exactly
 * why the decision is a pure function rather than an #ifdef at the call site -
 * an #ifdef would compile the interesting branch out of every test.
 */
TEST_CASE("gcode_viewer_should_stream: opt-out is ignored without a 3D renderer",
          "[gcode][streaming]") {
    // The K2 case: screen opted out, no 3D, file big enough to want streaming.
    CHECK(helix::gcode_viewer_should_stream(/*screen_opted_out=*/true, /*build_has_3d=*/false,
                                            /*streaming_for_size=*/true));
}

TEST_CASE("gcode_viewer_should_stream: opt-out is honoured when 3D is available",
          "[gcode][streaming]") {
    // The original intent, preserved: a 3D-capable screen still gets full-load.
    CHECK_FALSE(helix::gcode_viewer_should_stream(true, true, true));
}

TEST_CASE("gcode_viewer_should_stream: no opt-out defers to the size decision",
          "[gcode][streaming]") {
    CHECK(helix::gcode_viewer_should_stream(false, true, true));
    CHECK(helix::gcode_viewer_should_stream(false, false, true));
    CHECK_FALSE(helix::gcode_viewer_should_stream(false, true, false));
    CHECK_FALSE(helix::gcode_viewer_should_stream(false, false, false));
}

TEST_CASE("gcode_viewer_should_stream: a small file still skips streaming without 3D",
          "[gcode][streaming]") {
    // The guard must not force streaming on every file - only stop the opt-out
    // from overriding the size decision when there is no 3D to justify it.
    CHECK_FALSE(helix::gcode_viewer_should_stream(true, false, false));
}
