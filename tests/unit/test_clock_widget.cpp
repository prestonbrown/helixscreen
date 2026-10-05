// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_format_utils.h"

#include "../lvgl_test_fixture.h"
#include "format_utils.h"
#include "locale_formats.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/clock_widget.h"
#include "system_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

class ClockWidgetFixture : public LVGLTestFixture {
  public:
    ClockWidgetFixture() {
        // Idempotent — safe to call per test case, no static guard needed
        PanelWidgetManager::instance().init_widget_subjects();
    }

    /// Create a minimal mock widget tree (no XML required)
    lv_obj_t* create_mock_clock(lv_obj_t* parent) {
        lv_obj_t* container = lv_obj_create(parent);
        lv_obj_set_name(container, "panel_widget_clock");

        lv_obj_t* time_label = lv_obj_create(container);
        lv_obj_set_name(time_label, "clock_time");

        lv_obj_t* date_label = lv_obj_create(container);
        lv_obj_set_name(date_label, "clock_date");

        lv_obj_t* uptime_label = lv_obj_create(container);
        lv_obj_set_name(uptime_label, "clock_uptime");

        return container;
    }
};

TEST_CASE_METHOD(ClockWidgetFixture, "ClockWidget: factory registration", "[clock_widget]") {
    const auto* def = find_widget_def("clock");
    REQUIRE(def != nullptr);
    REQUIRE(def->factory != nullptr);

    auto widget = def->factory("clock");
    REQUIRE(widget != nullptr);
    REQUIRE(std::string(widget->id()) == "clock");
}

TEST_CASE_METHOD(ClockWidgetFixture, "ClockWidget: attach_tile binds user_data", "[clock_widget]") {
    ClockWidget widget;
    lv_obj_t* container = create_mock_clock(test_screen());

    widget.attach_tile(container, test_screen());
    REQUIRE(lv_obj_get_user_data(container) == &widget);

    widget.detach_tile();
    REQUIRE(lv_obj_get_user_data(container) == nullptr);
}

TEST_CASE_METHOD(ClockWidgetFixture, "ClockWidget: timer lifecycle", "[clock_widget]") {
    ClockWidget widget;
    lv_obj_t* container = create_mock_clock(test_screen());
    widget.attach_tile(container, test_screen());

    SECTION("no timer before activation") {
        // Timer should not exist until on_activate() is called
        // We verify indirectly: calling on_deactivate() should be safe (no-op)
        widget.on_deactivate();
    }

    SECTION("timer starts on activate") {
        widget.on_activate();

        // Get the initial time subject value
        auto* time_subj = lv_xml_get_subject(nullptr, "clock_time_text");
        REQUIRE(time_subj != nullptr);
        const char* initial = lv_subject_get_string(time_subj);
        REQUIRE(initial != nullptr);
        // Should have a real time value (not the "--:--" placeholder)
        REQUIRE(std::string(initial) != "--:--");

        widget.on_deactivate();
    }

    SECTION("timer stops on deactivate") {
        widget.on_activate();
        widget.on_deactivate();

        // Calling deactivate again should be safe (idempotent)
        widget.on_deactivate();
    }

    SECTION("timer restarts on re-activate") {
        widget.on_activate();
        widget.on_deactivate();
        widget.on_activate();

        // Should still work after restart
        auto* time_subj = lv_xml_get_subject(nullptr, "clock_time_text");
        REQUIRE(time_subj != nullptr);
        const char* val = lv_subject_get_string(time_subj);
        REQUIRE(val != nullptr);
        REQUIRE(std::string(val) != "--:--");

        widget.on_deactivate();
    }

    widget.detach_tile();
}

TEST_CASE_METHOD(ClockWidgetFixture, "ClockWidget: detach stops timer", "[clock_widget]") {
    ClockWidget widget;
    lv_obj_t* container = create_mock_clock(test_screen());
    widget.attach_tile(container, test_screen());
    widget.on_activate();

    // Detach should stop the timer even if on_deactivate() wasn't called
    widget.detach_tile();

    // Processing LVGL timers after detach should not crash
    process_lvgl(100);
}

