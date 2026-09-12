// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_error_helper.cpp
 * @brief AmsErrorHelper is the one place every AMS backend spells a gcode tool
 * number and a physical position for the user.
 */

#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_types.h"
#include "display_numbering.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("tool_out_of_range names the tool as a gcode T-number", "[ams][numbering]") {
    const auto err = AmsErrorHelper::tool_out_of_range(7);
    CHECK(err.result == AmsResult::INVALID_TOOL);
    CHECK(err.technical_msg == "Tool T7 out of range");
    CHECK(err.user_msg == "Invalid tool number");
    CHECK(err.suggestion == "Select a valid tool");
}

TEST_CASE("tool_out_of_range matches tool_label for every non-negative tool", "[ams][numbering]") {
    for (int tool : {0, 1, 63}) {
        const auto err = AmsErrorHelper::tool_out_of_range(tool);
        CHECK(err.technical_msg == "Tool " + helix::ui::tool_label(tool) + " out of range");
    }
}

TEST_CASE("tool_out_of_range still names a negative tool in the technical detail",
          "[ams][numbering]") {
    // tool_label(-1) is empty (helix::ui has no T<n> spelling for a negative
    // index), and a backend does call this helper with one: both do_change_tool
    // and set_tool_mapping_impl guard on "< 0 || >= max" and pass tool_number
    // straight through on either side of that OR. The technical detail is for
    // logs, so it must still carry the value that was rejected.
    const auto err = AmsErrorHelper::tool_out_of_range(-1);
    CHECK(err.technical_msg == "Tool -1 out of range");
}

// --- Positions. A toast names the backend's own word and the number printed on
// the machine; the technical detail keeps the storage index a log reader wants.

TEST_CASE("a position error names the backend's noun, 1-based", "[ams][numbering]") {
    CHECK(AmsErrorHelper::slot_not_available(ui::LaneNoun::Gate, 2).user_msg == "Gate 3 is empty");
    CHECK(AmsErrorHelper::slot_blocked(ui::LaneNoun::Lane, 0).user_msg == "Lane 1 blocked");
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Feeder, 7, 3).user_msg ==
          "Feeder 8 does not exist");
    CHECK(AmsErrorHelper::load_failed(ui::LaneNoun::Slot, 1).user_msg ==
          "Failed to load filament from Slot 2");
}

TEST_CASE("a position error keeps the raw index in the technical detail", "[ams][numbering]") {
    CHECK(AmsErrorHelper::slot_not_available(ui::LaneNoun::Gate, 2).technical_msg ==
          "Slot 2 has no filament loaded");
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Gate, 7, 3).technical_msg ==
          "Slot 7 out of range (0-3)");
}

TEST_CASE("an out-of-range position suggests a 1-based span", "[ams][numbering]") {
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Lane, 9, 3).suggestion ==
          "Choose one between 1 and 4");
    // A backend that has not reported its positions yet has no span to offer.
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Lane, 0, -1).suggestion ==
          "Wait for the filament system to report its positions");
}

TEST_CASE("a position error still names a sentinel index", "[ams][numbering]") {
    // lane_label() has no spelling for a negative index, and backends do pass
    // one: every guard here reads "< 0 || >= max" and hands the value through
    // on either side of that OR.
    CHECK(AmsErrorHelper::invalid_slot(ui::LaneNoun::Gate, -1, 3).user_msg ==
          "Gate -1 does not exist");
}

TEST_CASE("Happy Hare reports an out-of-range gate as a gate", "[ams][numbering]") {
    // The noun reaches the toast from the backend rather than from a default.
    // A backend that never started has no gates, so any index is out of range.
    helix::AmsBackendHappyHare backend(nullptr, nullptr);
    REQUIRE(backend.lane_noun() == ui::LaneNoun::Gate);

    const auto err = backend.set_slot_info(2, helix::SlotInfo{}, /*persist=*/false);
    CHECK(err.result == AmsResult::INVALID_SLOT);
    CHECK(err.user_msg == "Gate 3 does not exist");
}

TEST_CASE("the mock backend reports a bad index without deadlocking", "[ams][numbering]") {
    // AmsBackendMock is the one backend whose noun comes from state mutex_
    // protects, and every operation already holds mutex_ when it rejects an
    // index. mutex_ is not recursive, so this call returning at all is half the
    // assertion.
    AmsBackendMock mock(4);
    REQUIRE(mock.get_type() == AmsType::HAPPY_HARE); // the mock's default persona
    const auto err = mock.set_slot_info(9, helix::SlotInfo{}, /*persist=*/false);
    CHECK(err.result == AmsResult::INVALID_SLOT);
    CHECK(err.user_msg == "Gate 10 does not exist");
}
