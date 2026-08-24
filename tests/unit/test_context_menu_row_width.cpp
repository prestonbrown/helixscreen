// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_context_menu_row_width.cpp
 * @brief Context menu rows are tappable across their column, and the card fits the screen.
 *
 * Three defects, one show path:
 *
 * 1. A card sized `width="content"` is as wide as its longest row, and every
 *    shorter row kept its own natural width — a tap on the empty half of "Load"
 *    missed the button and hit the backdrop, dismissing the menu. The declarative
 *    fix does not exist: LVGL flex has no cross-axis stretch, and `width="100%"`
 *    drops a child out of the parent's content-width calculation (`w_ignore_size`,
 *    lv_obj_pos.c), collapsing the card to its widest remaining non-percentage
 *    child.
 *
 * 2. The stacked card was taller than the screen (313px on a 272px panel), so the
 *    header and the whole Load row rendered off the top on every open, at every
 *    tap point. Action groups now flip side by side when stacking will not fit.
 *
 * 3. position_near_widget() clamped the low edge of y BEFORE the high edge, so
 *    fitting the bottom on screen drove y negative for any card taller than the
 *    backdrop. The x path always clamped in the safe order; y did not.
 */

#include "ui_ams_context_menu.h"
#include "ui_context_menu.h"

#include "../test_fixtures.h"
#include "ams_state.h"
#include "theme_manager.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Drives the base-class show path against a real menu layout. The AMS menu is
// used because it has the widest mix of row lengths ("Load" vs "Spool Info") and
// is the one that overflows a real panel.
class BareContextMenu : public helix::ui::ContextMenu {
    HELIX_CONTEXT_MENU_KIND(BareContextMenu)

  protected:
    const char* xml_component_name() const override {
        return "ams_context_menu";
    }
    const char* menu_card_name() const override {
        return "context_menu";
    }
};

// Stands in for external-spool mode at the base-class contract: every action in
// the Filament group is hidden before the card is measured.
class EmptyFilamentColumnMenu : public BareContextMenu {
    // Not required to compile — the base already satisfies the pure virtual — but
    // active_as<> matches on the exact tag, so a derived menu must claim its own.
    HELIX_CONTEXT_MENU_KIND(EmptyFilamentColumnMenu)

  protected:
    void on_created(lv_obj_t* menu) override {
        for (const char* name : {"btn_load", "btn_unload", "btn_gate_select", "btn_gate_check"}) {
            if (lv_obj_t* btn = lv_obj_find_by_name(menu, name))
                lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
};

struct Row {
    int32_t width;
    int32_t container_content_w;
};

bool is_tappable(lv_obj_t* row) {
    return row && !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) &&
           lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_height(row) >= 8;
}

void collect_from(lv_obj_t* container, std::vector<Row>& out) {
    const int32_t content_w = lv_obj_get_content_width(container);
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
        lv_obj_t* row = lv_obj_get_child(container, i);
        if (is_tappable(row))
            out.push_back({lv_obj_get_width(row), content_w});
    }
}

// Rows live in the card AND one level down inside each column group, and a row
// belongs to whichever of those holds it — the two columns are not the same
// width, so "the card's width" is the wrong expectation for a row inside one.
std::vector<Row> tappable_rows(lv_obj_t* card) {
    std::vector<Row> rows;
    collect_from(card, rows);
    if (lv_obj_t* columns = lv_obj_find_by_name(card, "menu_columns")) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(columns); i++) {
            lv_obj_t* col = lv_obj_get_child(columns, i);
            if (col && !lv_obj_has_flag(col, LV_OBJ_FLAG_HIDDEN))
                collect_from(col, rows);
        }
    }
    return rows;
}

lv_obj_t* show_and_get_card(helix::ui::ContextMenu& menu, lv_obj_t* screen) {
    menu.set_click_point({100, 100});
    REQUIRE(menu.show_near_widget(screen, 0, screen));
    lv_obj_t* card = lv_obj_find_by_name(screen, "context_menu");
    REQUIRE(card != nullptr);
    lv_obj_update_layout(card);
    return card;
}

} // namespace

// Establishes the premise the width fix exists for: left alone, rows really do come
// out narrower than the group holding them. Without this the equality assertions
// below would pass just as happily on a menu whose rows were already uniform.
TEST_CASE_METHOD(XMLTestFixture,
                 "context menu: raw card leaves short rows narrower than their column",
                 "[ui][context_menu]") {
    REQUIRE(register_component("ams_context_menu"));
    AmsState::instance().init_subjects(true);
    helix::ui::AmsContextMenu::init_subjects();

    lv_obj_t* menu = create_component("ams_context_menu");
    REQUIRE(menu != nullptr);
    lv_obj_t* card = lv_obj_find_by_name(menu, "context_menu");
    REQUIRE(card != nullptr);
    lv_obj_update_layout(card);

    const std::vector<Row> rows = tappable_rows(card);
    REQUIRE(rows.size() >= 2);

    const bool some_row_is_short = std::any_of(
        rows.begin(), rows.end(), [](const Row& r) { return r.width < r.container_content_w; });
    CHECK(some_row_is_short);
}

