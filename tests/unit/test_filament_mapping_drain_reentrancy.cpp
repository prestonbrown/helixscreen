// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapping_drain_reentrancy.cpp
 * @brief Regression test for #1221 — SIGSEGV in rebuild_compact_view().
 *
 * rebuild_compact_view() null-checks rows_container_, then does a
 * freeze+drain to escape the surrounding UpdateQueue/LVGL batch, then styles
 * and repopulates the container. The drain is not inert: it runs whatever was
 * already queued, and NavigationManager::go_back() is fully deferred — its
 * entire body is a queue_update(). So a pop queued before we entered executes
 * on the drain line, tears down the print-detail overlay, and reaches
 * FilamentMappingCard::on_ui_destroyed(), which nulls rows_container_.
 *
 * On a Snapmaker U1 this happened for real: a print-preparation timeout fired
 * ~60s after the optimistic navigation to print status, and its completion
 * handler called go_back() (queued) and then show_detail_view()
 * (synchronous) — which walked straight into rebuild_compact_view() with the
 * pop sitting in the queue. lv_obj_set_flex_flow(nullptr, ...) then faulted at
 * 0x38 inside get_local_style().
 *
 * The contract pinned here: whatever the drain does to the card's widgets,
 * rebuild_compact_view() must not dereference a container that died during it.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_test_fixture.h"

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

TEST_CASE_METHOD(MappingCardFixture,
                 "rebuild_compact_view: survives the container dying inside its own drain",
                 "[filament][mapping][filament_mapping][crash][1221]") {
    // Stand in for the deferred NavigationManager::go_back() that pops the
    // print-detail overlay: a callback already sitting in the queue, performing
    // the same two steps destroy_overlay_ui() does and in the same order —
    // safe_delete_deferred() (which nulls the caller's pointer), then
    // on_ui_destroyed(). A sync lv_obj_delete() here would be banned inside a
    // queued callback anyway, and the real path does not do one.
    helix::ui::queue_update([this]() {
        helix::ui::safe_delete_deferred(rows); // nulls `rows`
        card.on_ui_destroyed();                // nulls rows_container_ / card_ / warning_container_
    });

    // set_used_tools() is the exact entry point the crash came through
    // (PrintSelectDetailView::show() line 393). It calls rebuild_compact_view(),
    // whose drain() executes the callback queued above — after the guard at the
    // top of the function has already passed.
    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{0}));

    // The teardown must actually have run during the drain, or this test
    // proves nothing. If the queued callback were still pending here, the
    // reentrancy window was never opened and the assertion above is vacuous.
    REQUIRE(rows == nullptr);
}

TEST_CASE_METHOD(MappingCardFixture,
                 "rebuild_compact_view: still rebuilds normally when nothing is queued",
                 "[filament][mapping][filament_mapping][1221]") {
    // Mutation guard for the fix: a bare early-return added after the drain
    // would satisfy the crash test above while silently disabling the card.
    // With an empty queue the container must survive and stay styled.
    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{0}));

    REQUIRE(rows != nullptr);
    REQUIRE(lv_obj_is_valid(rows));
    REQUIRE(lv_obj_get_style_flex_flow(rows, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP);
}