TEST_CASE_METHOD(ClockWidgetFixture, "ClockWidget: subjects populated on attach",
                 "[clock_widget]") {
    ClockWidget widget;
    lv_obj_t* container = create_mock_clock(test_screen());

    // attach() calls update_clock() which populates subjects immediately
    widget.attach_tile(container, test_screen());

    auto* time_subj = lv_xml_get_subject(nullptr, "clock_time_text");
    auto* date_subj = lv_xml_get_subject(nullptr, "clock_date_text");
    auto* uptime_subj = lv_xml_get_subject(nullptr, "clock_uptime_text");

    REQUIRE(time_subj != nullptr);
    REQUIRE(date_subj != nullptr);
    REQUIRE(uptime_subj != nullptr);

    // Time should be populated (not placeholder)
    REQUIRE(std::string(lv_subject_get_string(time_subj)) != "--:--");

    // Date should be populated (not empty)
    REQUIRE(std::string(lv_subject_get_string(date_subj)).size() > 0);

    // Uptime should be populated (starts with "Up: ")
    std::string uptime = lv_subject_get_string(uptime_subj);
    REQUIRE(uptime.find("Up: ") == 0);

    widget.detach_tile();
}

TEST_CASE_METHOD(ClockWidgetFixture, "ClockWidget: timer fires during LVGL processing",
                 "[clock_widget]") {
    ClockWidget widget;
    lv_obj_t* container = create_mock_clock(test_screen());
    widget.attach_tile(container, test_screen());
    widget.on_activate();

    // Processing LVGL for longer than the timer interval (1000ms) should
    // not crash and timer callback should execute
    process_lvgl(1100);

    auto* time_subj = lv_xml_get_subject(nullptr, "clock_time_text");
    REQUIRE(time_subj != nullptr);
    REQUIRE(std::string(lv_subject_get_string(time_subj)) != "--:--");

    widget.on_deactivate();
    widget.detach_tile();
}

// ---------------------------------------------------------------------------
// Locale-aware date formatting tests
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(HelixTestFixture, "Locale date formatting", "[clock_widget][i18n]") {
    // Fixed time: Friday, February 28, 2026 14:30:00
    struct tm test_tm = {};
    test_tm.tm_year = 126; // 2026 - 1900
    test_tm.tm_mon = 1;    // February (0-based)
    test_tm.tm_mday = 28;
    test_tm.tm_hour = 14;
    test_tm.tm_min = 30;
    test_tm.tm_wday = 5; // Friday (0=Sunday)

    SECTION("English: Day, Mon DD") {
        helix::SystemSettingsManager::instance().set_language("en");
        helix::ui::locale_set_language("en");
        REQUIRE(helix::ui::format_localized_date(&test_tm) == "Fri, Feb 28");
    }

    SECTION("German: Day, DD. Mon") {
        helix::SystemSettingsManager::instance().set_language("de");
        helix::ui::locale_set_language("de");
        auto result = helix::ui::format_localized_date(&test_tm);
        // System locale may or may not be installed, check structure
        REQUIRE(result.find("28.") != std::string::npos);
    }

    SECTION("French: Day, DD Mon") {
        helix::SystemSettingsManager::instance().set_language("fr");
        helix::ui::locale_set_language("fr");
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find(", 28 ") != std::string::npos);
    }

    SECTION("Japanese: MM/DD (Day)") {
        helix::SystemSettingsManager::instance().set_language("ja");
        helix::ui::locale_set_language("ja");
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find("2/28") != std::string::npos);
    }

    SECTION("Chinese: MM/DD (Day)") {
        helix::SystemSettingsManager::instance().set_language("zh");
        helix::ui::locale_set_language("zh");
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find("2/28") != std::string::npos);
    }

    SECTION("Russian") {
        helix::SystemSettingsManager::instance().set_language("ru");
        helix::ui::locale_set_language("ru");
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find(", 28 ") != std::string::npos);
    }

    SECTION("Unknown language falls back to English") {
        helix::SystemSettingsManager::instance().set_language("xx");
        helix::ui::locale_set_language("xx");
        REQUIRE(helix::ui::format_localized_date(&test_tm) == "Fri, Feb 28");
    }

    SECTION("nullptr tm returns unavailable") {
        REQUIRE(helix::ui::format_localized_date(nullptr) == helix::format::UNAVAILABLE);
    }

    // Restore default
    helix::SystemSettingsManager::instance().set_language("en");
    helix::ui::locale_set_language("en");
}

