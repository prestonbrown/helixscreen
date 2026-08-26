// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// MacroButtonsOverlay::quick_button_index_to_slot_name() — the dropdown-index
// to slot-name mapping behind the two quick-button rows.
//
// This replaces a block of tests in the former test_settings_macro_buttons_char.cpp,
// which declared its own copy of the mapping and asserted against the copy. The
// copy could not fail when the real function changed, and several of its
// assertions compared a string literal to itself.
//
// The mapping is off-by-one on purpose: dropdown index 0 is the "Empty" entry,
// so index N names StandardMacros slot N-1.

#include "../test_helpers/macro_buttons_test_access.h"
#include "standard_macros.h"

#include <set>

#include "../catch_amalgamated.hpp"

TEST_CASE("quick_button_index_to_slot_name maps dropdown index to StandardMacros order",
          "[settings][macro_buttons]") {
    const auto& slots = StandardMacros::instance().all();
    REQUIRE_FALSE(slots.empty()); // the table is ctor-filled; an empty one voids this test

    SECTION("index 0 is the Empty entry, not a slot") {
        REQUIRE(MacroButtonsOverlayTestAccess::quick_button_index_to_slot_name(0).empty());
    }

    SECTION("index N names slot N-1, for every slot in the table") {
        for (int i = 1; i <= static_cast<int>(slots.size()); ++i) {
            CAPTURE(i);
            REQUIRE(MacroButtonsOverlayTestAccess::quick_button_index_to_slot_name(i) ==
                    slots[static_cast<size_t>(i) - 1].slot_name);
        }
    }

    SECTION("the mapping is a bijection over the table — no two indices name one slot") {
        std::set<std::string> seen;
        for (int i = 1; i <= static_cast<int>(slots.size()); ++i) {
            auto name = MacroButtonsOverlayTestAccess::quick_button_index_to_slot_name(i);
            REQUIRE_FALSE(name.empty());
            REQUIRE(seen.insert(name).second); // false => a duplicate slot_name
        }
    }

    SECTION("one past the end is empty, not the last slot") {
        // Guards the `index - 1 < size` bound. An off-by-one there would return
        // the final slot for an index the dropdown never offers.
        REQUIRE(MacroButtonsOverlayTestAccess::quick_button_index_to_slot_name(
                    static_cast<int>(slots.size()) + 1)
                    .empty());
    }
}
