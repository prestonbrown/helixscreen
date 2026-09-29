// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// select_thumbnail() is the one rule every surface uses to pick which of a
// file's thumbnails to fetch: the print-select grid and detail view (through
// FileMetadata), the home card and the history parse (through PrintHistoryJob).

#include "moonraker_types.h"
#include "print_history_parse.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

ThumbnailInfo thumb(const char* path, int w, int h) {
    ThumbnailInfo t;
    t.relative_path = path;
    t.width = w;
    t.height = h;
    return t;
}

std::string pick(const std::vector<ThumbnailInfo>& thumbs, int w, int h) {
    const ThumbnailInfo* t = select_thumbnail(thumbs, w, h);
    return t ? t->relative_path : "(none)";
}

const std::vector<ThumbnailInfo> kSlicer = {thumb("32.png", 32, 32), thumb("300.png", 300, 300),
                                            thumb("160.png", 160, 160)};

} // namespace

TEST_CASE("select_thumbnail: the smallest entry that covers the box",
          "[thumbnail][select_thumbnail]") {
    CHECK(pick(kSlicer, 100, 100) == "160.png");
    CHECK(pick(kSlicer, 200, 120) == "300.png");
    CHECK(pick(kSlicer, 20, 20) == "32.png");
}

TEST_CASE("select_thumbnail: an exact fit is covered", "[thumbnail][select_thumbnail]") {
    CHECK(pick(kSlicer, 160, 160) == "160.png");
    CHECK(pick(kSlicer, 300, 300) == "300.png");
}

TEST_CASE("select_thumbnail: nothing big enough takes the largest",
          "[thumbnail][select_thumbnail]") {
    CHECK(pick(kSlicer, 400, 400) == "300.png");
    // Covering one axis is not covering the box.
    CHECK(pick(kSlicer, 301, 10) == "300.png");
}

TEST_CASE("select_thumbnail: an unmeasured box takes the largest, not the smallest icon",
          "[thumbnail][select_thumbnail]") {
    CHECK(pick(kSlicer, 0, 0) == "300.png");
    CHECK(pick(kSlicer, 0, 100) == "300.png");
    CHECK(pick(kSlicer, -1, -1) == "300.png");
}

TEST_CASE("select_thumbnail: a QIDI .gcode.3mf single plate is chosen at any size",
          "[thumbnail][select_thumbnail]") {
    const std::vector<ThumbnailInfo> plate = {
        thumb(".thumbs/Slipper (PETG).gcode/plate_1.png", 512, 512)};
    CHECK(pick(plate, 108, 160) == ".thumbs/Slipper (PETG).gcode/plate_1.png");
    CHECK(pick(plate, 900, 900) == ".thumbs/Slipper (PETG).gcode/plate_1.png");
    CHECK(pick(plate, 0, 0) == ".thumbs/Slipper (PETG).gcode/plate_1.png");
}

TEST_CASE("select_thumbnail: an empty list selects nothing", "[thumbnail][select_thumbnail]") {
    CHECK(select_thumbnail({}, 100, 100) == nullptr);
    CHECK(select_thumbnail({}, 0, 0) == nullptr);
}

TEST_CASE("select_thumbnail: ties keep the first listed", "[thumbnail][select_thumbnail]") {
    const std::vector<ThumbnailInfo> twins = {thumb("a.png", 200, 200), thumb("b.png", 200, 200)};
    CHECK(pick(twins, 100, 100) == "a.png");
    CHECK(pick(twins, 0, 0) == "a.png");
}

TEST_CASE("select_thumbnail: entries with no recorded size still get chosen",
          "[thumbnail][select_thumbnail]") {
    const std::vector<ThumbnailInfo> unsized = {thumb("only.png", 0, 0)};
    CHECK(pick(unsized, 100, 100) == "only.png");
}

TEST_CASE("history parse pre-selects the largest thumbnail, sized or not",
          "[thumbnail][select_thumbnail][history]") {
    auto job_with = [](nlohmann::json thumbs) {
        return helix::parse_history_job(
            nlohmann::json{{"job_id", "000001"},
                           {"filename", "a.gcode"},
                           {"status", "completed"},
                           {"metadata", {{"thumbnails", std::move(thumbs)}}}});
    };
    const auto sized = job_with(nlohmann::json::array(
        {{{"relative_path", ".thumbs/a-32x32.png"}, {"width", 32}, {"height", 32}},
         {{"relative_path", ".thumbs/a-300x300.png"}, {"width", 300}, {"height", 300}}}));
    CHECK(sized.thumbnail_path == ".thumbs/a-300x300.png");

    // Slicers that omit the dimensions still name a thumbnail worth showing.
    const auto unsized = job_with(nlohmann::json::array({{{"relative_path", ".thumbs/a.png"}}}));
    CHECK(unsized.thumbnail_path == ".thumbs/a.png");
}
