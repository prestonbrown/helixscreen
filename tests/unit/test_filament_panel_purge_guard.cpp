// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A purge preamble (home, then travel to the purge bin) runs under the homing
// budget, which is longer than the panel's operation timeout. The operation
// guard has to outlast the preamble, or the panel times the purge out while the
// toolhead is still on its way to the bin and then purges anyway.

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_macro_harness.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "ams_backend_mock.h"
#include "i_moonraker_api.h"
#include "printer_discovery.h"
#include "standard_macros.h"

#include <lvgl.h>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using TA = helix::ui::FilamentPanelTestAccess;
using helix::test::FilamentPanelMacroHarness;

namespace {

struct BinBackend : helix::AmsBackendMock {
    BinBackend() : helix::AmsBackendMock(2) {}
    std::string purge_preamble_gcode(bool) const override {
        return "TEST_BIN_PREAMBLE";
    }
};

/// Runs LVGL's clock forward by @p ms in one step, firing whatever came due.
void advance_clock(uint32_t ms) {
    lv_tick_inc(ms);
    lv_timer_handler();
    helix::ui::UpdateQueue::instance().drain();
}

/// Waits out the panel's own operation budget while the preamble is unanswered.
void outlast_operation_budget() {
    advance_clock(TA::kOperationTimeoutMs + 1000);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "A purge macro's guard outlasts a slow bin preamble",
                 "[filament][purge_preamble][op_timeout_expire]") {
    FilamentPanelMacroHarness h(std::make_unique<BinBackend>());
    h.cache_macros({{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\nM109 S{t}"}});
    h.set_extruder_target(255.0);

    h.client.defer_next("printer.gcode.script");
    TA::execute_purge(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.client.last_send_script().find("TEST_BIN_PREAMBLE") != std::string::npos);
    REQUIRE(TA::operation_active(*h.panel));

    outlast_operation_budget();
    CHECK(TA::operation_active(*h.panel));

    // The macro that follows still answers to the panel's own budget.
    h.client.defer_next("printer.gcode.script");
    h.client.fire_deferred("printer.gcode.script");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.client.last_send_script().find("PURGE PURGE_TEMP=255") == 0);
    REQUIRE(TA::operation_active(*h.panel));
    outlast_operation_budget();
    CHECK_FALSE(TA::operation_active(*h.panel));
}

TEST_CASE_METHOD(LVGLUITestFixture, "The purge fallback's guard outlasts a slow bin preamble",
                 "[filament][purge_preamble][op_timeout_expire]") {
    FilamentPanelMacroHarness h(std::make_unique<BinBackend>());
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().reset();
    StandardMacros::instance().init(hardware);
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::Purge).is_empty());
    h.set_extruder_target(255.0);

    h.client.defer_next("printer.gcode.script");
    TA::execute_purge(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.client.last_send_script().find("TEST_BIN_PREAMBLE") != std::string::npos);
    REQUIRE(TA::operation_active(*h.panel));

    outlast_operation_budget();
    CHECK(TA::operation_active(*h.panel));

    // The extrusion that follows still answers to the panel's own budget.
    h.client.defer_next("printer.gcode.script");
    h.client.fire_deferred("printer.gcode.script");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.client.last_send_script().find("G1 E") != std::string::npos);
    REQUIRE(TA::operation_active(*h.panel));
    outlast_operation_budget();
    CHECK_FALSE(TA::operation_active(*h.panel));
}

namespace {

bool history_contains(const FilamentPanelMacroHarness& h, const char* needle) {
    for (const auto& s : h.client.gcode_script_history()) {
        if (s.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A purge macro that failed during its bin preamble does not run late",
                 "[filament][purge_preamble][unknown_command]") {
    FilamentPanelMacroHarness h(std::make_unique<BinBackend>());
    h.cache_macros({{"PURGE", "{% set t = params.PURGE_TEMP|default(240)|int %}\nM109 S{t}"}});
    h.set_extruder_target(255.0);

    h.client.defer_next("printer.gcode.script");
    TA::execute_purge(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.client.last_send_script().find("TEST_BIN_PREAMBLE") != std::string::npos);

    h.panel->fail_op_on_unknown_command("STATUS_PURGING");
    REQUIRE_FALSE(TA::operation_active(*h.panel));

    h.client.fire_deferred("printer.gcode.script");
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(history_contains(h, "PURGE PURGE_TEMP"));
    CHECK_FALSE(TA::operation_active(*h.panel));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A purge fallback that failed during its bin preamble does not run late",
                 "[filament][purge_preamble][unknown_command]") {
    FilamentPanelMacroHarness h(std::make_unique<BinBackend>());
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().reset();
    StandardMacros::instance().init(hardware);
    h.set_extruder_target(255.0);

    h.client.defer_next("printer.gcode.script");
    TA::execute_purge(*h.panel);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.client.last_send_script().find("TEST_BIN_PREAMBLE") != std::string::npos);

    h.panel->fail_op_on_unknown_command("STATUS_PURGING");
    REQUIRE_FALSE(TA::operation_active(*h.panel));

    h.client.fire_deferred("printer.gcode.script");
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(history_contains(h, "G1 E"));
    CHECK_FALSE(TA::operation_active(*h.panel));
}
