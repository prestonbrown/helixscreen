// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_footer_summary.h"

#include "../catch_amalgamated.hpp"

using helix::gcode::gcode_tail_window_bytes;
using helix::gcode::GCODE_TAIL_WINDOW_DEFAULT;
using helix::gcode::GCODE_TAIL_WINDOW_MAX;
using helix::gcode::GCODE_TAIL_WINDOW_MIN;
using helix::gcode::parse_gcode_footer_summary;

namespace {

// The shape a real Orca-family footer has, condensed: the two lines that
// matter sit ~5 KB apart in the file with config noise in between.
constexpr const char* REAL_FOOTER = R"(; EXECUTABLE_BLOCK_END

; filament used [mm] = 0.00, 0.00, 0.00, 0.00, 11534.28
; filament used [cm3] = 0.00, 0.00, 0.00, 0.00, 27.71
; filament used [g] = 0.00, 0.00, 0.00, 0.00, 34.35
; total filament used [g] = 34.35
; filament_type = PLA;ASA-GF;ASA-GF;PLA;ASA
; filament_colour = #FFFFFF;#1A1A1A;#1A1A1A;#C12E1F;#1A1A1A
; nozzle_diameter = 0.4
)";

} // namespace

TEST_CASE("parse_gcode_footer_summary - real Orca footer", "[gcode][footer_summary]") {
    const auto s = parse_gcode_footer_summary(REAL_FOOTER);

    REQUIRE(s.has_usage_line);
    REQUIRE(s.usable());
    REQUIRE(s.tools_used == std::set<int>{4});
    REQUIRE(s.colours ==
            std::vector<std::string>{"#FFFFFF", "#1A1A1A", "#1A1A1A", "#C12E1F", "#1A1A1A"});
}

TEST_CASE("parse_gcode_footer_summary - used-tool rule", "[gcode][footer_summary]") {
    SECTION("multiple nonzero tools") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = 3.2, 0.00, 1.5, 0\n");
        REQUIRE(s.tools_used == std::set<int>{0, 2});
        REQUIRE(s.usable());
    }

    SECTION("single-extruder scalar answers tool 0") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = 12.34\n");
        REQUIRE(s.tools_used == std::set<int>{0});
        REQUIRE(s.usable());
    }

    SECTION("zero spellings all count as unused") {
        const auto s =
            parse_gcode_footer_summary("; filament used [g] = 0, 0.00, 0.0000, 0e5, 7\n");
        REQUIRE(s.tools_used == std::set<int>{4});
    }

    SECTION("scientific-notation trace usage still counts as used") {
        // 1e-3 g is a milligram — physically nothing, but the slicer named the
        // tool, and dropping a used tool silently skips its filament check.
        const auto s = parse_gcode_footer_summary("; filament used [g] = 1e-3, 0.00\n");
        REQUIRE(s.tools_used == std::set<int>{0});
    }

    SECTION("negative usage is not usage") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = -1.0, 2.0\n");
        REQUIRE(s.tools_used == std::set<int>{1});
    }

    SECTION("unparsable token keeps slot alignment") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = 0.00, nan?, junk, 4.0\n");
        REQUIRE(s.tools_used == std::set<int>{3});
    }

    SECTION("all-zero vector is not usable") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = 0.00, 0.00\n");
        REQUIRE(s.has_usage_line);
        REQUIRE(s.tools_used.empty());
        REQUIRE_FALSE(s.usable()); // caller must fall back to the full scan
    }

    SECTION("whitespace and tabs around values") {
        const auto s = parse_gcode_footer_summary(";filament used [g]=\t0.00 ,\t 5.5 \n");
        REQUIRE(s.tools_used == std::set<int>{1});
    }

    SECTION("key match is case-insensitive") {
        const auto s = parse_gcode_footer_summary("; Filament Used [G] = 0.00, 9.0\n");
        REQUIRE(s.tools_used == std::set<int>{1});
    }
}

TEST_CASE("parse_gcode_footer_summary - total line never answers tools",
          "[gcode][footer_summary]") {
    // A scalar total read as a one-element vector would claim {0} for every
    // multi-tool file — exactly the wrong answer, so it must be ignored.
    const auto s = parse_gcode_footer_summary("; total filament used [g] = 34.35\n");
    REQUIRE_FALSE(s.has_usage_line);
    REQUIRE(s.tools_used.empty());
    REQUIRE_FALSE(s.usable());
}

TEST_CASE("parse_gcode_footer_summary - colour selection", "[gcode][footer_summary]") {
    SECTION("filament_colour alone") {
        const auto s = parse_gcode_footer_summary("; filament_colour = #AABBCC;#112233\n");
        REQUIRE(s.colours == std::vector<std::string>{"#AABBCC", "#112233"});
    }

    SECTION("extruder_colour wins over filament_colour") {
        const auto s = parse_gcode_footer_summary("; filament_colour = #AABBCC\n"
                                                  "; extruder_colour = #001122\n");
        REQUIRE(s.colours == std::vector<std::string>{"#001122"});
    }

    SECTION("empty extruder_colour falls through to filament_colour") {
        const auto s = parse_gcode_footer_summary("; extruder_colour = ;;;;\n"
                                                  "; filament_colour = #AABBCC;#DDEEFF\n");
        REQUIRE(s.colours == std::vector<std::string>{"#AABBCC", "#DDEEFF"});
    }

    SECTION("neighbouring config keys do not hijack the palette") {
        // Substring matching on the key would let these override the real one.
        const auto s = parse_gcode_footer_summary("; filament_colour = #AABBCC\n"
                                                  "; default_filament_colour = #FFFFFF\n"
                                                  "; filament_colour_change = #000000\n");
        REQUIRE(s.colours == std::vector<std::string>{"#AABBCC"});
    }

    SECTION("last occurrence of a key wins") {
        const auto s = parse_gcode_footer_summary("; filament_colour = #AABBCC\n"
                                                  "; filament_colour = #123456;#654321\n");
        REQUIRE(s.colours == std::vector<std::string>{"#123456", "#654321"});
    }
}

