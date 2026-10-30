// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_context_menu_fit_width.cpp
 * @brief A `fit_content` context menu is as wide as its rows, within the screen.
 *
 * The printer switch menu is the live case: its rows are `width="100%"`, which a
 * `width="content"` card ignores when sizing itself (`w_ignore_size`), so the
 * card has to be told how wide its names are.
 */

#include "ui_button.h"
#include "ui_context_menu.h"
#include "ui_printer_switch_menu.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/layout_manager_test_access.h"
#include "config.h"
#include "layout_manager.h"
#include "lvgl.h"
#include "theme_manager.h"
#include "translation_loader.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::ContextMenu;
using CardWidth = ContextMenu::CardWidth;

// ============================================================================
// compute_card_width
// ============================================================================

// ============================================================================
// decide_column_flow
// ============================================================================

TEST_CASE("decide_column_flow: stacked, then side by side, then wrapped", "[context_menu][fit]") {
    using Flow = ContextMenu::ColumnFlow;
    // Card sizes measured from the Happy Hare lane menu (stacked height, then
    // side-by-side width) against each screen's room less its margins.
    // 800x480: 445 of 460 tall stacks.
    CHECK(ContextMenu::decide_column_flow(445, 193, 780, 460) == Flow::Stacked);
    // 320x480: 471 > 468 goes side by side, 250 of 308 wide.
    CHECK(ContextMenu::decide_column_flow(471, 250, 308, 468) == Flow::Row);
    // 480x272 with a plugin column: 412 > 260, side by side 350 of 468 wide.
    CHECK(ContextMenu::decide_column_flow(412, 350, 468, 260) == Flow::Row);
    // 320x480 with a plugin column: 574 > 468, and 376 > 308 side by side.
    CHECK(ContextMenu::decide_column_flow(574, 376, 308, 468) == Flow::RowWrap);
    // Exactly full fits; one pixel over does not.
    CHECK(ContextMenu::decide_column_flow(100, 999, 50, 100) == Flow::Stacked);
    CHECK(ContextMenu::decide_column_flow(101, 50, 50, 100) == Flow::Row);
    CHECK(ContextMenu::decide_column_flow(101, 51, 50, 100) == Flow::RowWrap);
}

TEST_CASE("compute_card_width: fitted card takes its content width", "[context_menu][fit]") {
    const CardWidth fit{50, 160, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 800, 250, 10, false) == 250);
}

TEST_CASE("compute_card_width: fitted card never drops below min", "[context_menu][fit]") {
    const CardWidth fit{50, 160, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 800, 90, 10, false) == 160);
}

TEST_CASE("compute_card_width: fitted card stops at its share of the screen",
          "[context_menu][fit]") {
    const CardWidth fit{50, 160, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 800, 600, 10, false) == 400);
    CHECK(ContextMenu::compute_card_width(fit, 480, 600, 10, false) == 240);
}

TEST_CASE("compute_card_width: fitted card honours max when it is tighter", "[context_menu][fit]") {
    const CardWidth fit{50, 160, 300, true};
    CHECK(ContextMenu::compute_card_width(fit, 800, 600, 10, false) == 300);
    CHECK(ContextMenu::compute_card_width(fit, 800, 250, 10, false) == 250);
}

TEST_CASE("compute_card_width: the screen margin beats min and the share", "[context_menu][fit]") {
    // A 100% share on a 200px screen would touch both edges; min would overflow it.
    const CardWidth fit{100, 260, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 200, 600, 10, false) == 180);
}

TEST_CASE("compute_card_width: portrait_pct replaces the share on a portrait screen only",
          "[context_menu][fit]") {
    const CardWidth fit{50, 160, 0, true, 100};
    // Portrait: the whole width less a margin each side.
    CHECK(ContextMenu::compute_card_width(fit, 480, 600, 16, true) == 448);
    CHECK(ContextMenu::compute_card_width(fit, 480, 300, 16, true) == 300);
    // Landscape keeps half the screen.
    CHECK(ContextMenu::compute_card_width(fit, 800, 600, 16, false) == 400);
}

TEST_CASE("compute_card_width: without portrait_pct a portrait screen keeps the share",
          "[context_menu][fit]") {
    const CardWidth fit{50, 160, 0, true};
    CHECK(ContextMenu::compute_card_width(fit, 480, 600, 16, true) == 240);
}

TEST_CASE("compute_card_width: a share policy without fit clamps the share to [min,max]",
          "[context_menu][fit]") {
    const CardWidth share{40, 200, 320, false};
    CHECK(ContextMenu::compute_card_width(share, 800, 0, 10, false) == 320);
    CHECK(ContextMenu::compute_card_width(share, 400, 0, 10, false) == 200);
    CHECK(ContextMenu::compute_card_width(share, 600, 999, 10, false) == 240);
}

// ============================================================================
// PrinterSwitchMenu
// ============================================================================

namespace {

class SwitchMenuFixture : public XMLTestFixture {
  public:
    SwitchMenuFixture() {
        REQUIRE(register_component("components/printer_switch_row"));
        REQUIRE(register_component("printer_switch_menu"));

        // Placement follows the app's layout, so no other test's layout may leak in.
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());