TEST_CASE_METHOD(XMLTestFixture, "context menu: every tappable row spans its own column",
                 "[ui][context_menu]") {
    REQUIRE(register_component("ams_context_menu"));
    AmsState::instance().init_subjects(true);
    helix::ui::AmsContextMenu::init_subjects();

    BareContextMenu menu;
    lv_obj_t* card = show_and_get_card(menu, test_screen());

    const std::vector<Row> rows = tappable_rows(card);
    REQUIRE(rows.size() >= 2);

    for (size_t i = 0; i < rows.size(); i++) {
        INFO("tappable row " << i << " of " << rows.size());
        CHECK(rows[i].width == rows[i].container_content_w);
    }

    menu.hide();
    process_lvgl(50);
}

// The whole point of the reflow: whatever the layout ends up being, the card has to
// sit inside the screen with a margin, or be scrollable if even the wide layout
// cannot fit. A card that overflows is one whose top rows cannot be reached at all.
TEST_CASE_METHOD(XMLTestFixture, "context menu: card fits the screen height budget",
                 "[ui][context_menu]") {
    REQUIRE(register_component("ams_context_menu"));
    AmsState::instance().init_subjects(true);
    helix::ui::AmsContextMenu::init_subjects();

    BareContextMenu menu;
    lv_obj_t* card = show_and_get_card(menu, test_screen());

    const int32_t margin = theme_manager_get_spacing("space_md");
    const int32_t budget = TEST_DISPLAY_HEIGHT - (margin * 2);

    INFO("card h=" << lv_obj_get_height(card) << " budget=" << budget);
    CHECK((lv_obj_get_height(card) <= budget || lv_obj_has_flag(card, LV_OBJ_FLAG_SCROLLABLE)));

    menu.hide();
    process_lvgl(50);
}

// The clamp-order bug only bites when the card is taller than the backdrop, so
// shrink the screen until that is true however the columns lay out. A negative y is
// the failure: it puts the header and the first action off the top of the screen.
TEST_CASE_METHOD(XMLTestFixture, "context menu: card is never positioned off the top",
                 "[ui][context_menu]") {
    REQUIRE(register_component("ams_context_menu"));
    AmsState::instance().init_subjects(true);
    helix::ui::AmsContextMenu::init_subjects();

    ScopedResolution shrink(lv_display_get_default(), 480, 200);

    BareContextMenu menu;
    lv_obj_t* card = show_and_get_card(menu, test_screen());

    lv_obj_t* backdrop = lv_obj_get_parent(card);
    REQUIRE(backdrop != nullptr);
    lv_area_t card_area;
    lv_area_t backdrop_area;
    lv_obj_get_coords(card, &card_area);
    lv_obj_get_coords(backdrop, &backdrop_area);

    INFO("card y1=" << card_area.y1 << " backdrop y1=" << backdrop_area.y1
                    << " card h=" << lv_obj_get_height(card));
    CHECK(card_area.y1 >= backdrop_area.y1);

    menu.hide();
    process_lvgl(50);
}

// A group whose actions were all hidden would otherwise render as a heading and a
// rule with nothing under them. The 1px rule is the trap here: a bare lv_obj is
// CLICKABLE by default, so a naive "has a clickable child" test keeps it alive.
TEST_CASE_METHOD(XMLTestFixture, "context menu: a column with no visible action is hidden",
                 "[ui][context_menu]") {
    REQUIRE(register_component("ams_context_menu"));
    AmsState::instance().init_subjects(true);
    helix::ui::AmsContextMenu::init_subjects();

    EmptyFilamentColumnMenu menu;
    lv_obj_t* card = show_and_get_card(menu, test_screen());

    lv_obj_t* col_filament = lv_obj_find_by_name(card, "col_filament");
    lv_obj_t* col_spool = lv_obj_find_by_name(card, "col_spool");
    REQUIRE(col_filament != nullptr);
    REQUIRE(col_spool != nullptr);

    CHECK(lv_obj_has_flag(col_filament, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(col_spool, LV_OBJ_FLAG_HIDDEN));

    // The survivor is the only group left, so its heading is redundant with the card
    // header directly above it — the external-spool menu read "External Spool" and
    // then "Spool" one line down.
    lv_obj_t* heading = lv_obj_find_by_name(col_spool, "col_heading");
    REQUIRE(heading != nullptr);
    CHECK(lv_obj_has_flag(heading, LV_OBJ_FLAG_HIDDEN));

    menu.hide();
    process_lvgl(50);
}

// The mirror of the rule above: with both groups showing, both headings must stay,
// because now they are the only thing saying which group is which.
TEST_CASE_METHOD(XMLTestFixture, "context menu: both headings show when both columns do",
                 "[ui][context_menu]") {
    REQUIRE(register_component("ams_context_menu"));
    AmsState::instance().init_subjects(true);
    helix::ui::AmsContextMenu::init_subjects();

    BareContextMenu menu;
    lv_obj_t* card = show_and_get_card(menu, test_screen());

    for (const char* col_name : {"col_filament", "col_spool"}) {
        lv_obj_t* col = lv_obj_find_by_name(card, col_name);
        REQUIRE(col != nullptr);
        REQUIRE_FALSE(lv_obj_has_flag(col, LV_OBJ_FLAG_HIDDEN));

        INFO("heading of " << col_name);
        lv_obj_t* heading = lv_obj_find_by_name(col, "col_heading");
        REQUIRE(heading != nullptr);
        CHECK_FALSE(lv_obj_has_flag(heading, LV_OBJ_FLAG_HIDDEN));
    }

    menu.hide();
    process_lvgl(50);
}
