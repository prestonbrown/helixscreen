// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The multi-unit AMS screen's page model: hub grouping, the same-hub counts on
// each page and the page to open on.

#include "ams_unit_pages.h"

#include <set>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

AmsSystemInfo make_info(const std::vector<std::string>& hubs, int slots_per_unit = 4) {
    AmsSystemInfo info;
    int first = 0;
    for (size_t i = 0; i < hubs.size(); ++i) {
        AmsUnit u;
        u.unit_index = static_cast<int>(i);
        u.hub_id = hubs[i];
        u.slot_count = slots_per_unit;
        u.first_slot_global_index = first;
        first += slots_per_unit;
        info.units.push_back(u);
    }
    info.total_slots = first;
    return info;
}

std::vector<int> unit_order(const std::vector<UnitPage>& pages) {
    std::vector<int> out;
    for (const auto& p : pages)
        out.push_back(p.unit_index);
    return out;
}

const auto never = [](int) { return false; };

} // namespace

TEST_CASE("unit pages group interleaved hubs into consecutive pages", "[ams][pages]") {
    auto info = make_info({"fps", "fps2", "fps", "fps2", "fps"});
    auto pages = build_unit_pages(info, never);
    CHECK(unit_order(pages) == std::vector<int>{0, 2, 4, 1, 3});
    CHECK(pages[0].group == 0);
    CHECK(pages[2].group == 0);
    CHECK(pages[3].group == 1);
    CHECK(pages[4].group == 1);
}

TEST_CASE("unit pages give an empty hub_id a group of its own", "[ams][pages]") {
    auto info = make_info({"", "", "fps", ""});
    auto pages = build_unit_pages(info, never);
    REQUIRE(pages.size() == 4);
    CHECK(unit_order(pages) == std::vector<int>{0, 1, 2, 3});
    CHECK(pages[0].group == 0);
    CHECK(pages[1].group == 1);
    CHECK(pages[2].group == 2);
    CHECK(pages[3].group == 3);
    for (const auto& p : pages) {
        CHECK(p.same_hub_before == 0);
        CHECK(p.same_hub_after == 0);
    }
}

TEST_CASE("unit pages skip absent units", "[ams][pages]") {
    auto info = make_info({"fps", "fps", "fps"});
    info.units[1].absent = true;
    auto pages = build_unit_pages(info, never);
    CHECK(unit_order(pages) == std::vector<int>{0, 2});
    CHECK(pages[0].same_hub_after == 1);
    CHECK(pages[1].same_hub_before == 1);
}

TEST_CASE("unit pages count same-hub units before and after", "[ams][pages]") {
    auto info = make_info({"a", "a", "a", "b"});
    auto pages = build_unit_pages(info, never);
    REQUIRE(pages.size() == 4);
    CHECK(pages[0].same_hub_before == 0);
    CHECK(pages[0].same_hub_after == 2);
    CHECK(pages[1].same_hub_before == 1);
    CHECK(pages[1].same_hub_after == 1);
    CHECK(pages[2].same_hub_before == 2);
    CHECK(pages[2].same_hub_after == 0);
    CHECK(pages[3].same_hub_before == 0);
    CHECK(pages[3].same_hub_after == 0);
}

TEST_CASE("unit pages report drying only for the same hub and the right side", "[ams][pages]") {
    auto info = make_info({"a", "b", "a", "a"});
    // Unit 2 (hub a, third member... second of a) dries; unit 1 (hub b) dries too.
    auto pages = build_unit_pages(info, [](int u) { return u == 2 || u == 1; });
    REQUIRE(unit_order(pages) == std::vector<int>{0, 2, 3, 1});
    CHECK_FALSE(pages[0].drying_before);
    CHECK(pages[0].drying_after);
    CHECK_FALSE(pages[1].drying_before);
    CHECK_FALSE(pages[1].drying_after);
    CHECK(pages[2].drying_before);
    CHECK_FALSE(pages[2].drying_after);
    // Hub b's single unit sees nothing on either side: its own drying is not "other".
    CHECK_FALSE(pages[3].drying_before);
    CHECK_FALSE(pages[3].drying_after);
}

TEST_CASE("initial unit page follows the active slot", "[ams][pages]") {
    auto info = make_info({"a", "b", "a"});
    auto pages = build_unit_pages(info, never);
    REQUIRE(unit_order(pages) == std::vector<int>{0, 2, 1});

    info.current_slot = -1;
    CHECK(initial_unit_page(pages, info) == 0);

    info.current_slot = 9; // unit 2
    CHECK(initial_unit_page(pages, info) == 1);

    info.current_slot = 5; // unit 1
    CHECK(initial_unit_page(pages, info) == 2);

    info.current_slot = 99; // no unit holds it
    CHECK(initial_unit_page(pages, info) == 0);

    CHECK(initial_unit_page({}, info) == -1);
}

TEST_CASE("page_of_unit finds a unit's page or -1", "[ams][pages]") {
    auto info = make_info({"a", "b", "a"});
    info.units[1].absent = true;
    auto pages = build_unit_pages(info, never);
    CHECK(page_of_unit(pages, 2) == 1);
    CHECK(page_of_unit(pages, 1) == -1);
    CHECK(page_of_unit(pages, 7) == -1);
}

TEST_CASE("unit pages cover twelve units across two hubs", "[ams][pages]") {
    std::vector<std::string> hubs;
    for (int i = 0; i < 12; ++i)
        hubs.push_back(i < 10 ? "fps" : "fps2");
    auto info = make_info(hubs);
    auto pages = build_unit_pages(info, [](int u) { return u == 5; });
    REQUIRE(pages.size() == 12);
    std::set<int> seen;
    for (size_t i = 0; i < pages.size(); ++i) {
        CHECK(pages[i].unit_index == static_cast<int>(i));
        seen.insert(pages[i].unit_index);
    }
    CHECK(seen.size() == 12);
    CHECK(pages[0].same_hub_after == 9);
    CHECK(pages[0].drying_after);
    CHECK(pages[5].same_hub_before == 5);
    CHECK(pages[5].same_hub_after == 4);
    CHECK_FALSE(pages[5].drying_before);
    CHECK_FALSE(pages[5].drying_after);
    CHECK(pages[9].drying_before);
    CHECK(pages[10].group == 1);
    CHECK(pages[10].same_hub_after == 1);
    CHECK_FALSE(pages[10].drying_before);
    CHECK(pages[11].same_hub_before == 1);
}
