// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_console_widget_tail.cpp
 * @brief The console tile's live output tail.
 *
 * At two cells tall or more the tile shows the newest console lines, fed by
 * server.gcode_store and notify_gcode_response through the same conversion and
 * filtering the console overlay uses. Below that it is the icon tile.
 *
 * Run with: ./build/bin/helix-tests "[gcode_console_tail]"
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_overlay_cache_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "app_globals.h"
#include "console_line.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Two cells wide at a generous height, so every kept line has a visible row.
constexpr int kW = 240;
constexpr int kTallH = 600;

class ConsoleTailFixture : public LVGLUITestFixture {
  public:
    ConsoleTailFixture() {
        helix::init_widget_registrations();
        set_moonraker_api(&api_);
    }

    ~ConsoleTailFixture() override {
        set_moonraker_api(previous_api_);
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api_{client_, get_printer_state()};
    IMoonrakerAPI* previous_api_ = get_moonraker_api();
};

int view() {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, "gcode_console_tail_view"));
}

std::string row_text(lv_obj_t* row) {
    std::string text;
    const uint32_t n = lv_spangroup_get_span_count(row);
    for (uint32_t i = 0; i < n; ++i) {
        text += lv_span_get_text(lv_spangroup_get_child(row, static_cast<int32_t>(i)));
    }
    return text;
}

/// Visible rows, oldest first, as their drawn text.
std::vector<std::string> rows(PanelWidgetHarnessBase& h) {
    lv_obj_t* container = h.child("gcode_console_tail_rows");
    REQUIRE(container != nullptr);
    std::vector<std::string> out;
    const uint32_t n = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < n; ++i) {
        out.push_back(row_text(lv_obj_get_child(container, static_cast<int32_t>(i))));
    }
    return out;
}

