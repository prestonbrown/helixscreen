// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests for the lightweight, memory-safe tool-change scanner used by the
// pre-flight gate on 2D-only platforms (where the visual GCodeParser does not
// run). The scanner must find tool changes scattered through the WHOLE body,
// ignore Tn inside comments, and match the standalone-Tn semantics of the full
// parser.

#include "gcode_parser.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::gcode::scan_tools_used_from_content;
using helix::gcode::scan_tools_used_from_file;

TEST_CASE("scan_tools_used_from_content - basic tool changes", "[gcode][tools_used]") {
    SECTION("No tool changes (single extruder body)") {
        // No Tn anywhere → empty set. (The {0} convention is applied by callers
        // who also know a color palette exists; the scanner reports raw truth.)
        const std::string g = "G28\nG1 X0 Y0 Z0.2\nG1 X10 Y10 E1.0\n";
        REQUIRE(scan_tools_used_from_content(g).empty());
    }

    SECTION("Single explicit T0") {
        const std::string g = "G28\nT0\nG1 X10 E1\n";
        std::set<int> expect{0};
        REQUIRE(scan_tools_used_from_content(g) == expect);
    }

    SECTION("Multiple distinct tools deduped and sorted") {
        const std::string g = "T0\nG1 X1 E1\nT2\nG1 X2 E1\nT0\nG1 X3 E1\nT1\n";
        std::set<int> expect{0, 1, 2};
        REQUIRE(scan_tools_used_from_content(g) == expect);
    }
}

TEST_CASE("scan_tools_used_from_content - tool changes after first extrusion",
          "[gcode][tools_used]") {
    // The defining failure mode: a partial / header-only scan stops at the first
    // extrusion. Tool changes in the body MUST still be found.
    std::string g;
    g += "; HEADER\n";
    g += "T0\n";
    g += "G1 X10 Y10 Z0.2 E5.0\n"; // first extrusion — a partial scanner stops here
    for (int i = 0; i < 2000; ++i) {
        g += "G1 X" + std::to_string(i % 50) + " Y" + std::to_string(i % 40) + " E0.05\n";
    }
    g += "T3\n"; // deep in the body
    g += "G1 X5 Y5 E0.05\n";
    g += "T1\n";

    std::set<int> expect{0, 1, 3};
    REQUIRE(scan_tools_used_from_content(g) == expect);
}

TEST_CASE("scan_tools_used_from_content - comment and false-positive handling",
          "[gcode][tools_used]") {
    SECTION("Tn inside a full-line comment is ignored") {
        const std::string g = "; T5 this is a comment about tool 5\nG1 X1 E1\n";
        REQUIRE(scan_tools_used_from_content(g).empty());
    }

    SECTION("Tn inside a trailing comment is ignored") {
        const std::string g = "G1 X1 E1 ; switch to T7 next\n";
        REQUIRE(scan_tools_used_from_content(g).empty());
    }

    SECTION("Trailing comment after a real tool change is still counted") {
        const std::string g = "T2 ; change to tool 2\nG1 X1 E1\n";
        std::set<int> expect{2};
        REQUIRE(scan_tools_used_from_content(g) == expect);
    }

    SECTION("Indented tool change is counted") {
        const std::string g = "   T4   \nG1 X1 E1\n";
        std::set<int> expect{4};
        REQUIRE(scan_tools_used_from_content(g) == expect);
    }

    SECTION("Tn with parameters on the same line is NOT a tool change") {
        // "T0 X1" style is a different command form; mirror the full parser which
        // only treats standalone Tn as a tool change.
        const std::string g = "T0 P1 S210\nG1 X1 E1\n";
        REQUIRE(scan_tools_used_from_content(g).empty());
    }

    SECTION("Word starting with T is not a tool change") {
        const std::string g = "TURN_ON_HEATERS\nTOOL_PARK\nG1 X1 E1\n";
        REQUIRE(scan_tools_used_from_content(g).empty());
    }

    SECTION("Bare T with no digits is ignored") {
        const std::string g = "T\nG1 X1 E1\n";
        REQUIRE(scan_tools_used_from_content(g).empty());
    }

    SECTION("Multi-digit tool index (extended tools)") {
        const std::string g = "T0\nT12\nT3\n";
        std::set<int> expect{0, 3, 12};
        REQUIRE(scan_tools_used_from_content(g) == expect);
    }

    SECTION("CRLF line endings") {
        const std::string g = "T0\r\nG1 X1 E1\r\nT1\r\n";
        std::set<int> expect{0, 1};
        REQUIRE(scan_tools_used_from_content(g) == expect);
    }
}

TEST_CASE("scan_tools_used_from_file - streams from disk", "[gcode][tools_used]") {
    char tmpl[] = "/tmp/helix_tools_used_XXXXXX";
    int fd = mkstemp(tmpl);
    REQUIRE(fd >= 0);
    std::string path(tmpl);

    {
        std::ofstream out(path);
        out << "; header\n";
        out << "T0\n";
        out << "G1 X10 Y10 Z0.2 E5.0\n";
        for (int i = 0; i < 500; ++i) {
            out << "G1 X" << (i % 30) << " E0.05\n";
        }
        out << "T2 ; deep tool change\n";
        out << "G1 X1 E0.05\n";
    }
    ::close(fd);

    std::set<int> expect{0, 2};
    REQUIRE(scan_tools_used_from_file(path) == expect);

    std::remove(path.c_str());
}

TEST_CASE("scan_tools_used_from_file - missing file returns empty", "[gcode][tools_used]") {
    REQUIRE(scan_tools_used_from_file("/tmp/does_not_exist_helix_XXXXXX.gcode").empty());
}

TEST_CASE("scan_tools_used - early exit", "[gcode][tools_used]") {
    // Body where all palette tools appear near the head, then a huge tail.
    std::string g = "T0\nG1 X1 E1\nT1\nG1 X2 E1\nT0\n";
    for (int i = 0; i < 50000; ++i) {
        g += "G1 X" + std::to_string(i % 50) + " E0.05\n";
    }
    const std::set<int> full{0, 1};

    SECTION("Early exit returns the same set as a full scan") {
        REQUIRE(scan_tools_used_from_content(g, full) == full);
    }
    SECTION("Subset file: stop set never completed, full scan still correct") {
        // File only ever uses T0; stop set {0,1} is unreachable => whole body
        // is read and the result is still exactly {0}.
        const std::set<int> expect{0};
        REQUIRE(scan_tools_used_from_content(g.substr(0, g.find("T1")), {0, 1}) == expect);
    }
    SECTION("Empty stop set behaves exactly like today") {
        REQUIRE(scan_tools_used_from_content(g, {}) == full);
        REQUIRE(scan_tools_used_from_content(g) == full);
    }
    SECTION("Early exit finds a late tool that IS in the stop set") {
        // T1 appears only deep in the body — must not be missed just because
        // T0 was seen immediately.
        std::string late = "T0\n";
        for (int i = 0; i < 20000; ++i)
            late += "G1 X1 E0.1\n";
        late += "T1\n";
        const std::set<int> expect{0, 1};
        REQUIRE(scan_tools_used_from_content(late, {0, 1}) == expect);
    }
}
