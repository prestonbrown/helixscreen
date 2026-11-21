// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lane_spool_history.h"
#include "moonraker_error.h"
#include "spool_assign.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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
    CHECK(close_note(differs, SpoolInfo{}, "close: {}", "similar color").empty());
    differs.material = true;
    differs.color = true;
    CHECK(close_note(differs, spool(1, "eSUN", "PLA+", "", "#EEEEEE"), "close: {}",
                     "similar color") == "close: PLA+, similar color");
    CloseFields only_product;
    only_product.product = true;
    CHECK(close_note(only_product, spool(1, "", "", "Polar White", ""), "ähnlich: {}",
                     "similar color") == "ähnlich: Polar White");
}

TEST_CASE("common spools: four materials by three colors, until a few spools exist",
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

TEST_CASE("lane history keeps the latest filament first, once, and caps its length",
          "[spool-assign]") {
    using helix::lane_spool_history::Entry;
    using helix::lane_spool_history::with_recent;
    const Entry pla{"PLA", "Polymaker", "", "", 0xFFFFFF, 1000};
    const Entry petg{"PETG", "eSUN", "", "", 0x1E4FD8, 1000};
    const Entry pla_again{"pla", "polymaker", "", "", 0xFFFFFF, 750};

    auto list = with_recent({}, pla);
    list = with_recent(list, petg);
    REQUIRE(list.size() == 2);
    CHECK(list[0].material == "PETG");

    list = with_recent(list, pla_again);
    REQUIRE(list.size() == 2);
    CHECK(list[0].material == "pla");
    CHECK(list[0].total_weight_g == 750);
    CHECK(list[1].material == "PETG");

    // Same material and brand in another color is another filament.
    const Entry pla_black{"PLA", "Polymaker", "", "", 0x1A1A1A, 1000};
    CHECK(with_recent(list, pla_black).size() == 3);

    std::vector<Entry> full;
    for (int i = 0; i < 12; ++i) {
        full.push_back({"PLA", "Brand" + std::to_string(i), "", "", 0, 0});
    }
    const auto capped = with_recent(full, petg);
    REQUIRE(capped.size() == 12);
    CHECK(capped.front().material == "PETG");
    CHECK(capped.back().brand == "Brand10");
}

TEST_CASE("lane history records only a lane holding a material", "[spool-assign]") {
    SlotInfo empty;
    CHECK_FALSE(helix::lane_spool_history::entry_from_slot(empty).has_value());
    SlotInfo lane;
    lane.material = "ASA";
    lane.brand = "Generic";
    lane.color_rgb = 0x7F7F7F;
    const auto entry = helix::lane_spool_history::entry_from_slot(lane);
    REQUIRE(entry.has_value());
    CHECK(entry->brand.empty());
    CHECK(entry->color_rgb == 0x7F7F7F);
}

TEST_CASE("lane history round-trips through its stored form, capped", "[spool-assign]") {
    using helix::lane_spool_history::Entry;
    std::vector<Entry> list = {{"PETG", "eSUN", "PETG+", "esun-petg", 0x1E4FD8, 1000},
                               {"PLA", "", "", "", 0xFFFFFF, 750}};
    const auto back =
        helix::lane_spool_history::from_json(helix::lane_spool_history::to_json(list));
    REQUIRE(back.size() == 2);
    CHECK(back[0].brand == "eSUN");
    CHECK(back[0].product_name == "PETG+");
    CHECK(back[0].color_rgb == 0x1E4FD8);
    CHECK(back[1].total_weight_g == 750);

    // Junk and material-less entries are skipped; nothing stored reads empty.
    nlohmann::json stored =
        nlohmann::json::array({"junk", {{"brand", "x"}}, {{"material", "ABS"}}});
    const auto parsed = helix::lane_spool_history::from_json(stored);
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].material == "ABS");
    CHECK(helix::lane_spool_history::from_json(nlohmann::json()).empty());

    nlohmann::json many = nlohmann::json::array();
    for (int i = 0; i < 20; ++i) {
        many.push_back({{"material", "PLA"}, {"brand", std::to_string(i)}});
    }
    CHECK(helix::lane_spool_history::from_json(many).size() ==
          helix::lane_spool_history::kMaxEntries);
}