        cfg_ = helix::Config::get_instance();
        saved_data_ = helix::ConfigTestAccess::data(*cfg_);
        saved_active_ = helix::ConfigTestAccess::active_printer_id(*cfg_);

        // A badge-sized anchor at the bottom of the left navbar.
        badge_ = lv_obj_create(test_screen());
        lv_obj_set_size(badge_, 60, 60);
    }

    ~SwitchMenuFixture() override {
        menu_.hide();
        process_lvgl(20);
        helix::ConfigTestAccess::data(*cfg_) = saved_data_;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = saved_active_;
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }

    /// Initialise LayoutManager as Application does, optionally under a --layout override.
    static void set_layout(int w, int h, const char* override_name = "") {
        helix::LayoutManager::instance().set_override(override_name);
        helix::LayoutManager::instance().init(w, h);
    }

    static int32_t margin() {
        return theme_manager_get_spacing("space_md");
    }

    void set_printers(const std::vector<std::string>& names) {
        nlohmann::json data;
        data["config_version"] = 3;
        data["active_printer_id"] = "p0";
        for (size_t i = 0; i < names.size(); i++) {
            data["printers"]["p" + std::to_string(i)]["printer_name"] = names[i];
        }
        helix::ConfigTestAccess::data(*cfg_) = data;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = "p0";
    }

    lv_obj_t* show() {
        lv_obj_update_layout(test_screen());
        menu_.show(test_screen(), badge_);
        lv_obj_t* card = lv_obj_find_by_name(test_screen(), "context_menu");
        REQUIRE(card != nullptr);
        lv_obj_update_layout(card);
        return card;
    }

    /// Text the printer_name label of row @p id draws: dotted text carries the dots.
    std::string drawn_name(const char* id) {
        lv_obj_t* list = lv_obj_find_by_name(test_screen(), "printer_list");
        REQUIRE(list != nullptr);
        lv_obj_t* row = lv_obj_find_by_name(list, id);
        REQUIRE(row != nullptr);
        lv_obj_t* label = lv_obj_find_by_name(row, "printer_name");
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    static void check_on_screen(lv_obj_t* card) {
        lv_area_t a;
        lv_obj_get_coords(card, &a);
        const int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        const int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
        INFO("card (" << a.x1 << "," << a.y1 << ")-(" << a.x2 << "," << a.y2 << ") in " << w << "x"
                      << h);
        CHECK(a.x1 >= 0);
        CHECK(a.y1 >= 0);
        CHECK(a.x2 < w);
        CHECK(a.y2 < h);
    }

    helix::Config* cfg_ = nullptr;
    helix::ui::PrinterSwitchMenu menu_;
    lv_obj_t* badge_ = nullptr;

  private:
    nlohmann::json saved_data_;
    std::string saved_active_;
};

} // namespace

TEST_CASE_METHOD(SwitchMenuFixture,
                 "Printer switch menu: short names leave the card as narrow as its header",
                 "[context_menu][fit][printer_switch]") {
    set_printers({"K1", "U1"});
    lv_obj_set_pos(badge_, 10, 400);
    lv_obj_t* card = show();
    // The header (title + close) and the Add button set the width, not two-letter names.
    CHECK(lv_obj_get_width(card) >= 160);
    CHECK(lv_obj_get_width(card) < 200);
    CHECK(drawn_name("p0") == "K1");
}

TEST_CASE_METHOD(SwitchMenuFixture, "Printer switch menu: the card widens to show a long name",
                 "[context_menu][fit][printer_switch]") {
    const std::string name = "Voron 2.4 350 (Enclosure Build)";
    set_printers({name, "Snapmaker U1"});
    lv_obj_set_pos(badge_, 10, 400);
    lv_obj_t* card = show();
    CHECK(lv_obj_get_width(card) > 160);
    CHECK(lv_obj_get_width(card) <= kTestDisplayWidth / 2);
    CHECK(drawn_name("p0") == name);
    CHECK(drawn_name("p1") == "Snapmaker U1");
    check_on_screen(card);
}

TEST_CASE_METHOD(SwitchMenuFixture, "Printer switch menu: a name past half the screen ellipsizes",
                 "[context_menu][fit][printer_switch]") {
    const std::string name(90, 'W');
    set_printers({name});
    lv_obj_set_pos(badge_, 10, 400);
    lv_obj_t* card = show();
    CHECK(lv_obj_get_width(card) == kTestDisplayWidth / 2);
    CHECK(drawn_name("p0") != name);
    check_on_screen(card);
}

