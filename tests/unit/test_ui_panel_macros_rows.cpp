// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// MacrosPanel through its real widget tree: which macro each on-screen row names,
// and what tapping that row does. Rows are found by walking the list, never by
// pool or subject index, so these cases hold whatever builds the rows.

#include "ui_panel_macros.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "../test_fixtures.h"
#include "../test_helpers/macros_panel_test_access.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "device_display_name.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::UpdateQueue;

namespace {

struct Row {
    lv_obj_t* card;
    std::string label;
    lv_area_t area;
};

class MacrosRowsFixture : public XMLTestFixture {
  public:
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState mock_state;
    MoonrakerAPIMock mock_api{mock_client, mock_state};
    std::vector<std::string> toasts;
    MacrosPanel& panel = get_global_macros_panel();
    lv_obj_t* root = nullptr;

    MacrosRowsFixture() {
        helix::Config::get_instance()->reset_to_defaults();
        helix::MacroParamCache::instance().clear();
        helix::SafetySettingsManager::instance().set_macro_require_confirmation(false);
        mock_state.init_subjects(false);
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity, const std::string& m, uint32_t) { toasts.push_back(m); });

        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("overlay_panel"));
        REQUIRE(register_component("macro_card"));
        REQUIRE(register_component("macro_panel"));
        panel.deinit_subjects();
        panel.init_subjects();
        panel.register_callbacks();
    }

    ~MacrosRowsFixture() override {
        set_moonraker_api(nullptr);
        helix::ui::set_test_toast_hook(nullptr);
        if (root) {
            panel.on_deactivate(DeactivateReason::NavigateAway);
            panel.destroy_overlay_ui(root);
            settle();
        }
        MacrosPanelTestAccess::teardown(panel);
        panel.deinit_subjects();
        helix::MacroParamCache::instance().clear();
        helix::SafetySettingsManager::instance().set_macro_require_confirmation(true);
    }

    /// Every macro is known to take no parameters, so a tap runs it outright.
    /// `described` macros also carry a description, which makes their rows taller.
    void open(const std::vector<std::string>& macros,
              const std::vector<std::string>& described = {}, lv_obj_t* parent = nullptr) {
        nlohmann::json config;
        for (const auto& m : macros) {
            config["gcode_macro " + m]["gcode"] = "G28";
        }
        for (const auto& m : described) {
            config["gcode_macro " + m]["description"] =
                "Description long enough to wrap onto a second line of the row on an 800 "
                "pixel wide screen, so this row is taller than its neighbours";
        }
        helix::MacroParamCache::instance().populate_from_configfile(
            config, std::unordered_set<std::string>(macros.begin(), macros.end()));

        // create() rebuilds from all_macros_ when no API is installed.
        MacrosPanelTestAccess::seed(panel, macros);
        root = panel.create(parent ? parent : test_screen());
        REQUIRE(root != nullptr);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(root, lv_pct(100), lv_pct(100));
        panel.on_activate();
        settle();
    }

    void settle() {
        for (int i = 0; i < 4; i++) {
            UpdateQueue::instance().drain_until_empty();
            lv_obj_update_layout(test_screen());
        }
    }

    lv_obj_t* list() {
        return lv_obj_find_by_name(root, "macro_list");
    }

    /// Row cards on the list, shown or not.
    std::vector<lv_obj_t*> cards() {
        std::vector<lv_obj_t*> out;
        lv_obj_t* rows = lv_obj_find_by_name(root, "rows_container");
        REQUIRE(rows != nullptr);
        for (uint32_t i = 0; i < lv_obj_get_child_count(rows); i++) {
            lv_obj_t* c = lv_obj_get_child(rows, static_cast<int32_t>(i));
            if (lv_obj_find_by_name(c, "macro_label"))
                out.push_back(c);
        }
        return out;
    }

    /// Shown rows in screen order.
    std::vector<Row> rows() {
        settle();
        std::vector<Row> out;
        for (lv_obj_t* c : cards()) {
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN))
                continue;
            Row r{c, lv_label_get_text(lv_obj_find_by_name(c, "macro_label")), {}};
            lv_obj_get_coords(c, &r.area);
            out.push_back(r);
        }
        std::sort(out.begin(), out.end(),
                  [](const Row& a, const Row& b) { return a.area.y1 < b.area.y1; });
        return out;
    }

    std::vector<std::string> labels() {
        std::vector<std::string> out;
        for (const auto& r : rows())
            out.push_back(r.label);
        return out;
    }

    static std::string shown(const std::string& macro) {
        return helix::get_display_name(macro, helix::DeviceType::MACRO);
    }

    void tap(lv_obj_t* card) {
        helix::ui::send_tap(card);
        settle();
    }

    static lv_point_t centre_of(lv_obj_t* card) {
        lv_area_t a;
        lv_obj_get_coords(card, &a);
        return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
    }

    void long_press(lv_obj_t* card) {
        lv_obj_send_event(card, LV_EVENT_LONG_PRESSED, nullptr);
        settle();
    }

    lv_obj_t* row_named(const std::string& macro) {
        for (const auto& r : rows()) {
            if (r.label == shown(macro))
                return r.card;
        }
        FAIL("no row shows " << macro);
        return nullptr;
    }

    bool checked(lv_obj_t* card) {
        return !lv_obj_has_flag(lv_obj_find_by_name(card, "chk_mark"), LV_OBJ_FLAG_HIDDEN);
    }
};

} // namespace

