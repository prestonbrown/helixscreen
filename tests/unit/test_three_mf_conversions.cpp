// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "three_mf_conversions.h"

#include <map>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::three_mf::Conversion;
using helix::three_mf::converted_archives;
using helix::three_mf::ListedFile;
using helix::three_mf::parse_conversions;

namespace {

nlohmann::json one(const char* state) {
    return {{"source", "dir/X.gcode.3mf"},
            {"source_mtime", 1760000000.5},
            {"source_size", 2139171},
            {"plate", 9},
            {"plate_md5", "09c7"},
            {"output", "dir/X.gcode"},
            {"output_md5", "09c7"},
            {"state", state},
            {"error", ""}};
}

std::vector<ListedFile> listing() {
    return {{"dir/X.gcode.3mf", 2139171, 1760000000.5}, {"dir/X.gcode", 8117702, 1760000100.0}};
}

} // namespace

TEST_CASE("parse_conversions reads every field and skips malformed entries",
          "[three_mf][conversions]") {
    nlohmann::json result = {{"conversions", {one("done"), 42, {{"state", "done"}}}}};
    const auto parsed = parse_conversions(result);
    REQUIRE(parsed.size() == 1);
    const Conversion& c = parsed[0];
    CHECK(c.source == "dir/X.gcode.3mf");
    CHECK(c.source_mtime == Catch::Approx(1760000000.5));
    CHECK(c.source_size == 2139171);
    CHECK(c.plate == 9);
    CHECK(c.output == "dir/X.gcode");
    CHECK(c.state == "done");

    CHECK(parse_conversions(nlohmann::json::array()).empty());
    CHECK(parse_conversions({{"conversions", "nope"}}).empty());
    CHECK(parse_conversions(nlohmann::json()).empty());
}

TEST_CASE("an archive hides only behind a finished conversion of exactly itself",
          "[three_mf][conversions]") {
    const auto done = parse_conversions({{"conversions", {one("done")}}});
    REQUIRE(done.size() == 1);
    CHECK(converted_archives(done, listing()) == std::set<std::string>{"dir/X.gcode.3mf"});

    SECTION("pending or failed") {
        CHECK(converted_archives(parse_conversions({{"conversions", {one("pending")}}}), listing())
                  .empty());
        CHECK(converted_archives(parse_conversions({{"conversions", {one("failed")}}}), listing())
                  .empty());
    }
    SECTION("the archive was replaced since: another size") {
        auto files = listing();
        files[0].size += 1;
        CHECK(converted_archives(done, files).empty());
    }
    SECTION("the archive was replaced since: another time") {
        auto files = listing();
        files[0].modified += 2.0;
        CHECK(converted_archives(done, files).empty());
    }
    SECTION("a converter rounding the time to the hundredth still matches") {
        auto files = listing();
        files[0].modified += 0.004;
        CHECK(converted_archives(done, files).size() == 1);
    }
    SECTION("the output is gone") {
        auto files = listing();
        files.pop_back();
        CHECK(converted_archives(done, files).empty());
    }
    SECTION("the archive is not in this listing") {
        CHECK(converted_archives(done, {{"dir/X.gcode", 1, 1}}).empty());
    }
    SECTION("no converter answered") {
        CHECK(converted_archives({}, listing()).empty());
    }
}

TEST_CASE("an archive with several plates hides only when every plate is converted",
          "[three_mf][conversions]") {
    auto plate = [](int n, const char* state, const char* output) {
        nlohmann::json e = one(state);
        e["plate"] = n;
        e["output"] = output;
        return e;
    };
    auto files = listing();
    files.push_back({"dir/X_plate5.gcode", 10, 1760000200.0});
    const auto both_done = parse_conversions(
        {{"conversions",
          {plate(2, "done", "dir/X.gcode"), plate(5, "done", "dir/X_plate5.gcode")}}});
    CHECK(converted_archives(both_done, files).size() == 1);

    const auto one_pending = parse_conversions(
        {{"conversions",
          {plate(2, "done", "dir/X.gcode"), plate(5, "pending", "dir/X_plate5.gcode")}}});
    CHECK(converted_archives(one_pending, files).empty());
    // The finished plate still prints from its output; the pending one does not.
    const auto outputs = helix::three_mf::current_outputs(one_pending, files, "dir/X.gcode.3mf");
    CHECK(outputs == std::map<int, std::string>{{2, "dir/X.gcode"}});
    CHECK(helix::three_mf::current_outputs(one_pending, files, "dir/Other.gcode.3mf").empty());

    // A project with nothing sliced: one failed entry, plate 0, no output.
    const auto project = parse_conversions({{"conversions", {plate(0, "failed", "")}}});
    CHECK(converted_archives(project, files).empty());
    CHECK(helix::three_mf::current_outputs(project, files, "dir/X.gcode.3mf").empty());
}
