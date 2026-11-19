// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_fonts.h"

#include "../lvgl_test_fixture.h"
#include "featured_tip.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

TEST_CASE("featured_tip: parses the record a chat bot writes", "[featured_tip]") {
    const auto value = nlohmann::json::parse(R"({
        "id": "a1b2", "title": "Two prints at the same time man.",
        "details": "Lawrence in Office Space (1999).", "source": "Print Handy",
        "posted_at": 1763400000.5})");
    const auto tip = featured_tip::parse_record(value);
    REQUIRE(tip);
    CHECK(tip->id == "a1b2");
    CHECK(tip->title == "Two prints at the same time man.");
    CHECK(tip->details == "Lawrence in Office Space (1999).");
    CHECK(tip->source == "Print Handy");
    CHECK(tip->posted_at == Catch::Approx(1763400000.5));
}

TEST_CASE("featured_tip: a record without an id or a title is no tip", "[featured_tip]") {
    CHECK_FALSE(featured_tip::parse_record(nlohmann::json::parse(R"({"title": "x"})")));
    CHECK_FALSE(featured_tip::parse_record(nlohmann::json::parse(R"({"id": "x"})")));
    CHECK_FALSE(featured_tip::parse_record(nlohmann::json::parse(R"({"id": "x", "title": ""})")));
    CHECK_FALSE(featured_tip::parse_record(nlohmann::json::parse(R"({"id": 7, "title": "x"})")));
    CHECK_FALSE(featured_tip::parse_record(nlohmann::json::parse(R"("just a string")")));
    CHECK_FALSE(featured_tip::parse_record(nlohmann::json()));
}

TEST_CASE("featured_tip: optional fields default when missing or mistyped", "[featured_tip]") {
    const auto tip = featured_tip::parse_record(
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

TEST_CASE("featured_tip: lines mix with the tips unless the tile opts into replacing them",
          "[featured_tip]") {
    CHECK_FALSE(featured_tip::featured_only(nlohmann::json()));
    CHECK_FALSE(featured_tip::featured_only(nlohmann::json::object()));
    CHECK(featured_tip::featured_only(nlohmann::json::parse(R"({"featured_only": true})")));
    CHECK_FALSE(featured_tip::featured_only(nlohmann::json::parse(R"({"featured_only": false})")));
    CHECK_FALSE(featured_tip::featured_only(nlohmann::json::parse(R"({"featured_only": "yes"})")));
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

TEST_CASE_METHOD(LVGLTestFixture,
                 "featured_tip: a line takes the largest face that holds all of it",
                 "[featured_tip]") {
    const std::vector<const lv_font_t*> faces = {&noto_sans_20, &noto_sans_16, &noto_sans_12};
    const int32_t line_16 = lv_font_get_line_height(&noto_sans_16);
    const std::string line = "These go to eleven.";

    SECTION("room for the largest keeps it") {
        CHECK(featured_tip::fit_font(line, faces, 400, 200, 0, 0) == &noto_sans_20);
    }
    SECTION("a face whose line is too tall steps down") {
        CHECK(featured_tip::fit_font(line, faces, 400, line_16, 0, 0) == &noto_sans_16);
    }
    SECTION("a face that wraps past the height steps down") {
        lv_point_t one_line;
        lv_text_get_size(&one_line, line.c_str(), &noto_sans_16, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        CHECK(featured_tip::fit_font(line, faces, one_line.x - 1, line_16, 0, 0) == &noto_sans_12);
    }
    SECTION("too long for every face ends in the smallest") {
        CHECK(featured_tip::fit_font(line, faces, 400, 1, 0, 0) == &noto_sans_12);
    }
}