bool hidden(PanelWidgetHarnessBase& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// TEST_MIRROR_OK: contains() is a one-line assertion utility (vector membership), not logic
// under test
bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE_METHOD(ConsoleTailFixture, "console tile shows the tail only at two cells tall",
                 "[gcode_console_tail][panel_widget]") {
    PanelWidgetHarness<GCodeConsoleWidget> h(test_screen());

    SECTION("one cell tall is the icon tile") {
        h.resize(4, 2, kW, 120);
        drain();
        CHECK(view() == 0);
        CHECK(hidden(h, "gcode_console_tail"));
        CHECK_FALSE(hidden(h, "gcode_console_icon_face"));
    }

    SECTION("three tracks tall is still the icon tile") {
        h.resize(4, 3, kW, 180);
        drain();
        CHECK(view() == 0);
        CHECK(hidden(h, "gcode_console_tail"));
    }

    SECTION("two cells tall is the tail, with the icon hidden") {
        h.resize(4, 4, kW, 240);
        drain();
        CHECK(view() == 2);
        CHECK_FALSE(hidden(h, "gcode_console_tail"));
        CHECK(hidden(h, "gcode_console_icon_face"));
        CHECK_FALSE(rows(h).empty());
    }

    SECTION("one cell wide by two tall is the tail too") {
        h.resize(2, 4, 120, 240);
        drain();
        CHECK(view() == 2);
    }

    SECTION("half a cell wide stays the icon tile") {
        h.resize(1, 4, 60, 240);
        drain();
        CHECK(view() == 0);
    }

    SECTION("shrinking back drops the tail and its rows") {
        h.resize(4, 4, kW, 240);
        drain();
        REQUIRE(view() == 2);
        h.resize(4, 2, kW, 120);
        drain();
        CHECK(view() == 0);
        CHECK(rows(h).empty());
        CHECK(GCodeConsoleWidgetTestAccess::handler_name(h.widget()).empty());
    }
}

TEST_CASE_METHOD(ConsoleTailFixture, "console tail history is filtered and newest-last",
                 "[gcode_console_tail][panel_widget]") {
    REQUIRE(SettingsManager::instance().get_console_filter_temps());

    PanelWidgetHarness<GCodeConsoleWidget> h(test_screen());
    h.resize(4, 8, kW, kTallH);
    drain();

    const auto shown = rows(h);
    INFO("rows: " << shown.size());
    REQUIRE(shown.size() >= 3);

    // The mock store's bed temperature reports are what the default filter hides.
    for (const auto& line : shown) {
        INFO(line);
        CHECK_FALSE(helix::ui::is_console_temp_message(line));
    }

    // Commands carry the overlay's prompt, and the store's order is kept with
    // the newest line at the bottom.
    auto pos = [&](const std::string& s) {
        return std::find(shown.begin(), shown.end(), s) - shown.begin();
    };
    REQUIRE(contains(shown, "> G28"));
    REQUIRE(contains(shown, "> RESTART"));
    REQUIRE(contains(shown, "!! Error: MCU protocol error"));
    CHECK(pos("> G28") < pos("!! Error: MCU protocol error"));
    CHECK(pos("!! Error: MCU protocol error") < pos("> RESTART"));
    CHECK(shown.back() == "ok");
}

TEST_CASE_METHOD(ConsoleTailFixture, "console tail appends live responses at the bottom",
                 "[gcode_console_tail][panel_widget]") {
    PanelWidgetHarness<GCodeConsoleWidget> h(test_screen());
    h.resize(4, 8, kW, kTallH);
    drain();

    client_.dispatch_gcode_response("// probe at 150.000,150.000 is z=1.234");
    drain();
    CHECK(rows(h).back() == "// probe at 150.000,150.000 is z=1.234");

    // A bare "ok" and a temperature report are the overlay's live-path noise.
    client_.dispatch_gcode_response("ok");
    client_.dispatch_gcode_response("ok T:210.0 /210.0 B:60.0 /60.0");
    drain();
    CHECK(rows(h).back() == "// probe at 150.000,150.000 is z=1.234");
}

TEST_CASE_METHOD(ConsoleTailFixture, "console tail stops listening while Home is hidden",
                 "[gcode_console_tail][panel_widget]") {
    PanelWidgetHarness<GCodeConsoleWidget> h(test_screen());
    h.resize(4, 8, kW, kTallH);
    drain();
    REQUIRE_FALSE(GCodeConsoleWidgetTestAccess::handler_name(h.widget()).empty());
    const size_t before = GCodeConsoleWidgetTestAccess::lines(h.widget()).size();

    h.widget().on_deactivate();
    CHECK(GCodeConsoleWidgetTestAccess::handler_name(h.widget()).empty());
    client_.dispatch_gcode_response("// while hidden");
    drain();
    CHECK(GCodeConsoleWidgetTestAccess::lines(h.widget()).size() == before);

    h.widget().on_activate();
    drain();
    CHECK_FALSE(GCodeConsoleWidgetTestAccess::handler_name(h.widget()).empty());
    client_.dispatch_gcode_response("// back on screen");
    drain();
    CHECK(rows(h).back() == "// back on screen");
}

TEST_CASE_METHOD(ConsoleTailFixture, "console tail keeps a bounded ring of lines",
                 "[gcode_console_tail][panel_widget]") {
    PanelWidgetHarness<GCodeConsoleWidget> h(test_screen());
    h.resize(4, 4, kW, 240);
    drain();

    const int total = static_cast<int>(GCodeConsoleWidget::MAX_LINES) + 30;
    for (int i = 0; i < total; ++i) {
        client_.dispatch_gcode_response("// line " + std::to_string(i));
    }
    drain();

    const auto& lines = GCodeConsoleWidgetTestAccess::lines(h.widget());
    REQUIRE(lines.size() == GCodeConsoleWidget::MAX_LINES);
    CHECK(lines.back().message == "// line " + std::to_string(total - 1));
    CHECK(lines.front().message ==
          "// line " + std::to_string(total - static_cast<int>(GCodeConsoleWidget::MAX_LINES)));

    // Only what fits is drawn, and the newest is the bottom row.
    const auto shown = rows(h);
    CHECK(shown.size() < GCodeConsoleWidget::MAX_LINES);
    CHECK(shown.back() == "// line " + std::to_string(total - 1));
}

TEST_CASE_METHOD(ConsoleTailFixture, "console tail survives teardown with work still queued",
                 "[gcode_console_tail][panel_widget]") {
    std::string handler;
    {
        PanelWidgetHarness<GCodeConsoleWidget> h(test_screen());
        // The mock answers gcode_store inline, so the history reply and this
        // live line are both queued, not yet run, when the widget goes away.
        h.resize(4, 4, kW, 240);
        client_.dispatch_gcode_response("// queued behind teardown");
        handler = GCodeConsoleWidgetTestAccess::handler_name(h.widget());
        REQUIRE_FALSE(handler.empty());
    }
    drain();

    // Unsubscribed on the way out: nothing is left to remove.
    CHECK_FALSE(api_.unregister_method_callback("notify_gcode_response", handler));
    client_.dispatch_gcode_response("// after teardown");
    drain();
}

TEST_CASE("console line spans: prompt, line colour and span markup",
          "[gcode_console_tail][console]") {
    using helix::ui::console_line_spans;

    auto cmd = console_line_spans("G28", true, false);
    REQUIRE(cmd.size() == 2);
    CHECK(cmd[0].text == "> ");
    CHECK(cmd[1].text == "G28");
    CHECK(std::string(cmd[1].color_token) == "text");

    auto err = console_line_spans("!! Timer too close", false, true);
    REQUIRE(err.size() == 1);
    CHECK(std::string(err[0].color_token) == "danger");

    auto ok = console_line_spans("// Klipper state: Ready", false, false);
    REQUIRE(ok.size() == 1);
    CHECK(std::string(ok[0].color_token) == "success");

    auto afc = console_line_spans(
        "lane1 <span class=error--text>JAM</span> <span class=info--text>ready</span>", false,
        false);
    REQUIRE(afc.size() == 4);
    CHECK(std::string(afc[0].color_token) == "success");
    CHECK(afc[1].text == "JAM");
    CHECK(std::string(afc[1].color_token) == "danger");
    CHECK(std::string(afc[3].color_token) == "info");
}
