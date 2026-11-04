// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file toolchanger_panel_fixture.h
 * @brief A four-tool klipper-toolchanger, for the Tool Offsets panel's tests.
 *
 * `test_tool_offset_cal_panel_lifecycle.cpp` drives a run with no widgets;
 * `test_tool_offset_cal_panel_rows.cpp` builds the XML and needs
 * LVGLUITestFixture to do it. Everything in between was identical in both: the
 * mock's toolchanger persona, the discovery the panel's capability question
 * reads, ToolState's four tools, and a real MoonrakerAPI over the mock. It
 * lives here once, and the base class is the template parameter — so a change
 * to what a toolchanger looks like is one edit, not two that are easy to leave
 * half-done.
 *
 * `test_mock_tool_offset_calibration.cpp` deliberately does NOT use this: it
 * tests the simulator itself and wants no PrinterState, ToolState or API
 * around it.
 */

#include "ui_test_utils.h"

#include "app_globals.h"
#include "core/update_queue.h"
#include "json_fwd.h"
#include "lvgl_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/scoped_env.h"
#include "tool_state.h"

#include <functional>
#include <optional>

namespace helix::test {

/// The objects a klipper-toolchanger with the calibration macro publishes.
inline helix::PrinterDiscovery toolchanger_hardware() {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"gcode_move", "toolhead", "extruder", "toolchanger",
                                            "tool T0", "tool T1", "tool T2", "tool T3",
                                            "gcode_macro CALIBRATE_TOOL_OFFSETS"}));
    return hw;
}

/**
 * @brief A READY four-tool toolchanger with a mock client and a real API over it.
 *
 * @tparam Base LVGLTestFixture, or LVGLUITestFixture when the test creates the
 *         panel's widgets (the XML resolves `active_tool` and the rest by name,
 *         which is why ToolState registers into XML scope here either way).
 */
template <typename Base> struct ToolchangerPanelFixture : Base {
    /// The persona is read when the mock is CONSTRUCTED, so the guard is
    /// declared first and the client built in the body once the variable is
    /// set. Members die in reverse: the client goes before the guard restores.
    helix::ScopedEnv ams_env{"HELIX_MOCK_AMS"};
    std::optional<MoonrakerClientMock> client;
    std::optional<MoonrakerAPI> api;

    ToolchangerPanelFixture() {
        setenv("HELIX_MOCK_AMS", "toolchanger", 1);
        client.emplace(MoonrakerClientMock::PrinterType::VORON_24, 100.0);

        // begin_run() asks the global PrinterState whether the printer supports
        // the macro, and the API gates execute_gcode() on klippy being READY.
        helix::PrinterState& ps = get_printer_state();
        helix::PrinterStateTestAccess::reset(ps);
        ps.init_subjects(true);
        ps.set_klippy_state_sync(helix::KlippyState::READY);
        const helix::PrinterDiscovery hw = toolchanger_hardware();
        ps.set_hardware(hw);

        helix::ToolState& ts = helix::ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(true);
        ts.init_tools(hw);

        api.emplace(*client, ps);
        set_moonraker_api(&*api);
    }

    ~ToolchangerPanelFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        set_moonraker_api(nullptr);
    }

    /// Pumps the mock's calibration timer (600 ms per tick), LVGL, and the
    /// UpdateQueue the run's bg_cb callbacks land on.
    bool pump_until(const std::function<bool()>& done, int max_ticks = 200) {
        if (done() || max_ticks <= 0) {
            return done();
        }
        return UITest::advance_until(
            [&done] {
                helix::ui::UpdateQueue::instance().drain();
                return done();
            },
            static_cast<uint32_t>(max_ticks) * 100, 100);
    }
};

} // namespace helix::test