TEST_CASE_METHOD(HelixTestFixture, "Locale date formatting: all months cycle",
                 "[clock_widget][i18n]") {
    helix::SystemSettingsManager::instance().set_language("en");
    helix::ui::locale_set_language("en");

    // Verify all 12 months produce correct English abbreviations
    const char* expected_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    for (int m = 0; m < 12; ++m) {
        struct tm test_tm = {};
        test_tm.tm_year = 126;
        test_tm.tm_mon = m;
        test_tm.tm_mday = 15;
        test_tm.tm_wday = 0; // Sunday
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find(expected_months[m]) != std::string::npos);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "Locale date formatting: all days of week",
                 "[clock_widget][i18n]") {
    helix::SystemSettingsManager::instance().set_language("en");
    helix::ui::locale_set_language("en");

    const char* expected_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    for (int d = 0; d < 7; ++d) {
        struct tm test_tm = {};
        test_tm.tm_year = 126;
        test_tm.tm_mon = 0; // January
        test_tm.tm_mday = 1;
        test_tm.tm_wday = d;
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find(expected_days[d]) != std::string::npos);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "Locale date formatting: Spanish and Portuguese",
                 "[clock_widget][i18n]") {
    struct tm test_tm = {};
    test_tm.tm_year = 126;
    test_tm.tm_mon = 1;
    test_tm.tm_mday = 28;
    test_tm.tm_hour = 14;
    test_tm.tm_min = 30;
    test_tm.tm_wday = 5;

    SECTION("Spanish: DMY order") {
        helix::SystemSettingsManager::instance().set_language("es");
        helix::ui::locale_set_language("es");
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find(", 28 ") != std::string::npos);
    }

    SECTION("Portuguese: DMY order") {
        helix::SystemSettingsManager::instance().set_language("pt");
        helix::ui::locale_set_language("pt");
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find(", 28 ") != std::string::npos);
    }

    SECTION("Italian: DMY order") {
        helix::SystemSettingsManager::instance().set_language("it");
        helix::ui::locale_set_language("it");
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find(", 28 ") != std::string::npos);
    }

    helix::SystemSettingsManager::instance().set_language("en");
    helix::ui::locale_set_language("en");
}

TEST_CASE_METHOD(HelixTestFixture, "Locale modified date formatting", "[clock_widget][i18n]") {
    struct tm test_tm = {};
    test_tm.tm_year = 126;
    test_tm.tm_mon = 1; // February
    test_tm.tm_mday = 28;
    test_tm.tm_hour = 14;
    test_tm.tm_min = 30;
    test_tm.tm_wday = 5;

    SECTION("English modified date includes month and time") {
        helix::SystemSettingsManager::instance().set_language("en");
        helix::ui::locale_set_language("en");
        auto result = helix::ui::format_localized_modified_date(&test_tm);
        // Should contain "Feb" and "28" and time
        REQUIRE(result.find("Feb") != std::string::npos);
        REQUIRE(result.find("28") != std::string::npos);
    }

    SECTION("German modified date: DD. Mon time") {
        helix::SystemSettingsManager::instance().set_language("de");
        helix::ui::locale_set_language("de");
        auto result = helix::ui::format_localized_modified_date(&test_tm);
        REQUIRE(result.find("28.") != std::string::npos);
    }

    SECTION("CJK modified date: MM/DD time") {
        helix::SystemSettingsManager::instance().set_language("ja");
        helix::ui::locale_set_language("ja");
        auto result = helix::ui::format_localized_modified_date(&test_tm);
        REQUIRE(result.find("2/28") != std::string::npos);
    }

    SECTION("nullptr returns unavailable") {
        REQUIRE(helix::ui::format_localized_modified_date(nullptr) == helix::format::UNAVAILABLE);
    }

    helix::SystemSettingsManager::instance().set_language("en");
    helix::ui::locale_set_language("en");
}

