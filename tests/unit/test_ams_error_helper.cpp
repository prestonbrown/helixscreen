// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_error_helper.cpp
 * @brief AmsErrorHelper::tool_out_of_range() is the one place every AMS
 * backend reports an invalid gcode tool number.
 */

#include "ams_error.h"
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
