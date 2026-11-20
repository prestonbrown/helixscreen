// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_streaming_config.cpp
 * @brief Unit tests for G-code streaming configuration and the low-RAM threshold cap
 */

#include "config.h"
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
static constexpr size_t kGbKb = 1024ULL * 1024; // 1GB in KB

// ============================================================================
// MemoryInfo::is_low_ram_device() tests (the <=2GB tier)
// ============================================================================

TEST_CASE("is_low_ram_device returns true for 1GB device", "[gcode]") {
    auto mem = make_mem(1 * kGbKb);
    REQUIRE(mem.is_low_ram_device());
}

TEST_CASE("is_low_ram_device returns true for 2GB device", "[gcode]") {
    auto mem = make_mem(2 * kGbKb);
    REQUIRE(mem.is_low_ram_device());
}

TEST_CASE("is_low_ram_device returns false for 4GB device", "[gcode]") {
    auto mem = make_mem(4 * kGbKb);
    REQUIRE_FALSE(mem.is_low_ram_device());
}

TEST_CASE("is_low_ram_device returns false for just above 2GB", "[gcode]") {
    auto mem = make_mem(2 * kGbKb + 1);
    REQUIRE_FALSE(mem.is_low_ram_device());
}

TEST_CASE("is_low_ram_device returns false for 8GB device", "[gcode]") {
    auto mem = make_mem(8 * kGbKb);
    REQUIRE_FALSE(mem.is_low_ram_device());
}

TEST_CASE("is_low_ram_device returns false for 16GB device", "[gcode]") {
    auto mem = make_mem(16 * kGbKb);
    REQUIRE_FALSE(mem.is_low_ram_device());
}

TEST_CASE("is_low_ram_device returns false when total_kb is 0 (unknown)", "[gcode]") {
    auto mem = make_mem(0);
    REQUIRE_FALSE(mem.is_low_ram_device());
}

// ============================================================================
// should_use_gcode_streaming(file_size, mem) testable overload
// ============================================================================

TEST_CASE("Testable overload full-parses a small file on a 2GB device with memory to spare",
          "[gcode]") {
    // Low-RAM boards get a smaller threshold PERCENTAGE, not a blanket force:
    // with 512MB available the ceiling stays multi-MB, so a 100KB file fits.
    auto mem = make_mem(2 * kGbKb, 512 * KB); // 2GB total, 512MB available
    size_t small_file = 100 * KB;             // 100KB file
    REQUIRE_FALSE(should_use_gcode_streaming(small_file, mem));
}

// ============================================================================
// Low-RAM fit check: the Pi 3B numbers the plan measured (856MB total,
// ~600MB available with the app running; a Benchy's 3D geometry is ~25-35MB
// on top of the parse)
// ============================================================================

TEST_CASE("A low-RAM board full-parses a Benchy-sized file", "[gcode]") {
    // 2.9MB Benchy on a Pi 3B: the print-status preview must be allowed to
    // load 3D and follow the print.
    auto mem = make_mem(856 * 1024, 600 * 1024); // 856MB total, 600MB available
    size_t benchy = 2970 * KB;                   // ~2.9MB
    REQUIRE_FALSE(should_use_gcode_streaming(benchy, mem));
}

TEST_CASE("A low-RAM board still streams a 45MB file", "[gcode]") {
    auto mem = make_mem(856 * 1024, 600 * 1024); // 856MB total, 600MB available
    size_t big_file = 45 * MB;
    REQUIRE(should_use_gcode_streaming(big_file, mem));
}

TEST_CASE("A low-RAM board streams again once available memory collapses", "[gcode]") {
    // The low-RAM rule is a fit check, not a force: the same 856MB board with
    // only 200MB available cannot afford the Benchy parse any more.
    auto mem = make_mem(856 * 1024, 200 * 1024);
    size_t benchy = 2970 * KB;
    REQUIRE(should_use_gcode_streaming(benchy, mem));
}

TEST_CASE("A configured threshold below 15 is honoured on a low-RAM board", "[gcode][streaming]") {
    // The 15% low-RAM share is a cap on big configured values, not a floor on
    // small ones: a user's 10 must stay 10, or a memory-starved board silently
    // full-parses files the smaller ceiling was meant to stream.
    const char* key = "/gcode_viewer/streaming_threshold_percent";
    helix::Config* config = helix::Config::get_instance();
    const int original = config->get<int>(key, 40);
    struct Restore {
        helix::Config* config;
        const char* key;
        int value;
        ~Restore() {
            config->set<int>(key, value);
        }
    } restore{config, key, original};

    config->set<int>(key, 10);
    REQUIRE(get_streaming_threshold_percent() == 10);

    // 2GB board, 512MB available: 10% of it over the 15x expansion is a
    // ~3.4MB ceiling; the 15% cap would give ~5.1MB. A 4.5MB file sits between
    // the two, so it streams only while the configured 10 is in force.
    auto mem = make_mem(2 * kGbKb, 512 * KB);
    REQUIRE(mem.is_low_ram_device());
    REQUIRE(should_use_gcode_streaming(size_t(4.5 * MB), mem));
    REQUIRE_FALSE(should_use_gcode_streaming(3 * MB, mem));
}

TEST_CASE("Testable overload uses threshold logic for 8GB device", "[gcode]") {
    // 8GB device with 4GB available, default 40% threshold:
    // threshold = (4GB * 0.40) / 15 expansion = ~109MB
    // A 1MB file should NOT trigger streaming
    auto mem = make_mem(8 * kGbKb, 4 * kGbKb); // 8GB total, 4GB available
    size_t small_file = 1 * MB;                // 1MB file
    REQUIRE_FALSE(should_use_gcode_streaming(small_file, mem));
}

