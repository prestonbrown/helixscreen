// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tile_edit_callout.cpp
 * @brief Which rows home Edit Mode's tile callout offers
 */

#include "ui_tile_edit_callout.h"

#include "../catch_amalgamated.hpp"

using helix::ui::tile_callout_rows;

TEST_CASE("tile_callout_rows: Configure only for a widget with a configure action",
          "[edit-callout]") {
    CHECK(tile_callout_rows(true).configure);
    CHECK_FALSE(tile_callout_rows(false).configure);
    CHECK(tile_callout_rows(true).remove);
    CHECK(tile_callout_rows(false).remove);
}