TEST_CASE("parse_gcode_footer_summary - malformed and absent input", "[gcode][footer_summary]") {
    SECTION("empty input") {
        const auto s = parse_gcode_footer_summary("");
        REQUIRE(s.colours.empty());
        REQUIRE(s.tools_used.empty());
        REQUIRE_FALSE(s.usable());
    }

    SECTION("a slicer that writes neither key") {
        const auto s = parse_gcode_footer_summary("G1 X10 Y10 E1.2\n"
                                                  "; generated by SomeSlicer\n"
                                                  "M104 S0\n");
        REQUIRE(s.colours.empty());
        REQUIRE_FALSE(s.has_usage_line);
        REQUIRE_FALSE(s.usable());
    }

    SECTION("comment with no '=' is ignored") {
        const auto s = parse_gcode_footer_summary("; filament used [g]\n; filament_colour\n");
        REQUIRE_FALSE(s.has_usage_line);
        REQUIRE(s.colours.empty());
    }

    SECTION("usage key present with an empty value") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = \n");
        REQUIRE(s.has_usage_line);
        REQUIRE(s.tools_used.empty());
        REQUIRE_FALSE(s.usable());
    }

    SECTION("CRLF line endings") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = 0.00, 8.5\r\n"
                                                  "; filament_colour = #AABBCC;#DDEEFF\r\n");
        REQUIRE(s.tools_used == std::set<int>{1});
        REQUIRE(s.colours == std::vector<std::string>{"#AABBCC", "#DDEEFF"});
    }

    SECTION("no trailing newline on the last line") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = 0.00, 8.5");
        REQUIRE(s.tools_used == std::set<int>{1});
    }

    SECTION("leading partial line from a windowed read is ignored") {
        // A suffix range starts mid-line; the fragment lost its key, so it can
        // never be mistaken for a usage vector.
        const auto s = parse_gcode_footer_summary("0.00, 0.00, 99.9\n"
                                                  "; filament used [g] = 0.00, 8.5\n");
        REQUIRE(s.tools_used == std::set<int>{1});
    }

    SECTION("truncated final line yields a short vector, not a wrong one") {
        const auto s = parse_gcode_footer_summary("; filament used [g] = 0.00, 8.5, 3");
        REQUIRE(s.tools_used == std::set<int>{1, 2});
    }

    SECTION("binary noise does not crash or match") {
        static constexpr char kNoise[] = "\x01\x00;\xff= junk\n; filament used [g] = 4.0\n";
        const std::string noise(kNoise, sizeof(kNoise) - 1);
        const auto s = parse_gcode_footer_summary(noise);
        REQUIRE(s.tools_used == std::set<int>{0});
    }
}

TEST_CASE("gcode_tail_window_bytes - sizing", "[gcode][footer_summary]") {
    SECTION("exact footer size from metadata") {
        // The verified sample: 869908 - 845343 = 24565 bytes of footer.
        REQUIRE(gcode_tail_window_bytes(869908, 845343) == 24565);
    }

    SECTION("tiny footer is floored, never a pointless micro-request") {
        REQUIRE(gcode_tail_window_bytes(5'000'000, 4'999'000) == GCODE_TAIL_WINDOW_MIN);
    }

    SECTION("absurd footer is capped") {
        REQUIRE(gcode_tail_window_bytes(50'000'000, 1'000) == GCODE_TAIL_WINDOW_MAX);
    }

    SECTION("unknown gcode_end_byte falls back to the fixed window") {
        REQUIRE(gcode_tail_window_bytes(10'000'000, 0) == GCODE_TAIL_WINDOW_DEFAULT);
    }

    SECTION("20KB would have missed the sample's usage line") {
        // Regression guard on the default: the measured footer is larger.
        REQUIRE(GCODE_TAIL_WINDOW_DEFAULT > 24565);
    }

    SECTION("gcode_end_byte at or past EOF is nonsense - use the default") {
        REQUIRE(gcode_tail_window_bytes(1'000'000, 1'000'000) == GCODE_TAIL_WINDOW_DEFAULT);
        REQUIRE(gcode_tail_window_bytes(1'000'000, 2'000'000) == GCODE_TAIL_WINDOW_DEFAULT);
    }

    SECTION("never larger than the file") {
        REQUIRE(gcode_tail_window_bytes(4'096, 0) == 4'096);
        REQUIRE(gcode_tail_window_bytes(4'096, 4'000) == 4'096);
    }

    SECTION("unknown size still yields a requestable window") {
        REQUIRE(gcode_tail_window_bytes(0, 0) == GCODE_TAIL_WINDOW_DEFAULT);
    }

    SECTION("never zero - download_file_tail rejects a zero-length range") {
        REQUIRE(gcode_tail_window_bytes(1, 0) > 0);
    }
}
