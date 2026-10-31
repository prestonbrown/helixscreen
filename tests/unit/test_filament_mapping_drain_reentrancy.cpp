// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapping_drain_reentrancy.cpp
 * @brief publish_chips() runs no queued work under itself (#1221).
 *
 * NavigationManager::go_back() is fully deferred, so a pop of the print-detail
 * overlay can sit in the queue when set_used_tools() reaches
 * publish_chips(). The pop deletes the container and calls
 * on_ui_destroyed(). The rebuild must finish against the live container, the
 * pop must run on the next tick, and the card must stay usable after it.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_test_fixture.h"
#include "../mapping_card_render_fixture.h"
#include "../test_helpers/widget_in_tree.h"

#include <set>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentMappingCard;

namespace {

struct MappingCardFixture : public LVGLTestFixture {
    FilamentMappingCard card;
    lv_obj_t* card_widget = nullptr;
    lv_obj_t* rows = nullptr;
    lv_obj_t* warning = nullptr;

    MappingCardFixture() {
        card_widget = lv_obj_create(test_screen());
        rows = lv_obj_create(card_widget);
        warning = lv_obj_create(card_widget);
        card.create(card_widget, rows, warning);
    }
};

} // namespace

TEST_CASE_METHOD(MappingCardFixture, "publish_chips: a pop queued ahead of it runs after it",
                 "[filament][mapping][filament_mapping][crash][1221][drain_reentrancy]") {
    // Stand in for the deferred go_back() that pops the print-detail overlay:
    // the same two steps destroy_overlay_ui() takes, in the same order.
    bool ran = false;
    helix::ui::queue_update([this, &ran]() {
        ran = true;
        helix::ui::safe_delete_deferred(rows); // nulls `rows`
        card.on_ui_destroyed();                // nulls rows_container_ / card_ / warning_container_
    });

    // set_used_tools() is the entry point the crash came through.
    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{0}));
    CHECK_FALSE(ran);
    CHECK(rows != nullptr);

    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ran);
    REQUIRE(rows == nullptr);

    // The card outlives its widgets: a later call finds no container and does nothing.
    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{1}));
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "publish_chips: still publishes normally when nothing is queued",
                 "[filament][mapping][filament_mapping][1221]") {
    // A bare early-return in publish_chips() would satisfy the case above while
    // silently disabling the card. With an empty queue the container survives
    // and the chips are republished: 2 chips become 1.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(card.should_show());
    REQUIRE(chip_count() == 2);

    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{0}));

    REQUIRE(rows != nullptr);
    REQUIRE(helix::test::widget_in_tree(rows));
    CHECK(chip_count() == 1);
    CHECK(chip(0) != nullptr);
    CHECK(chip(1) == nullptr);
}
