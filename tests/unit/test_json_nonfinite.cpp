// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "json_nonfinite.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::json_util::nonfinite_literals_to_null;

TEST_CASE("nonfinite_literals_to_null rewrites bare non-finite numbers", "[json_nonfinite]") {
    std::string out;
    REQUIRE(nonfinite_literals_to_null(R"({"a": NaN, "b": [Infinity, -Infinity, 1.5]})", out));
    CHECK(out == R"({"a": null, "b": [null, null, 1.5]})");
}

TEST_CASE("nonfinite_literals_to_null leaves strings alone", "[json_nonfinite]") {
    std::string out;
    CHECK_FALSE(nonfinite_literals_to_null(R"({"NaN": "Infinity -Infinity NaN"})", out));
    REQUIRE(nonfinite_literals_to_null(R"({"s": "a \"NaN\" b", "v": NaN})", out));
    CHECK(out == R"({"s": "a \"NaN\" b", "v": null})");
}

TEST_CASE("nonfinite_literals_to_null reports nothing to do on plain JSON", "[json_nonfinite]") {
    std::string out;
    CHECK_FALSE(nonfinite_literals_to_null(R"({"t": 21.5, "ok": true, "x": null})", out));
    CHECK_FALSE(nonfinite_literals_to_null("", out));
}
