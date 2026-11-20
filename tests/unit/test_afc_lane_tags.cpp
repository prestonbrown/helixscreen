// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_lane_tags.cpp
 * @brief The RFID tags AFC DEV's ACE and CFS units read reach each lane's tag
 *        reading, beside the TD-1 color.
 */

#include "../lvgl_test_fixture.h"
#include "afc_unit_hardware.h"
#include "ams_backend_afc.h"
#include "test_helpers/registered_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {

class AfcTagHelper : public AmsBackendAfc {
  public:
    explicit AfcTagHelper(const std::vector<std::string>& names) : AmsBackendAfc(nullptr, nullptr) {
        // The unit objects build the slots from the discovered lanes.
        set_discovered_lanes(names, {});
    }

    void feed(const nlohmann::json& params) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }
};

} // namespace helix

namespace {

nlohmann::json ace_slot(int index, int rfid, const char* material, std::vector<int> rgb) {
    return {{"index", index},       {"status", "ready"}, {"rfid", rfid}, {"sku", ""},
            {"material", material}, {"uid", ""},         {"color", rgb}};
}

} // namespace

TEST_CASE("AFC unit object: ACE slot and CFS lane tags parse", "[afc][afc_tags]") {
    const auto ace = afc::parse_unit_hardware(
        {{"ace_slots", {ace_slot(1, 2, "PETG", {255, 0, 0}), ace_slot(0, 1, "PLA", {0, 0, 0})}}});
    REQUIRE(ace.ace.has_value());
    REQUIRE(ace.ace->slot_tags.has_value());
    REQUIRE(ace.ace->slot_tags->size() == 2);
    // Slot 0's read found no tag; slot 1's identified a red PETG.
    CHECK_FALSE((*ace.ace->slot_tags)[0].has_value());
    REQUIRE((*ace.ace->slot_tags)[1].has_value());
    CHECK((*ace.ace->slot_tags)[1]->material == std::optional<std::string>("PETG"));
    CHECK((*ace.ace->slot_tags)[1]->color_rgb == std::optional<uint32_t>(0xFF0000));

    const auto cfs = afc::parse_unit_hardware(
        {{"cfs",
          {{"online", true},
           {"lanes",
            {{"lane1",
              {{"slot", "A"},
               {"tag",
                {{"material", "PLA"}, {"color", "00FF00"}, {"weight", 1000}, {"spool_id", 12}}}}},
             {"lane2", {{"slot", "B"}, {"tag", nullptr}}}}}}}});
    REQUIRE(cfs.cfs_lane_tags.has_value());
    const auto& lane1 = cfs.cfs_lane_tags->at("lane1");
    REQUIRE(lane1.has_value());
    CHECK(lane1->material == std::optional<std::string>("PLA"));
    CHECK(lane1->color_rgb == std::optional<uint32_t>(0x00FF00));
    CHECK(lane1->total_weight_g == std::optional<float>(1000.0f));
    CHECK(lane1->spoolman_id == std::optional<int>(12));
    CHECK_FALSE(cfs.cfs_lane_tags->at("lane2").has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC ACE unit: a lane's tag is its slot's RFID read",
                 "[afc][afc_tags][ams]") {
    helix::test::RegisteredBackend<AfcTagHelper> reg{
        std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"}};
    AfcTagHelper& afc = *reg;
    // lane1 draws from ACE slot 2, lane2 from slot 1 (each lane's `lane` index).
    afc.feed({{"AFC", {{"units", {"ACE Ace_1"}}}},
              {"AFC_ACE Ace_1",
               {{"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                {"extruders", {"extruder"}},
                {"hubs", {"Ace_1"}},
                {"buffers", nlohmann::json::array()},
                {"ace_dryer", "stop"},
                {"ace_slots",
                 {ace_slot(0, 2, "PLA", {0, 0, 255}), ace_slot(1, 2, "ABS", {255, 255, 255}),
                  ace_slot(2, 3, "", {0, 0, 0}), ace_slot(3, 0, "", {0, 0, 0})}}}}});
    afc.feed({{"AFC_lane lane1", {{"lane", 2}}},
              {"AFC_lane lane2", {{"lane", 1}}},
              {"AFC_lane lane3", {{"lane", 3}}},
              {"AFC_lane lane4", {{"lane", 4}}}});

    // One unit: its lanes fill slots 0-3 in the unit's order.
    const auto tag1 = afc.tag_reading(0);
    REQUIRE(tag1.has_value());
    CHECK(tag1->material == std::optional<std::string>("ABS"));
    CHECK(tag1->color_rgb == std::optional<uint32_t>(0xFFFFFF));
    const auto tag2 = afc.tag_reading(1);
    REQUIRE(tag2.has_value());
    CHECK(tag2->material == std::optional<std::string>("PLA"));
    // A read still in progress is no reading.
    CHECK_FALSE(afc.tag_reading(2).has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC CFS unit: a lane's tag comes from cfs.lanes",
                 "[afc][afc_tags][ams]") {
    helix::test::RegisteredBackend<AfcTagHelper> reg{
        std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"}};
    AfcTagHelper& afc = *reg;
    const auto frame = [](const nlohmann::json& lane1_tag) {
        return nlohmann::json{
            {"AFC", {{"units", {"CFS cfs_1"}}}},
            {"AFC_CFS cfs_1",
             {{"lanes", {"lane1", "lane2", "lane3", "lane4"}},
              {"extruders", {"extruder"}},
              {"hubs", {"cfs_1"}},
              {"buffers", nlohmann::json::array()},
              {"cfs", {{"lanes", {{"lane1", {{"slot", "A"}, {"tag", lane1_tag}}}}}}}}}};
    };
    afc.feed(frame({{"material", "PETG"}, {"color", "112233"}, {"weight", 500}}));

    auto tag = afc.tag_reading(0);
    REQUIRE(tag.has_value());
    CHECK(tag->material == std::optional<std::string>("PETG"));
    CHECK(tag->color_rgb == std::optional<uint32_t>(0x112233));
    CHECK(tag->total_weight_g == std::optional<float>(500.0f));

    // The next read found no tag: the lane has no reading any more.
    afc.feed(frame(nullptr));
    CHECK_FALSE(afc.tag_reading(0).has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC CFS unit: the TD-1 color fills in where the tag states none",
                 "[afc][afc_tags][ams]") {
    helix::test::RegisteredBackend<AfcTagHelper> reg{
        std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"}};
    AfcTagHelper& afc = *reg;
    const auto frame = [](const nlohmann::json& lane1_tag) {
        return nlohmann::json{
            {"AFC", {{"units", {"CFS cfs_1"}}}},
            {"AFC_CFS cfs_1",
             {{"lanes", {"lane1", "lane2", "lane3", "lane4"}},
              {"extruders", {"extruder"}},
              {"hubs", {"cfs_1"}},
              {"buffers", nlohmann::json::array()},
              {"cfs", {{"lanes", {{"lane1", {{"slot", "A"}, {"tag", lane1_tag}}}}}}}}}};
    };
    afc.feed(frame({{"material", "PLA"}}));
    afc.feed({{"AFC_lane lane1", {{"td1_color", "FF8800"}}}});

    auto tag = afc.tag_reading(0);
    REQUIRE(tag.has_value());
    CHECK(tag->material == std::optional<std::string>("PLA"));
    CHECK(tag->color_rgb == std::optional<uint32_t>(0xFF8800));

    // A tag that states its colour keeps it over the scan.
    afc.feed(frame({{"material", "PLA"}, {"color", "112233"}}));
    tag = afc.tag_reading(0);
    REQUIRE(tag.has_value());
    CHECK(tag->color_rgb == std::optional<uint32_t>(0x112233));
}