TEST_CASE_METHOD(MacrosRowsFixture, "Macros rows list the visible macros in order",
                 "[macros][macros_rows]") {
    open({"PRINT_START", "CLEAN_NOZZLE", "_HOME_Z", "LOAD_FILAMENT"});

    // The first-run hidden set holds the _-prefixed macros.
    CHECK(labels() == std::vector<std::string>{shown("CLEAN_NOZZLE"), shown("LOAD_FILAMENT"),
                                               shown("PRINT_START")});
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(root, "empty_state"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(MacrosRowsFixture, "Macros with nothing to show reach the empty state",
                 "[macros][macros_rows]") {
    open({"_ONLY_SYSTEM"});

    CHECK(labels().empty());
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(root, "empty_state"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(list(), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(MacrosRowsFixture, "Tapping a Macros row runs the macro it names",
                 "[macros][macros_rows]") {
    open({"CLEAN_NOZZLE", "LOAD_FILAMENT", "PRINT_START"});
    set_moonraker_api(&mock_api);

    tap(row_named("LOAD_FILAMENT"));

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0] == shown("LOAD_FILAMENT") + " sent");
}

TEST_CASE_METHOD(MacrosRowsFixture, "Macros edit mode toggles a row and Save hides it",
                 "[macros][macros_rows]") {
    open({"CLEAN_NOZZLE", "LOAD_FILAMENT", "PRINT_START", "_HOME_Z"});

    long_press(row_named("CLEAN_NOZZLE"));

    // Edit mode lists every macro; the seeded _HOME_Z starts unchecked.
    REQUIRE(labels().size() == 4);
    CHECK_FALSE(checked(row_named("_HOME_Z")));
    CHECK(checked(row_named("LOAD_FILAMENT")));

    tap(row_named("LOAD_FILAMENT"));
    CHECK_FALSE(checked(row_named("LOAD_FILAMENT")));
    CHECK(checked(row_named("PRINT_START")));

    lv_obj_t* save = lv_obj_find_by_name(root, "action_button");
    REQUIRE(save != nullptr);
    lv_obj_send_event(save, LV_EVENT_CLICKED, nullptr);
    settle();

    CHECK(labels() == std::vector<std::string>{shown("CLEAN_NOZZLE"), shown("PRINT_START")});
}

namespace {

std::vector<std::string> many_macros(int n) {
    std::vector<std::string> out;
    for (int i = 0; i < n; i++) {
        char name[32];
        std::snprintf(name, sizeof(name), "MACRO_%03d", i);
        out.emplace_back(name);
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(MacrosRowsFixture,
                 "A long Macros list builds a viewport of rows and rebinds them on scroll",
                 "[macros][macros_rows]") {
    const auto macros = many_macros(200);
    std::vector<std::string> described;
    for (size_t i = 0; i < macros.size(); i += 7)
        described.push_back(macros[i]);
    open(macros, described);

    lv_obj_t* scroller = list();
    lv_area_t view;
    lv_obj_get_coords(scroller, &view);
    REQUIRE(lv_obj_get_height(scroller) > 100);

    // Rows exist for what the viewport shows plus a margin, not for every macro.
    CHECK(cards().size() < 30);

    // Each shown row names the macro whose place in the list it occupies, and the shown
    // rows together cover the viewport.
    auto check_window = [&](const char* where) {
        INFO(where);
        const auto shown_rows = rows();
        REQUIRE_FALSE(shown_rows.empty());
        const auto first = std::find_if(macros.begin(), macros.end(), [&](const auto& m) {
            return shown(m) == shown_rows.front().label;
        });
        REQUIRE(first != macros.end());
        const size_t base = static_cast<size_t>(first - macros.begin());
        for (size_t k = 0; k < shown_rows.size(); k++) {
            REQUIRE(base + k < macros.size());
            CHECK(shown_rows[k].label == shown(macros[base + k]));
        }
        CHECK(shown_rows.front().area.y1 <= view.y1);
        CHECK((shown_rows.back().area.y2 >= view.y2 || base + shown_rows.size() == macros.size()));
        return base;
    };

    CHECK(check_window("top") == 0);

    lv_obj_scroll_to_y(scroller, 4000, LV_ANIM_OFF);
    settle();
    const size_t mid = check_window("middle");
    CHECK(mid > 20);

    // A row reached by scrolling runs the macro it shows.
    set_moonraker_api(&mock_api);
    Row target{};
    for (const auto& r : rows()) {
        if (r.area.y1 >= view.y1 && r.area.y2 <= view.y2) {
            target = r;
            break;
        }
    }
    REQUIRE(target.card != nullptr);
    tap(target.card);
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0] == target.label + " sent");

    lv_obj_scroll_to_y(scroller, 1000000, LV_ANIM_OFF);
    settle();
    check_window("bottom");
    CHECK(rows().back().label == shown(macros.back()));

    lv_obj_scroll_to_y(scroller, 0, LV_ANIM_OFF);
    settle();
    CHECK(check_window("back at top") == 0);
}

TEST_CASE_METHOD(MacrosRowsFixture,
                 "A Macros list change under a held row does not run the rebound macro",
                 "[macros][macros_rows]") {
    open({"CLEAN_NOZZLE", "LOAD_FILAMENT", "PRINT_START"});
    set_moonraker_api(&mock_api);
    helix_test::ScopedPointerIndev indev;

    lv_obj_t* card = row_named("LOAD_FILAMENT");
    lv_area_t a;
    lv_obj_get_coords(card, &a);
    const int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
    indev.press(x, y);
    bool pressing_card = false;
    for (lv_obj_t* o = indev.indev()->pointer.act_obj; o; o = lv_obj_get_parent(o))
        pressing_card = pressing_card || o == card;
    REQUIRE(pressing_card);

    // A reconnect discovers one more macro, sorted ahead of the held one, so the
    // pressed slot now shows a different macro.
    MacrosPanelTestAccess::seed(panel,
                                {"BED_MESH", "CLEAN_NOZZLE", "LOAD_FILAMENT", "PRINT_START"});
    MacrosPanelTestAccess::rebuild(panel);
    settle();
    REQUIRE(row_named("BED_MESH") != nullptr);

    indev.release(x, y);
    settle();
    CHECK(toasts.empty());
}

TEST_CASE_METHOD(MacrosRowsFixture,
                 "A Macros row hold that acts ends the press; one that declines keeps the tap",
                 "[macros][macros_rows][tap_hold]") {
    open({"CLEAN_NOZZLE", "LOAD_FILAMENT", "PRINT_START"});
    helix_test::ScopedPointerIndev finger;
    const auto hold_row = [&](const char* macro) {
        const lv_point_t p = centre_of(row_named(macro));
        finger.press(p.x, p.y);
        finger.hold(finger.long_press_hold_ms());
        finger.release(p.x, p.y);
        settle();
    };

    SECTION("a hold enters edit mode, and its lift is not also a tap on the row") {
        hold_row("LOAD_FILAMENT");
        REQUIRE(MacrosPanelTestAccess::edit_mode(panel));
        // A tap in edit mode toggles the row, so the lift would uncheck it.
        CHECK(checked(row_named("LOAD_FILAMENT")));
    }

    SECTION("a hold kept down three times the long-press time is still one hold") {
        lv_obj_t* card = row_named("LOAD_FILAMENT");
        int holds = 0;
        lv_obj_add_event_cb(
            card, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
            LV_EVENT_LONG_PRESSED, &holds);
        const lv_point_t p = centre_of(card);
        finger.press(p.x, p.y);
        finger.hold(3 * finger.long_press_hold_ms());
        finger.release(p.x, p.y);
        settle();
        REQUIRE(MacrosPanelTestAccess::edit_mode(panel));
        CHECK(holds == 1);
    }

    SECTION("in edit mode the hold declines, so its release is still the row's tap") {
        long_press(row_named("CLEAN_NOZZLE"));
        REQUIRE(MacrosPanelTestAccess::edit_mode(panel));
        REQUIRE(checked(row_named("LOAD_FILAMENT")));

        hold_row("LOAD_FILAMENT");
        CHECK_FALSE(checked(row_named("LOAD_FILAMENT")));

        const lv_point_t p = centre_of(row_named("LOAD_FILAMENT"));
        finger.press(p.x, p.y);
        finger.release(p.x, p.y);
        settle();
        CHECK(checked(row_named("LOAD_FILAMENT")));
    }
}

TEST_CASE_METHOD(MacrosRowsFixture, "A taller Macros viewport fills with rows",
                 "[macros][macros_rows]") {
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_remove_style_all(host);
    lv_obj_set_size(host, lv_pct(100), 150);
    lv_obj_update_layout(test_screen());
    open(many_macros(200), {}, host);
    const size_t slots_short = cards().size();

    lv_obj_set_height(host, lv_pct(100));
    settle();

    lv_area_t view;
    lv_obj_get_coords(list(), &view);
    REQUIRE(view.y2 - view.y1 > 300);
    const auto shown_rows = rows();
    REQUIRE_FALSE(shown_rows.empty());
    CHECK(cards().size() > slots_short);
    CHECK(shown_rows.back().area.y2 >= view.y2);
}

TEST_CASE_METHOD(MacrosRowsFixture,
                 "A Macros list that shrinks while scrolled shows the rows its slots hold",
                 "[macros][macros_rows]") {
    const auto macros = many_macros(200);
    open(macros);
    lv_obj_scroll_to_y(list(), 6000, LV_ANIM_OFF);
    settle();
    REQUIRE(lv_obj_get_scroll_y(list()) > 3000);

    const std::vector<std::string> fewer(macros.begin(), macros.begin() + 30);
    MacrosPanelTestAccess::seed(panel, fewer);
    MacrosPanelTestAccess::rebuild(panel);
    settle();

    const auto shown_rows = rows();
    REQUIRE_FALSE(shown_rows.empty());
    CHECK(shown_rows.back().label == shown(fewer.back()));
    const auto& displayed = MacrosPanelTestAccess::displayed(panel);
    for (const auto& r : shown_rows) {
        const char* name = lv_obj_get_name(r.card);
        REQUIRE(name != nullptr);
        size_t slot = 0;
        REQUIRE(std::sscanf(name, "macro_slot_%zu", &slot) == 1);
        const size_t item = MacrosPanelTestAccess::item_in_slot(panel, slot);
        REQUIRE(item < displayed.size());
        CHECK(r.label == shown(displayed[item]));
    }
    lv_area_t view;
    lv_obj_get_coords(list(), &view);
    CHECK(shown_rows.front().area.y1 <= view.y1);
    CHECK(shown_rows.back().area.y2 >= view.y1);
}

TEST_CASE_METHOD(MacrosRowsFixture, "A Macros edit toggle survives scrolling its row away and back",
                 "[macros][macros_rows]") {
    const auto macros = many_macros(200);
    open(macros);
    long_press(row_named(macros[2]));
    REQUIRE(checked(row_named(macros[2])));

    tap(row_named(macros[2]));
    REQUIRE_FALSE(checked(row_named(macros[2])));
    CHECK(checked(row_named(macros[3])));

    lv_obj_scroll_to_y(list(), 6000, LV_ANIM_OFF);
    settle();
    for (const auto& r : rows())
        REQUIRE(r.label != shown(macros[2]));

    lv_obj_scroll_to_y(list(), 0, LV_ANIM_OFF);
    settle();
    CHECK_FALSE(checked(row_named(macros[2])));
    CHECK(checked(row_named(macros[3])));
}
