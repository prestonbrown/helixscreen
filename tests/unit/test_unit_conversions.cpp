// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "unit_conversions_json.h"

#include "../catch_amalgamated.hpp"

using namespace helix::units;
using json = nlohmann::json;

// ============================================================================
// Temperature Conversion Tests
// ============================================================================

TEST_CASE("to_decidegrees converts correctly", "[unit_conversions][temperature]") {
    SECTION("zero degrees") {
        REQUIRE(to_decidegrees(0.0) == 0);
    }

    SECTION("positive temperatures") {
        REQUIRE(to_decidegrees(25.0) == 250);
        REQUIRE(to_decidegrees(25.5) == 255);
        REQUIRE(to_decidegrees(200.0) == 2000);
        REQUIRE(to_decidegrees(210.7) == 2107);
    }

    SECTION("decimal precision") {
        REQUIRE(to_decidegrees(25.15) == 251); // Truncates to int
        REQUIRE(to_decidegrees(25.99) == 259); // Truncates, not rounds
    }

    SECTION("negative temperatures") {
        REQUIRE(to_decidegrees(-10.0) == -100);
        REQUIRE(to_decidegrees(-0.5) == -5);
    }
}

TEST_CASE("from_decidegrees converts correctly", "[unit_conversions][temperature]") {
    REQUIRE(from_decidegrees(0) == 0.0);
    REQUIRE(from_decidegrees(250) == 25.0);
    REQUIRE(from_decidegrees(255) == 25.5);
    REQUIRE(from_decidegrees(-100) == -10.0);
}

// ============================================================================
// Percent Conversion Tests
// ============================================================================

TEST_CASE("to_percent converts correctly", "[unit_conversions][percent]") {
    SECTION("standard values") {
        REQUIRE(to_percent(0.0) == 0);
        REQUIRE(to_percent(0.5) == 50);
        REQUIRE(to_percent(1.0) == 100);
        REQUIRE(to_percent(0.75) == 75);
    }

    SECTION("over 100%") {
        REQUIRE(to_percent(1.5) == 150);
        REQUIRE(to_percent(2.0) == 200);
    }

    SECTION("small values") {
        REQUIRE(to_percent(0.01) == 1);
        REQUIRE(to_percent(0.001) == 0); // Rounds to 0
    }

    SECTION("float precision edge cases (rounding vs truncation)") {
        // Issue #14: 0.9 represented as 0.8999... should round to 90, not truncate to 89
        REQUIRE(to_percent(0.8999999999) == 90);
        REQUIRE(to_percent(1.999999) == 200);
    }

    SECTION("rounding boundaries") {
        REQUIRE(to_percent(0.895) == 90); // 89.5 rounds up
        REQUIRE(to_percent(0.894) == 89); // 89.4 rounds down
        REQUIRE(to_percent(0.005) == 1);  // 0.5 rounds up
        REQUIRE(to_percent(0.004) == 0);  // 0.4 rounds down
    }
}

TEST_CASE("from_percent converts correctly", "[unit_conversions][percent]") {
    REQUIRE(from_percent(0) == 0.0);
    REQUIRE(from_percent(50) == 0.5);
    REQUIRE(from_percent(100) == 1.0);
    REQUIRE(from_percent(150) == 1.5);
}

// ============================================================================
// Length Conversion Tests
// ============================================================================

TEST_CASE("to_centimm converts correctly", "[unit_conversions][length]") {
    SECTION("standard values") {
        REQUIRE(to_centimm(0.0) == 0);
        REQUIRE(to_centimm(1.0) == 100);
        REQUIRE(to_centimm(1.25) == 125);
        REQUIRE(to_centimm(10.5) == 1050);
    }

    SECTION("small values") {
        REQUIRE(to_centimm(0.01) == 1);
        REQUIRE(to_centimm(0.001) == 0); // Truncates
    }

    SECTION("negative values") {
        REQUIRE(to_centimm(-1.0) == -100);
    }
}

TEST_CASE("from_centimm converts correctly", "[unit_conversions][length]") {
    REQUIRE(from_centimm(0) == 0.0);
    REQUIRE(from_centimm(100) == 1.0);
    REQUIRE(from_centimm(125) == 1.25);
    REQUIRE(from_centimm(-100) == -1.0);
}

// ============================================================================
// Speed Conversion Tests
// ============================================================================

TEST_CASE("json_to_* convert a numeric field and fall back to the default otherwise",
          "[unit_conversions][json]") {
    using Reader = int (*)(const json&, const char*, int);
    struct Case {
        const char* name;
        Reader read;
        double value;
        int converted;
    };
    const Case cases[] = {
        {"decidegrees", json_to_decidegrees, 25.5, 255},
        {"percent", json_to_percent, 0.75, 75},
        {"centimm", json_to_centimm, 1.25, 125},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        REQUIRE(c.read(json{{"v", c.value}}, "v", -1) == c.converted);
        REQUIRE(c.read(json{{"other", 1}}, "v", -1) == -1);
        REQUIRE(c.read(json{{"v", "hot"}}, "v", -1) == -1);
        REQUIRE(c.read(json{{"v", nullptr}}, "v", -1) == -1);
        for (const json& not_object : {json::array({1, 2}), json(42), json(nullptr), json("s")}) {
            REQUIRE(c.read(not_object, "v", -1) == -1);
        }
    }
    REQUIRE(json_to_decidegrees(json::object(), "v") == 0);
}

TEST_CASE("speed_factor_to_percent converts correctly", "[unit_conversions][speed]") {
    REQUIRE(speed_factor_to_percent(1.0) == 100);
    REQUIRE(speed_factor_to_percent(0.5) == 50);
    REQUIRE(speed_factor_to_percent(1.5) == 150); // Overdrive
    REQUIRE(speed_factor_to_percent(0.0) == 0);
}

TEST_CASE("mm_per_sec_to_mm_per_min converts correctly", "[unit_conversions][speed]") {
    REQUIRE(mm_per_sec_to_mm_per_min(1.0) == 60);
    REQUIRE(mm_per_sec_to_mm_per_min(10.0) == 600);
    REQUIRE(mm_per_sec_to_mm_per_min(0.5) == 30);
    REQUIRE(mm_per_sec_to_mm_per_min(100.0) == 6000);
}

// ============================================================================
// Round-trip Tests
// ============================================================================

TEST_CASE("round-trip conversions maintain precision", "[unit_conversions][roundtrip]") {
    SECTION("temperature round-trip") {
        double original = 25.5;
        int deci = to_decidegrees(original);
        double recovered = from_decidegrees(deci);
        REQUIRE(recovered == original);
    }

    SECTION("percent round-trip") {
        double original = 0.75;
        int pct = to_percent(original);
        double recovered = from_percent(pct);
        REQUIRE(recovered == original);
    }

    SECTION("length round-trip") {
        double original = 1.25;
        int centi = to_centimm(original);
        double recovered = from_centimm(centi);
        REQUIRE(recovered == original);
    }
}
