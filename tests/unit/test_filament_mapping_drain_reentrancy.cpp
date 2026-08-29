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
#include "../mapping_card_render_fixture.h"

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

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "rebuild_compact_view: still rebuilds normally when nothing is queued",
                 "[filament][mapping][filament_mapping][1221]") {
    // Mutation guard for the fix: a bare early-return added after the drain
    // would satisfy the crash test above while silently disabling the card.
    // With an empty queue the container must survive AND be re-rendered.
    //
    // MappingCardRenderFixture, NOT the MappingCardFixture the crash test uses.
    // rebuild_compact_view() styles and populates the container only for a card
    // it is going to show, and "show" needs a started AMS backend and a palette
    // - neither of which that fixture has. On it, the function returns before
    // lv_obj_set_flex_flow() ever runs, so every assertion here read an
    // untouched container, and LV_FLEX_FLOW_ROW is 0x00, the LVGL style default
    // for a property with no case in lv_style_prop_get_default(). The flow check
    // passed whether or not the production call existed.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(card.should_show());
    REQUIRE(lv_obj_get_child_count(rows) == 2);

    // ONE tool out of a two-tool palette, deliberately: it changes the render
    // fingerprint, so this goes through a real rebuild rather than the
    // idempotent-render early-out that an identical set would hit. Without that
    // the chip count below would be describing update()'s render, not this one,
    // and the early-return this case exists to catch would sail through.
    REQUIRE_NOTHROW(card.set_used_tools(std::set<int>{0}));

    REQUIRE(rows != nullptr);
    REQUIRE(lv_obj_is_valid(rows));
    // The rebuild genuinely ran: 2 chips became 1. This is what makes the flow
    // assertion below a statement about code that executed.
    REQUIRE(lv_obj_get_child_count(rows) == 1);
    // ROW, not ROW_WRAP: the chips live in one fixed-height row, so a wrapped
    // second row would be clipped rather than shown - rebuild_compact_view caps
    // them at what fits and renders a "+N" pill for the rest instead.
    CHECK(lv_obj_get_style_flex_flow(rows, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
}