TEST_CASE("a lane history record writes over only what its read saw", "[spool-assign]") {
    using helix::lane_spool_history::base_for_record;
    const nlohmann::json stored =
        nlohmann::json::array({{{"material", "PLA"}}, {{"material", "ABS"}}});
    const auto from_stored = base_for_record(&stored, nullptr);
    REQUIRE(from_stored.has_value());
    CHECK(from_stored->size() == 2);

    // Nothing stored yet: start a history.
    MoonrakerError not_found;
    not_found.type = MoonrakerErrorType::JSON_RPC_ERROR;
    not_found.code = 404;
    not_found.message = "Key 'lane_spool_history' in namespace 'helix-screen' not found";
    const auto fresh = base_for_record(nullptr, &not_found);
    REQUIRE(fresh.has_value());
    CHECK(fresh->empty());

    // A read that failed otherwise writes nothing, or it would wipe the history.
    MoonrakerError timeout;
    timeout.type = MoonrakerErrorType::TIMEOUT;
    timeout.message = "Request timed out";
    CHECK_FALSE(base_for_record(nullptr, &timeout).has_value());
}

TEST_CASE("Undo applies only while the lane holds what the save put in", "[spool-assign]") {
    SlotInfo after;
    after.spoolman_id = 22;
    after.material = "PETG";
    after.brand = "eSUN";
    after.color_rgb = 0x00FF00;
    CHECK(undo_still_applies(after, after));
    SlotInfo weighed = after;
    weighed.remaining_weight_g = 300; // printing used some: still the same spool
    CHECK(undo_still_applies(weighed, after));
    SlotInfo relinked = after;
    relinked.spoolman_id = 23;
    CHECK_FALSE(undo_still_applies(relinked, after));
    SlotInfo recolored = after;
    recolored.color_rgb = 0x123456;
    CHECK_FALSE(undo_still_applies(recolored, after));
    SlotInfo rematerialed = after;
    rematerialed.material = "PLA";
    CHECK_FALSE(undo_still_applies(rematerialed, after));
}

TEST_CASE("the spool list builds at most its row cap, best first", "[spool-assign]") {
    std::vector<SpoolInfo> many;
    for (int i = 0; i < 100; ++i) {
        many.push_back(spool(i + 1, "Polymaker", "PLA", "", "#FFFFFF"));
    }
    const auto ranked = rank_spools(SpoolQuery{}, many);
    REQUIRE(ranked.size() == kMaxListedSpools);
    CHECK(many[ranked.front().index].id == 1);
    CHECK(rank_spools(SpoolQuery{}, many, 5).size() == 5);
}

TEST_CASE("a multi-color spool matches by its first color", "[spool-assign]") {
    SpoolInfo rainbow = spool(9, "Polymaker", "PLA", "Rainbow", "");
    rainbow.multi_color_hexes = "#FF0000,#00FF00,#0000FF";
    SpoolQuery q;
    q.color = 0xFF0000;
    CHECK(match_spool(q, rainbow).kind == MatchKind::Exact);
    q.color = 0x0000FF;
    CHECK(match_spool(q, rainbow).kind == MatchKind::None);
}

TEST_CASE("a save that changed nothing on the lane leaves nothing to undo", "[spool-assign]") {
    SlotInfo lane;
    lane.spoolman_id = 7;
    lane.material = "PLA";
    lane.brand = "eSUN";
    lane.color_rgb = 0xFFFFFF;
    lane.remaining_weight_g = 600;
    lane.total_weight_g = 1000;
    CHECK_FALSE(save_changed_lane(lane, lane)); // notes, price, location, lot only
    SlotInfo weighed = lane;
    weighed.remaining_weight_g = 550;
    CHECK(save_changed_lane(lane, weighed));
    SlotInfo relinked = lane;
    relinked.spoolman_id = 8;
    CHECK(save_changed_lane(lane, relinked));
    SlotInfo product = lane;
    product.product_name = "PLA+ 2.0";
    CHECK(save_changed_lane(lane, product));
}
