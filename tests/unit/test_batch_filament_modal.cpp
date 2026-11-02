// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_batch_filament_modal.cpp
 * @brief Pure-logic tests for the head picker modal
 *
 * Which rows it offers is decide_head_picker_rows() (test_filament_action_set.cpp);
 * these cases pin the text and the batch sift.
 */

#include "ui_batch_filament_modal.h"

#include "lvgl.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::BatchFilamentModal;

TEST_CASE("BatchFilamentModal selected keys convert to slot indices", "[ams][batch]") {
    SECTION("keys in row order become slots") {
        REQUIRE(BatchFilamentModal::selected_slots({"0", "2", "3"}) == std::vector<int>{0, 2, 3});
    }
    SECTION("single slot") {
        REQUIRE(BatchFilamentModal::selected_slots({"1"}) == std::vector<int>{1});
    }
    SECTION("empty selection stays empty") {
        REQUIRE(BatchFilamentModal::selected_slots({}).empty());
    }
}

TEST_CASE("BatchFilamentModal row label names the lane contents", "[ams][batch]") {
    using helix::SlotInfo;
    using helix::ui::LaneNoun;

    SECTION("a lane feeding the toolhead says loaded") {
        SlotInfo info;
        info.material = "PETG";
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Feeder, 0, info, true, true) ==
                std::string("Feeder 1 (PETG - ") + lv_tr("loaded") + ")");
    }
    SECTION("a full lane not yet fed says ready to load") {
        SlotInfo info;
        info.material = "PLA";
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Feeder, 1, info, true, false) ==
                std::string("Feeder 2 (PLA - ") + lv_tr("ready to load") + ")");
    }
    SECTION("a lane known to be empty says so, with no loaded/ready suffix") {
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Feeder, 2, SlotInfo{}, false, false) ==
                std::string("Feeder 3 (") + lv_tr("Empty") + ")");
    }
    SECTION("an unanswerable presence leaves the lane name bare") {
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Feeder, 3, SlotInfo{}, std::nullopt,
                                              false) == "Feeder 4");
    }
    SECTION("material wins over presence, so a stale nullopt cannot blank a loaded lane") {
        SlotInfo info;
        info.material = "PLA";
        REQUIRE(BatchFilamentModal::row_label(LaneNoun::Slot, 0, info, std::nullopt, true) ==
                std::string("Slot 1 (PLA - ") + lv_tr("loaded") + ")");
    }
}

TEST_CASE("BatchFilamentModal sifts eligible slots and names the first refusal", "[ams][batch]") {
    using E = helix::AmsBackend::FilamentOpEligibility;
    const std::vector<E> per_slot = {E::Eligible, E::Empty, E::Eligible, E::Busy};

    SECTION("eligible slots keep their order; the first refusal is named") {
        const auto sift = BatchFilamentModal::sift_eligible({0, 1, 2, 3}, per_slot);
        CHECK(sift.eligible == std::vector<int>{0, 2});
        CHECK(sift.dropped == 1);
        CHECK(sift.drop_reason == E::Empty);
    }
    SECTION("nothing selected that is refused: nothing dropped") {
        const auto sift = BatchFilamentModal::sift_eligible({0, 2}, per_slot);
        CHECK(sift.eligible == std::vector<int>{0, 2});
        CHECK(sift.dropped == -1);
    }
    SECTION("a slot past the eligibility table is dropped without reading it") {
        // total_slots can shrink between the picker opening and the press.
        const auto sift = BatchFilamentModal::sift_eligible({0, 4, 5}, per_slot);
        CHECK(sift.eligible == std::vector<int>{0});
        CHECK(sift.dropped == -1); // no reason exists for an undescribable slot
    }
}

TEST_CASE("Head picker row label names the head, its slot and why it is disabled", "[ams][batch]") {
    using helix::SlotInfo;
    using helix::ui::LaneNoun;
    BatchFilamentModal::HeadRowText text;
    text.head_noun = LaneNoun::Tool;
    text.slot_noun = LaneNoun::Slot;
    text.head = 1;
    text.slot = 4;

    SECTION("the slot is named only where asked") {
        text.show_slot = true;
        CHECK(BatchFilamentModal::head_row_label(text) == "Tool 2 \u00b7 Slot 5");
        text.show_slot = false;
        CHECK(BatchFilamentModal::head_row_label(text) == "Tool 2");
    }
    SECTION("contents follow, as on a lane row") {
        text.info.material = "PLA";
        text.at_toolhead = true;
        CHECK(BatchFilamentModal::head_row_label(text) ==
              std::string("Tool 2 (PLA - ") + lv_tr("loaded") + ")");
    }
    SECTION("a disabled row says why instead of what it holds") {
        text.info.material = "PLA";
        text.show_slot = true;
        text.reason = "Assign a slot first";
        CHECK(BatchFilamentModal::head_row_label(text) ==
              "Tool 2 \u00b7 Slot 5 (Assign a slot first)");
    }
    SECTION("a row naming no slot is the head alone") {
        text.slot = -1;
        text.show_slot = true;
        text.present = false;
        CHECK(BatchFilamentModal::head_row_label(text) == "Tool 2");
    }
}