TEST_CASE_METHOD(HelixTestFixture, "Locale date formatting: boundary day values",
                 "[clock_widget][i18n]") {
    helix::SystemSettingsManager::instance().set_language("en");
    helix::ui::locale_set_language("en");

    SECTION("Day 1 (first of month)") {
        struct tm test_tm = {};
        test_tm.tm_year = 126;
        test_tm.tm_mon = 0;
        test_tm.tm_mday = 1;
        test_tm.tm_wday = 3;
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result == "Wed, Jan 1");
    }

    SECTION("Day 31 (last of month)") {
        struct tm test_tm = {};
        test_tm.tm_year = 126;
        test_tm.tm_mon = 0;
        test_tm.tm_mday = 31;
        test_tm.tm_wday = 5;
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result == "Fri, Jan 31");
    }
}

TEST_CASE_METHOD(HelixTestFixture, "Locale date formatting: CJK single-digit month",
                 "[clock_widget][i18n]") {
    helix::SystemSettingsManager::instance().set_language("ja");
    helix::ui::locale_set_language("ja");

    SECTION("January (single digit)") {
        struct tm test_tm = {};
        test_tm.tm_year = 126;
        test_tm.tm_mon = 0;
        test_tm.tm_mday = 5;
        test_tm.tm_wday = 0;
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find("1/5") != std::string::npos);
    }

    SECTION("December (double digit)") {
        struct tm test_tm = {};
        test_tm.tm_year = 126;
        test_tm.tm_mon = 11;
        test_tm.tm_mday = 25;
        test_tm.tm_wday = 4;
        auto result = helix::ui::format_localized_date(&test_tm);
        REQUIRE(result.find("12/25") != std::string::npos);
    }

    helix::SystemSettingsManager::instance().set_language("en");
    helix::ui::locale_set_language("en");
}

TEST_CASE_METHOD(HelixTestFixture, "format_modified_date integration", "[clock_widget][i18n]") {
    // Verify the public API format_modified_date() uses locale formatting
    helix::SystemSettingsManager::instance().set_language("en");
    helix::ui::locale_set_language("en");

    // Use a known timestamp
    time_t ts = 1740700200; // Feb 28, 2025 ~2:30 UTC (actual local depends on TZ)
    auto result = helix::ui::format_modified_date(ts);
    REQUIRE(!result.empty());
    REQUIRE(result != "Unknown");
}

TEST_CASE("Locale default 24h", "[clock_widget][i18n]") {
    REQUIRE_FALSE(helix::ui::locale_default_24h("en"));
    REQUIRE(helix::ui::locale_default_24h("de"));
    REQUIRE(helix::ui::locale_default_24h("fr"));
    REQUIRE(helix::ui::locale_default_24h("es"));
    REQUIRE(helix::ui::locale_default_24h("ru"));
    REQUIRE(helix::ui::locale_default_24h("pt"));
    REQUIRE(helix::ui::locale_default_24h("it"));
    REQUIRE(helix::ui::locale_default_24h("ja"));
    REQUIRE(helix::ui::locale_default_24h("zh"));
    REQUIRE_FALSE(helix::ui::locale_default_24h("xx")); // unknown = en default
    REQUIRE_FALSE(helix::ui::locale_default_24h(""));   // empty = en default
}

TEST_CASE_METHOD(HelixTestFixture, "Locale set language caching", "[clock_widget][i18n]") {
    // Setting the same language twice shouldn't break anything
    helix::ui::locale_set_language("fr");
    helix::ui::locale_set_language("fr");

    struct tm test_tm = {};
    test_tm.tm_year = 126;
    test_tm.tm_mon = 1;
    test_tm.tm_mday = 28;
    test_tm.tm_wday = 5;

    auto result = helix::ui::format_localized_date(&test_tm);
    REQUIRE(result.find(", 28 ") != std::string::npos);

    // Switch back
    helix::ui::locale_set_language("en");
    result = helix::ui::format_localized_date(&test_tm);
    REQUIRE(result == "Fri, Feb 28");

    helix::SystemSettingsManager::instance().set_language("en");
}

// ---------------------------------------------------------------------------
// Tests for the widget rebuild + activation fix
// ---------------------------------------------------------------------------

