// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_context_menu_kind_tag.cpp
 * @brief ContextMenu::active_as<T>() resolves the live menu without RTTI.
 *
 * The firmware builds `-fno-rtti`, so the `dynamic_cast` that used to back
 * active_as<>() is gone; each subclass now declares HELIX_CONTEXT_MENU_KIND and
 * the helper compares kind tags. What has to hold for the ~40 XML callbacks that
 * downcast through it: the menu on screen answers to its own type, and — the half
 * a broken tag scheme would silently lose — does NOT answer to a sibling's.
 */

#include "ui_ams_context_menu.h"
#include "ui_ams_selector_menu.h"
#include "ui_context_menu.h"
#include "ui_printer_switch_menu.h"

#include "../test_fixtures.h"
#include "ams_state.h"
#include "helix_type_tag.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Two menus that differ only in type. Both render the AMS card because it is the
// component the other base-class tests already register; nothing here depends on
// its contents, only on which C++ object the base's active() is pointing at.
class MenuAlpha : public helix::ui::ContextMenu {
    HELIX_CONTEXT_MENU_KIND(MenuAlpha)

  protected:
    const char* xml_component_name() const override {
        return "ams_context_menu";
    }
};

class MenuBeta : public helix::ui::ContextMenu {
    HELIX_CONTEXT_MENU_KIND(MenuBeta)

  protected:
    const char* xml_component_name() const override {
        return "ams_context_menu";
    }
};

void show(helix::ui::ContextMenu& menu, lv_obj_t* screen) {
    menu.set_click_point({100, 100});
    REQUIRE(menu.show_near_widget(screen, 0, screen));
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "context menu: active_as resolves the live menu by kind",
                 "[ui][context_menu][type_tag]") {
    REQUIRE(register_component("ams_context_menu"));
    AmsState::instance().init_subjects(true);

    using helix::ui::ContextMenu;

    SECTION("nothing on screen resolves to nullptr") {
        CHECK(ContextMenu::active_as<MenuAlpha>() == nullptr);
    }

    SECTION("the live menu answers to its own type and not to a sibling's") {
        MenuAlpha alpha;
        show(alpha, test_screen());

        CHECK(ContextMenu::active() == &alpha);
        CHECK(ContextMenu::active_as<MenuAlpha>() == &alpha);
        CHECK(ContextMenu::active_as<MenuBeta>() == nullptr);

        alpha.hide();
        process_lvgl(50);
        CHECK(ContextMenu::active_as<MenuAlpha>() == nullptr);
    }

    SECTION("raising the sibling flips which type resolves") {
        MenuAlpha alpha;
        show(alpha, test_screen());
        alpha.hide();
        process_lvgl(50);

        MenuBeta beta;
        show(beta, test_screen());

        CHECK(ContextMenu::active_as<MenuBeta>() == &beta);
        CHECK(ContextMenu::active_as<MenuAlpha>() == nullptr);

        beta.hide();
        process_lvgl(50);
    }
}

// Production menus have to carry distinct tags for the same reason — an omitted
// HELIX_CONTEXT_MENU_KIND is a compile error, but a copy-pasted one naming the
// wrong class is not, and would hand one menu's callbacks another menu's object.
// AmsContextMenu's ctor registers XML subjects, so this needs LVGL up even though
// no menu is shown.
TEST_CASE_METHOD(LVGLTestFixture, "context menu: production menu kinds are distinct",
                 "[ui][context_menu][type_tag]") {
    helix::ui::AmsContextMenu ams_menu;
    helix::ui::AmsSelectorMenu selector_menu;
    helix::ui::PrinterSwitchMenu switcher_menu;

    // Through the base, which is the only way active_as<>() ever calls it: the
    // macro emits no access specifier, so the override sits in whatever section
    // of the subclass body it was written into.
    const helix::ui::ContextMenu& ams = ams_menu;
    const helix::ui::ContextMenu& selector = selector_menu;
    const helix::ui::ContextMenu& switcher = switcher_menu;

    CHECK(ams.kind_tag() == helix::type_tag<helix::ui::AmsContextMenu>());
    CHECK(selector.kind_tag() == helix::type_tag<helix::ui::AmsSelectorMenu>());
    CHECK(switcher.kind_tag() == helix::type_tag<helix::ui::PrinterSwitchMenu>());

    CHECK(ams.kind_tag() != selector.kind_tag());
    CHECK(ams.kind_tag() != switcher.kind_tag());
    CHECK(selector.kind_tag() != switcher.kind_tag());
}