TEST_CASE("Testable overload streams large file on 8GB device", "[gcode]") {
    // 8GB device with 4GB available, default 40% threshold:
    // threshold = (4GB * 0.40) / 15 = ~109MB
    // A 200MB file SHOULD trigger streaming
    auto mem = make_mem(8 * kGbKb, 4 * kGbKb); // 8GB total, 4GB available
    size_t large_file = 200 * MB;              // 200MB file
    REQUIRE(should_use_gcode_streaming(large_file, mem));
}

TEST_CASE("Testable overload falls back for unknown available memory on 8GB device", "[gcode]") {
    // 8GB total but available_kb=0 (unknown) - should fall back to 2MB heuristic
    auto mem = make_mem(8 * kGbKb, 0);
    size_t small_file = 1 * MB; // 1MB < 2MB threshold
    REQUIRE_FALSE(should_use_gcode_streaming(small_file, mem));

    size_t large_file = 3 * MB; // 3MB > 2MB threshold
    REQUIRE(should_use_gcode_streaming(large_file, mem));
}

// ============================================================================
// The "streaming" config section feeds the same decision
// ============================================================================

namespace {
template <typename T> struct ScopedConfigValue {
    const char* key;
    T original;
    ScopedConfigValue(const char* k, T value)
        : key(k), original(helix::Config::get_instance()->get<T>(k, T{})) {
        helix::Config::get_instance()->set<T>(key, value);
    }
    ~ScopedConfigValue() {
        helix::Config::get_instance()->set<T>(key, original);
    }
};
} // namespace

TEST_CASE("force_streaming in the streaming section forces streaming mode", "[gcode][streaming]") {
    ScopedConfigValue<std::string> mode("/gcode_viewer/streaming_mode", "auto");
    ScopedConfigValue<bool> force("/streaming/force_streaming", true);
    CHECK(get_gcode_streaming_mode() == GCodeStreamingMode::ON);
}

TEST_CASE("threshold_mb streams files above it even when RAM would allow a full load",
          "[gcode][streaming]") {
    ScopedConfigValue<int> threshold("/streaming/threshold_mb", 2);
    auto mem = make_mem(8 * kGbKb, 4 * kGbKb); // RAM alone would full-load ~109MB
    CHECK(should_use_gcode_streaming(3 * MB, mem));
    CHECK_FALSE(should_use_gcode_streaming(1 * MB, mem));
}

// ============================================================================
// A screen's streaming opt-out only counts when a full-parse 3D render can run
// ============================================================================

/**
 * PrintSelectDetailView calls helix::ui::gcode_viewer_disable_streaming() so a
 * 3D-preferred screen gets the full-load path, which parses the whole file
 * into RAM. The opt-out must not reach that path when nothing will render 3D,
 * nor when the parse does not fit the device.
 *
 * The full parse has no memory budget of its own, and the preview gate in
 * front of the load prices the streaming renderer, so this decision is the
 * only thing between a large file and an OOM kill on a low-RAM board.
 *
 * These run in a test binary that IS compiled with 3D enabled, which is exactly
 * why the decision is a pure function rather than an #ifdef at the call site:
 * an #ifdef would compile the interesting branch out of every test.
 */
TEST_CASE("gcode_viewer_should_stream: opt-out is ignored when nothing renders 3D",
          "[gcode][streaming]") {
    // Screen opted out, no 3D, file big enough to want streaming.
    CHECK(helix::gcode_viewer_should_stream(/*screen_opted_out=*/true, /*renders_3d=*/false,
                                            /*full_parse_fits=*/true,
                                            /*streaming_for_size=*/true));
}

TEST_CASE("gcode_viewer_should_stream: opt-out is honored when 3D renders and the parse fits",
          "[gcode][streaming]") {
    // A 3D-capable screen with the RAM for it still gets full-load.
    CHECK_FALSE(helix::gcode_viewer_should_stream(true, true, true, true));
}

TEST_CASE("gcode_viewer_should_stream: opt-out streams when the full parse would not fit",
          "[gcode][streaming]") {
    // A 3D build on a low-RAM board (pi32): the full parse of a large file
    // would exhaust memory, so the viewer streams despite the opt-out.
    CHECK(helix::gcode_viewer_should_stream(/*screen_opted_out=*/true, /*renders_3d=*/true,
                                            /*full_parse_fits=*/false,
                                            /*streaming_for_size=*/true));
}

TEST_CASE("gcode_viewer_should_stream: opt-out never forces streaming on its own",
          "[gcode][streaming]") {
    // A file the size rule already full-loads keeps doing so, whatever the
    // opt-out's own conditions say.
    CHECK_FALSE(helix::gcode_viewer_should_stream(true, true, false, false));
    CHECK_FALSE(helix::gcode_viewer_should_stream(true, false, true, false));
    CHECK_FALSE(helix::gcode_viewer_should_stream(true, false, false, false));
}

TEST_CASE("gcode_viewer_should_stream: no opt-out defers to the size decision",
          "[gcode][streaming]") {
    for (const bool renders_3d : {false, true}) {
        for (const bool fits : {false, true}) {
            CAPTURE(renders_3d, fits);
            CHECK(helix::gcode_viewer_should_stream(false, renders_3d, fits, true));
            CHECK_FALSE(helix::gcode_viewer_should_stream(false, renders_3d, fits, false));
        }
    }
}