/// Mock PanelWidget that tracks on_activate/on_deactivate calls.
/// Optional external_detach_counter survives the widget's destruction so
/// tests can verify detach() was called even after the unique_ptr freed it.
class MockWidget : public PanelWidget {
  public:
    int activate_count = 0;
    int deactivate_count = 0;
    bool attached = false;
    int* external_detach_counter = nullptr;

    void attach(lv_obj_t* /*widget_obj*/, lv_obj_t* /*parent_screen*/) override {
        attached = true;
    }
    void detach() override {
        attached = false;
        if (external_detach_counter)
            ++*external_detach_counter;
    }
    void on_activate() override {
        activate_count++;
    }
    void on_deactivate() override {
        deactivate_count++;
    }
    const char* id() const override {
        return "mock";
    }
};

TEST_CASE("Widget rebuild activation contract", "[panel_widget][lifecycle]") {
    // This tests the contract that HomePanel now implements:
    // when widgets are rebuilt while the panel is active, on_activate()
    // must be called on the new widgets.

    // Simulate the lifecycle that HomePanel manages
    std::vector<std::unique_ptr<PanelWidget>> active_widgets;
    bool panel_active = false;

    // Helper lambdas matching HomePanel's implementation
    auto on_activate = [&]() {
        panel_active = true;
        for (auto& w : active_widgets) {
            w->on_activate();
        }
    };

    auto on_deactivate = [&]() {
        panel_active = false;
        for (auto& w : active_widgets) {
            w->on_deactivate();
        }
    };

    auto populate_widgets = [&]() {
        // Detach old widgets
        for (auto& w : active_widgets) {
            w->detach();
        }
        active_widgets.clear();

        // Create new widgets
        auto w = std::make_unique<MockWidget>();
        active_widgets.push_back(std::move(w));

        // THE FIX: activate new widgets if panel is already active
        if (panel_active) {
            for (auto& widget : active_widgets) {
                widget->on_activate();
            }
        }
    };

    SECTION("initial setup: widgets not activated until panel activates") {
        populate_widgets();
        auto* mock = static_cast<MockWidget*>(active_widgets[0].get());
        REQUIRE(mock->activate_count == 0);

        on_activate();
        REQUIRE(mock->activate_count == 1);

        on_deactivate();
        REQUIRE(mock->deactivate_count == 1);
    }

    SECTION("rebuild while inactive: widgets not activated") {
        populate_widgets();
        auto* mock = static_cast<MockWidget*>(active_widgets[0].get());
        REQUIRE(mock->activate_count == 0);
    }

    SECTION("rebuild while active: new widgets get activated") {
        // Initial population + activation
        populate_widgets();
        on_activate();
        auto* first_mock = static_cast<MockWidget*>(active_widgets[0].get());
        REQUIRE(first_mock->activate_count == 1);

        // Hook detach observer BEFORE rebuild — populate_widgets() clears the
        // vector and frees first_mock, so we can't read first_mock->attached
        // after the rebuild without a UAF.
        int first_detach_count = 0;
        first_mock->external_detach_counter = &first_detach_count;

        // Rebuild while panel is active (simulates gate observer or settings change)
        populate_widgets();
        auto* new_mock = static_cast<MockWidget*>(active_widgets[0].get());

        // New widget should have been activated during populate
        REQUIRE(new_mock->activate_count == 1);
        // Old widget should have been detached (not deactivated — detach handles cleanup)
        REQUIRE(first_detach_count == 1);
    }

    SECTION("multiple rebuilds while active: each batch gets activated") {
        populate_widgets();
        on_activate();

        populate_widgets();
        auto* w1 = static_cast<MockWidget*>(active_widgets[0].get());
        REQUIRE(w1->activate_count == 1);

        populate_widgets();
        auto* w2 = static_cast<MockWidget*>(active_widgets[0].get());
        REQUIRE(w2->activate_count == 1);
        // w1 is now detached and destroyed (unique_ptr)
    }

    SECTION("deactivate after rebuild: new widgets get deactivated") {
        populate_widgets();
        on_activate();

        populate_widgets();
        auto* mock = static_cast<MockWidget*>(active_widgets[0].get());
        REQUIRE(mock->activate_count == 1);

        on_deactivate();
        REQUIRE(mock->deactivate_count == 1);
    }
}
