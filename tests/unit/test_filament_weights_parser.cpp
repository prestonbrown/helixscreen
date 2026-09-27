// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../src/api/moonraker_api_internal.h"

#include "../catch_amalgamated.hpp"

using moonraker_internal::parse_filament_weights;
using nlohmann::json;

TEST_CASE("parse_filament_weights: filament_weights array (preferred form)",
          "[moonraker][metadata][weights]") {
    json obj = {{"filament_weights", json::array({0.0, 0.0, 0.0, 12.5})}};
    auto w = parse_filament_weights(obj);

    REQUIRE(w.size() == 4);
    CHECK(w[0] == 0.0);
    CHECK(w[1] == 0.0);
    CHECK(w[2] == 0.0);
    CHECK(w[3] == Catch::Approx(12.5));
}

TEST_CASE("parse_filament_weights: filament_used array fallback",
          "[moonraker][metadata][weights]") {
    // Some Moonraker builds expose per-tool used-mm here. Caller only cares
    // about >0/==0 distinction, so unit mismatch (mm vs g) is fine.
    json obj = {{"filament_used", json::array({0.0, 0.0, 4123.5, 0.0})}};
    auto w = parse_filament_weights(obj);

    REQUIRE(w.size() == 4);
    CHECK(w[0] == 0.0);
    CHECK(w[1] == 0.0);
    CHECK(w[2] == Catch::Approx(4123.5));
    CHECK(w[3] == 0.0);
}

TEST_CASE("parse_filament_weights: filament_used semicolon string",
          "[moonraker][metadata][weights]") {
    json obj = {{"filament_used", "0.0;0.0;0.0;12.5"}};
    auto w = parse_filament_weights(obj);

    REQUIRE(w.size() == 4);
    CHECK(w[0] == 0.0);
    CHECK(w[3] == Catch::Approx(12.5));
}

TEST_CASE("parse_filament_weights: filament_used comma string", "[moonraker][metadata][weights]") {
    json obj = {{"filament_used", "0.0,0.0,0.0,12.5"}};
    auto w = parse_filament_weights(obj);

    REQUIRE(w.size() == 4);
    CHECK(w[3] == Catch::Approx(12.5));
}

TEST_CASE("parse_filament_weights: missing field returns empty", "[moonraker][metadata][weights]") {
    // Empty MUST mean "unknown" so the caller falls back to checking every
    // tool. Returning all-zeros here would incorrectly suppress all warnings.
    json obj = json::object();
    auto w = parse_filament_weights(obj);

    CHECK(w.empty());
}

TEST_CASE("parse_filament_weights: filament_weights preferred over filament_used",
          "[moonraker][metadata][weights]") {
    // Both keys present (some slicers emit both) — prefer the more specific
    // filament_weights array.
    json obj = {{"filament_weights", json::array({1.0, 2.0})},
                {"filament_used", json::array({99.0, 99.0})}};
    auto w = parse_filament_weights(obj);

    REQUIRE(w.size() == 2);
    CHECK(w[0] == Catch::Approx(1.0));
    CHECK(w[1] == Catch::Approx(2.0));
}

TEST_CASE("parse_filament_weights: numeric strings parse like numbers",
          "[moonraker][metadata][weights][json_coercion]") {
    json obj = {{"filament_weights", json::array({"12.5", "0", 3.25})}};
    auto w = parse_filament_weights(obj);

    REQUIRE(w.size() == 3);
    CHECK(w[0] == Catch::Approx(12.5));
    CHECK(w[1] == 0.0);
    CHECK(w[2] == Catch::Approx(3.25));

    obj = {{"filament_used", json::array({"0", "4123.5"})}};
    w = parse_filament_weights(obj);
    REQUIRE(w.size() == 2);
    CHECK(w[0] == 0.0);
    CHECK(w[1] == Catch::Approx(4123.5));
}

// A 0.0 standing in for an entry that did not parse would read as "tool unused"
// and skip that tool's material checks, so the whole vector becomes unknown.
TEST_CASE("parse_filament_weights: any unparseable array entry makes the result unknown",
          "[moonraker][metadata][weights][json_coercion]") {
    CHECK(
        parse_filament_weights({{"filament_weights", json::array({1.5, "garbage", 4.0})}}).empty());
    CHECK(parse_filament_weights({{"filament_weights", json::array({1.5, nullptr, 4.0})}}).empty());
    CHECK(parse_filament_weights({{"filament_used", json::array({"12.5", "abc"})}}).empty());
    CHECK(parse_filament_weights({{"filament_used", json::array({"12.5", "inf"})}}).empty());
}

TEST_CASE("parse_filament_weights: any unparseable string token makes the result unknown",
          "[moonraker][metadata][weights][json_coercion]") {
    CHECK(parse_filament_weights({{"filament_used", "1.5,xyz,4.0"}}).empty());
    CHECK(parse_filament_weights({{"filament_used", "1.5,,4.0"}}).empty());
}

TEST_CASE("parse_filament_weights: null filament_weights is treated as missing",
          "[moonraker][metadata][weights]") {
    // L087 guard: nlohmann::json null is not an array; parser must not throw.
    json obj = {{"filament_weights", nullptr}};
    auto w = parse_filament_weights(obj);

    CHECK(w.empty());
}
