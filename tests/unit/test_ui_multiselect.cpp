// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_multiselect.cpp
 * @brief Unit tests for UiMultiselect widget
 */

#include "ui_multiselect.h"

#include "../lvgl_test_fixture.h"
#include "asset_manager.h"
#include "theme_manager.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

// ============================================================================
// Fixture with theme initialized (eliminates theme token warnings)
// ============================================================================

class MultiSelectTestFixture : public LVGLTestFixture {
    static bool s_theme_initialized;

  public:
    MultiSelectTestFixture() : LVGLTestFixture() {
        if (!s_theme_initialized) {
            // Theme init requires no screens present
            if (m_test_screen) {
                lv_obj_delete(m_test_screen);
                m_test_screen = nullptr;
            }

            AssetManager::register_all();
            lv_xml_register_component_from_file("A:ui_xml/globals.xml");
            theme_manager_init(lv_display_get_default(), false);
            s_theme_initialized = true;

            // Recreate test screen with theme applied
            m_test_screen = lv_obj_create(nullptr);
            lv_screen_load(m_test_screen);
        }
    }
};

bool MultiSelectTestFixture::s_theme_initialized = false;

// ============================================================================
// Basic Lifecycle Tests
// ============================================================================

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: default state", "[multiselect]") {
    UiMultiselect ms;
    REQUIRE_FALSE(ms.is_attached());
    REQUIRE(ms.item_count() == 0);
    REQUIRE(ms.get_selected_count() == 0);
    REQUIRE(ms.get_selected_keys().empty());
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: attach and detach", "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());

    ms.attach(container);
    REQUIRE(ms.is_attached());

    ms.detach();
    REQUIRE_FALSE(ms.is_attached());
}

// ============================================================================
// Item Management
// ============================================================================

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: set items", "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({{"a", "Alpha"}, {"b", "Beta"}, {"c", "Charlie"}});
    REQUIRE(ms.item_count() == 3);
    REQUIRE(ms.get_selected_count() == 0);

    auto items = ms.get_items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].key == "a");
    REQUIRE(items[0].label == "Alpha");
    REQUIRE_FALSE(items[0].selected);
    REQUIRE(items[1].key == "b");
    REQUIRE(items[2].key == "c");
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: set items replaces previous",
                 "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({{"a", "Alpha"}, {"b", "Beta"}});
    REQUIRE(ms.item_count() == 2);

    ms.set_items({{"x", "X-ray"}});
    REQUIRE(ms.item_count() == 1);
    REQUIRE(ms.get_items()[0].key == "x");
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: empty items list", "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({});
    REQUIRE(ms.item_count() == 0);
    REQUIRE(ms.get_selected_keys().empty());
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: key defaults to label", "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    // Empty key should use label as key
    ms.set_items({{"", "Fallback Label"}});
    auto items = ms.get_items();
    REQUIRE(items[0].key == "Fallback Label");
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: initial selection state",
                 "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({{"a", "Alpha", true}, {"b", "Beta", false}, {"c", "Charlie", true}});
    REQUIRE(ms.get_selected_count() == 2);

    auto keys = ms.get_selected_keys();
    REQUIRE(keys.size() == 2);
    REQUIRE(keys[0] == "a");
    REQUIRE(keys[1] == "c");
}

// ============================================================================
// Selection Operations
// ============================================================================

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: set_selected", "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({{"a", "Alpha"}, {"b", "Beta"}, {"c", "Charlie"}});

    REQUIRE(ms.set_selected("b", true));
    REQUIRE(ms.get_selected_count() == 1);
    REQUIRE(ms.get_selected_keys()[0] == "b");

    // Setting same state is a no-op (returns true, no callback)
    REQUIRE(ms.set_selected("b", true));
    REQUIRE(ms.get_selected_count() == 1);

    // Unknown key returns false
    REQUIRE_FALSE(ms.set_selected("nonexistent", true));
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: select_all and deselect_all",
                 "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({{"a", "Alpha"}, {"b", "Beta"}, {"c", "Charlie"}});

    ms.select_all();
    REQUIRE(ms.get_selected_count() == 3);

    ms.deselect_all();
    REQUIRE(ms.get_selected_count() == 0);
}

