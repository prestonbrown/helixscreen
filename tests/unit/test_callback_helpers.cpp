// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_callback_helpers.cpp
 * @brief Unit tests for ui_callback_helpers.h batch registration and widget lookup helpers
 */

#include "ui_callback_helpers.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/log_capture.h"

#include <spdlog/spdlog.h>

#include <stdexcept>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Callbacks (static functions matching lv_event_cb_t signature)
// ============================================================================

static int g_callback_a_count = 0;
static int g_callback_b_count = 0;

static void test_callback_a(lv_event_t* /*e*/) {
    g_callback_a_count++;
}

static void test_callback_b(lv_event_t* /*e*/) {
    g_callback_b_count++;
}

// ============================================================================
// register_xml_callbacks Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "register_xml_callbacks registers without crash",
                 "[callback_helpers]") {
    // Registering callbacks should not crash
    REQUIRE_NOTHROW(register_xml_callbacks({
        {"test_cb_a", test_callback_a},
        {"test_cb_b", test_callback_b},
    }));

    // Verify callbacks are retrievable via LVGL XML API
    lv_event_cb_t retrieved_a = lv_xml_get_event_cb(nullptr, "test_cb_a");
    lv_event_cb_t retrieved_b = lv_xml_get_event_cb(nullptr, "test_cb_b");
    REQUIRE(retrieved_a == test_callback_a);
    REQUIRE(retrieved_b == test_callback_b);
}

TEST_CASE_METHOD(LVGLTestFixture, "register_xml_callbacks handles empty list",
                 "[callback_helpers]") {
    REQUIRE_NOTHROW(register_xml_callbacks({}));
}

TEST_CASE_METHOD(LVGLTestFixture, "register_xml_callbacks handles single entry",
                 "[callback_helpers]") {
    REQUIRE_NOTHROW(register_xml_callbacks({
        {"test_single_cb", test_callback_a},
    }));

    lv_event_cb_t retrieved = lv_xml_get_event_cb(nullptr, "test_single_cb");
    REQUIRE(retrieved == test_callback_a);
}

// Regression: bundle SSHGTVZQ (Qidi Q2 / v0.99.46 / pi). WizardWifiStep
// registered `on_network_item_clicked` globally during add-printer; when
// NetworkSettingsOverlay later tried to register the same name for clicks on
// its own wifi_network_item instances, lv_xml_register_event_cb's
// first-write-wins semantics silently dropped the second registration. Items
// created by NetworkSettingsOverlay::populate_network_list still bound to the
// wizard's static handler, which cast NetworkSettingsItemData{ssid, is_secured}
// as the larger WifiWizardNetworkItemData and SEGV'd dereferencing
// item_data->parent.
//
// This test reproduces the dispatch path: register Owner A under a shared
// name, register Owner B under the same name, then go through the same
// codepath the XML parser uses to bind a callback to a widget at instance
// creation (lv_xml_get_event_cb -> lv_obj_add_event_cb) and fire a synthetic
// click. Owner B's handler must run; Owner A's must not. Under the original
// first-write-wins behavior this test would dispatch to Owner A and fail.
TEST_CASE_METHOD(LVGLTestFixture,
                 "shared callback name: later registration wins at widget bind time",
                 "[callback_helpers][regression][bundle_SSHGTVZQ]") {
    g_callback_a_count = 0;
    g_callback_b_count = 0;

    // Owner A (e.g. WizardWifiStep) registers first.
    lv_xml_register_event_cb(nullptr, "shared_click_cb", test_callback_a);
    REQUIRE(lv_xml_get_event_cb(nullptr, "shared_click_cb") == test_callback_a);

    // Owner B (e.g. NetworkSettingsOverlay) registers the same name later.
    // Under first-write-wins this would be silently dropped — the bug.
    lv_xml_register_event_cb(nullptr, "shared_click_cb", test_callback_b);

    // Owner B then creates a widget. lv_obj_xml_event_cb_apply in the XML
    // parser does exactly this lookup-and-bind pair at instance creation, so
    // we mimic it directly.
    lv_obj_t* item = lv_obj_create(test_screen());
    lv_event_cb_t bound = lv_xml_get_event_cb(nullptr, "shared_click_cb");
    REQUIRE(bound == test_callback_b);
    lv_obj_add_event_cb(item, bound, LV_EVENT_CLICKED, nullptr);

    lv_obj_send_event(item, LV_EVENT_CLICKED, nullptr);

    REQUIRE(g_callback_b_count == 1);
    REQUIRE(g_callback_a_count == 0);
}

