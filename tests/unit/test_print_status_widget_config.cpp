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

TEST_CASE_METHOD(HelixTestFixture, "PrintStatusWidget config skips unknown and wrong-typed values",
                 "[print_status][config]") {
    using Flags = std::array<bool, 5>; // title, files, reprint, recent, queue
    PrintStatusWidget w;
    w.set_config({{"layout_style", "bogus"}, {"show_title", false}, {"show_print_files", 0}});
    CHECK(w.layout_style_for_test() == "library");
    CHECK(w.show_options_for_test() == Flags{false, true, true, true, true});
    w.set_config({{"layout_style", "detailed"}});
    w.set_config({{"layout_style", 1}, {"show_title", true}, {"show_job_queue", "false"}});
    CHECK(w.layout_style_for_test() == "detailed");
    CHECK(w.show_options_for_test() == Flags{true, true, true, true, true});
}
