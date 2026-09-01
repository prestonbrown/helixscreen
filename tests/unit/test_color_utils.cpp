// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_color_utils.cpp
 * @brief Unit tests for color_utils parsing and naming functions
 */

#include "color_utils.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// parse_hex_color Tests
// ============================================================================

TEST_CASE("parse_hex_color: valid 6-digit formats", "[color][parse]") {
    uint32_t rgb;

    SECTION("#RRGGBB format") {
        REQUIRE(helix::parse_hex_color("#FF0000", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("#00FF00", rgb) == true);
        REQUIRE(rgb == 0x00FF00);

        REQUIRE(helix::parse_hex_color("#0000FF", rgb) == true);
        REQUIRE(rgb == 0x0000FF);
    }

    SECTION("RRGGBB format (no hash)") {
        REQUIRE(helix::parse_hex_color("FF4444", rgb) == true);
        REQUIRE(rgb == 0xFF4444);
    }

    SECTION("0xRRGGBB format (C-style)") {
        REQUIRE(helix::parse_hex_color("0xFF4444", rgb) == true);
        REQUIRE(rgb == 0xFF4444);

        REQUIRE(helix::parse_hex_color("0XFF4444", rgb) == true);
        REQUIRE(rgb == 0xFF4444);
    }

    SECTION("Case insensitive") {
        REQUIRE(helix::parse_hex_color("#ff4444", rgb) == true);
        REQUIRE(rgb == 0xFF4444);

        REQUIRE(helix::parse_hex_color("#fF44Aa", rgb) == true);
        REQUIRE(rgb == 0xFF44AA);
    }
}

TEST_CASE("parse_hex_color: valid 3-digit shorthand", "[color][parse]") {
    uint32_t rgb;

    SECTION("#RGB expands to #RRGGBB") {
        REQUIRE(helix::parse_hex_color("#F00", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("#0F0", rgb) == true);
        REQUIRE(rgb == 0x00FF00);

        REQUIRE(helix::parse_hex_color("#00F", rgb) == true);
        REQUIRE(rgb == 0x0000FF);

        REQUIRE(helix::parse_hex_color("#ABC", rgb) == true);
        REQUIRE(rgb == 0xAABBCC);
    }

    SECTION("RGB without hash") {
        REQUIRE(helix::parse_hex_color("F44", rgb) == true);
        REQUIRE(rgb == 0xFF4444);
    }
}

TEST_CASE("parse_hex_color: whitespace handling", "[color][parse]") {
    uint32_t rgb;

    SECTION("Leading whitespace trimmed") {
        REQUIRE(helix::parse_hex_color("  #FF0000", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("\t#FF0000", rgb) == true);
        REQUIRE(rgb == 0xFF0000);
    }

    SECTION("Trailing whitespace trimmed") {
        REQUIRE(helix::parse_hex_color("#FF0000  ", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("#FF0000\n", rgb) == true);
        REQUIRE(rgb == 0xFF0000);
    }

    SECTION("Both leading and trailing") {
        REQUIRE(helix::parse_hex_color("  #FF0000  ", rgb) == true);
        REQUIRE(rgb == 0xFF0000);
    }
}

TEST_CASE("parse_hex_color: 0x prefix with shorthand", "[color][parse]") {
    uint32_t rgb;

    SECTION("0xRGB expands to 0xRRGGBB") {
        REQUIRE(helix::parse_hex_color("0xF00", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("0xABC", rgb) == true);
        REQUIRE(rgb == 0xAABBCC);
    }
}

TEST_CASE("parse_hex_color: invalid inputs", "[color][parse]") {
    uint32_t rgb = 0xDEADBEEF; // Sentinel value

    SECTION("Empty string") {
        REQUIRE(helix::parse_hex_color("", rgb) == false);
        REQUIRE(rgb == 0xDEADBEEF); // Unchanged
    }

    SECTION("Null pointer") {
        REQUIRE(helix::parse_hex_color(nullptr, rgb) == false);
    }

    SECTION("Whitespace only") {
        REQUIRE(helix::parse_hex_color("   ", rgb) == false);
        REQUIRE(helix::parse_hex_color("\t\n", rgb) == false);
    }

    SECTION("Invalid characters") {
        REQUIRE(helix::parse_hex_color("#GGGGGG", rgb) == false);
        REQUIRE(helix::parse_hex_color("#ZZZZZZ", rgb) == false);
        REQUIRE(helix::parse_hex_color("invalid", rgb) == false);
    }

    SECTION("Wrong digit count") {
        REQUIRE(helix::parse_hex_color("#FF", rgb) == false);      // 2 digits
        REQUIRE(helix::parse_hex_color("#FFFF", rgb) == false);    // 4 digits
        REQUIRE(helix::parse_hex_color("#FFFFF", rgb) == false);   // 5 digits
        REQUIRE(helix::parse_hex_color("#FFFFFFF", rgb) == false); // 7 digits
    }

    SECTION("Garbage after valid hex") {
        REQUIRE(helix::parse_hex_color("#FF0000garbage", rgb) == false);
        REQUIRE(helix::parse_hex_color("#FF0000 garbage", rgb) == false);
    }

    SECTION("Only prefix") {
        REQUIRE(helix::parse_hex_color("#", rgb) == false);
        REQUIRE(helix::parse_hex_color("0x", rgb) == false);
    }

    SECTION("Hash with only whitespace") {
        REQUIRE(helix::parse_hex_color("#   ", rgb) == false);
    }
}

// ============================================================================
// 8-digit #RRGGBBAA (prestonbrown/helixscreen#1419)
// ============================================================================

TEST_CASE("parse_hex_color: 8-digit #RRGGBBAA drops alpha", "[color][parse]") {
    uint32_t rgb = 0;

    SECTION("Alpha is shifted off, not masked off") {
        // The whole bug in one assertion. Consumers spelled the conversion
        // lv_color_hex(strtol(hex + 1, nullptr, 16)), handing lv_color_hex the
        // full 0x800080FF so it read red from bits 16-23 - RGB(00,80,FF),
        // bright azure, for a purple filament. Masking the alpha off with
        // & 0xFFFFFF instead of shifting reproduces that exact wrong answer,
        // so pin the value rather than just asserting "parsed".
        REQUIRE(helix::parse_hex_color("#800080FF", rgb));
        REQUIRE(rgb == 0x800080);
        REQUIRE(rgb != 0x0080FF);
    }

    SECTION("The alpha value does not change the result") {
        uint32_t opaque = 0;
        uint32_t clear = 0;
        REQUIRE(helix::parse_hex_color("#ED1C24FF", opaque));
        REQUIRE(helix::parse_hex_color("#ED1C2400", clear));
        REQUIRE(opaque == 0xED1C24);
        REQUIRE(clear == 0xED1C24);
    }

    SECTION("A high red byte does not saturate") {
        // strtol returns a signed long. On 32-bit ARM every 8-digit token with
        // RR >= 0x80 exceeds LONG_MAX and clamps to 0x7FFFFFFF, which renders
        // white - a failure no x86 dev host or x86 CI run can reproduce.
        // parse_hex_color accumulates into a uint32_t, so this holds anywhere.
        REQUIRE(helix::parse_hex_color("#FF0000FF", rgb));
        REQUIRE(rgb == 0xFF0000);
        REQUIRE(helix::parse_hex_color("#80000000", rgb));
        REQUIRE(rgb == 0x800000);
    }

    SECTION("Accepted bare and with the 0x prefix") {
        REQUIRE(helix::parse_hex_color("800080FF", rgb));
        REQUIRE(rgb == 0x800080);
        REQUIRE(helix::parse_hex_color("0x800080FF", rgb));
        REQUIRE(rgb == 0x800080);
    }

    SECTION("The digit-count boundary still holds at 7 and 9") {
        uint32_t sentinel = 0xDEADBEEF;
        REQUIRE(helix::parse_hex_color("#FFFFFFF", sentinel) == false);
        REQUIRE(helix::parse_hex_color("#800080FFA", sentinel) == false);
        REQUIRE(sentinel == 0xDEADBEEF);
    }
}

TEST_CASE("parse_hex_color: 8-digit through the optional overload", "[color][parse]") {
    const auto purple = helix::parse_hex_color(std::string("#800080FF"));
    REQUIRE(purple.has_value());
    REQUIRE(*purple == 0x800080);
}

// ============================================================================
// parse_hex_color optional overload Tests
// ============================================================================

TEST_CASE("parse_hex_color optional overload", "[color]") {
    SECTION("parses standard 6-digit hex with #") {
        auto result = helix::parse_hex_color(std::string("#FF0000"));
        REQUIRE(result.has_value());
        REQUIRE(*result == 0xFF0000);
    }
    SECTION("parses without # prefix") {
        auto result = helix::parse_hex_color(std::string("00FF00"));
        REQUIRE(result.has_value());
        REQUIRE(*result == 0x00FF00);
    }
    SECTION("parses 3-digit shorthand") {
        auto result = helix::parse_hex_color(std::string("#F00"));
        REQUIRE(result.has_value());
        REQUIRE(*result == 0xFF0000);
    }
    SECTION("returns nullopt for empty string") {
        auto result = helix::parse_hex_color(std::string(""));
        REQUIRE_FALSE(result.has_value());
    }
    SECTION("returns nullopt for invalid hex") {
        auto result = helix::parse_hex_color(std::string("#ZZZZZZ"));
        REQUIRE_FALSE(result.has_value());
    }
    SECTION("handles whitespace") {
        auto result = helix::parse_hex_color(std::string("  #0000FF  "));
        REQUIRE(result.has_value());
        REQUIRE(*result == 0x0000FF);
    }
}

// ============================================================================
// describe_color Tests
// ============================================================================

TEST_CASE("describe_color: basic colors", "[color][describe]") {
    SECTION("Pure red") {
        std::string name = helix::describe_color(0xFF0000);
        REQUIRE(name.find("Red") != std::string::npos);
    }

    SECTION("Pure green") {
        std::string name = helix::describe_color(0x00FF00);
        REQUIRE(name.find("Green") != std::string::npos);
    }

    SECTION("Pure blue") {
        std::string name = helix::describe_color(0x0000FF);
        REQUIRE(name.find("Blue") != std::string::npos);
    }
}

TEST_CASE("describe_color: grayscale", "[color][describe]") {
    REQUIRE(helix::describe_color(0xFFFFFF) == "White");
    REQUIRE(helix::describe_color(0x000000) == "Black");

    std::string gray = helix::describe_color(0x808080);
    REQUIRE(gray.find("Gray") != std::string::npos);
}
