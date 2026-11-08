// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include <array>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget config defaults", "[print_status][config]") {
    PrintStatusWidget w;
    nlohmann::json empty = nlohmann::json::object();
    w.set_config(empty);
    REQUIRE(w.layout_style_for_test() == "library");
    REQUIRE(w.nozzle_tool_override_for_test() == "auto");
}

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget config sets detailed",
                 "[print_status][config]") {
    PrintStatusWidget w;
    nlohmann::json cfg = {{"layout_style", "detailed"}, {"nozzle_tool_override", "extruder1"}};
    w.set_config(cfg);
    REQUIRE(w.layout_style_for_test() == "detailed");
    // 'extruder1' doesn't resolve in this fixture (no multi-extruder PrinterState
    // init), so set_config's formatter dispatch returns false and the widget
    // clears the stale override back to 'auto'. That's the expected fallback —
    // see DetailedFormatter::set_nozzle_tool_override and the matching
    // ghost-tool clean-up in PrintStatusWidget::set_config.
    REQUIRE(w.nozzle_tool_override_for_test() == "auto");
}

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget rejects invalid layout_style",
                 "[print_status][config]") {
    PrintStatusWidget w;
    nlohmann::json cfg = {{"layout_style", "bogus"}};
    w.set_config(cfg);
    REQUIRE(w.layout_style_for_test() == "library"); // unchanged
}

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget show options need a JSON boolean",
                 "[print_status][config]") {
    PrintStatusWidget w;
    w.set_config({{"show_title", false},
                  {"show_print_files", 0},
                  {"show_reprint_last", "false"},
                  {"show_recent_prints", nullptr},
                  {"show_job_queue", false}});
    CHECK(w.show_options_for_test() == std::array<bool, 5>{false, true, true, true, false});

    w.set_config({{"show_title", true}, {"show_job_queue", 1}});
    CHECK(w.show_options_for_test() == std::array<bool, 5>{true, true, true, true, false});
}

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget ignores a non-string layout_style",
                 "[print_status][config]") {
    PrintStatusWidget w;
    w.set_config({{"layout_style", "detailed"}});
    w.set_config({{"layout_style", 1}, {"nozzle_tool_override", 2}});
    CHECK(w.layout_style_for_test() == "detailed");
    CHECK(w.nozzle_tool_override_for_test() == "auto");
}