// ============================================================================
// Callback Tests
// ============================================================================

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: callback fires on set_selected",
                 "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({{"a", "Alpha"}, {"b", "Beta"}});

    std::string last_key;
    bool last_selected = false;
    int callback_count = 0;
    ms.set_on_change([&](const std::string& key, bool selected) {
        last_key = key;
        last_selected = selected;
        ++callback_count;
    });

    ms.set_selected("a", true);
    REQUIRE(callback_count == 1);
    REQUIRE(last_key == "a");
    REQUIRE(last_selected == true);

    ms.set_selected("a", false);
    REQUIRE(callback_count == 2);
    REQUIRE(last_key == "a");
    REQUIRE(last_selected == false);

    // No-op doesn't fire callback
    ms.set_selected("a", false);
    REQUIRE(callback_count == 2);
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: callback fires on select_all",
                 "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({{"a", "Alpha"}, {"b", "Beta", true}}); // b already selected

    int callback_count = 0;
    ms.set_on_change([&](const std::string& /*key*/, bool /*selected*/) { ++callback_count; });

    ms.select_all();
    // Only "a" should trigger callback (b was already selected)
    REQUIRE(callback_count == 1);
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: set_items without attach warns",
                 "[multiselect]") {
    UiMultiselect ms;
    // Should not crash, just warn
    ms.set_items({{"a", "Alpha"}});
    REQUIRE(ms.item_count() == 0);
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: move constructor", "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);
    ms.set_items({{"a", "Alpha", true}, {"b", "Beta"}});

    UiMultiselect ms2(std::move(ms));
    REQUIRE(ms2.is_attached());
    REQUIRE(ms2.item_count() == 2);
    REQUIRE(ms2.get_selected_count() == 1);
    REQUIRE_FALSE(ms.is_attached()); // NOLINT - testing moved-from state
}

TEST_CASE_METHOD(MultiSelectTestFixture,
                 "UiMultiselect: rows and checkboxes are addressable by name", "[multiselect]") {
    UiMultiselect ms;
    lv_obj_t* container = lv_obj_create(test_screen());
    ms.attach(container);

    ms.set_items({{"lane_a", "Lane A"}, {"lane_b", "Lane B"}});

    // The row carries the key in its name so remote control and tests can
    // reach it without knowing child indices.
    lv_obj_t* row_a = lv_obj_find_by_name(container, "item_lane_a");
    lv_obj_t* row_b = lv_obj_find_by_name(container, "item_lane_b");
    REQUIRE(row_a != nullptr);
    REQUIRE(row_b != nullptr);
    REQUIRE(row_a != row_b);

    lv_obj_t* check_a = lv_obj_find_by_name(row_a, "check");
    REQUIRE(check_a != nullptr);
    REQUIRE_FALSE(lv_obj_has_state(check_a, LV_STATE_CHECKED));

    // The checkbox itself is not clickable; the row click toggles it.
    lv_obj_send_event(row_a, LV_EVENT_CLICKED, nullptr);

    REQUIRE(lv_obj_has_state(check_a, LV_STATE_CHECKED));
    const auto keys = ms.get_selected_keys();
    REQUIRE(std::find(keys.begin(), keys.end(), "lane_a") != keys.end());
    REQUIRE(ms.get_selected_count() == 1);
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: an All row toggles every enabled row",
                 "[multiselect][batch]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    UiMultiselect ms;
    ms.attach(container);
    ms.set_select_all_label("All");
    ms.set_items({{"0", "T0"}, {"1", "T1"}, {"2", "T2"}, {"3", "T3", false, false}});

    lv_obj_t* all = lv_obj_find_by_name(container, "item_all");
    REQUIRE(all != nullptr);
    lv_obj_t* all_check = lv_obj_find_by_name(all, "check");
    REQUIRE(all_check != nullptr);
    // The All row is not an item: counts and keys see only the tools.
    REQUIRE(ms.item_count() == 4);

    lv_obj_send_event(all, LV_EVENT_CLICKED, nullptr);
    CHECK(ms.get_selected_keys() == std::vector<std::string>{"0", "1", "2"});
    CHECK(lv_obj_has_state(all_check, LV_STATE_CHECKED));

    SECTION("unchecking one tool unchecks All, rechecking it checks All again") {
        lv_obj_t* row1 = lv_obj_find_by_name(container, "item_1");
        lv_obj_send_event(row1, LV_EVENT_CLICKED, nullptr);
        CHECK_FALSE(lv_obj_has_state(all_check, LV_STATE_CHECKED));
        CHECK(ms.get_selected_count() == 2);
        lv_obj_send_event(row1, LV_EVENT_CLICKED, nullptr);
        CHECK(lv_obj_has_state(all_check, LV_STATE_CHECKED));
    }
    SECTION("All again clears every row") {
        lv_obj_send_event(all, LV_EVENT_CLICKED, nullptr);
        CHECK(ms.get_selected_count() == 0);
        CHECK_FALSE(lv_obj_has_state(all_check, LV_STATE_CHECKED));
    }
}

TEST_CASE_METHOD(MultiSelectTestFixture, "UiMultiselect: no All row unless asked",
                 "[multiselect][batch]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    UiMultiselect ms;
    ms.attach(container);
    ms.set_items({{"0", "T0"}, {"1", "T1"}});
    CHECK(lv_obj_find_by_name(container, "item_all") == nullptr);
}