// ============================================================================
// Table lambdas: the exception guard and the event readers
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a lambda table entry resolves and fires like a function entry",
                 "[callback_helpers]") {
    static int lambda_hits = 0;
    lambda_hits = 0;
    register_xml_callbacks({
        {"test_cb_fn_entry", test_callback_a},
        {"test_cb_lambda_entry", [](lv_event_t*) { ++lambda_hits; }},
    });
    CHECK(lv_xml_get_event_cb(nullptr, "test_cb_fn_entry") == test_callback_a);
    lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, "test_cb_lambda_entry");
    REQUIRE(cb != nullptr);

    lv_obj_t* btn = lv_obj_create(test_screen());
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    CHECK(lambda_hits == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "two lambda entries in one table each run their own body",
                 "[callback_helpers]") {
    static int first_hits = 0;
    static int second_hits = 0;
    first_hits = 0;
    second_hits = 0;
    register_xml_callbacks({
        {"test_cb_lambda_first", [](lv_event_t*) { ++first_hits; }},
        {"test_cb_lambda_second", [](lv_event_t*) { ++second_hits; }},
    });

    lv_obj_t* btn = lv_obj_create(test_screen());
    lv_obj_add_event_cb(btn, lv_xml_get_event_cb(nullptr, "test_cb_lambda_first"), LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    CHECK(first_hits == 1);
    CHECK(second_hits == 0);

    lv_obj_t* other = lv_obj_create(test_screen());
    lv_obj_add_event_cb(other, lv_xml_get_event_cb(nullptr, "test_cb_lambda_second"),
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(other, LV_EVENT_CLICKED, nullptr);
    CHECK(first_hits == 1);
    CHECK(second_hits == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "a throwing lambda entry logs its callback name and returns",
                 "[callback_helpers]") {
    register_xml_callbacks({
        {"test_cb_throwing_entry",
         [](lv_event_t*) { throw std::runtime_error("boom from the handler"); }},
    });
    lv_obj_t* btn = lv_obj_create(test_screen());
    lv_obj_add_event_cb(btn, lv_xml_get_event_cb(nullptr, "test_cb_throwing_entry"),
                        LV_EVENT_CLICKED, nullptr);

    helix::LogCapture log;
    REQUIRE_NOTHROW(lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr));
    CHECK(log.has_line_with({"test_cb_throwing_entry", "boom from the handler"}));
}

TEST_CASE_METHOD(LVGLTestFixture, "event_checked and event_selected read the event's widget",
                 "[callback_helpers]") {
    static bool checked = false;
    static int selected = -1;

    lv_obj_t* toggle = lv_obj_create(test_screen());
    lv_obj_add_event_cb(
        toggle, [](lv_event_t* e) { checked = helix::ui::event_checked(e); }, LV_EVENT_CLICKED,
        nullptr);
    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_CLICKED, nullptr);
    CHECK(checked);
    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(checked);

    lv_obj_t* dropdown = lv_dropdown_create(test_screen());
    lv_dropdown_set_options(dropdown, "a\nb\nc");
    lv_dropdown_set_selected(dropdown, 2);
    lv_obj_add_event_cb(
        dropdown, [](lv_event_t* e) { selected = helix::ui::event_selected(e); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(selected == 2);
}
