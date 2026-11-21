// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spool_assign.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::spool_assign;

namespace {

SpoolInfo spool(int id, const char* vendor, const char* material, const char* name,
                const char* hex) {
    SpoolInfo s;
    s.id = id;
    s.vendor = vendor;
    s.material = material;
    s.filament_name = name;
    s.color_hex = hex;
    return s;
}

std::vector<int> ids_of(const std::vector<RankedSpool>& ranked,
                        const std::vector<SpoolInfo>& spools) {
    std::vector<int> out;
    for (const auto& r : ranked) {
        out.push_back(spools[r.index].id);
    }
    return out;
}

} // namespace

TEST_CASE("an empty form lists every spool in recency order", "[spool-assign]") {
    const std::vector<SpoolInfo> spools = {
        spool(7, "Polymaker", "PLA", "PolyTerra Charcoal", "#333333"),
        spool(3, "eSUN", "PETG", "PETG Blue", "#1E4FD8"),
    };
    SpoolQuery q;
    q.brand = "Generic"; // the brand dropdown's unset value
    REQUIRE(q.empty());
    CHECK(ids_of(rank_spools(q, spools), spools) == std::vector<int>{7, 3});
}

TEST_CASE("exact matches come before close ones, each kept in recency order", "[spool-assign]") {
    const std::vector<SpoolInfo> spools = {
        spool(1, "Polymaker", "PLA+", "", "#FFFFFF"), // close: material
        spool(2, "Polymaker", "PLA", "", "#FFFFFF"),  // exact
        spool(3, "Polymaker", "PETG", "", "#FFFFFF"), // different polymer
        spool(4, "polymaker", "pla", "", "#FBFBFB"),  // exact: case, near-identical hex
        spool(5, "Polymaker", "PLA", "", "#E8E8E8"),  // close: color
        spool(6, "Polymaker", "PLA", "", "#1A1A1A"),  // black is not white
    };
    SpoolQuery q;
    q.material = "PLA";
    q.brand = "Polymaker";
    q.color = 0xFFFFFF;
    const auto ranked = rank_spools(q, spools);
    CHECK(ids_of(ranked, spools) == std::vector<int>{2, 4, 1, 5});
    REQUIRE(ranked.size() == 4);
    CHECK(ranked[0].match.kind == MatchKind::Exact);
    CHECK(ranked[2].match.kind == MatchKind::Close);
    CHECK(ranked[2].match.differs.material);
    CHECK_FALSE(ranked[2].match.differs.color);
    CHECK(ranked[3].match.differs.color);
    CHECK_FALSE(ranked[3].match.differs.material);
}

TEST_CASE("color matching goes by perceived distance on the hex, not the name", "[spool-assign]") {
    SpoolQuery q;
    q.color = 0x1E4FD8; // a blue
    // Named "Red" but the hex is the same blue: matches exactly.
    CHECK(match_spool(q, spool(1, "", "", "Red", "#1E4FD8")).kind == MatchKind::Exact);
    // A slightly lighter blue is close.
    CHECK(match_spool(q, spool(2, "", "", "", "#3A66E8")).kind == MatchKind::Close);
    // Orange is no match at all.
    CHECK(match_spool(q, spool(3, "", "", "", "#FF9800")).kind == MatchKind::None);
    // A spool with no colour cannot satisfy a form that sets one.
    CHECK(match_spool(q, spool(4, "", "", "", "")).kind == MatchKind::None);
}

TEST_CASE("brand and product compare loosely: case and whole extra words", "[spool-assign]") {
    SpoolQuery q;
    q.brand = "Polymaker";
    q.product = "White";
    const SpoolMatch polar = match_spool(q, spool(1, "POLYMAKER", "PLA", "Polar White", ""));
    CHECK(polar.kind == MatchKind::Close);
    CHECK(polar.differs.product);
    CHECK_FALSE(polar.differs.brand);
    // A word that merely starts the same is a different product.
    CHECK(match_spool(q, spool(2, "Polymaker", "PLA", "Whitesmoke", "")).kind == MatchKind::None);
    CHECK(match_spool(q, spool(3, "Prusament", "PLA", "White", "")).kind == MatchKind::None);
    CHECK(match_spool(q, spool(4, "Polymaker", "PLA", "white", "")).kind == MatchKind::Exact);
}

