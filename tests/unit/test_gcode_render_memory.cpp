// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/gcode_render_memory.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::gcode::RenderMemoryReport;

// ---------------------------------------------------------------------------
// This is measurement apparatus, so it gets held to the standard the
// measurements will be: if the accounting is wrong, every A/B built on it is
// wrong in the same direction and nothing downstream will notice.
// ---------------------------------------------------------------------------

TEST_CASE("an empty report totals zero and says so", "[gcode_render_memory]") {
    RenderMemoryReport r;
    REQUIRE(r.total() == 0);
    REQUIRE(r.items().empty());
    REQUIRE(r.format() == "0 KB total");
}

TEST_CASE("the total is the sum of the line items", "[gcode_render_memory]") {
    RenderMemoryReport r;
    r.add("solid_cache", 1000);
    r.add("ghost_cache", 2000);
    r.add("ssao_cache", 500);
    REQUIRE(r.total() == 3500);
    REQUIRE(r.items().size() == 3);
}

TEST_CASE("a zero-byte item is recorded, not dropped", "[gcode_render_memory]") {
    // "allocated and empty" and "not allocated" are different states, and the
    // whole point of the SSAO work is watching one buffer go to zero. A report
    // that omitted the item would make the change invisible in the log rather
    // than obvious.
    RenderMemoryReport r;
    r.add("solid_cache", 4096);
    r.add("ssao_cache", 0);
    REQUIRE(r.items().size() == 2);
    REQUIRE(r.bytes_for("ssao_cache") == 0);
    REQUIRE(r.format().find("ssao_cache 0") != std::string::npos);
}

TEST_CASE("line items are kept in the order they were added", "[gcode_render_memory]") {
    // The renderers order these to be read, not to be sorted.
    RenderMemoryReport r;
    r.add("first", 1);
    r.add("second", 2);
    r.add("third", 3);
    REQUIRE(std::string(r.items()[0].name) == "first");
    REQUIRE(std::string(r.items()[1].name) == "second");
    REQUIRE(std::string(r.items()[2].name) == "third");
}

TEST_CASE("bytes_for compares text, not pointers", "[gcode_render_memory]") {
    // Two identical literals are not guaranteed to be the same pointer across
    // translation units. A pointer comparison would work in this test file and
    // silently return 0 in the renderer, which is the worst possible failure
    // for a measurement tool: a plausible number that is not the real one.
    RenderMemoryReport r;
    r.add("ssao_cache", 12345);

    std::string built = "ssao";
    built += "_cache";
    REQUIRE(r.bytes_for(built.c_str()) == 12345);
    REQUIRE(r.bytes_for("nope") == 0);
}

TEST_CASE("add_buffer counts nothing when the buffer is not allocated", "[gcode_render_memory]") {
    RenderMemoryReport r;
    r.add_buffer("ghost_raw", /*allocated=*/false, 400, 400, 4);
    REQUIRE(r.total() == 0);
    REQUIRE(r.items().size() == 1);
}

TEST_CASE("add_buffer multiplies width, height and depth", "[gcode_render_memory]") {
    RenderMemoryReport r;
    r.add_buffer("ghost_raw", true, 377, 287, 4);
    REQUIRE(r.bytes_for("ghost_raw") == 377u * 287u * 4u);
}

TEST_CASE("a large buffer does not overflow the accounting", "[gcode_render_memory]") {
    // 4K at 4bpp is 33MB, well inside size_t, but the multiply has to happen in
    // size_t and not in int. On a 32-bit printer int is 32 bits and 4096*4096*4
    // overflows a signed int before it is ever widened.
    RenderMemoryReport r;
    r.add_buffer("huge", true, 4096, 4096, 4);
    REQUIRE(r.bytes_for("huge") == static_cast<size_t>(4096) * 4096 * 4);
    REQUIRE(r.total() > 67000000u);
}

TEST_CASE("format rounds kilobytes up so a non-empty buffer never reads as zero",
          "[gcode_render_memory]") {
    // A buffer holding 1 byte is allocated. Truncating to 0 KB would report it
    // as absent, which is the one thing this line must never do.
    RenderMemoryReport r;
    r.add("tiny", 1);
    REQUIRE(r.format().find("tiny 1") != std::string::npos);
    REQUIRE(r.format().find("1 KB total") != std::string::npos);
}

TEST_CASE("format names every item and the total", "[gcode_render_memory]") {
    RenderMemoryReport r;
    r.add("solid_cache", 434 * 1024);
    r.add("ghost_cache", 434 * 1024);
    r.add("ghost_raw", 434 * 1024);
    r.add("ssao_cache", 434 * 1024);

    const std::string out = r.format();
    INFO(out);
    REQUIRE(out.find("1736 KB total") != std::string::npos);
    for (const char* name : {"solid_cache", "ghost_cache", "ghost_raw", "ssao_cache"}) {
        REQUIRE(out.find(name) != std::string::npos);
    }
}