TEST_CASE_METHOD(SwitchMenuFixture,
                 "Printer switch menu: a badge at the right of a bottom navbar opens above it",
                 "[context_menu][fit][printer_switch]") {
    ScopedResolution portrait(lv_display_get_default(), 480, 800);
    lv_obj_set_size(test_screen(), 480, 800);
    const std::string name = "Voron 2.4 350 (Enclosure Build)";
    set_printers({name, "FlashForge Adventurer 5M Pro"});
    lv_obj_set_pos(badge_, 410, 730);
    lv_obj_t* card = show();
    check_on_screen(card);

    lv_area_t c;
    lv_area_t b;
    lv_obj_get_coords(card, &c);
    lv_obj_get_coords(badge_, &b);
    CHECK(c.y2 < b.y1);
    CHECK(c.x2 >= b.x1);
    // Past half the portrait width, which would have dotted both names.
    CHECK(lv_obj_get_width(card) > 240);
    CHECK(drawn_name("p0") == name);
    CHECK(drawn_name("p1") == "FlashForge Adventurer 5M Pro");
}

TEST_CASE_METHOD(SwitchMenuFixture,
                 "Printer switch menu: on a portrait screen a long name stops at the side margins",
                 "[context_menu][fit][printer_switch]") {
    ScopedResolution portrait(lv_display_get_default(), 480, 800);
    lv_obj_set_size(test_screen(), 480, 800);
    const std::string name(90, 'W');
    set_printers({name});
    lv_obj_set_pos(badge_, 410, 730);
    lv_obj_t* card = show();
    CHECK(lv_obj_get_width(card) == 480 - 2 * margin());
    CHECK(drawn_name("p0") != name);
    check_on_screen(card);
}

TEST_CASE_METHOD(SwitchMenuFixture,
                 "Printer switch menu: a badge in the left half of a bottom navbar opens above it",
                 "[context_menu][fit][printer_switch]") {
    ScopedResolution portrait(lv_display_get_default(), 480, 800);
    lv_obj_set_size(test_screen(), 480, 800);
    set_layout(480, 800);
    set_printers({"Voron 2.4 350 (Enclosure Build)"});
    lv_obj_set_pos(badge_, 10, 730);
    lv_obj_t* card = show();
    check_on_screen(card);

    lv_area_t c;
    lv_area_t b;
    lv_obj_get_coords(card, &c);
    lv_obj_get_coords(badge_, &b);
    CHECK(c.y2 < b.y1);
}

TEST_CASE_METHOD(SwitchMenuFixture,
                 "Printer switch menu: a portrait --layout override on a landscape screen opens "
                 "above the badge and uses the portrait cap",
                 "[context_menu][fit][printer_switch]") {
    set_layout(kTestDisplayWidth, kTestDisplayHeight, "portrait");
    REQUIRE(helix::app_layout_is_portrait(kTestDisplayWidth, kTestDisplayHeight));
    const std::string name(120, 'W');
    set_printers({name});
    lv_obj_set_pos(badge_, 700, 400);
    lv_obj_t* card = show();
    CHECK(lv_obj_get_width(card) == kTestDisplayWidth - 2 * margin());
    check_on_screen(card);

    lv_area_t c;
    lv_area_t b;
    lv_obj_get_coords(card, &c);
    lv_obj_get_coords(badge_, &b);
    CHECK(c.y2 < b.y1);
}

TEST_CASE_METHOD(SwitchMenuFixture,
                 "Printer switch menu: a standard --layout override on a portrait screen keeps "
                 "the half-screen cap beside the badge",
                 "[context_menu][fit][printer_switch]") {
    ScopedResolution portrait(lv_display_get_default(), 480, 800);
    lv_obj_set_size(test_screen(), 480, 800);
    set_layout(480, 800, "standard");
    REQUIRE_FALSE(helix::app_layout_is_portrait(480, 800));
    const std::string name(120, 'W');
    set_printers({name});
    lv_obj_set_pos(badge_, 10, 400);
    lv_obj_t* card = show();
    CHECK(lv_obj_get_width(card) == 240);

    // Beside: the card hangs off the badge's right edge rather than above it.
    lv_area_t c;
    lv_area_t b;
    lv_obj_get_coords(card, &c);
    lv_obj_get_coords(badge_, &b);
    CHECK(c.x1 >= b.x2 - margin());
}

TEST_CASE_METHOD(SwitchMenuFixture,
                 "Printer switch menu: a translated Add Printer button fits on one line",
                 "[context_menu][fit][printer_switch]") {
    helix::ui::ensure_translation_loaded("fr");
    lv_translation_set_language("fr");
    REQUIRE(std::string(lv_tr("+ Add Printer")) != "+ Add Printer");

    set_printers({"K1"});
    lv_obj_set_pos(badge_, 10, 400);
    lv_obj_t* card = show();
    lv_obj_t* add_label = ui_button_get_label(lv_obj_find_by_name(card, "btn_add_printer"));
    REQUIRE(add_label != nullptr);
    REQUIRE(std::string(lv_label_get_text(add_label)) == lv_tr("+ Add Printer"));
    const lv_font_t* font = lv_obj_get_style_text_font(add_label, LV_PART_MAIN);
    REQUIRE(font != nullptr);
    CHECK(lv_obj_get_height(add_label) == lv_font_get_line_height(font));
    CHECK(lv_obj_get_width(card) > 160);
}