TEST_CASE("close_note names what the spool says differently", "[spool-assign]") {
    CloseFields differs;
    CHECK(close_note(differs, SpoolInfo{}, "color").empty());
    differs.material = true;
    differs.color = true;
    CHECK(close_note(differs, spool(1, "eSUN", "PLA+", "", "#EEEEEE"), "color") ==
          "close: PLA+, color");
    CloseFields only_product;
    only_product.product = true;
    CHECK(close_note(only_product, spool(1, "", "", "Polar White", ""), "color") ==
          "close: Polar White");
}

TEST_CASE("common spools: four materials by three colours, until a few spools exist",
          "[spool-assign]") {
    const auto& common = common_spools();
    REQUIRE(common.size() == 12);
    CHECK(std::string(common[0].material) == "PLA");
    CHECK(common[0].rgb == 0x1A1A1A);
    CHECK(std::string(common[4].material) == "PETG");
    CHECK(std::string(common[4].color_name) == "White");
    CHECK(std::string(common[11].material) == "ASA");
    CHECK(common[11].rgb == 0x7F7F7F);
    CHECK(show_common_spools(0));
    CHECK(show_common_spools(2));
    CHECK_FALSE(show_common_spools(3));
}

TEST_CASE("a lane with a spool opens to Edit, an empty one to Assign", "[spool-assign]") {
    SlotInfo empty;
    CHECK(screen_for_lane(empty) == LaneScreen::Assign);
    SlotInfo described;
    described.material = "PETG";
    CHECK(screen_for_lane(described) == LaneScreen::Edit);
    SlotInfo linked;
    linked.spoolman_id = 12;
    CHECK(screen_for_lane(linked) == LaneScreen::Edit);
}

TEST_CASE("bottom action: Add New while spools are listed, Add & Assign when none, Assign "
          "without Spoolman",
          "[spool-assign]") {
    CHECK(bottom_action(true, 4) == BottomAction::AddNewSecondary);
    CHECK(bottom_action(true, 0) == BottomAction::AddToSpoolmanAndUse);
    CHECK(bottom_action(false, 0) == BottomAction::AssignLocal);
    CHECK(bottom_action(false, 5) == BottomAction::AssignLocal);
    CHECK(offers_local_assign(BottomAction::AddToSpoolmanAndUse));
    CHECK_FALSE(offers_local_assign(BottomAction::AddNewSecondary));
    CHECK_FALSE(offers_local_assign(BottomAction::AssignLocal));
}

TEST_CASE("size chips light for a full spool of their size, else Partial", "[spool-assign]") {
    CHECK(size_chip_for(1000, 1000) == 0);
    CHECK(size_chip_for(750, 750) == 1);
    CHECK(size_chip_for(250, 250) == 2);
    CHECK(size_chip_for(400, 1000) == kPartialChip);
    CHECK(size_chip_for(500, 500) == kPartialChip);
}

TEST_CASE("remaining steps stay between empty and the spool's total", "[spool-assign]") {
    CHECK(step_remaining(500, 10, 1000) == 510);
    CHECK(step_remaining(995, 10, 1000) == 1000);
    CHECK(step_remaining(4, -10, 1000) == 0);
    CHECK(step_remaining(1200, 10, 0) == 1210); // no known total, no ceiling
}

TEST_CASE("a new spool with no weight starts as a full 1 kg spool", "[spool-assign]") {
    SlotInfo fresh;
    fresh.total_weight_g = 0;
    fresh.remaining_weight_g = -1;
    apply_new_spool_default_weight(fresh);
    CHECK(fresh.total_weight_g == 1000);
    CHECK(fresh.remaining_weight_g == 1000);

    SlotInfo weighed;
    weighed.total_weight_g = 750;
    weighed.remaining_weight_g = 300;
    apply_new_spool_default_weight(weighed);
    CHECK(weighed.total_weight_g == 750);
    CHECK(weighed.remaining_weight_g == 300);
}
