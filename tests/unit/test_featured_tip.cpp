// SPDX-License-Identifier: GPL-3.0-or-later

#include "featured_tip.h"

#include "hv/json.hpp"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("featured_tip: parses the record a chat bot writes", "[featured_tip]") {
    const auto value = nlohmann::json::parse(R"({
        "id": "a1b2", "title": "Two prints at the same time man.",
        "details": "Lawrence in Office Space (1999).", "source": "Print Handy",
        "posted_at": 1763400000.5})");
    const auto tip = featured_tip::parse(value);
    REQUIRE(tip);
    CHECK(tip->id == "a1b2");
    CHECK(tip->title == "Two prints at the same time man.");
    CHECK(tip->details == "Lawrence in Office Space (1999).");
    CHECK(tip->source == "Print Handy");
    CHECK(tip->posted_at == Catch::Approx(1763400000.5));
}

TEST_CASE("featured_tip: a record without an id or a title is no tip", "[featured_tip]") {
    CHECK_FALSE(featured_tip::parse(nlohmann::json::parse(R"({"title": "x"})")));
    CHECK_FALSE(featured_tip::parse(nlohmann::json::parse(R"({"id": "x"})")));
    CHECK_FALSE(featured_tip::parse(nlohmann::json::parse(R"({"id": "x", "title": ""})")));
    CHECK_FALSE(featured_tip::parse(nlohmann::json::parse(R"({"id": 7, "title": "x"})")));
    CHECK_FALSE(featured_tip::parse(nlohmann::json::parse(R"("just a string")")));
    CHECK_FALSE(featured_tip::parse(nlohmann::json()));
}

TEST_CASE("featured_tip: optional fields default when missing or mistyped", "[featured_tip]") {
    const auto tip = featured_tip::parse(
        nlohmann::json::parse(R"({"id": "x", "title": "t", "details": 3, "posted_at": "soon"})"));
    REQUIRE(tip);
    CHECK(tip->details.empty());
    CHECK(tip->source.empty());
    CHECK(tip->posted_at == 0.0);
}

TEST_CASE("featured_tip: freshness", "[featured_tip]") {
    featured_tip::FeaturedTip tip{"x", "t", "", "", 1000.0};
    CHECK(featured_tip::is_fresh(tip, 1000.0 + featured_tip::kMaxAgeSeconds));
    CHECK_FALSE(featured_tip::is_fresh(tip, 1000.0 + featured_tip::kMaxAgeSeconds + 1.0));
    CHECK(featured_tip::is_fresh(tip, 900.0)); // writer's clock ahead of the screen's

    tip.posted_at = 0.0; // no time given
    CHECK(featured_tip::is_fresh(tip, 1e12));
}

TEST_CASE("featured_tip: lines replace the tips unless the tile says otherwise",
          "[featured_tip]") {
    CHECK(featured_tip::featured_only(nlohmann::json()));
    CHECK(featured_tip::featured_only(nlohmann::json::object()));
    CHECK(featured_tip::featured_only(nlohmann::json::parse(R"({"featured_only": true})")));
    CHECK_FALSE(featured_tip::featured_only(nlohmann::json::parse(R"({"featured_only": false})")));
    CHECK(featured_tip::featured_only(nlohmann::json::parse(R"({"featured_only": "no"})")));
}

TEST_CASE("featured_tip: replacing the tips, a line stays current for a week", "[featured_tip]") {
    CHECK(featured_tip::max_age_seconds(false) == featured_tip::kMaxAgeSeconds);
    CHECK(featured_tip::max_age_seconds(true) == featured_tip::kReplaceMaxAgeSeconds);
    CHECK(featured_tip::kReplaceMaxAgeSeconds == 7.0 * 24 * 3600);

    const featured_tip::FeaturedTip tip{"x", "t", "", "", 1000.0};
    const double week = featured_tip::max_age_seconds(true);
    CHECK(featured_tip::is_fresh(tip, 1000.0 + 3600.0, week)); // an hour on: mixed would skip it
    CHECK_FALSE(featured_tip::is_fresh(tip, 1000.0 + 3600.0));
    CHECK(featured_tip::is_fresh(tip, 1000.0 + week, week));
    CHECK_FALSE(featured_tip::is_fresh(tip, 1000.0 + week + 1.0, week));
}

TEST_CASE("featured_tip: the tile shows the line, the tap shows it with its details",
          "[featured_tip]") {
    const featured_tip::FeaturedTip tip{"a1", "These go to eleven.", "This Is Spinal Tap (1984).",
                                        "Print Handy", 1.0};
    const PrintingTip out = featured_tip::to_printing_tip(tip);
    CHECK(out.title == "These go to eleven.");
    CHECK(out.heading == "Print Handy");
    CHECK(out.content == "These go to eleven.\n\nThis Is Spinal Tap (1984).");
    CHECK(out.id == "featured:a1");

    const PrintingTip bare = featured_tip::to_printing_tip({"b", "Just the joke.", "", "", 0.0});
    CHECK(bare.content == "Just the joke.");
    CHECK(bare.heading.empty()); // the dialog falls back to the title
}
