// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "page_scroll_auto_inject.h"
#include "panel_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {
lv_obj_t* add_vscroll(lv_obj_t* parent, int n) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, 400, 200);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < n; ++i) {
        lv_obj_t* r = lv_obj_create(c);
        lv_obj_set_size(r, lv_pct(100), 60);
    }
    return c;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AutoInject attaches only to overflowing vertical scrollables",
                 "[page_scroll_buttons][ui]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    auto& inj = PageScrollAutoInject::instance();
    inj.shutdown(); // clean slate
    inj.init();
    dsm.set_page_scroll_buttons(true);

    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_set_size(root, 480, 320);
    lv_obj_t* overflowing = add_vscroll(root, 20); // qualifies
    lv_obj_t* fits = add_vscroll(root, 1);         // no overflow -> skip
    lv_obj_t* horizontal = add_vscroll(root, 20);
    lv_obj_set_scroll_dir(horizontal, LV_DIR_HOR); // horizontal -> skip
    lv_obj_update_layout(root);

    inj.on_root_shown(root);
    CHECK(inj.managed_count() == 1);
    CHECK(lv_obj_find_by_name(overflowing, "up") != nullptr);
    CHECK(lv_obj_find_by_name(fits, "up") == nullptr);
    CHECK(lv_obj_find_by_name(horizontal, "up") == nullptr);

    // Disabling the setting detaches everything. The toggle callback drives this
    // via on_setting_toggled() (there is no subject observer — see
    // PageScrollAutoInject::init); replicate that call here.
    dsm.set_page_scroll_buttons(false);
    inj.on_setting_toggled(false);
    process_lvgl(20); // flush async gutter deletes
    CHECK(inj.managed_count() == 0);

    inj.shutdown();
}

TEST_CASE_METHOD(LVGLUITestFixture, "AutoInject skips nested scrollables",
                 "[page_scroll_buttons][ui]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    auto& inj = PageScrollAutoInject::instance();
    inj.shutdown();
    inj.init();
    dsm.set_page_scroll_buttons(true);

    lv_obj_t* outer = add_vscroll(test_screen(), 5);
    lv_obj_t* inner = add_vscroll(outer, 20); // nested; must be skipped
    (void)inner;
    lv_obj_update_layout(outer);

    inj.on_root_shown(outer);
    CHECK(inj.managed_count() == 1); // outer only
    CHECK(lv_obj_find_by_name(inner, "up") == nullptr);

    inj.shutdown();
    process_lvgl(20);
}

// Page scrolling is a page affordance, so the walk stops at a home widget tile
// rather than gutter-ing whatever happens to scroll inside one. The gutter is
// two chevrons plus a gap tall (172px at the medium tier), which on an 800x480
// panel is most of a tile's height, so it lands on the widget's own content.
TEST_CASE_METHOD(LVGLUITestFixture, "AutoInject cuts the walk at a home widget tile",
                 "[page_scroll_buttons][ui]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    auto& inj = PageScrollAutoInject::instance();
    inj.shutdown();
    inj.init();
    dsm.set_page_scroll_buttons(true);

    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_set_size(root, 480, 320);

    // A tile the way PanelWidgetManager builds one: the root itself is NOT
    // scrollable (39 of 40 panel_widget_*.xml roots clear the flag, and ui_card
    // clears it in C), with something scrollable inside. Testing SCROLLABLE on
    // the tile would therefore miss this entirely — only the mark catches it.
    lv_obj_t* tile = lv_obj_create(root);
    lv_obj_set_size(tile, 240, 200);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, helix::PANEL_WIDGET_TILE_FLAG);
    lv_obj_t* inner = add_vscroll(tile, 20);

    // A page-level container beside the tile must still qualify, or the cut is
    // eating more than it should.
    lv_obj_t* page = add_vscroll(root, 20);

    lv_obj_update_layout(root);
    inj.on_root_shown(root);

    CHECK(lv_obj_find_by_name(inner, "up") == nullptr);
    CHECK(lv_obj_find_by_name(tile, "up") == nullptr);
    CHECK(lv_obj_find_by_name(page, "up") != nullptr);
    CHECK(inj.managed_count() == 1); // the page, never the tile

    inj.shutdown();
    process_lvgl(20);
}

// panel_widget_nozzle_temps is the one tile whose own root is scrollable="true",
// so the mark has to win over qualifies(), not merely stop the descent.
TEST_CASE_METHOD(LVGLUITestFixture, "AutoInject skips a tile whose own root scrolls",
                 "[page_scroll_buttons][ui]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    auto& inj = PageScrollAutoInject::instance();
    inj.shutdown();
    inj.init();
    dsm.set_page_scroll_buttons(true);

    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_set_size(root, 480, 320);

    lv_obj_t* tile = add_vscroll(root, 20); // overflows; would qualify unmarked
    lv_obj_add_flag(tile, helix::PANEL_WIDGET_TILE_FLAG);

    lv_obj_update_layout(root);
    inj.on_root_shown(root);

    CHECK(lv_obj_find_by_name(tile, "up") == nullptr);
    CHECK(inj.managed_count() == 0);

    inj.shutdown();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AutoInject propagates managed-ancestor claim across repeated on_root_shown",
                 "[page_scroll_buttons][ui]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    auto& inj = PageScrollAutoInject::instance();
    inj.shutdown(); // clean slate
    inj.init();
    dsm.set_page_scroll_buttons(true);

    lv_obj_t* root = test_screen();
    lv_obj_t* outer = add_vscroll(root, 5);   // A: qualifies
    lv_obj_t* inner = add_vscroll(outer, 20); // B: nested inside A; also qualifies alone
    lv_obj_update_layout(root);

    // First pass: A gets attached, B is skipped as a fresh nested descendant.
    inj.on_root_shown(root);
    process_lvgl(20);
    CHECK(inj.managed_count() == 1);
    CHECK(lv_obj_find_by_name(outer, "up") != nullptr);
    CHECK(lv_obj_find_by_name(inner, "up") == nullptr);

    // Second pass over the SAME persistent tree (simulates navigate-away and
    // back to a cached panel). A is already in controllers_, so the fresh-attach
    // branch is skipped for it — but the ancestor claim must still propagate to
    // B. Without Fix 1, `claimed` stays false for A's subtree walk and B wrongly
    // qualifies for its own controller, producing nested gutters.
    inj.on_root_shown(root);
    process_lvgl(20);
    CHECK(inj.managed_count() == 1); // still just A; without Fix 1 this is 2
    CHECK(lv_obj_find_by_name(outer, "up") != nullptr);
    CHECK(lv_obj_find_by_name(inner, "up") == nullptr);

    inj.shutdown();
    process_lvgl(20);
}
